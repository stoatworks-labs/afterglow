/**
 * Afterglow — the ported halves of Controls.cpp and Decay.cpp.
 *
 * Split out of `plugin.js` so it can be imported by something that is not a
 * browser. `tools/check_decay.mjs` runs it against the plugin's own answers,
 * dumped by `agtest --ghosts`, and that check is in `tools/verify.sh`.
 *
 * That split is the point. The demo pages in this fleet copy the plugin's GLSL
 * verbatim and PORT its C++ by hand, and the ported half is the half nothing
 * was checking: a shader that drifts is caught by `check_shaders.py`, and a
 * conversion or a weight curve that drifts was caught by nobody. On this
 * plugin that gap is wide — everything about a ghost comes out of `GhostAt`,
 * so a mistake here is not a slider reading 0.47 instead of 0.5, it is the
 * whole trail being a different shape from the one the plugin makes.
 *
 * Nothing in this file touches the DOM or WebGL, and it must stay that way.
 *
 * The per-PIXEL half of Decay.cpp is deliberately NOT here. The page runs the
 * plugin's own GLSL for that, which is why the shader text is copied rather
 * than rewritten.
 */

//===========================================================================
// Controls.cpp — what a slider position means.
//===========================================================================

const clamp01 = (v) => Math.min(Math.max(v, 0), 1);
const lerp = (from, to, t) => from + (to - from) * clamp01(t);
const geometric = (from, to, t) => from * (to / from) ** clamp01(t);

/// 0.5 is the null and each half is one direction. Written as a signed
/// magnitude rather than `lerp(-limit, limit, v)` so the null is exactly zero
/// at exactly 0.5 — a trail that drifts imperceptibly with every control at
/// its default is the kind of defect nobody reports and everybody works around.
const bipolar = (v, limit) => (clamp01(v) * 2 - 1) * limit;

const framesFromParam = (v) => Math.round(geometric(4, 32, v));
const holdFromParam = (v) => Math.round(lerp(1, 8, v));
const curveFromParam = (v) => geometric(0.25, 4, v);
const gainFromParam = (v) => lerp(0, 2, v);
const driftFromParam = (v) => (v <= 0 ? 0 : geometric(0.002, 0.25, v));
const driftAngleFromParam = (v) => clamp01(v);
const zoomFromParam = (v) => bipolar(v, 0.5);
const spinFromParam = (v) => bipolar(v, 0.25);
const crushFromParam = (v) => clamp01(v);
const pixelateFromParam = (v) => clamp01(v);
const warpFromParam = (v) => (v <= 0 ? 0 : geometric(0.0004, 0.08, v));
const warpScaleFromParam = (v) => geometric(1, 64, v);
const warpSpeedFromParam = (v) => lerp(0, 2, v);
const hueFromParam = (v) => bipolar(v, 0.5);
const bleachFromParam = (v) => clamp01(v);
const halationFromParam = (v) => lerp(0, 2, v);
const halationSizeFromParam = (v) => geometric(0.5, 12, v);
const halationTintFromParam = (v) => clamp01(v);

/// The option's stored VALUE, not its slot in the list.
const resolutionDivisor = (value) => [1, 2, 4, 8][Math.round(value)] ?? 2;

//===========================================================================
// Decay.cpp — the per-SLOT half. The per-pixel half is in the GLSL above and
// is not written twice here; this page runs the plugin's own shader for that.
//===========================================================================

const TAU = 6.283185307179586;

/// The longest queue the plugin will hold, matching kMaxFrames.
const MAX_FRAMES = 32;

const ageOf = (slot, frames) =>
  (frames <= 1 ? 0 : clamp01(slot / (frames - 1)));

function ghostAt(slot, p) {
  const frames = Math.max(2, p.frames);
  const t = ageOf(slot, frames);

  // The oldest slot is worth exactly zero because it is the frame about to be
  // evicted, and the newest exactly one because it is the live picture. The
  // falloff runs to zero across the queue and is then normalised by its own
  // value at the head, so both ends are exact by construction.
  const u = (slot + 1) / frames;
  const head = (frames - 1) / frames;
  const fall = Math.max(0, 1 - u) ** p.curve;
  const norm = head ** p.curve;

  const weight = norm > 0 ? fall / norm : 0;
  const angle = p.driftAngle * TAU;
  const coarse = p.pixelate * t;

  return {
    weight,
    offsetX: Math.cos(angle) * p.drift * t,
    offsetY: Math.sin(angle) * p.drift * t,
    scale: 1 + p.zoom * t,
    spin: p.spin * t * TAU,
    levels: 2 ** (8 - 7 * p.crush * t),
    cells: coarse > 0 ? 2048 * (6 / 2048) ** coarse : 0,
    warp: p.warp * t,
    hue: p.hue * t,
    bleach: p.bleach * t,
    // Scaled by the age AND the weight, so halation peaks in the middle of the
    // queue: the newest frame has not aged into anything and the oldest is not
    // there any more.
    halation: p.halation * t * weight,
  };
}

const SCHEDULE_INCREMENTAL = 0;
const SCHEDULE_PROPORTIONAL = 1;
const SCHEDULE_SHAPE = 2;

/**
 * The shape weights turned into the alpha each ghost is drawn at.
 *
 * The property every schedule holds up is that a still picture goes through
 * unchanged. Blend composites (`dst*(1-a)+src`) so it wants the running
 * average; Add sums, so it wants alphas that add to one; Lighten takes a max,
 * which does not accumulate at all and would be brightest at the TAIL if it
 * were handed either of the others.
 *
 * Screen is the exception and says so: it lifts wherever ghosts genuinely
 * differ, which is the reason to pick it, and never darkens.
 */
function resolveDrawAlphas(p, firstSlot, count, schedule) {
  const alpha = new Float32Array(MAX_FRAMES);
  const halation = new Float32Array(MAX_FRAMES);
  if (count <= 0) return { alpha, halation };

  let total = 0;
  for (let i = 0; i < count; i += 1) total += ghostAt(firstSlot + i, p).weight;

  if (schedule === SCHEDULE_INCREMENTAL) {
    // Oldest first, which is also the order they are drawn in. The two have to
    // agree: this schedule IS the draw order written down.
    let running = 0;
    for (let i = count - 1; i >= 0; i -= 1) {
      const w = ghostAt(firstSlot + i, p).weight;
      running += w;
      alpha[i] = running > 0 ? w / running : 0;
    }
  } else if (schedule === SCHEDULE_PROPORTIONAL) {
    for (let i = 0; i < count; i += 1) {
      alpha[i] = total > 0 ? ghostAt(firstSlot + i, p).weight / total : 0;
    }
  } else {
    for (let i = 0; i < count; i += 1) alpha[i] = ghostAt(firstSlot + i, p).weight;
  }

  for (let i = 0; i < count; i += 1) {
    halation[i] = total > 0 ? ghostAt(firstSlot + i, p).halation / total : 0;
  }

  return { alpha, halation };
}

export {
  clamp01, lerp, geometric, bipolar,
  framesFromParam, holdFromParam, curveFromParam, gainFromParam,
  driftFromParam, driftAngleFromParam, zoomFromParam, spinFromParam,
  crushFromParam, pixelateFromParam, warpFromParam, warpScaleFromParam,
  warpSpeedFromParam, hueFromParam, bleachFromParam,
  halationFromParam, halationSizeFromParam, halationTintFromParam,
  resolutionDivisor,
  MAX_FRAMES, TAU, ageOf, ghostAt, resolveDrawAlphas,
  SCHEDULE_INCREMENTAL, SCHEDULE_PROPORTIONAL, SCHEDULE_SHAPE,
};
