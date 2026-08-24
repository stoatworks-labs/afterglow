#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace afterglow
{
namespace
{
inline float clamp01( float value )
{
	return std::min( std::max( value, 0.0f ), 1.0f );
}

inline float lerp( float from, float to, float t )
{
	return from + ( to - from ) * clamp01( t );
}

/// Geometric interpolation. Equal slider movements are equal *ratios*, which
/// is the right behaviour for any quantity where the question is "how many
/// times more" rather than "how much more".
inline float geometric( float from, float to, float t )
{
	return from * std::pow( to / from, clamp01( t ) );
}

/// A bipolar control: 0.5 is the null, and each half maps to one direction.
///
/// Written as a signed magnitude rather than as `lerp( -limit, limit, v )`
/// because the two are not the same thing at the ends of a *geometric*
/// mapping, and because this way the null is exactly 0 at exactly 0.5 rather
/// than within a float's worth of it. A trail that drifts imperceptibly with
/// every control at its default is the kind of defect nobody reports and
/// everybody works around.
inline float bipolar( float value, float limit )
{
	const float signed_ = clamp01( value ) * 2.0f - 1.0f;
	return signed_ * limit;
}
} // namespace

int FramesFromParam( float value )
{
	return static_cast< int >( std::lround( geometric( 4.0f, 32.0f, value ) ) );
}

int HoldFromParam( float value )
{
	return static_cast< int >( std::lround( lerp( 1.0f, 8.0f, value ) ) );
}

float CurveFromParam( float value )
{
	return geometric( 0.25f, 4.0f, value );
}

float GainFromParam( float value )
{
	return lerp( 0.0f, 2.0f, value );
}

float DriftFromParam( float value )
{
	//Geometric from a floor rather than from zero, and then floored to zero at
	//the very bottom of the travel: a geometric mapping cannot reach zero, and
	//a Drift control that never quite stops drifting is a control that has to
	//be fought rather than used.
	if( value <= 0.0f )
		return 0.0f;

	return geometric( 0.002f, 0.25f, value );
}

float DriftAngleFromParam( float value )
{
	return clamp01( value );
}

float ZoomFromParam( float value )
{
	return bipolar( value, 0.5f );
}

float SpinFromParam( float value )
{
	return bipolar( value, 0.25f );
}

float CrushFromParam( float value )
{
	return clamp01( value );
}

float PixelateFromParam( float value )
{
	return clamp01( value );
}

float WarpFromParam( float value )
{
	if( value <= 0.0f )
		return 0.0f;

	return geometric( 0.0004f, 0.08f, value );
}

float WarpScaleFromParam( float value )
{
	return geometric( 1.0f, 64.0f, value );
}

float WarpSpeedFromParam( float value )
{
	return lerp( 0.0f, 2.0f, value );
}

float HueFromParam( float value )
{
	return bipolar( value, 0.5f );
}

float BleachFromParam( float value )
{
	return clamp01( value );
}

float HalationFromParam( float value )
{
	return lerp( 0.0f, 2.0f, value );
}

float HalationSizeFromParam( float value )
{
	return geometric( 0.5f, 12.0f, value );
}

float HalationTintFromParam( float value )
{
	return clamp01( value );
}

int ResolutionDivisor( float optionValue )
{
	//The option's stored VALUE, not its position in the list -- the list is
	//declared alphabetically and Eighth sorts first. See the declaration in
	//Afterglow.cpp.
	switch( static_cast< int >( std::lround( optionValue ) ) )
	{
	case 0: return 1;
	case 1: return 2;
	case 2: return 4;
	case 3: return 8;
	default: return 2;
	}
}

} // namespace afterglow
