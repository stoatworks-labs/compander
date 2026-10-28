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

### Declare the output frame-varying, or Fusion repeats a generator's first frame

`getClipPreferences` calls `setOutputFrameVarying( true )`. The frame number seeds
the noise floor, so it moves from frame to frame. Without that declaration a host
may treat the output as fixed while the inputs and parameters hold still. Measured
2026-10-04 in Resolve Studio 21.1's Fusion page: every fleet generator rendered
frames 20-22 byte-identical, none having declared it, and with the declaration they
animate.

A tool fed by a MediaIn is re-rendered every frame either way, so in Fusion this
changes nothing visible; the declaration is still the correct one, and other hosts
(Nuke, Natron) cache by it.

The flag changes no pixels: `ofxprobe` renders byte-identical with and without it,
on a moving sequence, on a still and under `--quirks fusion`.

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

---

## The browser demo (2026-10-28)

`demo/` is <https://compander-demo.stoatworks-labs.com/>, built to the fleet's
`resolume-demo` kit rules (`~/Projects/resolume/specs/DEMO-BRIEF.md`; the kit in
`demo/vendor/` is copied from the backend and never edited). What a reader of it must know:

- **The shaders are the plugin's.** All ten pieces of `source/shaders/` are spliced by
  `demo/tools/check_shaders.py --write` into `demo/shaders.js` and compared character for
  character. The same script checks the JOIN (kCommon at the first `@COMMON@` of the encode,
  decode and output fragments, every pass against kVertex, as `assemble()` and InitGL do it,
  and plugin.js's copy of both), fails on a shader piece it does not list, and holds
  plugin.js's `PRESETS` to the rows of `Presets.h` as written. **All eight programs compile
  and link in WebGL2 as spliced** (ANGLE on Metal, M4 Max; and SwiftShader). The blur
  shader's local `float step` hides the built-in, and ES 3.00 accepts that.
- **The passes, buffers and envelope state are the plugin's.** R16F throughout, linear and
  clamped as `FFGLFBO` makes them, the detector at quarter width, and the frame-global
  follower carried across frames in a ping-pong pair of 1×1 R16F buffers with the same slot
  flip, the same First flag (first frame, and after a resize) and the decay computed on the
  CPU from the frame's own duration. Nothing is read back. The noise's frame counter steps
  once a render, as ProcessOpenGL's does.
- **The CPU half is a port, held to the C++.** The PORT block of `demo/plugin.js` --
  Controls.cpp, Compander.cpp (the laws and the two 128-point tables), Detector.cpp (the pass
  plan, the follower's seconds), Chain.cpp's crossoverFor, and ProcessOpenGL's CPU half
  written to RETURN each pass's uniforms. `demo/tools/crosscheck/crosscheck.sh` runs it under
  node against `printer.cpp` on the plugin's own GL-free sources: 3,199 lines, 139,262 floats.
  Against the C++ built `-ffp-contract=off` (the x86_64 slice's arithmetic), 138,869 are
  identical to the bit and the rest within 2 ulp -- V8's Math against Apple's libm -- except
  the per-pass decays, which are a^offset and so carry a 1-ulp difference in `a` raised to
  a power of up to 8,192; `a` is printed and held to 2 ulp and each decay to the bound that
  implies (the worst is at 40% of it). The native arm64 build fuses multiply-adds, which
  JavaScript cannot, and differs by up to 23 ulp in the tables and 256 ulp where Link Level
  crosses zero (5e-7 dB); that comparison is printed, never failed. Two broken ports (the
  active line 52.5 µs, the knee 5 dB) must fail, and do.
- **The pictures are held to the real plugin class.** `demo/tools/crosscheck/pixels.mjs`
  drives the page's own renderer in headless Chrome (`harness.html`, which imports
  `plugin.js`) and pipes the same input frames through `cmtest --pipe` on the same clock.
  14 scenarios, 330 frames: the defaults on two clips, every Sidechain mode, five presets,
  960 and 640 wide, Time Constant in the octave where the frame follower fades in, and past
  it. **On an M4 Max (ANGLE on Metal) every channel of every frame is within 1/255 of the
  plugin's, mean at most 0.0008/255**, and the tolerance is set there (max 1, mean 0.005).
  Through SwiftShader it is within 2 levels, mean at most 0.09, and fails that tolerance;
  look at `--out` pictures before loosening it for another GPU. Six negative controls (Time
  Constant 0.56 against 0.55, the plugin told the wrong frame rate twice, Time Constant 0.98
  against 1.0, Noise 0.14 against 0.13, the wrong preset) differ by 2 to 124 levels and
  fail.
- ⚠️ **No kit clip lets the frame's peak fall**, so on the clips alone the frame follower
  only ever attacks and its release -- the one piece of state carried across frames that the
  clock drives -- is never exercised: a test told the wrong frame rate still passed. The
  harness's synthetic `step` input (a box switched from bright to dim) is what makes the
  release, and those negative controls, bite. Keep a scenario like it.
- **No audio.** A browser has no equivalent of the `FF_USAGE_FFT` buffer, so the Audio input,
  Audio Band and Audio Tilt are absent from the panel rather than present and dead. The rest
  is exactly the plugin with a silent spectrum: UpdateAudio over zeros gives an envelope of 0
  for any Audio Band and a balance of 0. Sidechain keeps its three elements, and Audio and
  Picture x Audio still act on silence (the shader fades the audio envelope towards unity by
  Audio Amount, so the control signal sits at 1 − Audio Amount), so Audio Amount stays.
- **Presets** are the plugin's Preset parameter, applied its way: the eleven covered values
  written, Sidechain, Audio Amount and Mix left alone, a covered control moved by 1e-3 or more
  dropping it to Custom. The panel moves its sliders to the preset, as a host that honours
  value events would; Resolume does not, and the page says so.
- **The clock** is the kit's, in seconds, handed to the port as cmtest --pipe hands the plugin
  SetTime with the unit settled. The seconds-or-milliseconds vote and the wall clock before
  it are not ported. The About block is not on the panel.
- **When you change** a shader, rerun `check_shaders.py --write`; Controls, Compander,
  Detector, Chain or ProcessOpenGL, change the PORT block and run both crosschecks;
  `Presets.h`, change `PRESETS`. `tools/verify.sh` runs all three, skipping the pictures
  without Chrome or a built cmtest.
- **Not verified:** the page in anything but headless Chrome on an M4 Max (Metal and
  SwiftShader), and on a phone, where `EXT_color_buffer_float` is often missing (the page then
  stops and says so). The kit's "Use my own…" path was tried once, with `docs/scene.png`: it
  ran at the still's 1280 × 720, the right way up.
