#include "Chain.h"

#include <algorithm>
#include <cmath>

namespace compander
{
namespace
{
inline float dbToLinear( float db )
{
	return std::pow( 10.0f, db / 20.0f );
}
} // namespace

Crossover crossoverFor( int width )
{
	Crossover c;

	const float w = static_cast< float >( std::max( width, 8 ) );

	// Floored at one sample and one and a half: a blur narrower than a sample
	// is not a blur, and the two corners have to stay apart or the mid band
	// collapses to nothing and Tilt loses the thing it acts on.
	c.narrow = std::max( 1.0f, w / 512.0f );
	c.wide   = std::max( c.narrow * 1.5f, w / 64.0f );

	return c;
}

float preEmphasis( const Bands& b, float emphasisDb )
{
	//= mirrored
	const float lift = dbToLinear( emphasisDb );
	return b.lo + b.mid + b.hi * lift;
}

float deEmphasis( const Bands& b, float emphasisDb, float tilt )
{
	//= mirrored
	// Tilt IS the mismatch, expressed as a fraction of the emphasis network.
	// The receiver's de-emphasis is `1 - tilt` as deep as the transmitter's
	// pre-emphasis was, so:
	//
	//     tilt  0  -- matched. The two networks cancel exactly and the round
	//                 trip is transparent in frequency, which is what a working
	//                 pair does.
	//     tilt +1  -- no de-emphasis at all. Everything the transmitter lifted
	//                 stays lifted: hard, glassy, edges ringing. This is the
	//                 encode-only character, and it is only reachable because
	//                 the control is defined this way.
	//     tilt -1  -- de-emphasised twice. A net cut of exactly one emphasis
	//                 network with nothing having lifted it: soft, smeared,
	//                 detail sucked out. The decode-only character.
	//
	// ⚠️ An earlier version made this a see-saw between the mid and high bands
	// with the de-emphasis always exactly cancelling. It could not reach either
	// end -- the two characters the header promises were simply not in the
	// plugin -- and the control was a mild detail balance instead.
	const float t         = std::clamp( tilt, -1.0f, 1.0f );
	const float deEmphDb  = emphasisDb * ( 1.0f - t );

	// The LOW band is never touched, by either network. An emphasis network is
	// a shelf and has unity gain at DC, so there is nothing down there to get
	// wrong -- and that is what keeps this a tilt rather than a brightness
	// control. On a flat field `mid` and `hi` are both zero and this provably
	// returns the input; `cmtest --flat` holds it.
	return b.lo + b.mid + b.hi / dbToLinear( deEmphDb );
}

float link( float encoded, float linkGainLinear, float headroom, float noise, float noiseSample )
{
	//= mirrored
	const float levelled = encoded * linkGainLinear;
	const float ceiled   = linkCeiling( levelled, headroom );

	return ceiled + noise * noiseSample;
}

float sidechainLevel( int mode, float pictureEnv, float audioEnv, float audioAmount )
{
	//= mirrored
	const float amount = std::clamp( audioAmount, 0.0f, 1.0f );

	// The audio envelope is faded towards unity rather than towards zero. Fading
	// it to zero would mean turning Audio Amount down drove the compressor into
	// its maximum boost and whited out the picture, which is a control that
	// destroys the shot on its way to doing nothing.
	const float audio = 1.0f + amount * ( audioEnv - 1.0f );

	switch( mode )
	{
		case kSideAudio: return audio;
		case kSideBoth:  return pictureEnv * audio;
		default:         return pictureEnv;
	}
}

void applyGain( const float rgbIn[ 3 ], float lumaIn, float lumaOut, float chroma, float rgbOut[ 3 ] )
{
	//= mirrored
	// The gain the chain applied to the luma, recovered as a ratio. Floored so a
	// black pixel does not divide by nothing; at that level the difference
	// between the two routes below is invisible anyway.
	const float base = std::max( lumaIn, kDetectorFloor );
	const float gain = lumaOut / base;

	const float c = std::clamp( chroma, 0.0f, 1.0f );

	for( int i = 0; i < 3; ++i )
	{
		// Chroma bypassed the link. The colour difference signal arrives with
		// the amplitude it left at, and is simply re-added to whatever the luma
		// channel came back as -- so when the expander lifts a dark area, the
		// colour in it does NOT lift with it and the area desaturates. That is
		// what one companded channel and one untouched one looks like.
		const float bypassed = lumaOut + ( rgbIn[ i ] - lumaIn );

		// Chroma went through with it. The whole colour is scaled by the gain
		// the luma got, so the ratio of colour to luma is preserved and
		// saturation holds while brightness moves.
		const float companded = rgbIn[ i ] * gain;

		rgbOut[ i ] = bypassed + c * ( companded - bypassed );
	}
}

float luma( const float rgb[ 3 ] )
{
	//= mirrored
	return 0.2126f * rgb[ 0 ] + 0.7152f * rgb[ 1 ] + 0.0722f * rgb[ 2 ];
}

} // namespace compander
