#pragma once

namespace compander
{
/**
    A radio mic's companding circuit, with a picture pushed through it.

    ------------------------------------------------------------- the one idea

    **A compander is a round trip, and everything anybody recognises about it is
    the two ends failing to cancel.**

    An analogue radio mic link cannot carry the dynamic range of the thing it is
    sent. So the transmitter squashes it -- pre-emphasis to lift the quiet top
    end above the link's noise, then a compressor that halves the signal's
    excursion in dB -- and the receiver does the exact opposite, expanding it
    back out and de-emphasising. In between sits a link with a noise floor, a
    ceiling, and no opinion about either.

    That chain, in that order, is this plugin:

        in -> EMPHASIS -> COMPRESS -> [ link: level, noise, headroom ] -> EXPAND -> DE-EMPHASIS -> out

    Get every stage right and almost nothing happens, which is the point: a
    working radio mic sounds like a cable. Everything an operator wants from
    this plugin comes from the ends disagreeing --

    - **Compress with nothing expanding** and the picture goes flat and bright,
      shadows lifted off the floor, edges screaming where the pre-emphasis was
      never taken back off.
    - **Expand with nothing compressed** and it goes contrasty and dead, blacks
      crushed below the floor, fine detail sucked out of everything dim.
    - **Mistrack the level** -- `Link Level` off centre -- and the signal
      crosses the link sitting at the wrong height above its noise floor, so
      the expander restores the picture and the noise by different amounts and
      the shadows breathe as the shot changes.

      ⚠️ Note what this control is NOT. It is a gain on the signal *entering*
      the link, not an offset of the expander's reference. Offsetting the
      reference is the obvious way to write "mistracking" and it does nothing
      interesting: the law is a straight line in dB, so it is scale-free, and
      moving the point it pivots about comes out as a flat level shift at every
      brightness -- a brightness knob with a misleading name. Measured, not
      reasoned about: `cmtest --roundtrip` printed a constant -3.00 dB at every
      input level for a 3 dB reference offset, which is what sent this back to
      the drawing board.

      Level-dependence in this plugin comes from the three places it comes from
      in a real circuit: the gain bound running out (`Max Gain`), the two ends
      being set to different ratios, and the two detectors looking at different
      signals. Not from a pivot offset.
    - **Tilt** the de-emphasis away from the emphasis and the round trip cancels
      in level but not in frequency: haloed and glassy one way, soft and smeared
      the other.

    ⚠️ **The noise is load-bearing, not decoration.** Companding exists to hide a
    link's noise floor, and a compander with no noise to hide is an identity
    function with a lot of steps. `Noise` above zero is part of this working;
    turning it to zero to see the companding alone is a legitimate thing to do
    and not the default.

    ------------------------------------------------ the scan is the time axis

    **This is the idea the whole plugin is built on, and it is what makes it a
    video effect rather than an audio compressor pointed at a picture.**

    A compander's most important number is its attack and release time. A signal
    on a wire has one axis, time, and when that signal is a picture, time is the
    scan -- left to right, line after line. So a time constant is not an
    abstraction to be reinterpreted here. It converts, exactly, into a distance
    along the scan:

        active line = 52 us of a 64 us line (625/50)
        samples per microsecond = width / 52

    and that single conversion is why one control covers three completely
    different-looking artifacts:

    | time constant | distance at 1920 wide | what it looks like            |
    |---------------|-----------------------|-------------------------------|
    | 0.2 us        | 7 samples             | haloing tight to every edge   |
    | 5 us          | 185 samples           | a smear off the side of things|
    | 60 us         | ~1.1 lines            | streaks pulling down the frame|
    | 2 ms          | ~37 lines             | broad vertical banding        |
    | 20 ms         | ~1/2 frame           | the whole picture pumping     |

    Two consequences fall out of taking this seriously, and both are the
    give-away that a real circuit is being modelled:

    **It is causal, and it trails to the RIGHT.** The detector can only know
    what has already gone past. A bright object drags its gain change out behind
    it across the scan and leaves the left-hand side untouched -- the asymmetric
    tail every analogue video level artifact has. A symmetric halo is the
    signature of an effect that was never a circuit.

    **It wraps from line to line.** Sample `n - d` when `d` runs past the left
    edge is the right-hand end of the line above, because that is where the
    signal was `d` samples ago. Clamping to the edge instead would be a
    perfectly reasonable image-processing decision and would quietly delete the
    entire multi-line range of the control.

    ---------------------------------------------------- what the detector does

    Instant attack, exponential release -- a peak detector, which is what a
    cheap compander has:

        E(n) = max over k >= 0 of ( level(n-k) * a^k ),   a = exp(-1/tau)

    Written that way it is a maximum over decayed history, and a maximum over a
    window is the one recursive filter that parallelises exactly, by recursive
    doubling:

        E_0(n)     = level(n)
        E_{k+1}(n) = max( E_k(n), a^(2^k) * E_k(n - 2^k) )

    After K passes it has seen 2^K samples back. K is chosen from the time
    constant and no larger, so a short setting costs three or four passes and
    only the longest costs the cap. This is exact for the peak detector, not an
    approximation of it -- see `scanPasses` and `Detector.h`.

    ⚠️ **The cap means the detector cannot reach a whole frame**, and the range
    above it is served by a second, frame-global envelope with a one-pole across
    frames, crossfaded in over the last octave. See `Detector.h` for where the
    join is and what it costs.

    -------------------------------------------------------- two detectors, not one

    The encoder's detector sees the emphasised input. The decoder's sees what
    came off the link -- compressed, noisy, at whatever level the link handed
    it. **They are different signals, and that is the whole reason a real
    compander mistracks.** Nothing here corrects for it: the residual is the
    effect, and `cmtest --roundtrip` reports how large it is rather than this
    file claiming it is zero.

    ------------------------------------------------------------------ naming

    The preset curves are the well-known wireless systems and the video links
    that used the same trick, and they are named for what they are rather than
    for whose trademark they are: a broadband two-to-one VCA compander, a
    dual-band one, a digital-hybrid one, a tape-format luma channel, a satellite
    up-link. Those are the terms the specifications use and the terms somebody
    looking for this behaviour will search for. See `Presets.h`.
*/

/// Where the compressor's control signal comes from.
///
/// A real compander has exactly one answer to this -- the signal itself -- and
/// `kSideSignal` is that answer. The other two exist because of what the
/// control signal IS: a compander is a circuit whose gain is driven by an audio
/// envelope, so handing it a real audio envelope is not a gimmick bolted onto a
/// video effect. It is the circuit being fed the kind of signal it was built
/// for, with the picture standing in for the carrier.
enum Sidechain
{
	/// The picture drives its own gain. The honest circuit.
	kSideSignal = 0,

	/// The music drives it. The picture is compressed and expanded by an
	/// envelope it has nothing to do with, so the round trip stops cancelling
	/// in time with the track rather than in time with the shot.
	kSideAudio,

	/// Both, multiplied. Level-dependent as a circuit is, and gated by the
	/// music on top -- which is the one that survives a whole show, because it
	/// still follows the footage.
	kSideBoth,

	kSideCount
};

/// The controls, in physical units. `Controls.cpp` is the only place a 0..1
/// slider becomes one of these.
struct Settings
{
	//-- Encode -----------------------------------------------------------

	/// Compression ratio at the transmitter, as N:1. 1.0 is a straight wire.
	/// 2.0 is what almost every analogue wireless system used.
	float compressRatio = 2.0f;

	/// Pre-emphasis, in dB of lift on the detail band. The network a link
	/// applies before the compressor so that quiet high-frequency content sits
	/// above the noise floor rather than under it.
	float emphasisDb = 6.0f;

	//-- Link -------------------------------------------------------------

	/// Gain applied to the encoded signal on its way into the link, in dB. Zero
	/// is a correctly aligned pair.
	///
	/// Applied BEFORE the noise is added, which is the whole point: the noise
	/// floor stays where it is while the signal moves relative to it, so this
	/// is a signal-to-noise control before it is a level control, and the
	/// expander -- which is referred to the nominal pivot and knows nothing
	/// about any of it -- restores signal and noise by different amounts.
	float linkLevelDb = 0.0f;

	/// The link's noise floor, as a linear amplitude on the encoded signal.
	/// Added AFTER compression and BEFORE expansion, which is the only place it
	/// can go and the only reason companding was worth doing.
	float noise = 0.02f;

	/// Where the link clips, as a linear level on the encoded signal. Below
	/// 1.0 the compressor's output starts running into the ceiling it was
	/// installed to keep away from.
	float headroom = 1.4f;

	//-- Decode -----------------------------------------------------------

	/// Expansion ratio at the receiver, as 1:N. Complementary to
	/// `compressRatio` when the two match; a plugin whose two ends must always
	/// match would be a plugin with one control and no character.
	float expandRatio = 2.0f;

	/// De-emphasis error, as a fraction. 0 is a de-emphasis network matching
	/// the emphasis exactly. Positive leaves the top end lifted and the picture
	/// glassy; negative takes off more than was put on and it goes soft and
	/// smeared. Applied as a see-saw about the mid band so overall level does
	/// not move with it.
	float tilt = 0.0f;

	//-- Detector ---------------------------------------------------------

	/// Attack and release time constant, in microseconds of signal time. See
	/// the table in the header comment for what this looks like as a picture.
	float timeConstantUs = 5.0f;

	/// The level the compression law pivots about, as a linear signal level.
	/// Signal at the pivot passes at unity gain through both ends regardless of
	/// ratio, so this is where the round trip is exact and everything either
	/// side of it is where the character is.
	float pivot = 0.5f;

	/// The most boost the compressor will ever apply, in dB, and the most cut
	/// the expander will ever apply.
	///
	/// ⚠️ **Not a knee.** A true broadband compander's law is a straight line in
	/// dB through the pivot -- same slope above and below -- so there is no
	/// corner anywhere on it to round off. What a real one does have is a bound:
	/// the law says a signal 60 dB down gets 30 dB of boost, and a circuit that
	/// obliged would be amplifying nothing but its own noise floor. So the boost
	/// runs out, and where it runs out is a real corner with a real soft knee on
	/// it (`kKneeDb` wide, fixed).
	///
	/// This is the control that decides how far into the shadows the effect
	/// reaches, and it is why the dark parts of a picture stop lifting rather
	/// than lifting forever.
	float maxGainDb = 24.0f;

	//-- Signal -----------------------------------------------------------

	int sidechain = kSideSignal;

	/// How much of the companding reaches the colour difference signals.
	///
	/// 0 is the radio mic reading: one channel, luma, and the colour follows it
	/// so hue and saturation are untouched. 1 is the video link reading, where
	/// the whole composite signal including the chroma subcarrier went through
	/// the same emphasis network -- which is exactly why those links were known
	/// for chroma noise and for saturation moving with picture level.
	float chroma = 0.0f;

	/// Wet/dry.
	float mix = 1.0f;

	//-- Audio ------------------------------------------------------------

	/// How hard the audio envelope drives the sidechain, 0..1. Inert unless
	/// `sidechain` is not `kSideSignal`.
	float audioAmount = 0.7f;

	/// Which part of the spectrum drives it: 0 is the bottom of the band, 1 the
	/// top. The window is broad, so this is a lean rather than a filter.
	float audioBand = 0.2f;

	/// How much the spectrum's balance drives `tilt` on top of its own setting.
	/// A bright mix tilts the de-emphasis one way and a dull one the other,
	/// which is the closest thing to a compander mistracking with the music.
	float audioTilt = 0.0f;
};

/// Signal level below which the detector stops caring. Not an epsilon to avoid
/// a divide -- it is the link's own floor, and a real detector sitting on a
/// silent input sees noise, not zero.
constexpr float kDetectorFloor = 1.0e-3f;

/// Width of the soft knee at the maximum-gain corner, in dB. Fixed rather than
/// exposed: it is a property of the detector's rectifier, not something an
/// operator has an opinion about, and a compander with an adjustable knee on
/// its gain ceiling is not a thing that was ever built.
constexpr float kKneeDb = 6.0f;

/// The active picture as a fraction of the line, 625/50: 52 us of 64.
constexpr float kActiveLineUs = 52.0f;

/// The most doubling passes the scan detector will run. See `Detector.h` for
/// why this is a cap rather than a range, and what covers the range above it.
constexpr int kMaxScanPasses = 14;

/// Samples of scan per microsecond of signal time, for a picture this wide.
float samplesPerMicrosecond( int width );

/// A time constant in microseconds, as a distance in samples along the scan.
float timeConstantSamples( float us, int width );

/// How many recursive-doubling passes the scan detector needs to cover a time
/// constant of `tauSamples`, clamped to `kMaxScanPasses`.
///
/// Four time constants of history, because the peak detector's contribution has
/// decayed to below two percent by then and the passes are octaves -- one fewer
/// halves the history, which is visible, and one more costs a pass to change
/// nothing.
int scanPasses( float tauSamples );

/// How much of the envelope comes from the frame-global detector rather than
/// the scan detector, 0..1, for a time constant of `tauSamples`.
///
/// Zero everywhere the scan detector can reach on its own. Crossing to one over
/// the last octave below the cap, so the join is a fade rather than a step.
float frameBlend( float tauSamples );

/// The per-sample release coefficient for a time constant, `exp(-1/tau)`.
float releaseCoefficient( float tauSamples );

/// Compressor gain for an envelope level.
///
/// The law, in dB about the pivot: a signal `d` dB from the pivot leaves at
/// `d / ratio` dB from it, so the gain applied is `d * (1/ratio - 1)`. Below
/// the pivot that is a boost, above it a cut, and at the pivot it is nothing --
/// which is what makes the pivot the one level the round trip is exact at.
///
/// The law is bounded at `maxGainDb`, with a quadratic soft knee `kKneeDb`
/// wide at that corner -- which is the only corner the law has.
float compressGain( float level, float pivot, float ratio, float maxGainDb );

/// Expander gain for an envelope level: the same law with the ratio inverted,
/// about the same nominal pivot.
///
/// Deliberately symmetric with `compressGain` and deliberately given no error
/// term of its own. A real receiver's expander is not misaligned -- it is
/// correct, and it is being handed a signal that arrived at the wrong height.
/// Modelling it the other way round collapses to a level shift; see the header.
float expandGain( float level, float pivot, float ratio, float maxGainDb );

/// Soft ceiling for the link, a `tanh`-shaped knee reaching `limit`
/// asymptotically. Real headroom does not clip square and neither does this.
float linkCeiling( float x, float limit );

/// Weight for spectrum bin `i` of `bins`, for an `audioBand` setting.
///
/// A raised cosine two octaves wide, which is broad on purpose: this is a lean
/// towards the bottom or the top of the spectrum, not a filter, and a narrow
/// window on a 64-bin FFT tracks one drum rather than the music.
float audioBandWeight( int i, int bins, float band );

/// Sampling of the gain tables handed to the shader.
///
/// 128 points over 60 dB is a shade under half a dB a step, and the curves are
/// straight lines in dB with one soft corner, so a linear interpolation between
/// samples is exact everywhere except across the knee -- where it is inside a
/// thousandth of the value. Far below anything a picture can show.
constexpr int kGainTableSize = 128;

/// The bottom of the tables' range, in dB. `kDetectorFloor` is -60 dB and the
/// tables span up to 0 dB.
constexpr float kGainTableFloorDb = -60.0f;

/// Where a level lands in a gain table, as a 0..1 coordinate. Log-spaced,
/// because the laws are straight lines in dB and a linear table would spend
/// most of its points where nothing happens.
float gainTableCoord( float level );

/// Fill both gain tables: `kGainTableSize` compress gains, then the same number
/// of expand gains, level running over `kGainTableFloorDb`..0 dB.
///
/// THE CURVES EXIST ONCE, HERE, and are deliberately NOT mirrored in GLSL. A
/// preset must not be able to mean two different things, and a law written out
/// twice is a law that will drift. Only the per-pixel arithmetic in `Chain.h`
/// is mirrored, because that is per-pixel and has no choice.
void fillGainTables( const Settings& s, float* compressTable, float* expandTable );

int sidechainCount();
const char* sidechainLabel( int mode );

} // namespace compander
