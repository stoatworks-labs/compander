#include "Compander.h"

#include <algorithm>
#include <cmath>

namespace compander
{
namespace
{
/// dB from a linear level, floored at the detector's own noise floor rather
/// than at an epsilon. See `kDetectorFloor`.
inline float toDb( float level )
{
	return 20.0f * std::log10( std::max( level, kDetectorFloor ) );
}

/// Apply a soft ceiling of `limit` to a dB figure, with a quadratic knee
/// `kKneeDb` wide centred on the corner.
///
/// Continuous in value and in slope: at `limit - k/2` it is the identity, at
/// `limit + k/2` it has flattened to exactly `limit`, and the derivative meets
/// zero there rather than stepping to it.
inline float softCeiling( float x, float limit )
{
	const float half = 0.5f * kKneeDb;

	if( x <= limit - half )
		return x;
	if( x >= limit + half )
		return limit;

	const float over = x - ( limit - half );
	return x - over * over / ( 2.0f * kKneeDb );
}
} // namespace

float samplesPerMicrosecond( int width )
{
	return static_cast< float >( std::max( width, 1 ) ) / kActiveLineUs;
}

float timeConstantSamples( float us, int width )
{
	// A time constant shorter than a sample is not a time constant, it is an
	// identity. Floor at one so `releaseCoefficient` cannot return zero and
	// the detector cannot become a pass-through of the current sample.
	return std::max( 1.0f, us * samplesPerMicrosecond( width ) );
}

int scanPasses( float tauSamples )
{
	// Four time constants of history: the peak detector's contribution has
	// decayed to e^-4, under two percent, by then. The passes are octaves, so
	// this is not a tuning knob with a range -- one fewer halves the history
	// the detector can see, which is visible as a truncated smear, and one more
	// costs a full pass to change nothing.
	const float reach = 4.0f * std::max( tauSamples, 1.0f );
	const int   need  = static_cast< int >( std::ceil( std::log2( std::max( reach, 2.0f ) ) ) );

	return std::clamp( need, 1, kMaxScanPasses );
}

float frameBlend( float tauSamples )
{
	// The scan detector reaches 2^kMaxScanPasses samples back, and covers a
	// time constant a quarter of that. Past there it is running out of history
	// rather than being switched off, so the frame-global envelope fades in
	// across the octave below the limit instead of taking over at a step.
	const float reach   = static_cast< float >( 1 << kMaxScanPasses );
	const float tauFull = 0.25f * reach;
	const float tauFrom = 0.5f * tauFull;

	if( tauSamples <= tauFrom )
		return 0.0f;
	if( tauSamples >= tauFull )
		return 1.0f;

	const float t = ( tauSamples - tauFrom ) / ( tauFull - tauFrom );
	return t * t * ( 3.0f - 2.0f * t );
}

float releaseCoefficient( float tauSamples )
{
	return std::exp( -1.0f / std::max( tauSamples, 1.0f ) );
}

float compressGain( float level, float pivot, float ratio, float maxGainDb )
{
	// The law: a straight line in dB through the pivot. A signal `d` dB from
	// the pivot must leave `d / ratio` dB from it, so the gain applied is
	// `d * (1/ratio - 1)`. At the pivot that is zero regardless of ratio, which
	// is what makes the pivot the one level a round trip is exact at.
	const float d     = toDb( level ) - toDb( pivot );
	const float slope = 1.0f / std::max( ratio, 1.0f ) - 1.0f;
	const float gdb   = d * slope;

	// Below the pivot the law is a boost, and the boost is what runs out. Above
	// it the law is a cut and there is nothing to bound -- a compressor is not
	// obliged to stop attenuating.
	return std::pow( 10.0f, softCeiling( gdb, std::max( maxGainDb, 0.0f ) ) / 20.0f );
}

float expandGain( float level, float pivot, float ratio, float maxGainDb )
{
	// The complementary law, about the same nominal pivot. There is no error
	// term here on purpose: the receiver is not the thing that is wrong, the
	// level the signal arrived at is, and that is applied in the signal path
	// where a link's gain error actually sits.
	const float d     = toDb( level ) - toDb( pivot );
	const float slope = std::max( ratio, 1.0f ) - 1.0f;
	const float gdb   = d * slope;

	// Mirrored bound: the expander's cut runs out where the compressor's boost
	// did. Without this the two ends stop being complements at the bottom of
	// the range and a black frame expands to something below black.
	return std::pow( 10.0f, -softCeiling( -gdb, std::max( maxGainDb, 0.0f ) ) / 20.0f );
}

float linkCeiling( float x, float limit )
{
	const float lim = std::max( limit, 1.0e-3f );

	// Linear until half the ceiling, then tanh onto it. Real headroom does not
	// clip square, and a hard clip here would put a flat-topped edge on the
	// encoded signal that the expander then multiplies out into a hard-edged
	// blob rather than a compressed highlight.
	const float knee = 0.5f * lim;
	if( x <= knee )
		return x;

	return knee + ( lim - knee ) * std::tanh( ( x - knee ) / ( lim - knee ) );
}

float audioBandWeight( int i, int bins, float band )
{
	if( bins <= 1 )
		return 1.0f;

	// Spectrum bins are linear in frequency and music is not, so the window is
	// placed and measured in log-bin space. Two octaves wide: broad enough that
	// the control leans towards the bottom or the top of the mix rather than
	// picking out one drum, which is what a narrow window on 64 bins does.
	const float lmax   = std::log2( static_cast< float >( bins ) );
	const float centre = std::clamp( band, 0.0f, 1.0f ) * lmax;
	const float here   = std::log2( static_cast< float >( i ) + 1.0f );
	const float x      = here - centre;

	if( x <= -1.0f || x >= 1.0f )
		return 0.0f;

	return 0.5f * ( 1.0f + std::cos( 3.14159265358979f * x ) );
}

float gainTableCoord( float level )
{
	const float db = toDb( level );
	return std::clamp( ( db - kGainTableFloorDb ) / -kGainTableFloorDb, 0.0f, 1.0f );
}

void fillGainTables( const Settings& s, float* compressTable, float* expandTable )
{
	for( int i = 0; i < kGainTableSize; ++i )
	{
		const float t     = static_cast< float >( i ) / static_cast< float >( kGainTableSize - 1 );
		const float db    = kGainTableFloorDb + t * -kGainTableFloorDb;
		const float level = std::pow( 10.0f, db / 20.0f );

		compressTable[ i ] = compressGain( level, s.pivot, s.compressRatio, s.maxGainDb );
		expandTable[ i ]   = expandGain( level, s.pivot, s.expandRatio, s.maxGainDb );
	}
}

int sidechainCount()
{
	return kSideCount;
}

const char* sidechainLabel( int mode )
{
	switch( mode )
	{
		case kSideSignal: return "Picture";
		case kSideAudio:  return "Audio";
		case kSideBoth:   return "Picture x Audio";
		default:          return "Picture";
	}
}

} // namespace compander
