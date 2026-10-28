// GENERATED from source/shaders/*.cpp by demo/tools/check_shaders.py --write.
// Do not edit: tools/verify.sh fails if a character of this differs from the
// plugin's. The one escape allowed is \` for a backtick inside a comment.

// kVertex, source/shaders/Vertex.cpp
export const VERTEX = `#version 410 core
uniform vec2 MaxUV;

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV * MaxUV;
}
`;

// kCommon, source/shaders/Common.cpp
export const COMMON = `
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
`;

// kLumaFragment, source/shaders/Passes.cpp
export const LUMA_FRAGMENT = `#version 410 core
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
`;

// kBlurFragment, source/shaders/Passes.cpp
export const BLUR_FRAGMENT = `#version 410 core
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
`;

// kReduceFragment, source/shaders/Passes.cpp
export const REDUCE_FRAGMENT = `#version 410 core
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
`;

// kScanFragment, source/shaders/Passes.cpp
export const SCAN_FRAGMENT = `#version 410 core
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
`;

// kFrameFragment, source/shaders/Passes.cpp
export const FRAME_FRAGMENT = `#version 410 core
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
	// mipmapping min filter, which \`ffglex::FFGLFBO\` does not do -- so the
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
`;

// kEncodeFragment, source/shaders/Passes.cpp
export const ENCODE_FRAGMENT = `#version 410 core
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
`;

// kDecodeFragment, source/shaders/Passes.cpp
export const DECODE_FRAGMENT = `#version 410 core
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
`;

// kOutputFragment, source/shaders/Passes.cpp
export const OUTPUT_FRAGMENT = `#version 410 core
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
`;
