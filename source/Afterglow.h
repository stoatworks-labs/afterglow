#pragma once

#include "Decay.h"
#include "PassBuffer.h"
#include "Presets.h"
#include "StoatworksAboutParams.h"

#include <FFGLSDK.h>

#include <string>

/// The longest queue the plugin will hold. Thirty-two full pictures is a
/// quarter of a gigabyte at 1080p and a gigabyte at 4K, which is why
/// Resolution exists and why it defaults to Half.
constexpr int kMaxFrames = 32;

/**
	Afterglow -- a queue of recent frames, composited back over the picture
	with each one degraded further as it ages.

	**What it does.** Every frame goes into a ring of buffers. Every frame,
	the whole ring is drawn back over the picture, oldest first: each ghost
	dimmer than the one in front of it, and each one crushed, pixelated,
	warped, drifted, spun and hue-shifted in proportion to how old it is,
	until it reaches the end of the queue and drops off. What the operator
	sets is how long the queue is, how fast it falls away, and how much of
	each kind of damage a frame has taken by the time it goes.

	**Why a queue and not a feedback buffer** is the whole design, and the
	long version is at the top of `Decay.h`. The short version: a recursive
	blend makes a similar trail for one buffer instead of thirty-two, but it
	cannot make an old frame look *older* -- fold the frames together and
	every degradation compounds on the accumulator instead of on the frame it
	belongs to.

	**Four shaders, more passes**, in `Shaders.h`. The one that matters is
	`ghost`: it runs once per queue slot into the trail, and again once per
	slot into the quarter-size halation buffer, which is why the cost of this
	plugin is roughly linear in Frames and quadratic in Resolution.

	**Where the maths lives.** `GhostAt` in `Decay.cpp` decides everything
	about a slot from one number -- its age -- and runs on the CPU in both
	builds. Only the per-pixel stage is written twice, in `Decay.cpp` and in
	`kDecayLibrary`. See AGENTS.md for the traps.
*/
class Afterglow : public CFFGLPlugin
{
public:
	/// Clock test hook. The offline harness DECLARES its unit rather than
	/// leaving the calibration to infer one -- an absolute time handed over in
	/// a single frame is genuinely ambiguous, and an implicit unit is what let
	/// the millisecond bug through elsewhere in the fleet.
	void SetClockScaleForTest( double scale );

	Afterglow();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	/// Test hook: the parameter ids a preset covers, in presets::Param order.
	/// Handed out rather than copied into the harness, so a second list
	/// cannot go quietly out of step with this one.
	static const unsigned int* PresetParamIDsForTest( int& count );

	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// The order the host shows them in: say how long the trail is, then what
	/// happens to a frame while it is on it, then how it glows, then how it
	/// goes back over the picture.
	enum ParamID : FFUInt32
	{
		//Trail
		PT_FRAMES,
		PT_HOLD,
		PT_CURVE,
		PT_BLEND,
		PT_GAIN,
		PT_RESOLUTION,

		//Decay
		PT_CRUSH,
		PT_PIXELATE,
		PT_WARP,
		PT_WARP_SCALE,
		PT_WARP_SPEED,
		PT_DRIFT,
		PT_DRIFT_ANGLE,
		PT_ZOOM,
		PT_SPIN,
		PT_HUE,
		PT_BLEACH,

		//Halation
		PT_HALATION,
		PT_HALATION_SIZE,
		PT_HALATION_TINT,

		//Output
		PT_BACKGROUND,
		PT_MIX,

		//Preset. Declared after the real controls so their IDs — which a
		//saved composition refers to — do not shift under existing users when
		//more presets arrive.
		PT_PRESET,

		//About. FFGL has no window and cannot make one, so the name, the
		//version, the maker and the links are parameters the host draws with
		//everything else. Last in the enum, so no saved composition's
		//parameter ids shift. See StoatworksAboutParams.h.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// What Blend stores. These are option VALUES, not list positions -- the
	/// list is declared alphabetically, which puts Add first by coincidence
	/// and Screen last by coincidence.
	enum BlendMode
	{
		kBlendAdd     = 0,
		kBlendOver    = 1,
		kBlendLighten = 2,
		kBlendScreen  = 3,
		kBlendCount
	};

	/// What Background stores.
	///
	/// Two of these also decide something the composite cannot see: whether
	/// the LIVE frame is in the trail at all. Source and Ghosts leave it out,
	/// because in Source mode the untouched picture is already underneath and
	/// putting the live frame in the trail as well would show it twice.
	enum BackgroundMode
	{
		kBackgroundBlack       = 0,///< the whole trail, live frame included, on black
		kBackgroundGhosts      = 1,///< the trail WITHOUT the live frame, on black
		kBackgroundSource      = 2,///< the untouched clip, with the ghosts over it
		kBackgroundTransparent = 3,///< the whole trail over nothing
		kBackgroundCount
	};

private:
	/// The ParamID each presets::Param drives, in presets::Param order. The
	/// preset table stays host-agnostic; this is the FFGL binding of it.
	static constexpr unsigned int kPresetParamIDs[ afterglow::presets::kParamCount ] = {
		PT_FRAMES, PT_HOLD, PT_CURVE, PT_BLEND, PT_GAIN,
		PT_CRUSH, PT_PIXELATE, PT_WARP, PT_WARP_SCALE, PT_WARP_SPEED,
		PT_DRIFT, PT_DRIFT_ANGLE, PT_ZOOM, PT_SPIN, PT_HUE, PT_BLEACH,
		PT_HALATION, PT_HALATION_SIZE, PT_HALATION_TINT, PT_BACKGROUND
	};

	/// The active preset's value for `id`, or -1 when no preset is active or
	/// this one has no opinion about `id`. Preset values are all 0..1, so a
	/// negative is unambiguous.
	float presetValue( int presetIndex, unsigned int id ) const;

	/// True when this write is the HOST restating a value it still believes in
	/// rather than the operator moving anything -- in which case it must not
	/// reach params[] and must not disturb the preset.
	bool hostIsRestatingItself( unsigned int index, float value );

	/// Record the defaults as the host's opening position, once, before
	/// anything has had a chance to move them.
	void seedHostValues();

	void applyPreset( int presetIndex );

	/// Bring the queue to this size and length, reallocating only what has
	/// changed, and empty it if the ring's shape moved. False means the
	/// driver would not give us the memory.
	bool EnsureQueue( int slotWidth, int slotHeight, int frames );

	/// Set the GL blend state one accumulation pass wants.
	static void ApplyBlendMode( int mode );

	/// What the HOST last sent for each parameter, which is not the same
	/// thing as what the plugin is rendering with.
	///
	/// FFGL's host owns parameter state. It pushes its own values back down
	/// whenever it likes, and nothing obliges it to act on the value events
	/// applyPreset raises -- Resolume does not. So a preset that writes
	/// params[] and trusts the host to follow is relying on behaviour the
	/// specification never promised, and when the host instead restates the
	/// values it still believes in, the rule that a covered parameter
	/// changing means the operator has taken over fires on the host's own
	/// echo and drops straight back to Custom. Reported against vertigo as
	/// its issue #2; the same pattern had been copied into seven plugins
	/// before it was found.
	///
	/// Keeping the host's own last word separately is what tells the two
	/// apart.
	float hostValues[ PT_COUNT ] = {};
	bool hostValuesSeeded        = false;

	ffglex::FFGLShader copyShader;
	ffglex::FFGLShader ghostShader;
	ffglex::FFGLShader blurShader;
	ffglex::FFGLShader compositeShader;
	ffglex::FFGLScreenQuad quad;

	afterglow::PassBuffer copyBuffer;         ///< the picture, ours, mipmapped

	/// The frame queue, as a ring. A fixed array and not a `std::vector`:
	/// `ffglex::FFGLFBO` has a user-declared destructor and raw GL ids, so its
	/// implicit copy constructor duplicates the ids without duplicating the
	/// objects -- and a vector reallocation would hand two PassBuffers the
	/// same framebuffer and delete it twice. Thirty-two unallocated buffers
	/// are a few hundred bytes; only `queueFrames` of them ever hold a
	/// texture.
	afterglow::PassBuffer slots[ kMaxFrames ];
	afterglow::PassBuffer trailBuffer;        ///< every ghost, accumulated
	afterglow::PassBuffer halationBuffer[ 2 ];///< quarter size, ping-ponged by the blur

	//---------------------------------------------------------------------
	// The ring.
	//
	// `head` is the slot holding the newest frame, so the frame of age k is
	// at `( head - k + n ) % n`. `filled` is how many slots hold a frame at
	// all -- a queue that has just been reallocated is empty, and drawing a
	// cleared buffer is free but drawing a STALE one is a frame of somebody
	// else's footage appearing in the trail.
	//---------------------------------------------------------------------
	int head        = 0;
	int filled      = 0;
	int captureTick = 0;

	int queueWidth  = 0;
	int queueHeight = 0;
	int queueFrames = 0;

	//---------------------------------------------------------------------
	// Time.
	//
	// The warp phase is accumulated from the host's clock rather than
	// computed from it. `time * speed` is the obvious form and it is wrong:
	// moving Warp Speed rescales the whole history, so the field jumps to a
	// different point the instant the control is touched -- worst at the
	// moment an operator is nudging it, which is exactly when it is being
	// watched. Integrating the rate instead means the control changes what
	// happens next and nothing else.
	//---------------------------------------------------------------------
	double hostTime     = -1.0;
	double lastHostTime = -1.0;
	double warpPhase    = 0.0;

	//---------------------------------------------------------------------
	// Host clock units.
	//
	// The FFGL header never says what unit SetTime is in, and hosts
	// disagree: Resolume hands over MILLISECONDS (measured live: 20.0 per
	// frame at its 50 fps, and the SDK's own Particles sample divides by
	// 1000), while the offline harness -- and any host following the
	// header's silence -- sends seconds. Decided by comparing the host's
	// clock against a steady one over several frames, because the magnitude
	// of a single frame delta does not settle it.
	//---------------------------------------------------------------------
	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double lastRawTime  = -1.0;

	/// Counts frames so the sixtieth can log what the host's clock actually
	/// looks like. One line, once, in the diag log.
	int clockFrames = 0;

	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
