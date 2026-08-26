#pragma once

#include "Compander.h"

#include <vector>

namespace compander
{
/**
    The envelope follower, and the one place the scan is treated as time.

    ------------------------------------------------------------------ the law

    Instant attack, exponential release -- a peak detector, which is what a
    cheap compander has and what every artifact anybody wants out of one comes
    from:

        E(n) = max( level(n), a * E(n-1) ),   a = exp( -1 / tau )

    written over `n`, the sample index along the SERIAL scan. Unrolled, that is

        E(n) = max over k >= 0 of ( level(n-k) * a^k )

    -- a maximum over exponentially decayed history. A maximum has no inverse
    and no closed form, but it is associative, and that is enough: a maximum
    over a window parallelises exactly by recursive doubling.

        E_0(n)     = level(n)
        E_{k+1}(n) = max( E_k(n), a^(2^k) * E_k(n - 2^k) )

    After K passes each output has seen 2^K samples of history. This is not an
    approximation of the recursion -- for a window of 2^K it is the same number,
    to the bit. The only thing truncated is history older than 2^K samples, and
    `scanPasses` picks K so that what is dropped has decayed below e^-4.

    ------------------------------------------------------ what makes it serial

    An offset of `d` samples back from `(x, y)` is

        n  = y * width + x - d
        py = n / width,  px = n % width

    so once `d` passes the left-hand edge it becomes the right-hand end of the
    line above, and once it passes a whole line it becomes a vertical offset.
    **The same doubling passes therefore cover the tight halo, the horizontal
    smear, the line-to-line streak and the broad vertical band, with no special
    case anywhere** -- because on a serial signal those are all the same thing
    seen at different distances.

    ⚠️ Clamped at the start of the picture, not wrapped to the end of it. There
    was no signal before the first sample of the frame. Wrapping would drag the
    bottom-right corner's level up into the top-left one, which is a visible
    artifact with no circuit behind it.

    ---------------------------------------------------- resolution and the cap

    **Quarter width, full height.** An envelope is a smoothed signal by
    definition, so the detector runs on a picture a quarter as wide, which costs
    a quarter as much and loses nothing above a time constant of a few samples.
    The reduction is a MAX rather than a mean -- this is a peak detector, and
    averaging four samples before taking a peak of them is measuring the wrong
    quantity.

    **The pass count is capped at `kMaxScanPasses`.** At quarter width that
    reaches 2^14 detector samples, which is about 34 lines of a 1920-wide frame.
    Past there the envelope is genuinely a frame-scale quantity, and it is
    served instead by `FrameEnvelope` -- a full reduction of the picture to one
    number, with a one-pole across frames. `frameBlend` crossfades the two over
    the octave below the cap.

    ⚠️ **This is the model's one real seam.** Between roughly 34 lines and a
    whole frame, the scan detector has run out of history and the frame envelope
    has no spatial structure, so a time constant in that range renders as a
    blend of a truncated streak and a global pump rather than as the very long
    streak it should be. It reads convincingly and it is cheap. It is not exact,
    and `cmtest --detector` reports the error against the serial reference
    rather than this file claiming there is none.
*/

/// One recursive-doubling pass: how far back it reaches, and what history at
/// that distance has decayed to.
struct ScanPass
{
	/// Offset in detector samples along the serial scan. Always a power of two.
	int offset = 1;

	/// `a^offset` -- what a sample `offset` back has decayed to by now.
	float decay = 0.0f;
};

/// The schedule of passes for one time constant.
struct ScanPlan
{
	/// Time constant in DETECTOR samples, which is quarter-width samples.
	float tau = 1.0f;

	std::vector< ScanPass > passes;

	/// How much of the final envelope comes from the frame-global follower.
	float frameBlend = 0.0f;
};

/// Build the pass schedule for a time constant given in FULL-RESOLUTION samples
/// along the scan. `detectorWidth` is the width the passes will actually run
/// at.
ScanPlan planScan( float tauFullResSamples, int fullWidth, int detectorWidth );

/// The serial peak detector, computed directly rather than by doubling.
///
/// **The ground truth.** O(n) and completely sequential, so it could never run
/// on a GPU, which is exactly why it is worth having: `cmtest --detector`
/// checks the doubling passes and then the shader against this, and a
/// disagreement means one of the two moved. Operates on `w * h` samples as one
/// signal, left to right and top to bottom.
void serialEnvelope( const float* level, int w, int h, float tau, float* out );

/// The doubling passes, run on the CPU exactly as the shader runs them.
///
/// Not used at render time -- the GPU does this - but it is the middle term of
/// the comparison above, and it is what tells a failure apart: if this agrees
/// with `serialEnvelope` and the shader does not, the shader is wrong; if this
/// disagrees too, the plan is.
void doublingEnvelope( const float* level, int w, int h, const ScanPlan& plan, float* out );

/// Peak-preserving reduction to quarter width. `dst` must hold `(w/4) * h`.
void reduceWidth( const float* src, int w, int h, float* dst );

/**
    The frame-global envelope: one number for the whole picture, followed across
    frames.

    Where the scan detector runs out. A time constant of tens of milliseconds is
    longer than a frame, so at that end the envelope genuinely is one value per
    frame and the only interesting question is how it moves between them.

    Instant attack and exponential release again, for the same reason and with
    the same law, but stepped once per frame with the frame's own duration --
    not once per sample. A host that drops frames therefore gets the right
    decay in seconds rather than the right decay per call, which is the
    difference between a pump that stays in time with the music and one that
    speeds up when the machine is busy.
*/
class FrameEnvelope
{
public:
	/// Advance by `dtSeconds` towards `level`, with a time constant of
	/// `tauSeconds`. Returns the new envelope.
	float step( float level, float tauSeconds, double dtSeconds );

	float value() const { return envelope; }

	/// Forget everything. For a resize, a new clip, or the first frame -- any
	/// point where the previous value describes a different picture.
	void reset() { envelope = -1.0f; }

private:
	/// Negative until the first frame, so the first level is taken rather than
	/// faded up to from black.
	float envelope = -1.0f;
};

/// A time constant in scan samples, as one in seconds. The bridge between the
/// scan detector's units and the frame follower's.
float samplesToSeconds( float tauSamples, int width, int height );

} // namespace compander
