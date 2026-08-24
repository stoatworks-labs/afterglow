#include "Shaders.h"

namespace afterglow
{

const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;

	//Straight through, in 0..1 picture space. The usual FFGL vertex shader
	//folds MaxUV in here; that happens once in the copy pass instead, and
	//every pass after it works on a texture we allocated, where the picture
	//really does fill the texture.
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// Pass 1: copy. Run twice -- see Shaders.h.
//---------------------------------------------------------------------------
const char* const kCopyShader = R"(#version 410 core

uniform sampler2D InputTexture;
uniform vec2 MaxUV;      //the part of the input texture that is really picture
uniform vec2 HalfTexel;  //half an input texel, in picture space
uniform float SourceLod; //mip level to read at; 0 on the way in from the host

in vec2 uv;
out vec4 fragColor;

void main()
{
	//Half a texel in from the edge. GL_LINEAR at the picture boundary takes
	//half its weight from the texture's undrawn padding, and a trail is built
	//by sampling the same frame dozens of times -- so a dark fringe on the
	//first copy is a dark fringe repeated once per ghost, which reads as a
	//deliberate vignette that cannot be turned off.
	vec2 picture = clamp( uv, HalfTexel, vec2( 1.0 ) - HalfTexel );

	//Premultiplied in, premultiplied out. The mip chain built on this texture
	//is a box filter, and averaging premultiplied samples is the correct
	//filter; averaging straight colour smears the colour of transparent pixels
	//into the picture.
	//
	//The second use of this pass is the one the level is for: filling a
	//half- or quarter-size queue slot. Point-sampling a picture four times
	//finer than the target is not a downsample, it is an aliased one, and on
	//footage with detail in it the queue would crawl frame to frame while the
	//live picture sat still.
	fragColor = textureLod( InputTexture, picture * MaxUV, SourceLod );
}
)";

//---------------------------------------------------------------------------
// The mirrored library. See Decay.cpp; every line marked there is marked here.
//---------------------------------------------------------------------------
static const char* const kDecayLibrarySource = R"(
struct Ghost
{
	float weight;
	vec2 offset;
	float scale;
	float spin;
	float levels;
	float cells;
	float warp;
	float hue;
	float bleach;
	float halation;
};

const float kTau = 6.283185307179586;

//= mirrored
uint hashInt( uint seed )
{
	uint state = seed * 747796405u + 2891336453u;
	uint word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

//= mirrored
float hash01( uint seed )
{
	return float( hashInt( seed ) ) * 2.3283064365386963e-10;
}

//= mirrored
float smootherstepf( float f )
{
	return f * f * f * ( f * ( f * 6.0 - 15.0 ) + 10.0 );
}

//= mirrored
float lattice( int ix, int iy, int iz, uint salt )
{
	uint seed = uint( ix ) * 374761393u
	          + uint( iy ) * 668265263u
	          + uint( iz ) * 2246822519u
	          + salt * 3266489917u;
	return hash01( seed );
}

//= mirrored
float valueNoise3( float x, float y, float z, uint salt )
{
	float fx = floor( x );
	float fy = floor( y );
	float fz = floor( z );

	int ix = int( fx );
	int iy = int( fy );
	int iz = int( fz );

	float ax = smootherstepf( x - fx );
	float ay = smootherstepf( y - fy );
	float az = smootherstepf( z - fz );

	float c000 = lattice( ix, iy, iz, salt );
	float c100 = lattice( ix + 1, iy, iz, salt );
	float c010 = lattice( ix, iy + 1, iz, salt );
	float c110 = lattice( ix + 1, iy + 1, iz, salt );
	float c001 = lattice( ix, iy, iz + 1, salt );
	float c101 = lattice( ix + 1, iy, iz + 1, salt );
	float c011 = lattice( ix, iy + 1, iz + 1, salt );
	float c111 = lattice( ix + 1, iy + 1, iz + 1, salt );

	float x00 = c000 + ( c100 - c000 ) * ax;
	float x10 = c010 + ( c110 - c010 ) * ax;
	float x01 = c001 + ( c101 - c001 ) * ax;
	float x11 = c011 + ( c111 - c011 ) * ax;

	float y0 = x00 + ( x10 - x00 ) * ay;
	float y1 = x01 + ( x11 - x01 ) * ay;

	return y0 + ( y1 - y0 ) * az;
}

//= mirrored
float noiseField( float x, float y, float z, uint salt )
{
	float a = valueNoise3( x, y, z, salt );
	float b = valueNoise3( x * 2.17 + 11.3, y * 2.17 - 7.1, z * 1.63 + 3.7, salt + 7u );
	return ( a * 0.65 + b * 0.35 ) * 2.0 - 1.0;
}

//= mirrored
vec2 warpOffset( float x, float y, float phase )
{
	return vec2( noiseField( x, y, phase, 0u ), noiseField( x, y, phase, 1u ) );
}

//= mirrored
vec2 ghostSampleUV( Ghost g, vec2 at, float aspect, float warpScale, float warpPhase )
{
	//Into picture widths on both axes, centred. In 0..1 on both axes instead,
	//Spin becomes a shear on anything that is not square, Drift travels
	//further vertically than horizontally at the same setting, and a round
	//pixelation cell comes out oblong.
	float px = at.x - 0.5;
	float py = ( at.y - 0.5 ) / aspect;

	//The INVERSE of the ghost's own transform: this is a fetch, so a ghost
	//that was pushed right is sampled to the left.
	px -= g.offset.x;
	py -= g.offset.y;

	float c = cos( -g.spin );
	float s = sin( -g.spin );
	float rx = px * c - py * s;
	float ry = px * s + py * c;

	float scale = g.scale != 0.0 ? g.scale : 1.0;
	px = rx / scale;
	py = ry / scale;

	//Warp, sampled at the point being fetched rather than at the point being
	//drawn, so the field is attached to the picture and not to the screen.
	if( g.warp > 0.0 )
	{
		vec2 d = warpOffset( px * warpScale, py * warpScale, warpPhase );
		px += d.x * g.warp;
		py += d.y * g.warp;
	}

	vec2 outUV = vec2( px + 0.5, py * aspect + 0.5 );

	//Pixelation last, so the cells are square on screen and stay put while
	//the ghost drifts underneath them.
	if( g.cells > 0.0 )
	{
		vec2 cells = vec2( g.cells, g.cells / aspect );
		outUV = ( floor( outUV * cells ) + 0.5 ) / cells;
	}

	return outUV;
}

//= mirrored
vec4 degradeColour( Ghost g, vec4 colour )
{
	float a = colour.a;

	//Un-premultiply first. Quantising premultiplied colour quantises the
	//COVERAGE into the colour, so a soft edge acquires bands that follow the
	//alpha rather than the picture.
	vec3 c = a > 0.0031 ? colour.rgb / a : colour.rgb;

	if( g.levels < 255.5 )
	{
		//Round, do not truncate. Truncation is a half-level darkening as well
		//as a quantisation.
		float steps = max( 1.0, g.levels - 1.0 );
		c = floor( c * steps + 0.5 ) / steps;
	}

	//Hue, in YIQ. The luma this produces is used for Bleach as well -- one
	//definition of grey per pass. The rotation preserves it exactly, so it is
	//still the luma of the rotated colour.
	float y = dot( c, vec3( 0.299, 0.587, 0.114 ) );

	if( g.hue != 0.0 )
	{
		float i = dot( c, vec3( 0.596, -0.274, -0.322 ) );
		float q = dot( c, vec3( 0.211, -0.523, 0.312 ) );

		float angle = g.hue * kTau;
		float cs = cos( angle );
		float sn = sin( angle );

		float i2 = i * cs - q * sn;
		float q2 = i * sn + q * cs;

		c = vec3( y + 0.956 * i2 + 0.621 * q2,
		          y - 0.272 * i2 - 0.647 * q2,
		          y - 1.106 * i2 + 1.703 * q2 );
	}

	if( g.bleach > 0.0 )
		c = vec3( y ) + ( c - vec3( y ) ) * ( 1.0 - g.bleach );

	//Alpha is deliberately untouched.
	return vec4( c * a, a );
}
)";

const char* const kDecayLibrary = kDecayLibrarySource;

//---------------------------------------------------------------------------
// Pass 2: ghost. One slot of the queue, degraded, weighted, blended.
//---------------------------------------------------------------------------
static const char* const kGhostPreamble = R"(#version 410 core

uniform sampler2D SlotTexture;
uniform vec2 SlotHalfTexel;

uniform float Aspect;
uniform float WarpScale;
uniform float WarpPhase;

//The resolved ghost, computed on the CPU by GhostAt(). See Decay.h.
uniform float Weight;
uniform vec2 Offset;
uniform float Scale;
uniform float Spin;
uniform float Levels;
uniform float Cells;
uniform float WarpAmount;
uniform float Hue;
uniform float Bleach;

//0 for the trail, 1 for the halation source.
uniform float Bloom;
uniform vec3 BloomTint;

in vec2 uv;
out vec4 fragColor;
)";

static const char* const kGhostMain = R"(
void main()
{
	Ghost g = Ghost( Weight, Offset, Scale, Spin, Levels, Cells, WarpAmount, Hue, Bleach, 0.0 );

	vec2 at = ghostSampleUV( g, uv, Aspect, WarpScale, WarpPhase );

	//Outside the picture there is nothing to show, and CLAMP_TO_EDGE is the
	//wrong answer rather than a safe one: a ghost that has drifted a tenth of
	//the frame would drag its edge row across that tenth as a set of coloured
	//streaks. They read as a smear effect -- which is what this plugin does
	//everywhere else, so they do not look like a bug.
	if( at.x < 0.0 || at.x > 1.0 || at.y < 0.0 || at.y > 1.0 )
	{
		fragColor = vec4( 0.0 );
		return;
	}

	vec2 fetch = clamp( at, SlotHalfTexel, vec2( 1.0 ) - SlotHalfTexel );
	vec4 colour = degradeColour( g, texture( SlotTexture, fetch ) );

	if( Bloom > 0.5 )
	{
		//A bright pass, because halation is highlights spilling and not the
		//whole picture going soft. The knee is wide on purpose: a hard
		//threshold makes the glow pop in and out as the footage passes
		//through it, which on a trail happens once per ghost.
		float lum = dot( colour.rgb, vec3( 0.2126, 0.7152, 0.0722 ) );
		colour.rgb *= smoothstep( 0.35, 0.75, lum ) * BloomTint;
	}

	//Premultiplied, so one multiply covers colour and coverage together. The
	//blend mode is GL state, set per pass -- see Afterglow.cpp.
	fragColor = colour * Weight;
}
)";

//---------------------------------------------------------------------------
// Pass 3: blur. Run twice per axis, on the halation buffer only.
//---------------------------------------------------------------------------
const char* const kBlurShader = R"(#version 410 core

uniform sampler2D SourceTexture;
uniform vec2 Direction;  //one tap step, in picture space. Zero on the other axis.

in vec2 uv;
out vec4 fragColor;

void main()
{
	//A nine-tap Gaussian folded into five fetches. The offsets are not texel
	//centres: each fetch sits between two texels so that GL_LINEAR returns
	//their weighted average, which is why this needs Sampling::Linear and
	//would silently become a five-tap box on a Nearest buffer.
	const float offsets[ 3 ] = float[]( 0.0, 1.3846153846, 3.2307692308 );
	const float weights[ 3 ] = float[]( 0.2270270270, 0.3162162162, 0.0702702703 );

	vec4 sum = texture( SourceTexture, uv ) * weights[ 0 ];
	for( int i = 1; i < 3; ++i )
	{
		sum += texture( SourceTexture, uv + Direction * offsets[ i ] ) * weights[ i ];
		sum += texture( SourceTexture, uv - Direction * offsets[ i ] ) * weights[ i ];
	}

	fragColor = sum;
}
)";

//---------------------------------------------------------------------------
// Pass 4: composite, straight to the host's framebuffer.
//---------------------------------------------------------------------------
const char* const kCompositeShader = R"(#version 410 core

uniform sampler2D SourceTexture;
uniform sampler2D TrailTexture;
uniform sampler2D HalationTexture;

uniform float Background; //0 black, 1 ghosts, 2 source, 3 transparent
uniform float Gain;
uniform float Halation;
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec4 source = texture( SourceTexture, uv );
	vec4 trail  = texture( TrailTexture, uv ) * Gain;
	vec4 halo   = texture( HalationTexture, uv ) * Halation;

	int mode = int( Background + 0.5 );

	//Which frames are in the trail is decided in the accumulation loop, not
	//here: only Ghosts leaves the live frame out. See Afterglow.cpp.
	vec4 back = mode == 2 ? source
	          : mode == 3 ? vec4( 0.0 )
	                      : vec4( 0.0, 0.0, 0.0, 1.0 );

	//The trail goes OVER the background, not added to it. The trail is a
	//picture -- a weighted average of recent frames -- and on footage that is
	//not moving it is the picture, so adding it to the source would put every
	//still frame in the show through at twice its own brightness. Over, it
	//passes a still frame through untouched, which is the invariant the draw
	//weights are built to hold up (see ResolveDrawAlphas).
	//
	//Gain multiplies a PREMULTIPLIED colour, so below 1 it fades the trail
	//towards the background rather than towards black.
	float cover = clamp( trail.a, 0.0, 1.0 );
	vec3 over   = back.rgb * ( 1.0 - cover ) + trail.rgb;
	float alpha = clamp( back.a * ( 1.0 - cover ) + trail.a, 0.0, 1.0 );

	//The halation IS light, so that one is added. A halo is light scattering
	//back through the emulsion on top of whatever is already there, and
	//alpha-blending it would have the glow HIDE the picture it came from.
	vec4 result = vec4( over + halo.rgb, clamp( alpha + halo.a, 0.0, 1.0 ) );

	fragColor = mix( source, result, MixAmount );
}
)";

//---------------------------------------------------------------------------
// Assembly.
//---------------------------------------------------------------------------
std::string GhostShaderSource()
{
	return std::string( kGhostPreamble ) + kDecayLibrarySource + kGhostMain;
}

std::string DecayProbeShaderSource()
{
	//One case per DRAW and one sample point per COLUMN, with the two mirrored
	//functions on two rows: row 0 is where GhostSampleUV says to fetch, row 1
	//is what DegradeColour does to a colour. Both are read back from an
	//RGBA32F target, so what comes back is the shader's own float rather than
	//a quantised version of it -- an 8-bit readback would agree to within
	//1/255 with almost any drift and call it a pass.
	//
	//The sample points and the test colour are derived from the column index
	//with integer arithmetic and the same PCG hash both sides run, so the
	//harness knows exactly what the shader was asked without having to be
	//told.
	static const char* const probeMain = R"(
uniform float Aspect;
uniform float WarpScale;
uniform float WarpPhase;
uniform float Weight;
uniform vec2 Offset;
uniform float Scale;
uniform float Spin;
uniform float Levels;
uniform float Cells;
uniform float WarpAmount;
uniform float Hue;
uniform float Bleach;
uniform int Points;

out vec4 fragColor;

void main()
{
	int x = int( gl_FragCoord.x - 0.5 );
	int row = int( gl_FragCoord.y - 0.5 );

	Ghost g = Ghost( Weight, Offset, Scale, Spin, Levels, Cells, WarpAmount, Hue, Bleach, 0.0 );

	if( row == 0 )
	{
		vec2 at = vec2( ( float( x ) + 0.5 ) / float( Points ),
		                ( float( ( x * 7 + 3 ) % Points ) + 0.5 ) / float( Points ) );
		fragColor = vec4( ghostSampleUV( g, at, Aspect, WarpScale, WarpPhase ), 0.0, 1.0 );
	}
	else
	{
		uint seed = uint( x ) * 2654435761u;
		vec4 colour = vec4( hash01( seed ), hash01( seed + 1u ), hash01( seed + 2u ),
		                    0.25 + 0.75 * hash01( seed + 3u ) );
		//A premultiplied sample: the colour cannot exceed the coverage.
		colour.rgb *= colour.a;
		fragColor = degradeColour( g, colour );
	}
}
)";

	return std::string( "#version 410 core\n" ) + kDecayLibrarySource + probeMain;
}

} // namespace afterglow
