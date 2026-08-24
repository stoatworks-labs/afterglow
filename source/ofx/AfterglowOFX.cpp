/// The OpenFX build of Afterglow, for DaVinci Resolve, Vegas, Nuke, Natron and
/// other OFX hosts.
///
/// The parameter mappings and the whole per-slot decay model live once --
/// `Controls.cpp` and `Decay.cpp` -- and this file links them. What is
/// mirrored is the per-pixel stage of `kDecayLibrary`, collapsed onto the CPU,
/// and only that; see the note at the top of `Decay.h`.
///
/// ------------------------------------------------ the one real difference
///
/// **The queue is not a queue here.** The FFGL build keeps a ring of textures
/// because FFGL hands it one frame at a time, in order, and there is no way to
/// ask for a frame it was not given. OFX is the opposite: frames render in any
/// order, alone and concurrently, and the host will fetch any frame of the
/// source clip on request. So there is nothing to keep -- slot `k` is simply
/// the source at `t - k * hold`, fetched through temporal clip access.
///
/// That is not a compromise, it is strictly better, and it is worth being
/// clear about why:
///
/// - **It is exact.** The FFGL queue holds whatever frames the host happened
///   to ask for. Scrub the composition, retrigger the clip, or drop a frame
///   under load and the trail holds pictures that were never adjacent. Here
///   `t - 3` is `t - 3`.
/// - **It is deterministic.** Rendering frame 500 alone gives the same
///   picture as rendering the whole timeline up to it. The FFGL build cannot
///   promise that and does not claim to.
/// - **It costs more.** Thirty-two source fetches and thirty-two resamples per
///   output frame, against one texture upload. That is the trade an offline
///   host is for.
///
/// The consequence to know about: **the first frames of a clip are short of
/// history**, because `t - k` is before the clip starts and the host returns
/// nothing for it. The trail builds over the first Frames x Hold frames rather
/// than being wrong, which is the same thing the FFGL build does after a
/// resize -- but here it happens at the head of every render range, including
/// a range that starts in the middle of the timeline.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

// After the OFX Support headers, which is where the OFX types come from.
#include "StoatworksAboutOFX.h"

#include "../Controls.h"
#include "../Decay.h"
#include "../Presets.h"

namespace
{
constexpr const char* kPluginIdentifier = "com.stoatworks.afterglow";
constexpr const char* kPluginName       = "Afterglow";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"A queue of recent frames, composited back over the picture with each one "
	"decaying further as it ages: dimmer, crushed to fewer bits, pixelated, "
	"warped, drifting, spinning and hue-shifted in proportion to how old it "
	"is, until it reaches the end of the queue and drops off.\n\n"
	"Everything a ghost looks like is a function of one number -- its age -- "
	"which is why the trail can be made to fall apart as it goes rather than "
	"merely fade.\n\n"
	"https://stoatworks-labs.com";

/// The longest queue, matching the FFGL build's kMaxFrames. Here it bounds
/// how many source frames one output frame may fetch, which is the number
/// that decides whether this is usable on a timeline.
constexpr int kMaxFrames = 32;

constexpr const char* kParamFrames       = "frames";
constexpr const char* kParamHold         = "hold";
constexpr const char* kParamCurve        = "decay";
constexpr const char* kParamBlend        = "blend";
constexpr const char* kParamGain         = "gain";
constexpr const char* kParamCrush        = "crush";
constexpr const char* kParamPixelate     = "pixelate";
constexpr const char* kParamWarp         = "warp";
constexpr const char* kParamWarpScale    = "warpScale";
constexpr const char* kParamWarpSpeed    = "warpSpeed";
constexpr const char* kParamDrift        = "drift";
constexpr const char* kParamDriftAngle   = "direction";
constexpr const char* kParamZoom         = "zoom";
constexpr const char* kParamSpin         = "spin";
constexpr const char* kParamHue          = "hueShift";
constexpr const char* kParamBleach       = "bleach";
constexpr const char* kParamHalation     = "halation";
constexpr const char* kParamHalationSize = "halationSize";
constexpr const char* kParamHalationTint = "halationTint";
constexpr const char* kParamBackground   = "background";
constexpr const char* kParamMix          = "mix";
constexpr const char* kParamPreset       = "preset";

const char* const kBlendNames[]      = { "Add", "Blend", "Lighten", "Screen" };
const char* const kBackgroundNames[] = { "Black", "Ghosts", "Source", "Transparent" };

enum BlendMode
{
	kBlendAdd     = 0,
	kBlendOver    = 1,
	kBlendLighten = 2,
	kBlendScreen  = 3
};

enum BackgroundMode
{
	kBackgroundBlack       = 0,
	kBackgroundGhosts      = 1,
	kBackgroundSource      = 2,
	kBackgroundTransparent = 3
};

/// The colour a halation ring actually is. Matches kHalationWarm in the FFGL
/// build; they are the same number for the same reason, and there is nothing
/// to share it through that would not be more machinery than the constant.
constexpr float kHalationWarm[ 3 ] = { 1.0f, 0.72f, 0.42f };

//---------------------------------------------------------------------------
// A small RGBA float image, and the sampling the GPU passes relied on.
//---------------------------------------------------------------------------
struct Plane4
{
	int w = 0, h = 0;
	std::vector< float > v;//!< RGBA interleaved

	void resize( int width, int height )
	{
		w = width;
		h = height;
		v.assign( size_t( w ) * h * 4, 0.0f );
	}
	bool empty() const
	{
		return w <= 0 || h <= 0;
	}
	const float* at( int x, int y ) const
	{
		x = std::clamp( x, 0, w - 1 );
		y = std::clamp( y, 0, h - 1 );
		return &v[ ( size_t( y ) * w + x ) * 4 ];
	}
	float* row( int y )
	{
		return &v[ size_t( y ) * w * 4 ];
	}
};

/// GL_LINEAR, uv in 0..1 with texel centres at half a texel.
void bilinear4( const Plane4& p, float u, float v, float out[ 4 ] )
{
	const float fx = u * p.w - 0.5f;
	const float fy = v * p.h - 0.5f;
	const int x0   = int( std::floor( fx ) );
	const int y0   = int( std::floor( fy ) );
	const float ax = fx - x0;
	const float ay = fy - y0;

	const float* p00 = p.at( x0, y0 );
	const float* p10 = p.at( x0 + 1, y0 );
	const float* p01 = p.at( x0, y0 + 1 );
	const float* p11 = p.at( x0 + 1, y0 + 1 );
	for( int k = 0; k < 4; ++k )
	{
		const float top    = p00[ k ] + ( p10[ k ] - p00[ k ] ) * ax;
		const float bottom = p01[ k ] + ( p11[ k ] - p01[ k ] ) * ax;
		out[ k ]           = top + ( bottom - top ) * ay;
	}
}

/// Run `body(y0, y1)` across the hardware threads.
void parallelRows( int height, const std::function< void( int, int ) >& body )
{
	const int workers = int( std::max( 1u, std::thread::hardware_concurrency() ) );
	const int chunk   = std::max( 1, ( height + workers - 1 ) / workers );
	std::vector< std::thread > pool;
	for( int y0 = 0; y0 < height; y0 += chunk )
	{
		const int y1 = std::min( height, y0 + chunk );
		pool.emplace_back( [ =, &body ] { body( y0, y1 ); } );
	}
	for( std::thread& t : pool )
		t.join();
}

float smoothstepf( float lo, float hi, float x )
{
	const float t = std::clamp( ( x - lo ) / ( hi - lo ), 0.0f, 1.0f );
	return t * t * ( 3.0f - 2.0f * t );
}

//---------------------------------------------------------------------------
// Everything one render computes before pixels are written.
//---------------------------------------------------------------------------
struct FrameSetup
{
	Plane4 source;  //!< the untouched picture, premultiplied
	Plane4 trail;   //!< every ghost, accumulated
	Plane4 halation;//!< quarter size, blurred

	float gain      = 1.0f;
	float halationGain = 0.0f;
	int background  = kBackgroundSource;
	float mixAmount = 1.0f;
};

class CompositeProcessorBase : public OFX::ImageProcessor
{
public:
	explicit CompositeProcessorBase( OFX::ImageEffect& effect ) :
		OFX::ImageProcessor( effect )
	{
	}

	void setSetup( const FrameSetup* v, bool premultipliedValue )
	{
		setup         = v;
		premultiplied = premultipliedValue;
	}

protected:
	const FrameSetup* setup = nullptr;
	bool premultiplied      = false;
};

template< class PIX, int nComponents, int maxValue >
class CompositeProcessor : public CompositeProcessorBase
{
public:
	explicit CompositeProcessor( OFX::ImageEffect& effect ) :
		CompositeProcessorBase( effect )
	{
	}

	void multiThreadProcessImages( OfxRectI window ) override
	{
		const OfxRectI bounds = _dstImg->getBounds();
		const int outW        = bounds.x2 - bounds.x1;
		const int outH        = bounds.y2 - bounds.y1;
		const FrameSetup& s   = *setup;

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			PIX* dstPix   = static_cast< PIX* >( _dstImg->getPixelAddress( window.x1, y ) );
			const float v = ( y - bounds.y1 + 0.5f ) / outH;
			const int py  = y - bounds.y1;

			for( int x = window.x1; x < window.x2; ++x, dstPix += nComponents )
			{
				const float u = ( x - bounds.x1 + 0.5f ) / outW;
				const int px  = x - bounds.x1;

				const float* source = s.source.at( px, py );
				const float* trail  = s.trail.at( px, py );

				float halo[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
				if( !s.halation.empty() )
					bilinear4( s.halation, u, v, halo );

				//= mirrored from the composite pass. See kCompositeShader.
				float lit[ 4 ];
				for( int k = 0; k < 4; ++k )
					lit[ k ] = trail[ k ] * s.gain;

				float back[ 4 ] = { 0.0f, 0.0f, 0.0f, 1.0f };
				if( s.background == kBackgroundSource )
				{
					for( int k = 0; k < 4; ++k )
						back[ k ] = source[ k ];
				}
				else if( s.background == kBackgroundTransparent )
				{
					back[ 3 ] = 0.0f;
				}

				//The trail goes OVER the background; the halation is added.
				const float cover = std::clamp( lit[ 3 ], 0.0f, 1.0f );

				float result[ 4 ];
				for( int k = 0; k < 3; ++k )
					result[ k ] = back[ k ] * ( 1.0f - cover ) + lit[ k ]
					              + halo[ k ] * s.halationGain;
				result[ 3 ] = std::clamp( back[ 3 ] * ( 1.0f - cover ) + lit[ 3 ]
				                              + halo[ 3 ] * s.halationGain,
				                          0.0f, 1.0f );

				double r = source[ 0 ] + ( result[ 0 ] - source[ 0 ] ) * s.mixAmount;
				double g = source[ 1 ] + ( result[ 1 ] - source[ 1 ] ) * s.mixAmount;
				double b = source[ 2 ] + ( result[ 2 ] - source[ 2 ] ) * s.mixAmount;
				double a = source[ 3 ] + ( result[ 3 ] - source[ 3 ] ) * s.mixAmount;

				a = std::clamp( a, 0.0, 1.0 );
				r = std::min( std::clamp( r, 0.0, 1.0 ), a );
				g = std::min( std::clamp( g, 0.0, 1.0 ), a );
				b = std::min( std::clamp( b, 0.0, 1.0 ), a );

				if( !premultiplied && nComponents == 4 && a > 0.0 )
				{
					r /= a;
					g /= a;
					b /= a;
				}

				dstPix[ 0 ] = quantise( r );
				dstPix[ 1 ] = quantise( g );
				dstPix[ 2 ] = quantise( b );
				if( nComponents == 4 )
					dstPix[ 3 ] = quantise( a );
			}
		}
	}

private:
	static PIX quantise( double v )
	{
		if( maxValue == 1 )
			return PIX( v );

		v = std::clamp( v, 0.0, 1.0 );
		return PIX( v * maxValue + 0.5 );
	}
};

class AfterglowPlugin : public OFX::ImageEffect
{
public:
	explicit AfterglowPlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		framesParam   = fetchDoubleParam( kParamFrames );
		holdParam     = fetchDoubleParam( kParamHold );
		curveParam    = fetchDoubleParam( kParamCurve );
		blendParam    = fetchChoiceParam( kParamBlend );
		gainParam     = fetchDoubleParam( kParamGain );
		crushParam    = fetchDoubleParam( kParamCrush );
		pixelateParam = fetchDoubleParam( kParamPixelate );
		warpParam     = fetchDoubleParam( kParamWarp );
		warpScaleParam = fetchDoubleParam( kParamWarpScale );
		warpSpeedParam = fetchDoubleParam( kParamWarpSpeed );
		driftParam    = fetchDoubleParam( kParamDrift );
		driftAngleParam = fetchDoubleParam( kParamDriftAngle );
		zoomParam     = fetchDoubleParam( kParamZoom );
		spinParam     = fetchDoubleParam( kParamSpin );
		hueParam      = fetchDoubleParam( kParamHue );
		bleachParam   = fetchDoubleParam( kParamBleach );
		halationParam = fetchDoubleParam( kParamHalation );
		halationSizeParam = fetchDoubleParam( kParamHalationSize );
		halationTintParam = fetchDoubleParam( kParamHalationTint );
		backgroundParam = fetchChoiceParam( kParamBackground );
		mixParam      = fetchDoubleParam( kParamMix );
		presetParam   = fetchChoiceParam( kParamPreset );
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		// The About links open a browser and change nothing about the render.
		if( stoatworks::about::ofx::changedParam( args, paramName ) )
			return;

		using namespace afterglow::presets;

		if( paramName == kParamPreset )
		{
			int chosen = 0;
			presetParam->getValue( chosen );
			if( chosen <= 0 || chosen > kCount || applyingPreset )
				return;

			// The copy IS the preset -- same table as the FFGL build, same
			// 0..1 space. One edit block so undo takes the whole preset back
			// at once.
			const Preset& p = kPresets[ chosen - 1 ];
			applyingPreset  = true;
			beginEditBlock( "Preset" );
			setDouble( framesParam, p.v[ kFrames ] );
			setDouble( holdParam, p.v[ kHold ] );
			setDouble( curveParam, p.v[ kCurve ] );
			setChoice( blendParam, p.v[ kBlend ] );
			setDouble( gainParam, p.v[ kGain ] );
			setDouble( crushParam, p.v[ kCrush ] );
			setDouble( pixelateParam, p.v[ kPixelate ] );
			setDouble( warpParam, p.v[ kWarp ] );
			setDouble( warpScaleParam, p.v[ kWarpScale ] );
			setDouble( warpSpeedParam, p.v[ kWarpSpeed ] );
			setDouble( driftParam, p.v[ kDrift ] );
			setDouble( driftAngleParam, p.v[ kDriftAngle ] );
			setDouble( zoomParam, p.v[ kZoom ] );
			setDouble( spinParam, p.v[ kSpin ] );
			setDouble( hueParam, p.v[ kHue ] );
			setDouble( bleachParam, p.v[ kBleach ] );
			setDouble( halationParam, p.v[ kHalation ] );
			setDouble( halationSizeParam, p.v[ kHalationSize ] );
			setDouble( halationTintParam, p.v[ kHalationTint ] );
			setChoice( backgroundParam, p.v[ kBackground ] );
			endEditBlock();
			applyingPreset = false;
			return;
		}

		// Editing a covered control while a preset is active hands control
		// back to the sliders. Judged by value, not by the change reason:
		// hosts are not consistent about reasons, but "still equal to the
		// preset" is unambiguous and also absorbs the host echoing our own
		// setValues.
		if( applyingPreset || args.reason == OFX::eChangeTime )
			return;

		int active = 0;
		presetParam->getValue( active );
		if( active <= 0 || active > kCount )
			return;

		const Preset& p    = kPresets[ active - 1 ];
		const bool covered =
			( paramName == kParamFrames && doubleDiffers( framesParam, p.v[ kFrames ] ) ) ||
			( paramName == kParamHold && doubleDiffers( holdParam, p.v[ kHold ] ) ) ||
			( paramName == kParamCurve && doubleDiffers( curveParam, p.v[ kCurve ] ) ) ||
			( paramName == kParamBlend && choiceDiffers( blendParam, p.v[ kBlend ] ) ) ||
			( paramName == kParamGain && doubleDiffers( gainParam, p.v[ kGain ] ) ) ||
			( paramName == kParamCrush && doubleDiffers( crushParam, p.v[ kCrush ] ) ) ||
			( paramName == kParamPixelate && doubleDiffers( pixelateParam, p.v[ kPixelate ] ) ) ||
			( paramName == kParamWarp && doubleDiffers( warpParam, p.v[ kWarp ] ) ) ||
			( paramName == kParamWarpScale && doubleDiffers( warpScaleParam, p.v[ kWarpScale ] ) ) ||
			( paramName == kParamWarpSpeed && doubleDiffers( warpSpeedParam, p.v[ kWarpSpeed ] ) ) ||
			( paramName == kParamDrift && doubleDiffers( driftParam, p.v[ kDrift ] ) ) ||
			( paramName == kParamDriftAngle && doubleDiffers( driftAngleParam, p.v[ kDriftAngle ] ) ) ||
			( paramName == kParamZoom && doubleDiffers( zoomParam, p.v[ kZoom ] ) ) ||
			( paramName == kParamSpin && doubleDiffers( spinParam, p.v[ kSpin ] ) ) ||
			( paramName == kParamHue && doubleDiffers( hueParam, p.v[ kHue ] ) ) ||
			( paramName == kParamBleach && doubleDiffers( bleachParam, p.v[ kBleach ] ) ) ||
			( paramName == kParamHalation && doubleDiffers( halationParam, p.v[ kHalation ] ) ) ||
			( paramName == kParamHalationSize && doubleDiffers( halationSizeParam, p.v[ kHalationSize ] ) ) ||
			( paramName == kParamHalationTint && doubleDiffers( halationTintParam, p.v[ kHalationTint ] ) ) ||
			( paramName == kParamBackground && choiceDiffers( backgroundParam, p.v[ kBackground ] ) );

		if( covered )
		{
			applyingPreset = true;
			presetParam->setValue( 0 );
			applyingPreset = false;
		}
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr< OFX::Image > src( srcClip->fetchImage( args.time ) );

		const bool premultiplied = srcClip->getPreMultiplication() == OFX::eImagePreMultiplied;

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();

		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		FrameSetup setup;
		buildSetup( args, *src, premultiplied, setup );

		switch( depth )
		{
		case OFX::eBitDepthUByte:
			comps == OFX::ePixelComponentRGBA
				? runComposite< CompositeProcessor< unsigned char, 4, 255 > >( args, dst.get(), setup, premultiplied )
				: runComposite< CompositeProcessor< unsigned char, 3, 255 > >( args, dst.get(), setup, premultiplied );
			break;
		case OFX::eBitDepthUShort:
			comps == OFX::ePixelComponentRGBA
				? runComposite< CompositeProcessor< unsigned short, 4, 65535 > >( args, dst.get(), setup, premultiplied )
				: runComposite< CompositeProcessor< unsigned short, 3, 65535 > >( args, dst.get(), setup, premultiplied );
			break;
		case OFX::eBitDepthFloat:
			comps == OFX::ePixelComponentRGBA
				? runComposite< CompositeProcessor< float, 4, 1 > >( args, dst.get(), setup, premultiplied )
				: runComposite< CompositeProcessor< float, 3, 1 > >( args, dst.get(), setup, premultiplied );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	/// The trail is a window of previous frames; tell the host so it can
	/// prefetch them. Without this a host is entitled to refuse the fetches
	/// and the effect renders as a plain pass-through with no error anywhere.
	void getFramesNeeded( const OFX::FramesNeededArguments& args, OFX::FramesNeededSetter& frames ) override
	{
		const int span = ( queueLength( args.time ) - 1 ) * holdAt( args.time );
		OfxRangeD range;
		range.min = args.time - span;
		range.max = args.time;
		frames.setFramesNeeded( *srcClip, range );
	}

private:
	int queueLength( double t ) const
	{
		return std::clamp( afterglow::FramesFromParam( float( framesParam->getValueAtTime( t ) ) ),
		                   2, kMaxFrames );
	}

	int holdAt( double t ) const
	{
		return std::max( 1, afterglow::HoldFromParam( float( holdParam->getValueAtTime( t ) ) ) );
	}

	/// Read an OFX image into a premultiplied float plane.
	static void toPlane( OFX::Image& img, bool premultiplied, Plane4& out )
	{
		const OfxRectI b = img.getBounds();
		const int w      = b.x2 - b.x1;
		const int h      = b.y2 - b.y1;
		out.resize( w, h );

		const OFX::BitDepthEnum depth       = img.getPixelDepth();
		const OFX::PixelComponentEnum comps = img.getPixelComponents();
		const int n                         = comps == OFX::ePixelComponentRGBA ? 4 : 3;

		parallelRows( h, [ & ]( int y0, int y1 ) {
			for( int y = y0; y < y1; ++y )
			{
				float* row = out.row( y );
				for( int x = 0; x < w; ++x )
				{
					const void* pixel = img.getPixelAddress( b.x1 + x, b.y1 + y );
					float rgba[ 4 ]   = { 0.0f, 0.0f, 0.0f, 1.0f };

					if( pixel != nullptr )
					{
						switch( depth )
						{
						case OFX::eBitDepthUByte:
						{
							const unsigned char* p = static_cast< const unsigned char* >( pixel );
							for( int k = 0; k < n; ++k )
								rgba[ k ] = float( p[ k ] ) / 255.0f;
							break;
						}
						case OFX::eBitDepthUShort:
						{
							const unsigned short* p = static_cast< const unsigned short* >( pixel );
							for( int k = 0; k < n; ++k )
								rgba[ k ] = float( p[ k ] ) / 65535.0f;
							break;
						}
						case OFX::eBitDepthFloat:
						{
							const float* p = static_cast< const float* >( pixel );
							for( int k = 0; k < n; ++k )
								rgba[ k ] = p[ k ];
							break;
						}
						default:
							break;
						}
					}

					if( n == 3 )
						rgba[ 3 ] = 1.0f;
					else if( !premultiplied )
						for( int k = 0; k < 3; ++k )
							rgba[ k ] *= rgba[ 3 ];

					std::memcpy( &row[ size_t( x ) * 4 ], rgba, sizeof( rgba ) );
				}
			}
		} );
	}

	/// One ghost, resampled and degraded, added into `target` under `blend`.
	///
	/// The geometry and the colour both come out of `Decay.cpp`, so this is
	/// the composite arithmetic and nothing else -- which is exactly the split
	/// the GPU build has, where the blend is GL state rather than shader code.
	static void accumulate( const Plane4& slotImage, const afterglow::Ghost& ghost,
	                        float alpha, int blend, float aspect,
	                        float warpScale, float warpPhase, bool bloom,
	                        const float tint[ 3 ], Plane4& target )
	{
		if( alpha <= 0.0f || slotImage.empty() || target.empty() )
			return;

		const int w = target.w;
		const int h = target.h;

		parallelRows( h, [ & ]( int y0, int y1 ) {
			for( int y = y0; y < y1; ++y )
			{
				float* row = target.row( y );
				const float v = ( float( y ) + 0.5f ) / float( h );

				for( int x = 0; x < w; ++x )
				{
					const float u = ( float( x ) + 0.5f ) / float( w );

					float su = 0.0f, sv = 0.0f;
					afterglow::GhostSampleUV( ghost, u, v, aspect, warpScale, warpPhase, su, sv );

					//Outside the picture there is nothing to show, and
					//clamping would drag the edge row across the frame.
					if( su < 0.0f || su > 1.0f || sv < 0.0f || sv > 1.0f )
						continue;

					float colour[ 4 ];
					bilinear4( slotImage, su, sv, colour );
					afterglow::DegradeColour( ghost, colour );

					if( bloom )
					{
						const float lum = 0.2126f * colour[ 0 ] + 0.7152f * colour[ 1 ]
						                  + 0.0722f * colour[ 2 ];
						const float knee = smoothstepf( 0.35f, 0.75f, lum );
						for( int k = 0; k < 3; ++k )
							colour[ k ] *= knee * tint[ k ];
					}

					for( int k = 0; k < 4; ++k )
						colour[ k ] *= alpha;

					float* dst = &row[ size_t( x ) * 4 ];
					switch( blend )
					{
					case kBlendOver:
						for( int k = 0; k < 4; ++k )
							dst[ k ] = dst[ k ] * ( 1.0f - colour[ 3 ] ) + colour[ k ];
						break;
					case kBlendLighten:
						for( int k = 0; k < 4; ++k )
							dst[ k ] = std::max( dst[ k ], colour[ k ] );
						break;
					case kBlendScreen:
						for( int k = 0; k < 4; ++k )
							dst[ k ] = colour[ k ] + dst[ k ] * ( 1.0f - colour[ k ] );
						break;
					case kBlendAdd:
					default:
						for( int k = 0; k < 4; ++k )
							dst[ k ] += colour[ k ];
						break;
					}
				}
			}
		} );
	}

	/// A separable five-fetch Gaussian, one axis. Mirrors kBlurShader.
	static void blur( Plane4& image, float stepX, float stepY )
	{
		if( image.empty() )
			return;

		static const float offsets[ 3 ] = { 0.0f, 1.3846153846f, 3.2307692308f };
		static const float weights[ 3 ] = { 0.2270270270f, 0.3162162162f, 0.0702702703f };

		Plane4 out;
		out.resize( image.w, image.h );

		parallelRows( image.h, [ & ]( int y0, int y1 ) {
			for( int y = y0; y < y1; ++y )
			{
				float* row = out.row( y );
				const float v = ( float( y ) + 0.5f ) / float( image.h );
				for( int x = 0; x < image.w; ++x )
				{
					const float u = ( float( x ) + 0.5f ) / float( image.w );

					float sum[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
					float tap[ 4 ];
					bilinear4( image, u, v, tap );
					for( int k = 0; k < 4; ++k )
						sum[ k ] = tap[ k ] * weights[ 0 ];

					for( int i = 1; i < 3; ++i )
					{
						bilinear4( image, u + stepX * offsets[ i ], v + stepY * offsets[ i ], tap );
						for( int k = 0; k < 4; ++k )
							sum[ k ] += tap[ k ] * weights[ i ];
						bilinear4( image, u - stepX * offsets[ i ], v - stepY * offsets[ i ], tap );
						for( int k = 0; k < 4; ++k )
							sum[ k ] += tap[ k ] * weights[ i ];
					}

					std::memcpy( &row[ size_t( x ) * 4 ], sum, sizeof( sum ) );
				}
			}
		} );

		image = std::move( out );
	}

	void buildSetup( const OFX::RenderArguments& args, OFX::Image& src, bool premultiplied,
	                 FrameSetup& setup )
	{
		using namespace afterglow;

		const double t = args.time;

		double fps = dstClip->getFrameRate();
		if( !( fps > 0.0 ) )
			fps = 24.0;

		toPlane( src, premultiplied, setup.source );
		const int w = setup.source.w;
		const int h = setup.source.h;
		if( w <= 0 || h <= 0 )
			return;

		const float aspect = float( w ) / float( h );

		DecayParams decay;
		decay.frames     = queueLength( t );
		decay.curve      = CurveFromParam( float( curveParam->getValueAtTime( t ) ) );
		decay.drift      = DriftFromParam( float( driftParam->getValueAtTime( t ) ) );
		decay.driftAngle = DriftAngleFromParam( float( driftAngleParam->getValueAtTime( t ) ) );
		decay.zoom       = ZoomFromParam( float( zoomParam->getValueAtTime( t ) ) );
		decay.spin       = SpinFromParam( float( spinParam->getValueAtTime( t ) ) );
		decay.crush      = CrushFromParam( float( crushParam->getValueAtTime( t ) ) );
		decay.pixelate   = PixelateFromParam( float( pixelateParam->getValueAtTime( t ) ) );
		decay.warp       = WarpFromParam( float( warpParam->getValueAtTime( t ) ) );
		decay.hue        = HueFromParam( float( hueParam->getValueAtTime( t ) ) );
		decay.bleach     = BleachFromParam( float( bleachParam->getValueAtTime( t ) ) );
		decay.halation   = HalationFromParam( float( halationParam->getValueAtTime( t ) ) );

		const float warpScale = WarpScaleFromParam( float( warpScaleParam->getValueAtTime( t ) ) );
		const float warpSpeed = WarpSpeedFromParam( float( warpSpeedParam->getValueAtTime( t ) ) );

		//Phase is `time * speed` and NOT integrated, unlike the FFGL build.
		//That is forced by OFX's time model: frames render in any order and
		//alone, so there is no previous frame to have integrated from. The
		//consequence is the one every plugin in the fleet with a clock has --
		//keyframing Warp Speed rescales the field's whole history rather than
		//changing what happens next -- and a deterministic frame matters more
		//in a host that renders them out of order.
		const float warpPhase = float( t / fps ) * warpSpeed;

		int blend = kBlendOver;
		blendParam->getValueAtTime( t, blend );
		int background = kBackgroundSource;
		backgroundParam->getValueAtTime( t, background );

		setup.gain         = GainFromParam( float( gainParam->getValueAtTime( t ) ) );
		setup.halationGain = decay.halation;
		setup.background   = background;
		setup.mixAmount    = float( mixParam->getValueAtTime( t ) );

		const int hold      = holdAt( t );
		const int firstSlot = background == kBackgroundGhosts ? 1 : 0;
		const int count     = std::max( 0, decay.frames - firstSlot );

		float alpha[ kMaxFrames ]     = {};
		float halation[ kMaxFrames ]  = {};
		//Which arithmetic each blend does, and so which weights it needs. Add
		//is NOT the same as Blend here: see the Schedule enum in Decay.h.
		const Schedule schedule = blend == kBlendLighten ? kScheduleShape
		                        : blend == kBlendAdd     ? kScheduleProportional
		                                                 : kScheduleIncremental;

		ResolveDrawAlphas( decay, firstSlot, count, schedule, alpha, halation );

		setup.trail.resize( w, h );

		const int halationW = std::max( 16, w / 4 );
		const int halationH = std::max( 16, h / 4 );
		const bool wantHalation = decay.halation > 0.0f;
		if( wantHalation )
			setup.halation.resize( halationW, halationH );

		const float tintAmount = HalationTintFromParam( float( halationTintParam->getValueAtTime( t ) ) );
		const float tint[ 3 ]  = {
            1.0f + ( kHalationWarm[ 0 ] - 1.0f ) * tintAmount,
            1.0f + ( kHalationWarm[ 1 ] - 1.0f ) * tintAmount,
            1.0f + ( kHalationWarm[ 2 ] - 1.0f ) * tintAmount
		};

		//Oldest first, matching the FFGL build's draw order -- which the
		//incremental weight schedule depends on. See ResolveDrawAlphas.
		for( int age = decay.frames - 1; age >= firstSlot; --age )
		{
			const int index = age - firstSlot;
			if( index >= count )
				continue;
			if( alpha[ index ] <= 0.0f && halation[ index ] <= 0.0f )
				continue;

			Plane4 slotImage;
			if( age == 0 )
			{
				slotImage = setup.source;
			}
			else
			{
				//The one real difference from the FFGL build: slot k is the
				//source at t - k*hold, fetched rather than remembered. Before
				//the start of the clip the host returns nothing, and the trail
				//is simply short of that many frames.
				std::unique_ptr< OFX::Image > past( srcClip->fetchImage( t - double( age * hold ) ) );
				if( !past )
					continue;
				toPlane( *past, premultiplied, slotImage );
				if( slotImage.w != w || slotImage.h != h )
					continue;
			}

			const Ghost ghost = GhostAt( age, decay );

			accumulate( slotImage, ghost, alpha[ index ], blend, aspect,
			            warpScale, warpPhase, false, tint, setup.trail );

			if( wantHalation )
				accumulate( slotImage, ghost, halation[ index ], kBlendAdd, aspect,
				            warpScale, warpPhase, true, tint, setup.halation );
		}

		if( wantHalation )
		{
			const float size  = HalationSizeFromParam( float( halationSizeParam->getValueAtTime( t ) ) ) * 0.001f;
			const float stepY = size * aspect;
			blur( setup.halation, size, 0.0f );
			blur( setup.halation, 0.0f, stepY );
			blur( setup.halation, size * 1.8f, 0.0f );
			blur( setup.halation, 0.0f, stepY * 1.8f );
		}
	}

	template< class Processor >
	void runComposite( const OFX::RenderArguments& args, OFX::Image* dst, const FrameSetup& setup,
	                   bool premultiplied )
	{
		Processor processor( *this );
		processor.setDstImg( dst );
		processor.setSetup( &setup, premultiplied );
		processor.setRenderWindow( args.renderWindow );
		processor.process();
	}

	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;

	OFX::DoubleParam* framesParam       = nullptr;
	OFX::DoubleParam* holdParam         = nullptr;
	OFX::DoubleParam* curveParam        = nullptr;
	OFX::ChoiceParam* blendParam        = nullptr;
	OFX::DoubleParam* gainParam         = nullptr;
	OFX::DoubleParam* crushParam        = nullptr;
	OFX::DoubleParam* pixelateParam     = nullptr;
	OFX::DoubleParam* warpParam         = nullptr;
	OFX::DoubleParam* warpScaleParam    = nullptr;
	OFX::DoubleParam* warpSpeedParam    = nullptr;
	OFX::DoubleParam* driftParam        = nullptr;
	OFX::DoubleParam* driftAngleParam   = nullptr;
	OFX::DoubleParam* zoomParam         = nullptr;
	OFX::DoubleParam* spinParam         = nullptr;
	OFX::DoubleParam* hueParam          = nullptr;
	OFX::DoubleParam* bleachParam       = nullptr;
	OFX::DoubleParam* halationParam     = nullptr;
	OFX::DoubleParam* halationSizeParam = nullptr;
	OFX::DoubleParam* halationTintParam = nullptr;
	OFX::ChoiceParam* backgroundParam   = nullptr;
	OFX::DoubleParam* mixParam          = nullptr;
	OFX::ChoiceParam* presetParam       = nullptr;

	// The preset table is plain floats; these give each param type its
	// reading of one. Option values are element indices.
	static bool doubleDiffers( OFX::DoubleParam* p, float v )
	{
		double current = 0.0;
		p->getValue( current );
		return std::fabs( current - double( v ) ) > 1e-4;
	}
	static bool choiceDiffers( OFX::ChoiceParam* p, float v )
	{
		int current = 0;
		p->getValue( current );
		return current != int( std::lround( v ) );
	}
	static void setDouble( OFX::DoubleParam* p, float v )
	{
		if( doubleDiffers( p, v ) )
			p->setValue( double( v ) );
	}
	static void setChoice( OFX::ChoiceParam* p, float v )
	{
		if( choiceDiffers( p, v ) )
			p->setValue( int( std::lround( v ) ) );
	}

	/// True while our own setValues are in flight, so the resulting
	/// changedParam callbacks are not mistaken for the operator editing.
	bool applyingPreset = false;
};

OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                          const char* name, const char* label, const char* hint, double def )
{
	OFX::DoubleParamDescriptor* p = desc.defineDoubleParam( name );
	p->setLabels( label, label, label );
	p->setHint( hint );
	p->setRange( 0.0, 1.0 );
	p->setDisplayRange( 0.0, 1.0 );
	p->setDefault( def );
	page->addChild( *p );
	return p;
}

} // namespace

mDeclarePluginFactory( AfterglowPluginFactory, {}, {} );

void AfterglowPluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// A ghost is fetched from anywhere in the frame -- Drift, Zoom, Spin and
	// Warp all sample outside the tile they are drawn into -- so no tiles.
	// Temporal access is the whole effect here rather than a detail of it.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( true );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
}

void AfterglowPluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );
	srcClip->setTemporalClipAccess( true );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );

	// Factory presets, from the same table the FFGL build reads (Presets.h).
	// Custom is not a preset: it means the sliders are the truth.
	OFX::ChoiceParamDescriptor* presetParam = desc.defineChoiceParam( kParamPreset );
	presetParam->setLabels( "Preset", "Preset", "Preset" );
	presetParam->setHint( "Named trails. Picking one sets the covered controls; editing any of "
	                      "them afterwards falls back to Custom. Mix is left alone, and so is "
	                      "Resolution -- which the FFGL build has and this one does not need." );
	presetParam->appendOption( "Custom" );
	for( int i = 0; i < afterglow::presets::kCount; ++i )
		presetParam->appendOption( afterglow::presets::kPresets[ i ].name );
	presetParam->setDefault( 0 );
	presetParam->setIsPersistant( true );
	presetParam->setEvaluateOnChange( false );//the copied values re-render; the label itself does not
	presetParam->setAnimates( false );
	page->addChild( *presetParam );

	OFX::GroupParamDescriptor* trail = desc.defineGroupParam( "Trail" );
	trail->setLabels( "Trail", "Trail", "Trail" );

	defineSlider( desc, page, kParamFrames, "Frames",
	              "4 to 32 frames in the queue. Every one of them is a separate source fetch "
	              "here, so this is also the render cost.",
	              0.53 )
		->setParent( *trail );
	defineSlider( desc, page, kParamHold, "Hold",
	              "1 to 8 frames per slot. The trail spans Frames x Hold frames, so this is how "
	              "a dozen ghosts cover two seconds.",
	              0.0 )
		->setParent( *trail );
	defineSlider( desc, page, kParamCurve, "Decay",
	              "How the weight falls off along the trail. Below the middle it hangs on and "
	              "drops away late; above it, only the last few frames read.",
	              0.50 )
		->setParent( *trail );

	OFX::ChoiceParamDescriptor* blendParam = desc.defineChoiceParam( kParamBlend );
	blendParam->setLabels( "Blend", "Blend", "Blend" );
	blendParam->setHint( "How the ghosts combine. Blend is the weighted average -- the queue "
	                     "interpolated into one picture, and the only mode that leaves footage "
	                     "that is not moving exactly as it was." );
	for( const char* name : kBlendNames )
		blendParam->appendOption( name );
	blendParam->setDefault( kBlendOver );
	blendParam->setParent( *trail );
	page->addChild( *blendParam );

	defineSlider( desc, page, kParamGain, "Gain",
	              "The trail's level against the background. Over 1 on purpose.", 0.50 )
		->setParent( *trail );

	OFX::GroupParamDescriptor* decay = desc.defineGroupParam( "Decay" );
	decay->setLabels( "Decay", "Decay", "Decay" );

	defineSlider( desc, page, kParamCrush, "Crush",
	              "Bit depth the oldest ghost is down to, from 8 bits to 1.", 0.0 )
		->setParent( *decay );
	defineSlider( desc, page, kParamPixelate, "Pixelate",
	              "How coarse the oldest ghost's grid is, down to six cells across.", 0.0 )
		->setParent( *decay );
	defineSlider( desc, page, kParamWarp, "Warp", "Displacement the oldest ghost is warped by.", 0.0 )
		->setParent( *decay );
	defineSlider( desc, page, kParamWarpScale, "Warp Scale",
	              "1 to 64 cells of noise across the picture: a slow swell, or boiling grain.", 0.50 )
		->setParent( *decay );
	defineSlider( desc, page, kParamWarpSpeed, "Warp Speed", "0 to 2 cycles a second; 0 is frozen.", 0.30 )
		->setParent( *decay );
	defineSlider( desc, page, kParamDrift, "Drift",
	              "How far the oldest ghost has travelled, up to a quarter of the frame.", 0.0 )
		->setParent( *decay );
	defineSlider( desc, page, kParamDriftAngle, "Direction", "Which way Drift goes.", 0.0 )
		->setParent( *decay );
	defineSlider( desc, page, kParamZoom, "Zoom",
	              "Bipolar: the middle is off. Below it the trail recedes into the picture, "
	              "above it the trail grows past the edges.",
	              0.50 )
		->setParent( *decay );
	defineSlider( desc, page, kParamSpin, "Spin", "Bipolar: the middle is off.", 0.50 )
		->setParent( *decay );
	defineSlider( desc, page, kParamHue, "Hue Shift",
	              "Bipolar: the middle is off. At the ends the oldest ghost is the complement of "
	              "the picture, which is where a trail stops reading as the same image.",
	              0.50 )
		->setParent( *decay );
	defineSlider( desc, page, kParamBleach, "Bleach", "Saturation the oldest ghost has lost.", 0.25 )
		->setParent( *decay );

	OFX::GroupParamDescriptor* halation = desc.defineGroupParam( "Halation" );
	halation->setLabels( "Halation", "Halation", "Halation" );

	defineSlider( desc, page, kParamHalation, "Halation",
	              "Highlights spilling off the ghosts. Peaks in the middle of the queue -- the "
	              "newest frame has not aged into anything and the oldest is not there any more.",
	              0.25 )
		->setParent( *halation );
	defineSlider( desc, page, kParamHalationSize, "Halation Size", "", 0.45 )
		->setParent( *halation );
	defineSlider( desc, page, kParamHalationTint, "Halation Tint",
	              "Neutral to amber: the colour a real halation ring is, because it is red light "
	              "scattering back through the film base.",
	              0.50 )
		->setParent( *halation );

	OFX::GroupParamDescriptor* output = desc.defineGroupParam( "Output" );
	output->setLabels( "Output", "Output", "Output" );

	OFX::ChoiceParamDescriptor* backgroundParam = desc.defineChoiceParam( kParamBackground );
	backgroundParam->setLabels( "Background", "Background", "Background" );
	backgroundParam->setHint( "What is behind the trail. Ghosts also leaves the live frame OUT of "
	                          "the queue, which is how to see what the decay is doing on its own." );
	for( const char* name : kBackgroundNames )
		backgroundParam->appendOption( name );
	backgroundParam->setDefault( kBackgroundSource );
	backgroundParam->setParent( *output );
	page->addChild( *backgroundParam );

	defineSlider( desc, page, kParamMix, "Mix", "Dry/wet with the untouched clip.", 1.0 )
		->setParent( *output );

	// The Stoatworks About block: a read-only credit line and one push button
	// per link, in a group that starts folded. Last, so it sits under the
	// effect's own controls.
	stoatworks::about::ofx::describe( desc, page );
}

OFX::ImageEffect* AfterglowPluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new AfterglowPlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static AfterglowPluginFactory* factory =
		new AfterglowPluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
