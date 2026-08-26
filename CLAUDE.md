# compander

An analogue radio mic's companding circuit with a picture pushed through it, as
an FFGL effect for Resolume Arena/Avenue. C++/GLSL, CMake MODULE → universal
`.bundle` (macOS) + Windows `.dll`. Public MIT repo.

Read `AGENTS.md` before changing the chain, the detector or the control mapping.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): configure with no architecture override
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build`

## Verify
- Everything: `tools/verify.sh`
- Flat fields are untouched by Tilt and Emphasis: `./build/cmtest --flat`
- The two ends cancel: `./build/cmtest --roundtrip`
- Doubling passes vs the serial law: `./build/cmtest --detector`
- A matched pair returns the picture: `./build/cmtest --transparent`
- Only detail along the scan moves: `./build/cmtest --anisotropy`
- Presets survive every host: `./build/cmtest --presets`
- No dead controls: `python3 tools/sweep.py`
- Render cost: `./build/cmtest --bench --width 1920 --height 1080`
- The bundle as a bundle: `../resolume-ofx-bridge/build/ffgltest build/Compander.bundle`

## Pictures
- A frame: `./build/cmtest --out /tmp/frame.png`
- The test scene: `./build/cmtest --scene /tmp/scene.png`
- Every preset on one page: `./build/cmtest --sheet /tmp/presets.png --width 420 --height 236`
- Set a control: `--set "Compress=0.6" --set "Time Constant=0.8"` (repeatable, by display name)

## Notes
- **The scan is the time axis.** A compander's time constant converts to a
  distance along the scan: `samples per microsecond = width / 52`. That single
  conversion is the whole plugin, and it is why one control covers edge haloing,
  horizontal smear, line-to-line streaking and whole-frame pumping.
- **Causal, and it trails to the RIGHT.** The detector only knows what has
  already gone past. A symmetric halo means somebody broke it.
- **The processing is horizontal and there is no vertical equivalent.** A link
  carries one signal with one frequency axis. Making it isotropic would turn it
  into a detail compressor that happens to be on a video plugin.
- **The noise is load-bearing.** Companding exists to hide a link's noise floor;
  with no noise the decode stage has nothing to show for itself.
- **`SetParamInfof` reads its default out of `GetFloatParameter`.** Fill
  `params[]` at the TOP of the constructor or every control is declared as zero,
  including Mix. Every offline test still passes. See docs/NOTES.md.
- **`InitGL` must stay idempotent.** The harness calls it per frame.
- Two detectors, not one. The encoder's sees the picture, the decoder's sees the
  encoded signal, and that difference IS the mistracking. Reusing one deletes the
  effect.
- The gain laws exist once, in `Compander.cpp`, and reach the shader as a
  uniform table. Only the per-pixel stage is mirrored, marked `//= mirrored` in
  `Chain.cpp` and `shaders/Common.cpp`.
- All host parameters are 0..1 and mapped in `Controls.cpp`.
- Parameter names are truncated to **16 characters** by Resolume and the SDK
  hides it completely. `verify.sh` checks.
- macOS build must be universal. Verify with `lipo`, never the build log.
- Public repo. "Commit" = commit **and** push.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It covers the two failures that actually happen: a
shader that will not compile (logged with the *assembled* source, because three
of them are spliced at runtime and a compiler line number refers to nothing on
disk), and a buffer the driver would not allocate.

    ~/Library/Logs/compander/compander.YYYY-MM-DD.log
