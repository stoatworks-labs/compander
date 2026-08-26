#include "Detector.h"

#include <algorithm>
#include <cmath>

namespace compander
{
namespace
{
/// Fetch `level` at `d` samples back from linear index `n`, clamped at the
/// start of the picture. See the header for why this clamps and does not wrap.
inline float behind( const float* level, int n, int d )
{
	return level[ std::max( n - d, 0 ) ];
}
} // namespace

ScanPlan planScan( float tauFullResSamples, int fullWidth, int detectorWidth )
{
	ScanPlan plan;

	// The detector runs on a narrower picture, so a time constant measured in
	// full-resolution samples is that many fewer detector samples. Getting this
	// scaling backwards -- or forgetting it -- gives a detector four times too
	// fast, which looks like a plausible effect and is the wrong one.
	const float scale = detectorWidth > 0 && fullWidth > 0
	                        ? static_cast< float >( detectorWidth ) / static_cast< float >( fullWidth )
	                        : 1.0f;

	plan.tau        = std::max( 1.0f, tauFullResSamples * scale );
	plan.frameBlend = compander::frameBlend( tauFullResSamples );

	const float a = releaseCoefficient( plan.tau );
	const int   n = scanPasses( plan.tau );

	plan.passes.reserve( static_cast< size_t >( n ) );
	for( int k = 0; k < n; ++k )
	{
		ScanPass pass;
		pass.offset = 1 << k;

		// a^offset, computed as a power rather than by repeated multiplication:
		// at the long end `offset` is 8192 and squaring a float that many times
		// accumulates a visible error into the coefficient the whole pass
		// depends on.
		pass.decay = std::pow( a, static_cast< float >( pass.offset ) );

		plan.passes.push_back( pass );
	}

	return plan;
}

void serialEnvelope( const float* level, int w, int h, float tau, float* out )
{
	const float a     = releaseCoefficient( std::max( tau, 1.0f ) );
	const int   total = w * h;

	float e = 0.0f;
	for( int n = 0; n < total; ++n )
	{
		// Instant attack, exponential release. No branch on direction: taking
		// the maximum of the new sample and the decayed old one IS both.
		e      = std::max( level[ n ], e * a );
		out[ n ] = e;
	}
}

void doublingEnvelope( const float* level, int w, int h, const ScanPlan& plan, float* out )
{
	const int total = w * h;

	std::vector< float > src( level, level + total );
	std::vector< float > dst( static_cast< size_t >( total ) );

	for( const ScanPass& pass : plan.passes )
	{
		for( int n = 0; n < total; ++n )
			dst[ n ] = std::max( src[ n ], pass.decay * behind( src.data(), n, pass.offset ) );

		src.swap( dst );
	}

	std::copy( src.begin(), src.end(), out );
}

void reduceWidth( const float* src, int w, int h, float* dst )
{
	const int dw = std::max( 1, w / 4 );

	for( int y = 0; y < h; ++y )
	{
		const float* row = src + static_cast< size_t >( y ) * w;
		float*       out = dst + static_cast< size_t >( y ) * dw;

		for( int x = 0; x < dw; ++x )
		{
			// A MAX of the four, not a mean. This feeds a peak detector, and a
			// peak of averages is not an average of peaks -- averaging first
			// would round the top off every highlight before the detector ever
			// saw it, and the effect would lose its bite at exactly the places
			// it is supposed to have some.
			float m = 0.0f;
			for( int i = 0; i < 4; ++i )
			{
				const int sx = std::min( x * 4 + i, w - 1 );
				m            = std::max( m, row[ sx ] );
			}
			out[ x ] = m;
		}
	}
}

float FrameEnvelope::step( float level, float tauSeconds, double dtSeconds )
{
	if( envelope < 0.0f )
	{
		// First frame. Take the level rather than releasing towards it from
		// nothing, or the effect opens with a gain excursion that has nothing
		// to do with the footage.
		envelope = level;
		return envelope;
	}

	if( level >= envelope )
	{
		envelope = level;//instant attack
		return envelope;
	}

	// Release stepped by the frame's real duration, so the time constant is in
	// seconds and not in frames.
	const float tau = std::max( tauSeconds, 1.0e-4f );
	const float a   = std::exp( -static_cast< float >( std::max( dtSeconds, 0.0 ) ) / tau );

	envelope = level + ( envelope - level ) * a;
	return envelope;
}

float samplesToSeconds( float tauSamples, int width, int height )
{
	// A sample is one position along the scan; a frame is width*height of them
	// and lasts a frame time. Rather than asking the host for its frame rate --
	// which FFGL does not offer and which would be wrong on the first frame --
	// this uses the line standard the rest of the file is built on: 64 us a
	// line, so a picture `height` lines tall is `height * 64 us` of signal.
	const float linesPerSample = width > 0 ? 1.0f / static_cast< float >( width ) : 1.0f;
	const float lineSeconds    = 64.0e-6f;

	( void )height;

	return tauSamples * linesPerSample * lineSeconds;
}

} // namespace compander
