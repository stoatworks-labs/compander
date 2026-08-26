# Compander

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The signal model is
> verified numerically by an offline harness that drives the real plugin class:
> it checks the compander's round trip against its own algebra, checks the
> parallel envelope detector against a strictly serial implementation of the same
> law, and proves the effect acts along the scan and not across it (see
> [Status](#status)). The plugin **loads in Resolume Arena 7.27.1** and is listed
> The FFGL build has been **loaded into Resolume Arena 7.27.1, put on a layer and
> run on real footage**, with its controls, groups and factory presets read back
> out of the running host. **No audio spectrum has ever reached it**, though — the
> audio-reactive controls are wired and recognised by Resolume but have never been
> driven. The OpenFX build has only ever been exercised by a test probe, never
> opened in Resolve. Check it in your own rig before trusting it in a show.

An analogue radio mic's companding circuit, with your footage pushed through it
instead of a microphone signal — an FFGL plugin for [Resolume](https://resolume.com)
Arena and Avenue, and the same thing again as an OpenFX plugin for Resolve, Nuke,
Natron and Vegas.

A wireless link cannot carry the dynamic range of what is sent down it. So the
transmitter squashes it — pre-emphasis to lift the quiet top end above the noise,
then a compressor that halves the signal's excursion in dB — and the receiver
does the exact opposite, expanding it back out and de-emphasising. In between
sits a link with a noise floor, a ceiling, and no opinion about either.

```
in → EMPHASIS → COMPRESS → [ link: level, ceiling, noise ] → EXPAND → DE-EMPHASIS → out
```

Get every stage right and almost nothing happens — a working radio mic sounds
like a cable. Everything this plugin is for comes from the two ends disagreeing.

## What it does

- **Compress** and **Expand** are set independently, so you can run half the
  chain. Compress with nothing expanding and the picture goes flat and bright,
  shadows lifted off the floor, edges screaming. Expand with nothing compressed
  and it goes contrasty and dead, blacks crushed, fine detail sucked out of
  anything dim.
- **Emphasis** is the pre-emphasis network, in dB of lift on the detail band.
- **Tilt** is the mismatch between the two emphasis networks — matched in the
  middle, no de-emphasis at all at the top, de-emphasised twice at the bottom.
- **Link Level**, **Noise** and **Headroom** are the link itself. Drop the level
  and the signal sits closer to a noise floor that has not moved, so the expander
  restores the picture and the hiss by different amounts and the shadows breathe.
- **Time Constant** is the one that makes this a video effect rather than an
  audio compressor pointed at a picture. See below.
- **Sidechain** can hand the compressor a **real audio envelope** instead of the
  picture's own. A compander is a circuit whose gain is driven by an audio
  envelope, so this is not a gimmick bolted on — it is the circuit being fed the
  kind of signal it was built for, with the picture standing in for the carrier.

## The time constant is a distance

A signal on a wire has one axis, time. When that signal is a picture, time is the
scan. So a compander's attack and release converts, exactly, into a distance
along the line:

| Time Constant | at 1920 wide | what you see |
|---|---|---|
| 0.2 µs | 7 samples | haloing tight to every edge |
| 5 µs | 185 samples | a smear off the side of things |
| 60 µs | about a line | streaks pulling down the frame |
| 2 ms | about 37 lines | broad vertical banding |
| 20 ms | half a frame | the whole picture pumping |

It is **causal**, so the smear trails to the right of a bright object and leaves
its left edge alone, and it **wraps line to line**, because that is where the
signal was a moment ago.

The processing is horizontal and there is no vertical equivalent. A link carries
one signal with one frequency axis: a vertical edge is a high frequency to a
transmitter, and a horizontal edge is not a frequency at all — it is the next
line.

## Presets

Ten real systems: a broadband two-to-one VCA compander, a high-density two-band
one, a digital hybrid, a receiver fading on the wrong side of the room, something
cheap from a market stall, a consumer tape format's luma channel, a satellite
up-link, an ENG microwave truck, and the two half-chains on their own.

They are named for what they are rather than for whose trademark they are. This
is not anybody's product and does not claim to be a measurement of one.

## Build

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build
```

`cmake --install` drops the bundle into `~/Documents/Resolume Arena/Extra Effects`.

Requires the FFGL SDK submodule: `git submodule update --init --recursive`.

## Status

Verified by measurement, in `tools/verify.sh`:

- the compander's round trip cancels to 3e-6 dB wherever its gain law is
  unbounded, and provably stops cancelling where the boost runs out;
- the parallel envelope detector equals a strictly serial implementation of the
  same law to float precision, and a deliberately wrong coefficient is caught;
- a flat field comes back **byte-identical** through a matched pair;
- detail along the scan moves 6.7× while detail across it moves 1.5×, measured
  from two combs that are identical at source;
- all ten presets survive all three host behaviours, including the one that
  ignores parameter events — which is Resolume;
- no control is dead, and no two presets render the same picture;
- 0.73–1.09 ms a frame at 1920×1080 on an M4 Max.

Confirmed in Resolume Arena 7.27.1, on a layer, rendering real footage:

- all 24 controls present with the right names, none truncated by the host's
  16-character limit;
- all eight parameter groups drawn in order, no duplicates;
- **factory presets apply, survive the host pushing parameters at them for eight
  seconds of live rendering, and fall back to Custom on a real edit**;
- Resolume draws the audio-source picker on the Audio input, so the FFT buffer is
  declared correctly.

**Not verified.** **No audio spectrum has ever reached the plugin** — the test
machine's Arena had its composition audio muted, so there was nothing to analyse.
The audio controls are wired and recognised but undriven; if they do nothing for
you, check the log for `audio input active`. The OpenFX build loads and renders
under a test probe but has never been opened in Resolve. It has never been built
on Windows.

The two builds deliberately differ in one place: a CPU can run the detector's
serial recursion directly, so the OpenFX build computes the envelope law exactly,
while the FFGL build reaches the same answer by recursive doubling on the GPU.
The OpenFX build has no audio sidechain, because OpenFX has no audio input.

## Licence

MIT. See [LICENSE](LICENSE).
