# Working on compander

The onboarding document. `CLAUDE.md` is the short command reference;
`docs/NOTES.md` is the record of what has already gone wrong here. This file is
the *why*.

---

## What it is

An analogue radio mic link's **companding circuit**, with a picture pushed
through it instead of a microphone signal.

A wireless link cannot carry the dynamic range of what is sent down it, so the
transmitter squashes it — pre-emphasis, then a compressor that halves the
signal's excursion in dB — and the receiver does the exact opposite. In between
sits a link with a noise floor, a ceiling, and no opinion about either.

    in → EMPHASIS → COMPRESS → [ link: level, ceiling, noise ] → EXPAND → DE-EMPHASIS → out

**Get every stage right and almost nothing happens.** That is the point: a
working radio mic sounds like a cable. Everything an operator wants out of this
plugin is the two ends failing to cancel.

---

## The one idea

**A compander's time constant, applied to a video signal, is a distance along
the scan.**

A signal on a wire has one axis, time. When that signal is a picture, time is the
scan — left to right, line after line. So a time constant is not an abstraction
to be reinterpreted; it converts exactly:

    active line = 52 µs of a 64 µs line (625/50)
    samples per microsecond = width / 52

| time constant | at 1920 wide | what it looks like |
|---|---|---|
| 0.2 µs | 7 samples | haloing tight to every edge |
| 5 µs | 185 samples | a smear off the side of things |
| 60 µs | ~1.1 lines | streaks pulling down the frame |
| 2 ms | ~37 lines | broad vertical banding |
| 20 ms | ~½ frame | the whole picture pumping |

Everything else in the repo follows from taking that seriously.

### The three invariants that fall out

If you change anything in the chain, these are what to protect. Each has a test
and each has already caught a real bug.

1. **Causal, trailing right.** The detector can only know what has already gone
   past, so a bright object drags its gain change out *behind* it and leaves its
   left-hand side alone. A symmetric halo is the signature of an effect that was
   never a circuit.
2. **Horizontal only.** A link has one frequency axis. A vertical edge is a high
   frequency; a horizontal edge is not a frequency at all, it is the next line.
   `--anisotropy` measures two identical combs, one turned through ninety
   degrees. There is no vertical pass anywhere in this plugin and adding one
   would not be an improvement, it would be a different effect.
3. **Unity at DC.** Emphasis networks are shelves. On a flat field, Tilt and
   Emphasis must do **nothing at all**, exactly. `--flat` holds it at zero error.

---

## Where things live

| file | what |
|---|---|
| `Compander.h/.cpp` | the laws — compress, expand, the ceiling, the gain tables. **The curves exist here and nowhere else.** |
| `Chain.h/.cpp` | the per-pixel stage. **Mirrored in GLSL**, every block marked `//= mirrored`. |
| `Detector.h/.cpp` | the envelope follower, the pass schedule, and the serial reference implementation the GPU is checked against. |
| `Controls.h/.cpp` | the only place a 0..1 slider becomes a physical quantity. |
| `Presets.h` | the preset table, host-agnostic, in 0..1 space. |
| `Shaders.h`, `shaders/` | the GLSL. `Common.cpp` is the mirror of `Chain.cpp`. |
| `Plugin.h/.cpp` | the FFGL glue: parameters, buffers, pass order. |
| `ofx/CompanderOFX.cpp` | the OpenFX build. Links the model and writes only the render loop again. |
| `tools/cmtest` | the harness. Drives the real plugin class. |

`compander_model` is a separate CMake target with **no GL and no FFGL in it at
all**, so it is obvious at a glance that none of the model can reach for a
texture, and so an OpenFX build can link exactly that and nothing else.

### What is mirrored and what is not

Only the **per-pixel arithmetic** is written twice — once in `Chain.cpp`, once in
`shaders/Common.cpp` — because it is per-pixel and has no choice. Every mirrored
block is marked `//= mirrored` in both. Change one, change both.

The **gain laws are not mirrored.** They are sampled into two 128-point uniform
arrays by `fillGainTables`. A curve that existed twice is a curve a preset could
disagree with itself about.

---

## The two builds differ on purpose

A CPU can run a serial recursion and a GPU cannot, so the FFGL build computes the
peak detector by recursive doubling (exact within `2^K` samples, quarter width,
capped, with a frame-global follower above the cap) and the OpenFX build just
calls `serialEnvelope` — the law itself, unbounded, full resolution.

**The OpenFX build is the more correct one, and it is missing two things.** It
has no frame-to-frame release, because OFX renders frames in any order and holds
no state; and it has no audio sidechain, because OpenFX has no audio input. Both
are stated in that file's header rather than papered over.

`setSupportsTiles(false)` there is load-bearing, not a performance choice: a tile
is the middle of a recursion without its beginning.

---

## Two detectors, not one

The encoder's detector sees the emphasised picture. The decoder's sees what came
off the link — compressed, levelled, noisy. **They are different signals, and
that is the entire reason a real compander mistracks.**

Nothing corrects for it. The residual is the effect. An optimisation that ran one
detector and reused its result would halve the pass count and delete the plugin's
reason for existing.

---

## Traps that will bite

Full detail in `docs/NOTES.md`. The short list:

- ☠️ **`SetParamInfof` takes no default** — it reads one out of
  `GetFloatParameter`. Fill `params[]` at the *top* of the constructor or every
  control is declared to the host as zero, `Mix` included, and the plugin does
  nothing when dragged onto a layer. **Every offline test still passes.**
- **`InitGL` must be idempotent.** The harness calls it per frame.
- **CMake object libraries do not propagate transitively.** Link both explicitly.
- **`ffglex` scoped bindings clear to 0 on scope exit rather than restoring**, so
  allocating a buffer unbinds whatever was on the active texture unit. Every
  `Ensure()` runs before anything is bound. See `PassBuffer.h`.
- **`ScopedFBOBinding` does not restore the viewport.** Captured at the top of
  `ProcessOpenGL`, restored before the composite.
- **Parameter names are truncated to 16 characters by Resolume** and the SDK
  hides it completely.
- `layout`, `flat`, `active`, `filter`, `input`, `output`, `sample` and `common`
  are GLSL keywords. A shader using one fails at *runtime*, and presents as "the
  effect does nothing".
- Randomness is an integer PCG hash, never `fract(sin(x)*…)` — a mirrored effect
  cannot use a function that differs per driver.

---

## Verified vs assumed

Read `docs/NOTES.md` § *Status* for the full list. The headline:

**Verified by measurement.** The round trip cancels to 3e-6 dB where the law is
unbounded. The doubling passes equal the serial detector to float precision, and
a deliberately wrong coefficient is caught. A flat field comes back
byte-identical. Detail along the scan moves 6.7× while detail across it moves
1.5×. All ten presets survive all three host behaviours. No dead controls. All
nine shaders compile. Arena 7.27.1 lists the plugin. 0.73–1.09 ms at 1080p.

**Verified in the host, 2026-08-26.** Instantiated on a layer in Arena 7.27.1 and
rendered on real footage. All 24 parameter names correct, all eight groups drawn
in order, declared defaults correct, About line correct — and **factory presets
hold under the host's parameter pushes and fall back to Custom on a real edit**,
which the fleet note on that pattern says had never been seen live.

**Not verified.** ☠️ **No audio spectrum has ever reached the shader** — the test
machine's Arena had composition audio at 0.0. The buffer parameter is declared
correctly (Resolume draws its audio-source picker) but nothing has driven it. The
OpenFX build has only ever been driven by `ofxprobe`, never opened in Resolve.
Windows builds in CI and ships in the v0.1.0 release, but has never been run.

Do not describe any of that as working.
