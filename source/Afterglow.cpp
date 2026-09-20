#include "Afterglow.h"

#include "Controls.h"
#include "Decay.h"
#include "Diag.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name. The symptom without it is an
//unknown-type error on ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace ffglex;
using namespace afterglow;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Afterglow >,                                     // Create method
	"AG01",                                                         // Plugin unique ID of maximum length 4.
	"SW Afterglow",                                                 // Plugin name
	2,                                                              // API major version number
	1,                                                              // API minor version number
	0,                                                              // Plugin major version number
	1,                                                              // Plugin minor version number
	FF_EFFECT,                                                      // Plugin type
	"Keeps the last few dozen frames and lays them back over the picture, each one further gone than the one in front of it.\n\nNot a blur and not a feedback loop. A queue: every frame goes in, the whole queue comes back out, and each frame in it is dimmer, coarser, more crushed, further drifted and further round the colour wheel than its neighbour, until it drops off the end.\n\nFold the frames into one accumulator instead and the damage compounds - yesterday's crush gets crushed again today - and there is no way back to the picture.\n\nFootage that is not moving comes out exactly as it went in.\n\nStart from a Preset, at the bottom.",// Plugin description
	"Afterglow FFGL effect"                                         // About
);

namespace
{
/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be
/// the thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kBlendNames[]      = { "Add", "Blend", "Lighten", "Screen" };
const char* const kBackgroundNames[] = { "Black", "Ghosts", "Source", "Transparent" };

/// Deliberately in ordinal order, not alphabetical: this is a divisor, the
/// position IS the meaning, and sorted it reads Eighth, Full, Half, Quarter.
const char* const kResolutionNames[] = { "Full", "Half", "Quarter", "Eighth" };
constexpr int kResolutionCount       = 4;

/// Seconds of host time a single frame is allowed to advance the warp by.
/// The host's clock is not ours: it jumps when the composition is scrubbed,
/// when a clip is retriggered, and by however long the machine was asleep.
constexpr double kMaxFrameDelta = 0.25;

/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// The colour a halation ring actually is: red light scattering back through
/// the film base, with the emulsion having stopped the blue first.
constexpr float kHalationWarm[ 3 ] = { 1.0f, 0.72f, 0.42f };

/// Wall clock, for hosts that never call SetTime. Steady rather than system,
/// so nothing here moves when the machine's clock is corrected.
double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

/// Case-insensitive name order, so a list sorts the way a reader expects it
/// to rather than the way a byte comparison does.
bool nameLess( const char* a, const char* b )
{
	for( ; *a && *b; ++a, ++b )
	{
		const int ca = std::tolower( static_cast< unsigned char >( *a ) );
		const int cb = std::tolower( static_cast< unsigned char >( *b ) );
		if( ca != cb )
			return ca < cb;
	}
	return *b != '\0';
}
} // namespace

Afterglow::Afterglow()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The host drives the warp where it can, so that rendering the same frame
	//twice gives the same picture twice and an export matches the preview.
	//
	//Note what it cannot drive: the QUEUE. Which frames are in it is a
	//consequence of which frames the host asked for, in the order it asked,
	//and nothing in FFGL lets a plugin ask for a frame it was not given. See
	//AGENTS.md -- this is the one thing about this effect that is genuinely
	//different between the Resolume build and the OpenFX one.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter,
	// so these assignments are what the host is told the defaults are.
	//
	// They add up to a soft, slightly bleached trail with a little halation,
	// over the untouched clip: the effect doing its one thing, recognisably,
	// with nothing found first. The null is Mix at zero.
	//---------------------------------------------------------------------
	params[ PT_FRAMES ]     = 0.53f;//about twelve
	params[ PT_HOLD ]       = 0.0f; //one host frame per slot
	params[ PT_CURVE ]      = 0.50f;//linear falloff
	//Blend and not Screen. Blend is the incremental weighted average -- the
	//queue interpolated into one picture -- which is the only mode that
	//provably leaves footage that is not moving exactly as it was. Screen is
	//the more exciting default and it lifts every static frame in the show.
	params[ PT_BLEND ]      = static_cast< float >( kBlendOver );
	params[ PT_GAIN ]       = 0.50f;//unity after mapping

	//Full, and not Half, even though Half is a quarter of the memory.
	//
	//The live frame is always read at full resolution (see the accumulation
	//loop), but the GHOSTS are not, and on footage that is not moving the
	//ghosts ARE the picture -- a dozen soft copies of the frame averaged with
	//one sharp one comes out soft. So a reduced queue quietly costs sharpness
	//on every static shot in the show, in the default Background mode, and
	//"the plugin makes my titles look fuzzy" is not a thing an operator should
	//have to work out.
	//
	//Half exists for the case it is actually for: a long queue on a big
	//canvas, where thirty-two 4K frames is a gigabyte of video memory and the
	//trail is heavily degraded anyway.
	params[ PT_RESOLUTION ] = 0.0f; //Full

	params[ PT_CRUSH ]      = 0.0f;
	params[ PT_PIXELATE ]   = 0.0f;
	params[ PT_WARP ]       = 0.0f;
	params[ PT_WARP_SCALE ] = 0.50f;
	params[ PT_WARP_SPEED ] = 0.30f;
	params[ PT_DRIFT ]      = 0.0f;
	params[ PT_DRIFT_ANGLE ] = 0.0f;
	params[ PT_ZOOM ]       = 0.50f;//bipolar null
	params[ PT_SPIN ]       = 0.50f;//bipolar null
	params[ PT_HUE ]        = 0.50f;//bipolar null
	params[ PT_BLEACH ]     = 0.25f;

	params[ PT_HALATION ]      = 0.25f;
	params[ PT_HALATION_SIZE ] = 0.45f;
	params[ PT_HALATION_TINT ] = 0.50f;

	params[ PT_BACKGROUND ] = static_cast< float >( kBackgroundSource );
	params[ PT_MIX ]        = 1.0f;

	params[ PT_PRESET ] = 0.0f;//Custom: the sliders are the truth

	//---------------------------------------------------------------------
	// Declaration.
	//
	// Every numeric parameter is a plain 0..1 float even where it stands for
	// a frame count or a number of turns. SetParamInfo clamps an
	// FF_TYPE_STANDARD default into 0..1 *before* a range can be attached
	// (SDK b1afaf9), so a parameter declared in frames cannot declare a
	// default in frames. The conversions live in Controls.cpp.
	//
	// Option lists are declared in alphabetical order, and every entry keeps
	// the value it has always had. Those are two separate things in FFGL:
	// SetParamElementInfo takes an element's display slot and its stored
	// value as different arguments, and the spec is explicit that picking an
	// option gives the parameter "a value equal to that of the option's
	// value" -- the slot is never stored. So a list can be re-sorted for
	// whoever has to find something in it without a saved composition, a
	// factory preset or the harness changing meaning.
	//---------------------------------------------------------------------

	/// Declare an option's elements alphabetically, each keeping its value.
	auto declareOptions = [ this ]( unsigned int paramID, int count, auto nameOf ) {
		std::vector< int > order( static_cast< size_t >( count ) );
		for( int i = 0; i < count; ++i )
			order[ i ] = i;

		std::stable_sort( order.begin(), order.end(),
		                  [ & ]( int a, int b ) { return nameLess( nameOf( a ), nameOf( b ) ); } );

		for( int slot = 0; slot < count; ++slot )
			SetParamElementInfo( paramID,
			                     static_cast< unsigned int >( slot ),
			                     nameOf( order[ slot ] ),
			                     static_cast< float >( order[ slot ] ) );
	};

	SetParamInfof( PT_FRAMES, "Frames", FF_TYPE_STANDARD );
	SetParamInfof( PT_HOLD, "Hold", FF_TYPE_STANDARD );
	SetParamInfof( PT_CURVE, "Decay", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_BLEND, "Blend", kBlendCount, params[ PT_BLEND ] );
	declareOptions( PT_BLEND, kBlendCount, []( int v ) { return kBlendNames[ v ]; } );

	SetParamInfof( PT_GAIN, "Gain", FF_TYPE_STANDARD );

	//Not sorted, and not an oversight to tidy up later -- see kResolutionNames.
	SetOptionParamInfo( PT_RESOLUTION, "Resolution", kResolutionCount, params[ PT_RESOLUTION ] );
	for( int i = 0; i < kResolutionCount; ++i )
		SetParamElementInfo( PT_RESOLUTION, i, kResolutionNames[ i ], static_cast< float >( i ) );

	SetParamInfof( PT_CRUSH, "Crush", FF_TYPE_STANDARD );
	SetParamInfof( PT_PIXELATE, "Pixelate", FF_TYPE_STANDARD );
	SetParamInfof( PT_WARP, "Warp", FF_TYPE_STANDARD );
	SetParamInfof( PT_WARP_SCALE, "Warp Scale", FF_TYPE_STANDARD );
	SetParamInfof( PT_WARP_SPEED, "Warp Speed", FF_TYPE_STANDARD );
	SetParamInfof( PT_DRIFT, "Drift", FF_TYPE_STANDARD );
	SetParamInfof( PT_DRIFT_ANGLE, "Direction", FF_TYPE_STANDARD );
	SetParamInfof( PT_ZOOM, "Zoom", FF_TYPE_STANDARD );
	SetParamInfof( PT_SPIN, "Spin", FF_TYPE_STANDARD );
	SetParamInfof( PT_HUE, "Hue Shift", FF_TYPE_STANDARD );
	SetParamInfof( PT_BLEACH, "Bleach", FF_TYPE_STANDARD );

	SetParamInfof( PT_HALATION, "Halation", FF_TYPE_STANDARD );
	SetParamInfof( PT_HALATION_SIZE, "Halation Size", FF_TYPE_STANDARD );
	SetParamInfof( PT_HALATION_TINT, "Halation Tint", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_BACKGROUND, "Background", kBackgroundCount, params[ PT_BACKGROUND ] );
	declareOptions( PT_BACKGROUND, kBackgroundCount, []( int v ) { return kBackgroundNames[ v ]; } );

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	// Factory presets. Element 0 is Custom; picking anything else copies that
	// preset's values into the covered parameters and raises value events so
	// the host re-reads the sliders. Editing a covered slider flips back to
	// Custom.
	//
	// Custom is pinned to the top and the presets sort below it. It is not a
	// preset -- it is the statement that there isn't one -- so a list that
	// filed it between Clean Echo and Datamosh would be lying about what it
	// is. Its value stays 0, which is what applyPreset's 1-based index and
	// the sweep's Preset context both read.
	SetOptionParamInfo( PT_PRESET, "Preset", 1 + presets::kCount, params[ PT_PRESET ] );
	{
		std::vector< int > order( static_cast< size_t >( presets::kCount ) );
		for( int i = 0; i < presets::kCount; ++i )
			order[ i ] = i + 1;
		std::stable_sort( order.begin(), order.end(), []( int a, int b ) {
			return nameLess( presets::kPresets[ a - 1 ].name, presets::kPresets[ b - 1 ].name );
		} );

		SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
		for( int slot = 0; slot < presets::kCount; ++slot )
			SetParamElementInfo( PT_PRESET, static_cast< unsigned int >( slot + 1 ),
			                     presets::kPresets[ order[ slot ] - 1 ].name,
			                     static_cast< float >( order[ slot ] ) );
	}

	//Twenty-three parameters is past the point where an ungrouped list in
	//somebody else's inspector stops being readable.
	for( FFUInt32 i = PT_FRAMES; i <= PT_RESOLUTION; ++i )
		SetParamGroup( i, "Trail" );
	for( FFUInt32 i = PT_CRUSH; i <= PT_BLEACH; ++i )
		SetParamGroup( i, "Decay" );
	for( FFUInt32 i = PT_HALATION; i <= PT_HALATION_TINT; ++i )
		SetParamGroup( i, "Halation" );
	for( FFUInt32 i = PT_BACKGROUND; i <= PT_MIX; ++i )
		SetParamGroup( i, "Output" );
	SetParamGroup( PT_PRESET, "Preset" );

	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin and nothing outside the class
	// can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Afterglow effect" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Afterglow::InitGL( const FFGLViewportStruct* vp )
{
	//The GL strings first, and unconditionally: when a shader will not
	//compile it is almost always the driver or the GL version, and knowing
	//which machine reported what is most of the diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	//The ghost pass is assembled rather than written out, because the decay
	//library in the middle of it is shared verbatim with the harness's probe.
	//Held in a local so the pointer handed to Compile outlives the call.
	const std::string ghostSource = GhostShaderSource();

	struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} const stages[] = {
		{ &copyShader, kCopyShader, "copy" },
		{ &ghostShader, ghostSource.c_str(), "ghost" },
		{ &blurShader, kBlurShader, "blur" },
		{ &compositeShader, kCompositeShader, "composite" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( kVertexShader, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect
		//simply does nothing in Resolume, with no message anywhere. These two
		//lines are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name
		             + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Afterglow: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Afterglow: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	head        = 0;
	filled      = 0;
	captureTick = 0;

	diag::info( "initialised, queue up to " + std::to_string( kMaxFrames ) + " frames" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
bool Afterglow::EnsureQueue( int slotWidth, int slotHeight, int frames )
{
	frames = std::clamp( frames, 2, kMaxFrames );

	const bool same = slotWidth == queueWidth && slotHeight == queueHeight && frames == queueFrames;

	for( int i = 0; i < kMaxFrames; ++i )
	{
		if( i >= frames )
		{
			//Shrinking really does have to free them. A queue of 32 quarter-
			//size buffers left allocated behind a queue of 4 is not a leak in
			//the sense that anything is lost, but it is a quarter of a
			//gigabyte of video memory the operator believes they gave back
			//when they moved the slider.
			slots[ i ].Destroy();
			continue;
		}

		if( !slots[ i ].Ensure( slotWidth, slotHeight, GL_RGBA8, PassBuffer::Sampling::Linear ) )
			return false;
	}

	if( !same )
	{
		//The ring's indexing is modulo its own length, so changing the length
		//does not shuffle the contents -- it reinterprets them. Every slot
		//would still hold a real frame and every one of them would be filed
		//under the wrong age, which shows as the trail jumping to a different
		//arrangement of the same pictures. Starting empty costs a queue's
		//worth of frames and is the only answer that is not wrong.
		for( int i = 0; i < frames; ++i )
			slots[ i ].Clear();

		head        = 0;
		filled      = 0;
		captureTick = 0;

		queueWidth  = slotWidth;
		queueHeight = slotHeight;
		queueFrames = frames;

		diag::info( "queue rebuilt: " + std::to_string( frames ) + " x "
		            + std::to_string( slotWidth ) + "x" + std::to_string( slotHeight ) + " = "
		            + std::to_string( ( static_cast< long long >( frames ) * slotWidth * slotHeight * 4 )
		                              / ( 1024 * 1024 ) )
		            + " MB" );
	}

	return true;
}

//---------------------------------------------------------------------------
void Afterglow::ApplyBlendMode( int mode )
{
	//Premultiplied throughout, which is what makes these one-liners rather
	//than a separate alpha function each.
	switch( mode )
	{
	case kBlendAdd:
		glBlendEquation( GL_FUNC_ADD );
		glBlendFunc( GL_ONE, GL_ONE );
		break;

	case kBlendOver:
		glBlendEquation( GL_FUNC_ADD );
		glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
		break;

	case kBlendLighten:
		//GL_MAX ignores the blend function entirely; it is set anyway so that
		//leaving this case does not depend on what the next one sets.
		glBlendEquation( GL_MAX );
		glBlendFunc( GL_ONE, GL_ONE );
		break;

	case kBlendScreen:
	default:
		glBlendEquation( GL_FUNC_ADD );
		glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_COLOR );
		break;
	}
}

//---------------------------------------------------------------------------
FFResult Afterglow::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	const int pictureWidth  = static_cast< int >( picture.Width );
	const int pictureHeight = static_cast< int >( picture.Height );
	const float aspect      = static_cast< float >( pictureWidth ) / static_cast< float >( pictureHeight );

	//The host's viewport, read before anything of ours changes it.
	//
	//`ScopedFBOBinding` restores the framebuffer binding and *only* the
	//framebuffer binding -- it does not touch the viewport (SDK b1afaf9,
	//FFGLScopedFBOBinding.cpp). So every pass's ResizeViewPort() leaks out
	//into the pass after it, and the composite, which draws to the host's own
	//framebuffer and so has no buffer of its own to size itself from,
	//inherits whatever the last pass left behind. Here that would be the
	//quarter-size halation buffer, and the effect would render into the
	//bottom-left quarter of the frame with the rest left transparent.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//And the host's blend state. This plugin is one of the few in the fleet
	//that blends at all -- the whole trail is an accumulation -- so it has to
	//put back what it found rather than leaving GL_BLEND on for whatever the
	//host draws next.
	const GLboolean hostBlend = glIsEnabled( GL_BLEND );
	GLint hostBlendSrc = GL_ONE, hostBlendDst = GL_ZERO, hostBlendEquation = GL_FUNC_ADD;
	glGetIntegerv( GL_BLEND_SRC_RGB, &hostBlendSrc );
	glGetIntegerv( GL_BLEND_DST_RGB, &hostBlendDst );
	glGetIntegerv( GL_BLEND_EQUATION_RGB, &hostBlendEquation );

	//---------------------------------------------------------------------
	// Time, for the warp field only. Integrate the rate; never rescale the
	// history. See Afterglow.h.
	//
	// Normalise the host's clock to seconds first: Resolume sends
	// milliseconds, the harness sends seconds, and the header says nothing.
	// steady_clock says how much real time passed, the host says how much
	// host time passed, and the ratio names the unit outright -- 1 for
	// seconds, 1000 for milliseconds, and nothing plausible in between.
	//---------------------------------------------------------------------
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	const double raw = hostTime;

	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		// A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}

	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	// Until the unit is settled -- and for a host that never calls SetTime --
	// run on the real clock: wrong in origin but right in rate, where
	// assuming seconds would be a thousand times fast on Resolume.
	const double now = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale
	                                                       : wallNow - wallStart;

	if( lastHostTime >= 0.0 )
	{
		const double delta = std::clamp( now - lastHostTime, 0.0, kMaxFrameDelta );
		warpPhase += delta * static_cast< double >( WarpSpeedFromParam( params[ PT_WARP_SPEED ] ) );
	}

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( raw )
		            + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );

	lastHostTime = now;

	//---------------------------------------------------------------------
	// What the controls say.
	//---------------------------------------------------------------------
	const int divisor = ResolutionDivisor( params[ PT_RESOLUTION ] );
	const int hold    = HoldFromParam( params[ PT_HOLD ] );
	const int mode    = std::clamp( static_cast< int >( std::lround( params[ PT_BACKGROUND ] ) ),
                                    0, kBackgroundCount - 1 );
	const int blend   = std::clamp( static_cast< int >( std::lround( params[ PT_BLEND ] ) ),
                                    0, kBlendCount - 1 );

	DecayParams decay;
	decay.frames     = FramesFromParam( params[ PT_FRAMES ] );
	decay.curve      = CurveFromParam( params[ PT_CURVE ] );
	decay.drift      = DriftFromParam( params[ PT_DRIFT ] );
	decay.driftAngle = DriftAngleFromParam( params[ PT_DRIFT_ANGLE ] );
	decay.zoom       = ZoomFromParam( params[ PT_ZOOM ] );
	decay.spin       = SpinFromParam( params[ PT_SPIN ] );
	decay.crush      = CrushFromParam( params[ PT_CRUSH ] );
	decay.pixelate   = PixelateFromParam( params[ PT_PIXELATE ] );
	decay.warp       = WarpFromParam( params[ PT_WARP ] );
	decay.hue        = HueFromParam( params[ PT_HUE ] );
	decay.bleach     = BleachFromParam( params[ PT_BLEACH ] );
	decay.halation   = HalationFromParam( params[ PT_HALATION ] );

	const float warpScale = WarpScaleFromParam( params[ PT_WARP_SCALE ] );
	const float gain      = GainFromParam( params[ PT_GAIN ] );

	//---------------------------------------------------------------------
	// Buffers.
	//
	// Every Ensure() happens here, before anything binds a texture. That is
	// not tidiness: ffglex::FFGLFBO::Initialise sizes its new colour texture
	// under a ScopedTextureBinding, and every ffglex Scoped* binding *clears*
	// to 0 on scope exit rather than restoring what was there. Allocating a
	// buffer therefore unbinds the input texture from the active unit, and
	// the symptom is the dangerous part -- correct on every frame except the
	// one that allocates, so a control reads as dead for a single frame after
	// load and once more each time a size drag reallocates.
	//---------------------------------------------------------------------
	const int slotWidth    = std::max( 16, pictureWidth / divisor );
	const int slotHeight   = std::max( 16, pictureHeight / divisor );
	const int halationW    = std::max( 16, pictureWidth / 4 );
	const int halationH    = std::max( 16, pictureHeight / 4 );

	const bool allocated =
		copyBuffer.Ensure( pictureWidth, pictureHeight, GL_RGBA8, PassBuffer::Sampling::Mipmapped )
		&& trailBuffer.Ensure( pictureWidth, pictureHeight, GL_RGBA16F, PassBuffer::Sampling::Linear )
		&& halationBuffer[ 0 ].Ensure( halationW, halationH, GL_RGBA16F, PassBuffer::Sampling::Linear )
		&& halationBuffer[ 1 ].Ensure( halationW, halationH, GL_RGBA16F, PassBuffer::Sampling::Linear )
		&& EnsureQueue( slotWidth, slotHeight, decay.frames );

	if( !allocated )
	{
		//The size goes in the message because the fix is almost always
		//Resolution or Frames rather than anything in the code.
		diag::error( "could not allocate the frame queue: " + std::to_string( decay.frames )
		             + " x " + std::to_string( slotWidth ) + "x" + std::to_string( slotHeight )
		             + " - try a lower Resolution or fewer Frames" );
		return FF_FAIL;
	}

	const int frames = queueFrames;

	//---------------------------------------------------------------------
	// 1. The picture, into a texture of ours, with a mip chain on it.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( copyBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		copyBuffer.ResizeViewPort();
		ScopedShaderBinding shader( copyShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( picture.Handle );

		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( picture );
		copyShader.Set( "InputTexture", 0 );
		copyShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		copyShader.Set( "HalfTexel",
		                0.5f / static_cast< float >( pictureWidth ),
		                0.5f / static_cast< float >( pictureHeight ) );
		copyShader.Set( "SourceLod", 0.0f );
		quad.Draw();
	}
	copyBuffer.GenerateMipmaps();

	//---------------------------------------------------------------------
	// 2. Capture.
	//
	// The head advances every `Hold` frames; the picture is written every
	// frame, into whichever slot the head is on. Those are deliberately not
	// the same thing. Advancing and writing together would leave the newest
	// slot up to Hold-1 frames stale, and at Hold = 8 the live picture in
	// Black and Transparent modes would judder at one eighth of the frame
	// rate -- a defect that looks like dropped frames rather than like the
	// control that was moved.
	//---------------------------------------------------------------------
	if( filled > 0 && ( captureTick % hold ) == 0 )
	{
		head = ( head + 1 ) % frames;
		if( filled < frames )
			++filled;
	}
	if( filled == 0 )
		filled = 1;
	++captureTick;

	{
		ScopedFBOBinding fbo( slots[ head ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		slots[ head ].ResizeViewPort();
		ScopedShaderBinding shader( copyShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( copyBuffer.TextureID() );

		//The mip level is the whole reason the copy exists as its own buffer.
		//Point-sampling a picture eight times finer than the slot is not a
		//downsample, it is an aliased one, and the queue would crawl frame to
		//frame while the live picture sat still.
		copyShader.Set( "InputTexture", 0 );
		copyShader.Set( "MaxUV", 1.0f, 1.0f );
		copyShader.Set( "HalfTexel", 0.0f, 0.0f );
		copyShader.Set( "SourceLod", std::log2( static_cast< float >( divisor ) ) );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 3. The trail. Every slot, oldest first.
	//
	// Oldest first matters for exactly one of the four blend modes -- Blend
	// is an over, and an over is not commutative -- and costs nothing in the
	// other three. Drawing newest-first would put the live frame underneath
	// its own history.
	//---------------------------------------------------------------------
	//Only Ghosts leaves the live frame out of the trail. It is the mode for
	//looking at what the decay is doing on its own; every other mode wants
	//the queue complete, because the trail IS the picture in those modes
	//rather than something laid over it.
	const bool includeLive = mode != kBackgroundGhosts;
	const int firstSlot    = includeLive ? 0 : 1;
	const int drawCount    = std::max( 0, filled - firstSlot );

	//The draw alphas, resolved once for the whole queue. Both the trail and
	//the halation loop read them, so the two cannot disagree about how much
	//of a ghost they are looking at. See ResolveDrawAlphas.
	float drawAlpha[ kMaxFrames ]     = {};
	float drawHalation[ kMaxFrames ]  = {};
	//Which arithmetic each blend does, and so which weights it needs. Add is
	//NOT the same as Blend here even though both are "accumulating": see the
	//Schedule enum in Decay.h.
	const afterglow::Schedule schedule =
		blend == kBlendLighten ? afterglow::kScheduleShape
		: blend == kBlendAdd   ? afterglow::kScheduleProportional
		                       : afterglow::kScheduleIncremental;

	ResolveDrawAlphas( decay, firstSlot, drawCount, schedule, drawAlpha, drawHalation );

	const float slotHalfTexelX = 0.5f / static_cast< float >( slotWidth );
	const float slotHalfTexelY = 0.5f / static_cast< float >( slotHeight );

	trailBuffer.Clear();
	halationBuffer[ 0 ].Clear();
	halationBuffer[ 1 ].Clear();

	auto accumulate = [ & ]( PassBuffer& target, bool bloom ) {
		ScopedFBOBinding fbo( target.GetGLID(), ScopedFBOBinding::RB_REVERT );
		target.ResizeViewPort();
		ScopedShaderBinding shader( ghostShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );

		ghostShader.Set( "SlotTexture", 0 );
		ghostShader.Set( "Aspect", aspect );
		ghostShader.Set( "WarpScale", warpScale );
		ghostShader.Set( "WarpPhase", static_cast< float >( warpPhase ) );
		ghostShader.Set( "Bloom", bloom ? 1.0f : 0.0f );

		const float tint = HalationTintFromParam( params[ PT_HALATION_TINT ] );
		ghostShader.Set( "BloomTint",
		                 1.0f + ( kHalationWarm[ 0 ] - 1.0f ) * tint,
		                 1.0f + ( kHalationWarm[ 1 ] - 1.0f ) * tint,
		                 1.0f + ( kHalationWarm[ 2 ] - 1.0f ) * tint );

		for( int age = filled - 1; age >= firstSlot; --age )
		{
			const Ghost ghost  = GhostAt( age, decay );
			const int schedule = age - firstSlot;
			const float weight = bloom ? drawHalation[ schedule ] : drawAlpha[ schedule ];
			if( weight <= 0.0f )
				continue;

			//The ring: age 0 is at the head and ages run backwards from it.
			//`+ frames` before the modulo because C++ gives a negative
			//left operand a negative remainder, and a negative index into
			//`slots` is not a wrong picture, it is a crash.
			const int index = ( head - age + frames ) % frames;

			//THE LIVE FRAME IS NEVER READ OUT OF THE QUEUE.
			//
			//Age 0 is the picture as it arrived, at full resolution, and it is
			//taken from the copy buffer rather than from the slot the capture
			//just wrote. Read it from the queue instead and Resolution stops
			//being a memory control and becomes a picture-quality one: at Half
			//the live frame is a bilinear upscale of a half-size copy of
			//itself, so in Source mode -- the default -- every static frame in
			//the show comes out softer than it went in, whether or not
			//anything in it was moving. The ghosts do not care, because they
			//are being crushed and warped and dimmed anyway; the live frame is
			//the one the operator is looking at.
			const bool live = age == 0;

			Scoped2DTextureBinding slotTexture(
				live ? copyBuffer.TextureID() : slots[ index ].TextureID() );

			ghostShader.Set( "SlotHalfTexel",
			                 live ? 0.5f / static_cast< float >( pictureWidth ) : slotHalfTexelX,
			                 live ? 0.5f / static_cast< float >( pictureHeight ) : slotHalfTexelY );
			ghostShader.Set( "Weight", weight );
			ghostShader.Set( "Offset", ghost.offsetX, ghost.offsetY );
			ghostShader.Set( "Scale", ghost.scale );
			ghostShader.Set( "Spin", ghost.spin );
			ghostShader.Set( "Levels", ghost.levels );
			ghostShader.Set( "Cells", ghost.cells );
			ghostShader.Set( "WarpAmount", ghost.warp );
			ghostShader.Set( "Hue", ghost.hue );
			ghostShader.Set( "Bleach", ghost.bleach );
			quad.Draw();
		}
	};

	glEnable( GL_BLEND );
	ApplyBlendMode( blend );
	accumulate( trailBuffer, false );

	//---------------------------------------------------------------------
	// 4. Halation: the same ghosts again, bright-passed, into a quarter-size
	//    buffer, then blurred. Skipped entirely when the control is at zero,
	//    which is a saving of Frames draws plus four blur passes -- on this
	//    plugin that is most of the frame.
	//---------------------------------------------------------------------
	const float halation = HalationFromParam( params[ PT_HALATION ] );
	if( halation > 0.0f )
	{
		glBlendEquation( GL_FUNC_ADD );
		glBlendFunc( GL_ONE, GL_ONE );
		accumulate( halationBuffer[ 0 ], true );

		glDisable( GL_BLEND );

		const float size  = HalationSizeFromParam( params[ PT_HALATION_SIZE ] ) * 0.001f;
		const float stepX = size;
		const float stepY = size * aspect;

		//Two Gaussians of different widths summed, rather than one wide one:
		//that is what gives a highlight a tight core and a wide falloff. The
		//wide pair is 1.8x and not 2.5x -- past that the five fetches start
		//showing as separate ghosts of the picture, which on THIS plugin is a
		//particularly bad joke.
		struct Stage
		{
			int from, to;
			float x, y;
		};
		const Stage stages[] = {
			{ 0, 1, stepX, 0.0f },
			{ 1, 0, 0.0f, stepY },
			{ 0, 1, stepX * 1.8f, 0.0f },
			{ 1, 0, 0.0f, stepY * 1.8f },
		};

		for( const Stage& stage : stages )
		{
			ScopedFBOBinding fbo( halationBuffer[ stage.to ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			halationBuffer[ stage.to ].ResizeViewPort();
			ScopedShaderBinding shader( blurShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			Scoped2DTextureBinding texture( halationBuffer[ stage.from ].TextureID() );

			blurShader.Set( "SourceTexture", 0 );
			blurShader.Set( "Direction", stage.x, stage.y );
			quad.Draw();
		}
	}

	//---------------------------------------------------------------------
	// 5. Composite, straight to the host's framebuffer.
	//---------------------------------------------------------------------
	glDisable( GL_BLEND );
	{
		//Back to the host's viewport. See the note where it was captured.
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( compositeShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding sourceTexture( copyBuffer.TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding trailTexture( trailBuffer.TextureID() );
		ScopedSamplerActivation sampler2( 2 );
		//The blur leaves its result in buffer 0 after an even number of
		//passes; when halation is off, that buffer is the one that was
		//cleared. Either way it is the one to read.
		Scoped2DTextureBinding halationTexture( halationBuffer[ 0 ].TextureID() );

		compositeShader.Set( "SourceTexture", 0 );
		compositeShader.Set( "TrailTexture", 1 );
		compositeShader.Set( "HalationTexture", 2 );

		compositeShader.Set( "Background", static_cast< float >( mode ) );
		compositeShader.Set( "Gain", gain );
		compositeShader.Set( "Halation", halation );
		compositeShader.Set( "MixAmount", params[ PT_MIX ] );
		quad.Draw();
	}

	//Put the host's blend state back exactly as it was found.
	glBlendEquation( hostBlendEquation );
	glBlendFunc( hostBlendSrc, hostBlendDst );
	if( hostBlend == GL_TRUE )
		glEnable( GL_BLEND );
	else
		glDisable( GL_BLEND );

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Afterglow::DeInitGL()
{
	copyShader.FreeGLResources();
	ghostShader.FreeGLResources();
	blurShader.FreeGLResources();
	compositeShader.FreeGLResources();
	quad.Release();

	copyBuffer.Destroy();
	trailBuffer.Destroy();
	halationBuffer[ 0 ].Destroy();
	halationBuffer[ 1 ].Destroy();
	for( int i = 0; i < kMaxFrames; ++i )
		slots[ i ].Destroy();

	queueWidth  = 0;
	queueHeight = 0;
	queueFrames = 0;
	head        = 0;
	filled      = 0;
	captureTick = 0;

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Afterglow::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	seedHostValues();

	// The About buttons open a browser and store nothing, so they are handled
	// before the params[] write below -- there is no value to keep.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_PRESET )
	{
		const int chosen = static_cast< int >( std::lround( value ) );
		if( chosen != static_cast< int >( std::lround( params[ PT_PRESET ] ) ) )
			applyPreset( chosen );
		return FF_SUCCESS;
	}

	// The host may be restating a value it still believes in rather than the
	// operator moving anything. Letting that through would overwrite the
	// preset's value in params[] AND read as an edit, dropping the dropdown
	// back to Custom -- which is what made presets look like they could not
	// be selected at all elsewhere in the fleet. See AGENTS.md.
	if( hostIsRestatingItself( index, value ) )
		return FF_SUCCESS;

	const float previous = params[ index ];
	params[ index ]      = value;

	// A slider moved while a preset is active means the operator has taken
	// over: the dropdown falls back to Custom. The equality guard matters --
	// hosts that honour the value events echo the preset's own values
	// straight back through here, and that echo must not un-set the preset.
	const int active = static_cast< int >( std::lround( params[ PT_PRESET ] ) );
	if( active > 0 && std::fabs( value - previous ) > 1e-4f )
	{
		for( unsigned int id : kPresetParamIDs )
		{
			if( id == index )
			{
				// Logged, unlike an ordinary parameter change: this one is a
				// state change an operator can be surprised by, it happens
				// once rather than per frame, and diagnosing the same defect
				// in vertigo needed a code read precisely because nothing
				// said it had happened.
				diag::info( "preset dropped to Custom: parameter "
				            + std::to_string( index ) + " moved to "
				            + std::to_string( value ) );
				params[ PT_PRESET ] = 0.0f;
				RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );
				break;
			}
		}
	}

	return FF_SUCCESS;
}

const unsigned int* Afterglow::PresetParamIDsForTest( int& count )
{
	count = afterglow::presets::kParamCount;
	return kPresetParamIDs;
}

float Afterglow::presetValue( int presetIndex, unsigned int id ) const
{
	if( presetIndex <= 0 || presetIndex > afterglow::presets::kCount )
		return -1.0f;

	const afterglow::presets::Preset& preset = afterglow::presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < afterglow::presets::kParamCount; ++j )
		if( kPresetParamIDs[ j ] == id )
			return preset.v[ j ];

	return -1.0f;
}

void Afterglow::seedHostValues()
{
	// Seeded on first parameter traffic rather than in the constructor, so
	// the whole mechanism stays in one place. It has to happen BEFORE
	// applyPreset can run: seeding afterwards would record the preset's own
	// values as the host's opening position, and the host's very next
	// restatement would then look like an edit -- which is the bug this
	// exists to fix, reintroduced.
	if( hostValuesSeeded )
		return;

	for( unsigned int i = 0; i < PT_COUNT; ++i )
		hostValues[ i ] = params[ i ];
	hostValuesSeeded = true;
}

bool Afterglow::hostIsRestatingItself( unsigned int index, float value )
{
	const float lastFromHost = hostValues[ index ];
	hostValues[ index ]      = value;

	const float fromPreset =
		presetValue( static_cast< int >( std::lround( params[ PT_PRESET ] ) ), index );
	if( fromPreset < 0.0f )
		return false;

	// A quantisation allowance rather than a float epsilon. A host that keeps
	// its parameters shorter than a float -- or round-trips them through a
	// UI, a MIDI value or a saved composition -- hands back a number near
	// ours rather than ours, and 1e-4 read that as an edit.
	constexpr float kSame = 1e-3f;

	if( std::fabs( value - fromPreset ) <= kSame )
	{
		// The host agreeing with the preset. Nothing to write -- and writing
		// it would actively hurt: a host that quantises hands back a ROUNDED
		// copy of our own value, params[] would take the rounding, and the
		// "did a covered parameter move?" test above works to a tighter
		// tolerance than this one and would read that rounding as an edit.
		return true;
	}

	if( std::fabs( value - lastFromHost ) > kSame )
		return false;//neither: the operator has taken over

	// Deliberately not logged. A host that pushes its parameters every frame
	// would put a line here every frame, and a log that scrolls is a log
	// nobody reads.
	return true;
}

void Afterglow::applyPreset( int presetIndex )
{
	params[ PT_PRESET ] = static_cast< float >( presetIndex );

	if( presetIndex <= 0 || presetIndex > presets::kCount )
		return;//Custom: the sliders keep whatever they said

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < presets::kParamCount; ++j )
	{
		const unsigned int id = kPresetParamIDs[ j ];
		if( std::fabs( params[ id ] - preset.v[ j ] ) <= 1e-6f )
			continue;

		// The copy is what changes the picture; the event only tells the host
		// to re-read the slider. A host that ignores it renders the preset
		// correctly and merely shows stale knobs.
		params[ id ] = preset.v[ j ];
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
	}
}

float Afterglow::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Afterglow::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

//---------------------------------------------------------------------------
FFResult Afterglow::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult Afterglow::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

void Afterglow::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}
