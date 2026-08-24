/**
	agtest -- render Afterglow offline, and check what its ghosts are doing.

	What a frame looks like once it is five frames old is a fact. Not "looks
	about right": the per-pixel decay stage is a pure function of a ghost and a
	point, it exists twice -- once in `Decay.cpp` for this harness and the
	OpenFX build to call, once in GLSL for the GPU to run -- and `--decay`
	runs both and compares them.

	The trick that makes that possible is in `Shaders.h`: the GLSL decay
	library is a *fragment* rather than a shader, and both the ghost pass and
	this harness's probe are assembled around the same string. So what is being
	checked is the text the plugin actually runs.

		agtest --out /tmp/frame.png     a picture, on a moving test card
		agtest --list                   every parameter and its default
		agtest --decay                  GLSL against C++, over the whole space
		agtest --presets                every preset survives every host
		agtest --card /tmp/card.png     the test card on its own
		agtest --bench                  the render cost
		agtest --pipe                   raw frames in, raw frames out

	**The test card MOVES**, and that is not decoration. This plugin's entire
	output is the difference between one frame and the last one; on a still
	card every slot in the queue holds the same picture, every ghost lands
	exactly on top of the live frame, and the trail is provably invisible. A
	still card would have `tools/sweep.py` report almost every control dead,
	and -- worse -- would make a broken build and a working one produce
	identical pictures.

	`--script` is a plain text file of `frame  Parameter Name  value` lines.
	Values are held before the first key and after the last, and linearly
	interpolated between. The format is identical to old-cathode's octest,
	porthole's phtest and tinsel's tinseltest on purpose, so one build.py can
	film any of them.

	`--pipe` takes the fleet's frame format:

		ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
		  | agtest --pipe --width 1920 --height 1080 [--script cues.txt] \
		  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov
*/

#include "Afterglow.h"
#include "Controls.h"
#include "Decay.h"
#include "Presets.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace afterglow;

namespace
{
//---------------------------------------------------------------------------
/// How far the GLSL and the C++ are allowed to disagree.
///
/// Not zero, and it cannot be. The GLSL specification allows three units in
/// the last place for `exp` and gives `sin` and `cos` no accuracy requirement
/// at all outside a limited range, so the hue rotation and the spin will
/// differ from libm's in the last few bits on any driver. What this tolerance
/// catches is a *drifted constant* -- a YIQ coefficient of 0.965 against
/// 0.956, an octave weight of 0.6 against 0.65 -- which misses by percent,
/// not by 1e-6.
constexpr float kDecayTolerance = 5e-4f;

/// How close to a step boundary counts as "on it".
///
/// Two of the mirrored functions are deliberately DISCONTINUOUS -- the
/// quantiser's `floor` and the pixelation grid's `floor` -- and at a
/// discontinuity the two implementations are allowed to disagree by a whole
/// step for a difference of one bit in the argument. That is not a defect and
/// no tolerance can absorb it, because the whole point of a step is that the
/// output jumps.
///
/// So a comparison whose argument lands within this of an integer is counted
/// and skipped rather than failed. The count is printed: a run where a large
/// fraction went that way would mean the sample points had been chosen badly
/// and the check was measuring less than it looks like it is.
constexpr float kStepGuard = 2e-4f;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The test card.
//
// Not meant to look nice. Each part of it exercises one control, and the
// whole thing MOVES, because a trail of identical frames is not a trail:
//
//   - a bright disc on a Lissajous path: the trail itself, and the only thing
//     in the frame bright enough for Halation's bright pass to find
//   - a bar sweeping horizontally:       Drift and Spin, which are readable
//                                        against a straight edge and not
//                                        against a round one
//   - six saturated colour bars:         Hue Shift and Bleach, both of which
//                                        do nothing measurable to grey
//   - a smooth luminance ramp:           Crush, whose banding has to have
//                                        somewhere to appear
//   - a fine checkerboard:               Pixelate, and the aliasing that a
//                                        Resolution divisor would cause if
//                                        the queue were filled by point
//                                        sampling rather than off a mip level
//   - a dark surround:                   so a screen blend has headroom and
//                                        the trail is visible at all
//---------------------------------------------------------------------------
std::vector< unsigned char > buildCard( int width, int height, int frame )
{
	std::vector< unsigned char > card( static_cast< size_t >( width ) * height * 4 );

	const float w = static_cast< float >( width );
	const float h = static_cast< float >( height );
	const float t = static_cast< float >( frame );

	//Incommensurable rates, so the card never repeats within a take.
	const float discX = 0.5f + 0.34f * std::sin( t * 0.1100f );
	const float discY = 0.5f + 0.24f * std::sin( t * 0.0770f + 1.1f );
	const float discR = 0.055f;

	const float barX = 0.5f + 0.40f * std::sin( t * 0.0430f + 2.3f );
	const float barW = 0.020f;

	const float aspect = w / h;

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = ( static_cast< float >( x ) + 0.5f ) / w;
			const float v = ( static_cast< float >( y ) + 0.5f ) / h;

			//A dark, slightly graded surround.
			float r = 0.04f + 0.05f * v;
			float g = 0.04f + 0.05f * v;
			float b = 0.06f + 0.06f * v;

			//Six saturated bars across the bottom eighth.
			if( v < 0.125f )
			{
				static const float bars[ 6 ][ 3 ] = {
					{ 1.0f, 0.1f, 0.1f }, { 0.1f, 1.0f, 0.1f }, { 0.1f, 0.1f, 1.0f },
					{ 0.1f, 1.0f, 1.0f }, { 1.0f, 0.1f, 1.0f }, { 1.0f, 1.0f, 0.1f }
				};
				const int bar = std::min( 5, static_cast< int >( u * 6.0f ) );
				r             = bars[ bar ][ 0 ];
				g             = bars[ bar ][ 1 ];
				b             = bars[ bar ][ 2 ];
			}
			//A smooth ramp above them: where Crush's banding shows up.
			else if( v < 0.25f )
			{
				r = g = b = u;
			}
			//A four-pixel checkerboard in the top-left: fine detail, for
			//Pixelate and for anything that resamples.
			else if( u < 0.25f && v > 0.75f )
			{
				const bool on = ( ( x / 4 ) + ( y / 4 ) ) % 2 == 0;
				r = g = b = on ? 0.85f : 0.10f;
			}

			//The sweeping bar. Straight edges on both sides, so a rotation or
			//a translation of an old frame is readable rather than merely
			//present.
			if( std::fabs( u - barX ) < barW && v > 0.28f )
			{
				r = 0.55f;
				g = 0.75f;
				b = 0.95f;
			}

			//The bright disc, with a soft shoulder so it does not alias as it
			//travels. Measured in picture WIDTHS on both axes, like everything
			//geometric in this plugin.
			const float dx   = u - discX;
			const float dy   = ( v - discY ) / aspect;
			const float dist = std::sqrt( dx * dx + dy * dy );
			if( dist < discR )
			{
				const float edge = std::min( 1.0f, ( discR - dist ) / ( discR * 0.25f ) );
				r                = r + ( 1.0f - r ) * edge;
				g                = g + ( 1.0f - g ) * edge;
				b                = b + ( 1.0f - b ) * edge;
			}

			const size_t i  = ( static_cast< size_t >( y ) * width + x ) * 4;
			card[ i + 0 ]   = static_cast< unsigned char >( std::clamp( r, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 1 ]   = static_cast< unsigned char >( std::clamp( g, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 2 ]   = static_cast< unsigned char >( std::clamp( b, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 3 ]   = 255;
		}
	}

	return card;
}

std::vector< unsigned char > addNoise( const std::vector< unsigned char >& card, int frame, float amount )
{
	std::vector< unsigned char > noisy = card;
	if( amount <= 0.0f )
		return noisy;

	const float scale = amount * 255.0f;
	for( size_t i = 0; i < noisy.size(); i += 4 )
	{
		//The same PCG the decay library uses, over pixel index and frame.
		const uint32_t seed = HashInt( static_cast< uint32_t >( i / 4 ) * 2654435761u
		                               ^ static_cast< uint32_t >( frame ) );
		const float jitter  = ( Hash01( seed ) - 0.5f ) * scale;

		for( int c = 0; c < 3; ++c )
		{
			const float value = static_cast< float >( noisy[ i + c ] ) + jitter;
			noisy[ i + c ]    = static_cast< unsigned char >( std::min( 255.0f, std::max( 0.0f, value ) ) );
		}
	}
	return noisy;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without
	//a GPU, where it will at least prove the shaders compile.
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

GLuint makeTexture( int width, int height, const unsigned char* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
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

std::vector< unsigned char > readBackRaw( GLuint fbo, int width, int height )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
	return pixels;
}

//---------------------------------------------------------------------------
// Shader compilation, for the probe. The plugin uses ffglex for this; the
// probe cannot, because ffglex::FFGLShader insists on a vertex shader with the
// SDK's attribute layout and its own uniform helpers, and the probe needs
// neither.
//---------------------------------------------------------------------------
GLuint compileStage( GLenum type, const std::string& source, std::string& error )
{
	const GLuint shader   = glCreateShader( type );
	const char* const ptr = source.c_str();
	glShaderSource( shader, 1, &ptr, nullptr );
	glCompileShader( shader );

	GLint compiled = GL_FALSE;
	glGetShaderiv( shader, GL_COMPILE_STATUS, &compiled );
	if( compiled == GL_TRUE )
		return shader;

	GLint length = 0;
	glGetShaderiv( shader, GL_INFO_LOG_LENGTH, &length );
	std::string log( static_cast< size_t >( std::max( length, 1 ) ), '\0' );
	glGetShaderInfoLog( shader, length, nullptr, log.data() );
	error = log;
	glDeleteShader( shader );
	return 0;
}

GLuint buildProbeProgram( std::string& error )
{
	static const char* const vertexSource = R"(#version 410 core
layout( location = 0 ) in vec4 vPosition;
void main() { gl_Position = vPosition; }
)";

	const GLuint vertex = compileStage( GL_VERTEX_SHADER, vertexSource, error );
	if( vertex == 0 )
		return 0;

	const GLuint fragment = compileStage( GL_FRAGMENT_SHADER, DecayProbeShaderSource(), error );
	if( fragment == 0 )
	{
		glDeleteShader( vertex );
		return 0;
	}

	const GLuint program = glCreateProgram();
	glAttachShader( program, vertex );
	glAttachShader( program, fragment );
	glLinkProgram( program );
	glDeleteShader( vertex );
	glDeleteShader( fragment );

	GLint linked = GL_FALSE;
	glGetProgramiv( program, GL_LINK_STATUS, &linked );
	if( linked == GL_TRUE )
		return program;

	GLint length = 0;
	glGetProgramiv( program, GL_INFO_LOG_LENGTH, &length );
	std::string log( static_cast< size_t >( std::max( length, 1 ) ), '\0' );
	glGetProgramInfoLog( program, length, nullptr, log.data() );
	error = log;
	glDeleteProgram( program );
	return 0;
}

//---------------------------------------------------------------------------
// Parameters by display name, so the automation reads as English.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	float value;
};

std::vector< NamedParameter > listParameters( Afterglow& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Afterglow::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ) } );
	}
	return list;
}

bool applySetting( Afterglow& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}

	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );

	for( const NamedParameter& parameter : listParameters( plugin ) )
	{
		if( parameter.name != name )
			continue;
		plugin.SetFloatParameter( parameter.index, std::strtof( value.c_str(), nullptr ) );
		return true;
	}

	error = "no parameter called '" + name + "'";
	return false;
}

//---------------------------------------------------------------------------
// --decay
//---------------------------------------------------------------------------

/// Sample points, one per column of the probe. Prime, so no point lands on a
/// binary fraction and cancels a quantisation bug out on both sides.
constexpr int kProbePoints = 127;

/// The same point the probe computes for a column. Integer arithmetic on both
/// sides, so this is not an approximation of what the shader was asked.
void probePoint( int x, float& u, float& v )
{
	u = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( kProbePoints );
	v = ( static_cast< float >( ( x * 7 + 3 ) % kProbePoints ) + 0.5f ) / static_cast< float >( kProbePoints );
}

/// The same colour the probe computes for a column, premultiplied.
void probeColour( int x, float rgba[ 4 ] )
{
	const uint32_t seed = static_cast< uint32_t >( x ) * 2654435761u;
	rgba[ 3 ]           = 0.25f + 0.75f * Hash01( seed + 3u );
	rgba[ 0 ]           = Hash01( seed ) * rgba[ 3 ];
	rgba[ 1 ]           = Hash01( seed + 1u ) * rgba[ 3 ];
	rgba[ 2 ]           = Hash01( seed + 2u ) * rgba[ 3 ];
}

/// True when a value is within kStepGuard of an integer -- i.e. sitting on one
/// of the mirrored functions' deliberate discontinuities.
bool onAStep( float value )
{
	const float frac = value - std::floor( value );
	return frac < kStepGuard || frac > 1.0f - kStepGuard;
}

int runDecayCheck()
{
	std::string error;
	const GLuint program = buildProbeProgram( error );
	if( program == 0 )
	{
		std::fprintf( stderr, "the probe shader would not build:\n%s\n", error.c_str() );
		return 1;
	}

	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, kProbePoints, 2, 0, GL_RGBA, GL_FLOAT, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );

	const GLuint fbo = makeFramebuffer( texture );
	if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
	{
		std::fprintf( stderr, "could not make a float target for the probe\n" );
		return 1;
	}

	//A full-screen triangle, which needs no index buffer and no UVs.
	GLuint vao = 0, vbo = 0;
	glGenVertexArrays( 1, &vao );
	glBindVertexArray( vao );
	const float triangle[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
	glGenBuffers( 1, &vbo );
	glBindBuffer( GL_ARRAY_BUFFER, vbo );
	glBufferData( GL_ARRAY_BUFFER, sizeof( triangle ), triangle, GL_STATIC_DRAW );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, 0, nullptr );

	//The cases. Each one turns some controls up and leaves the rest at their
	//nulls, plus two that turn everything up at once -- because a mirrored
	//function can be right on every control taken alone and wrong about the
	//ORDER it applies them in, and only a case with several at once can see
	//that.
	struct Case
	{
		const char* name;
		DecayParams p;
	};

	DecayParams base;
	base.frames = 12;
	base.curve  = 1.0f;

	auto with = [ base ]( auto&& mutate ) {
		DecayParams p = base;
		mutate( p );
		return p;
	};

	const Case cases[] = {
		{ "null",      base },
		{ "drift",     with( []( DecayParams& p ) { p.drift = 0.12f; p.driftAngle = 0.37f; } ) },
		{ "zoom-in",   with( []( DecayParams& p ) { p.zoom = 0.31f; } ) },
		{ "zoom-out",  with( []( DecayParams& p ) { p.zoom = -0.29f; } ) },
		{ "spin",      with( []( DecayParams& p ) { p.spin = 0.17f; } ) },
		{ "spin-back", with( []( DecayParams& p ) { p.spin = -0.23f; } ) },
		{ "crush",     with( []( DecayParams& p ) { p.crush = 0.83f; } ) },
		{ "crush-max", with( []( DecayParams& p ) { p.crush = 1.0f; } ) },
		{ "pixelate",  with( []( DecayParams& p ) { p.pixelate = 0.61f; } ) },
		{ "warp",      with( []( DecayParams& p ) { p.warp = 0.043f; } ) },
		{ "hue",       with( []( DecayParams& p ) { p.hue = 0.29f; } ) },
		{ "hue-back",  with( []( DecayParams& p ) { p.hue = -0.41f; } ) },
		{ "bleach",    with( []( DecayParams& p ) { p.bleach = 0.73f; } ) },
		{ "everything",
		  with( []( DecayParams& p ) {
			  p.drift = 0.07f; p.driftAngle = 0.61f; p.zoom = 0.19f; p.spin = -0.11f;
			  p.crush = 0.55f; p.pixelate = 0.33f; p.warp = 0.021f; p.hue = 0.23f; p.bleach = 0.41f;
		  } ) },
		{ "everything-hard",
		  with( []( DecayParams& p ) {
			  p.frames = 32; p.curve = 2.5f;
			  p.drift = 0.22f; p.driftAngle = 0.13f; p.zoom = -0.44f; p.spin = 0.21f;
			  p.crush = 0.97f; p.pixelate = 0.89f; p.warp = 0.071f; p.hue = -0.47f; p.bleach = 0.88f;
		  } ) },
	};

	//Aspect ratios that are not 1, because everything geometric here divides
	//by one and a bug that cancels at 1:1 is a bug that shows on every clip
	//anybody actually plays.
	const float aspects[]    = { 16.0f / 9.0f, 4.0f / 3.0f, 1.0f, 9.0f / 16.0f };
	const float warpScales[] = { 3.0f, 17.0f };
	const float warpPhases[] = { 0.0f, 2.718f };

	int checks        = 0;
	int disagreements = 0;
	int skipped       = 0;
	float worst       = 0.0f;
	std::string worstWhere;

	std::vector< float > readback( static_cast< size_t >( kProbePoints ) * 2 * 4 );

	glUseProgram( program );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glViewport( 0, 0, kProbePoints, 2 );

	auto setFloat = [ program ]( const char* name, float value ) {
		glUniform1f( glGetUniformLocation( program, name ), value );
	};

	for( const Case& c : cases )
	{
		for( float aspect : aspects )
		{
			for( float warpScale : warpScales )
			{
				for( float warpPhase : warpPhases )
				{
					const int step = std::max( 1, c.p.frames / 5 );
					for( int slot = 0; slot < c.p.frames; slot += step )
					{
						const Ghost ghost = GhostAt( slot, c.p );

						setFloat( "Aspect", aspect );
						setFloat( "WarpScale", warpScale );
						setFloat( "WarpPhase", warpPhase );
						setFloat( "Weight", ghost.weight );
						glUniform2f( glGetUniformLocation( program, "Offset" ), ghost.offsetX, ghost.offsetY );
						setFloat( "Scale", ghost.scale );
						setFloat( "Spin", ghost.spin );
						setFloat( "Levels", ghost.levels );
						setFloat( "Cells", ghost.cells );
						setFloat( "WarpAmount", ghost.warp );
						setFloat( "Hue", ghost.hue );
						setFloat( "Bleach", ghost.bleach );
						glUniform1i( glGetUniformLocation( program, "Points" ), kProbePoints );

						glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
						glClear( GL_COLOR_BUFFER_BIT );
						glDrawArrays( GL_TRIANGLES, 0, 3 );
						glReadPixels( 0, 0, kProbePoints, 2, GL_RGBA, GL_FLOAT, readback.data() );

						for( int x = 0; x < kProbePoints; ++x )
						{
							//--- row 0: where the ghost is sampled ---------
							float u = 0.0f, v = 0.0f;
							probePoint( x, u, v );

							float wantU = 0.0f, wantV = 0.0f;
							GhostSampleUV( ghost, u, v, aspect, warpScale, warpPhase, wantU, wantV );

							const float* row0 = &readback[ static_cast< size_t >( x ) * 4 ];

							//A pixelation grid is a step function: within a
							//fraction of a cell of its edge the two sides are
							//allowed to land in different cells.
							bool guarded = false;
							if( ghost.cells > 0.0f )
							{
								//Recomputed from the UNSNAPPED point, which is
								//what the floor is actually applied to.
								Ghost unsnapped = ghost;
								unsnapped.cells = 0.0f;
								float rawU = 0.0f, rawV = 0.0f;
								GhostSampleUV( unsnapped, u, v, aspect, warpScale, warpPhase, rawU, rawV );
								guarded = onAStep( rawU * ghost.cells )
								          || onAStep( rawV * ghost.cells / aspect );
							}

							if( !guarded )
							{
								const float d = std::max( std::fabs( row0[ 0 ] - wantU ),
								                          std::fabs( row0[ 1 ] - wantV ) );
								++checks;
								if( d > worst )
								{
									worst = d;
									char where[ 256 ];
									std::snprintf( where, sizeof( where ), "%s uv slot %d aspect %.3f x=%d",
									               c.name, slot, aspect, x );
									worstWhere = where;
								}
								if( d > kDecayTolerance )
								{
									++disagreements;
									if( disagreements <= 12 )
										std::printf( "  %-16s slot %2d aspect %.3f x=%3d  "
										             "uv %.6f,%.6f vs %.6f,%.6f\n",
										             c.name, slot, aspect, x,
										             row0[ 0 ], row0[ 1 ], wantU, wantV );
								}
							}
							else
							{
								++skipped;
							}

							//--- row 1: what the ghost does to a colour ----
							float colour[ 4 ];
							probeColour( x, colour );

							bool colourGuarded = false;
							if( ghost.levels < 255.5f )
							{
								const float steps    = std::max( 1.0f, ghost.levels - 1.0f );
								const float straight = colour[ 3 ] > 0.0031f ? colour[ 3 ] : 1.0f;
								for( int k = 0; k < 3; ++k )
									colourGuarded = colourGuarded
									                || onAStep( colour[ k ] / straight * steps + 0.5f );
							}

							DegradeColour( ghost, colour );
							const float* row1 = &readback[ ( static_cast< size_t >( kProbePoints ) + x ) * 4 ];

							if( colourGuarded )
							{
								++skipped;
								continue;
							}

							float d = 0.0f;
							for( int k = 0; k < 4; ++k )
								d = std::max( d, std::fabs( row1[ k ] - colour[ k ] ) );

							++checks;
							if( d > worst )
							{
								worst = d;
								char where[ 256 ];
								std::snprintf( where, sizeof( where ), "%s colour slot %d x=%d",
								               c.name, slot, x );
								worstWhere = where;
							}
							if( d > kDecayTolerance )
							{
								++disagreements;
								if( disagreements <= 12 )
									std::printf( "  %-16s slot %2d x=%3d  colour "
									             "%.5f,%.5f,%.5f,%.5f vs %.5f,%.5f,%.5f,%.5f\n",
									             c.name, slot, x,
									             row1[ 0 ], row1[ 1 ], row1[ 2 ], row1[ 3 ],
									             colour[ 0 ], colour[ 1 ], colour[ 2 ], colour[ 3 ] );
							}
						}
					}
				}
			}
		}
	}

	glDeleteBuffers( 1, &vbo );
	glDeleteVertexArrays( 1, &vao );
	glDeleteFramebuffers( 1, &fbo );
	glDeleteTextures( 1, &texture );
	glDeleteProgram( program );

	std::printf( "\n%d cases, %d comparisons, %d on a step boundary and skipped, "
	             "%d disagreements past %g\n",
	             static_cast< int >( sizeof( cases ) / sizeof( cases[ 0 ] ) ),
	             checks, skipped, disagreements, kDecayTolerance );
	std::printf( "largest difference %.3g, at %s\n", worst, worstWhere.c_str() );

	if( disagreements > 12 )
		std::printf( "(%d more not shown)\n", disagreements - 12 );

	return disagreements == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// Rendering.
//---------------------------------------------------------------------------

/// Drive the plugin's clock. The harness DECLARES its unit rather than
/// leaving the calibration to infer one: an absolute time handed over in a
/// single frame is genuinely ambiguous, and an implicit unit is what let the
/// millisecond bug through elsewhere in the fleet.
void driveClock( Afterglow& plugin, double seconds )
{
	plugin.SetClockScaleForTest( 1.0 );//seconds, said out loud
	plugin.SetTime( seconds );
}

double benchAt( Afterglow& plugin, int width, int height, int frames, double fps )
{
	const GLuint sourceTexture = makeTexture( width, height, buildCard( width, height, 0 ).data() );
	const GLuint outputTexture = makeTexture( width, height, nullptr );
	const GLuint outputFBO     = makeFramebuffer( outputTexture );

	FFGLTextureStruct inputStruct = {};
	inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
	inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
	inputStruct.Handle                              = sourceTexture;
	FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

	ProcessOpenGLStruct process = {};
	process.numInputTextures    = 1;
	process.inputTextures       = inputs;
	process.HostFBO             = outputFBO;

	auto renderOne = [ & ]( int frame ) {
		driveClock( plugin, static_cast< double >( frame ) / fps );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		plugin.ProcessOpenGL( &process );
	};

	//The warm-up has to be at least a full queue long, or the measurement is
	//dominated by the frames where the queue is still filling and half the
	//ghost draws are skipped for having no frame in them yet.
	const int warmup = 40;
	for( int frame = 0; frame < warmup; ++frame )
		renderOne( frame );
	glFinish();

	const auto start = std::chrono::steady_clock::now();
	for( int frame = 0; frame < frames; ++frame )
		renderOne( warmup + frame );
	glFinish();
	const auto end = std::chrono::steady_clock::now();

	glDeleteFramebuffers( 1, &outputFBO );
	glDeleteTextures( 1, &outputTexture );
	glDeleteTextures( 1, &sourceTexture );

	const double seconds = std::chrono::duration< double >( end - start ).count();
	return seconds * 1000.0 / static_cast< double >( frames );
}

int runBench( Afterglow& plugin, int frames, double fps )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720  ", 1280, 720 },
		{ "1920x1080 ", 1920, 1080 },
		{ "2560x1440 ", 2560, 1440 },
		{ "3840x2160 ", 3840, 2160 },
	};

	//Two things make this a measurement rather than a number: glFinish on both
	//sides, because GL calls queue and an unsynchronised version times how
	//fast a `for` loop runs; and a warm-up that is thrown away, because the
	//first frames pay for allocating the whole queue.
	std::printf( "%d frames each, after a 40-frame warm-up, glFinish both sides.\n\n", frames );
	std::printf( "resolution     ms/frame   equivalent fps   %% of a 60fps frame\n" );

	for( const Size& size : sizes )
	{
		const double ms = benchAt( plugin, size.width, size.height, frames, fps );
		std::printf( "%s    %7.3f       %8.0f            %5.1f%%\n",
		             size.name, ms, ms > 0.0 ? 1000.0 / ms : 0.0, ms / 16.667 * 100.0 );
	}

	std::printf( "\nCost is roughly LINEAR in Frames and quadratic in Resolution: one\n"
	             "ghost draw per slot at picture size, and -- when Halation is above\n"
	             "zero -- one more per slot at quarter size plus four blur passes.\n"
	             "Whatever the settings above were, they are what was measured; run\n"
	             "with --set to measure something else.\n" );
	return 0;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"agtest -- render and check the Afterglow frame-trail effect\n"
		"\n"
		"  --out PATH        render the moving test card through the plugin (default /tmp/afterglow.png)\n"
		"  --card PATH       write the test card alone, undecorated\n"
		"  --width N         width (default 1280)\n"
		"  --height N        height (default 720)\n"
		"  --frames N        frames to render before reading back (default 40)\n"
		"  --fps N           synthetic frame rate driving the clock (default 60)\n"
		"  --noise F         per-frame noise on the source, 0..1\n"
		"  --set \"Name=V\"    set a parameter by its display name, 0..1. Repeatable.\n"
		"  --list            print every parameter and its default, then exit\n"
		"  --presets         every factory preset survives every host behaviour\n"
		"  --decay           run the GLSL decay library against the C++ one\n"
		"  --still           a picture that does not move must come out untouched\n"
		"  --ghosts          dump the resolved decay model as JSON, for the demo's checker\n"
		"  --bench           time ProcessOpenGL at 720p through 4K\n"
		"  --pipe            raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH     parameter cues for --pipe: 'frame Name Value'\n"
		"  --help\n" );
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line, applied when the frame
// number is reached. Same format as porthole, old-cathode and tinsel, so one
// filming script drives any of them.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;//blank or comment

		//The name is everything up to the last token, because parameters have
		//spaces in them ("Halation Size") and the value never does.
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

//---------------------------------------------------------------------------
/// Dump what the plugin thinks, as JSON, for the browser demo to be checked
/// against.
///
/// The demo pages in this fleet copy the plugin's GLSL verbatim and PORT its
/// C++ by hand. `demo/tools/check_shaders.py` catches a drifted shader; until
/// this existed, nothing caught a drifted CONVERSION or a drifted weight
/// curve -- and on this plugin those are not cosmetic, because everything
/// about a ghost comes out of GhostAt.
///
/// So: the same raw slider positions go in on both sides, and the answers are
/// compared. This end writes them; `demo/tools/check_decay.mjs` computes the
/// same from `demo/decay.js` and diffs.
///
/// Hand-rolled JSON, because a dependency for eleven fields is a worse trade
/// than forty lines. Printed at full float precision on purpose: a tolerance
/// belongs to the checker, not to the format.
//---------------------------------------------------------------------------
int runGhostDump()
{
	//Positions chosen to land away from the round numbers as well as on them.
	//A mapping that only agrees at 0, 0.5 and 1 is a mapping that has not been
	//checked -- both sides get those right by accident.
	const float positions[] = { 0.0f, 0.13f, 0.37f, 0.5f, 0.62f, 0.88f, 1.0f };

	std::printf( "{\n  \"controls\": [\n" );
	bool firstRow = true;
	for( float v : positions )
	{
		if( !firstRow )
			std::printf( ",\n" );
		firstRow = false;
		std::printf(
			"    { \"v\": %.9g, \"frames\": %d, \"hold\": %d, \"curve\": %.9g, \"gain\": %.9g,"
			" \"drift\": %.9g, \"driftAngle\": %.9g, \"zoom\": %.9g, \"spin\": %.9g,"
			" \"crush\": %.9g, \"pixelate\": %.9g, \"warp\": %.9g, \"warpScale\": %.9g,"
			" \"warpSpeed\": %.9g, \"hue\": %.9g, \"bleach\": %.9g, \"halation\": %.9g,"
			" \"halationSize\": %.9g, \"halationTint\": %.9g, \"divisor\": %d }",
			v, FramesFromParam( v ), HoldFromParam( v ), CurveFromParam( v ), GainFromParam( v ),
			DriftFromParam( v ), DriftAngleFromParam( v ), ZoomFromParam( v ), SpinFromParam( v ),
			CrushFromParam( v ), PixelateFromParam( v ), WarpFromParam( v ), WarpScaleFromParam( v ),
			WarpSpeedFromParam( v ), HueFromParam( v ), BleachFromParam( v ), HalationFromParam( v ),
			HalationSizeFromParam( v ), HalationTintFromParam( v ),
			ResolutionDivisor( v < 0.5f ? 0.0f : ( v < 0.9f ? 2.0f : 3.0f ) ) );
	}
	std::printf( "\n  ],\n  \"cases\": [\n" );

	//Every factory preset, plus the plugin's own defaults, plus two extremes.
	//The presets earn their place here twice over: they are the settings an
	//operator is most likely to be looking at, and they are already a table
	//both builds read, so a disagreement about one of them is a disagreement
	//about something shipped.
	struct RawCase
	{
		const char* name;
		float frames, hold, curve, drift, driftAngle, zoom, spin;
		float crush, pixelate, warp, hue, bleach, halation;
	};
	std::vector< RawCase > cases = {
		{ "defaults", 0.53f, 0.0f, 0.5f, 0.0f, 0.0f, 0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 0.5f, 0.25f, 0.25f },
		{ "floor", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
		{ "ceiling", 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
	};
	for( int i = 0; i < afterglow::presets::kCount; ++i )
	{
		using namespace afterglow::presets;
		const Preset& p = kPresets[ i ];
		cases.push_back( RawCase {
			p.name, p.v[ kFrames ], p.v[ kHold ], p.v[ kCurve ], p.v[ kDrift ], p.v[ kDriftAngle ],
			p.v[ kZoom ], p.v[ kSpin ], p.v[ kCrush ], p.v[ kPixelate ], p.v[ kWarp ],
			p.v[ kHue ], p.v[ kBleach ], p.v[ kHalation ] } );
	}

	bool firstCase = true;
	for( const RawCase& raw : cases )
	{
		DecayParams p;
		p.frames     = FramesFromParam( raw.frames );
		p.curve      = CurveFromParam( raw.curve );
		p.drift      = DriftFromParam( raw.drift );
		p.driftAngle = DriftAngleFromParam( raw.driftAngle );
		p.zoom       = ZoomFromParam( raw.zoom );
		p.spin       = SpinFromParam( raw.spin );
		p.crush      = CrushFromParam( raw.crush );
		p.pixelate   = PixelateFromParam( raw.pixelate );
		p.warp       = WarpFromParam( raw.warp );
		p.hue        = HueFromParam( raw.hue );
		p.bleach     = BleachFromParam( raw.bleach );
		p.halation   = HalationFromParam( raw.halation );

		if( !firstCase )
			std::printf( ",\n" );
		firstCase = false;

		std::printf( "    { \"name\": \"%s\", \"raw\": [%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
		             "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g], \"frames\": %d,\n      \"ghosts\": [\n",
		             raw.name, raw.frames, raw.hold, raw.curve, raw.drift, raw.driftAngle,
		             raw.zoom, raw.spin, raw.crush, raw.pixelate, raw.warp, raw.hue, raw.bleach,
		             raw.halation, p.frames );

		for( int slot = 0; slot < p.frames; ++slot )
		{
			const Ghost g = GhostAt( slot, p );
			std::printf( "        { \"slot\": %d, \"weight\": %.9g, \"offsetX\": %.9g,"
			             " \"offsetY\": %.9g, \"scale\": %.9g, \"spin\": %.9g, \"levels\": %.9g,"
			             " \"cells\": %.9g, \"warp\": %.9g, \"hue\": %.9g, \"bleach\": %.9g,"
			             " \"halation\": %.9g }%s\n",
			             slot, g.weight, g.offsetX, g.offsetY, g.scale, g.spin, g.levels,
			             g.cells, g.warp, g.hue, g.bleach, g.halation,
			             slot + 1 < p.frames ? "," : "" );
		}
		std::printf( "      ],\n      \"schedules\": [\n" );

		//Both draw ranges -- with the live frame and without it -- across all
		//three schedules. Nine combinations per case, and the one that goes
		//wrong is always a specific one: Add with the incremental alphas was
		//the defect that started this.
		const Schedule schedules[] = { kScheduleIncremental, kScheduleProportional, kScheduleShape };
		bool firstSchedule         = true;
		for( int firstSlot = 0; firstSlot <= 1; ++firstSlot )
		{
			for( Schedule schedule : schedules )
			{
				const int count = std::max( 0, p.frames - firstSlot );
				std::vector< float > alpha( static_cast< size_t >( kMaxFrames ), 0.0f );
				std::vector< float > halation( static_cast< size_t >( kMaxFrames ), 0.0f );
				ResolveDrawAlphas( p, firstSlot, count, schedule, alpha.data(), halation.data() );

				if( !firstSchedule )
					std::printf( ",\n" );
				firstSchedule = false;

				std::printf( "        { \"firstSlot\": %d, \"schedule\": %d, \"alpha\": [",
				             firstSlot, static_cast< int >( schedule ) );
				for( int i = 0; i < count; ++i )
					std::printf( "%s%.9g", i ? "," : "", alpha[ static_cast< size_t >( i ) ] );
				std::printf( "], \"halation\": [" );
				for( int i = 0; i < count; ++i )
					std::printf( "%s%.9g", i ? "," : "", halation[ static_cast< size_t >( i ) ] );
				std::printf( "] }" );
			}
		}
		std::printf( "\n      ] }" );
	}

	std::printf( "\n  ]\n}\n" );
	return 0;
}

//---------------------------------------------------------------------------
/// Prove that footage which is not moving comes out exactly as it went in.
///
/// This is the invariant the whole weight schedule is built to hold up, and
/// it is worth a test of its own because it is the one an operator notices
/// first and cannot work around. An effect that lifts, dims or softens a
/// static shot is an effect that has to be switched off between cues.
///
/// It holds only where it claims to, and the claims are exactly:
///
///   - **every decay control at its null.** Bleach and Halation are not, by
///     default -- the plugin ships doing something visible -- so this turns
///     them off, and that is a statement about the defaults, not a loophole.
///   - **an accumulating blend.** Blend, Add and Lighten all preserve a still
///     picture. Screen deliberately does not; it is checked here for being
///     BRIGHTER and never darker, which is the honest claim for it.
///   - **Resolution at Full.** A reduced queue holds soft copies, and on a
///     still picture the ghosts are the picture. Half is checked for how far
///     it drifts rather than for equality, so the number is on the record.
///
/// Run against a card that does NOT move, unlike every other path here.
//---------------------------------------------------------------------------
int runStillCheck( int width, int height, int frames )
{
	const std::vector< unsigned char > card = buildCard( width, height, 0 );

	struct Case
	{
		const char* name;
		const char* blend;     ///< option value, as a string
		const char* resolution;///< option value
		int allowed;           ///< permitted deviation, in 8-bit steps
		bool brighterOnly;     ///< Screen: may lift, must never darken
	};
	const Case cases[] = {
		{ "Blend,   Full   ", "1", "0", 1, false },
		{ "Add,     Full   ", "0", "0", 1, false },
		{ "Lighten, Full   ", "2", "0", 1, false },
		{ "Screen,  Full   ", "3", "0", 255, true },
		//Not an equality: a reduced queue holds SOFT copies, and on a still
		//picture the ghosts are the picture, so the trail comes out softer
		//than it went in. The bound is here to keep the number on the record
		//and to catch it becoming something other than softening.
		{ "Blend,   Half   ", "1", "1", 160, false },
	};

	const GLuint sourceTexture = makeTexture( width, height, card.data() );
	const GLuint outputTexture = makeTexture( width, height, nullptr );
	const GLuint outputFBO     = makeFramebuffer( outputTexture );

	FFGLTextureStruct inputStruct = {};
	inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
	inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
	inputStruct.Handle                              = sourceTexture;
	FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

	ProcessOpenGLStruct process = {};
	process.numInputTextures    = 1;
	process.inputTextures       = inputs;
	process.HostFBO             = outputFBO;

	int failures = 0;

	for( const Case& c : cases )
	{
		Afterglow plugin;

		const char* const nulls[][ 2 ] = {
			{ "Bleach", "0" }, { "Halation", "0" }, { "Crush", "0" }, { "Pixelate", "0" },
			{ "Warp", "0" }, { "Drift", "0" }, { "Zoom", "0.5" }, { "Spin", "0.5" },
			{ "Hue Shift", "0.5" }, { "Gain", "0.5" }, { "Mix", "1" }, { "Background", "2" }
		};
		std::string error;
		for( const auto& pair : nulls )
		{
			if( applySetting( plugin, std::string( pair[ 0 ] ) + "=" + pair[ 1 ], error ) )
				continue;
			std::fprintf( stderr, "still: %s\n", error.c_str() );
			return 1;
		}
		applySetting( plugin, std::string( "Blend=" ) + c.blend, error );
		applySetting( plugin, std::string( "Resolution=" ) + c.resolution, error );

		FFGLViewportStruct viewport = {};
		viewport.width  = static_cast< FFUInt32 >( width );
		viewport.height = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "still: InitGL failed\n" );
			return 1;
		}

		for( int frame = 0; frame < frames; ++frame )
		{
			driveClock( plugin, static_cast< double >( frame ) / 60.0 );
			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "still: ProcessOpenGL failed\n" );
				return 1;
			}
		}

		//NOT flipped, unlike the paths that write a PNG. The card was uploaded
		//to the texture as it stands, so GL has been treating its first row as
		//the bottom one all along and the readback comes back in the same
		//order the card is in. Flipping here -- which the first version of this
		//check did, by copying the line out of the render path -- compares the
		//top of the output against the bottom of the card and reports a
		//difference of 242/255 on a plugin that is behaving perfectly.
		const std::vector< unsigned char > out = readBackRaw( outputFBO, width, height );
		plugin.DeInitGL();

		int worst   = 0;
		int darkest = 0;
		for( size_t i = 0; i < out.size(); ++i )
		{
			if( i % 4 == 3 )
				continue;//alpha: the card is opaque and stays so
			const int delta = int( out[ i ] ) - int( card[ i ] );
			worst           = std::max( worst, std::abs( delta ) );
			darkest         = std::min( darkest, delta );
		}

		const bool ok = c.brighterOnly ? ( darkest >= -1 ) : ( worst <= c.allowed );
		std::printf( "still %s  worst %3d/255, darkest %4d  %s\n",
		             c.name, worst, darkest, ok ? "ok" : "FAILED" );
		if( !ok )
			++failures;
	}

	glDeleteFramebuffers( 1, &outputFBO );
	glDeleteTextures( 1, &outputTexture );
	glDeleteTextures( 1, &sourceTexture );

	std::printf( "%s\n", failures == 0 ? "still: a picture that does not move is not touched"
	                                    : "still: FAILURES" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
/// Prove a factory preset survives whatever the host does next.
///
/// FFGL's host owns parameter state and is free to push it back down at any
/// time, and nothing in the specification obliges it to act on the value
/// events a plugin raises when it changes a parameter itself. So there are
/// three hosts to survive, and the plugin cannot tell which one it is talking
/// to:
///
///   - one that honours the events and hands the new values straight back;
///   - one that ignores them and carries on restating the values it still
///     believes in, which are the ones from before the preset;
///   - one that honours them but keeps its parameters shorter than a float,
///     so what comes back is near the preset rather than equal to it.
///
/// All three arrive as SetFloatParameter calls carrying a changed value,
/// which is why "the value changed, so the operator must have taken over" is
/// the wrong test. Resolume is the second kind, and against the pattern this
/// plugin was copied from it fails in exactly that column -- reported as
/// vertigo issue #2.
///
/// No GL here: this is the parameter plumbing, not the picture.
//---------------------------------------------------------------------------
int runPresetTest()
{
	using namespace afterglow::presets;

	int coveredCount            = 0;
	const unsigned int* covered = Afterglow::PresetParamIDsForTest( coveredCount );

	enum class Host
	{
		Honours,
		Ignores,
		Quantises
	};
	struct HostCase
	{
		Host kind;
		const char* name;
	};
	const HostCase hosts[] = {
		{ Host::Honours, "honours value events" },
		{ Host::Ignores, "ignores value events" },
		{ Host::Quantises, "honours, 1/1000 steps" },
	};

	int failures = 0;

	for( const HostCase& host : hosts )
	{
		for( int preset = 1; preset <= kCount; ++preset )
		{
			Afterglow plugin;

			int presetIndex = -1;
			for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
			{
				const char* declared = plugin.GetParamName( i );
				if( declared != nullptr && std::strcmp( declared, "Preset" ) == 0 )
				{
					presetIndex = int( i );
					break;
				}
			}
			if( presetIndex < 0 )
			{
				std::fprintf( stderr, "presets: no parameter is called \"Preset\"\n" );
				return 1;
			}

			// What the host thinks the sliders say before the operator reaches
			// for the dropdown.
			std::vector< float > hostOwn;
			for( int j = 0; j < coveredCount; ++j )
				hostOwn.push_back( plugin.GetFloatParameter( covered[ j ] ) );

			// The operator picks a preset.
			plugin.SetFloatParameter( unsigned( presetIndex ), float( preset ) );

			// And now the host says its piece.
			for( int j = 0; j < coveredCount; ++j )
			{
				float back = 0.0f;
				switch( host.kind )
				{
				case Host::Honours:
					back = plugin.GetFloatParameter( covered[ j ] );
					break;
				case Host::Ignores:
					back = hostOwn[ size_t( j ) ];
					break;
				case Host::Quantises:
					back = std::round( plugin.GetFloatParameter( covered[ j ] ) * 1000.0f ) / 1000.0f;
					break;
				}
				plugin.SetFloatParameter( covered[ j ], back );
			}

			const int still = int( std::lround( plugin.GetFloatParameter( unsigned( presetIndex ) ) ) );
			bool ok         = still == preset;

			// Still selected is not enough -- it has to be what renders.
			for( int j = 0; j < coveredCount; ++j )
			{
				const float want = kPresets[ preset - 1 ].v[ j ];
				const float got  = plugin.GetFloatParameter( covered[ j ] );
				ok               = ok && std::fabs( got - want ) <= 1e-4f;
			}

			if( !ok )
			{
				std::printf( "presets %-22s %-22s FAILED (shows %d)\n",
				             host.name, kPresets[ preset - 1 ].name, still );
				++failures;
				continue;
			}

			// An operator turning a covered knob must still drop to Custom --
			// a preset that cannot be left is no better than one that will not
			// stick. Move it somewhere neither the preset nor the host named.
			const float moved = kPresets[ preset - 1 ].v[ 0 ] > 0.5f ? 0.123f : 0.877f;
			plugin.SetFloatParameter( covered[ 0 ], moved );
			const int after = int( std::lround( plugin.GetFloatParameter( unsigned( presetIndex ) ) ) );
			if( after != 0 )
			{
				std::printf( "presets %-22s %-22s FAILED (an edit left it on %d)\n",
				             host.name, kPresets[ preset - 1 ].name, after );
				++failures;
				continue;
			}

			std::printf( "presets %-22s %-22s ok\n", host.name, kPresets[ preset - 1 ].name );
		}
	}

	std::printf( "%s\n", failures == 0 ? "presets: all ok" : "presets: FAILURES" );
	return failures == 0 ? 0 : 1;
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/afterglow.png";
	std::string cardPath;
	std::string scriptPath;
	int width    = 1280;
	int height   = 720;
	//Forty rather than a dozen: the queue is up to thirty-two frames long and
	//a picture taken before it has filled is a picture of the buffers still
	//being empty rather than of the effect.
	int frames   = 40;
	double fps   = 60.0;
	float noise  = 0.0f;
	bool wantList  = false;
	bool wantDecay = false;
	bool wantStill = false;
	bool wantGhosts = false;
	bool wantBench = false;
	bool wantPipe  = false;
	std::vector< std::string > settings;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;

		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--card" && hasNext )
			cardPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--noise" && hasNext )
			noise = std::strtof( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--presets" )
			return runPresetTest();
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--decay" )
			wantDecay = true;
		else if( argument == "--still" )
			wantStill = true;
		else if( argument == "--ghosts" )
			wantGhosts = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	//No GL needed, so it is answered before a context is made -- which also
	//means it still works on a machine where creating one fails, and in CI.
	if( wantGhosts )
		return runGhostDump();

	if( !cardPath.empty() )
	{
		const std::vector< unsigned char > card = buildCard( width, height, 0 );
		if( !writePng( cardPath, width, height, card ) )
		{
			std::fprintf( stderr, "could not write %s\n", cardPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s\n", cardPath.c_str() );
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}

	if( wantDecay )
	{
		const int result = runDecayCheck();
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	}

	if( wantStill )
	{
		const int result = runStillCheck( width, height, frames );
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	}

	Afterglow plugin;

	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return 2;
	}

	if( wantList )
	{
		std::printf( "%-3s %-16s %s\n", "id", "name", "default" );
		for( const NamedParameter& parameter : listParameters( plugin ) )
			std::printf( "%-3u %-16s %.4f\n", parameter.index, parameter.name.c_str(), parameter.value );
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return 0;
	}

	if( wantBench )
	{
		FFGLViewportStruct benchViewport = {};
		benchViewport.width  = static_cast< FFUInt32 >( width );
		benchViewport.height = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &benchViewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return 1;
		}
		const int result = runBench( plugin, frames, fps );
		plugin.DeInitGL();
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	}

	FFGLViewportStruct viewport = {};
	viewport.x                  = 0;
	viewport.y                  = 0;
	viewport.width              = static_cast< FFUInt32 >( width );
	viewport.height             = static_cast< FFUInt32 >( height );

	if( plugin.InitGL( &viewport ) != FF_SUCCESS )
	{
		std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
		return 1;
	}

	GLuint sourceTexture   = makeTexture( width, height, buildCard( width, height, 0 ).data() );
	GLuint outputTexture   = makeTexture( width, height, nullptr );
	const GLuint outputFBO = makeFramebuffer( outputTexture );

	FFGLTextureStruct inputStruct = {};
	inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
	inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
	inputStruct.Handle                              = sourceTexture;
	FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

	ProcessOpenGLStruct process = {};
	process.numInputTextures    = 1;
	process.inputTextures       = inputs;
	process.HostFBO             = outputFBO;

	if( wantPipe )
	{
		//Raw RGBA in, raw RGBA out, one frame at a time.
		//
		//Resolve the script's parameter names to indices once, up front, and
		//refuse to run on a name that is not a parameter. A misspelled name
		//that silently did nothing would produce a take that looks deliberate
		//and is wrong -- the reel would hold whatever the default was, with a
		//caption over it describing the control that never moved.
		std::map< unsigned int, Track > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return 2;
			}

			const std::vector< NamedParameter > known = listParameters( plugin );
			for( const auto& entry : tracks )
			{
				bool found = false;
				for( const NamedParameter& parameter : known )
				{
					if( parameter.name != entry.first )
						continue;
					automation[ parameter.index ] = entry.second;
					found                         = true;
					break;
				}
				if( !found )
				{
					std::fprintf( stderr,
					              "script names '%s', which is not a parameter (try --list)\n",
					              entry.first.c_str() );
					return 2;
				}
			}
		}

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );

		for( int index = 0;; ++index )
		{
			size_t filled = 0;
			while( filled < frame.size() )
			{
				const ssize_t got = read( STDIN_FILENO, frame.data() + filled, frame.size() - filled );
				if( got <= 0 )
					break;
				filled += static_cast< size_t >( got );
			}
			if( filled < frame.size() )
				break;

			for( const auto& track : automation )
				plugin.SetFloatParameter( track.first, valueAt( track.second, index ) );

			//Same synthetic clock as the still path, so a filmed sequence
			//advances at the frame rate it will be played back at rather than
			//at whatever rate the pipe happens to deliver -- a stall in ffmpeg
			//must not show up as the warp field speeding up afterwards.
			driveClock( plugin, static_cast< double >( index ) / fps );

			//Flipped on the way in because a raw frame arrives top row first
			//and GL wants bottom row first.
			const std::vector< unsigned char > flipped = flipRows( frame, width, height );
			glBindTexture( GL_TEXTURE_2D, sourceTexture );
			glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
			glBindTexture( GL_TEXTURE_2D, 0 );

			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
				break;

			const std::vector< unsigned char > out = flipRows( readBackRaw( outputFBO, width, height ), width, height );
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
		}

		plugin.DeInitGL();
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return 0;
	}

	//A still, at the end of a run of frames. Several frames and not one, and
	//more of them than any other plugin in the fleet needs: the queue is up
	//to thirty-two slots long and every one of them has to have been filled
	//by a DIFFERENT picture before what comes out is the effect rather than a
	//picture of the buffers warming up.
	for( int frame = 0; frame < frames; ++frame )
	{
		//A synthetic clock, and it has to be synthetic. Left to the wall clock
		//the harness renders a hundred frames in a few milliseconds, so no
		//time passes, the warp field stays frozen and Warp Speed measurably
		//does nothing -- and what little time DID pass would be whatever the
		//machine happened to take, so no two runs would produce the same
		//picture and nothing here could be compared against anything.
		driveClock( plugin, static_cast< double >( frame ) / fps );

		//THE CARD MOVES. See the note on buildCard: on a still card every slot
		//holds the same picture and the trail is provably invisible.
		std::vector< unsigned char > source = buildCard( width, height, frame );
		if( noise > 0.0f )
			source = addNoise( source, frame, noise );

		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, source.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "ProcessOpenGL failed on frame %d\n", frame );
			return 1;
		}
	}

	const std::vector< unsigned char > image = flipRows( readBackRaw( outputFBO, width, height ), width, height );
	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return 1;
	}

	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );

	plugin.DeInitGL();
	glDeleteFramebuffers( 1, &outputFBO );
	glDeleteTextures( 1, &outputTexture );
	glDeleteTextures( 1, &sourceTexture );
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return 0;
}
