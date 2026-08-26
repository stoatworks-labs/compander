# compander — notes

Repo-specific things worth knowing. Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes), not here.

Started 2026-08-26.

---

## The modelling errors, and how each one was caught

All four of these compiled, linked, loaded, rendered, and looked plausible. Each
was caught by a **number**, not by looking at a picture, and each is the reason
a particular test exists. They are recorded because the same mistakes are easy
to make again from the same reasoning.

### `Link Level` was a brightness control

The obvious way to model mistracking is to offset the expander's reference: the
receiver is aligned to the wrong point, so it applies its law about the wrong
place. It is wrong, and the maths says so immediately.

A true broadband compander's law is a **straight line in dB through the pivot**
— same slope above and below — so it is scale-free. Moving the point it pivots
about produces:

    output_dB = input_dB − error_dB × (ratio − 1)

which is a **constant offset at every brightness**. The first `--roundtrip` run
printed exactly `-3.00 dB` at every input level for a 3 dB reference offset.
That is a brightness knob with a misleading name.

**The fix is in the signal path, not the law.** `Link Level` is now a gain
applied to the encoded signal *entering* the link, ahead of the noise. The noise
floor stays where it is while the signal moves relative to it, so the expander —
which is referred to the nominal pivot and knows nothing about any of it —
restores signal and noise by different amounts. That is level-dependent, and it
is what a mistracked receiver actually looks like.

Level dependence in this plugin comes from the three places it comes from in a
real circuit: the gain bound running out (`Max Gain`), the two ends being set to
different ratios, and the two detectors looking at different signals. Never from
a pivot offset. `--roundtrip` now asserts that a ratio mismatch gives a
level-dependent error and fails if the spread across level is under 1 dB.

### `Chroma`'s two branches were the same expression

Written as "chromaticity-preserving" versus "companded alongside":

    scaled    = rgb * gain
    companded = lumaOut + (rgb − lumaIn) * gain

Since `gain` is *defined* as `lumaOut / lumaIn`, the second expands to
`lumaIn*gain + rgb*gain − lumaIn*gain` = `rgb * gain`. **Identical.** The control
would have shipped completely dead, and `sweep.py` would have caught it only
because the scene happens to have a colour patch.

The two behaviours were also the wrong way round. What actually distinguishes a
radio mic from a video link is whether the colour difference signal went through
the compander **at all**:

    bypassed  = lumaOut + (rgb − lumaIn)   // chroma never crossed the link
    companded = rgb * gain                 // chroma went through with the luma

At `Chroma` 0 the colour comes back at the amplitude it left at, so lifting a
shadow **desaturates** it — one channel was companded and the colour was not. At
1 the ratio holds and saturation stays put.

⚠️ **Chroma noise is not modelled.** The link's noise enters the luma channel and
is scaled into the colour from there, so it arrives as luminance noise tinted by
the picture rather than as noise on the colour difference signals in their own
right. Real video links were notorious for the latter. Doing it properly means
carrying two colour difference channels through the whole chain instead of one
luma channel — three times the passes. Said in `Chain.h` too, because the
control's name implies more than it does.

### `Tilt` was mostly a brightness control, and then could not reach either end

Two errors in the same function, one after the other.

**First:** implemented as a see-saw scaling the low band by `(1−t)` and the high
band by `(1+t)`. On a picture the low band carries essentially all the energy and
the high band almost none, so the "see-saw" was lopsided: a half-scale tilt moved
a test signal by **32%**. A tilt control that is 95% a brightness control.

The fix is physical. An emphasis network is a shelf and has **unity gain at DC**
— there is nothing at the bottom of a shelf to get wrong. So the low band, the
only one of the three carrying any mean, is never touched by either network. The
invariant that falls out is sharp enough to test: **on a flat field, Tilt and
Emphasis must do nothing at all**, and `--flat` holds it at exactly zero error
across twenty combinations.

**Second:** with de-emphasis always exactly cancelling emphasis, the encode-only
and decode-only characters the design promised were simply not reachable. The
header described "edges screaming where the pre-emphasis was never taken back
off" and no setting produced it.

`Tilt` is now **the mismatch itself**: the receiver's network is `(1 − tilt)` as
deep as the transmitter's.

| tilt | what it is | look |
|------|-----------|------|
| 0 | matched | cancels exactly |
| +1 | no de-emphasis at all | hard, glassy, edges ringing — encode only |
| −1 | de-emphasised twice | soft, smeared, detail sucked out — decode only |

Three characters on one control, the flat-field invariant still exact, and the
`Encode Only` and `Decode Only` presets are just this control at its ends.

### The emphasis crossover was tied to the detector

`crossoverFor()` originally took the time constant, on the reasoning that one box
was designed against one bandwidth. That is wrong about the circuit: a
pre-emphasis network is an RC in the transmitter with a corner set when the thing
was designed, and the compander's attack and release is a separate time constant
in a separate part of the circuit. **They are independent in every real system.**

Coupling them also demanded a blur hundreds of samples wide at long time
constants, for nothing. The corners are now a fraction of the active line —
`width/512` and `width/64`, roughly a tenth and eight tenths of a microsecond,
which brackets where video pre-emphasis networks actually sat.

---

## The one idea, and what it costs

**A compander's time constant, on a video signal, is a distance along the scan.**
That is the whole plugin. The active line is 52 µs of 64, so

    samples per microsecond = width / 52

and one control covers haloing tight to an edge (0.2 µs), a smear off the side of
things (5 µs), streaks pulling down the frame (60 µs) and the whole picture
pumping (20 ms).

Two consequences are load-bearing:

- **It is causal and trails to the RIGHT.** The detector only knows what has
  already gone past. A symmetric halo is the signature of an effect that was
  never a circuit.
- **It wraps line to line.** Sample `n − d` past the left edge is the right-hand
  end of the line above. Clamping instead would be a perfectly reasonable
  image-processing decision and would quietly delete the entire multi-line range
  of the control.

The detector is a peak detector — instant attack, exponential release — which is
a **maximum over exponentially decayed history**, and a maximum over a window is
the one recursive filter that parallelises exactly, by recursive doubling. After
`K` passes each output has seen `2^K` samples. `--detector` checks the doubling
against a strictly serial implementation that could never run on a GPU: they
agree to float precision (max error 6.6e-7 at the settings that matter).

### The seam

The pass count is capped at `kMaxScanPasses` = 14, which at quarter width reaches
about **34 lines** of a 1920-wide frame. Above that the envelope is served by a
frame-global follower with a one-pole across frames, crossfaded in over the
octave below the cap.

⚠️ **Between roughly 34 lines and a whole frame the model is an interpolation,
not a computation.** The scan detector has run out of history and the frame
follower has no spatial structure, so a time constant in that range renders as a
blend of a truncated streak and a global pump rather than as the very long streak
it should be. It reads convincingly. It is not exact. Raising the cap is
possible — the passes are cheap — and was not done because nothing has asked.

---

## Traps

### `SetParamInfof` reads its default out of the plugin

☠️ **The one that would have shipped.**

    void SetParamInfof( unsigned int index, const char* name, unsigned int type )
    {
        SetParamInfo( index, name, type, GetFloatParameter( index ) );
    }

There is no default argument. It reads one back out of the plugin's own
`params[]`. So a constructor that declares the parameters first and fills
`params[]` afterwards tells the host that **every control is zero** — including
`Mix`, which means the effect does nothing at all when it is dragged onto a
layer.

Every offline test still passed, because the harness never asks the host what the
defaults were. `ffgltest` caught it, by reporting `0 of 8192 bytes differ from
the input` when the default noise floor should have moved the frame. `verify.sh`
now treats that line as a failure.

**Fill `params[]` at the top of the constructor.** There is a comment there
saying so.

### `InitGL` must be idempotent

The harness calls `InitGL` every frame so that one run can render at several
sizes without tearing the GL resources down. `InitGL` was recompiling all eight
shader programs each time, which put `--bench` at **20 ms a frame at 1080p** and
— the giveaway — reported the *same* figure for two detector passes and for
fourteen. Guarding on `FFGLShader::IsReady()` took it to **0.73–1.09 ms**, and the
cost now tracks the pass count the way it should.

`FFGLScreenQuad` has no equivalent `IsInitialised`, so that is tracked in a bool.
Initialising it twice leaks a VAO and two buffers per call.

### Object libraries do not propagate transitively

`compander_core` links `compander_model`, both `OBJECT` libraries. CMake does
**not** propagate an object library's objects through another object library, so
the MODULE target has to link both explicitly. The failure is undefined symbols
for the entire model, which reads like a missing source file rather than a
linkage rule.

Both are `OBJECT` and not `STATIC` on purpose: `CFFGLPluginInfo` registers itself
from a file-scope constructor in `Plugin.cpp` and nothing references it by name,
so in an archive the linker may drop the whole translation unit — giving a bundle
that loads, exports `plugMain`, and reports that it contains no plugins.

### glslc needs Vulkan's rules turned off

`glslc` targets SPIR-V, which demands an explicit `layout(location=…)` on every
uniform and varying. Those are Vulkan rules, not GLSL ones. Without
`-fauto-map-locations` all nine shaders "fail" for reasons that have nothing to
do with the code, and the real errors are buried.

    glslc --target-env=opengl4.5 -fauto-map-locations shader.frag -o /dev/null

Worth having: a shader that will not compile presents to an operator as "the
effect does nothing", with the message only in the log.

### The frame follower does not use mipmaps

The obvious reduction to one texel is `glGenerateMipmap` and a high `textureLod`.
It needs the FBO's colour texture to have mip storage and a mipmapping min
filter, and `ffglex::FFGLFBO` gives it neither — so the obvious version samples
level 0 forever and the whole frame-scale range of `Time Constant` quietly stops
working. It samples an 8×8 grid instead, which is cheap at 1×1 and depends on
nothing.

### An include's case, which only Linux can see

☠️ **Cutting v0.1.0 failed on this**, after the tag.

`source/ofx/CompanderOFX.cpp` included `"ofxsProcessing.H"`. The file is
`ofxsProcessing.h`. macOS is case-insensitive and so is Windows, so two of the
three release jobs went green and the Linux OpenFX one died with
`fatal error: ofxsProcessing.H: No such file or directory`. No release was
published.

**The header was not even needed** — this plugin deliberately does not use
`OFX::ImageProcessor`, and the include was carried over from the donor file
along with everything else. So the fix was to delete it, not to correct it.

Nothing local could have caught it: not a build, not a test, not a review that
was not specifically looking for case. `tools/check-include-case.py` now compares
every `#include` against the real filenames on disk and is wired into
`verify.sh`, which closes the class rather than this instance.

### The tests had two bugs of their own

Both made a test pass while measuring the wrong thing, which is worse than
failing.

- **`--transparent` indexed a top-down scene with bottom-up buffers.** It
  reported the ramp's error under the flat band's name and passed the flat band
  by measuring the ramp. The scene is described top-down and the buffers are
  stored bottom-up; `ry = H − 1 − y`.
- **`--anisotropy` measured nothing.** The scene's only horizontal structure was
  band boundaries, so across-scan detail came out as exactly zero, the ratio
  defaulted to 1.0 and the test passed. The scene now carries **two combs** —
  same two levels, same one-pixel spacing, one turned through ninety degrees — so
  the two axes are directly comparable, and the test fails if either source
  measurement is degenerate.

---

## The two builds do not match, and the OpenFX one is more correct

A CPU can run a serial recursion and a GPU cannot. So:

- **FFGL** computes the peak detector by **recursive doubling** — exact within a
  window of `2^K` samples, at quarter width, with a pass cap that stops the
  window reaching a whole frame, and a frame-global follower crossfaded in above
  it.
- **OpenFX** simply calls `serialEnvelope`: the law itself, unbounded history,
  full resolution. There is no seam and no cap.

⚠️ **What the OpenFX build cannot do is the frame-to-frame release.** OFX renders
frames in any order and holds no state between them, so there is no previous
frame's envelope to decay from and the whole-picture pumping across a cut is
absent. Exact and deterministic there, merely faithful on the FFGL side — the
same trade afterglow's OpenFX build makes with its frame queue.

**There is also no audio sidechain in the OpenFX build.** OFX has no FFT input
and no beat info. Rather than expose four controls that do nothing, they are left
out of that build's control surface entirely.

`desc.setSupportsTiles( false )` is **not a performance choice**. The detector is
a serial scan in which every sample depends on the one before it, so a tile is
the middle of a recursion without its beginning. `getRegionsOfInterest` asks for
the full source for the same reason. With tiles enabled the plugin would render
correctly only when the host happened to ask for a whole frame.

---

## Status — verified vs assumed

**Verified by measurement:**

- The round trip cancels to 3e-6 dB wherever the gain law is unbounded, and
  provably stops cancelling where the boost runs out (`--roundtrip`).
- Tilt and Emphasis are inert on a flat field to exactly zero error across
  twenty combinations, and demonstrably alive on a detailed one (`--flat`).
- The parallel doubling passes equal the strictly serial peak detector to float
  precision, and a deliberately wrong decay coefficient is caught (`--detector`).
- A matched pair with no noise returns the flat band **byte-identically**
  (`--transparent`).
- Detail along the scan moves 6.7× while detail across it moves 1.5×, from two
  combs identical at source (`--anisotropy`).
- All ten presets survive all three host behaviours, including the one that
  ignores value events, which is Resolume (`--presets`).
- No dead controls; all ten presets render distinctly (`sweep.py`).
- All nine shaders compile under a real GLSL compiler (`verify.sh`).
- The FFGL bundle instantiates through `ffgltest` and changes 5557 of 8192
  bytes; the OpenFX bundle renders through `ofxprobe` and changes 678562 of
  921600, and ad-hoc signs with `CFBundleExecutable` matching the binary on
  disk.
- **Arena 7.27.1 lists the plugin**: idstring `CM01`, name `Compander`,
  category `Video Effects`, description read correctly.
- **Instantiated on a layer in Arena 7.27.1 and rendered live on real footage**
  (a DXV AV clip), 2026-08-26. Everything below was read back out of the running
  host over its REST API or seen in its inspector, not inferred.
- **All 24 parameters present with the right names, none truncated.** Two are
  exactly 16 characters (`Source on GitHub`, `Support the work`) and both display
  complete — they are the fleet's standard About buttons.
- **All eight parameter groups** — Signal, Encode, Link, Decode, Detector, Audio,
  Output, Preset — render as their own collapsible headers, in order, with no
  duplicates.
- **The declared defaults arrive correctly**, which is the in-host confirmation of
  the `SetParamInfof` fix below.
- ☠️ **Factory presets work in a real Resolume.** Applying one sets the controls;
  it then **held for eight seconds of live rendering** while the host pushed
  parameters at it, and **dropped to Custom on a genuine edit**. The fleet note on
  this pattern says it had never been seen live in Resolume or Resolve — it has
  now, and it behaves.
- **The About line renders with the version**: `Compander v0.1.0 - MIT -
  Stoatworks Labs, stoatworks-labs.com`.
- **Resolume recognises the FFT buffer.** The `Audio` parameter is drawn as
  Resolume's own audio-source picker (Local / Composition / External), which only
  happens for a correctly declared `FF_USAGE_FFT` buffer param.
- 0.73–1.09 ms a frame at 1920×1080 on an M4 Max, scaling with the pass count.

**Not verified:**

- ☠️ **No audio spectrum has ever reached the shader.** Resolume draws the
  audio-source picker, so the buffer parameter is declared correctly — but the
  test machine's Arena had composition audio at **0.0**, so there was no signal
  to analyse and no envelope was ever delivered. `Sidechain`, `Audio Amount`,
  `Audio Band` and `Audio Tilt` remain unexercised end to end, in Arena and in
  the harness alike.

  **Tried, twice, and it could not be closed on this machine.** With the plugin
  on a layer, an AV clip connected, Sidechain on *Picture × Audio* and Audio
  Amount at 1, the composition master was raised to 0.35 and then the layer's
  own audio to 0.6 as well. No `audio input active` line appeared either time,
  and both volumes were put back to 0.0.

  The cause is in Arena's own audio configuration, not in the plugin.
  `~/Documents/Resolume Arena/Preferences/config.xml` has:

      audioOutputDeviceName="NDI Audio"  audioInputDeviceName=""  audioDeviceInChans="0"

  — output routed to a virtual NDI sink and no audio input device at all, which
  is also why the composition was muted to begin with. There is no live audio
  bus on this machine for Resolume to analyse, so nothing was ever going to be
  delivered to an `FF_USAGE_FFT` buffer.

  ⚠️ **That is a machine finding, not a plugin finding, and it does not clear
  the plugin.** The FFT path remains unexercised end to end. To close it, on a
  machine with a real audio device: put an AV clip on a layer, set Sidechain to
  Audio, and check the log for `audio input active: 64 bins, envelope …` — a
  line that exists precisely so this question has a one-line answer.
- **Whether Arena's UI sliders visually follow a preset is not established.** The
  REST readback matches the preset exactly, but REST reads `GetFloatParameter`,
  which reflects the plugin's own state rather than what the inspector draws. The
  two are only distinguishable by eye, and the attempt to expand a group in the
  UI mis-clicked twice and was abandoned. `plugin-bench/arena` is the right tool.
- **The macOS build here is arm64 only.** What ships must be built without
  `-DCMAKE_OSX_ARCHITECTURES`, and checked with `lipo` rather than the build log.
- **The OpenFX build has only ever been driven by `ofxprobe`.** It loads,
  renders 678562 of 921600 bytes changed, and ad-hoc signs — but it has never
  been opened in Resolve, Nuke, Natron or Vegas, and no host has ever drawn its
  parameter pages.
- **Windows has never been built.** `vcpkg.json` is present and correct by
  inspection only.
- The website page and the published guide do not exist until the site is
  deployed. `projects.json`, `shots.json` and `sync-about.py`'s own slug table
  all carry `compander` as of 2026-08-26, and `source/StoatworksAbout.h` is
  generated rather than hand-seeded — so the four About buttons carry real URLs,
  but the guide and project pages behind two of them are live only once the site
  is built and deployed.
