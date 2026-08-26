#pragma once

#include <FFGLSDK.h>

#include "Chain.h"
#include "Compander.h"
#include "Controls.h"
#include "Detector.h"
#include "PassBuffer.h"
#include "Presets.h"
#include "StoatworksAbout.h"
#include "StoatworksAboutParams.h"

#include <array>
#include <string>

namespace compander
{
/**
    The FFGL plugin: the chain in `Compander.h`, wired to a host.

    Everything about *what* this does is in `Compander.h`, `Chain.h` and
    `Detector.h`. This file is the glue -- parameters, buffers, and the order
    the passes run in.

    ------------------------------------------------------------- the buffers

    Five full-resolution single-channel buffers, two at quarter width, and two
    single texels. The chain is a signal, so from the luma pass to the output
    pass everything is one channel; only the first and last passes ever see a
    colour.

    Buffers are reused where a stage is finished with one, and the reuse is not
    an optimisation to be undone lightly: `blur[ 0 ]` and `blur[ 1 ]` hold the
    band split of the INPUT while the encode pass runs, and the band split of
    the DECODED signal while the output pass runs. Those two never overlap.

    ------------------------------------------------------------- the presets

    The preset is an OVERRIDE, not a write. Resolume owns parameter state and
    does not consume the value events a copy-based apply raises, so it carries
    on pushing the values it still believes in -- and a plugin that treats a
    covered parameter changing as "the operator has taken over" fires on the
    host's own echo and drops back to Custom immediately. Reported against
    vertigo as its issue #2, after the pattern had been copied into seven
    plugins.

    So `hostValues[]` records what the host last SENT, separately from what the
    plugin renders with, and a restatement that matches what the host already
    said is ignored rather than written. Judged by what the value **is**, never
    by the fact that it changed.

    ⚠️ `seedHostValues()` must run BEFORE `applyPreset` can. Seeding lazily from
    `params[]` inside the guard records the preset's own values as the host's
    opening position, so the host's very next restatement looks like an edit and
    the bug comes straight back.
*/
class Plugin : public CFFGLPlugin
{
public:
	Plugin();

	FFResult InitGL( const FFGLViewportStruct* viewport ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* input ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	char* GetTextParameter( unsigned int index ) override;
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	FFResult SetTime( double time ) override;

	/// Drive the clock from a test harness rather than from a host. See the
	/// unit detection in `ProcessOpenGL`.
	void SetClockScaleForTest( double scale );

	enum ParamID : FFUInt32
	{
		//Signal
		PT_SIDECHAIN,
		PT_CHROMA,

		//Encode
		PT_COMPRESS,
		PT_EMPHASIS,

		//Link
		PT_LINK_LEVEL,
		PT_NOISE,
		PT_HEADROOM,

		//Decode
		PT_EXPAND,
		PT_TILT,

		//Detector
		PT_TIME_CONSTANT,
		PT_PIVOT,
		PT_MAX_GAIN,

		//Audio. PT_AUDIO is an FFT buffer (FF_TYPE_BUFFER, FF_USAGE_FFT):
		//Resolume shows it as an audio-source picker and writes one spectrum
		//bin per element, low frequencies first.
		PT_AUDIO,
		PT_AUDIO_AMOUNT,
		PT_AUDIO_BAND,
		PT_AUDIO_TILT,

		//Output
		PT_MIX,

		//Preset. Declared after the real controls so that adding a preset, or
		//changing what one covers, cannot shift a ParamID that a saved
		//composition refers to.
		PT_PRESET,

		//About. FFGL has no window and cannot make one, so the name, the
		//version, the maker and the links are parameters the host draws with
		//everything else. Last in the enum for the same reason as PT_PRESET.
		//See StoatworksAboutParams.h.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// Spectrum bins asked of the host. Resolume's own analysis is 64 wide.
	static constexpr int kAudioBins = 64;

private:
	/// The ParamID each presets::Param drives, in presets::Param order. The
	/// preset table stays host-agnostic; this is the FFGL binding of it.
	static constexpr unsigned int kPresetParamIDs[ presets::kParamCount ] = {
		PT_CHROMA, PT_COMPRESS, PT_EMPHASIS, PT_LINK_LEVEL, PT_NOISE,
		PT_HEADROOM, PT_EXPAND, PT_TILT, PT_TIME_CONSTANT, PT_PIVOT, PT_MAX_GAIN
	};

	//-- Presets ----------------------------------------------------------

	/// The active preset's value for `id`, or -1 when no preset is active or
	/// this one has no opinion about `id`. Preset values are all 0..1, so a
	/// negative is unambiguous.
	float presetValue( int presetIndex, unsigned int id ) const;

	/// True when this write is the HOST restating a value it still believes in
	/// rather than the operator moving anything -- in which case it must not
	/// reach `params[]` and must not disturb the preset.
	bool hostIsRestatingItself( unsigned int index, float value );

	/// Record the defaults as the host's opening position, once, before
	/// anything has had a chance to move them.
	void seedHostValues();

	void applyPreset( int presetIndex );

	//-- Render -----------------------------------------------------------

	/// Gather the host's parameters into physical units.
	Settings CurrentSettings() const;

	/// Allocate every buffer for this frame's size. Called before anything is
	/// bound -- see PassBuffer.h for why that ordering is load-bearing.
	bool EnsureBuffers( GLsizei width, GLsizei height );

	void UpdateAudio();

	/// Advance the host clock, deciding its unit if that is still open.
	/// Returns the frame delta in seconds.
	double AdvanceClock();

	//-- GL ---------------------------------------------------------------

	ffglex::FFGLShader lumaShader;
	ffglex::FFGLShader blurShader;
	ffglex::FFGLShader reduceShader;
	ffglex::FFGLShader scanShader;
	ffglex::FFGLShader frameShader;
	ffglex::FFGLShader encodeShader;
	ffglex::FFGLShader decodeShader;
	ffglex::FFGLShader outputShader;

	ffglex::FFGLScreenQuad quad;

	/// FFGLScreenQuad cannot be asked whether it is already initialised, and
	/// initialising it twice leaks a VAO and two buffers.
	bool quadReady = false;

	PassBuffer lumaBuffer;
	PassBuffer blurBuffer[ 2 ];
	PassBuffer encodedBuffer;
	PassBuffer decodedBuffer;
	PassBuffer detectorBuffer[ 2 ];

	/// The frame-global follower, ping-ponged across frames. `frameSlot` is the
	/// one written this frame; the other holds last frame's value.
	PassBuffer frameBuffer[ 2 ];
	int  frameSlot  = 0;
	bool frameFirst = true;

	int bufferWidth  = 0;
	int bufferHeight = 0;

	/// Counts frames, for the noise. Wrapped rather than allowed to overflow:
	/// the hash takes a uint and a signed overflow is undefined.
	int frameCounter = 0;

	//-- Clock ------------------------------------------------------------
	//
	// The FFGL header never says what unit SetTime is in, and hosts disagree:
	// Resolume hands over MILLISECONDS (measured live: 20.0 per frame at its
	// 50 fps, and the SDK's own Particles sample divides by 1000), while the
	// offline harness -- and any host following the header's silence -- sends
	// seconds. Decide from several plausible frame deltas against the wall
	// clock and stick.
	//
	// Until it is settled, run on the wall clock: wrong in origin but right in
	// rate, where assuming seconds would be a thousand times fast on Resolume.
	//---------------------------------------------------------------------
	double hostTime     = -1.0;
	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double lastRawTime  = -1.0;
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	double lastNow      = -1.0;
	int    secondsVotes = 0;
	int    millisVotes  = 0;

	static constexpr int kClockVotes = 4;

	//-- Audio ------------------------------------------------------------

	/// One smoothed level per spectrum bin. Instant attack, ~150 ms release --
	/// a flash that arrives a frame late reads as broken, one that takes a
	/// sixth of a second to die away reads as intended.
	std::array< float, kAudioBins > audioLevel = {};
	double audioClock = -1.0;

	/// The band-weighted envelope handed to the shader, and the spectral
	/// balance driving Audio Tilt.
	float audioEnvelope = 1.0f;
	float audioBalance  = 0.0f;

	//-- Host state -------------------------------------------------------

	float params[ PT_COUNT ] = {};

	/// What the HOST last sent for each parameter, which is not the same thing
	/// as what the plugin is rendering with. See the class comment.
	float hostValues[ PT_COUNT ] = {};
	bool  hostValuesSeeded       = false;

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};

} // namespace compander
