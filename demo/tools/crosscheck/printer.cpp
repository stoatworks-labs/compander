// The C++ half of the browser demo's PORT cross-check: the plugin's own
// GL-free model -- Controls.cpp, Compander.cpp, Detector.cpp and Chain.cpp's
// crossoverFor, plus Controls.h's HostValues and Presets.h -- driven through a
// fixed scenario and printed. run.mjs prints the same scenario from the PORT
// block of demo/plugin.js; crosscheck.sh compares the two.
//
//     printer > cpp.txt
//
// Floats print as their bits (0x%08x), integers as decimals. Nothing here
// computes anything the plugin does not: every number is the return value of
// one of the plugin's own functions.

#include "Chain.h"
#include "Compander.h"
#include "Controls.h"
#include "Detector.h"
#include "Presets.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using namespace compander;

namespace
{
void f( float v )
{
	uint32_t bits;
	std::memcpy( &bits, &v, sizeof bits );
	std::printf( " 0x%08x", bits );
}

void i( int v )
{
	std::printf( " %d", v );
}

/// The settings a preset row describes: HostValues' defaults, then the row
/// written over the eleven parameters it covers, in presets::Param order.
controls::HostValues fromPreset( const presets::Preset& p )
{
	controls::HostValues v;
	v.chroma       = p.v[ presets::kChroma ];
	v.compress     = p.v[ presets::kCompress ];
	v.emphasis     = p.v[ presets::kEmphasis ];
	v.linkLevel    = p.v[ presets::kLinkLevel ];
	v.noise        = p.v[ presets::kNoise ];
	v.headroom     = p.v[ presets::kHeadroom ];
	v.expand       = p.v[ presets::kExpand ];
	v.tilt         = p.v[ presets::kTilt ];
	v.timeConstant = p.v[ presets::kTimeConstant ];
	v.pivot        = p.v[ presets::kPivot ];
	v.maxGain      = p.v[ presets::kMaxGain ];
	return v;
}

void settings( const char* label, const controls::HostValues& v )
{
	const Settings s = controls::toSettings( v );
	std::printf( "settings %s", label );
	i( s.sidechain );
	f( s.chroma );
	f( s.compressRatio );
	f( s.emphasisDb );
	f( s.linkLevelDb );
	f( s.noise );
	f( s.headroom );
	f( s.expandRatio );
	f( s.tilt );
	f( s.timeConstantUs );
	f( s.pivot );
	f( s.maxGainDb );
	f( s.audioAmount );
	f( s.audioBand );
	f( s.audioTilt );
	f( s.mix );
	std::printf( "\n" );

	float compress[ kGainTableSize ];
	float expand[ kGainTableSize ];
	fillGainTables( s, compress, expand );
	std::printf( "tables %s", label );
	for( float g : compress )
		f( g );
	for( float g : expand )
		f( g );
	std::printf( "\n" );
}
} // namespace

int main()
{
	//-- the constructor's defaults
	const controls::HostValues d;
	std::printf( "default" );
	for( float v : { d.sidechain, d.chroma, d.compress, d.emphasis, d.linkLevel, d.noise, d.headroom,
	                 d.expand, d.tilt, d.timeConstant, d.pivot, d.maxGain, d.audioAmount, d.audioBand,
	                 d.audioTilt, d.mix } )
		f( v );
	std::printf( "\n" );

	//-- every slider position the page can produce, through every conversion
	for( int k = 0; k <= 1000; ++k )
	{
		const float v = static_cast< float >( k ) / 1000.0f;
		std::printf( "control %d", k );
		f( controls::ratioFromParam( v ) );
		f( controls::emphasisDbFromParam( v ) );
		f( controls::linkLevelDbFromParam( v ) );
		f( controls::noiseFromParam( v ) );
		f( controls::headroomFromParam( v ) );
		f( controls::tiltFromParam( v ) );
		f( controls::timeConstantUsFromParam( v ) );
		f( controls::pivotFromParam( v ) );
		f( controls::maxGainDbFromParam( v ) );
		std::printf( "\n" );
	}

	//-- the gain tables: the defaults, every preset, and a grid of the four
	//-- controls that shape them
	settings( "defaults", d );
	for( int p = 0; p < presets::kCount; ++p )
		settings( ( "preset" + std::to_string( p + 1 ) ).c_str(), fromPreset( presets::kPresets[ p ] ) );

	const float pivots[]   = { 0.0f, 0.25f, 0.5f, 0.7f, 0.823f, 1.0f };
	const float ratios[]   = { 0.0f, 0.333f, 0.667f, 1.0f };
	const float maxGains[] = { 0.0f, 0.25f, 0.5f, 1.0f };
	int         n          = 0;
	for( float pivot : pivots )
		for( float compress : ratios )
			for( float expand : ratios )
				for( float maxGain : maxGains )
				{
					controls::HostValues v;
					v.pivot    = pivot;
					v.compress = compress;
					v.expand   = expand;
					v.maxGain  = maxGain;
					settings( ( "grid" + std::to_string( n++ ) ).c_str(), v );
				}

	//-- the detector's plan, the frame follower's time constant and the band
	//-- split, across Time Constant at several widths
	for( int width : { 64, 333, 640, 960, 1280, 1920, 3840 } )
	{
		for( int k = 0; k <= 200; ++k )
		{
			const float v   = static_cast< float >( k ) / 200.0f;
			const float us  = controls::timeConstantUsFromParam( v );
			const float tau = timeConstantSamples( us, width );
			const int   det = width / 4 > 1 ? width / 4 : 1;

			const ScanPlan  plan  = planScan( tau, width, det );
			const Crossover cross = crossoverFor( width );

			std::printf( "plan %d %d", width, k );
			f( tau );
			f( plan.tau );
			f( plan.frameBlend );
			f( samplesToSeconds( tau, width, 0 ) );
			f( cross.narrow );
			f( cross.wide );
			f( releaseCoefficient( plan.tau ) );
			i( static_cast< int >( plan.passes.size() ) );
			for( const ScanPass& pass : plan.passes )
			{
				i( pass.offset );
				f( pass.decay );
			}
			std::printf( "\n" );
		}
	}

	return 0;
}
