/**
 * Compander — browser demo.
 *
 * An analogue radio mic's companding circuit with a picture pushed through it.
 * The one idea, from `AGENTS.md`: **a compander's time constant, applied to a
 * video signal, is a distance along the scan.** The active line is 52 µs of a
 * 64 µs line, so a time constant converts exactly into samples (`width / 52`
 * per microsecond), and one control runs from a halo hugging every edge to the
 * whole picture pumping.
 *
 *     in → EMPHASIS → COMPRESS → [ link: level, ceiling, noise ] → EXPAND → DE-EMPHASIS → out
 *
 * Two halves, and they are not equally faithful here:
 *
 *   **The GPU half is the plugin's own GLSL.** `shaders.js` is the ten
 *   `R"( ... )"` pieces of `source/shaders/` (kVertex, kCommon and the eight
 *   fragment shaders), spliced by `demo/tools/check_shaders.py --write` and
 *   never typed; the same script compares them character for character,
 *   checks that kCommon is spliced in at the `@COMMON@` marker as `assemble()`
 *   splices it, and checks the preset table against `Presets.h`.
 *   `tools/verify.sh` runs it. They run as the plugin's passes, in the
 *   plugin's order, on the plugin's buffers: R16F throughout, the detector at
 *   quarter width, the frame follower ping-ponged between two 1×1 buffers,
 *   all linear-filtered and clamped as `ffglex::FFGLFBO` makes them.
 *
 *   **The CPU half is a port.** Between the PORT markers below: the control
 *   conversions (Controls.cpp), the gain laws and the two 128-point tables the
 *   shaders read them through (Compander.cpp), the detector's pass plan and
 *   the frame follower's units (Detector.cpp), the band-split radii
 *   (Chain.cpp's crossoverFor), and the CPU half of Plugin.cpp's ProcessOpenGL
 *   — the clock, the frame follower's slot, its decay and its first-frame
 *   flag, the noise's frame counter, and every uniform — written as a function
 *   that RETURNS what each pass is fed, so the GL code further down only binds
 *   and draws. demo/tools/crosscheck/ holds the first four to the plugin's own
 *   C++ (verify.sh runs it), and its pixels.mjs runs this page and the real
 *   plugin class (`cmtest --pipe`) through the same frames and compares them.
 *
 * ---------------------------------------------------------------------------
 * Decisions this page made, and why
 * ---------------------------------------------------------------------------
 *
 * **The envelope state is the plugin's, kept the plugin's way.** Two of the
 * three things a compander remembers live on the GPU and nowhere else: the scan
 * detector is recomputed from the picture every frame (it is causal along the
 * scan, not across frames), and the frame-global follower is a 1×1 R16F value
 * carried from one frame to the next in a ping-pong pair, attacked instantly and
 * released by exp(−dt / tau) with dt the frame's own duration. Both are here as
 * they are there: the same two buffers, the same slot flip every frame, the same
 * First flag on the first frame and after a resize, the same decay computed on
 * the CPU from the clock. Nothing is read back. The third, the audio follower,
 * is CPU-side in the plugin and has nothing to follow here (below).
 *
 * **No audio.** The plugin's `Audio` input is a 64-bin FFT buffer Resolume
 * fills (`FF_USAGE_FFT`); a browser has no equivalent, and asking a visitor for
 * a microphone to demonstrate a video effect is not a trade worth making. So
 * the buffer, Audio Band and Audio Tilt are absent from the panel rather than
 * present and dead. What is left is exactly the plugin with a silent spectrum,
 * which is not an approximation: UpdateAudio() over 64 zeros gives an envelope
 * of 0 (a sum of zeros over a nonzero weight, whatever Audio Band says) and a
 * balance of 0 (so Audio Tilt adds nothing to Tilt). Sidechain keeps all three
 * of its elements, because Picture is the honest circuit and the default, and
 * the other two still do something with silence: the shader fades the audio
 * envelope towards unity by Audio Amount, so the compressor's control signal
 * sits at 1 − Audio Amount (Audio) or the picture's envelope times that
 * (Picture × Audio). That is what the plugin does when its audio picker is set
 * to something silent, so Audio Amount stays on the panel and stays live.
 *
 * **The preset is the plugin's Preset parameter**, not the kit's preset menu:
 * the same dropdown, last in the panel, Custom first. Choosing one writes its
 * eleven values, as `applyPreset` does, and leaves Sidechain, Audio Amount and
 * Mix alone, as the plugin does; moving a covered control off the preset's
 * value by a thousandth or more drops it to Custom, as `SetFloatParameter`
 * does. The panel moves its sliders to the preset, which is what a host that
 * honours the plugin's value events would do. Resolume does not, so in
 * Resolume the sliders stay put while the preset renders.
 *
 * **The clock.** The kit's: seconds from frame deltas, capped at a tenth of a
 * second a frame, handed to the port as the host's time already in seconds —
 * what `cmtest --pipe` does with SetClockScaleForTest( 1.0 ). The plugin's
 * vote on whether its host sends seconds or milliseconds, and its wall clock
 * until the vote is in, are not ported: this host's unit is known. A clock that
 * does not move forward is no time passing, so Pause holds the frame follower
 * where it is and Restart does not release it.
 *
 * **The About block is not a parameter here**, as on every page in the suite.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';
import * as S from './shaders.js';

// ==== BEGIN PORT ===========================================================
// Controls.h/.cpp, Compander.h/.cpp, Detector.h/.cpp, Chain.cpp's
// crossoverFor, Presets.h, and the CPU half of Plugin.cpp's ProcessOpenGL,
// ported. No DOM and no GL between the two markers: demo/tools/crosscheck/
// run.mjs slices this block out by its markers, runs it under node and diffs
// what it prints against the same printout from the plugin's own C++
// (printer.cpp).
//
// The plugin's arithmetic is float. JavaScript's is double, so every value is
// rounded to float (f32) where the C++ would hold one; the transcendental
// functions are V8's rather than Apple's libm, which is the remaining
// difference and is measured by the crosscheck rather than assumed away.

const f32 = Math.fround;
/** std::clamp, std::min and std::max exactly: comparisons, so -0 stays -0. */
const clamp = (v, lo, hi) => (v < lo ? lo : hi < v ? hi : v);
const max2 = (a, b) => (a < b ? b : a);
/** std::lround: half away from zero (Math.round alone rounds -2.5 to -2). */
const lround = (v) => (v < 0 ? -Math.round(-v) : Math.round(v));

//---------------------------------------------------------------------------
// Compander.h
//---------------------------------------------------------------------------

const kSideSignal = 0;
const kSideCount = 3;
const SIDECHAIN_NAMES = ['Picture', 'Audio', 'Picture x Audio'];

/** The link's own floor: -60 dB. Not an epsilon. */
const kDetectorFloor = f32(1.0e-3);
/** Width of the soft knee at the maximum-gain corner, in dB. */
const kKneeDb = f32(6.0);
/** The active picture as a fraction of the line, 625/50: 52 us of 64. */
const kActiveLineUs = f32(52.0);
const kMaxScanPasses = 14;
const kGainTableSize = 128;
const kGainTableFloorDb = f32(-60.0);

//---------------------------------------------------------------------------
// Compander.cpp
//---------------------------------------------------------------------------

/** dB from a linear level, floored at the detector's own noise floor. */
function toDb(level) {
  return f32(20.0 * f32(Math.log10(max2(level, kDetectorFloor))));
}

/** A soft ceiling of `limit` on a dB figure, quadratic knee kKneeDb wide. */
function softCeiling(x, limit) {
  const half = f32(0.5 * kKneeDb);

  if (x <= f32(limit - half)) return x;
  if (x >= f32(limit + half)) return limit;

  const over = f32(x - f32(limit - half));
  return f32(x - f32(f32(over * over) / f32(2.0 * kKneeDb)));
}

function samplesPerMicrosecond(width) {
  return f32(max2(width, 1) / kActiveLineUs);
}

/** A time constant in microseconds as a distance in samples, floored at one. */
function timeConstantSamples(us, width) {
  return max2(1.0, f32(us * samplesPerMicrosecond(width)));
}

/** Doubling passes for four time constants of history, clamped to the cap. */
function scanPasses(tauSamples) {
  const reach = f32(4.0 * max2(tauSamples, 1.0));
  const need = Math.trunc(Math.ceil(f32(Math.log2(max2(reach, 2.0)))));

  return clamp(need, 1, kMaxScanPasses);
}

/** How much of the envelope is the frame follower's: 0 until the octave below the cap. */
function frameBlend(tauSamples) {
  const reach = f32(1 << kMaxScanPasses);
  const tauFull = f32(0.25 * reach);
  const tauFrom = f32(0.5 * tauFull);

  if (tauSamples <= tauFrom) return 0.0;
  if (tauSamples >= tauFull) return 1.0;

  const t = f32(f32(tauSamples - tauFrom) / f32(tauFull - tauFrom));
  return f32(f32(t * t) * f32(3.0 - f32(2.0 * t)));
}

function releaseCoefficient(tauSamples) {
  return f32(Math.exp(f32(-1.0 / max2(tauSamples, 1.0))));
}

/** The compressor: a straight line in dB through the pivot, its boost bounded. */
function compressGain(level, pivot, ratio, maxGainDb) {
  const d = f32(toDb(level) - toDb(pivot));
  const slope = f32(f32(1.0 / max2(ratio, 1.0)) - 1.0);
  const gdb = f32(d * slope);

  return f32(Math.pow(10.0, f32(softCeiling(gdb, max2(maxGainDb, 0.0)) / 20.0)));
}

/** The expander: the same law with the ratio inverted, its cut bounded to match. */
function expandGain(level, pivot, ratio, maxGainDb) {
  const d = f32(toDb(level) - toDb(pivot));
  const slope = f32(max2(ratio, 1.0) - 1.0);
  const gdb = f32(d * slope);

  return f32(Math.pow(10.0, f32(f32(-softCeiling(-gdb, max2(maxGainDb, 0.0))) / 20.0)));
}

/**
 * fillGainTables: 128 compress gains and 128 expand gains, level running over
 * -60..0 dB, log spaced. The curves exist here and reach the shaders only as
 * these two uniform arrays, as in the plugin.
 */
function fillGainTables(s) {
  const compress = new Float32Array(kGainTableSize);
  const expand = new Float32Array(kGainTableSize);

  for (let i = 0; i < kGainTableSize; i += 1) {
    const t = f32(i / (kGainTableSize - 1));
    const db = f32(kGainTableFloorDb + f32(t * -kGainTableFloorDb));
    const level = f32(Math.pow(10.0, f32(db / 20.0)));

    compress[i] = compressGain(level, s.pivot, s.compressRatio, s.maxGainDb);
    expand[i] = expandGain(level, s.pivot, s.expandRatio, s.maxGainDb);
  }
  return { compress, expand };
}

//---------------------------------------------------------------------------
// Controls.h/.cpp: the only place a 0..1 slider becomes a physical quantity.
//---------------------------------------------------------------------------

const clamp01 = (v) => clamp(v, 0.0, 1.0);
/** Geometric interpolation: half a slider is the geometric middle of the range. */
const expRange = (v, lo, hi) => f32(lo * f32(Math.pow(f32(hi / lo), clamp01(v))));
const linRange = (v, lo, hi) => f32(lo + f32(f32(hi - lo) * clamp01(v)));

/** 1:1 .. 4:1, linear in the ratio, so 2:1 sits at a third. */
const ratioFromParam = (v) => linRange(v, f32(1.0), f32(4.0));
/** 0 .. 12 dB. */
const emphasisDbFromParam = (v) => linRange(v, f32(0.0), f32(12.0));
/** -12 .. +12 dB, centred. */
const linkLevelDbFromParam = (v) => linRange(v, f32(-12.0), f32(12.0));
/** 0 .. 0.15 linear amplitude on the encoded signal. */
const noiseFromParam = (v) => linRange(v, f32(0.0), f32(0.15));
/** 2.0 .. 0.5, exponential and DESCENDING: less headroom is the harder setting. */
const headroomFromParam = (v) => expRange(v, f32(2.0), f32(0.5));
/** -1 .. +1, centred. */
const tiltFromParam = (v) => linRange(v, f32(-1.0), f32(1.0));
/** 0.1 us .. 40 ms, exponential: five and a half decades on one control. */
const timeConstantUsFromParam = (v) => expRange(v, f32(0.1), f32(40000.0));
/** 0.02 .. 1.0 signal level, exponential. */
const pivotFromParam = (v) => expRange(v, f32(0.02), f32(1.0));
/** 0 .. 48 dB. */
const maxGainDbFromParam = (v) => linRange(v, f32(0.0), f32(48.0));

/**
 * controls::HostValues: the constructor's defaults, every one 0..1, option
 * parameters holding their element index. The panel below is declared from
 * these, and the crosscheck holds them to Controls.h.
 */
const DEFAULTS = {
  sidechain: f32(0.0),
  chroma: f32(0.0),
  compress: f32(0.333),
  emphasis: f32(0.5),
  linkLevel: f32(0.5),
  noise: f32(0.13),
  headroom: f32(0.6),
  expand: f32(0.333),
  tilt: f32(0.5),
  timeConstant: f32(0.55),
  pivot: f32(0.7),
  maxGain: f32(0.5),
  audioAmount: f32(0.7),
  audioBand: f32(0.2),
  audioTilt: f32(0.5),
  mix: f32(1.0),
};

/** controls::toSettings: a full set of host values in physical units. */
function toSettings(v) {
  return {
    sidechain: clamp(lround(v.sidechain), 0, kSideCount - 1),
    chroma: clamp01(v.chroma),

    compressRatio: ratioFromParam(v.compress),
    emphasisDb: emphasisDbFromParam(v.emphasis),

    linkLevelDb: linkLevelDbFromParam(v.linkLevel),
    noise: noiseFromParam(v.noise),
    headroom: headroomFromParam(v.headroom),

    expandRatio: ratioFromParam(v.expand),
    tilt: tiltFromParam(v.tilt),

    timeConstantUs: timeConstantUsFromParam(v.timeConstant),
    pivot: pivotFromParam(v.pivot),
    maxGainDb: maxGainDbFromParam(v.maxGain),

    audioAmount: clamp01(v.audioAmount),
    audioBand: clamp01(v.audioBand),
    audioTilt: tiltFromParam(v.audioTilt),

    mix: clamp01(v.mix),
  };
}

//---------------------------------------------------------------------------
// Presets.h, rows as written (check_shaders.py compares them with the C++),
// and the order Plugin.h's kPresetParamIDs binds them to parameters in.
//---------------------------------------------------------------------------

const PRESETS = [
  ['Two-to-One VCA', [0.000, 0.333, 0.500, 0.500, 0.200, 0.257, 0.333, 0.500, 0.357, 0.823, 0.500]],
  ['High Density', [0.000, 0.267, 0.750, 0.500, 0.133, 0.161, 0.267, 0.500, 0.442, 0.732, 0.375]],
  ['Digital Hybrid', [0.000, 0.333, 0.250, 0.500, 0.067, 0.000, 0.333, 0.500, 0.264, 0.823, 0.625]],
  ['Diversity Fade', [0.000, 0.333, 0.500, 0.250, 0.533, 0.257, 0.333, 0.500, 0.536, 0.823, 0.500]],
  ['Cheap Handheld', [0.000, 0.667, 0.917, 0.375, 0.400, 0.661, 0.467, 0.625, 0.660, 0.732, 0.625]],
  ['Tape Luma', [0.000, 0.167, 1.000, 0.500, 0.333, 0.757, 0.167, 0.650, 0.179, 0.732, 0.250]],
  ['Satellite Link', [1.000, 0.267, 0.583, 0.417, 0.467, 0.500, 0.267, 0.500, 0.482, 0.823, 0.500]],
  ['ENG Microwave', [1.000, 0.333, 0.750, 0.333, 0.600, 0.576, 0.300, 0.425, 0.567, 0.823, 0.500]],
  ['Encode Only', [0.000, 0.500, 0.750, 0.500, 0.100, 0.369, 0.000, 1.000, 0.357, 0.823, 0.500]],
  ['Decode Only', [0.000, 0.000, 0.750, 0.500, 0.100, 0.000, 0.500, 0.000, 0.357, 0.823, 0.500]],
];

/** kPresetParamIDs, as this page's parameter ids. */
const PRESET_TARGETS = [
  'chroma', 'compress', 'emphasis', 'linkLevel', 'noise',
  'headroom', 'expand', 'tilt', 'timeConstant', 'pivot', 'maxGain',
];

/** presetValue(): the active preset's value for `id`, or -1 if it has no opinion. */
function presetValue(presetIndex, id) {
  if (presetIndex <= 0 || presetIndex > PRESETS.length) return -1.0;
  const column = PRESET_TARGETS.indexOf(id);
  return column < 0 ? -1.0 : f32(PRESETS[presetIndex - 1][1][column]);
}

//---------------------------------------------------------------------------
// Detector.h/.cpp: the pass schedule, and the frame follower's units.
//---------------------------------------------------------------------------

/** planScan: the doubling passes for a time constant in FULL-resolution samples. */
function planScan(tauFullResSamples, fullWidth, detectorWidth) {
  const scale = detectorWidth > 0 && fullWidth > 0 ? f32(detectorWidth / fullWidth) : 1.0;

  const tau = max2(1.0, f32(tauFullResSamples * scale));
  const blend = frameBlend(tauFullResSamples);

  const a = releaseCoefficient(tau);
  const n = scanPasses(tau);

  const passes = [];
  for (let k = 0; k < n; k += 1) {
    const offset = 1 << k;
    // a^offset as a power, not by repeated squaring, as the plugin does.
    passes.push({ offset, decay: f32(Math.pow(a, offset)) });
  }
  return { tau, passes, frameBlend: blend };
}

/** A time constant in scan samples as one in seconds: 64 us a line. */
function samplesToSeconds(tauSamples, width) {
  const linesPerSample = width > 0 ? f32(1.0 / width) : 1.0;
  const lineSeconds = f32(64.0e-6);

  return f32(f32(tauSamples * linesPerSample) * lineSeconds);
}

//---------------------------------------------------------------------------
// Chain.cpp: crossoverFor, the band split's two horizontal blur radii.
//---------------------------------------------------------------------------

function crossoverFor(width) {
  const w = max2(width, 8);
  const narrow = max2(1.0, f32(w / 512.0));
  const wide = max2(f32(narrow * 1.5), f32(w / 64.0));
  return { narrow, wide };
}

//---------------------------------------------------------------------------
// Plugin.cpp's ProcessOpenGL, the CPU half: everything that decides what each
// pass is fed, as one object per instance. step() is called once a frame, in
// the plugin's order, and RETURNS the frame's plan and uniforms; the GL code
// binds and draws exactly that and nothing else.
//---------------------------------------------------------------------------

/**
 * UpdateAudio() over a silent spectrum: every bin 0, so the smoothed levels
 * stay 0, the band-weighted envelope is 0 / weight = 0 for any Audio Band, and
 * the spectral balance is 0 (total 0, under its 1e-4 floor). This page can
 * offer no other spectrum.
 */
const kSilentEnvelope = 0.0;
const kSilentBalance = 0.0;

function createFrameLogic() {
  // Plugin.h's state. frameFirst starts true: InitGL sets it the first time.
  let bufferWidth = 0;
  let bufferHeight = 0;
  let frameSlot = 0;
  let frameFirst = true;
  let frameCounter = 0;
  let lastNow = -1.0;

  return {
    /**
     * @param {object} values  host values: every control, 0..1 as the host holds them
     * @param {number} width   the input's size (source.Width / source.Height)
     * @param {number} height
     * @param {number} time    the host clock, in seconds
     */
    step(values, width, height, time) {
      // EnsureBuffers: a new size is a different picture, so the frame
      // follower takes the next level rather than releasing towards the old.
      const detWidth = Math.max(1, Math.trunc(width / 4));
      if (width !== bufferWidth || height !== bufferHeight) {
        bufferWidth = width;
        bufferHeight = height;
        frameFirst = true;
      }

      // AdvanceClock, with the unit already settled as seconds. Time that does
      // not move forward is no time passing.
      const now = time;
      const dt = lastNow >= 0.0 && now > lastNow ? now - lastNow : 0.0;
      lastNow = now;

      const settings = toSettings(values);

      const tauSamples = timeConstantSamples(settings.timeConstantUs, bufferWidth);
      const plan = planScan(tauSamples, bufferWidth, detWidth);
      const cross = crossoverFor(bufferWidth);
      const tables = fillGainTables(settings);

      // 4. the frame-global follower.
      const prev = frameSlot;
      frameSlot = 1 - frameSlot;
      const tauSeconds = samplesToSeconds(tauSamples, bufferWidth);
      const decay = f32(Math.exp(f32(-f32(max2(dt, 0.0)) / max2(tauSeconds, f32(1.0e-4)))));
      const first = frameFirst;
      frameFirst = false;

      const frame = frameCounter;
      frameCounter = (frameCounter + 1) & 0x00ffffff;

      return {
        width: bufferWidth,
        height: bufferHeight,
        detWidth,
        dt,
        settings,
        plan,
        cross,
        tables,
        follower: { prev, slot: frameSlot, decay, first },
        encode: {
          EmphasisDb: settings.emphasisDb,
          LinkGain: f32(Math.pow(10.0, f32(settings.linkLevelDb / 20.0))),
          Headroom: settings.headroom,
          Noise: settings.noise,
          FrameBlend: plan.frameBlend,
          AudioEnv: kSilentEnvelope,
          AudioAmount: settings.audioAmount,
          SidechainMode: settings.sidechain,
          Frame: frame,
        },
        decode: { FrameBlend: plan.frameBlend },
        output: {
          EmphasisDb: settings.emphasisDb,
          // Audio Tilt rides on top of Tilt; with silence the balance is 0.
          Tilt: clamp(f32(settings.tilt + f32(settings.audioTilt * kSilentBalance)), -1.0, 1.0),
          Chroma: settings.chroma,
          Mix: settings.mix,
        },
      };
    },
  };
}
// ==== END PORT =============================================================

//---------------------------------------------------------------------------
// GL: Plugin.cpp's buffers and passes, in its order. Binds and draws what the
// frame logic returns; computes nothing.
//---------------------------------------------------------------------------

/** The host values the frame logic reads: the panel's, f32 as the host holds them. */
function hostValues(params) {
  const v = { ...DEFAULTS };
  for (const id of Object.keys(DEFAULTS)) {
    if (params.byId.has(id)) v[id] = f32(params.get(id));
  }
  return v;
}

function createRenderer(gl, quad) {
  const assemble = (piece) => piece.replace('@COMMON@', () => S.COMMON);

  // InitGL's eight programs, each against kVertex.
  const P = {
    luma: new Program(gl, S.VERTEX, S.LUMA_FRAGMENT, 'luma'),
    blur: new Program(gl, S.VERTEX, S.BLUR_FRAGMENT, 'blur'),
    reduce: new Program(gl, S.VERTEX, S.REDUCE_FRAGMENT, 'reduce'),
    scan: new Program(gl, S.VERTEX, S.SCAN_FRAGMENT, 'scan'),
    frame: new Program(gl, S.VERTEX, S.FRAME_FRAGMENT, 'frame'),
    encode: new Program(gl, S.VERTEX, assemble(S.ENCODE_FRAGMENT), 'encode'),
    decode: new Program(gl, S.VERTEX, assemble(S.DECODE_FRAGMENT), 'decode'),
    output: new Program(gl, S.VERTEX, assemble(S.OUTPUT_FRAGMENT), 'output'),
  };

  // EnsureBuffers: five full-size R16F buffers, two quarter-width, two 1x1.
  // Linear and clamped, as FFGLFBO::GenerateColorTexture makes them. WebGL
  // zero-fills a new texture, which is what PassBuffer::Ensure's clear does.
  const buffer = () => new PassBuffer(gl, { filter: 'linear' });
  const B = {
    luma: buffer(),
    blur: [buffer(), buffer()],
    encoded: buffer(),
    decoded: buffer(),
    detector: [buffer(), buffer()],
    frame: [buffer(), buffer()],
  };

  const logic = createFrameLogic();

  const bind = (textures) => textures.forEach((t, unit) => bindTexture(gl, unit, t));
  const unbind = (count) => {
    for (let unit = count - 1; unit >= 0; unit -= 1) bindTexture(gl, unit, null);
  };

  function render({ input, params, time }) {
    // ScopedFBOBinding does not restore the viewport, so the plugin captures
    // the host's before anything changes it. The page's host is the kit, which
    // has the canvas (or the crosscheck's target) bound.
    const hostFbo = gl.getParameter(gl.FRAMEBUFFER_BINDING);
    const hostViewport = gl.getParameter(gl.VIEWPORT);

    const f = logic.step(hostValues(params), input.width, input.height, time);
    const { width: W, height: H, detWidth, plan, cross, tables } = f;

    B.luma.ensure(W, H, gl.R16F);
    B.blur[0].ensure(W, H, gl.R16F);
    B.blur[1].ensure(W, H, gl.R16F);
    B.encoded.ensure(W, H, gl.R16F);
    B.decoded.ensure(W, H, gl.R16F);
    B.detector[0].ensure(detWidth, H, gl.R16F);
    B.detector[1].ensure(detWidth, H, gl.R16F);
    B.frame[0].ensure(1, 1, gl.R16F);
    B.frame[1].ensure(1, 1, gl.R16F);

    //-- 1. luma
    B.luma.bind();
    P.luma.use().setSampler('Source', 0).set('MaxUV', 1, 1);
    bind([input.texture]);
    quad.draw();
    unbind(1);

    //-- 2. the band split of the INPUT
    const blurPass = (dst, src, radius) => {
      dst.bind();
      P.blur.use()
        .setSampler('Source', 0)
        .set('MaxUV', 1, 1)
        .set('TexelSize', 1 / W, 1 / H)
        .set('Radius', radius);
      bind([src]);
      quad.draw();
      unbind(1);
    };
    blurPass(B.blur[0], B.luma.texture, cross.narrow);
    blurPass(B.blur[1], B.luma.texture, cross.wide);

    //-- 3. the detector: reduce to quarter width, then the doubling passes.
    const runDetector = (src) => {
      B.detector[0].bind();
      P.reduce.use().setSampler('Source', 0).set('MaxUV', 1, 1);
      bind([src]);
      quad.draw();
      unbind(1);

      let from = 0;
      for (const pass of plan.passes) {
        const to = 1 - from;
        B.detector[to].bind();
        P.scan.use()
          .setSampler('Source', 0)
          .set('MaxUV', 1, 1)
          .setInt('Offset', pass.offset)
          .set('Decay', pass.decay);
        bind([B.detector[from].texture]);
        quad.draw();
        unbind(1);
        from = to;
      }
      return from;
    };
    const encodeEnv = runDetector(B.luma.texture);

    //-- 4. the frame-global follower, ping-ponged across frames
    const { prev, slot, decay, first } = f.follower;
    B.frame[slot].bind();
    P.frame.use()
      .setSampler('Envelope', 0)
      .setSampler('Previous', 1)
      .set('MaxUV', 1, 1)
      .set('Decay', decay)
      .setInt('First', first ? 1 : 0);
    bind([B.detector[encodeEnv].texture, B.frame[prev].texture]);
    quad.draw();
    unbind(2);

    //-- 5. encode
    const e = f.encode;
    B.encoded.bind();
    P.encode.use()
      .setSampler('Luma', 0)
      .setSampler('BlurNarrow', 1)
      .setSampler('BlurWide', 2)
      .setSampler('Envelope', 3)
      .setSampler('FrameLevel', 4)
      .set('MaxUV', 1, 1)
      .set('EmphasisDb', e.EmphasisDb)
      .set('LinkGain', e.LinkGain)
      .set('Headroom', e.Headroom)
      .set('Noise', e.Noise)
      .set('FrameBlend', e.FrameBlend)
      .set('AudioEnv', e.AudioEnv)
      .set('AudioAmount', e.AudioAmount)
      .setInt('SidechainMode', e.SidechainMode)
      .setInt('Frame', e.Frame)
      .setArray('CompressTable', tables.compress)
      .setArray('ExpandTable', tables.expand);
    bind([B.luma.texture, B.blur[0].texture, B.blur[1].texture, B.detector[encodeEnv].texture, B.frame[slot].texture]);
    quad.draw();
    unbind(5);

    //-- 6. the SECOND detector, on the encoded signal
    const decodeEnv = runDetector(B.encoded.texture);

    //-- 7. decode
    B.decoded.bind();
    P.decode.use()
      .setSampler('Encoded', 0)
      .setSampler('Envelope', 1)
      .setSampler('FrameLevel', 2)
      .set('MaxUV', 1, 1)
      .set('FrameBlend', f.decode.FrameBlend)
      .setArray('CompressTable', tables.compress)
      .setArray('ExpandTable', tables.expand);
    bind([B.encoded.texture, B.detector[decodeEnv].texture, B.frame[slot].texture]);
    quad.draw();
    unbind(3);

    //-- 8. the band split of the DECODED signal, into the same two buffers
    blurPass(B.blur[0], B.decoded.texture, cross.narrow);
    blurPass(B.blur[1], B.decoded.texture, cross.wide);

    //-- 9. output, to the host's framebuffer and viewport
    const o = f.output;
    gl.bindFramebuffer(gl.FRAMEBUFFER, hostFbo);
    gl.viewport(hostViewport[0], hostViewport[1], hostViewport[2], hostViewport[3]);
    P.output.use()
      .setSampler('Source', 0)
      .setSampler('Luma', 1)
      .setSampler('Decoded', 2)
      .setSampler('BlurNarrow', 3)
      .setSampler('BlurWide', 4)
      // The host's MaxUV: the page's input texture is never padded.
      .set('MaxUV', 1, 1)
      .set('EmphasisDb', o.EmphasisDb)
      .set('Tilt', o.Tilt)
      .set('Chroma', o.Chroma)
      .set('Mix', o.Mix);
    bind([input.texture, B.luma.texture, B.decoded.texture, B.blur[0].texture, B.blur[1].texture]);
    quad.draw();
    unbind(5);

    return f;
  }

  function dispose() {
    for (const p of Object.values(P)) p.dispose();
    for (const b of [B.luma, ...B.blur, B.encoded, B.decoded, ...B.detector, ...B.frame]) b.dispose();
  }

  return { render, dispose };
}

//---------------------------------------------------------------------------
// The presets, as the plugin applies them (see the header).
//---------------------------------------------------------------------------

/** applyPreset: write the row's eleven values. Nothing else moves. */
function applyPreset(params, presetIndex) {
  if (presetIndex <= 0 || presetIndex > PRESETS.length) return;
  for (const id of PRESET_TARGETS) params.set(id, presetValue(presetIndex, id), { silent: true });
}

/**
 * Wire the Preset dropdown to the plugin's rules: choosing one applies it, and
 * a real edit to a covered control drops it to Custom. A value within 1e-3 of
 * the preset's is the plugin's host-quantisation allowance: it renders the
 * preset's value and stays on the preset, so the slider is put back on it.
 */
function wirePresets(params, query) {
  let applying = false;
  const refresh = () => params.dispatchEvent(new CustomEvent('reset'));

  params.addEventListener('change', (event) => {
    const id = event.detail?.id;
    if (applying || id == null) return;

    const active = lround(params.get('preset'));
    if (id === 'preset') {
      applying = true;
      applyPreset(params, active);
      applying = false;
      refresh();
      return;
    }

    const covered = presetValue(active, id);
    if (active <= 0 || covered < 0) return;

    applying = true;
    if (Math.abs(f32(params.get(id)) - covered) < 1.0e-3) {
      params.set(id, covered, { silent: true });
    } else {
      params.set('preset', 0, { silent: true });
    }
    applying = false;
    refresh();
  });

  // A link carrying ?preset=N: the preset's values, except where the link names
  // a covered control itself; one that differs from the preset is an edit.
  const active = lround(params.get('preset'));
  if (active > 0) {
    let edited = false;
    for (const id of PRESET_TARGETS) {
      if (!query.has(id)) params.set(id, presetValue(active, id), { silent: true });
      else if (Math.abs(f32(params.get(id)) - presetValue(active, id)) >= 1.0e-3) edited = true;
    }
    if (edited) params.set('preset', 0, { silent: true });
    refresh();
  }
}

//---------------------------------------------------------------------------
// The parameters, exactly as Plugin::Plugin declares them: same names, same
// groups, same order, same defaults, same dropdown elements.
//
// Absent: the Audio buffer (PT_AUDIO, the 64-bin FFT input), Audio Band and
// Audio Tilt, for the reason at the top of this file, and the About block.
//---------------------------------------------------------------------------

const fixed = (n, d) => n.toFixed(d);
const signed = (n, d) => `${n >= 0 ? '+' : '−'}${Math.abs(n).toFixed(d)}`;
function microseconds(us) {
  if (us >= 1000) return `${fixed(us / 1000, us >= 10000 ? 1 : 2)} ms`;
  if (us >= 10) return `${fixed(us, 0)} µs`;
  return `${fixed(us, us >= 1 ? 1 : 2)} µs`;
}

const PARAMS = [
  // Signal
  { id: 'sidechain', name: 'Sidechain', type: 'option', elements: SIDECHAIN_NAMES, default: DEFAULTS.sidechain, group: 'Signal',
    hint: 'What drives the compressor. Picture is the honest circuit: the picture drives its own gain. There is no audio on this page, so Audio and Picture x Audio run on silence, exactly as the plugin does when its audio picker is set to something silent: the compressor’s control signal sits at 1 − Audio Amount, or the picture’s envelope times that.' },
  { id: 'chroma', name: 'Chroma', type: 'standard', default: DEFAULTS.chroma, group: 'Signal',
    display: (v) => fixed(clamp01(v), 2),
    hint: 'How much of the companding reaches the colour. 0 is the radio mic reading: one channel, luma, so lifting a shadow desaturates it. 1 is the video link reading: the whole colour scaled, saturation moving with level.' },

  // Encode
  { id: 'compress', name: 'Compress', type: 'standard', default: DEFAULTS.compress, group: 'Encode',
    display: (v) => `${fixed(ratioFromParam(f32(v)), 2)}:1`,
    hint: 'Compression ratio at the transmitter, 1:1 (a straight wire) to 4:1. 2:1, what almost every analogue wireless system used, sits at a third.' },
  { id: 'emphasis', name: 'Emphasis', type: 'standard', default: DEFAULTS.emphasis, group: 'Encode',
    display: (v) => `${fixed(emphasisDbFromParam(f32(v)), 1)} dB`,
    hint: 'Pre-emphasis: dB of lift on the detail band before the compressor, 0 to 12. A shelf, so a flat field is untouched.' },

  // Link
  { id: 'linkLevel', name: 'Link Level', type: 'standard', default: DEFAULTS.linkLevel, group: 'Link',
    display: (v) => `${signed(linkLevelDbFromParam(f32(v)), 1)} dB`,
    hint: 'Gain on the encoded signal on its way into the link, −12 to +12 dB, before the noise: the noise floor stays put while the signal moves against it, so the expander restores picture and noise by different amounts.' },
  { id: 'noise', name: 'Noise', type: 'standard', default: DEFAULTS.noise, group: 'Link',
    display: (v) => fixed(noiseFromParam(f32(v)), 3),
    hint: 'The link’s noise floor, 0 to 0.15 linear on the encoded signal. Load-bearing: companding exists to hide it.' },
  { id: 'headroom', name: 'Headroom', type: 'standard', default: DEFAULTS.headroom, group: 'Link',
    display: (v) => fixed(headroomFromParam(f32(v)), 2),
    hint: 'Where the link clips, 2.0 down to 0.5 — backwards on purpose, so the top of the slider is the slammed end. A tanh knee, not a square clip.' },

  // Decode
  { id: 'expand', name: 'Expand', type: 'standard', default: DEFAULTS.expand, group: 'Decode',
    display: (v) => `1:${fixed(ratioFromParam(f32(v)), 2)}`,
    hint: 'Expansion ratio at the receiver, 1:1 to 1:4. Complementary to Compress when the two match.' },
  { id: 'tilt', name: 'Tilt', type: 'standard', default: DEFAULTS.tilt, group: 'Decode',
    display: (v) => signed(tiltFromParam(f32(v)), 2),
    hint: 'The mismatch between the emphasis networks: 0 cancels, +1 leaves no de-emphasis at all, −1 de-emphasises twice.' },

  // Detector
  { id: 'timeConstant', name: 'Time Constant', type: 'standard', default: DEFAULTS.timeConstant, group: 'Detector',
    display: (v) => microseconds(timeConstantUsFromParam(f32(v))),
    hint: 'Attack and release, 0.1 µs to 40 ms of signal time, which on a picture is a distance along the scan: a halo at the bottom, a smear, streaks down the frame, broad banding, the whole picture pumping at the top.' },
  { id: 'pivot', name: 'Pivot', type: 'standard', default: DEFAULTS.pivot, group: 'Detector',
    display: (v) => fixed(pivotFromParam(f32(v)), 3),
    hint: 'The level the law pivots about, 0.02 to 1.0: signal there passes both ends at unity whatever the ratios.' },
  { id: 'maxGain', name: 'Max Gain', type: 'standard', default: DEFAULTS.maxGain, group: 'Detector',
    display: (v) => `${fixed(maxGainDbFromParam(f32(v)), 1)} dB`,
    hint: 'The most boost the compressor will apply and the most cut the expander will, 0 to 48 dB. Not a knee: a bound, with a 6 dB soft corner where it runs out.' },

  // Audio — the buffer, Audio Band and Audio Tilt are absent (see the header).
  { id: 'audioAmount', name: 'Audio Amount', type: 'standard', default: DEFAULTS.audioAmount, group: 'Audio',
    display: (v) => fixed(clamp01(v), 2),
    hint: 'How hard the audio envelope drives the sidechain. There is no audio here, so the envelope is the plugin’s silent one, 0: on Audio or Picture x Audio this sets where the control signal sits (1 − Audio Amount); on Picture it does nothing, as in the plugin.' },

  // Output
  { id: 'mix', name: 'Mix', type: 'standard', default: DEFAULTS.mix, group: 'Output',
    display: (v) => fixed(clamp01(v), 2) },

  // Preset
  { id: 'preset', name: 'Preset', type: 'option', elements: ['Custom', ...PRESETS.map(([name]) => name)], default: 0, group: 'Preset',
    hint: 'The plugin’s factory presets: choosing one sets its eleven controls and leaves Sidechain, Audio Amount and Mix alone. Moving one of those eleven drops it back to Custom.' },
];

export { PARAMS, PRESETS, DEFAULTS, createRenderer, applyPreset, toSettings };

//---------------------------------------------------------------------------
// The page. Not mounted when there is no #demo: the crosscheck's harness
// imports this module for its renderer and has a page of its own.
//---------------------------------------------------------------------------

if (document.querySelector('#demo')) {
  const query = new URLSearchParams(window.location.search);

  const mounted = mountDemo({
    name: 'Compander',
    pluginId: 'CM01',
    kind: 'effect',
    tagline:
      'An analogue radio mic’s companding circuit, with your footage pushed through it instead of a microphone signal. The transmitter lifts the quiet top end and halves the signal’s excursion in dB; the receiver does the exact opposite; between them sits a link with a noise floor and a ceiling. Get every stage right and almost nothing happens. Everything this is for comes from the two ends disagreeing, and from one idea: a compander’s time constant, applied to a picture, is a distance along the scan.',
    repo: 'https://github.com/stoatworks-labs/compander',
    page: 'https://stoatworks-labs.com/software/compander/',
    video: 'https://www.youtube.com/watch?v=AMorya_4GKE',

    blurb:
      'It is Compander’s own GLSL — all eight of its shader programs, run pass for pass as the plugin runs them on the same half-float buffers — ported from the repository to WebGL2 and running on generated clips in this page, with the plugin’s gain tables, control conversions and frame logic ported to JavaScript. There is no audio here, so the audio sidechain runs on silence.',

    needFloat: true,

    // Lights on black first: bright objects on near-black are where the
    // detector's trail to the right, the lifted floor and the breathing noise
    // are easiest to read. The geometry card's hard vertical and horizontal
    // edges show that only one of them moves; the ramp shows the law.
    sources: ['spot', 'scene', 'grid', 'ramp', 'bars', 'detail'],

    params: PARAMS,

    differences: [
      'The CPU half is a port, not the plugin’s code. The control conversions (Controls.cpp), the gain laws and the two 128-point tables the shaders read them through (Compander.cpp), the detector’s pass plan and the frame follower’s time constant (Detector.cpp), the band-split radii and the part of ProcessOpenGL that turns the controls and the clock into each pass’s uniforms are JavaScript here. A check in the repository runs the port and the plugin’s own C++ side by side — every slider position through every conversion, the gain tables for the defaults, every preset and 384 other settings, and the detector’s plan across Time Constant at seven widths, 139,262 floats in all. Against the plugin’s arithmetic as its Intel slice does it, 99.7% are identical to the bit and the rest within 2 units in the last place (JavaScript’s Math is not Apple’s libm), the per-pass decays aside, which are a 1-ulp difference raised to a power of up to 8,192 and are held to exactly that. The Apple Silicon build fuses multiply-adds, which JavaScript cannot, and differs by up to 23 ulp in the gain tables.',
      'The pictures were compared too: a second check drives this page’s renderer and the real plugin class (its own offline harness, cmtest --pipe) through the same 330 input frames on the same clock — fourteen cases covering the defaults, every Sidechain mode, five presets, two widths, the blend into the frame follower and the follower releasing after a light is switched down — and on the M4 Max it was run on, every colour channel of every frame came out within one level of 255 of the plugin’s, with a mean difference of at most 0.0008 of a level. Through SwiftShader, Chrome’s software renderer, it is within two levels. Six deliberately wrong runs (a time constant a hundredth out, the plugin told the wrong frame rate, the wrong preset) all fail the same test.',
      'The GPU half is not a port: the luma, blur, reduce, scan, frame, encode, decode and output passes are the plugin’s own GLSL, on the plugin’s R16F buffers (WebGL2 needs EXT_color_buffer_float to render into them, and the page stops and says so without it). demo/tools/check_shaders.py fails the repository’s verify script if a character of them, the way kCommon is spliced into them, or a number of the preset table drifts.',
      'The envelope’s memory is kept the plugin’s way, on the GPU: the frame-global follower is one half-float value carried from frame to frame in a pair of 1×1 buffers, attacked instantly and released by the frame’s own duration in seconds. Nothing is read back. So the long end of Time Constant pumps here as it does in Resolume, and a release takes the same time whatever the frame rate; only how often it is sampled differs.',
      'No audio. The plugin’s Audio input is a 64-bin spectrum Resolume hands it as an FFT parameter, and a browser has no equivalent; asking for your microphone to demonstrate a video effect is not a trade worth making. So the Audio buffer, Audio Band and Audio Tilt are absent from the panel rather than present and dead. What runs is exactly the plugin with a silent spectrum: an envelope of 0 and a balance of 0. Sidechain keeps all three entries, and Audio and Picture x Audio do what the plugin does in a silent room — the compressor’s control signal sits at 1 − Audio Amount, or the picture’s envelope times it — so Audio Amount stays. The plugin’s own audio path has never been driven by a real spectrum either; the repository says so.',
      'Presets: the plugin’s own Preset parameter and its ten rows, applied its way (Sidechain, Audio Amount and Mix untouched; a covered control moved by a thousandth or more drops it to Custom). The panel moves its sliders to the preset’s values, as a host that honours the plugin’s value events would. Resolume does not, so there the sliders stay where they were while the preset renders.',
      'The clock is the page’s: seconds from frame deltas, capped at a tenth of a second a frame. The plugin’s vote on whether its host counts in seconds or milliseconds, and the wall clock it runs on until the vote is in, are not ported, because this host’s unit is known. A clock that does not move forward is no time passing, so Pause holds the frame follower and Restart does not release it; Step is a sixtieth of a second. The noise moves on every frame drawn, as the plugin’s does on every ProcessOpenGL, so moving a control while paused redraws it.',
      'The About block is not on the panel: a line of text and buttons that open a browser.',
    ],

    createRenderer: (gl, quad) => createRenderer(gl, quad),
  });

  if (mounted) wirePresets(mounted.params, query);
}
