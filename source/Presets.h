#pragma once

/**
	Factory presets: named trails an operator can reach in one gesture.

	The values live in the same 0..1 parameter space both builds expose (the
	FFGL and OpenFX builds deliberately share it), so ONE table drives both and
	a preset looks identical in Resolume and Resolve. Plain data only; the
	application machinery lives with each host's glue.

	Element 0 of the host-facing dropdown is "Custom" and is not in this table:
	it means "the sliders are the truth".

	A preset covers the trail, the decay and the halation -- what the effect
	looks like. It deliberately does not cover **Mix**, which is the operator's
	way of pulling any of it back, or **Resolution**, which is a statement
	about the machine rather than about the picture: a preset that quietly
	moved a 4K show from a quarter-size queue to a full-size one would take a
	gigabyte of video memory with it.
*/

namespace afterglow
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. The FFGL build binds
/// this order to its ParamIDs and the OpenFX build to its param handles; both
/// static_assert against kParamCount so the three lists cannot drift apart
/// silently.
enum Param
{
	kFrames,
	kHold,
	kCurve,
	kBlend,
	kGain,
	kCrush,
	kPixelate,
	kWarp,
	kWarpScale,
	kWarpSpeed,
	kDrift,
	kDriftAngle,
	kZoom,
	kSpin,
	kHue,
	kBleach,
	kHalation,
	kHalationSize,
	kHalationTint,
	kBackground,
	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

// Option values are element values, not list positions: Blend is
// 0 Add / 1 Blend / 2 Lighten / 3 Screen, and Background is
// 0 Black / 1 Ghosts / 2 Source / 3 Transparent.
//
// Everything else is a slider in 0..1; Controls.cpp says what each one means
// in physical units. Zoom, Spin and Hue are bipolar, so 0.5 is their null and
// the comment beside each says which way it has been pushed.
inline constexpr Preset kPresets[] = {
	// The plain article: a clean trail with nothing done to it but time. The
	// one to start from, and the one to come back to when a look has been
	// tuned into something unreadable.
	//
	// Note that this is NOT the plugin's own defaults -- it has no bleach and
	// no halation where the defaults have a little of each. That is on
	// purpose: a factory preset whose values are the defaults provably changes
	// nothing when it is applied over them, which makes it indistinguishable
	// from a preset that is broken. tools/sweep.py found exactly that in
	// tinsel.
	{ "Clean Echo",
	  { /*Frames*/ 0.53f, /*Hold*/ 0.0f, /*Curve*/ 0.50f, /*Blend*/ 3, /*Gain*/ 0.50f,
	    /*Crush*/ 0.0f, /*Pixelate*/ 0.0f, /*Warp*/ 0.0f, /*WarpScale*/ 0.50f, /*WarpSpeed*/ 0.30f,
	    /*Drift*/ 0.0f, /*Angle*/ 0.0f, /*Zoom*/ 0.50f, /*Spin*/ 0.50f, /*Hue*/ 0.50f, /*Bleach*/ 0.0f,
	    /*Halation*/ 0.0f, /*HaloSize*/ 0.45f, /*HaloTint*/ 0.50f, /*Back*/ 2 } },

	// A long queue held four frames to a slot, so the trail covers nearly two
	// seconds, with the falloff pushed late. Warm halation over the top.
	{ "Slow Burn",
	  { /*Frames*/ 0.86f, /*Hold*/ 0.43f, /*Curve*/ 0.30f, /*Blend*/ 3, /*Gain*/ 0.55f,
	    /*Crush*/ 0.0f, /*Pixelate*/ 0.0f, /*Warp*/ 0.0f, /*WarpScale*/ 0.50f, /*WarpSpeed*/ 0.30f,
	    /*Drift*/ 0.0f, /*Angle*/ 0.0f, /*Zoom*/ 0.50f, /*Spin*/ 0.50f, /*Hue*/ 0.50f, /*Bleach*/ 0.45f,
	    /*Halation*/ 0.70f, /*HaloSize*/ 0.62f, /*HaloTint*/ 0.80f, /*Back*/ 2 } },

	// Short queue, everything falls apart fast: the bit depth collapses and
	// the grid coarsens over half a dozen frames. Added rather than screened,
	// because the clipping is the look.
	{ "Datamosh",
	  { /*Frames*/ 0.20f, /*Hold*/ 0.14f, /*Curve*/ 0.65f, /*Blend*/ 0, /*Gain*/ 0.45f,
	    /*Crush*/ 0.85f, /*Pixelate*/ 0.55f, /*Warp*/ 0.0f, /*WarpScale*/ 0.50f, /*WarpSpeed*/ 0.30f,
	    /*Drift*/ 0.0f, /*Angle*/ 0.0f, /*Zoom*/ 0.50f, /*Spin*/ 0.50f, /*Hue*/ 0.60f, /*Bleach*/ 0.0f,
	    /*Halation*/ 0.0f, /*HaloSize*/ 0.45f, /*HaloTint*/ 0.50f, /*Back*/ 2 } },

	// The trail sinking backwards into the frame as it goes: zoom in, drift
	// down, and a slow bleach. Reads as depth rather than as motion.
	{ "Undertow",
	  { /*Frames*/ 0.75f, /*Hold*/ 0.0f, /*Curve*/ 0.42f, /*Blend*/ 3, /*Gain*/ 0.55f,
	    /*Crush*/ 0.0f, /*Pixelate*/ 0.0f, /*Warp*/ 0.0f, /*WarpScale*/ 0.50f, /*WarpSpeed*/ 0.30f,
	    /*Drift*/ 0.40f, /*Angle*/ 0.75f, /*Zoom*/ 0.36f, /*Spin*/ 0.50f, /*Hue*/ 0.50f, /*Bleach*/ 0.30f,
	    /*Halation*/ 0.30f, /*HaloSize*/ 0.55f, /*HaloTint*/ 0.60f, /*Back*/ 2 } },

	// Each ghost turned a little further and shifted a little further round
	// the wheel, so the trail fans out in colour as well as in angle.
	{ "Spin Cycle",
	  { /*Frames*/ 0.67f, /*Hold*/ 0.14f, /*Curve*/ 0.50f, /*Blend*/ 3, /*Gain*/ 0.60f,
	    /*Crush*/ 0.0f, /*Pixelate*/ 0.0f, /*Warp*/ 0.0f, /*WarpScale*/ 0.50f, /*WarpSpeed*/ 0.30f,
	    /*Drift*/ 0.0f, /*Angle*/ 0.0f, /*Zoom*/ 0.56f, /*Spin*/ 0.62f, /*Hue*/ 0.68f, /*Bleach*/ 0.0f,
	    /*Halation*/ 0.25f, /*HaloSize*/ 0.45f, /*HaloTint*/ 0.30f, /*Back*/ 2 } },

	// A warp that boils, a bit depth that goes, and the colour draining out:
	// the trail as a tape that has been through the machine too many times.
	{ "Bad Tape",
	  { /*Frames*/ 0.45f, /*Hold*/ 0.29f, /*Curve*/ 0.55f, /*Blend*/ 1, /*Gain*/ 0.50f,
	    /*Crush*/ 0.60f, /*Pixelate*/ 0.0f, /*Warp*/ 0.62f, /*WarpScale*/ 0.28f, /*WarpSpeed*/ 0.45f,
	    /*Drift*/ 0.30f, /*Angle*/ 0.0f, /*Zoom*/ 0.50f, /*Spin*/ 0.50f, /*Hue*/ 0.44f, /*Bleach*/ 0.55f,
	    /*Halation*/ 0.40f, /*HaloSize*/ 0.50f, /*HaloTint*/ 0.70f, /*Back*/ 2 } },

	// Long, soft, entirely halation: the picture on a tube that has not let
	// go of it yet. Screened over black so the glow is the whole image.
	{ "Phosphor",
	  { /*Frames*/ 0.80f, /*Hold*/ 0.14f, /*Curve*/ 0.35f, /*Blend*/ 3, /*Gain*/ 0.50f,
	    /*Crush*/ 0.0f, /*Pixelate*/ 0.0f, /*Warp*/ 0.0f, /*WarpScale*/ 0.50f, /*WarpSpeed*/ 0.30f,
	    /*Drift*/ 0.0f, /*Angle*/ 0.0f, /*Zoom*/ 0.50f, /*Spin*/ 0.50f, /*Hue*/ 0.50f, /*Bleach*/ 0.65f,
	    /*Halation*/ 0.85f, /*HaloSize*/ 0.72f, /*HaloTint*/ 0.90f, /*Back*/ 0 } },

	// The decay on its own, with the live frame left out of the trail
	// entirely. As much a way of seeing what the controls are doing as a look.
	{ "Ghost Print",
	  { /*Frames*/ 0.62f, /*Hold*/ 0.29f, /*Curve*/ 0.45f, /*Blend*/ 2, /*Gain*/ 0.70f,
	    /*Crush*/ 0.35f, /*Pixelate*/ 0.0f, /*Warp*/ 0.0f, /*WarpScale*/ 0.50f, /*WarpSpeed*/ 0.30f,
	    /*Drift*/ 0.25f, /*Angle*/ 0.13f, /*Zoom*/ 0.50f, /*Spin*/ 0.50f, /*Hue*/ 0.34f, /*Bleach*/ 0.40f,
	    /*Halation*/ 0.35f, /*HaloSize*/ 0.55f, /*HaloTint*/ 0.40f, /*Back*/ 1 } },
};

inline constexpr int kCount = int( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace afterglow
