#include "Decay.h"

#include <algorithm>
#include <cmath>

namespace afterglow
{
namespace
{
constexpr float kTau = 6.283185307179586f;

inline float clampf( float v, float lo, float hi )
{
	return std::min( std::max( v, lo ), hi );
}

/// Ken Perlin's second smoothstep. Continuous in the second derivative, which
/// on a noise field is the difference between a warp that flows and one with
/// visible lattice creases running through it.
inline float smootherstep( float f )
{
	return f * f * f * ( f * ( f * 6.0f - 15.0f ) + 10.0f );//= mirrored
}

/// One lattice corner, in 0..1.
inline float lattice( int32_t ix, int32_t iy, int32_t iz, uint32_t salt )
{
	//= mirrored
	const uint32_t seed = static_cast< uint32_t >( ix ) * 374761393u
	                      + static_cast< uint32_t >( iy ) * 668265263u
	                      + static_cast< uint32_t >( iz ) * 2246822519u
	                      + salt * 3266489917u;
	return Hash01( seed );
}

/// Trilinear value noise, in 0..1. The third axis is time, so the field
/// *boils* rather than sliding past -- a warp that only translates reads as
/// the ghost being pushed by something, which is a different effect.
float valueNoise3( float x, float y, float z, uint32_t salt )
{
	//= mirrored
	const float fx = std::floor( x );
	const float fy = std::floor( y );
	const float fz = std::floor( z );

	const int32_t ix = static_cast< int32_t >( fx );
	const int32_t iy = static_cast< int32_t >( fy );
	const int32_t iz = static_cast< int32_t >( fz );

	const float ax = smootherstep( x - fx );
	const float ay = smootherstep( y - fy );
	const float az = smootherstep( z - fz );

	const float c000 = lattice( ix, iy, iz, salt );
	const float c100 = lattice( ix + 1, iy, iz, salt );
	const float c010 = lattice( ix, iy + 1, iz, salt );
	const float c110 = lattice( ix + 1, iy + 1, iz, salt );
	const float c001 = lattice( ix, iy, iz + 1, salt );
	const float c101 = lattice( ix + 1, iy, iz + 1, salt );
	const float c011 = lattice( ix, iy + 1, iz + 1, salt );
	const float c111 = lattice( ix + 1, iy + 1, iz + 1, salt );

	const float x00 = c000 + ( c100 - c000 ) * ax;
	const float x10 = c010 + ( c110 - c010 ) * ax;
	const float x01 = c001 + ( c101 - c001 ) * ax;
	const float x11 = c011 + ( c111 - c011 ) * ax;

	const float y0 = x00 + ( x10 - x00 ) * ay;
	const float y1 = x01 + ( x11 - x01 ) * ay;

	return y0 + ( y1 - y0 ) * az;
}

/// Two octaves, signed, roughly -1..1.
float field( float x, float y, float z, uint32_t salt )
{
	//= mirrored
	const float a = valueNoise3( x, y, z, salt );
	const float b = valueNoise3( x * 2.17f + 11.3f, y * 2.17f - 7.1f, z * 1.63f + 3.7f, salt + 7u );
	return ( a * 0.65f + b * 0.35f ) * 2.0f - 1.0f;
}
} // namespace

//---------------------------------------------------------------------------
uint32_t HashInt( uint32_t seed )
{
	//= mirrored
	const uint32_t state = seed * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

float Hash01( uint32_t seed )
{
	//= mirrored
	return static_cast< float >( HashInt( seed ) ) * 2.3283064365386963e-10f;
}

//---------------------------------------------------------------------------
float AgeOf( int slot, int frames )
{
	if( frames <= 1 )
		return 0.0f;

	return clampf( static_cast< float >( slot ) / static_cast< float >( frames - 1 ), 0.0f, 1.0f );
}

//---------------------------------------------------------------------------
Ghost GhostAt( int slot, const DecayParams& p )
{
	Ghost g;

	const int frames = std::max( 2, p.frames );
	const float t    = AgeOf( slot, frames );

	//-----------------------------------------------------------------------
	// Weight.
	//
	// Two things are being arranged here and they pull in opposite
	// directions.
	//
	// The **oldest slot must be worth exactly zero**, because it is the frame
	// that will be evicted at the next capture. Give it any visible weight and
	// every capture removes something the eye was looking at, and a trail that
	// loses its tail once per frame flickers at the capture rate -- which
	// reads as the plugin dropping frames rather than as a design decision.
	//
	// The **newest slot must be worth exactly one**, because it is the live
	// picture and an effect that dims what it was given is an effect nobody
	// trusts. `pow( 1 - u, curve )` on its own does not manage that: at four
	// frames it hands the live frame 0.75^curve.
	//
	// So the falloff runs to zero across the queue and is then normalised by
	// its own value at the head. Both ends are exact by construction rather
	// than by arithmetic that happens to land near them.
	//-----------------------------------------------------------------------
	const float u    = static_cast< float >( slot + 1 ) / static_cast< float >( frames );
	const float head = static_cast< float >( frames - 1 ) / static_cast< float >( frames );
	const float fall = std::pow( std::max( 0.0f, 1.0f - u ), p.curve );
	const float norm = std::pow( head, p.curve );

	g.weight = norm > 0.0f ? fall / norm : 0.0f;

	//-----------------------------------------------------------------------
	// Motion. Both offsets are in picture WIDTHS -- see GhostSampleUV.
	//-----------------------------------------------------------------------
	const float angle = p.driftAngle * kTau;
	g.offsetX         = std::cos( angle ) * p.drift * t;
	g.offsetY         = std::sin( angle ) * p.drift * t;
	g.scale           = 1.0f + p.zoom * t;
	g.spin            = p.spin * t * kTau;

	//-----------------------------------------------------------------------
	// Bit crushing.
	//
	// Levels, not bits: `exp2( 8 - 7 * crush * t )` runs 256 down to 2 and is
	// continuous the whole way, so dragging the control is a smooth loss of
	// resolution rather than eight discrete steps with nothing in between.
	//-----------------------------------------------------------------------
	g.levels = std::exp2( 8.0f - 7.0f * p.crush * t );

	//-----------------------------------------------------------------------
	// Pixelation. Geometric, from finer than any picture down to six cells
	// across, and exactly zero -- meaning "do not" -- when the control is off,
	// because a geometric mapping never reaches its own top.
	//-----------------------------------------------------------------------
	const float coarse = p.pixelate * t;
	g.cells            = coarse > 0.0f ? 2048.0f * std::pow( 6.0f / 2048.0f, coarse ) : 0.0f;

	//-----------------------------------------------------------------------
	// Colour and warp: all straight lines in age.
	//-----------------------------------------------------------------------
	g.warp   = p.warp * t;
	g.hue    = p.hue * t;
	g.bleach = p.bleach * t;

	//-----------------------------------------------------------------------
	// Halation.
	//
	// Scaled by the age AND by the weight, so it peaks in the middle of the
	// queue rather than at either end. That is not a compromise between the
	// two, it is what halation is: the newest frame has not aged into
	// anything, and the oldest is not there any more. What blooms is a ghost
	// on its way out -- which is also the only place a glow can be seen, since
	// added on top of a frame at full strength it is invisible.
	//-----------------------------------------------------------------------
	g.halation = p.halation * t * g.weight;

	return g;
}

//---------------------------------------------------------------------------
void ResolveDrawAlphas( const DecayParams& p, int firstSlot, int count, Schedule schedule,
                        float* outAlpha, float* outHalation )
{
	if( count <= 0 )
		return;

	float total = 0.0f;
	for( int i = 0; i < count; ++i )
		total += GhostAt( firstSlot + i, p ).weight;

	switch( schedule )
	{
	case kScheduleIncremental:
	{
		//Oldest first, which is also the order they are drawn in. The two have
		//to agree: this schedule IS the draw order written down, and running
		//it against ghosts composited newest-first would produce a weighted
		//average of the wrong weights.
		float running = 0.0f;
		for( int i = count - 1; i >= 0; --i )
		{
			const float w = GhostAt( firstSlot + i, p ).weight;
			running += w;
			outAlpha[ i ] = running > 0.0f ? w / running : 0.0f;
		}
		break;
	}

	case kScheduleProportional:
		for( int i = 0; i < count; ++i )
			outAlpha[ i ] = total > 0.0f ? GhostAt( firstSlot + i, p ).weight / total : 0.0f;
		break;

	case kScheduleShape:
	default:
		for( int i = 0; i < count; ++i )
			outAlpha[ i ] = GhostAt( firstSlot + i, p ).weight;
		break;
	}

	for( int i = 0; i < count; ++i )
		outHalation[ i ] = total > 0.0f ? GhostAt( firstSlot + i, p ).halation / total : 0.0f;
}

//---------------------------------------------------------------------------
void WarpOffset( float x, float y, float phase, float& outX, float& outY )
{
	outX = field( x, y, phase, 0u );//= mirrored
	outY = field( x, y, phase, 1u );//= mirrored
}

//---------------------------------------------------------------------------
void GhostSampleUV( const Ghost& g, float u, float v, float aspect,
                    float warpScale, float warpPhase, float& outU, float& outV )
{
	//= mirrored
	//Into picture widths on both axes, centred. See the declaration.
	float px = u - 0.5f;
	float py = ( v - 0.5f ) / aspect;

	//The INVERSE of the ghost's own transform: this is a fetch, so a ghost
	//that was pushed right is sampled to the left. Getting the sign wrong here
	//is not obviously wrong on screen -- the trail still trails, it just goes
	//the way the arrow says it should not.
	px -= g.offsetX;
	py -= g.offsetY;

	const float c = std::cos( -g.spin );
	const float s = std::sin( -g.spin );
	const float rx = px * c - py * s;
	const float ry = px * s + py * c;

	const float scale = g.scale != 0.0f ? g.scale : 1.0f;
	px                = rx / scale;
	py                = ry / scale;

	//Warp, sampled at the point being fetched rather than at the point being
	//drawn, so the field is attached to the picture and not to the screen.
	if( g.warp > 0.0f )
	{
		float dx = 0.0f;
		float dy = 0.0f;
		WarpOffset( px * warpScale, py * warpScale, warpPhase, dx, dy );
		px += dx * g.warp;
		py += dy * g.warp;
	}

	outU = px + 0.5f;
	outV = py * aspect + 0.5f;

	//Pixelation last, so the cells are square on screen and stay put while
	//the ghost drifts underneath them. Snapping before the transform would
	//give a grid that slides and shears with the ghost, which reads as a
	//compression artefact rather than as a resolution.
	if( g.cells > 0.0f )
	{
		const float cellsX = g.cells;
		const float cellsY = g.cells / aspect;
		outU               = ( std::floor( outU * cellsX ) + 0.5f ) / cellsX;
		outV               = ( std::floor( outV * cellsY ) + 0.5f ) / cellsY;
	}
}

//---------------------------------------------------------------------------
void DegradeColour( const Ghost& g, float rgba[ 4 ] )
{
	//= mirrored
	const float a = rgba[ 3 ];

	//Un-premultiply first. Quantising premultiplied colour quantises the
	//COVERAGE into the colour, so a soft edge acquires bands that follow the
	//alpha rather than the picture -- and on a clip with no alpha at all,
	//which is most of them, the two are identical and the bug is invisible
	//until somebody puts a keyed layer through it.
	float r = a > 0.0031f ? rgba[ 0 ] / a : rgba[ 0 ];
	float gg = a > 0.0031f ? rgba[ 1 ] / a : rgba[ 1 ];
	float b = a > 0.0031f ? rgba[ 2 ] / a : rgba[ 2 ];

	if( g.levels < 255.5f )
	{
		//Round, do not truncate. Truncation is a half-level darkening as well
		//as a quantisation, so the trail would go dim as it went chunky for a
		//reason that has nothing to do with the weight curve.
		const float steps = std::max( 1.0f, g.levels - 1.0f );
		r                 = std::floor( r * steps + 0.5f ) / steps;
		gg                = std::floor( gg * steps + 0.5f ) / steps;
		b                 = std::floor( b * steps + 0.5f ) / steps;
	}

	//Hue, in YIQ. The matrix is the NTSC one and the luma it produces is used
	//for Bleach as well -- one definition of grey per pass, even though Rec709
	//would be the more modern choice for the luma on its own. Two different
	//lumas inside one function is the kind of thing that is right in both
	//halves and wrong overall.
	const float y = 0.299f * r + 0.587f * gg + 0.114f * b;

	if( g.hue != 0.0f )
	{
		const float i = 0.596f * r - 0.274f * gg - 0.322f * b;
		const float q = 0.211f * r - 0.523f * gg + 0.312f * b;

		const float angle = g.hue * kTau;
		const float cs    = std::cos( angle );
		const float sn    = std::sin( angle );

		const float i2 = i * cs - q * sn;
		const float q2 = i * sn + q * cs;

		r  = y + 0.956f * i2 + 0.621f * q2;
		gg = y - 0.272f * i2 - 0.647f * q2;
		b  = y - 1.106f * i2 + 1.703f * q2;
	}

	if( g.bleach > 0.0f )
	{
		const float keep = 1.0f - g.bleach;
		r                = y + ( r - y ) * keep;
		gg               = y + ( gg - y ) * keep;
		b                = y + ( b - y ) * keep;
	}

	rgba[ 0 ] = r * a;
	rgba[ 1 ] = gg * a;
	rgba[ 2 ] = b * a;
	//Alpha is deliberately untouched. Crushing it as well turns every soft
	//edge in the trail into a stencil, which looks like a bug in the keyer
	//rather than like an old frame.
}

} // namespace afterglow
