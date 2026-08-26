#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace compander
{
namespace controls
{
namespace
{
inline float clamp01( float v )
{
	return std::clamp( v, 0.0f, 1.0f );
}

/// Geometric interpolation: half a slider is the geometric middle of the range.
inline float expRange( float v, float lo, float hi )
{
	return lo * std::pow( hi / lo, clamp01( v ) );
}

inline float linRange( float v, float lo, float hi )
{
	return lo + ( hi - lo ) * clamp01( v );
}
} // namespace

float ratioFromParam( float v )
{
	return linRange( v, 1.0f, 4.0f );
}

float emphasisDbFromParam( float v )
{
	return linRange( v, 0.0f, 12.0f );
}

float linkLevelDbFromParam( float v )
{
	return linRange( v, -12.0f, 12.0f );
}

float noiseFromParam( float v )
{
	return linRange( v, 0.0f, 0.15f );
}

float headroomFromParam( float v )
{
	// Descending: see the header. 1.0 -- the point where the encoded signal
	// only just fits -- sits at about 0.58 of the travel.
	return expRange( v, 2.0f, 0.5f );
}

float tiltFromParam( float v )
{
	return linRange( v, -1.0f, 1.0f );
}

float timeConstantUsFromParam( float v )
{
	// Five and a half decades: a tenth of a microsecond is a handful of samples
	// and forty milliseconds is most of a frame. Everything in the table in
	// Compander.h lives on this one control.
	return expRange( v, 0.1f, 40000.0f );
}

float pivotFromParam( float v )
{
	return expRange( v, 0.02f, 1.0f );
}

float maxGainDbFromParam( float v )
{
	return linRange( v, 0.0f, 48.0f );
}

Settings toSettings( const HostValues& v )
{
	Settings s;

	s.sidechain = std::clamp( static_cast< int >( std::lround( v.sidechain ) ), 0, kSideCount - 1 );
	s.chroma    = clamp01( v.chroma );

	s.compressRatio = ratioFromParam( v.compress );
	s.emphasisDb    = emphasisDbFromParam( v.emphasis );

	s.linkLevelDb = linkLevelDbFromParam( v.linkLevel );
	s.noise       = noiseFromParam( v.noise );
	s.headroom    = headroomFromParam( v.headroom );

	s.expandRatio = ratioFromParam( v.expand );
	s.tilt        = tiltFromParam( v.tilt );

	s.timeConstantUs = timeConstantUsFromParam( v.timeConstant );
	s.pivot          = pivotFromParam( v.pivot );
	s.maxGainDb      = maxGainDbFromParam( v.maxGain );

	s.audioAmount = clamp01( v.audioAmount );
	s.audioBand   = clamp01( v.audioBand );
	s.audioTilt   = tiltFromParam( v.audioTilt );

	s.mix = clamp01( v.mix );

	return s;
}

} // namespace controls
} // namespace compander
