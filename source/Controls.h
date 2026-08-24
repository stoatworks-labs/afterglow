#pragma once

/**
	Host parameters are 0..1; these are what they mean.

	Every numeric parameter this plugin declares is a plain FF_TYPE_STANDARD
	float in 0..1, including the ones that stand for a frame count or a number
	of turns. That is not a style preference.
	`CFFGLPluginManager::SetParamInfo` clamps a standard default into 0..1
	*before* returning, and `SetParamRange` can only be called afterwards
	because it finds the parameter by ID -- so a parameter declared in frames
	cannot declare a default in frames, and 12 would silently become 1. The
	conversions live here instead, in one file that the plugin, the OpenFX
	build and the test harness all use, so there is only ever one answer to
	what a slider position means.

	Where a mapping is geometric rather than linear it is because the
	interesting range is at one end. A slider is a fixed number of pixels wide,
	and spending half of them on a range nobody uses is the difference between
	a control that works and one with a sweet spot at 0.03.

	Where a mapping is **bipolar** -- Zoom, Spin, Hue Shift -- 0.5 is the
	null and the two halves are mirror images. Those are the controls where
	the interesting question is "which way", and a unipolar version of any of
	them would need a second Reverse checkbox to say the same thing worse.
*/
namespace afterglow
{

/// 4 to 32 frames in the queue, geometrically, rounded.
///
/// Four is the floor and not two, because the oldest slot in the queue is
/// deliberately weighted to zero -- it is the frame on its way out, and
/// letting it leave at zero rather than at whatever it happened to be is what
/// stops the trail flickering at its own tail. At two that convention would
/// leave one live frame and one invisible one, i.e. no trail at all.
///
/// Geometric because the difference between 5 frames and 8 is a different
/// effect and the difference between 29 and 32 is not.
int FramesFromParam( float value );

/// 1 to 8 host frames per queue slot, linear, rounded.
///
/// The trail spans Frames x Hold host frames, so this is how a queue of a
/// dozen buffers covers two seconds. It costs nothing: holding is not
/// rendering the same thing more often, it is capturing less often.
int HoldFromParam( float value );

/// 0.25 to 4.0, geometrically. The exponent on the weight falloff.
///
/// Below 1 the trail hangs on and drops away late -- long, even, filmic.
/// Above 1 it falls off immediately and only the last few frames read, which
/// is the tight double-image an interlaced monitor gives. 1.0 is linear.
float CurveFromParam( float value );

/// 0 to 2. Over 1 on purpose: the trail is being added to a picture that is
/// already there, and a screen blend that never clips reads as haze rather
/// than as light.
float GainFromParam( float value );

/// How far the oldest ghost has travelled, as a fraction of the picture
/// width: 0 to 0.25, geometrically. A quarter of the frame is a long way --
/// past that the trail is off the edge and the control is only throwing
/// pixels away.
float DriftFromParam( float value );

/// 0 to 1 turn. Which way Drift goes.
float DriftAngleFromParam( float value );

/// -0.5 to +0.5, bipolar about 0.5. The oldest ghost's scale change: negative
/// pulls the trail inward (the frame receding into the picture), positive
/// pushes it out past the edges.
float ZoomFromParam( float value );

/// -0.25 to +0.25 turns, bipolar about 0.5. The oldest ghost's rotation.
float SpinFromParam( float value );

/// 0 to 1, linear, and used as a fraction of the way from 8 bits to 1.
/// The bit depth of the oldest ghost is `exp2( 8 - 7 * Crush )`.
float CrushFromParam( float value );

/// 0 to 1, linear. Used geometrically inside the ghost: the oldest ghost's
/// grid goes from the picture's own resolution down to 6 cells across.
float PixelateFromParam( float value );

/// 0 to 0.08 of the picture width, geometrically: the displacement amplitude
/// the oldest ghost is warped by.
float WarpFromParam( float value );

/// 1 to 64 cells of noise across the picture, geometrically. Low is a slow
/// swell that moves the whole ghost; high is boiling grain.
float WarpScaleFromParam( float value );

/// 0 to 2 cycles per second, linear, with zero meaning frozen. Not bipolar:
/// noise has no direction to reverse.
float WarpSpeedFromParam( float value );

/// -0.5 to +0.5 turns, bipolar about 0.5. How far round the wheel the oldest
/// ghost's hue has gone. Half a turn is the complement, which is the point at
/// which a trail stops reading as the same picture and starts reading as a
/// second one.
float HueFromParam( float value );

/// 0 to 1, linear. Saturation the oldest ghost has lost.
float BleachFromParam( float value );

/// 0 to 2. How much of the halation buffer is added back.
float HalationFromParam( float value );

/// 0.5 to 12, geometrically: the halation's radius as a fraction of the
/// picture width, in per-mille.
float HalationSizeFromParam( float value );

/// 0 to 1, linear. Pushes the halation from neutral towards amber -- the
/// colour a real halation ring is, because it is red light scattering back
/// through the film base and the emulsion stops the blue first.
float HalationTintFromParam( float value );

/// The divisor the frame queue is stored at: 1, 2, 4 or 8.
///
/// Not a slider. It is an option parameter, because the memory this plugin
/// holds is Frames x picture x 4 bytes divided by the square of this, and the
/// difference between 1 and 2 at 4K with a full queue is a gigabyte against
/// two hundred and fifty megabytes.
int ResolutionDivisor( float optionValue );

} // namespace afterglow
