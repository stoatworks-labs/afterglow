#!/usr/bin/env node
/**
 * Prove the demo's ported maths is the plugin's maths.
 *
 * The rule the demo pages are built on is that the shader text is *copied* and
 * the C++ is *ported*. `check_shaders.py` enforces the first half. This is the
 * second half, and until it existed nothing enforced it at all: a conversion
 * that drifted, or a weight curve that drifted, produced a page that looked
 * plausible, ran without an error and did not behave like the plugin.
 *
 * On this plugin that gap is wider than on most of the fleet. Everything about
 * a ghost — how much it is worth, where it has drifted to, how many bits it has
 * left — comes out of `GhostAt`, so a mistake in the port is not a slider
 * reading 0.47 instead of 0.5, it is the whole trail being a different shape.
 * The specific defect that started this was real: Add was handed the
 * incremental alphas, which sum to about three over a dozen slots, and a static
 * frame came out 194/255 too bright.
 *
 *     node demo/tools/check_decay.mjs [--binary build/agtest]
 *
 * Exit status is 0 when everything agrees, 1 otherwise, so it goes in
 * `tools/verify.sh`.
 *
 * The plugin's side is single-precision C++ and this side is double-precision
 * JavaScript, so the two cannot agree bit for bit and a check that demanded it
 * would fail on arithmetic that is correct. The tolerance below is set to catch
 * a drifted CONSTANT — a range of 0.3 against 0.25, an exponent of 2 against
 * 2.17 — which misses by percent, not by 1e-7.
 */

import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

import {
  framesFromParam, holdFromParam, curveFromParam, gainFromParam,
  driftFromParam, driftAngleFromParam, zoomFromParam, spinFromParam,
  crushFromParam, pixelateFromParam, warpFromParam, warpScaleFromParam,
  warpSpeedFromParam, hueFromParam, bleachFromParam,
  halationFromParam, halationSizeFromParam, halationTintFromParam,
  resolutionDivisor,
  ghostAt, resolveDrawAlphas,
} from '../decay.js';

/// A float32 round trip's worth of headroom, and no more.
const TOLERANCE = 2e-6;

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');

const binaryIndex = process.argv.indexOf('--binary');
const BINARY = binaryIndex > 0 ? process.argv[binaryIndex + 1] : 'build/agtest';

let failures = 0;
let comparisons = 0;
let worst = 0;
let worstWhere = '';

function compare(where, got, want) {
  comparisons += 1;
  // Relative for anything large, absolute for anything near zero. A quantisation
  // level of 256 and a drift of 0.002 cannot share one absolute tolerance.
  const scale = Math.max(1, Math.abs(want));
  const delta = Math.abs(got - want) / scale;
  if (delta > worst) {
    worst = delta;
    worstWhere = where;
  }
  if (delta <= TOLERANCE) return;
  failures += 1;
  if (failures <= 12) {
    console.log(`  DRIFTED  ${where}: demo ${got} vs plugin ${want}`);
  }
}

let dump;
try {
  dump = JSON.parse(execFileSync(path.join(ROOT, BINARY), ['--ghosts'], { encoding: 'utf8' }));
} catch (error) {
  console.log(`could not run ${BINARY} --ghosts: ${error.message}`);
  console.log('build it first:  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build');
  process.exit(2);
}

//--------------------------------------------------------------------------
// Controls.cpp
//--------------------------------------------------------------------------
const MAPPINGS = {
  frames: framesFromParam,
  hold: holdFromParam,
  curve: curveFromParam,
  gain: gainFromParam,
  drift: driftFromParam,
  driftAngle: driftAngleFromParam,
  zoom: zoomFromParam,
  spin: spinFromParam,
  crush: crushFromParam,
  pixelate: pixelateFromParam,
  warp: warpFromParam,
  warpScale: warpScaleFromParam,
  warpSpeed: warpSpeedFromParam,
  hue: hueFromParam,
  bleach: bleachFromParam,
  halation: halationFromParam,
  halationSize: halationSizeFromParam,
  halationTint: halationTintFromParam,
};

for (const row of dump.controls) {
  for (const [name, fn] of Object.entries(MAPPINGS)) {
    compare(`controls[${row.v}].${name}`, fn(row.v), row[name]);
  }
  // Resolution is an option, so the plugin dumps the divisor for the option
  // value its own dump chose. Mirrored here rather than reconstructed.
  const option = row.v < 0.5 ? 0 : (row.v < 0.9 ? 2 : 3);
  compare(`controls[${row.v}].divisor`, resolutionDivisor(option), row.divisor);
}

//--------------------------------------------------------------------------
// Decay.cpp — GhostAt and ResolveDrawAlphas
//--------------------------------------------------------------------------
const GHOST_FIELDS = [
  'weight', 'offsetX', 'offsetY', 'scale', 'spin',
  'levels', 'cells', 'warp', 'hue', 'bleach', 'halation',
];

for (const kase of dump.cases) {
  const [
    rawFrames, , rawCurve, rawDrift, rawAngle, rawZoom, rawSpin,
    rawCrush, rawPixelate, rawWarp, rawHue, rawBleach, rawHalation,
  ] = kase.raw;

  const p = {
    frames: framesFromParam(rawFrames),
    curve: curveFromParam(rawCurve),
    drift: driftFromParam(rawDrift),
    driftAngle: driftAngleFromParam(rawAngle),
    zoom: zoomFromParam(rawZoom),
    spin: spinFromParam(rawSpin),
    crush: crushFromParam(rawCrush),
    pixelate: pixelateFromParam(rawPixelate),
    warp: warpFromParam(rawWarp),
    hue: hueFromParam(rawHue),
    bleach: bleachFromParam(rawBleach),
    halation: halationFromParam(rawHalation),
  };

  compare(`${kase.name}.frames`, p.frames, kase.frames);

  for (const want of kase.ghosts) {
    const got = ghostAt(want.slot, p);
    for (const field of GHOST_FIELDS) {
      compare(`${kase.name}.ghost[${want.slot}].${field}`, got[field], want[field]);
    }
  }

  for (const want of kase.schedules) {
    const count = Math.max(0, p.frames - want.firstSlot);
    const got = resolveDrawAlphas(p, want.firstSlot, count, want.schedule);
    const label = `${kase.name}.schedule[${want.schedule}, from ${want.firstSlot}]`;
    for (let i = 0; i < count; i += 1) {
      compare(`${label}.alpha[${i}]`, got.alpha[i], want.alpha[i]);
      compare(`${label}.halation[${i}]`, got.halation[i], want.halation[i]);
    }
  }
}

console.log(`${dump.cases.length} cases, ${comparisons} comparisons, ${failures} past ${TOLERANCE}`);
console.log(`largest relative difference ${worst.toExponential(3)}, at ${worstWhere}`);
if (failures > 12) console.log(`(${failures - 12} more not shown)`);
if (failures === 0) console.log('the demo is running the plugin\'s maths');
process.exit(failures === 0 ? 0 : 1);
