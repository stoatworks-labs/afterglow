/**
 * Afterglow — browser demo.
 *
 * The shaders below are copied unedited from `source/Shaders.cpp`, the ghost
 * pass included: `GHOST_SHADER` is assembled from the same three pieces
 * `GhostShaderSource()` concatenates, so the decay library in the middle is
 * the same text the plugin compiles. `demo/tools/check_shaders.py` proves it,
 * character for character, and runs in `tools/verify.sh`.
 *
 * `Controls.cpp` (the 0..1 conversions), the per-slot half of `Decay.cpp`
 * (`GhostAt` and `ResolveDrawAlphas`) and `Presets.h` are ported below rather
 * than re-derived. The parameter declarations — names, groups, order, elements
 * and defaults — come from `Afterglow.cpp`'s constructor.
 *
 * The one idea, before any of it: **everything about a ghost is a function of
 * how old it is.** Nothing here looks at a pixel to decide how much a frame is
 * worth, where it has drifted to or how many bits it has left; it looks at one
 * number between 0 and 1. That is why the trail can be made to fall apart as
 * it goes rather than merely fade, and it is why the plugin keeps the frames
 * separately instead of folding them into one feedback buffer.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';
import {
  framesFromParam, holdFromParam, curveFromParam, gainFromParam,
  driftFromParam, driftAngleFromParam, zoomFromParam, spinFromParam,
  crushFromParam, pixelateFromParam, warpFromParam, warpScaleFromParam,
  warpSpeedFromParam, hueFromParam, bleachFromParam,
  halationFromParam, halationSizeFromParam, halationTintFromParam,
  resolutionDivisor,
  MAX_FRAMES, ghostAt, resolveDrawAlphas,
  SCHEDULE_INCREMENTAL, SCHEDULE_PROPORTIONAL, SCHEDULE_SHAPE,
} from './decay.js';

//===========================================================================
// The shaders. Copied from source/Shaders.cpp.
//===========================================================================

const VERTEX_SHADER = `#version 410 core

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
`;

const COPY_SHADER = `#version 410 core

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
`;

const DECAY_LIBRARY = `
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
`;

const GHOST_PREAMBLE = `#version 410 core

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
`;

const GHOST_MAIN = `
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
`;

const BLUR_SHADER = `#version 410 core

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
`;

const COMPOSITE_SHADER = `#version 410 core

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
`;

/// The ghost pass, assembled around the library exactly as GhostShaderSource()
/// does it. The three pieces are checked individually rather than as one
/// string, so a change to any of them is attributed to the piece it is in.
const GHOST_SHADER = GHOST_PREAMBLE + DECAY_LIBRARY + GHOST_MAIN;

//===========================================================================
// Controls.cpp and the per-slot half of Decay.cpp, in demo/decay.js so that
// something without a DOM can check them. See the note at the top of that
// file, and tools/check_decay.mjs.
//===========================================================================

//===========================================================================
// The options and the presets, from Afterglow.cpp and Presets.h.
//===========================================================================

/// Declared alphabetically in the plugin, and each entry keeps the value it
/// has always had — which for these two happens to be the same order.
const BLEND_NAMES = ['Add', 'Blend', 'Lighten', 'Screen'];
const BACKGROUND_NAMES = ['Black', 'Ghosts', 'Source', 'Transparent'];

/// Deliberately NOT alphabetical: this is a divisor, the position is the
/// meaning, and sorted it reads Eighth, Full, Half, Quarter.
const RESOLUTION_NAMES = ['Full', 'Half', 'Quarter', 'Eighth'];

const BLEND_ADD = 0;
const BLEND_OVER = 1;
const BLEND_LIGHTEN = 2;
const BLEND_SCREEN = 3;

const BACKGROUND_BLACK = 0;
const BACKGROUND_GHOSTS = 1;
const BACKGROUND_SOURCE = 2;
const BACKGROUND_TRANSPARENT = 3;

/// The parameters a preset sets, in the fixed order `presets::Param` declares.
const PRESET_PARAM_IDS = [
  'frames', 'hold', 'decay', 'blend', 'gain',
  'crush', 'pixelate', 'warp', 'warpScale', 'warpSpeed',
  'drift', 'direction', 'zoom', 'spin', 'hueShift', 'bleach',
  'halation', 'halationSize', 'halationTint', 'background',
];

const PRESET_TABLE = [
  ['Clean Echo', [0.53, 0.0, 0.50, 3, 0.50, 0, 0, 0, 0.50, 0.30,
    0, 0, 0.50, 0.50, 0.50, 0, 0, 0.45, 0.50, 2]],
  ['Slow Burn', [0.86, 0.43, 0.30, 3, 0.55, 0, 0, 0, 0.50, 0.30,
    0, 0, 0.50, 0.50, 0.50, 0.45, 0.70, 0.62, 0.80, 2]],
  ['Datamosh', [0.20, 0.14, 0.65, 0, 0.45, 0.85, 0.55, 0, 0.50, 0.30,
    0, 0, 0.50, 0.50, 0.60, 0, 0, 0.45, 0.50, 2]],
  ['Undertow', [0.75, 0.0, 0.42, 3, 0.55, 0, 0, 0, 0.50, 0.30,
    0.40, 0.75, 0.36, 0.50, 0.50, 0.30, 0.30, 0.55, 0.60, 2]],
  ['Spin Cycle', [0.67, 0.14, 0.50, 3, 0.60, 0, 0, 0, 0.50, 0.30,
    0, 0, 0.56, 0.62, 0.68, 0, 0.25, 0.45, 0.30, 2]],
  ['Bad Tape', [0.45, 0.29, 0.55, 1, 0.50, 0.60, 0, 0.62, 0.28, 0.45,
    0.30, 0, 0.50, 0.50, 0.44, 0.55, 0.40, 0.50, 0.70, 2]],
  ['Phosphor', [0.80, 0.14, 0.35, 3, 0.50, 0, 0, 0, 0.50, 0.30,
    0, 0, 0.50, 0.50, 0.50, 0.65, 0.85, 0.72, 0.90, 0]],
  ['Ghost Print', [0.62, 0.29, 0.45, 2, 0.70, 0.35, 0, 0, 0.50, 0.30,
    0.25, 0.13, 0.50, 0.50, 0.34, 0.40, 0.35, 0.55, 0.40, 1]],
];

const PRESETS = Object.fromEntries(
  PRESET_TABLE.map(([name, values]) => [
    name,
    Object.fromEntries(values.map((value, i) => [PRESET_PARAM_IDS[i], value])),
  ]),
);

//===========================================================================
// The chain, in the order ProcessOpenGL runs it.
//===========================================================================

/// Seconds of host time a single frame may advance the warp field by. The
/// host's clock is not ours: it jumps when the composition is scrubbed and by
/// however long the machine was asleep.
const MAX_FRAME_DELTA = 0.25;

/// The colour a halation ring actually is: red light scattering back through
/// the film base, with the emulsion having stopped the blue first.
const HALATION_WARM = [1.0, 0.72, 0.42];

class AfterglowRenderer {
  constructor(gl, quad) {
    this.gl = gl;
    this.quad = quad;

    this.copy = new Program(gl, VERTEX_SHADER, COPY_SHADER, 'copy');
    this.ghost = new Program(gl, VERTEX_SHADER, GHOST_SHADER, 'ghost');
    this.blur = new Program(gl, VERTEX_SHADER, BLUR_SHADER, 'blur');
    this.composite = new Program(gl, VERTEX_SHADER, COMPOSITE_SHADER, 'composite');

    this.copyBuffer = new PassBuffer(gl, { filter: 'linear', mip: true });
    this.trailBuffer = new PassBuffer(gl, { filter: 'linear' });
    this.halationBuffer = [
      new PassBuffer(gl, { filter: 'linear' }),
      new PassBuffer(gl, { filter: 'linear' }),
    ];

    // The queue. Thirty-two of them, allocated lazily by ensure().
    this.slots = Array.from({ length: MAX_FRAMES }, () => new PassBuffer(gl, { filter: 'linear' }));

    this.head = 0;
    this.filled = 0;
    this.captureTick = 0;
    this.queueWidth = 0;
    this.queueHeight = 0;
    this.queueFrames = 0;

    this.warpPhase = 0;
    this.lastTime = -1;
  }

  /**
   * Integrate the rate, exactly as the plugin does, and never rescale the
   * history: moving Warp Speed changes what happens next and nothing else.
   */
  advanceClock(params, time) {
    const speed = warpSpeedFromParam(params.get('warpSpeed'));
    if (this.lastTime >= 0) {
      // Restart sets this page's clock back to zero. The plugin would clamp
      // that to a zero delta and carry on from wherever it had got to; here it
      // means the visitor asked for the top, so the phase goes back with it
      // and the queue is emptied.
      if (time < this.lastTime) {
        this.warpPhase = 0;
        this.filled = 0;
        this.head = 0;
        this.captureTick = 0;
      } else {
        this.warpPhase += Math.min(time - this.lastTime, MAX_FRAME_DELTA) * speed;
      }
    }
    this.lastTime = time;
  }

  /**
   * Bring the queue to this size and length, emptying it if the ring's shape
   * moved. The ring's indexing is modulo its own length, so changing the
   * length does not shuffle the contents — it reinterprets them, and every
   * slot ends up filed under the wrong age.
   */
  ensureQueue(width, height, frames) {
    const gl = this.gl;
    const same = width === this.queueWidth && height === this.queueHeight
      && frames === this.queueFrames;

    for (let i = 0; i < MAX_FRAMES; i += 1) {
      if (i >= frames) {
        this.slots[i].dispose();
      } else {
        this.slots[i].ensure(width, height, gl.RGBA8);
      }
    }

    if (!same) {
      for (let i = 0; i < frames; i += 1) this.slots[i].clearTo(0, 0, 0, 0);
      this.head = 0;
      this.filled = 0;
      this.captureTick = 0;
      this.queueWidth = width;
      this.queueHeight = height;
      this.queueFrames = frames;
    }
  }

  applyBlendMode(mode) {
    const gl = this.gl;
    switch (mode) {
      case BLEND_ADD:
        gl.blendEquation(gl.FUNC_ADD);
        gl.blendFunc(gl.ONE, gl.ONE);
        break;
      case BLEND_OVER:
        gl.blendEquation(gl.FUNC_ADD);
        gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
        break;
      case BLEND_LIGHTEN:
        gl.blendEquation(gl.MAX);
        gl.blendFunc(gl.ONE, gl.ONE);
        break;
      case BLEND_SCREEN:
      default:
        gl.blendEquation(gl.FUNC_ADD);
        gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_COLOR);
        break;
    }
  }

  render({ input, params, width, height, time }) {
    const gl = this.gl;
    const quad = this.quad;

    this.advanceClock(params, time);

    const divisor = resolutionDivisor(params.option('resolution'));
    const hold = holdFromParam(params.get('hold'));
    const blend = params.option('blend');
    const background = params.option('background');

    const decay = {
      frames: framesFromParam(params.get('frames')),
      curve: curveFromParam(params.get('decay')),
      drift: driftFromParam(params.get('drift')),
      driftAngle: driftAngleFromParam(params.get('direction')),
      zoom: zoomFromParam(params.get('zoom')),
      spin: spinFromParam(params.get('spin')),
      crush: crushFromParam(params.get('crush')),
      pixelate: pixelateFromParam(params.get('pixelate')),
      warp: warpFromParam(params.get('warp')),
      hue: hueFromParam(params.get('hueShift')),
      bleach: bleachFromParam(params.get('bleach')),
      halation: halationFromParam(params.get('halation')),
    };

    const aspect = width / height;
    const slotWidth = Math.max(16, Math.floor(width / divisor));
    const slotHeight = Math.max(16, Math.floor(height / divisor));
    const halationWidth = Math.max(16, Math.floor(width / 4));
    const halationHeight = Math.max(16, Math.floor(height / 4));

    // Every ensure() before anything binds a texture, as in the plugin and for
    // the same reason: allocating a buffer disturbs the bindings around it.
    this.copyBuffer.ensure(width, height, gl.RGBA8);
    this.trailBuffer.ensure(width, height, gl.RGBA16F);
    this.halationBuffer[0].ensure(halationWidth, halationHeight, gl.RGBA16F);
    this.halationBuffer[1].ensure(halationWidth, halationHeight, gl.RGBA16F);
    this.ensureQueue(slotWidth, slotHeight, decay.frames);

    const frames = this.queueFrames;

    gl.disable(gl.BLEND);

    //---------------------------------------------------------------------
    // 1. The picture, into a texture of ours, with a mip chain on it.
    //---------------------------------------------------------------------
    this.copyBuffer.bind();
    this.copy.use();
    bindTexture(gl, 0, input.texture);
    // setSampler, not set. The plugin writes `Set( "InputTexture", 0 )` and
    // the SDK picks glUniform1i from the C++ type; here a bare 0 is a float,
    // and glUniform1f on a sampler is rejected — leaving every sampler on unit
    // 0, which renders a plausible picture built from the wrong texture.
    this.copy.setSampler('InputTexture', 0);
    this.copy.set('MaxUV', 1, 1);
    this.copy.set('HalfTexel', 0.5 / width, 0.5 / height);
    this.copy.set('SourceLod', 0);
    quad.draw();
    this.copyBuffer.generateMipmap();

    //---------------------------------------------------------------------
    // 2. Capture. The head advances every Hold frames; the picture is written
    //    every frame. Advancing and writing together would leave the newest
    //    slot up to Hold-1 frames stale, and the live picture would judder.
    //---------------------------------------------------------------------
    if (this.filled > 0 && this.captureTick % hold === 0) {
      this.head = (this.head + 1) % frames;
      if (this.filled < frames) this.filled += 1;
    }
    if (this.filled === 0) this.filled = 1;
    this.captureTick += 1;

    this.slots[this.head].bind();
    this.copy.use();
    bindTexture(gl, 0, this.copyBuffer.texture);
    this.copy.setSampler('InputTexture', 0);
    this.copy.set('MaxUV', 1, 1);
    this.copy.set('HalfTexel', 0, 0);
    // The mip level is why the copy exists as its own buffer: point-sampling a
    // picture eight times finer than the slot is an aliased downsample, and
    // the queue would crawl frame to frame while the live picture sat still.
    this.copy.set('SourceLod', Math.log2(divisor));
    quad.draw();

    //---------------------------------------------------------------------
    // 3. The trail. Every slot, oldest first — which matters for Blend, an
    //    over, and costs nothing in the other three.
    //---------------------------------------------------------------------
    const includeLive = background !== BACKGROUND_GHOSTS;
    const firstSlot = includeLive ? 0 : 1;
    const drawCount = Math.max(0, this.filled - firstSlot);

    const schedule = blend === BLEND_LIGHTEN ? SCHEDULE_SHAPE
      : blend === BLEND_ADD ? SCHEDULE_PROPORTIONAL
        : SCHEDULE_INCREMENTAL;
    const weights = resolveDrawAlphas(decay, firstSlot, drawCount, schedule);

    const warpScale = warpScaleFromParam(params.get('warpScale'));
    const tintAmount = halationTintFromParam(params.get('halationTint'));
    const tint = HALATION_WARM.map((c) => 1 + (c - 1) * tintAmount);

    const accumulate = (target, bloom) => {
      target.bind();
      this.ghost.use();
      this.ghost.setSampler('SlotTexture', 0);
      this.ghost.set('Aspect', aspect);
      this.ghost.set('WarpScale', warpScale);
      this.ghost.set('WarpPhase', this.warpPhase);
      this.ghost.set('Bloom', bloom ? 1 : 0);
      this.ghost.set('BloomTint', tint[0], tint[1], tint[2]);

      for (let age = this.filled - 1; age >= firstSlot; age -= 1) {
        const index = age - firstSlot;
        const weight = bloom ? weights.halation[index] : weights.alpha[index];
        if (weight <= 0) continue;

        const g = ghostAt(age, decay);

        // THE LIVE FRAME IS NEVER READ OUT OF THE QUEUE. Age 0 is the picture
        // as it arrived, at full resolution. Read it from the queue instead
        // and Resolution stops being a memory control and becomes a
        // picture-quality one.
        const live = age === 0;
        const slot = live ? this.copyBuffer : this.slots[(this.head - age + frames) % frames];

        bindTexture(gl, 0, slot.texture);
        this.ghost.set('SlotHalfTexel', 0.5 / slot.width, 0.5 / slot.height);
        this.ghost.set('Weight', weight);
        this.ghost.set('Offset', g.offsetX, g.offsetY);
        this.ghost.set('Scale', g.scale);
        this.ghost.set('Spin', g.spin);
        this.ghost.set('Levels', g.levels);
        this.ghost.set('Cells', g.cells);
        this.ghost.set('WarpAmount', g.warp);
        this.ghost.set('Hue', g.hue);
        this.ghost.set('Bleach', g.bleach);
        quad.draw();
      }
    };

    this.trailBuffer.clearTo(0, 0, 0, 0);
    this.halationBuffer[0].clearTo(0, 0, 0, 0);
    this.halationBuffer[1].clearTo(0, 0, 0, 0);

    gl.enable(gl.BLEND);
    this.applyBlendMode(blend);
    accumulate(this.trailBuffer, false);

    //---------------------------------------------------------------------
    // 4. Halation: the same ghosts again, bright-passed into a quarter-size
    //    buffer, then blurred. Skipped entirely at zero, which on this plugin
    //    is most of the frame.
    //---------------------------------------------------------------------
    const halation = halationFromParam(params.get('halation'));
    if (halation > 0) {
      gl.blendEquation(gl.FUNC_ADD);
      gl.blendFunc(gl.ONE, gl.ONE);
      accumulate(this.halationBuffer[0], true);

      gl.disable(gl.BLEND);

      const size = halationSizeFromParam(params.get('halationSize')) * 0.001;
      const stages = [
        [0, 1, size, 0],
        [1, 0, 0, size * aspect],
        [0, 1, size * 1.8, 0],
        [1, 0, 0, size * aspect * 1.8],
      ];
      for (const [from, to, x, y] of stages) {
        this.halationBuffer[to].bind();
        this.blur.use();
        bindTexture(gl, 0, this.halationBuffer[from].texture);
        this.blur.setSampler('SourceTexture', 0);
        this.blur.set('Direction', x, y);
        quad.draw();
      }
    }

    //---------------------------------------------------------------------
    // 5. Composite, straight to the canvas.
    //---------------------------------------------------------------------
    gl.disable(gl.BLEND);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, width, height);

    this.composite.use();
    bindTexture(gl, 0, this.copyBuffer.texture);
    bindTexture(gl, 1, this.trailBuffer.texture);
    bindTexture(gl, 2, this.halationBuffer[0].texture);
    this.composite.setSampler('SourceTexture', 0);
    this.composite.setSampler('TrailTexture', 1);
    this.composite.setSampler('HalationTexture', 2);
    this.composite.set('Background', background);
    this.composite.set('Gain', gainFromParam(params.get('gain')));
    this.composite.set('Halation', halation);
    this.composite.set('MixAmount', params.get('mix'));
    quad.draw();
  }
}

//===========================================================================

const pct = (v) => `${Math.round(v * 100)}%`;

mountDemo({
  name: 'Afterglow',
  pluginId: 'AG01',
  tagline: 'Keeps the last few dozen frames and lays them back over the picture, each one further gone than the one in front of it.',
  repo: 'https://github.com/stoatworks-labs/afterglow',
  needFloat: true,
  showBackdrop: true,
  presets: PRESETS,

  // Bright things on near-black first: a trail is only legible where there is
  // something moving against something dark, and a full-contrast photographic
  // frame smears everything at once — which is the plugin working correctly
  // and the worst possible first impression of it.
  //
  // Colour bars is the one clip here that does NOT move, and it earns its
  // place for that: with Blend, Add or Lighten it comes out identical to the
  // input, which is the invariant the whole weight schedule is built on.
  sources: ['spot', 'scene', 'grid', 'alpha', 'detail', 'bars', 'ramp'],

  differences: [
    'The queue holds whatever frames it was given, in the order it was given them — which is true in Resolume too. The OpenFX build of this plugin is the odd one out: it fetches frame t-3 from the clip rather than remembering it, so it is exact where these two are merely faithful.',
    'A browser tab that loses focus throttles its animation frames. That does not change what the trail looks like — the queue holds frames, not seconds — but it does change how much real time Frames × Hold covers, so a trail measured in seconds is only right while the tab is in front.',
    'Restart empties the queue as well as putting the clock back, because on this page a backwards clock is the visitor asking for the top. The plugin would clamp the delta and carry on with the frames it already had.',
    'Preset is an option parameter in the plugin, with Custom as element 0 and a slider edit dropping back to it. Here the same eight presets are in the panel header instead, from the plugin\'s own table.',
    'Resolution is a real memory control in the plugin — thirty-two 4K frames is a gigabyte of video memory. Here it does the same thing to the same buffers, but the canvas is small enough that the number never matters.',
  ],

  params: [
    //---- Trail ------------------------------------------------------------
    {
      id: 'frames', name: 'Frames', type: 'standard', default: 0.53, group: 'Trail',
      display: (v) => `${framesFromParam(v)}`,
      hint: 'How many frames are in the queue. Four is the floor because the oldest slot is deliberately weighted to zero — it is the frame on its way out, and letting it leave at zero rather than at whatever it happened to be is what stops the tail of the trail flickering.',
    },
    {
      id: 'hold', name: 'Hold', type: 'standard', default: 0.0, group: 'Trail',
      display: (v) => `${holdFromParam(v)} frame${holdFromParam(v) === 1 ? '' : 's'}`,
      hint: 'Host frames per slot. The trail spans Frames × Hold frames, so this is how a dozen ghosts cover two seconds — and it costs nothing, because holding is not rendering the same thing more often, it is capturing less often.',
    },
    {
      id: 'decay', name: 'Decay', type: 'standard', default: 0.50, group: 'Trail',
      display: (v) => `×${curveFromParam(v).toFixed(2)}`,
      hint: 'The exponent on the weight falloff. Below 1 the trail hangs on and drops away late — long, even, filmic. Above 1 only the last few frames read, which is the tight double-image an interlaced monitor gives.',
    },
    {
      id: 'blend', name: 'Blend', type: 'option', default: 1, group: 'Trail',
      elements: BLEND_NAMES,
      hint: 'Blend is the weighted average — the queue interpolated into one picture, and the default because it is the only mode that provably leaves footage that is not moving exactly as it was. Try Colour bars with each of them: Add and Lighten pass it through untouched too, Screen lifts it.',
    },
    {
      id: 'gain', name: 'Gain', type: 'standard', default: 0.50, group: 'Trail',
      display: (v) => gainFromParam(v).toFixed(2),
      hint: 'The trail\'s level against the background. It multiplies a premultiplied colour, so below 1 it fades the trail towards the background rather than towards black.',
    },
    {
      id: 'resolution', name: 'Resolution', type: 'option', default: 0, group: 'Trail',
      elements: RESOLUTION_NAMES,
      hint: 'What the queue is stored at. The live frame is always full resolution whatever this says; only the ghosts are reduced, which is why the picture does not go soft when the memory does.',
    },

    //---- Decay ------------------------------------------------------------
    {
      id: 'crush', name: 'Crush', type: 'standard', default: 0.0, group: 'Decay',
      display: (v) => `${(8 - 7 * v).toFixed(1)} bit`,
      hint: 'The bit depth the oldest ghost is down to. Levels rather than bits, so dragging it is a smooth loss of resolution instead of eight steps with nothing between them — and it rounds rather than truncating, or the trail would go dim as it went chunky.',
    },
    {
      id: 'pixelate', name: 'Pixelate', type: 'standard', default: 0.0, group: 'Decay',
      display: (v) => (v > 0 ? `${Math.round(2048 * (6 / 2048) ** v)} cells` : 'off'),
      hint: 'How coarse the oldest ghost\'s grid is. Snapped after the ghost has been moved, so the cells stay square on screen and stay put while the ghost drifts underneath them.',
    },
    {
      id: 'warp', name: 'Warp', type: 'standard', default: 0.0, group: 'Decay',
      display: (v) => (v > 0 ? `${(warpFromParam(v) * 100).toFixed(2)}%` : 'off'),
      hint: 'Displacement the oldest ghost is warped by, as a fraction of the picture width. The field is value noise on an integer lattice — never fract(sin(x)·43758), which is a different function on every driver and would put the CPU and GPU builds permanently out of step.',
    },
    {
      id: 'warpScale', name: 'Warp Scale', type: 'standard', default: 0.50, group: 'Decay',
      display: (v) => `${warpScaleFromParam(v).toFixed(1)} cells`,
      hint: 'Low is a slow swell that moves the whole ghost; high is boiling grain.',
    },
    {
      id: 'warpSpeed', name: 'Warp Speed', type: 'standard', default: 0.30, group: 'Decay',
      display: (v) => `${warpSpeedFromParam(v).toFixed(2)} /s`,
      hint: 'The noise has a third axis and it is time, so the field boils rather than sliding past. A warp that only translates reads as the ghost being pushed by something, which is a different effect.',
    },
    {
      id: 'drift', name: 'Drift', type: 'standard', default: 0.0, group: 'Decay',
      display: (v) => (v > 0 ? `${(driftFromParam(v) * 100).toFixed(1)}%` : 'off'),
      hint: 'How far the oldest ghost has travelled, as a fraction of the picture width. Everything geometric here is in picture widths on BOTH axes, so drift covers the same distance whichever way it goes.',
    },
    {
      id: 'direction', name: 'Direction', type: 'standard', default: 0.0, group: 'Decay',
      display: (v) => `${Math.round(driftAngleFromParam(v) * 360)}°`,
      hint: 'Which way Drift goes.',
    },
    {
      id: 'zoom', name: 'Zoom', type: 'standard', default: 0.50, group: 'Decay',
      display: (v) => `${zoomFromParam(v) >= 0 ? '+' : ''}${(zoomFromParam(v) * 100).toFixed(0)}%`,
      hint: 'Bipolar: the middle is off. Below it the trail recedes into the picture; above it the trail grows out past the edges.',
    },
    {
      id: 'spin', name: 'Spin', type: 'standard', default: 0.50, group: 'Decay',
      display: (v) => `${(spinFromParam(v) * 360).toFixed(0)}°`,
      hint: 'Bipolar: the middle is off. About the centre of the picture, in a square space, so it is a rotation on a 16:9 frame and not a shear.',
    },
    {
      id: 'hueShift', name: 'Hue Shift', type: 'standard', default: 0.50, group: 'Decay',
      display: (v) => `${(hueFromParam(v) * 360).toFixed(0)}°`,
      hint: 'Bipolar: the middle is off. Half a turn is the complement, which is the point at which a trail stops reading as the same picture and starts reading as a second one.',
    },
    {
      id: 'bleach', name: 'Bleach', type: 'standard', default: 0.25, group: 'Decay',
      display: pct,
      hint: 'Saturation the oldest ghost has lost. On by default, because a trail that has aged into something is the whole point and an effect that needs a control found before it does anything recognisable is an effect nobody keeps.',
    },

    //---- Halation ---------------------------------------------------------
    {
      id: 'halation', name: 'Halation', type: 'standard', default: 0.25, group: 'Halation',
      display: (v) => halationFromParam(v).toFixed(2),
      hint: 'Highlights spilling off the ghosts. Weighted by the age AND by the weight, so it peaks in the middle of the queue: the newest frame has not aged into anything, and the oldest is not there any more.',
    },
    {
      id: 'halationSize', name: 'Halation Size', type: 'standard', default: 0.45, group: 'Halation',
      display: (v) => `${halationSizeFromParam(v).toFixed(2)}‰`,
      hint: 'As a fraction of the picture width, in per-mille. Two Gaussians of different widths summed, which is what gives a highlight a tight core and a wide falloff.',
    },
    {
      id: 'halationTint', name: 'Halation Tint', type: 'standard', default: 0.50, group: 'Halation',
      display: pct,
      hint: 'Neutral to amber. That is the colour a real halation ring is, because it is red light scattering back off the film base and the emulsion has stopped the blue first.',
    },

    //---- Output -----------------------------------------------------------
    {
      id: 'background', name: 'Background', type: 'option', default: 2, group: 'Output',
      elements: BACKGROUND_NAMES,
      hint: 'What is behind the trail. Ghosts also leaves the live frame OUT of the queue, which is how to see what the decay is actually doing rather than what it looks like under a sharp copy of the picture.',
    },
    {
      id: 'mix', name: 'Mix', type: 'standard', default: 1.0, group: 'Output', display: pct,
      hint: 'Dry/wet with the untouched clip.',
    },
  ],

  createRenderer: (gl, quad) => new AfterglowRenderer(gl, quad),
});
