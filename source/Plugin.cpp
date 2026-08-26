#include "Plugin.h"

//The SDK's umbrella FFGLSDK.h pulls in every other scoped binding but leaves
//this one out (SDK b1afaf9), so it has to be reached for by hand.
#include <ffglex/FFGLScopedFBOBinding.h>

#include "Diag.h"
#include "Shaders.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace compander;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Plugin >,// Create method
	"CM01",                 // Plugin unique ID of maximum length 4
	"Compander",            // Plugin name
	2,                      // API major version number
	1,                      // API minor version number
	0,                      // Plugin major version number
	1,                      // Plugin minor version number
	FF_EFFECT,              // Plugin type
	stoatworks::about::hook,// Plugin description
	stoatworks::about::org  // About
);

namespace
{
/// A steady wall clock, for hosts that never call SetTime and for deciding what
/// unit the ones that do are using.
double wallSeconds()
{
	using clock = std::chrono::steady_clock;
	static const clock::time_point start = clock::now();

	return std::chrono::duration< double >( clock::now() - start ).count();
}

const char* const kSidechainNames[] = { "Picture", "Audio", "Picture x Audio" };
} // namespace

Plugin::Plugin()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	const controls::HostValues d;

	//-- Defaults ---------------------------------------------------------
	//
	// ⚠️ **BEFORE any SetParamInfof call, and that ordering is load-bearing.**
	//
	// `SetParamInfof( index, name, type )` does not take a default. It reads
	// one back out of the plugin itself:
	//
	//     SetParamInfo( index, name, type, GetFloatParameter( index ) )
	//
	// So a params[] filled at the END of the constructor declares every single
	// control to the host as ZERO -- including Mix, which means the effect does
	// nothing at all when it is dragged onto a layer, and including every
	// control an operator would then have to find and set by hand. Every
	// offline test still passes, because the harness never asks the host what
	// the defaults were; `ffgltest` caught it by reporting that a frame came
	// back byte-identical to its input when the default noise floor should have
	// moved it.
	params[ PT_SIDECHAIN ]     = d.sidechain;
	params[ PT_CHROMA ]        = d.chroma;
	params[ PT_COMPRESS ]      = d.compress;
	params[ PT_EMPHASIS ]      = d.emphasis;
	params[ PT_LINK_LEVEL ]    = d.linkLevel;
	params[ PT_NOISE ]         = d.noise;
	params[ PT_HEADROOM ]      = d.headroom;
	params[ PT_EXPAND ]        = d.expand;
	params[ PT_TILT ]          = d.tilt;
	params[ PT_TIME_CONSTANT ] = d.timeConstant;
	params[ PT_PIVOT ]         = d.pivot;
	params[ PT_MAX_GAIN ]      = d.maxGain;
	params[ PT_AUDIO_AMOUNT ]  = d.audioAmount;
	params[ PT_AUDIO_BAND ]    = d.audioBand;
	params[ PT_AUDIO_TILT ]    = d.audioTilt;
	params[ PT_MIX ]           = d.mix;
	params[ PT_PRESET ]        = 0.0f;



	//-- Signal -----------------------------------------------------------
	SetOptionParamInfo( PT_SIDECHAIN, "Sidechain", kSideCount, d.sidechain );
	for( int i = 0; i < kSideCount; ++i )
		SetParamElementInfo( PT_SIDECHAIN, i, kSidechainNames[ i ], static_cast< float >( i ) );
	SetParamGroup( PT_SIDECHAIN, "Signal" );

	SetParamInfof( PT_CHROMA, "Chroma", FF_TYPE_STANDARD );
	SetParamGroup( PT_CHROMA, "Signal" );

	//-- Encode -----------------------------------------------------------
	SetParamInfof( PT_COMPRESS, "Compress", FF_TYPE_STANDARD );
	SetParamGroup( PT_COMPRESS, "Encode" );
	SetParamInfof( PT_EMPHASIS, "Emphasis", FF_TYPE_STANDARD );
	SetParamGroup( PT_EMPHASIS, "Encode" );

	//-- Link -------------------------------------------------------------
	SetParamInfof( PT_LINK_LEVEL, "Link Level", FF_TYPE_STANDARD );
	SetParamGroup( PT_LINK_LEVEL, "Link" );
	SetParamInfof( PT_NOISE, "Noise", FF_TYPE_STANDARD );
	SetParamGroup( PT_NOISE, "Link" );
	SetParamInfof( PT_HEADROOM, "Headroom", FF_TYPE_STANDARD );
	SetParamGroup( PT_HEADROOM, "Link" );

	//-- Decode -----------------------------------------------------------
	SetParamInfof( PT_EXPAND, "Expand", FF_TYPE_STANDARD );
	SetParamGroup( PT_EXPAND, "Decode" );
	SetParamInfof( PT_TILT, "Tilt", FF_TYPE_STANDARD );
	SetParamGroup( PT_TILT, "Decode" );

	//-- Detector ---------------------------------------------------------
	SetParamInfof( PT_TIME_CONSTANT, "Time Constant", FF_TYPE_STANDARD );
	SetParamGroup( PT_TIME_CONSTANT, "Detector" );
	SetParamInfof( PT_PIVOT, "Pivot", FF_TYPE_STANDARD );
	SetParamGroup( PT_PIVOT, "Detector" );
	SetParamInfof( PT_MAX_GAIN, "Max Gain", FF_TYPE_STANDARD );
	SetParamGroup( PT_MAX_GAIN, "Detector" );

	//-- Audio ------------------------------------------------------------
	//
	// The host delivers one spectrum bin per element, low frequencies first,
	// through FF_SET_PARAMETER_ELEMENT_VALUE. Resolume draws the buffer itself
	// as an audio-source picker (Local / Composition / External).
	SetBufferParamInfo( PT_AUDIO, "Audio", kAudioBins, FF_USAGE_FFT );
	for( int i = 0; i < kAudioBins; ++i )
		SetParamElementInfo( PT_AUDIO, i, "", 0.0f );
	SetParamGroup( PT_AUDIO, "Audio" );

	SetParamInfof( PT_AUDIO_AMOUNT, "Audio Amount", FF_TYPE_STANDARD );
	SetParamGroup( PT_AUDIO_AMOUNT, "Audio" );
	SetParamInfof( PT_AUDIO_BAND, "Audio Band", FF_TYPE_STANDARD );
	SetParamGroup( PT_AUDIO_BAND, "Audio" );
	SetParamInfof( PT_AUDIO_TILT, "Audio Tilt", FF_TYPE_STANDARD );
	SetParamGroup( PT_AUDIO_TILT, "Audio" );

	//-- Output -----------------------------------------------------------
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );
	SetParamGroup( PT_MIX, "Output" );

	//-- Preset -----------------------------------------------------------
	SetOptionParamInfo( PT_PRESET, "Preset", presets::kCount + 1, 0.0f );
	SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
	for( int i = 0; i < presets::kCount; ++i )
		SetParamElementInfo( PT_PRESET, i + 1, presets::kPresets[ i ].name,
		                     static_cast< float >( i + 1 ) );
	SetParamGroup( PT_PRESET, "Preset" );

	//-- About ------------------------------------------------------------
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
	for( const auto& b : stoatworks::about::buttons() )
		SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );

	audioLevel.fill( 0.0f );

	SetTimeSupported( true );
}

//===========================================================================
// GL
//===========================================================================

FFResult Plugin::InitGL( const FFGLViewportStruct* viewport )
{
	diag::init();

	struct Build
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	};

	const Build builds[] = {
		{ &lumaShader, shaders::kLumaFragment, "luma" },
		{ &blurShader, shaders::kBlurFragment, "blur" },
		{ &reduceShader, shaders::kReduceFragment, "reduce" },
		{ &scanShader, shaders::kScanFragment, "scan" },
		{ &frameShader, shaders::kFrameFragment, "frame" },
		{ &encodeShader, shaders::EncodeFragment(), "encode" },
		{ &decodeShader, shaders::DecodeFragment(), "decode" },
		{ &outputShader, shaders::OutputFragment(), "output" },
	};

	for( const Build& b : builds )
	{
		// Idempotent. A host calls InitGL once, but the offline harness calls
		// it every frame so that one run can render at several sizes without
		// tearing the GL resources down -- and recompiling eight programs a
		// frame put the floor at twenty milliseconds and made `--bench` report
		// the same figure for two passes and for fourteen, which is what gave
		// it away.
		if( b.shader->IsReady() )
			continue;

		if( !b.shader->Compile( shaders::kVertex, b.fragment ) )
		{
			// A shader that will not compile presents as "the effect does
			// nothing", with no message anywhere the operator can see it. The
			// log is the only place this surfaces.
			diag::shaderFailed( b.name, b.fragment );

			// The GL strings, from here rather than from Diag, which is kept
			// GL-free for the OpenFX build. On a machine where a shader will
			// not build, which GL this is matters more than what the shader
			// says.
			auto glString = []( GLenum name ) -> std::string {
				const GLubyte* s = glGetString( name );
				return s != nullptr ? reinterpret_cast< const char* >( s ) : "?";
			};
			diag::info( "  vendor:   " + glString( GL_VENDOR ) );
			diag::info( "  renderer: " + glString( GL_RENDERER ) );
			diag::info( "  version:  " + glString( GL_VERSION ) );

			FFGLLog::LogToHost( "Compander: shader failed to compile" );
			return FF_FAIL;
		}
	}

	// FFGLScreenQuad has no way to ask whether it is already initialised, so
	// that is tracked here. Initialising it twice leaks a VAO and two buffers
	// per call, which on the harness's per-frame InitGL is a leak per frame.
	if( quadReady )
		return CFFGLPlugin::InitGL( viewport );

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Compander: quad geometry failed to initialise" );
		return FF_FAIL;
	}

	quadReady  = true;
	frameFirst = true;

	return CFFGLPlugin::InitGL( viewport );
}

bool Plugin::EnsureBuffers( GLsizei width, GLsizei height )
{
	const GLsizei detectorWidth = std::max< GLsizei >( 1, width / 4 );

	// R16F throughout. The chain is one signal, it goes well outside 0..1 in
	// the middle -- pre-emphasis lifts edges above white before the compressor
	// pulls them back - and an 8-bit buffer would clip exactly the excursion
	// the effect exists to model.
	const bool ok =
	    lumaBuffer.Ensure( width, height, GL_R16F ) &&
	    blurBuffer[ 0 ].Ensure( width, height, GL_R16F ) &&
	    blurBuffer[ 1 ].Ensure( width, height, GL_R16F ) &&
	    encodedBuffer.Ensure( width, height, GL_R16F ) &&
	    decodedBuffer.Ensure( width, height, GL_R16F ) &&
	    detectorBuffer[ 0 ].Ensure( detectorWidth, height, GL_R16F ) &&
	    detectorBuffer[ 1 ].Ensure( detectorWidth, height, GL_R16F ) &&
	    frameBuffer[ 0 ].Ensure( 1, 1, GL_R16F ) &&
	    frameBuffer[ 1 ].Ensure( 1, 1, GL_R16F );

	if( !ok )
	{
		diag::allocationFailed( width, height );
		return false;
	}

	if( width != bufferWidth || height != bufferHeight )
	{
		bufferWidth  = width;
		bufferHeight = height;

		// The frame follower's previous value describes a different picture
		// now. Releasing towards it would open with a gain excursion nobody
		// asked for.
		frameFirst = true;
	}

	return true;
}

Settings Plugin::CurrentSettings() const
{
	controls::HostValues v;

	v.sidechain    = params[ PT_SIDECHAIN ];
	v.chroma       = params[ PT_CHROMA ];
	v.compress     = params[ PT_COMPRESS ];
	v.emphasis     = params[ PT_EMPHASIS ];
	v.linkLevel    = params[ PT_LINK_LEVEL ];
	v.noise        = params[ PT_NOISE ];
	v.headroom     = params[ PT_HEADROOM ];
	v.expand       = params[ PT_EXPAND ];
	v.tilt         = params[ PT_TILT ];
	v.timeConstant = params[ PT_TIME_CONSTANT ];
	v.pivot        = params[ PT_PIVOT ];
	v.maxGain      = params[ PT_MAX_GAIN ];
	v.audioAmount  = params[ PT_AUDIO_AMOUNT ];
	v.audioBand    = params[ PT_AUDIO_BAND ];
	v.audioTilt    = params[ PT_AUDIO_TILT ];
	v.mix          = params[ PT_MIX ];

	return controls::toSettings( v );
}

FFResult Plugin::ProcessOpenGL( ProcessOpenGLStruct* input )
{
	if( input == nullptr || input->numInputTextures < 1 || input->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& source = *input->inputTextures[ 0 ];

	// ScopedFBOBinding does not restore the viewport. Capture the host's before
	// anything changes it and put it back before the composite.
	GLint hostViewport[ 4 ] = {};
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	// Every Ensure() before anything is bound: allocating a buffer unbinds
	// whatever is on the active texture unit. See PassBuffer.h.
	if( !EnsureBuffers( source.Width, source.Height ) )
		return FF_FAIL;

	const double dt = AdvanceClock();
	UpdateAudio();

	const Settings settings = CurrentSettings();

	const float tauSamples = timeConstantSamples( settings.timeConstantUs, bufferWidth );
	const int   detWidth   = std::max( 1, bufferWidth / 4 );
	const ScanPlan plan    = planScan( tauSamples, bufferWidth, detWidth );
	const Crossover cross  = crossoverFor( bufferWidth );

	float compressTable[ kGainTableSize ] = {};
	float expandTable[ kGainTableSize ]   = {};
	fillGainTables( settings, compressTable, expandTable );

	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( source );

	//-- 1. luma ----------------------------------------------------------
	{
		ScopedFBOBinding fbo( lumaBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		glViewport( 0, 0, bufferWidth, bufferHeight );

		ScopedShaderBinding shader( lumaShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		ScopedTextureBinding texture( GL_TEXTURE_2D, source.Handle );

		lumaShader.Set( "Source", 0 );
		lumaShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		quad.Draw();
	}

	//-- 2. the band split of the INPUT -----------------------------------
	//
	// Everything downstream of the luma pass reads an unpadded buffer of our
	// own, so MaxUV is 1,1 from here on. Passing the host's would sample a
	// fraction of the picture.
	auto blurPass = [ & ]( PassBuffer& dst, GLuint src, float radius ) {
		ScopedFBOBinding fbo( dst.GetGLID(), ScopedFBOBinding::RB_REVERT );
		glViewport( 0, 0, bufferWidth, bufferHeight );

		ScopedShaderBinding shader( blurShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		ScopedTextureBinding texture( GL_TEXTURE_2D, src );

		blurShader.Set( "Source", 0 );
		blurShader.Set( "MaxUV", 1.0f, 1.0f );
		blurShader.Set( "TexelSize", 1.0f / float( bufferWidth ), 1.0f / float( bufferHeight ) );
		blurShader.Set( "Radius", radius );
		quad.Draw();
	};

	blurPass( blurBuffer[ 0 ], lumaBuffer.GetTextureInfo().Handle, cross.narrow );
	blurPass( blurBuffer[ 1 ], lumaBuffer.GetTextureInfo().Handle, cross.wide );

	//-- 3. the detector --------------------------------------------------
	//
	// Reduce to quarter width, then the doubling passes. Returns which of the
	// two detector buffers holds the answer.
	auto runDetector = [ & ]( GLuint src ) -> int {
		{
			ScopedFBOBinding fbo( detectorBuffer[ 0 ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			glViewport( 0, 0, detWidth, bufferHeight );

			ScopedShaderBinding shader( reduceShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			ScopedTextureBinding texture( GL_TEXTURE_2D, src );

			reduceShader.Set( "Source", 0 );
			reduceShader.Set( "MaxUV", 1.0f, 1.0f );
			quad.Draw();
		}

		int from = 0;
		for( const ScanPass& pass : plan.passes )
		{
			const int to = 1 - from;

			ScopedFBOBinding fbo( detectorBuffer[ to ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			glViewport( 0, 0, detWidth, bufferHeight );

			ScopedShaderBinding shader( scanShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			ScopedTextureBinding texture( GL_TEXTURE_2D, detectorBuffer[ from ].GetTextureInfo().Handle );

			scanShader.Set( "Source", 0 );
			scanShader.Set( "MaxUV", 1.0f, 1.0f );
			scanShader.Set( "Offset", pass.offset );
			scanShader.Set( "Decay", pass.decay );
			quad.Draw();

			from = to;
		}

		return from;
	};

	const int encodeEnv = runDetector( lumaBuffer.GetTextureInfo().Handle );

	//-- 4. the frame-global follower -------------------------------------
	{
		const int prev = frameSlot;
		frameSlot      = 1 - frameSlot;

		const float tauSeconds = samplesToSeconds( tauSamples, bufferWidth, bufferHeight );
		const float decay      = std::exp( -static_cast< float >( std::max( dt, 0.0 ) ) /
		                                   std::max( tauSeconds, 1.0e-4f ) );

		ScopedFBOBinding fbo( frameBuffer[ frameSlot ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		glViewport( 0, 0, 1, 1 );

		ScopedShaderBinding shader( frameShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, detectorBuffer[ encodeEnv ].GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, frameBuffer[ prev ].GetTextureInfo().Handle );

		frameShader.Set( "Envelope", 0 );
		frameShader.Set( "Previous", 1 );
		frameShader.Set( "MaxUV", 1.0f, 1.0f );
		frameShader.Set( "Decay", decay );
		frameShader.Set( "First", frameFirst ? 1 : 0 );
		quad.Draw();

		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, 0 );

		frameFirst = false;
	}

	//-- 5. encode --------------------------------------------------------
	{
		ScopedFBOBinding fbo( encodedBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		glViewport( 0, 0, bufferWidth, bufferHeight );

		ScopedShaderBinding shader( encodeShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, lumaBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, blurBuffer[ 0 ].GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, blurBuffer[ 1 ].GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE3 );
		glBindTexture( GL_TEXTURE_2D, detectorBuffer[ encodeEnv ].GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE4 );
		glBindTexture( GL_TEXTURE_2D, frameBuffer[ frameSlot ].GetTextureInfo().Handle );

		encodeShader.Set( "Luma", 0 );
		encodeShader.Set( "BlurNarrow", 1 );
		encodeShader.Set( "BlurWide", 2 );
		encodeShader.Set( "Envelope", 3 );
		encodeShader.Set( "FrameLevel", 4 );
		encodeShader.Set( "MaxUV", 1.0f, 1.0f );
		encodeShader.Set( "EmphasisDb", settings.emphasisDb );
		encodeShader.Set( "LinkGain", std::pow( 10.0f, settings.linkLevelDb / 20.0f ) );
		encodeShader.Set( "Headroom", settings.headroom );
		encodeShader.Set( "Noise", settings.noise );
		encodeShader.Set( "FrameBlend", plan.frameBlend );
		encodeShader.Set( "AudioEnv", audioEnvelope );
		encodeShader.Set( "AudioAmount", settings.audioAmount );
		encodeShader.Set( "SidechainMode", settings.sidechain );
		encodeShader.Set( "Frame", frameCounter );

		glUniform1fv( glGetUniformLocation( encodeShader.GetGLID(), "CompressTable" ),
		              kGainTableSize, compressTable );
		glUniform1fv( glGetUniformLocation( encodeShader.GetGLID(), "ExpandTable" ),
		              kGainTableSize, expandTable );

		quad.Draw();

		for( int i = 4; i >= 0; --i )
		{
			glActiveTexture( GL_TEXTURE0 + i );
			glBindTexture( GL_TEXTURE_2D, 0 );
		}
	}

	//-- 6. the SECOND detector, on the encoded signal --------------------
	const int decodeEnv = runDetector( encodedBuffer.GetTextureInfo().Handle );

	//-- 7. decode --------------------------------------------------------
	{
		ScopedFBOBinding fbo( decodedBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		glViewport( 0, 0, bufferWidth, bufferHeight );

		ScopedShaderBinding shader( decodeShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, encodedBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, detectorBuffer[ decodeEnv ].GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, frameBuffer[ frameSlot ].GetTextureInfo().Handle );

		decodeShader.Set( "Encoded", 0 );
		decodeShader.Set( "Envelope", 1 );
		decodeShader.Set( "FrameLevel", 2 );
		decodeShader.Set( "MaxUV", 1.0f, 1.0f );
		decodeShader.Set( "FrameBlend", plan.frameBlend );

		glUniform1fv( glGetUniformLocation( decodeShader.GetGLID(), "CompressTable" ),
		              kGainTableSize, compressTable );
		glUniform1fv( glGetUniformLocation( decodeShader.GetGLID(), "ExpandTable" ),
		              kGainTableSize, expandTable );

		quad.Draw();

		for( int i = 2; i >= 0; --i )
		{
			glActiveTexture( GL_TEXTURE0 + i );
			glBindTexture( GL_TEXTURE_2D, 0 );
		}
	}

	//-- 8. the band split of the DECODED signal --------------------------
	//
	// The input's split is finished with, so the same two buffers are reused.
	blurPass( blurBuffer[ 0 ], decodedBuffer.GetTextureInfo().Handle, cross.narrow );
	blurPass( blurBuffer[ 1 ], decodedBuffer.GetTextureInfo().Handle, cross.wide );

	//-- 9. output --------------------------------------------------------
	{
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( outputShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, source.Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, lumaBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, decodedBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE3 );
		glBindTexture( GL_TEXTURE_2D, blurBuffer[ 0 ].GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE4 );
		glBindTexture( GL_TEXTURE_2D, blurBuffer[ 1 ].GetTextureInfo().Handle );

		outputShader.Set( "Source", 0 );
		outputShader.Set( "Luma", 1 );
		outputShader.Set( "Decoded", 2 );
		outputShader.Set( "BlurNarrow", 3 );
		outputShader.Set( "BlurWide", 4 );

		// The SOURCE is the host's padded texture, so this pass needs the
		// host's MaxUV -- unlike every pass between the luma one and here.
		outputShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		outputShader.Set( "EmphasisDb", settings.emphasisDb );

		// Audio Tilt rides on top of the Tilt control rather than replacing it,
		// so an operator who has set a tilt keeps it and the music moves it.
		outputShader.Set( "Tilt", std::clamp( settings.tilt + settings.audioTilt * audioBalance,
		                                      -1.0f, 1.0f ) );
		outputShader.Set( "Chroma", settings.chroma );
		outputShader.Set( "Mix", settings.mix );

		quad.Draw();

		for( int i = 4; i >= 0; --i )
		{
			glActiveTexture( GL_TEXTURE0 + i );
			glBindTexture( GL_TEXTURE_2D, 0 );
		}
	}

	frameCounter = ( frameCounter + 1 ) & 0x00ffffff;

	return FF_SUCCESS;
}

FFResult Plugin::DeInitGL()
{
	lumaBuffer.Destroy();
	blurBuffer[ 0 ].Destroy();
	blurBuffer[ 1 ].Destroy();
	encodedBuffer.Destroy();
	decodedBuffer.Destroy();
	detectorBuffer[ 0 ].Destroy();
	detectorBuffer[ 1 ].Destroy();
	frameBuffer[ 0 ].Destroy();
	frameBuffer[ 1 ].Destroy();

	lumaShader.FreeGLResources();
	blurShader.FreeGLResources();
	reduceShader.FreeGLResources();
	scanShader.FreeGLResources();
	frameShader.FreeGLResources();
	encodeShader.FreeGLResources();
	decodeShader.FreeGLResources();
	outputShader.FreeGLResources();

	quad.Release();
	quadReady = false;

	bufferWidth  = 0;
	bufferHeight = 0;

	return FF_SUCCESS;
}

//===========================================================================
// Clock
//===========================================================================

FFResult Plugin::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

void Plugin::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}

double Plugin::AdvanceClock()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	const double raw = hostTime;

	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		// A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			// Several frames rather than one, so a single odd frame cannot
			// decide it alone.
			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}

	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	const double now = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale
	                                                       : wallNow - wallStart;

	const double dt = ( lastNow >= 0.0 && now > lastNow ) ? now - lastNow : 0.0;
	lastNow         = now;

	return dt;
}

//===========================================================================
// Audio
//===========================================================================

void Plugin::UpdateAudio()
{
	ParamInfo* info = FindParamInfo( PT_AUDIO );
	if( info == nullptr )
		return;

	const double now = lastNow;
	const double dt  = ( audioClock >= 0.0 && now > audioClock ) ? now - audioClock : 0.0;
	audioClock       = now;

	// Fast up, slow down. A flash that arrives a frame late reads as broken; one
	// that takes about a sixth of a second to die away reads as intended.
	const float release = dt > 0.0 ? 1.0f - std::exp( static_cast< float >( -dt / 0.15 ) ) : 1.0f;

	const size_t bins = std::min( info->elements.size(), audioLevel.size() );
	for( size_t i = 0; i < bins; ++i )
	{
		// sqrt because bin magnitudes bunch near zero: a spectrum used raw
		// responds to nothing but the kick drum.
		const float raw = std::sqrt( std::max( 0.0f, info->elements[ i ].value ) );

		if( raw >= audioLevel[ i ] )
			audioLevel[ i ] = raw;
		else
			audioLevel[ i ] += ( raw - audioLevel[ i ] ) * release;
	}

	// The band-weighted envelope the compressor's sidechain sees.
	const float band = std::clamp( params[ PT_AUDIO_BAND ], 0.0f, 1.0f );

	float sum    = 0.0f;
	float weight = 0.0f;
	float low    = 0.0f;
	float high   = 0.0f;

	for( int i = 0; i < static_cast< int >( bins ); ++i )
	{
		const float w = audioBandWeight( i, static_cast< int >( bins ), band );
		sum += w * audioLevel[ i ];
		weight += w;

		// The spectral balance, for Audio Tilt: the top half of the spectrum
		// against the bottom half, which is a crude brightness measure and is
		// all that control needs.
		if( i < static_cast< int >( bins ) / 2 )
			low += audioLevel[ i ];
		else
			high += audioLevel[ i ];
	}

	audioEnvelope = weight > 0.0f ? sum / weight : 0.0f;

	const float total = low + high;
	audioBalance      = total > 1.0e-4f ? ( high - low ) / total : 0.0f;
}

//===========================================================================
// Parameters
//===========================================================================

float Plugin::presetValue( int presetIndex, unsigned int id ) const
{
	if( presetIndex <= 0 || presetIndex > presets::kCount )
		return -1.0f;

	const presets::Preset& p = presets::kPresets[ presetIndex - 1 ];

	for( int i = 0; i < presets::kParamCount; ++i )
		if( kPresetParamIDs[ i ] == id )
			return p.v[ i ];

	return -1.0f;
}

void Plugin::seedHostValues()
{
	if( hostValuesSeeded )
		return;

	// ⚠️ Before anything can have moved them, and before the PT_PRESET branch
	// in SetFloatParameter can run. Seeding lazily from params[] inside that
	// guard would record the PRESET's values as the host's opening position,
	// so the host's very next restatement would look like an edit.
	for( unsigned int i = 0; i < PT_COUNT; ++i )
		hostValues[ i ] = params[ i ];

	hostValuesSeeded = true;
}

bool Plugin::hostIsRestatingItself( unsigned int index, float value )
{
	const int active = static_cast< int >( std::lround( params[ PT_PRESET ] ) );
	if( active <= 0 )
		return false;

	const float covered = presetValue( active, index );
	if( covered < 0.0f )
		return false;

	// Two different tolerances on purpose.
	//
	// 1e-3 here is a HOST QUANTISATION allowance: Resolume rounds what it
	// echoes to about a thousandth, and 1e-4 read that rounding as an edit.
	//
	// A value matching the preset must be IGNORED, not written -- writing the
	// host's rounded copy of our own value is what trips the tighter test
	// below on the following frame.
	if( std::fabs( value - covered ) < 1.0e-3f )
		return true;

	// It differs from the preset. It is only the host restating itself if it
	// also matches what the host last said; otherwise the operator moved it.
	return std::fabs( value - hostValues[ index ] ) < 1.0e-4f;
}

void Plugin::applyPreset( int presetIndex )
{
	if( presetIndex <= 0 || presetIndex > presets::kCount )
		return;

	const presets::Preset& p = presets::kPresets[ presetIndex - 1 ];

	// Written into params[] so a host that DOES honour value events agrees with
	// one that does not. Nothing downstream depends on the host acting on them.
	for( int i = 0; i < presets::kParamCount; ++i )
	{
		const unsigned int id = kPresetParamIDs[ i ];

		params[ id ] = p.v[ i ];
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
	}

	diag::info( std::string( "preset applied: " ) + p.name );
}

FFResult Plugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	seedHostValues();

	if( index == PT_PRESET )
	{
		const int chosen = static_cast< int >( std::lround( value ) );

		params[ PT_PRESET ] = static_cast< float >( chosen );
		applyPreset( chosen );

		hostValues[ index ] = value;
		return FF_SUCCESS;
	}

	if( hostIsRestatingItself( index, value ) )
	{
		// Record what the host said, and render with what the preset says.
		hostValues[ index ] = value;
		return FF_SUCCESS;
	}

	// A real edit to a covered parameter drops the preset to Custom.
	const int active = static_cast< int >( std::lround( params[ PT_PRESET ] ) );
	if( active > 0 && presetValue( active, index ) >= 0.0f )
	{
		// Logged because diagnosing the vertigo bug needed a code read, exactly
		// as the host-clock units did for orrery.
		diag::info( "preset dropped to Custom: parameter " + std::to_string( index ) +
		            " moved to " + std::to_string( value ) );
		params[ PT_PRESET ] = 0.0f;
		RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );
	}

	params[ index ]     = value;
	hostValues[ index ] = value;

	return FF_SUCCESS;
}

float Plugin::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

char* Plugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return nullptr;
}

FFResult Plugin::SetTextParameter( unsigned int index, const char* value )
{
	( void )value;

	// LOAD-BEARING, and its absence is invisible offline. instantiateGL pushes
	// every declared default back through the setters and deletes the instance
	// the moment one returns FF_FAIL -- which is what CFFGLPlugin's stub does.
	// Omit this and the plugin cannot be created in any real host while every
	// in-repo harness still passes.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return FF_FAIL;
}
