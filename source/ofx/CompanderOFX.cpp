/**
    Compander as an OpenFX plugin, for Resolve, Nuke, Natron and Vegas.

    Same controls, same presets, same 0..1 space as the FFGL build, and the same
    model: `Compander.cpp`, `Chain.cpp`, `Detector.cpp` and `Controls.cpp` are
    linked straight from source. Only the render loop is written twice.

    ------------------------------------------------- why there is no ImageProcessor

    Every other plugin in this fleet renders through `OFX::ImageProcessor`, which
    splits the render window into horizontal strips and runs them on separate
    threads. **This one cannot**, and the reason is the whole point of the
    plugin: the envelope detector is a strictly serial scan over the picture,
    left to right and top to bottom, in which every sample depends on the one
    before it. A strip starting halfway down the frame has no way to know what
    the envelope was when the scan reached it.

    So the chain is computed once over the WHOLE image into a set of planes, and
    then the render window is written out of them. `getRegionsOfInterest` asks
    for the full source for the same reason.

    ------------------------------------------------- the detector is EXACT here

    The FFGL build cannot run a serial recursion on a GPU, so it computes the
    same peak detector by recursive doubling -- exact within a window of `2^K`
    samples, with a pass cap that stops the window reaching a whole frame.

    A CPU has no such problem. This build calls `serialEnvelope`, which is the
    law itself with unbounded history, at full resolution rather than quarter.
    **So the two builds do not match, and this one is the more correct.** The
    difference shows at long time constants, where the FFGL build crossfades to
    a frame-global follower and this one simply keeps scanning.

    ⚠️ **What this build cannot do is the frame-to-frame release.** OFX renders
    frames in any order and holds no state between them, so there is no previous
    frame's envelope to decay from. The FFGL build's whole-picture pumping across
    a cut is therefore absent here. Exact and deterministic, where the FFGL side
    is merely faithful -- the same trade afterglow's OFX build makes with its
    frame queue, and worth knowing before comparing the two side by side.
*/

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"

#include "StoatworksAboutOFX.h"

#include "../Chain.h"
#include "../Compander.h"
#include "../Controls.h"
#include "../Detector.h"
#include "../Presets.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace
{
constexpr const char* kPluginIdentifier = "com.stoatworks.compander";
constexpr const char* kPluginName       = "Compander";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"A radio mic's companding circuit, with your picture pushed through it.\n\n"
	"An analogue wireless link cannot carry the dynamic range of what is sent "
	"down it, so the transmitter squashes it -- pre-emphasis, then a compressor "
	"that halves the signal's excursion in dB -- and the receiver does the "
	"opposite. In between sits a link with a noise floor, a ceiling and no "
	"opinion about either. Everything here comes from the two ends failing to "
	"cancel.\n\n"
	"Time Constant is the control that makes this a video effect: a compander's "
	"attack and release, applied to a picture, is a distance ALONG THE SCAN. It "
	"runs from haloing tight to every edge, through a smear off the side of "
	"things, to streaks pulling down the frame.\n\n"
	"https://stoatworks-labs.com";

constexpr const char* kParamPreset       = "preset";
constexpr const char* kParamSidechain    = "sidechain";
constexpr const char* kParamChroma       = "chroma";
constexpr const char* kParamCompress     = "compress";
constexpr const char* kParamEmphasis     = "emphasis";
constexpr const char* kParamLinkLevel    = "linkLevel";
constexpr const char* kParamNoise        = "noise";
constexpr const char* kParamHeadroom     = "headroom";
constexpr const char* kParamExpand       = "expand";
constexpr const char* kParamTilt         = "tilt";
constexpr const char* kParamTimeConstant = "timeConstant";
constexpr const char* kParamPivot        = "pivot";
constexpr const char* kParamMaxGain      = "maxGain";
constexpr const char* kParamMix          = "mix";

/// The preset table is host-agnostic; this is the OFX binding of it, in
/// presets::Param order. Same job as the FFGL build's kPresetParamIDs, and the
/// static_assert below is what stops the two drifting.
const char* const kPresetParamNames[ compander::presets::kParamCount ] = {
	kParamChroma, kParamCompress, kParamEmphasis, kParamLinkLevel, kParamNoise,
	kParamHeadroom, kParamExpand, kParamTilt, kParamTimeConstant, kParamPivot, kParamMaxGain
};

static_assert( sizeof( kPresetParamNames ) / sizeof( kPresetParamNames[ 0 ] ) ==
                   compander::presets::kParamCount,
               "the OFX preset binding and the preset table disagree" );

//---------------------------------------------------------------------------
// The link's noise.
//
// The same integer PCG hash as the shader, so the two builds put the noise in
// the same places for the same frame rather than merely putting a similar
// amount of it somewhere.
//---------------------------------------------------------------------------
unsigned int pcgHash( unsigned int v )
{
	unsigned int state = v * 747796405u + 2891336453u;
	unsigned int word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

float noiseAt( int x, int y, int frame )
{
	const unsigned int h =
	    pcgHash( static_cast< unsigned int >( x ) + 1973u * static_cast< unsigned int >( y ) +
	             9277u * static_cast< unsigned int >( frame ) );
	return static_cast< float >( h ) * ( 2.0f / 4294967295.0f ) - 1.0f;
}

/// One horizontal Gaussian blur, matching `kBlurFragment`: thirteen taps at
/// sigma/3 spacing. Horizontal and nothing else -- see Chain.h.
void blurHorizontal( const std::vector< float >& src, int w, int h, float sigma,
                     std::vector< float >& dst )
{
	const float s    = std::max( sigma, 0.5f );
	const float step = s / 3.0f;

	dst.resize( src.size() );

	for( int y = 0; y < h; ++y )
	{
		const float* row = &src[ static_cast< size_t >( y ) * w ];
		float*       out = &dst[ static_cast< size_t >( y ) * w ];

		for( int x = 0; x < w; ++x )
		{
			float sum = 0.0f, weight = 0.0f;

			for( int i = -6; i <= 6; ++i )
			{
				const float d = static_cast< float >( i ) * step;
				const float k = std::exp( -0.5f * ( d * d ) / ( s * s ) );

				// Clamped at the edges, matching GL_CLAMP_TO_EDGE, so a flat
				// field stays flat right to the frame border.
				const int sx = std::clamp( x + static_cast< int >( std::lround( d ) ), 0, w - 1 );

				sum += k * row[ sx ];
				weight += k;
			}

			out[ x ] = sum / weight;
		}
	}
}

/// The whole chain, over one full-resolution picture.
struct Planes
{
	int width  = 0;
	int height = 0;

	std::vector< float > luma;
	std::vector< float > outLuma;

	void compute( const std::vector< float >& source, int w, int h,
	              const compander::Settings& settings, int frame )
	{
		using namespace compander;

		width  = w;
		height = h;

		const size_t n = static_cast< size_t >( w ) * h;

		luma.assign( n, 0.0f );
		for( size_t i = 0; i < n; ++i )
		{
			const float rgb[ 3 ] = { source[ i * 4 ], source[ i * 4 + 1 ], source[ i * 4 + 2 ] };
			luma[ i ]            = compander::luma( rgb );
		}

		const Crossover cross = crossoverFor( w );
		const float     tau   = timeConstantSamples( settings.timeConstantUs, w );

		std::vector< float > narrow, wide, envelope( n ), encoded( n ), decoded( n );

		blurHorizontal( luma, w, h, cross.narrow, narrow );
		blurHorizontal( luma, w, h, cross.wide, wide );

		// The law itself, full resolution, unbounded history. See the file
		// header for why this is not what the FFGL build does.
		serialEnvelope( luma.data(), w, h, tau, envelope.data() );

		const float linkGain = std::pow( 10.0f, settings.linkLevelDb / 20.0f );

		for( size_t i = 0; i < n; ++i )
		{
			const Bands bands { wide[ i ], narrow[ i ] - wide[ i ], luma[ i ] - narrow[ i ] };

			// No audio sidechain in this build: OFX has no FFT input and no
			// beat info, so `sidechain` is left out of the control surface
			// entirely rather than exposed as a control that does nothing.
			const float gain = compressGain( envelope[ i ], settings.pivot,
			                                 settings.compressRatio, settings.maxGainDb );

			const int x = static_cast< int >( i % static_cast< size_t >( w ) );
			const int y = static_cast< int >( i / static_cast< size_t >( w ) );

			encoded[ i ] = link( preEmphasis( bands, settings.emphasisDb ) * gain, linkGain,
			                     settings.headroom, settings.noise, noiseAt( x, y, frame ) );
		}

		// The SECOND detector, on the encoded signal. It sees a different
		// signal from the first -- compressed, levelled and noisy -- and that
		// difference is where the mistracking comes from. Reusing the first
		// would delete the effect.
		serialEnvelope( encoded.data(), w, h, tau, envelope.data() );

		for( size_t i = 0; i < n; ++i )
			decoded[ i ] = encoded[ i ] * expandGain( envelope[ i ], settings.pivot,
			                                          settings.expandRatio, settings.maxGainDb );

		// De-emphasis acts on what the expander produced, so the band split is
		// taken again, of the decoded signal.
		blurHorizontal( decoded, w, h, cross.narrow, narrow );
		blurHorizontal( decoded, w, h, cross.wide, wide );

		outLuma.assign( n, 0.0f );
		for( size_t i = 0; i < n; ++i )
		{
			const Bands bands { wide[ i ], narrow[ i ] - wide[ i ], decoded[ i ] - narrow[ i ] };
			outLuma[ i ] = deEmphasis( bands, settings.emphasisDb, settings.tilt );
		}
	}
};

//---------------------------------------------------------------------------
// Pixel access. One template per depth rather than a conversion pass, so a
// float pipeline is not quantised on its way through.
//---------------------------------------------------------------------------
template< class T, int maxValue >
float toFloat( T v )
{
	return maxValue == 1 ? static_cast< float >( v )
	                     : static_cast< float >( v ) / static_cast< float >( maxValue );
}

template< class T, int maxValue >
T fromFloat( float v )
{
	if( maxValue == 1 )
		return static_cast< T >( v );

	const float scaled = v * static_cast< float >( maxValue ) + 0.5f;
	return static_cast< T >( std::clamp( scaled, 0.0f, static_cast< float >( maxValue ) ) );
}

class CompanderPlugin : public OFX::ImageEffect
{
public:
	explicit CompanderPlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		preset       = fetchChoiceParam( kParamPreset );
		chroma       = fetchDoubleParam( kParamChroma );
		compress     = fetchDoubleParam( kParamCompress );
		emphasis     = fetchDoubleParam( kParamEmphasis );
		linkLevel    = fetchDoubleParam( kParamLinkLevel );
		noise        = fetchDoubleParam( kParamNoise );
		headroom     = fetchDoubleParam( kParamHeadroom );
		expand       = fetchDoubleParam( kParamExpand );
		tilt         = fetchDoubleParam( kParamTilt );
		timeConstant = fetchDoubleParam( kParamTimeConstant );
		pivot        = fetchDoubleParam( kParamPivot );
		maxGain      = fetchDoubleParam( kParamMaxGain );
		mix          = fetchDoubleParam( kParamMix );
	}

	/// The whole source, always.
	///
	/// The detector is a serial scan over the picture, so a tile has no meaning
	/// here: rendering the bottom half needs to know what the envelope was when
	/// the scan arrived there, which means having scanned the top half.
	void getRegionsOfInterest( const OFX::RegionsOfInterestArguments& args,
	                           OFX::RegionOfInterestSetter& rois ) override
	{
		rois.setRegionOfInterest( *srcClip, srcClip->getRegionOfDefinition( args.time ) );
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr< OFX::Image > src( srcClip->fetchImage( args.time ) );

		if( !dst || !src )
			OFX::throwSuiteStatusException( kOfxStatFailed );

		const OFX::BitDepthEnum       depth = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();

		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		const OfxRectI bounds = src->getBounds();
		const int      w      = bounds.x2 - bounds.x1;
		const int      h      = bounds.y2 - bounds.y1;

		if( w <= 0 || h <= 0 )
			return;

		const compander::Settings settings = currentSettings( args.time );

		// The frame number seeds the noise, so it moves between frames the way
		// a link's noise floor does rather than sitting still like a texture.
		const int frame = static_cast< int >( std::lround( args.time ) ) & 0x00ffffff;

		std::vector< float > source( static_cast< size_t >( w ) * h * 4, 0.0f );

		switch( depth )
		{
			case OFX::eBitDepthUByte:
				gather< unsigned char, 255 >( *src, bounds, w, h, comps, source );
				break;
			case OFX::eBitDepthUShort:
				gather< unsigned short, 65535 >( *src, bounds, w, h, comps, source );
				break;
			case OFX::eBitDepthFloat:
				gather< float, 1 >( *src, bounds, w, h, comps, source );
				break;
			default:
				OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}

		Planes planes;
		planes.compute( source, w, h, settings, frame );

		switch( depth )
		{
			case OFX::eBitDepthUByte:
				scatter< unsigned char, 255 >( *dst, bounds, w, h, comps, source, planes,
				                               settings, args.renderWindow );
				break;
			case OFX::eBitDepthUShort:
				scatter< unsigned short, 65535 >( *dst, bounds, w, h, comps, source, planes,
				                                  settings, args.renderWindow );
				break;
			case OFX::eBitDepthFloat:
				scatter< float, 1 >( *dst, bounds, w, h, comps, source, planes, settings,
				                     args.renderWindow );
				break;
			default:
				break;
		}
	}

	bool isIdentity( const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip,
	                 double& identityTime ) override
	{
		if( mix->getValueAtTime( args.time ) <= 0.0 )
		{
			identityClip = srcClip;
			identityTime = args.time;
			return true;
		}
		return false;
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& name ) override
	{
		if( stoatworks::about::ofx::changedParam( args, name ) )
			return;

		if( name == kParamPreset )
		{
			// OFX::ChoiceParam::getValue takes an out-parameter rather than
			// returning one.
			int chosen = 0;
			preset->getValue( chosen );
			applyPreset( chosen, args.time );
			return;
		}

		// An edit to a covered control falls back to Custom. Guarded against
		// the plugin's own writes, and against a host replaying values at a new
		// time, both of which arrive here looking like edits.
		if( applyingPreset || args.reason != OFX::eChangeUserEdit )
			return;

		for( int i = 0; i < compander::presets::kParamCount; ++i )
			if( name == kPresetParamNames[ i ] )
			{
				preset->setValue( 0 );
				return;
			}
	}

private:
	compander::Settings currentSettings( double time ) const
	{
		compander::controls::HostValues v;

		v.chroma       = float( chroma->getValueAtTime( time ) );
		v.compress     = float( compress->getValueAtTime( time ) );
		v.emphasis     = float( emphasis->getValueAtTime( time ) );
		v.linkLevel    = float( linkLevel->getValueAtTime( time ) );
		v.noise        = float( noise->getValueAtTime( time ) );
		v.headroom     = float( headroom->getValueAtTime( time ) );
		v.expand       = float( expand->getValueAtTime( time ) );
		v.tilt         = float( tilt->getValueAtTime( time ) );
		v.timeConstant = float( timeConstant->getValueAtTime( time ) );
		v.pivot        = float( pivot->getValueAtTime( time ) );
		v.maxGain      = float( maxGain->getValueAtTime( time ) );
		v.mix          = float( mix->getValueAtTime( time ) );

		return compander::controls::toSettings( v );
	}

	void applyPreset( int index, double time )
	{
		if( index <= 0 || index > compander::presets::kCount )
			return;

		const compander::presets::Preset& p = compander::presets::kPresets[ index - 1 ];

		applyingPreset = true;
		beginEditBlock( "preset" );

		for( int i = 0; i < compander::presets::kParamCount; ++i )
			if( OFX::DoubleParam* param = doubleByName( kPresetParamNames[ i ] ) )
				param->setValueAtTime( time, p.v[ i ] );

		endEditBlock();
		applyingPreset = false;
	}

	OFX::DoubleParam* doubleByName( const std::string& name ) const
	{
		if( name == kParamChroma ) return chroma;
		if( name == kParamCompress ) return compress;
		if( name == kParamEmphasis ) return emphasis;
		if( name == kParamLinkLevel ) return linkLevel;
		if( name == kParamNoise ) return noise;
		if( name == kParamHeadroom ) return headroom;
		if( name == kParamExpand ) return expand;
		if( name == kParamTilt ) return tilt;
		if( name == kParamTimeConstant ) return timeConstant;
		if( name == kParamPivot ) return pivot;
		if( name == kParamMaxGain ) return maxGain;
		if( name == kParamMix ) return mix;
		return nullptr;
	}

	template< class T, int maxValue >
	void gather( OFX::Image& image, const OfxRectI& bounds, int w, int h,
	             OFX::PixelComponentEnum comps, std::vector< float >& out ) const
	{
		const int channels = comps == OFX::ePixelComponentRGBA ? 4 : 3;

		for( int y = 0; y < h; ++y )
		{
			for( int x = 0; x < w; ++x )
			{
				const T* p = static_cast< T* >( image.getPixelAddress( bounds.x1 + x, bounds.y1 + y ) );
				float*   o = &out[ ( static_cast< size_t >( y ) * w + x ) * 4 ];

				if( p == nullptr )
				{
					o[ 0 ] = o[ 1 ] = o[ 2 ] = o[ 3 ] = 0.0f;
					continue;
				}

				o[ 0 ] = toFloat< T, maxValue >( p[ 0 ] );
				o[ 1 ] = toFloat< T, maxValue >( p[ 1 ] );
				o[ 2 ] = toFloat< T, maxValue >( p[ 2 ] );
				o[ 3 ] = channels == 4 ? toFloat< T, maxValue >( p[ 3 ] ) : 1.0f;
			}
		}
	}

	template< class T, int maxValue >
	void scatter( OFX::Image& image, const OfxRectI& bounds, int w, int h,
	              OFX::PixelComponentEnum comps, const std::vector< float >& source,
	              const Planes& planes, const compander::Settings& settings,
	              const OfxRectI& window ) const
	{
		const int channels = comps == OFX::ePixelComponentRGBA ? 4 : 3;

		for( int y = window.y1; y < window.y2; ++y )
		{
			const int py = y - bounds.y1;
			if( py < 0 || py >= h )
				continue;

			for( int x = window.x1; x < window.x2; ++x )
			{
				const int px = x - bounds.x1;
				if( px < 0 || px >= w )
					continue;

				T* out = static_cast< T* >( image.getPixelAddress( x, y ) );
				if( out == nullptr )
					continue;

				const size_t i   = static_cast< size_t >( py ) * w + px;
				const float* rgb = &source[ i * 4 ];

				float wet[ 3 ] = {};
				compander::applyGain( rgb, planes.luma[ i ], planes.outLuma[ i ], settings.chroma, wet );

				for( int c = 0; c < 3; ++c )
					out[ c ] = fromFloat< T, maxValue >( rgb[ c ] + settings.mix * ( wet[ c ] - rgb[ c ] ) );

				if( channels == 4 )
					out[ 3 ] = fromFloat< T, maxValue >( rgb[ 3 ] );
			}
		}
	}

	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;

	OFX::ChoiceParam* preset       = nullptr;
	OFX::DoubleParam* chroma       = nullptr;
	OFX::DoubleParam* compress     = nullptr;
	OFX::DoubleParam* emphasis     = nullptr;
	OFX::DoubleParam* linkLevel    = nullptr;
	OFX::DoubleParam* noise        = nullptr;
	OFX::DoubleParam* headroom     = nullptr;
	OFX::DoubleParam* expand       = nullptr;
	OFX::DoubleParam* tilt         = nullptr;
	OFX::DoubleParam* timeConstant = nullptr;
	OFX::DoubleParam* pivot        = nullptr;
	OFX::DoubleParam* maxGain      = nullptr;
	OFX::DoubleParam* mix          = nullptr;

	bool applyingPreset = false;
};

OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc,
                                          OFX::PageParamDescriptor* page, const char* name,
                                          const char* label, const char* hint, double value )
{
	OFX::DoubleParamDescriptor* param = desc.defineDoubleParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	param->setDefault( value );
	param->setRange( 0.0, 1.0 );
	param->setDisplayRange( 0.0, 1.0 );
	param->setDoubleType( OFX::eDoubleTypePlain );
	page->addChild( *param );
	return param;
}

class CompanderPluginFactory : public OFX::PluginFactoryHelper< CompanderPluginFactory >
{
public:
	CompanderPluginFactory( const std::string& id, unsigned int major, unsigned int minor ) :
		OFX::PluginFactoryHelper< CompanderPluginFactory >( id, major, minor )
	{
	}

	void describe( OFX::ImageEffectDescriptor& desc ) override;
	void describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum context ) override;
	OFX::ImageEffect* createInstance( OfxImageEffectHandle handle, OFX::ContextEnum ) override;
};

void CompanderPluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	desc.setSingleInstance( false );
	desc.setHostFrameThreading( false );
	desc.setSupportsMultiResolution( true );

	// ⚠️ Tiles OFF, and this is not a performance choice.
	//
	// The envelope detector is a serial scan across the whole picture in which
	// every sample depends on the one before it. A host that handed this a tile
	// would be asking for the middle of a recursion without its beginning. With
	// tiles enabled the plugin renders correctly only when the host happens to
	// ask for the whole frame.
	desc.setSupportsTiles( false );

	desc.setTemporalClipAccess( false );
	desc.setRenderTwiceAlways( false );
	desc.setSupportsMultipleClipPARs( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
}

void CompanderPluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* src = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	src->addSupportedComponent( OFX::ePixelComponentRGBA );
	src->addSupportedComponent( OFX::ePixelComponentRGB );
	src->setTemporalClipAccess( false );
	src->setSupportsTiles( false );
	src->setIsMask( false );

	OFX::ClipDescriptor* dst = desc.defineClip( kOfxImageEffectOutputClipName );
	dst->addSupportedComponent( OFX::ePixelComponentRGBA );
	dst->addSupportedComponent( OFX::ePixelComponentRGB );
	dst->setSupportsTiles( false );

	// Same parameters, same 0..1 ranges, same defaults as the FFGL build, so
	// the two inspectors read identically and one set of docs covers both.
	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );
	const compander::controls::HostValues defaults;

	OFX::ChoiceParamDescriptor* presetParam = desc.defineChoiceParam( kParamPreset );
	presetParam->setLabels( "Preset", "Preset", "Preset" );
	presetParam->setHint( "Real circuits. Picking one sets the controls below; editing any of "
	                      "them afterwards falls back to Custom." );
	presetParam->appendOption( "Custom" );
	for( int i = 0; i < compander::presets::kCount; ++i )
		presetParam->appendOption( compander::presets::kPresets[ i ].name );
	presetParam->setDefault( 0 );
	presetParam->setIsPersistant( true );
	presetParam->setEvaluateOnChange( false );//the copied values re-render; the label does not
	presetParam->setAnimates( false );
	page->addChild( *presetParam );

	//------------------------------------------------------------------ Signal
	OFX::GroupParamDescriptor* signalGroup = desc.defineGroupParam( "Signal" );
	signalGroup->setLabels( "Signal", "Signal", "Signal" );

	defineSlider( desc, page, kParamChroma, "Chroma",
	              "Whether the colour crossed the link. At 0 the colour difference bypassed "
	              "it and comes back at the amplitude it left at, so lifting a shadow "
	              "desaturates it -- one channel was companded and the colour was not. At 1 "
	              "the whole colour goes through together, which is what the video links did.",
	              defaults.chroma )
		->setParent( *signalGroup );

	//------------------------------------------------------------------ Encode
	OFX::GroupParamDescriptor* encodeGroup = desc.defineGroupParam( "Encode" );
	encodeGroup->setLabels( "Encode", "Encode", "Encode" );

	defineSlider( desc, page, kParamCompress, "Compress",
	              "Compression ratio at the transmitter, 1:1 to 4:1. The two-to-one that "
	              "almost every analogue wireless system used is at one third.",
	              defaults.compress )
		->setParent( *encodeGroup );
	defineSlider( desc, page, kParamEmphasis, "Emphasis",
	              "Pre-emphasis: how far the detail band is lifted before the compressor, so "
	              "quiet high-frequency content sits above the link's noise instead of under "
	              "it. 0 to 12 dB.", defaults.emphasis )
		->setParent( *encodeGroup );

	//-------------------------------------------------------------------- Link
	OFX::GroupParamDescriptor* linkGroup = desc.defineGroupParam( "Link" );
	linkGroup->setLabels( "Link", "Link", "Link" );

	defineSlider( desc, page, kParamLinkLevel, "Link Level",
	              "Gain into the link, plus or minus 12 dB, applied BEFORE the noise. The "
	              "noise floor stays where it is while the signal moves relative to it, so "
	              "the expander restores the picture and the hiss by different amounts and "
	              "the shadows breathe. 0.5 is an aligned pair.", defaults.linkLevel )
		->setParent( *linkGroup );
	defineSlider( desc, page, kParamNoise, "Noise",
	              "The link's own noise floor. Load-bearing rather than decoration: "
	              "companding exists to hide this, and with none of it the decode stage has "
	              "nothing to show for itself.", defaults.noise )
		->setParent( *linkGroup );
	defineSlider( desc, page, kParamHeadroom, "Headroom",
	              "Where the link clips. Runs BACKWARDS -- higher is less headroom -- so it "
	              "reads the way a drive control should.", defaults.headroom )
		->setParent( *linkGroup );

	//------------------------------------------------------------------ Decode
	OFX::GroupParamDescriptor* decodeGroup = desc.defineGroupParam( "Decode" );
	decodeGroup->setLabels( "Decode", "Decode", "Decode" );

	defineSlider( desc, page, kParamExpand, "Expand",
	              "Expansion ratio at the receiver, 1:1 to 1:4. Set it to match Compress and "
	              "the round trip nearly cancels; that is what a working link does, and "
	              "everything interesting is a departure from it.", defaults.expand )
		->setParent( *decodeGroup );
	defineSlider( desc, page, kParamTilt, "Tilt",
	              "The mismatch between the two emphasis networks. 0.5 is matched. At the "
	              "top there is no de-emphasis at all and everything the transmitter lifted "
	              "stays lifted -- hard, glassy, edges ringing. At the bottom the signal is "
	              "de-emphasised twice -- soft, smeared, detail sucked out.", defaults.tilt )
		->setParent( *decodeGroup );

	//---------------------------------------------------------------- Detector
	OFX::GroupParamDescriptor* detectorGroup = desc.defineGroupParam( "Detector" );
	detectorGroup->setLabels( "Detector", "Detector", "Detector" );

	defineSlider( desc, page, kParamTimeConstant, "Time Constant",
	              "Attack and release, 0.1 us to 40 ms -- and on a picture that is a "
	              "DISTANCE ALONG THE SCAN. Haloing tight to every edge at the bottom, a "
	              "smear off the side of things in the middle, streaks pulling down the "
	              "frame near the top. It is causal, so the smear trails to the right.",
	              defaults.timeConstant )
		->setParent( *detectorGroup );
	defineSlider( desc, page, kParamPivot, "Pivot",
	              "The level the law pivots about. Signal at the pivot passes at unity "
	              "through both ends whatever the ratio, so this is where the round trip is "
	              "exact and everything either side of it is where the character is.",
	              defaults.pivot )
		->setParent( *detectorGroup );
	defineSlider( desc, page, kParamMaxGain, "Max Gain",
	              "The most boost the compressor will apply, 0 to 48 dB. A real circuit's "
	              "boost runs out -- otherwise it would be amplifying nothing but its own "
	              "noise floor -- and this is how far into the shadows the effect reaches.",
	              defaults.maxGain )
		->setParent( *detectorGroup );

	//------------------------------------------------------------------ Output
	OFX::GroupParamDescriptor* outputGroup = desc.defineGroupParam( "Output" );
	outputGroup->setLabels( "Output", "Output", "Output" );

	defineSlider( desc, page, kParamMix, "Mix", "Wet/dry against the untouched input.",
	              defaults.mix )
		->setParent( *outputGroup );

	// The Stoatworks About block: a read-only credit line and one push button
	// per link, in a group that starts folded. Last, so it sits under the
	// effect's own controls.
	stoatworks::about::ofx::describe( desc, page );
}

OFX::ImageEffect* CompanderPluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new CompanderPlugin( handle );
}
} // namespace

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static CompanderPluginFactory* factory =
		new CompanderPluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
