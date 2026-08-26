# Compander user guide

Compander is **an analogue radio mic's companding circuit, with your picture pushed through it**
— an FFGL plugin for [Resolume](https://resolume.com) Arena and Avenue, and an OpenFX plugin for
DaVinci Resolve, Vegas, Nuke and Natron.

A wireless microphone link cannot carry the dynamic range of what is sent down it. So the
transmitter squashes it — pre-emphasis to lift the quiet top end above the link's noise, then a
compressor that halves the signal's excursion in dB — and the receiver does the exact opposite.
In between sits a link with a noise floor, a ceiling, and no opinion about either.

```
in → EMPHASIS → COMPRESS → [ link: level, ceiling, noise ] → EXPAND → DE-EMPHASIS → out
```

Get every stage right and almost nothing happens, which is the point: a working radio mic sounds
like a cable. **Everything this plugin is for comes from the two ends failing to cancel.**

The idea that makes it a video effect rather than an audio compressor pointed at a picture:
**a compander's attack and release, applied to a picture, is a distance along the scan.** A signal
on a wire has one axis, time, and when that signal is a picture, time is the scan — left to right,
line after line.

> **Before you rely on this:** the signal model is verified numerically. The compander's round trip
> cancels to **three millionths of a decibel** wherever its gain law is unbounded, a flat field
> comes back **byte-identical** through a matched pair, and the envelope detector the GPU runs is
> checked against a strictly serial implementation of the same law — one that could never run on a
> GPU — with which it agrees to float precision. All 24 controls are confirmed to change the
> picture and no two presets render the same frame.
>
> It has been **loaded into Resolume Arena 7.27.1, put on a layer and run on real footage**, with
> its controls, groups and factory presets read back out of the running host.
>
> Still open: **no audio spectrum has ever reached it.** The audio-reactive controls are wired and
> recognised by Resolume but have never been driven — see [Audio reactivity](#audio-reactivity)
> for the one-line way to check them on your own rig. The OpenFX build has only ever been driven by
> a test probe and has **never been opened in Resolve**. Try it on a spare layer before you put it
> in a show.
>
> Released at v0.1.0. This codebase was created with AI assistance, directed and reviewed by a
> human author.

---

## Installing

Drop the plugin into Resolume's FFGL folder and restart Resolume:

```
macOS    ~/Documents/Resolume Arena/Extra Effects/
         (or /Users/Shared/Resolume Arena/Extra Effects/)
Windows  %USERPROFILE%\Documents\Resolume Arena\Extra Effects\
```

Avenue uses the same layout under its own folder name. Compander then appears in the effects
browser under **Video Effects**.

The macOS builds are **Developer ID-signed and notarised**, so the bundle simply loads — there is
nothing to clear and no `xattr` step. The Windows builds are not code-signed, but plugin files are
not gated the way `.exe` files are, so Resolume loads them normally.

### OpenFX hosts (Resolve, Vegas, Nuke, Natron)

Copy `Compander.ofx.bundle` into the OFX plugin directory:

```
macOS    /Library/OFX/Plugins/
Windows  C:\Program Files\Common Files\OFX\Plugins\
Linux    /usr/OFX/Plugins/
```

It appears under **Stoatworks**. ⚠️ The OpenFX build has never been opened in a real host — it is
built, it loads under a test probe, and that is all that is established.

---

## The one control that matters most

**Time Constant** is the compander's attack and release, and on a picture that is a *distance*. The
active picture is 52 microseconds of a 64 microsecond line, so:

| Time Constant | at 1920 wide | what you see |
|---|---|---|
| 0.2 µs | 7 samples | haloing tight to every edge |
| 5 µs | 185 samples | a smear off the side of things |
| 60 µs | about a line | streaks pulling down the frame |
| 2 ms | about 37 lines | broad vertical banding |
| 20 ms | half a frame | the whole picture pumping |

Two things about it are deliberate and worth knowing, because they are what make it read as a
circuit rather than as a filter:

- **It trails to the right.** The detector can only know what has already gone past, so a bright
  object drags its gain change out behind it and leaves its left-hand edge alone.
- **It wraps line to line.** A bright object at the end of one line affects the start of the next,
  because that is where the signal was a moment ago.

The processing is horizontal and there is no vertical equivalent. A link carries one signal with one
frequency axis: a vertical edge is a high frequency to a transmitter, and a horizontal edge is not a
frequency at all — it is the next line.

---

## The controls

### Signal

- **Sidechain** — where the compressor's control signal comes from. *Picture* is the honest
  circuit. *Audio* hands it a real spectrum instead. *Picture × Audio* is both, and is the one that
  survives a whole show because it still follows the footage.
- **Chroma** — whether the colour crossed the link. At **0** the colour difference bypassed it and
  comes back at the amplitude it left at, so lifting a shadow *desaturates* it — one channel was
  companded and the colour was not, which is the radio mic reading. At **1** the whole colour is
  carried through together, so saturation holds while brightness moves — the video link reading.

### Encode

- **Compress** — the ratio at the transmitter, 1:1 to 4:1. The two-to-one that essentially every
  analogue wireless system used sits at **one third** of the travel.
- **Emphasis** — pre-emphasis, 0 to 12 dB of lift on the detail band, so quiet high-frequency
  content sits above the link's noise instead of under it.

### Link

- **Link Level** — gain into the link, ±12 dB, applied **before** the noise. The noise floor stays
  where it is while the signal moves relative to it, so this is a signal-to-noise control before it
  is a level control. Centre is an aligned pair.
- **Noise** — the link's own noise floor. **This is load-bearing, not decoration.** Companding
  exists to hide a noise floor, and with none of it the decode stage has nothing to show for
  itself. Turning it to zero to see the companding on its own is a legitimate thing to do, and it
  is not the default.
- **Headroom** — where the link clips. **Runs backwards**: higher is *less* headroom, so it reads
  the way a drive control should.

### Decode

- **Expand** — the ratio at the receiver, 1:1 to 1:4. Match it to Compress and the round trip
  nearly cancels. That is what a working link does; everything interesting is a departure from it.
- **Tilt** — the mismatch between the transmitter's emphasis network and the receiver's. Centre is
  matched. At the **top** there is no de-emphasis at all and everything the transmitter lifted stays
  lifted: hard, glassy, edges ringing. At the **bottom** the signal is de-emphasised twice: soft,
  smeared, detail sucked out of anything fine.

### Detector

- **Time Constant** — see above.
- **Pivot** — the level the law pivots about. Signal at the pivot passes at unity through both ends
  whatever the ratio, so this is the one brightness where the round trip is exact, and everything
  either side of it is where the character is.
- **Max Gain** — the most boost the compressor will ever apply, 0 to 48 dB. A real circuit's boost
  runs out, or it would be amplifying nothing but its own noise floor. This decides **how far into
  the shadows the effect reaches**, and it is why dark parts of a picture stop lifting rather than
  lifting forever.

### Audio

- **Audio** — Resolume's own audio-source picker (Local / Composition / External). This is the
  spectrum the sidechain reads.
- **Audio Amount** — how hard that envelope drives the compressor.
- **Audio Band** — which part of the spectrum drives it, from the bottom of the band to the top.
  It is a *lean*, two octaves wide, not a filter — a narrow window on 64 bins tracks one drum
  rather than the music.
- **Audio Tilt** — how much the spectrum's brightness pushes Tilt on top of its own setting. It
  rides on the Tilt control rather than replacing it, so a tilt you have set is kept and the music
  moves it.

### Output

- **Mix** — wet/dry against the untouched input.

### Preset

Ten real systems, named for what they are rather than for whose trademark they are:

| Preset | What it is |
|---|---|
| **Two-to-One VCA** | The one almost everything was: broadband, 2:1, moderate pre-emphasis, fast enough that the gain change hugs the edges. |
| **High Density** | The quieter, more ambitious end of the same generation — a gentler ratio carrying a much deeper emphasis network, and a slower detector. |
| **Digital Hybrid** | A compander emulated in arithmetic rather than built out of a VCA, so it tracks almost perfectly. Nearly transparent on purpose: start here before breaking something deliberately. |
| **Diversity Fade** | A receiver on the wrong side of the room. Six dB down into the link with the noise floor where it always was, so the shadows breathe. |
| **Cheap Handheld** | Over-compressed, out of headroom, with the two ends set to ratios that do not match. Nothing ever quite comes back. |
| **Tape Luma** | Not a microphone at all: the luma channel of a consumer tape format, which used the same trick. Very heavy pre-emphasis into a hard white clip. |
| **Satellite Link** | A video up-link. Composite through one emphasis network, so the colour moves with the picture. |
| **ENG Microwave** | The same idea in a truck, done worse — further down into the link, noisier, and de-emphasising slightly harder than the transmitter emphasised. |
| **Encode Only** | Half the chain. Compressed and emphasised with nothing to undo either: flat, lifted out of the shadows, edges ringing. |
| **Decode Only** | The other half. A decoder handed material that was never encoded: contrasty, blacks crushed, fine detail sucked out. |

Picking a preset sets the controls below it. Editing any of them afterwards falls back to
**Custom** — the preset is a starting point, not a mode.

⚠️ **`High Density` is an impression, not a measurement.** The real two-band systems split the
signal and compand the halves against different laws; this plugin has one band, and the preset gets
near the character by leaning the emphasis harder and slowing the detector.

---

## Recipes

**A link that is only just working.** Start at *Digital Hybrid*, then pull **Link Level** down two
or three dB. The picture stays where it is and the shadows start to breathe, because the expander is
restoring the noise and the picture by different amounts.

**Analogue video, not analogue audio.** *Tape Luma*, then take **Chroma** to 0 if it is not there
already — the colour genuinely did go somewhere else in those formats, and the desaturation as
shadows lift is the give-away.

**The smear.** Any preset, then walk **Time Constant** up past halfway. Watch the right-hand side of
anything bright: the gain change trails off it and leaves the left edge alone. That asymmetry is the
whole point, and it is what an ordinary glow or blur cannot do.

**Something breaking on cue.** *Cheap Handheld* with **Mix** on a fader. The mismatch is worst on
high-contrast content, so it hits hardest exactly when the footage does.

**Restraint.** *Digital Hybrid* at **Mix** 0.3 is a grade rather than an effect — it tightens
shadows and puts a little life in the top end without announcing itself.

---

## Audio reactivity

Set **Sidechain** to *Audio* or *Picture × Audio*, then choose a source on the **Audio** control
(Local / Composition / External) exactly as you would for any Resolume audio-reactive parameter.

A compander is a circuit whose gain is driven by an audio envelope, so this is not a gimmick bolted
on to a video effect — it is the circuit being fed the kind of signal it was built for, with the
picture standing in for the carrier. **The audio drives the encode end only.** A real receiver's
expander knows nothing about what the transmitter's VCA was being fed, and that is the interesting
behaviour rather than a simplification: drive both ends and the round trip cancels and the picture
merely gets louder; drive one and the link mistracks in time with the music.

⚠️ **This is the least-tested part of the plugin.** It has never been driven by a live spectrum.

**If it appears to do nothing**, the usual cause is the audio source picker pointing at something
silent. The plugin cannot tell that apart from a quiet track, so it says which in its log — look for
one line:

```
audio input active: 64 bins, envelope 0.31, balance -0.12
```

If that line never appears, no spectrum is arriving and the problem is upstream of the plugin: check
Resolume's own audio input, and that the composition is not muted.

    macOS    ~/Library/Logs/compander/compander.YYYY-MM-DD.log
    Windows  %USERPROFILE%\compander\logs\

---

## Performance

About **0.7 to 1.1 ms a frame at 1920×1080** on an Apple M4 Max, and the cost tracks **Time
Constant**: a short setting needs a handful of detector passes and only the longest settings need
the full count. If you are tight for headroom, a shorter Time Constant is the cheaper one.

The chain runs at full resolution in one channel — a link carried one signal — and the detector runs
at quarter width, which is where the saving is.

---

## Troubleshooting

**The effect does nothing at all.** Check the log. A shader that will not compile presents exactly
this way, and the log names the stage, the compiler's complaint and the source it refers to.

**It is very subtle.** Check **Mix**, then **Noise** — with no noise in the link the decode stage
has nothing to reveal, and a matched pair is *designed* to be nearly transparent. Try *Cheap
Handheld* to confirm the plugin is doing anything at all, then come back.

**Everything is crushed to black.** **Expand** is high with **Compress** low — a decoder handed
material that was never encoded. That is *Decode Only*, and it is a real look, but if you did not
mean it, match the two ratios.

**The picture got brighter and flatter.** The opposite: **Compress** high with **Expand** low.

**Highlights have hard flat tops.** **Headroom** is running the encoded signal into the link's
ceiling. Remember the control runs backwards — turn it *down* for more headroom.

---

## Licence

MIT. Source at <https://github.com/stoatworks-labs/compander>.
