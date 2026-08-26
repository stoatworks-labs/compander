#include "../Shaders.h"

#include <string>

namespace compander::shaders
{
const char* const kLumaFragment = R"(#version 410 core
uniform sampler2D Source;
in vec2 uv;
out vec4 fragColor;

float lumaOf( vec3 rgb )
{
	return 0.2126 * rgb.r + 0.7152 * rgb.g + 0.0722 * rgb.b;
}

void main()
{
	fragColor = vec4( lumaOf( texture( Source, uv ).rgb ), 0.0, 0.0, 1.0 );
}
)";

/**
    One horizontal blur, for the band split.

    **Horizontal and nothing else.** There is no vertical pass anywhere in this
    plugin and adding one would not be an improvement, it would be a different
    effect: a link carries one signal with one frequency axis, and on a picture
    that axis is the scan. See `Chain.h`.

    Thirteen taps over two sigma with weights evaluated in the shader rather
    than baked, because the radius changes with the picture's width and a baked
    table would have to be re-uploaded on every resize to say the same thing.
*/
const char* const kBlurFragment = R"(#version 410 core
uniform sampler2D Source;
uniform vec2  TexelSize;
uniform float Radius;      // sigma, in texels along the scan

in vec2 uv;
out vec4 fragColor;

void main()
{
	float sigma = max( Radius, 0.5 );
	float step  = sigma / 3.0;

	float sum    = 0.0;
	float weight = 0.0;

	for( int i = -6; i <= 6; ++i )
	{
		float d = float( i ) * step;
		float w = exp( -0.5 * ( d * d ) / ( sigma * sigma ) );

		sum    += w * texture( Source, uv + vec2( d * TexelSize.x, 0.0 ) ).r;
		weight += w;
	}

	fragColor = vec4( sum / weight, 0.0, 0.0, 1.0 );
}
)";

/**
    Luma to quarter width.

    A MAX of the four, not a mean. This feeds a peak detector, and a peak of
    averages is not an average of peaks -- averaging first rounds the top off
    every highlight before the detector ever sees it.
*/
const char* const kReduceFragment = R"(#version 410 core
uniform sampler2D Source;
in vec2 uv;
out vec4 fragColor;

void main()
{
	ivec2 p    = ivec2( gl_FragCoord.xy );
	ivec2 size = textureSize( Source, 0 );

	float m = 0.0;
	for( int i = 0; i < 4; ++i )
	{
		int sx = min( p.x * 4 + i, size.x - 1 );
		m      = max( m, texelFetch( Source, ivec2( sx, p.y ), 0 ).r );
	}

	fragColor = vec4( m, 0.0, 0.0, 1.0 );
}
)";

/**
    One recursive-doubling pass of the scan detector.

    `E(n) = max( E(n), decay * E(n - offset) )`, where `n` is the index along
    the SERIAL scan -- so an offset past the left edge becomes the right-hand
    end of the line above, and an offset past a whole line becomes a vertical
    one. Run with offset doubling each time, this is the exact peak detector
    over a window of `2^passes`, not an approximation of it. See `Detector.h`.

    ⚠️ **The scan runs top to bottom and the texture does not.** FFGL hands over
    bottom-up textures, so the line index is `height - 1 - y`. Getting this
    backwards makes the smear trail upwards out of the top of bright objects,
    which looks deliberate and is exactly wrong: the detector can only know what
    has already gone past.
*/
const char* const kScanFragment = R"(#version 410 core
uniform sampler2D Source;
uniform int   Offset;
uniform float Decay;

out vec4 fragColor;

void main()
{
	ivec2 p    = ivec2( gl_FragCoord.xy );
	ivec2 size = textureSize( Source, 0 );

	int line = size.y - 1 - p.y;
	int n    = line * size.x + p.x;
	int m    = max( n - Offset, 0 );

	int my = m / size.x;
	int mx = m - my * size.x;

	float here   = texelFetch( Source, p, 0 ).r;
	float behind = texelFetch( Source, ivec2( mx, size.y - 1 - my ), 0 ).r;

	fragColor = vec4( max( here, Decay * behind ), 0.0, 0.0, 1.0 );
}
)";

/**
    The frame-global follower: one value for the whole picture, one-poled across
    frames.

    Renders to a 1x1 buffer, reading the previous frame's 1x1 buffer. Ping-pong
    rather than a read-back: pulling one pixel off the GPU every frame is a
    pipeline stall for a number nothing on the CPU needs.

    Instant attack and exponential release, stepped by the frame's real
    duration, so the time constant is in seconds. A host that drops frames gets
    the right decay in seconds rather than the right decay per call -- the
    difference between a pump that stays in time with the music and one that
    speeds up when the machine is busy.
*/
const char* const kFrameFragment = R"(#version 410 core
uniform sampler2D Envelope;   // the scan detector's output
uniform sampler2D Previous;   // 1x1, last frame's value
uniform float     Decay;      // exp( -dt / tau ), precomputed on the CPU
uniform int       First;      // 1 on the first frame and after a resize

out vec4 fragColor;

void main()
{
	// An 8x8 grid of the envelope, taking the peak.
	//
	// ⚠️ Not a mipmap. glGenerateMipmap would be the obvious reduction and it
	// would need the FBO's colour texture to have been given mip storage and a
	// mipmapping min filter, which `ffglex::FFGLFBO` does not do -- so the
	// obvious version samples level 0 forever and the whole frame-scale range
	// of Time Constant quietly stops working. Sixty-four taps is cheap at 1x1
	// and depends on nothing.
	//
	// A peak rather than a mean, for the same reason the width reduction takes
	// a max: this feeds a peak detector.
	float level = 0.0;
	for( int y = 0; y < 8; ++y )
		for( int x = 0; x < 8; ++x )
			level = max( level, texture( Envelope, ( vec2( x, y ) + 0.5 ) / 8.0 ).r );

	float prev = texelFetch( Previous, ivec2( 0, 0 ), 0 ).r;

	// First frame takes the level rather than releasing towards it from
	// nothing, or the effect opens with a gain excursion that has nothing to do
	// with the footage.
	float value = ( First == 1 || level >= prev )
	                  ? level
	                  : level + ( prev - level ) * Decay;

	fragColor = vec4( value, 0.0, 0.0, 1.0 );
}
)";

const char* const kEncodeFragment = R"(#version 410 core
uniform sampler2D Luma;
uniform sampler2D BlurNarrow;
uniform sampler2D BlurWide;
uniform sampler2D Envelope;      // scan detector, quarter width
uniform sampler2D FrameLevel;    // 1x1 frame follower

uniform float EmphasisDb;
uniform float LinkGain;
uniform float Headroom;
uniform float Noise;
uniform float FrameBlend;
uniform float AudioEnv;
uniform float AudioAmount;
uniform int   SidechainMode;
uniform int   Frame;

in vec2 uv;
out vec4 fragColor;

@COMMON@

void main()
{
	ivec2 p = ivec2( gl_FragCoord.xy );

	float x  = texelFetch( Luma, p, 0 ).r;
	float ln = texture( BlurNarrow, uv ).r;
	float lw = texture( BlurWide, uv ).r;

	// lo, mid, hi -- summing back to x exactly.
	vec3 bands = vec3( lw, ln - lw, x - ln );

	float scan  = texture( Envelope, uv ).r;
	float frame = texelFetch( FrameLevel, ivec2( 0, 0 ), 0 ).r;
	float env   = mix( scan, frame, clamp( FrameBlend, 0.0, 1.0 ) );

	// The audio drives the ENCODE side only. A real receiver's expander knows
	// nothing about what the transmitter's VCA was being fed, and that is the
	// interesting behaviour rather than a simplification: drive both ends and
	// the round trip cancels and the picture merely gets louder, drive one and
	// the link mistracks in time with the music.
	float level = sidechainLevel( SidechainMode, env, AudioEnv, AudioAmount );

	float pre = preEmphasis( bands, EmphasisDb );
	float enc = pre * compressGain( level );

	fragColor = vec4( linkStage( enc, LinkGain, Headroom, Noise, noiseAt( p, Frame ) ), 0.0, 0.0, 1.0 );
}
)";

const char* const kDecodeFragment = R"(#version 410 core
uniform sampler2D Encoded;
uniform sampler2D Envelope;      // the SECOND detector, on the encoded signal
uniform sampler2D FrameLevel;
uniform float     FrameBlend;

in vec2 uv;
out vec4 fragColor;

@COMMON@

void main()
{
	float e     = texelFetch( Encoded, ivec2( gl_FragCoord.xy ), 0 ).r;
	float scan  = texture( Envelope, uv ).r;
	float frame = texelFetch( FrameLevel, ivec2( 0, 0 ), 0 ).r;
	float env   = mix( scan, frame, clamp( FrameBlend, 0.0, 1.0 ) );

	// No sidechain override here, deliberately. See the encode pass.
	fragColor = vec4( e * expandGain( env ), 0.0, 0.0, 1.0 );
}
)";

const char* const kOutputFragment = R"(#version 410 core
uniform sampler2D Source;
uniform sampler2D Luma;
uniform sampler2D Decoded;
uniform sampler2D BlurNarrow;
uniform sampler2D BlurWide;

uniform float EmphasisDb;
uniform float Tilt;
uniform float Chroma;
uniform float Mix;

in vec2 uv;
out vec4 fragColor;

@COMMON@

void main()
{
	ivec2 p = ivec2( gl_FragCoord.xy );

	float d  = texelFetch( Decoded, p, 0 ).r;
	float dn = texture( BlurNarrow, uv ).r;
	float dw = texture( BlurWide, uv ).r;

	vec3  bands   = vec3( dw, dn - dw, d - dn );
	float outLuma = deEmphasis( bands, EmphasisDb, Tilt );

	vec4  src     = texture( Source, uv );
	float srcLuma = texelFetch( Luma, p, 0 ).r;

	vec3 wet = applyGain( src.rgb, srcLuma, outLuma, Chroma );

	fragColor = vec4( mix( src.rgb, wet, clamp( Mix, 0.0, 1.0 ) ), src.a );
}
)";

namespace
{
/// Splice `kCommon` in where the fragment source asks for it.
///
/// A marker rather than a concatenation at the top, because these shaders
/// declare their own uniforms first and GLSL wants `#version` on line one.
std::string assemble( const char* source )
{
	std::string s( source );
	const std::string marker = "@COMMON@";

	const size_t at = s.find( marker );
	if( at != std::string::npos )
		s.replace( at, marker.size(), kCommon );

	return s;
}
} // namespace

const char* EncodeFragment()
{
	static const std::string s = assemble( kEncodeFragment );
	return s.c_str();
}

const char* DecodeFragment()
{
	static const std::string s = assemble( kDecodeFragment );
	return s.c_str();
}

const char* OutputFragment()
{
	static const std::string s = assemble( kOutputFragment );
	return s.c_str();
}

} // namespace compander::shaders
