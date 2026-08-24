#pragma once

#include <cstdint>

/**
	What happens to a frame as it ages.

	**The one idea.** Every degradation this plugin applies -- how much a ghost
	is worth, where it has drifted to, how many bits it has left, how far round
	the wheel its hue has gone -- is a function of one number: `t`, the ghost's
	age, 0 for the frame that just arrived and 1 for the one about to leave the
	queue. Nothing in `GhostAt` looks at a pixel. That is what makes the decay
	model checkable: it is arithmetic, it has one right answer, and the answer
	does not depend on a picture.

	**Why a queue and not a feedback loop.** The obvious way to build this is
	recursive: keep one buffer, blend each new frame into it, show the result.
	That is one texture instead of thirty-two and it produces a genuinely
	similar picture -- an exponential trail is exactly what a recursive blend
	makes, and Decay at a curve of 1 over a long queue is its truncated impulse
	response.

	It is not what this does, because the moment a frame's *own* age is allowed
	to change how it looks -- crushed further, drifted further, hue-shifted
	further the older it gets -- the frames have to still exist separately.
	Fold them into an accumulator and the degradation compounds instead:
	yesterday's crush gets crushed again today, the drift integrates into a
	smear, and there is no way back to the picture. A queue costs memory and
	buys the thing the effect is for.

	----------------------------------------------------- what exists twice

	`GhostAt` exists **once**. It runs per slot, not per pixel -- a couple of
	dozen floating-point operations, at most thirty-two times a frame -- so
	both builds simply call it and the GPU is handed the answers as uniforms.
	A model that cannot drift is worth more than one that is checked.

	The **per-pixel** stage is the part that genuinely has to exist twice, in
	GLSL for the GPU and here for the OpenFX build's CPU render:
	`GhostSampleUV` and `DegradeColour`, and the noise `WarpOffset` is built
	on. Every mirrored line carries a `//= mirrored` marker in both this file
	and `kDecayLibrary` in `Shaders.cpp`. `agtest --decay` runs both over the
	whole parameter space and compares them.

	That split is deliberate and it is most of the design: the surface where
	two copies can disagree is three functions wide instead of the whole
	effect.
*/
namespace afterglow
{

/// The decay controls, already converted out of the host's 0..1 by Controls.h.
struct DecayParams
{
	int frames       = 12;   ///< slots in the queue, >= 2
	float curve      = 1.0f; ///< weight falloff exponent
	float drift      = 0.0f; ///< picture widths the oldest ghost has moved
	float driftAngle = 0.0f; ///< turns
	float zoom       = 0.0f; ///< -0.5..0.5, the oldest ghost's scale change
	float spin       = 0.0f; ///< turns, the oldest ghost's rotation
	float crush      = 0.0f; ///< 0..1 of the way from 8 bits to 1
	float pixelate   = 0.0f; ///< 0..1
	float warp       = 0.0f; ///< picture widths of displacement
	float hue        = 0.0f; ///< turns
	float bleach     = 0.0f; ///< 0..1 of saturation lost
	float halation   = 0.0f; ///< 0..2, how brightly a ghost feeds the glow
};

/// Everything one slot of the queue needs, resolved.
struct Ghost
{
	float weight   = 0.0f;  ///< opacity/gain, 1 for the live frame
	float offsetX  = 0.0f;  ///< translation, in picture WIDTHS on both axes
	float offsetY  = 0.0f;
	float scale    = 1.0f;  ///< about the picture centre
	float spin     = 0.0f;  ///< radians, about the picture centre
	float levels   = 256.0f;///< quantisation levels per channel; >= 256 is untouched
	float cells    = 0.0f;  ///< pixelation grid across the width; 0 is untouched
	float warp     = 0.0f;  ///< displacement amplitude, picture widths
	float hue      = 0.0f;  ///< turns
	float bleach   = 0.0f;  ///< 0..1
	float halation = 0.0f;  ///< this ghost's contribution to the glow
};

/// The age of a slot: 0 for the frame that just arrived, 1 for the oldest.
float AgeOf( int slot, int frames );

/// The whole per-slot decay model. `slot` is 0 for the newest frame.
///
/// One copy, called by both builds. See the note at the top of this file.
Ghost GhostAt( int slot, const DecayParams& p );

/// Which arithmetic a blend uses to put one ghost on top of the last, and so
/// which weights it has to be handed.
enum Schedule
{
	/// `dst*(1-a) + src`, for Blend, and `src + dst*(1-src)` for Screen.
	///
	/// Drawn oldest first, each ghost gets `w / (the weights so far,
	/// including its own)`. That is the running weighted average written as a
	/// series of composites: the oldest lands at 1 on an empty buffer, and
	/// every one after it pulls the result its own share of the way.
	kScheduleIncremental,

	/// `dst + src`, for Add. Each ghost gets `w / (all the weights)`, so the
	/// alphas sum to exactly one and a sum of identical frames is that frame.
	///
	/// NOT the same as incremental, and the difference is not subtle: the
	/// incremental alphas sum to about three over a dozen slots, which under
	/// a plain addition is three copies of the picture and a frame that is
	/// almost entirely white. Caught by `agtest --still`, which is the whole
	/// reason that check exists.
	kScheduleProportional,

	/// `max(dst, src)`, for Lighten. The weights as they are.
	///
	/// A max does not accumulate, so there is nothing to normalise, and
	/// normalising anyway would be actively wrong: both schedules above hand
	/// their LARGEST alpha to the oldest ghost, and under a max that would
	/// leave the trail brightest at its tail and dimmest at the live frame.
	kScheduleShape
};

/**
	Turn the shape weights into the alpha each ghost is actually drawn at.

	`Ghost::weight` is a SHAPE -- 1 at the head of the queue, 0 at the tail --
	and it is not what any of the blends want handed to them directly. A dozen
	ghosts at an average weight of half do not add up to a picture, they add up
	to five or six pictures, and the first thing that happens is that the frame
	blows out to white.

	The property every schedule above is chosen to hold up is that **a still
	picture goes through unchanged**: twelve copies of the same frame come out
	as that frame, exactly, whatever the queue length or the falloff curve is.
	An effect that quietly lifts, dims or softens footage that is not moving is
	an effect that has to be switched off between cues.

	Screen is the exception and says so: it brightens wherever ghosts genuinely
	differ, which is the whole reason to pick it. What the schedule buys there
	is that it never *darkens*, and that it brightens by a bounded amount
	instead of by the queue length.

	`firstSlot` is the youngest slot being drawn -- 1 rather than 0 when the
	live frame is being left out -- and `count` is how many follow it.
	`outAlpha[ i ]` and `outHalation[ i ]` are for slot `firstSlot + i`.

	The halation weights are ALWAYS proportional, whichever schedule is in use,
	because halation is additive light in every mode. What that buys is a glow
	whose strength does not change when Frames does.
*/
void ResolveDrawAlphas( const DecayParams& p, int firstSlot, int count, Schedule schedule,
                        float* outAlpha, float* outHalation );

//---------------------------------------------------------------------------
// The per-pixel stage. Mirrored in kDecayLibrary; keep the two in step.
//---------------------------------------------------------------------------

/// PCG output stage. Integer arithmetic, so it is exact in both languages.
///
/// Never `fract( sin( x ) * 43758.5453 )`: that is a different function on
/// every driver, which for a mirrored effect means the CPU and GPU builds
/// cannot be made to agree even in principle.
uint32_t HashInt( uint32_t seed );

/// A hash in 0..1.
float Hash01( uint32_t seed );

/// The warp displacement at a point, in picture widths, before a ghost's own
/// amplitude is applied. Two octaves: the first moves the whole ghost, the
/// second breaks its edge up. `x` and `y` are already scaled by Warp Scale.
void WarpOffset( float x, float y, float phase, float& outX, float& outY );

/// Where a ghost is sampled, for an output point.
///
/// Everything geometric is done in **picture widths on both axes** -- `u` is
/// already in them and `v` is divided by the aspect ratio on the way in and
/// multiplied back on the way out. Do it in 0..1 on both axes instead and
/// Spin becomes a shear on anything that is not square, Drift travels further
/// vertically than horizontally at the same setting, and a round pixelation
/// cell comes out oblong.
void GhostSampleUV( const Ghost& g, float u, float v, float aspect,
                    float warpScale, float warpPhase, float& outU, float& outV );

/// Apply a ghost's colour degradation to one **premultiplied** RGBA sample,
/// in place. Alpha is not touched.
void DegradeColour( const Ghost& g, float rgba[ 4 ] );

} // namespace afterglow
