#include "../Shaders.h"

namespace compander::shaders
{
/**
    The per-pixel stage, on the GPU.

    ⚠️ **This is a MIRROR of `Chain.cpp`.** Every block below is marked
    `//= mirrored` and has a twin there marked the same way. Change one and
    `cmtest --chain` fails, which is the entire reason it exists.

    Written for agreement rather than for idiom:

    - **Constants are spelled the same way in both files.** The Rec.709
      coefficients and the -60 dB table floor are written out identically so
      both compilers round them to the same float.
    - **`dbToLinear` is `exp2( db * (1.0/6.020599913...) )`** on both sides,
      rather than `pow(10, db/20)` on one and something else on the other.
    - **No `pow`.** It is a library call on the CPU and an approximation on the
      GPU, and the two disagree in the last bits at exactly the levels the
      detector cares about.

    The gain laws are not here. They arrive as tables -- see `Shaders.h`.
*/
const char* const kCommon = R"(
//---------------------------------------------------------------------------
// The gain tables. Filled by fillGainTables() in Compander.cpp; the curves
// exist there and nowhere else. 128 points spanning -60..0 dB, log spaced.
//---------------------------------------------------------------------------
uniform float CompressTable[ 128 ];
uniform float ExpandTable[ 128 ];

const float kGainTableFloorDb = -60.0;
const float kDetectorFloor    = 0.001;

//= mirrored -- gainTableCoord
float tableCoord( float level )
{
	float db = 20.0 * log( max( level, kDetectorFloor ) ) / log( 10.0 );
	return clamp( ( db - kGainTableFloorDb ) / -kGainTableFloorDb, 0.0, 1.0 );
}

float sampleTable( float tbl[ 128 ], float level )
{
	float x  = tableCoord( level ) * 127.0;
	float i  = floor( x );
	float f  = x - i;
	int   i0 = int( i );
	int   i1 = min( i0 + 1, 127 );

	return mix( tbl[ i0 ], tbl[ i1 ], f );
}

float compressGain( float level ) { return sampleTable( CompressTable, level ); }
float expandGain( float level )   { return sampleTable( ExpandTable, level ); }

//= mirrored -- luma
// Rec.709, applied to the values the texture holds and NOT to a linearised
// copy of them. The signal a link carried was gamma-corrected already, so
// linearising first would correct for a transfer function the circuit never
// saw.
float lumaOf( vec3 rgb )
{
	return 0.2126 * rgb.r + 0.7152 * rgb.g + 0.0722 * rgb.b;
}

//= mirrored -- dbToLinear
float dbToLinear( float db )
{
	return exp2( db * 0.16609640474436812 );
}

//= mirrored -- preEmphasis
float preEmphasis( vec3 bands, float emphasisDb )
{
	return bands.x + bands.y + bands.z * dbToLinear( emphasisDb );
}

//= mirrored -- deEmphasis
// Tilt IS the mismatch: the receiver's network is (1 - tilt) as deep as the
// transmitter's. 0 cancels exactly, +1 leaves everything lifted, -1
// de-emphasises twice. The LOW band is untouched by either network, which is
// what keeps this a tilt and not a brightness control.
float deEmphasis( vec3 bands, float emphasisDb, float tilt )
{
	float t = clamp( tilt, -1.0, 1.0 );
	return bands.x + bands.y + bands.z / dbToLinear( emphasisDb * ( 1.0 - t ) );
}

//= mirrored -- linkCeiling
float linkCeiling( float x, float limit )
{
	float lim  = max( limit, 0.001 );
	float knee = 0.5 * lim;

	if( x <= knee )
		return x;

	return knee + ( lim - knee ) * tanh( ( x - knee ) / ( lim - knee ) );
}

//= mirrored -- link
// Level, then ceiling, then noise. The order is the model: the level error is
// a transmitter's gain being wrong and happens before the signal is on the
// air; the noise is the link's own floor and does not care what level the
// signal arrived at -- which is exactly why getting the level wrong makes the
// noise louder relative to the picture.
float linkStage( float encoded, float linkGain, float headroom, float noise, float noiseSample )
{
	return linkCeiling( encoded * linkGain, headroom ) + noise * noiseSample;
}

//= mirrored -- sidechainLevel
// The audio envelope is faded towards unity, never towards zero: fading to
// zero would mean turning Audio Amount down drove the compressor to maximum
// boost and whited out the picture.
float sidechainLevel( int mode, float pictureEnv, float audioEnv, float audioAmount )
{
	float audio = 1.0 + clamp( audioAmount, 0.0, 1.0 ) * ( audioEnv - 1.0 );

	if( mode == 1 ) return audio;
	if( mode == 2 ) return pictureEnv * audio;
	return pictureEnv;
}

//= mirrored -- applyGain
vec3 applyGain( vec3 rgbIn, float lumaIn, float lumaOut, float chroma )
{
	float gain = lumaOut / max( lumaIn, kDetectorFloor );

	// Chroma bypassed the link: the colour difference arrives at the amplitude
	// it left at, so lifting a shadow desaturates it.
	vec3 bypassed = vec3( lumaOut ) + ( rgbIn - vec3( lumaIn ) );

	// Chroma went through with it: the whole colour is scaled, so saturation
	// holds while brightness moves.
	vec3 companded = rgbIn * gain;

	return mix( bypassed, companded, clamp( chroma, 0.0, 1.0 ) );
}

//---------------------------------------------------------------------------
// The link's noise floor.
//
// An integer hash, never fract( sin(x) * 43758.5453 ). The noise here is part
// of the model -- it is the thing companding exists to hide, and the reason
// the decode stage has anything to show for itself -- so it has to be the same
// noise on every driver. PCG, then scaled to a signed value.
//---------------------------------------------------------------------------
uint pcgHash( uint v )
{
	uint state = v * 747796405u + 2891336453u;
	uint word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

float noiseAt( ivec2 p, int frame )
{
	uint h = pcgHash( uint( p.x ) + 1973u * uint( p.y ) + 9277u * uint( frame ) );
	return float( h ) * ( 2.0 / 4294967295.0 ) - 1.0;
}
)";
} // namespace compander::shaders
