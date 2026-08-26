#pragma once

#include "Compander.h"

namespace compander
{
/**
    The per-pixel stage of the chain, written once in C++ and mirrored once in
    GLSL.

    Everything in this file is marked `//= mirrored` and has a line-for-line
    counterpart in `source/shaders/`. `cmtest --chain` renders the GPU's answer
    and compares it against this one across a sweep of levels and settings; if
    the two drift, that test fails rather than the picture quietly becoming
    something nobody chose. Change one, change both, run the test.

    ⚠️ **Only the per-pixel stage is mirrored.** The curves themselves --
    `compressGain`, `expandGain`, `linkCeiling` -- exist once, in Compander.cpp,
    and reach the shader as a sampled table. A preset must not be able to mean
    two different things, and a law written out twice is a law that will.

    ------------------------------------------------------------ the band split

    The emphasis networks need a detail band, and the tilt needs something to
    see-saw about, so the signal is split three ways by two blurs:

        lo  = blur( x, wide )
        mid = blur( x, narrow ) - lo
        hi  = x - blur( x, narrow )

    **Both blurs are horizontal.** A link carries one signal with one frequency
    axis, and on a picture that axis is the scan. A vertical edge is a high
    frequency to a transmitter; a horizontal edge is not a frequency at all, it
    is the next line. Splitting isotropically would make this a detail
    compressor that happens to be on a video plugin, and would throw away the
    give-away artifact -- vertical edges ringing and pumping while horizontal
    ones sit perfectly still.
*/

/// The three bands of a signal, summing back to it exactly.
struct Bands
{
	float lo  = 0.0f;
	float mid = 0.0f;
	float hi  = 0.0f;

	float sum() const { return lo + mid + hi; }
};

/// Crossover radii for the two band-splitting blurs, in samples along the scan.
///
/// ⚠️ **A function of the picture's width and NOT of the time constant.** An
/// earlier version tied these to the detector, on the reasoning that one box
/// was designed against one bandwidth. That is wrong about the circuit: a
/// pre-emphasis network is an RC in the transmitter with a corner set when the
/// thing was designed, and the compander's attack and release is a completely
/// separate time constant in a completely separate part of the circuit. They
/// are independent in every real system, and coupling them here also demanded a
/// blur hundreds of samples wide at long time constants, for no gain.
///
/// The corners are picked as a fraction of the active line, which is the only
/// scale-free way to say "about a megahertz" without knowing the sample rate:
/// `width / 512` is roughly a tenth of a microsecond and `width / 64` roughly
/// eight tenths, which brackets where video pre-emphasis networks actually sat.
struct Crossover
{
	float narrow = 4.0f;
	float wide   = 30.0f;
};

Crossover crossoverFor( int width );

//= mirrored --------------------------------------------------------------
/// Pre-emphasis: lift the detail band by `emphasisDb`, leaving everything below
/// the crossover where it was. The network a transmitter puts in front of its
/// compressor so quiet high-frequency content arrives above the link's noise
/// rather than under it.
float preEmphasis( const Bands& b, float emphasisDb );

//= mirrored --------------------------------------------------------------
/// De-emphasis: a network `1 - tilt` as deep as the pre-emphasis was.
///
/// **Tilt is the mismatch between the two ends, not a tone control bolted on
/// beside them.** At 0 the networks cancel exactly; at +1 there is no
/// de-emphasis and everything the transmitter lifted stays lifted; at -1 the
/// signal is de-emphasised twice and comes out net-cut with nothing having
/// lifted it. Those last two are the encode-only and decode-only characters,
/// and they are reachable only because the control is defined this way.
///
/// The low band is untouched by both networks -- an emphasis network is a shelf
/// and has unity gain at DC -- so on a flat field this provably returns its
/// input. `cmtest --flat` holds that, because getting it wrong is easy and
/// invisible to any test that only checks the control is not dead.
float deEmphasis( const Bands& b, float emphasisDb, float tilt );

//= mirrored --------------------------------------------------------------
/// The link: level error, then the ceiling, then the noise floor.
///
/// **This order is the whole model and it is not interchangeable.** The level
/// error goes first because it is a transmitter's gain being wrong, which
/// happens before the signal is on the air. The noise goes LAST because it is
/// the link's own floor and it does not care what level the signal arrived at
/// -- which is exactly why getting the level wrong makes the noise louder
/// relative to the picture, and why that is what breathes.
///
/// `noiseSample` is a signed value in -1..1 from the caller's own generator, so
/// this stays a pure function and the harness can drive it with a known
/// sequence.
float link( float encoded, float linkGainLinear, float headroom, float noise, float noiseSample );

//= mirrored --------------------------------------------------------------
/// Combine the picture's own envelope with the audio envelope according to the
/// sidechain mode. Both arrive as levels in the same 0..1 signal space.
float sidechainLevel( int mode, float pictureEnv, float audioEnv, float audioAmount );

//= mirrored --------------------------------------------------------------
/// Re-apply a luma gain to a colour, and take the colour difference signals
/// with it by `chroma`.
///
/// At `chroma` 0 the colour difference signal BYPASSED the link: it comes back
/// at the amplitude it left at and is re-added to whatever the luma channel
/// became, so lifting a shadow desaturates it. That is the radio mic reading --
/// one channel was companded and the colour was not.
///
/// At 1 the whole colour is scaled by the luma's gain, so the colour-to-luma
/// ratio holds and saturation stays put while brightness moves. That is the
/// video link reading, where the composite signal went through one emphasis
/// network together.
///
/// ⚠️ **Not modelled: chroma noise.** The link's noise enters the luma channel
/// and is scaled into the colour from there, so it arrives as luminance noise
/// tinted by the picture rather than as noise on the colour difference signals
/// in their own right. Real video links were notorious for the latter. Doing it
/// properly means carrying two colour difference channels through the whole
/// chain rather than one luma channel, which is three times the passes; the
/// judgement was that it is not worth it before anyone has asked. Say so rather
/// than letting the Chroma control imply it.
void applyGain( const float rgbIn[ 3 ], float lumaIn, float lumaOut, float chroma, float rgbOut[ 3 ] );

//= mirrored --------------------------------------------------------------
/// Rec.709 luma. The signal a link carried was gamma-corrected already, so this
/// is applied to the values the texture holds and NOT to a linearised copy of
/// them -- linearising first would be correcting for a transfer function that
/// the circuit being modelled never saw.
float luma( const float rgb[ 3 ] );

} // namespace compander
