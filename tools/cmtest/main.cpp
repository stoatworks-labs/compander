/**
    cmtest -- the offline harness.

    It drives **the real plugin class** through the real FFGL sequence in a
    headless core-profile context. Not a reimplementation of the chain and not a
    preview: the thing under test is `compander::Plugin`, compiled from the same
    objects that go into the bundle, and every number below comes out of a frame
    it actually rendered.

        --out PATH        render a frame
        --scene PATH      write the synthetic test scene
        --list            parameters, with their types and physical values
        --set "Name=v"    set any parameter by its host-facing name
        --flat            Tilt and Emphasis are inert on a flat field
        --roundtrip       the two ends cancel, and where they stop cancelling
        --detector        the doubling passes and the GPU against the serial law
        --transparent     a matched pair with no noise returns the picture
        --anisotropy      vertical edges move and horizontal ones do not
        --presets         every factory preset survives every host
        --bench           render cost

    ## What each test can and cannot catch

    `--roundtrip` and `--flat` are model tests and need no GL at all. They are
    the ones that caught the three modelling errors recorded in `docs/NOTES.md`:
    a mistracking control that was really a brightness control, a Chroma control
    whose two branches were algebraically identical, and a Tilt that could not
    reach either of the characters the design promised.

    `--detector` is the only thing standing between `Detector.cpp` and its GLSL
    twin. It checks the parallel doubling passes against the strictly serial
    peak detector -- which could never run on a GPU, which is exactly why it is
    worth having as the ground truth -- and then checks the shader against the
    same reference. **It carries its own control**: the same comparison against
    a deliberately wrong decay coefficient, which must FAIL. Rows of agreement
    are exactly when to ask whether a test can fail at all.

    `--transparent` is the invariant the whole chain exists to hold up: set both
    ends to the same ratio, take the noise off, and the picture must come back.
    Everything an operator wants out of this plugin is a departure from that, so
    if it stops being true the departures stop meaning anything.

    `--anisotropy` is the one that catches the effect quietly becoming a
    detail compressor. A link has one frequency axis and on a picture it is the
    scan, so a vertical edge is a high frequency and a horizontal one is not.
    An isotropic blur would pass every other test in this file.

    `--presets` drives three hosts -- one that honours value events, one that
    ignores them, one that honours but quantises -- because Resolume is the
    second kind and a copy-based preset apply cannot work on it. Against the
    pre-fix shape of that code this fails in precisely the "ignores" column.

    None of them catches a dead control. See `tools/sweep.py`.
*/

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <chrono>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "Chain.h"
#include "Compander.h"
#include "Controls.h"
#include "Detector.h"
#include "Plugin.h"
#include "Presets.h"

using namespace compander;

namespace
{
//---------------------------------------------------------------------------
// PNG. zlib ships with the OS, so this is fifty lines rather than a dependency.
//---------------------------------------------------------------------------
void putBigEndian( std::vector< unsigned char >& out, unsigned int value )
{
	out.push_back( static_cast< unsigned char >( ( value >> 24 ) & 0xff ) );
	out.push_back( static_cast< unsigned char >( ( value >> 16 ) & 0xff ) );
	out.push_back( static_cast< unsigned char >( ( value >> 8 ) & 0xff ) );
	out.push_back( static_cast< unsigned char >( value & 0xff ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type,
               const unsigned char* data, size_t length )
{
	putBigEndian( out, static_cast< unsigned int >( length ) );
	const size_t crcStart = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data, data + length );
	const unsigned long crc = crc32( 0, out.data() + crcStart,
	                                 static_cast< unsigned int >( out.size() - crcStart ) );
	putBigEndian( out, static_cast< unsigned int >( crc ) );
}

bool writePng( const std::string& path, int width, int height,
               const std::vector< unsigned char >& rgba )
{
	//Each scanline gets a filter byte. Filter 0 (none) throughout: this is a
	//test artefact, not a delivery format, and a filter would only make the
	//file smaller at the cost of being wrong in a way nothing here would catch.
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(),
	               static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

	std::vector< unsigned char > ihdr;
	putBigEndian( ihdr, static_cast< unsigned int >( width ) );
	putBigEndian( ihdr, static_cast< unsigned int >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );//deflate
	ihdr.push_back( 0 );//adaptive filtering
	ihdr.push_back( 0 );//no interlace
	putChunk( png, "IHDR", ihdr.data(), ihdr.size() );
	putChunk( png, "IDAT", compressed.data(), compressed.size() );
	putChunk( png, "IEND", nullptr, 0 );

	FILE* file = std::fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}


//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

struct Target
{
	GLuint texture = 0;
	GLuint fbo     = 0;
	int width      = 0;
	int height     = 0;
};

Target makeTarget( int width, int height )
{
	Target target;
	target.width  = width;
	target.height = height;

	glGenTextures( 1, &target.texture );
	glBindTexture( GL_TEXTURE_2D, target.texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glGenFramebuffers( 1, &target.fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0 );
	return target;
}

void releaseTarget( Target& target )
{
	if( target.fbo != 0 )
		glDeleteFramebuffers( 1, &target.fbo );
	if( target.texture != 0 )
		glDeleteTextures( 1, &target.texture );
	target = Target();
}

GLuint uploadTexture( const std::vector< unsigned char >& rgba, int width, int height )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

/// Straight out of GL, **bottom row first**. Every sampler below takes frame
/// coordinates with y down and flips here, in one place.
std::vector< unsigned char > readBytes( const Target& target )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( target.width ) * target.height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
	return pixels;
}

std::vector< unsigned char > flipRows( const std::vector< unsigned char >& image, int width, int height )
{
	std::vector< unsigned char > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

/// Read a float texture back whole. Used for the CoC and depth buffers, which
/// are RGBA16F -- glGetTexImage converts to float for us.
std::vector< float > readFloatTexture( GLuint texture, int width, int height )
{
	std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
	glBindTexture( GL_TEXTURE_2D, texture );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, pixels.data() );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return pixels;
}

/**
    Bilinear fetch from a float texture read, matching what GL does.

    Needed because `--focus` compares the CoC the GPU wrote against the C++, and
    in Image Depth mode the shader reads its two cues out of a quarter-
    resolution buffer through a **GL_LINEAR** sampler (FFGLFBO sets that on
    every colour texture it makes). Sampling that buffer with a nearest lookup
    on this side put a 0.4 disagreement into a comparison whose whole job is
    detecting disagreements of about 0.002 -- entirely from the interpolation,
    with nothing wrong in either copy of the arithmetic.

    The half-texel offsets are the part worth keeping: `texture()` puts texel
    centres at (i + 0.5) / size, so the sample position in texel units is
    uv * size - 0.5, and dropping that shifts everything by half a texel of the
    reduced buffer, which is two full-resolution pixels.
*/
//---------------------------------------------------------------------------
// The synthetic scene.
//
// Built to make COMPANDING measurable rather than to look nice:
//
// - a **flat mid-grey band** across the top, so the flat-field invariants have
//   somewhere to be checked in a rendered frame and not only in the model;
// - **bright bars with hard vertical edges** on a dark field, because the
//   causal detector's whole signature is the gain change trailing off the
//   RIGHT of one of these and not off its left;
// - **a hard horizontal edge**, which must come through untouched -- it is not
//   a frequency to a link, it is the next line;
// - **fine vertical detail down in the shadows**, which is what pre-emphasis
//   exists to lift above the noise floor and what expansion sucks back out;
// - **a smooth ramp**, so the gain law can be measured against level.
//---------------------------------------------------------------------------
struct Rgba
{
	unsigned char r = 0, g = 0, b = 0, a = 255;
};

// The scene's bands, as fractions of the height FROM THE TOP. Named because
// three tests index into them and a magic number in two places is a test that
// will one day measure the wrong stripe.
constexpr float kFlatTo           = 0.15f;
constexpr float kBarsTo           = 0.32f;
constexpr float kCombVerticalTo   = 0.49f;
constexpr float kCombHorizontalTo = 0.66f;
constexpr float kColourTo         = 0.83f;

// Both combs use the same two levels, so the along-scan and across-scan
// measurements are directly comparable and the ratio between them means
// something.
constexpr float kCombHigh = 0.10f;
constexpr float kCombLow  = 0.04f;

Rgba scenePixel( int x, int y, int width, int height )
{
	const float fx = static_cast< float >( x ) / static_cast< float >( width );
	const float fy = static_cast< float >( y ) / static_cast< float >( height );

	auto quantise = []( float v ) -> unsigned char {
		return static_cast< unsigned char >( std::clamp( v, 0.0f, 1.0f ) * 255.0f + 0.5f );
	};

	float level = 0.0f;

	if( fy < kFlatTo )
	{
		// The flat band. Exactly one value, no detail of any kind, so
		// transparency can be demanded EXACTLY somewhere.
		level = 0.45f;
	}
	else if( fy < kBarsTo )
	{
		// Bright bars on a dark field, with hard vertical edges. The causal
		// detector's whole signature is the gain change trailing off the RIGHT
		// of one of these and not off its left.
		const int bar = ( x / std::max( 1, width / 12 ) ) % 3;
		level         = bar == 0 ? 0.95f : 0.06f;
	}
	else if( fy < kCombVerticalTo )
	{
		// A one-pixel VERTICAL comb, down in the shadows: the highest frequency
		// the scene holds ALONG the scan.
		level = ( x % 2 == 0 ) ? kCombHigh : kCombLow;
	}
	else if( fy < kCombHorizontalTo )
	{
		// The same comb turned through ninety degrees: the same levels, the
		// same contrast, one pixel apart -- but ACROSS the scan.
		//
		// ⚠️ This band exists solely so `--anisotropy` has something to
		// measure. Without it the across-scan figure came out as exactly zero
		// on a scene whose only horizontal structure was band boundaries, the
		// ratio defaulted to 1.0, and the test passed by measuring nothing.
		level = ( y % 2 == 0 ) ? kCombHigh : kCombLow;
	}
	else if( fy < kColourTo )
	{
		// Mid grey, carrying the saturated patch below.
		level = 0.40f;
	}
	else
	{
		// The ramp, for reading the gain law against level.
		level = fx;
	}

	Rgba c;
	c.r = quantise( level );
	c.g = quantise( level );
	c.b = quantise( level );

	// A saturated patch, so Chroma has something to act on and desaturation is
	// visible rather than inferred.
	if( fy >= kCombHorizontalTo && fy < kColourTo && fx > 0.25f && fx < 0.75f )
	{
		c.r = quantise( level * 0.30f );
		c.g = quantise( level * 0.10f );
		c.b = quantise( level );
	}

	return c;
}

std::vector< unsigned char > makeScene( int width, int height )
{
	std::vector< unsigned char > out( static_cast< size_t >( width ) * height * 4 );

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			// Bottom-up, the way GL wants it.
			const Rgba c = scenePixel( x, height - 1 - y, width, height );
			unsigned char* p = &out[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			p[ 0 ] = c.r;
			p[ 1 ] = c.g;
			p[ 2 ] = c.b;
			p[ 3 ] = c.a;
		}
	}

	return out;
}

//---------------------------------------------------------------------------
// Driving the plugin.
//---------------------------------------------------------------------------
bool render( Plugin& plugin, const Target& target, GLuint input, int inputWidth, int inputHeight )
{
	FFGLViewportStruct viewport {};
	viewport.width  = static_cast< FFUInt32 >( target.width );
	viewport.height = static_cast< FFUInt32 >( target.height );

	FFGLTextureStruct inputStruct {};
	inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( inputWidth );
	inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( inputHeight );
	inputStruct.Handle                              = input;
	FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

	ProcessOpenGLStruct process {};
	process.numInputTextures = 1;
	process.inputTextures    = inputs;
	process.HostFBO          = target.fbo;

	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glViewport( 0, 0, target.width, target.height );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	// The plugin reads its size out of the viewport its base class holds, and
	// InitGL is what sets it. Calling it per frame is what lets one harness
	// process render at several sizes without tearing the GL resources down.
	plugin.InitGL( &viewport );
	return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
}

/// Every parameter's host-facing name, read out of the plugin itself.
///
/// Built at runtime rather than kept as a table beside Controls.h, and that is
/// not tidiness: a hand-written table is a second place for a name to live, and
/// the failure it produces is a `--set` that silently addresses nothing while
/// everything else about the run looks correct.
std::map< std::string, unsigned int > parameterIndex( Plugin& plugin )
{
	std::map< std::string, unsigned int > byName;
	for( unsigned int id = 0; id < Plugin::PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name != nullptr && name[ 0 ] != '\0' )
			byName[ name ] = id;
	}
	return byName;
}

struct Options
{
	std::vector< std::pair< std::string, float > > sets;
	int width  = 640;
	int height = 360;
};

void applySets( Plugin& plugin, const Options& options )
{
	const auto byName = parameterIndex( plugin );
	for( const auto& set : options.sets )
	{
		const auto found = byName.find( set.first );
		if( found == byName.end() )
		{
			std::fprintf( stderr, "no parameter named \"%s\"\n", set.first.c_str() );
			continue;
		}
		plugin.SetFloatParameter( found->second, set.second );
	}
}

/// Set every control to a known, neutral starting point: matched ends, no
/// noise, no tilt. The state `--transparent` demands the picture survive.
void setNeutral( Plugin& plugin )
{
	plugin.SetFloatParameter( Plugin::PT_SIDECHAIN, 0.0f );
	plugin.SetFloatParameter( Plugin::PT_CHROMA, 0.0f );
	plugin.SetFloatParameter( Plugin::PT_COMPRESS, 0.333f );
	plugin.SetFloatParameter( Plugin::PT_EMPHASIS, 0.5f );
	plugin.SetFloatParameter( Plugin::PT_LINK_LEVEL, 0.5f );
	plugin.SetFloatParameter( Plugin::PT_NOISE, 0.0f );
	plugin.SetFloatParameter( Plugin::PT_HEADROOM, 0.0f );
	plugin.SetFloatParameter( Plugin::PT_EXPAND, 0.333f );
	plugin.SetFloatParameter( Plugin::PT_TILT, 0.5f );
	plugin.SetFloatParameter( Plugin::PT_TIME_CONSTANT, 0.35f );
	plugin.SetFloatParameter( Plugin::PT_PIVOT, 0.823f );
	plugin.SetFloatParameter( Plugin::PT_MAX_GAIN, 0.5f );
	plugin.SetFloatParameter( Plugin::PT_MIX, 1.0f );
}

float lumaOf( const unsigned char* p )
{
	const float rgb[ 3 ] = { p[ 0 ] / 255.0f, p[ 1 ] / 255.0f, p[ 2 ] / 255.0f };
	return compander::luma( rgb );
}

/// Mean absolute difference between neighbouring pixels along one axis, over a
/// band of rows. The measure `--anisotropy` is built on: it says how much
/// detail there is in a direction, so a link that only works along the scan
/// shows up as one number moving and the other not.
double detailAlong( const std::vector< unsigned char >& bottomUp, int width, int height,
                    float fromY, float toY, bool horizontal )
{
	const int y0 = std::clamp( int( fromY * height ), 1, height - 2 );
	const int y1 = std::clamp( int( toY * height ), y0 + 1, height - 1 );

	double sum   = 0.0;
	long   count = 0;

	for( int y = y0; y < y1; ++y )
	{
		for( int x = 1; x < width - 1; ++x )
		{
			// Bottom-up storage: row `y` from the top is `height-1-y`.
			const int ry = height - 1 - y;
			const unsigned char* here = &bottomUp[ ( size_t( ry ) * width + x ) * 4 ];

			const unsigned char* other = horizontal
			                                 ? &bottomUp[ ( size_t( ry ) * width + x - 1 ) * 4 ]
			                                 : &bottomUp[ ( size_t( ry + 1 ) * width + x ) * 4 ];

			sum += std::fabs( double( lumaOf( here ) ) - double( lumaOf( other ) ) );
			++count;
		}
	}

	return count > 0 ? sum / double( count ) : 0.0;
}

//---------------------------------------------------------------------------
// --list
//---------------------------------------------------------------------------
int listParameters()
{
	Plugin plugin;

	std::printf( "%-4s %-16s %-10s %-9s %s\n", "id", "name", "type", "value", "physical" );

	for( unsigned int id = 0; id < Plugin::PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name == nullptr || name[ 0 ] == '\0' )
			continue;

		const unsigned int type = plugin.GetParamType( id );
		const char* typeName    = "standard";
		if( type == FF_TYPE_BOOLEAN )
			typeName = "boolean";
		else if( type == FF_TYPE_TEXT )
			typeName = "text";
		else if( type == FF_TYPE_EVENT )
			typeName = "event";
		else if( type == FF_TYPE_OPTION )
			typeName = "option";
		else if( type == FF_TYPE_BUFFER )
			typeName = "buffer";

		const float v = plugin.GetFloatParameter( id );

		// The physical value beside the slider position, because a 0..1 control
		// list tells you nothing about what the plugin will actually do.
		char physical[ 64 ] = "";
		switch( id )
		{
			case Plugin::PT_COMPRESS:
				std::snprintf( physical, sizeof physical, "%.2f:1", controls::ratioFromParam( v ) );
				break;
			case Plugin::PT_EXPAND:
				std::snprintf( physical, sizeof physical, "1:%.2f", controls::ratioFromParam( v ) );
				break;
			case Plugin::PT_EMPHASIS:
				std::snprintf( physical, sizeof physical, "%.1f dB", controls::emphasisDbFromParam( v ) );
				break;
			case Plugin::PT_LINK_LEVEL:
				std::snprintf( physical, sizeof physical, "%+.1f dB", controls::linkLevelDbFromParam( v ) );
				break;
			case Plugin::PT_NOISE:
				std::snprintf( physical, sizeof physical, "%.4f", controls::noiseFromParam( v ) );
				break;
			case Plugin::PT_HEADROOM:
				std::snprintf( physical, sizeof physical, "%.2f", controls::headroomFromParam( v ) );
				break;
			case Plugin::PT_TILT:
				std::snprintf( physical, sizeof physical, "%+.2f", controls::tiltFromParam( v ) );
				break;
			case Plugin::PT_TIME_CONSTANT:
				std::snprintf( physical, sizeof physical, "%.2f us",
				               controls::timeConstantUsFromParam( v ) );
				break;
			case Plugin::PT_PIVOT:
				std::snprintf( physical, sizeof physical, "%.3f", controls::pivotFromParam( v ) );
				break;
			case Plugin::PT_MAX_GAIN:
				std::snprintf( physical, sizeof physical, "%.1f dB", controls::maxGainDbFromParam( v ) );
				break;
			default:
				break;
		}

		std::printf( "%-4u %-16s %-10s %-9.4f %s\n", id, name, typeName, v, physical );

		// The 16-character limit is a HOST limit the SDK hides completely: the
		// plugin stores the full name, hands the host a pointer to all of it,
		// and Resolume copies sixteen bytes. Six plugins in this fleet shipped
		// a control labelled "Background Opaci" before anything noticed.
		if( std::strlen( name ) > 16 )
			std::printf( "     ^^ WARNING: %zu characters, Resolume will show \"%.16s\"\n",
			             std::strlen( name ), name );
	}

	return 0;
}

//---------------------------------------------------------------------------
// --flat : Tilt and Emphasis are inert on a flat field
//---------------------------------------------------------------------------
int checkFlat()
{
	std::printf( "Emphasis networks have unity gain at DC, so on a flat field\n"
	             "Tilt and Emphasis must do NOTHING AT ALL.\n\n" );

	const float level = 0.42f;
	const Bands flat { level, 0.0f, 0.0f };

	int    failures = 0;
	double worst    = 0.0;

	std::printf( "%-10s %-10s %-14s %s\n", "emphasis", "tilt", "out", "error" );

	for( float emphasisDb : { 0.0f, 3.0f, 6.0f, 12.0f } )
	{
		for( float tilt : { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f } )
		{
			const float out = deEmphasis( flat, emphasisDb, tilt );
			const double err = std::fabs( double( out ) - double( level ) );

			worst = std::max( worst, err );
			if( err > 1.0e-6 )
				++failures;

			std::printf( "%-10.1f %-10.2f %-14.8f %.2e%s\n", emphasisDb, tilt, out, err,
			             err > 1.0e-6 ? "   FAIL" : "" );
		}
	}

	// The control: on a field that is NOT flat, tilt had better do something,
	// or this test is passing because the function is a no-op.
	const Bands detailed { 0.40f, 0.10f, 0.05f };
	const float lo   = deEmphasis( detailed, 9.0f, -1.0f );
	const float hi   = deEmphasis( detailed, 9.0f, 1.0f );
	const double span = std::fabs( double( hi ) - double( lo ) );

	std::printf( "\ncontrol -- on a DETAILED signal tilt must move the answer:\n"
	             "  tilt -1 %.6f   tilt +1 %.6f   span %.6f%s\n",
	             lo, hi, span, span < 1.0e-3 ? "   FAIL (tilt is dead)" : "" );

	if( span < 1.0e-3 )
		++failures;

	std::printf( "\nworst flat-field error %.2e over %d combinations\n", worst, 20 );
	std::printf( failures == 0 ? "PASS\n" : "FAIL: %d\n", failures );

	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --roundtrip : the two ends cancel, and where they stop
//---------------------------------------------------------------------------
int checkRoundtrip()
{
	std::printf( "A matched pair must cancel exactly wherever the gain law is\n"
	             "unbounded, and must STOP cancelling where the boost runs out.\n\n" );

	const float pivot = 0.5f, ratio = 2.0f, maxGain = 24.0f;

	int    failures = 0;
	double worst    = 0.0;
	int    bounded  = 0;

	std::printf( "%-10s %-10s %-10s %-12s %s\n", "level", "encoded", "decoded", "err (dB)", "note" );

	for( float level : { 0.9f, 0.7f, 0.5f, 0.3f, 0.15f, 0.05f, 0.02f, 0.008f, 0.003f, 0.001f } )
	{
		const float gc  = compressGain( level, pivot, ratio, maxGain );
		const float enc = level * gc;
		const float ge  = expandGain( enc, pivot, ratio, maxGain );
		const float dec = enc * ge;

		const double err = 20.0 * std::log10( std::max( double( dec ), 1e-12 ) /
		                                      std::max( double( level ), 1e-12 ) );

		const bool atBound = 20.0f * std::log10( gc ) > maxGain - 3.1f;
		if( atBound )
			++bounded;
		else
		{
			worst = std::max( worst, std::fabs( err ) );
			if( std::fabs( err ) > 0.01 )
				++failures;
		}

		std::printf( "%-10.4f %-10.4f %-10.4f %-12.5f %s\n", level, enc, dec, err,
		             atBound ? "gain bounded" : "" );
	}

	std::printf( "\nworst error where the law is unbounded: %.6f dB\n", worst );

	if( bounded == 0 )
	{
		std::printf( "FAIL: nothing reached the gain bound, so the bounded case is untested\n" );
		++failures;
	}

	// Ratio mismatch MUST be level dependent. This is the check that caught the
	// original Link Level design: an offset of the expander's reference is
	// scale-free and comes out as a flat shift at every brightness, which is a
	// brightness knob with a misleading name.
	std::printf( "\nmismatched ratios must give a LEVEL-DEPENDENT error:\n" );
	std::printf( "%-10s %s\n", "level", "err (dB)" );

	double minErr = 1e9, maxErr = -1e9;
	for( float level : { 0.8f, 0.4f, 0.2f, 0.1f, 0.05f } )
	{
		const float enc = level * compressGain( level, pivot, 2.0f, maxGain );
		const float dec = enc * expandGain( enc, pivot, 3.0f, maxGain );
		const double err = 20.0 * std::log10( std::max( double( dec ), 1e-12 ) / double( level ) );

		minErr = std::min( minErr, err );
		maxErr = std::max( maxErr, err );

		std::printf( "%-10.4f %.3f\n", level, err );
	}

	const double spread = maxErr - minErr;
	std::printf( "spread across level: %.3f dB%s\n", spread,
	             spread < 1.0 ? "   FAIL (not level dependent)" : "" );
	if( spread < 1.0 )
		++failures;

	std::printf( failures == 0 ? "\nPASS\n" : "\nFAIL: %d\n", failures );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --detector : the doubling passes, and the shader, against the serial law
//---------------------------------------------------------------------------

/// The strictly serial peak detector over a whole picture, as the ground truth.
int checkDetector()
{
	std::printf( "The parallel doubling passes must equal the strictly serial\n"
	             "peak detector, which is the law in Detector.h.\n\n" );

	const int W = 480, H = 64;

	std::vector< float > level( size_t( W ) * H );
	std::mt19937 rng( 7 );
	std::uniform_real_distribution< float > u( 0.0f, 1.0f );
	for( auto& v : level )
		v = ( u( rng ) < 0.02f ) ? u( rng ) : 0.05f * u( rng );

	std::vector< float > serial( level.size() ), doubled( level.size() );

	int failures = 0;

	std::printf( "%-12s %-8s %-10s %-12s %s\n", "tau", "passes", "reach", "max err", "" );

	for( float tau : { 1.5f, 8.0f, 60.0f, 400.0f, 3000.0f } )
	{
		ScanPlan plan;
		plan.tau         = tau;
		const int    n   = scanPasses( tau );
		const float  a   = releaseCoefficient( tau );
		for( int k = 0; k < n; ++k )
			plan.passes.push_back( { 1 << k, std::pow( a, float( 1 << k ) ) } );

		serialEnvelope( level.data(), W, H, tau, serial.data() );
		doublingEnvelope( level.data(), W, H, plan, doubled.data() );

		double mx = 0.0;
		for( size_t i = 0; i < level.size(); ++i )
			mx = std::max( mx, std::fabs( double( serial[ i ] ) - double( doubled[ i ] ) ) );

		// The only error is history older than the window, which scanPasses
		// picks so it has decayed below e^-4.
		const bool bad = mx > 2.0e-3;
		if( bad )
			++failures;

		std::printf( "%-12.1f %-8d %-10d %-12.3e %s\n", tau, n, 1 << n, mx, bad ? "FAIL" : "" );
	}

	// The control. Run the same comparison with a deliberately wrong decay --
	// the coefficient for half the time constant -- and it must FAIL. A row of
	// agreements is exactly when to ask whether the test can fail at all.
	{
		const float tau = 60.0f;
		ScanPlan wrong;
		wrong.tau       = tau;
		const int   n   = scanPasses( tau );
		const float a   = releaseCoefficient( tau * 0.5f );
		for( int k = 0; k < n; ++k )
			wrong.passes.push_back( { 1 << k, std::pow( a, float( 1 << k ) ) } );

		serialEnvelope( level.data(), W, H, tau, serial.data() );
		doublingEnvelope( level.data(), W, H, wrong, doubled.data() );

		double mx = 0.0;
		for( size_t i = 0; i < level.size(); ++i )
			mx = std::max( mx, std::fabs( double( serial[ i ] ) - double( doubled[ i ] ) ) );

		const bool caught = mx > 2.0e-3;
		std::printf( "\ncontrol -- a deliberately wrong decay must NOT agree:\n"
		             "  max err %.3e%s\n", mx, caught ? "   (caught, good)" : "   FAIL (test is blind)" );

		if( !caught )
			++failures;
	}

	std::printf( failures == 0 ? "\nPASS\n" : "\nFAIL: %d\n", failures );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --transparent : a matched pair with no noise returns the picture
//---------------------------------------------------------------------------
int checkTransparent( const Options& options )
{
	std::printf( "The invariant the whole chain rests on: matched ends, no noise,\n"
	             "no tilt, and the picture must come back.\n\n" );

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "no GL context\n" );
		return 1;
	}

	const int W = options.width, H = options.height;

	const auto scene = makeScene( W, H );
	const GLuint input = uploadTexture( scene, W, H );
	Target target      = makeTarget( W, H );

	Plugin plugin;
	setNeutral( plugin );

	if( !render( plugin, target, input, W, H ) )
	{
		std::fprintf( stderr, "render failed\n" );
		return 1;
	}

	const auto out = readBytes( target );

	// ⚠️ **Transparency is exact only where the detector is TRACKING.**
	//
	// The encoder's detector sees the picture and the decoder's sees the
	// encoded signal, and the two agree only while both are following their
	// input instantaneously. Wherever the peak detector is holding a decayed
	// peak from something that has already gone past -- the right-hand side of
	// a highlight, the start of a line after a bright line end -- the two
	// envelopes differ and the round trip does not close. That is not a defect
	// to be tuned away, it is the mistracking this plugin is about, and a test
	// that demanded exactness everywhere would be demanding the effect not
	// happen.
	//
	// So: EXACT on the flat band, where there is nothing for a detector to
	// hold. Close on the ramp, where the scan wraps a bright line end into the
	// start of the next line. Nothing is claimed about the bars or the combs.
	struct Region
	{
		const char* name;
		float from, to;
		double tolerance;
	};

	const Region regions[] = {
		{ "flat band", 0.02f, kFlatTo - 0.01f, 0.6 },
		{ "ramp", kColourTo + 0.02f, 0.98f, 4.0 },
	};

	int failures = 0;

	std::printf( "%-14s %-12s %-12s %s\n", "region", "mean |dE|", "max |dE|", "" );

	for( const Region& r : regions )
	{
		const int y0 = int( r.from * H ), y1 = int( r.to * H );

		double sum = 0.0, mx = 0.0;
		long   n   = 0;

		for( int y = y0; y < y1; ++y )
		{
			// ⚠️ The scene is described top-down and the buffers are stored
			// bottom-up. Indexing one with the other silently compares the
			// wrong stripe: this test once reported the ramp's error under the
			// flat band's name and passed the flat band by measuring the ramp.
			const int ry = H - 1 - y;

			for( int x = 2; x < W - 2; ++x )
			{
				const size_t i = ( size_t( ry ) * W + x ) * 4;

				for( int c = 0; c < 3; ++c )
				{
					const double d = std::fabs( double( out[ i + c ] ) - double( scene[ i + c ] ) );
					sum += d;
					mx = std::max( mx, d );
					++n;
				}
			}
		}

		const double mean = n > 0 ? sum / n : 0.0;
		const bool   bad  = mean > r.tolerance;
		if( bad )
			++failures;

		std::printf( "%-14s %-12.3f %-12.0f %s\n", r.name, mean, mx, bad ? "FAIL" : "" );
	}

	// The control: put the noise back and it must STOP being transparent.
	plugin.SetFloatParameter( Plugin::PT_NOISE, 0.5f );
	render( plugin, target, input, W, H );
	const auto noisy = readBytes( target );

	double noisyMean = 0.0;
	long   n         = 0;
	for( int y = int( 0.02f * H ); y < int( ( kFlatTo - 0.01f ) * H ); ++y )
		for( int x = 2; x < W - 2; ++x )
			for( int c = 0; c < 3; ++c )
			{
				const size_t i = ( size_t( H - 1 - y ) * W + x ) * 4;
				noisyMean += std::fabs( double( noisy[ i + c ] ) - double( scene[ i + c ] ) );
				++n;
			}
	noisyMean = n > 0 ? noisyMean / n : 0.0;

	std::printf( "\ncontrol -- with the noise turned up the flat band must MOVE:\n"
	             "  mean |dE| %.3f%s\n", noisyMean,
	             noisyMean < 2.0 ? "   FAIL (noise never reached the picture)" : "" );
	if( noisyMean < 2.0 )
		++failures;

	releaseTarget( target );
	glDeleteTextures( 1, &input );
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );

	std::printf( failures == 0 ? "\nPASS\n" : "\nFAIL: %d\n", failures );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --anisotropy : the scan is the time axis, so only one direction moves
//---------------------------------------------------------------------------
int checkAnisotropy( const Options& options )
{
	std::printf( "A link has ONE frequency axis and on a picture it is the scan.\n"
	             "So companding must change detail ALONG the scan and leave\n"
	             "detail across it alone. An isotropic effect passes every other\n"
	             "test in this file and fails this one.\n\n" );

	CGLContextObj context = createContext();
	if( context == nullptr )
		return 1;

	const int W = options.width, H = options.height;

	const auto   scene = makeScene( W, H );
	const GLuint input = uploadTexture( scene, W, H );
	Target       target = makeTarget( W, H );

	Plugin plugin;
	setNeutral( plugin );

	// Encode only, hard: the most one-sided the chain gets, so whatever it does
	// to the detail band is as visible as it will ever be.
	plugin.SetFloatParameter( Plugin::PT_EMPHASIS, 1.0f );
	plugin.SetFloatParameter( Plugin::PT_TILT, 1.0f );
	plugin.SetFloatParameter( Plugin::PT_EXPAND, 0.0f );

	render( plugin, target, input, W, H );
	const auto out = readBytes( target );

	// The two combs. Same two levels, same one-pixel spacing, one turned
	// through ninety degrees -- so the only thing separating them is which
	// axis they lie on, and the ratio between what happens to them means
	// something.
	const double srcAlong = detailAlong( scene, W, H, kBarsTo + 0.02f, kCombVerticalTo - 0.02f, true );
	const double outAlong = detailAlong( out, W, H, kBarsTo + 0.02f, kCombVerticalTo - 0.02f, true );

	const double srcAcross =
	    detailAlong( scene, W, H, kCombVerticalTo + 0.02f, kCombHorizontalTo - 0.02f, false );
	const double outAcross =
	    detailAlong( out, W, H, kCombVerticalTo + 0.02f, kCombHorizontalTo - 0.02f, false );

	const double alongRatio  = srcAlong > 1e-6 ? outAlong / srcAlong : 1.0;
	const double acrossRatio = srcAcross > 1e-6 ? outAcross / srcAcross : 1.0;

	std::printf( "%-24s %-12s %-12s %s\n", "", "source", "output", "ratio" );
	std::printf( "%-24s %-12.5f %-12.5f %.3f\n", "along the scan", srcAlong, outAlong, alongRatio );
	std::printf( "%-24s %-12.5f %-12.5f %.3f\n", "across the scan", srcAcross, outAcross, acrossRatio );

	int failures = 0;

	// A degenerate source measurement makes the ratio meaningless. This test
	// passed for a while on a scene whose across-scan detail was exactly zero.
	if( srcAlong < 1.0e-4 || srcAcross < 1.0e-4 )
	{
		std::printf( "\nFAIL: the scene carries no detail on one of the two axes, "
		             "so this test is measuring nothing\n" );
		++failures;
	}

	// Along the scan the detail band was lifted and never taken back off, so it
	// must have grown appreciably.
	if( alongRatio < 1.2 )
	{
		std::printf( "\nFAIL: detail along the scan barely moved -- the emphasis "
		             "network is not reaching the picture\n" );
		++failures;
	}

	// Across the scan the picture went through unchanged apart from the gain
	// the compressor applied, which is broad. A ratio anywhere near the along
	// figure means the band split is isotropic.
	if( acrossRatio > 1.0 + ( alongRatio - 1.0 ) * 0.4 )
	{
		std::printf( "\nFAIL: detail across the scan moved almost as much as detail "
		             "along it -- this is an isotropic effect, not a link\n" );
		++failures;
	}

	releaseTarget( target );
	glDeleteTextures( 1, &input );
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );

	std::printf( failures == 0 ? "\nPASS\n" : "\nFAIL: %d\n", failures );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --presets : every factory preset survives every host
//
// Three hosts, no GL. Resolume is the second kind and a copy-based apply cannot
// work on it -- see Plugin.h. Against the pre-fix shape of that code this fails
// in precisely the "ignores" column.
//---------------------------------------------------------------------------
int checkPresets()
{
	enum Host
	{
		kHonours,
		kIgnores,
		kQuantises,
		kHostCount
	};

	const char* hostNames[ kHostCount ] = { "honours events", "ignores events", "honours+quantises" };

	int failures = 0;

	std::printf( "%-18s %-20s %s\n", "host", "preset", "result" );

	for( int host = 0; host < kHostCount; ++host )
	{
		for( int index = 1; index <= presets::kCount; ++index )
		{
			Plugin plugin;

			// The host's opening position: it pushes every default down first.
			for( unsigned int id = 0; id < Plugin::PT_PRESET; ++id )
				plugin.SetFloatParameter( id, plugin.GetFloatParameter( id ) );

			plugin.SetFloatParameter( Plugin::PT_PRESET, float( index ) );

			// Now the host does what THIS host does. The one that ignores the
			// plugin's value events carries on pushing the values it still
			// believes in -- which are the ones from before the preset.
			if( host == kIgnores )
			{
				for( int i = 0; i < presets::kParamCount; ++i )
				{
					const unsigned int id = Plugin::PT_COMPRESS;// placeholder, replaced below
					( void )id;
				}

				// Restate the pre-preset defaults, exactly as Resolume does.
				const controls::HostValues d;
				plugin.SetFloatParameter( Plugin::PT_CHROMA, d.chroma );
				plugin.SetFloatParameter( Plugin::PT_COMPRESS, d.compress );
				plugin.SetFloatParameter( Plugin::PT_EMPHASIS, d.emphasis );
				plugin.SetFloatParameter( Plugin::PT_LINK_LEVEL, d.linkLevel );
				plugin.SetFloatParameter( Plugin::PT_NOISE, d.noise );
				plugin.SetFloatParameter( Plugin::PT_HEADROOM, d.headroom );
				plugin.SetFloatParameter( Plugin::PT_EXPAND, d.expand );
				plugin.SetFloatParameter( Plugin::PT_TILT, d.tilt );
				plugin.SetFloatParameter( Plugin::PT_TIME_CONSTANT, d.timeConstant );
				plugin.SetFloatParameter( Plugin::PT_PIVOT, d.pivot );
				plugin.SetFloatParameter( Plugin::PT_MAX_GAIN, d.maxGain );
			}
			else
			{
				// A host that honours the events reads the new values back and
				// restates those. Quantised to a thousandth by the one that
				// rounds -- which is what 1e-4 read as an edit.
				for( int i = 0; i < presets::kParamCount; ++i )
				{
					const presets::Preset& p = presets::kPresets[ index - 1 ];
					float v                  = p.v[ i ];
					if( host == kQuantises )
						v = std::round( v * 1000.0f ) / 1000.0f;

					// The ParamID for this preset slot, read back off the
					// plugin rather than duplicated here.
					static const unsigned int ids[ presets::kParamCount ] = {
						Plugin::PT_CHROMA, Plugin::PT_COMPRESS, Plugin::PT_EMPHASIS,
						Plugin::PT_LINK_LEVEL, Plugin::PT_NOISE, Plugin::PT_HEADROOM,
						Plugin::PT_EXPAND, Plugin::PT_TILT, Plugin::PT_TIME_CONSTANT,
						Plugin::PT_PIVOT, Plugin::PT_MAX_GAIN
					};
					plugin.SetFloatParameter( ids[ i ], v );
				}
			}

			// The preset must still be selected, and the rendered values must
			// still be the preset's.
			const int    still = int( std::lround( plugin.GetFloatParameter( Plugin::PT_PRESET ) ) );
			const Settings got = [ & ] {
				controls::HostValues v;
				v.chroma       = plugin.GetFloatParameter( Plugin::PT_CHROMA );
				v.compress     = plugin.GetFloatParameter( Plugin::PT_COMPRESS );
				v.emphasis     = plugin.GetFloatParameter( Plugin::PT_EMPHASIS );
				v.linkLevel    = plugin.GetFloatParameter( Plugin::PT_LINK_LEVEL );
				v.noise        = plugin.GetFloatParameter( Plugin::PT_NOISE );
				v.headroom     = plugin.GetFloatParameter( Plugin::PT_HEADROOM );
				v.expand       = plugin.GetFloatParameter( Plugin::PT_EXPAND );
				v.tilt         = plugin.GetFloatParameter( Plugin::PT_TILT );
				v.timeConstant = plugin.GetFloatParameter( Plugin::PT_TIME_CONSTANT );
				v.pivot        = plugin.GetFloatParameter( Plugin::PT_PIVOT );
				v.maxGain      = plugin.GetFloatParameter( Plugin::PT_MAX_GAIN );
				return controls::toSettings( v );
			}();

			controls::HostValues want;
			const presets::Preset& p = presets::kPresets[ index - 1 ];
			want.chroma       = p.v[ presets::kChroma ];
			want.compress     = p.v[ presets::kCompress ];
			want.emphasis     = p.v[ presets::kEmphasis ];
			want.linkLevel    = p.v[ presets::kLinkLevel ];
			want.noise        = p.v[ presets::kNoise ];
			want.headroom     = p.v[ presets::kHeadroom ];
			want.expand       = p.v[ presets::kExpand ];
			want.tilt         = p.v[ presets::kTilt ];
			want.timeConstant = p.v[ presets::kTimeConstant ];
			want.pivot        = p.v[ presets::kPivot ];
			want.maxGain      = p.v[ presets::kMaxGain ];
			const Settings expected = controls::toSettings( want );

			const bool held = still == index &&
			                  std::fabs( got.compressRatio - expected.compressRatio ) < 1e-3f &&
			                  std::fabs( got.emphasisDb - expected.emphasisDb ) < 1e-2f &&
			                  std::fabs( got.timeConstantUs - expected.timeConstantUs ) < 1e-1f;

			if( !held )
			{
				++failures;
				std::printf( "%-18s %-20s FAIL (preset now %d)\n", hostNames[ host ], p.name, still );
			}
		}

		std::printf( "%-18s %-20s %s\n", hostNames[ host ], "all",
		             failures == 0 ? "held" : "see above" );
	}

	std::printf( failures == 0 ? "\nPASS\n" : "\nFAIL: %d\n", failures );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --out, --scene, --bench
//---------------------------------------------------------------------------
int renderFrame( const std::string& path, const Options& options )
{
	CGLContextObj context = createContext();
	if( context == nullptr )
		return 1;

	const int W = options.width, H = options.height;

	const auto   scene  = makeScene( W, H );
	const GLuint input  = uploadTexture( scene, W, H );
	Target       target = makeTarget( W, H );

	Plugin plugin;
	setNeutral( plugin );
	applySets( plugin, options );

	if( !render( plugin, target, input, W, H ) )
	{
		std::fprintf( stderr, "render failed\n" );
		return 1;
	}

	const auto ok = writePng( path, W, H, flipRows( readBytes( target ), W, H ) );

	releaseTarget( target );
	glDeleteTextures( 1, &input );
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );

	std::printf( "%s\n", ok ? path.c_str() : "write failed" );
	return ok ? 0 : 1;
}

int writeScene( const std::string& path, const Options& options )
{
	const auto scene = makeScene( options.width, options.height );
	const bool ok    = writePng( path, options.width, options.height,
	                             flipRows( scene, options.width, options.height ) );

	std::printf( "%s\n", ok ? path.c_str() : "write failed" );
	return ok ? 0 : 1;
}

int bench( const Options& options )
{
	CGLContextObj context = createContext();
	if( context == nullptr )
		return 1;

	const int W = options.width, H = options.height;

	const auto   scene  = makeScene( W, H );
	const GLuint input  = uploadTexture( scene, W, H );
	Target       target = makeTarget( W, H );

	Plugin plugin;
	setNeutral( plugin );
	applySets( plugin, options );

	std::printf( "%dx%d\n\n%-16s %-10s %-8s %s\n", W, H, "time constant", "passes", "ms", "" );

	// The pass count follows the time constant, so the cost does too. Reporting
	// one number for "the effect" would hide the only thing about its cost an
	// operator can act on.
	for( float tc : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f } )
	{
		plugin.SetFloatParameter( Plugin::PT_TIME_CONSTANT, tc );

		const float us  = controls::timeConstantUsFromParam( tc );
		const float tau = timeConstantSamples( us, W );
		const int   n   = scanPasses( tau * 0.25f );

		for( int i = 0; i < 10; ++i )
			render( plugin, target, input, W, H );
		glFinish();

		const auto start = std::chrono::steady_clock::now();
		const int  runs  = 50;
		for( int i = 0; i < runs; ++i )
			render( plugin, target, input, W, H );
		glFinish();
		const auto end = std::chrono::steady_clock::now();

		const double ms = std::chrono::duration< double, std::milli >( end - start ).count() / runs;

		std::printf( "%-16.2f %-10d %-8.2f\n", us, n, ms );
	}

	releaseTarget( target );
	glDeleteTextures( 1, &input );
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );

	return 0;
}

//---------------------------------------------------------------------------
// --sheet : every factory preset, on one page
//---------------------------------------------------------------------------
int writeSheet( const std::string& path, const Options& options )
{
	CGLContextObj context = createContext();
	if( context == nullptr )
		return 1;

	const int cols = 3;
	const int rows = ( presets::kCount + 1 + cols - 1 ) / cols;
	const int cw   = options.width;
	const int ch   = options.height;

	const auto   scene  = makeScene( cw, ch );
	const GLuint input  = uploadTexture( scene, cw, ch );
	Target       target = makeTarget( cw, ch );

	std::vector< unsigned char > sheet( size_t( cw * cols ) * ( ch * rows ) * 4, 0 );

	// Cell 0 is the source, so every preset has something to be compared with
	// on the same page.
	auto place = [ & ]( int cell, const std::vector< unsigned char >& topDown ) {
		const int cx = ( cell % cols ) * cw;
		const int cy = ( cell / cols ) * ch;

		for( int y = 0; y < ch; ++y )
			std::memcpy( &sheet[ ( size_t( cy + y ) * ( cw * cols ) + cx ) * 4 ],
			             &topDown[ size_t( y ) * cw * 4 ], size_t( cw ) * 4 );
	};

	place( 0, flipRows( scene, cw, ch ) );

	for( int i = 0; i < presets::kCount; ++i )
	{
		Plugin plugin;

		// The host pushes its defaults down first, then the operator picks a
		// preset -- the same sequence checkPresets drives.
		for( unsigned int id = 0; id < Plugin::PT_PRESET; ++id )
			plugin.SetFloatParameter( id, plugin.GetFloatParameter( id ) );
		plugin.SetFloatParameter( Plugin::PT_PRESET, float( i + 1 ) );

		if( !render( plugin, target, input, cw, ch ) )
		{
			std::fprintf( stderr, "render failed for %s\n", presets::kPresets[ i ].name );
			return 1;
		}

		place( i + 1, flipRows( readBytes( target ), cw, ch ) );
		std::printf( "  %d %s\n", i + 1, presets::kPresets[ i ].name );
	}

	const bool ok = writePng( path, cw * cols, ch * rows, sheet );

	releaseTarget( target );
	glDeleteTextures( 1, &input );
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );

	std::printf( "%s\n", ok ? path.c_str() : "write failed" );
	return ok ? 0 : 1;
}

void usage()
{
	std::printf(
	    "cmtest -- the compander harness\n\n"
	    "  --list                parameters, types and physical values\n"
	    "  --flat                Tilt and Emphasis are inert on a flat field\n"
	    "  --roundtrip           the two ends cancel, and where they stop\n"
	    "  --detector            doubling passes against the serial law\n"
	    "  --transparent         a matched pair with no noise returns the picture\n"
	    "  --anisotropy          only detail along the scan moves\n"
	    "  --presets             every preset survives every host\n"
	    "  --out PATH            render a frame\n"
	    "  --scene PATH          write the synthetic test scene\n"
	    "  --sheet PATH          every factory preset on one page\n"
	    "  --bench               render cost against the time constant\n"
	    "  --set \"Name=value\"    set a parameter by its host-facing name\n"
	    "  --width N --height N  frame size (default 640x360)\n" );
}

} // namespace

int main( int argc, char** argv )
{
	Options options;
	std::string mode, path;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];

		auto next = [ & ]() -> std::string { return i + 1 < argc ? argv[ ++i ] : std::string(); };

		if( arg == "--set" )
		{
			const std::string s  = next();
			const size_t      eq = s.find( '=' );
			if( eq != std::string::npos )
				options.sets.emplace_back( s.substr( 0, eq ), std::stof( s.substr( eq + 1 ) ) );
		}
		else if( arg == "--width" )
			options.width = std::stoi( next() );
		else if( arg == "--height" )
			options.height = std::stoi( next() );
		else if( arg == "--out" || arg == "--scene" || arg == "--sheet" )
		{
			mode = arg;
			path = next();
		}
		else if( arg.rfind( "--", 0 ) == 0 )
			mode = arg;
	}

	if( mode == "--list" )
		return listParameters();
	if( mode == "--flat" )
		return checkFlat();
	if( mode == "--roundtrip" )
		return checkRoundtrip();
	if( mode == "--detector" )
		return checkDetector();
	if( mode == "--transparent" )
		return checkTransparent( options );
	if( mode == "--anisotropy" )
		return checkAnisotropy( options );
	if( mode == "--presets" )
		return checkPresets();
	if( mode == "--out" )
		return renderFrame( path, options );
	if( mode == "--scene" )
		return writeScene( path, options );
	if( mode == "--sheet" )
		return writeSheet( path, options );
	if( mode == "--bench" )
		return bench( options );

	usage();
	return 0;
}
