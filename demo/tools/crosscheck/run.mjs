#!/usr/bin/env node
// The JavaScript half of the PORT cross-check: the block of demo/plugin.js
// between its PORT markers, sliced out and run under node, printing the same
// scenario printer.cpp prints from the plugin's own C++. crosscheck.sh compares
// the two.
//
//     node run.mjs [--wrong active-line|knee] > js.txt
//
// --wrong breaks the sliced block on purpose (the active line as 52.5 us rather
// than 52; the gain law's knee 5 dB wide rather than 6), for crosscheck.sh's
// negative controls: the comparison must FAIL on each. A --wrong whose text is
// no longer in the block exits 2 rather than quietly running the right code.

import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(HERE, '..', '..', 'plugin.js'), 'utf8');

const begin = source.indexOf('// ==== BEGIN PORT');
const end = source.indexOf('// ==== END PORT');
if (begin < 0 || end < 0 || end < begin) {
  console.error('run.mjs: the PORT markers are missing from demo/plugin.js');
  process.exit(2);
}
let block = source.slice(begin, end);

const WRONG = {
  'active-line': ['const kActiveLineUs = f32(52.0);', 'const kActiveLineUs = f32(52.5);'],
  knee: ['const kKneeDb = f32(6.0);', 'const kKneeDb = f32(5.0);'],
};
const wrongAt = process.argv.indexOf('--wrong');
if (wrongAt >= 0) {
  const [from, to] = WRONG[process.argv[wrongAt + 1]] ?? [];
  if (!from || !block.includes(from)) {
    console.error(`run.mjs: --wrong ${process.argv[wrongAt + 1]} has nothing to break`);
    process.exit(2);
  }
  block = block.replace(from, to);
}

// eslint-disable-next-line no-new-func
const port = new Function(`${block}
return {
  f32, DEFAULTS, PRESETS, PRESET_TARGETS, toSettings, fillGainTables, timeConstantSamples,
  planScan, releaseCoefficient, samplesToSeconds, crossoverFor, ratioFromParam, emphasisDbFromParam,
  linkLevelDbFromParam, noiseFromParam, headroomFromParam, tiltFromParam,
  timeConstantUsFromParam, pivotFromParam, maxGainDbFromParam,
};`)();

const { f32 } = port;
const bits = new Uint32Array(1);
const view = new Float32Array(bits.buffer);
const f = (v) => {
  view[0] = v;
  return ` 0x${bits[0].toString(16).padStart(8, '0')}`;
};
const i = (v) => ` ${v | 0}`;
const out = [];

//-- the constructor's defaults, in HostValues order
const D = port.DEFAULTS;
out.push(`default${[D.sidechain, D.chroma, D.compress, D.emphasis, D.linkLevel, D.noise, D.headroom,
  D.expand, D.tilt, D.timeConstant, D.pivot, D.maxGain, D.audioAmount, D.audioBand, D.audioTilt, D.mix]
  .map(f).join('')}`);

//-- every slider position
for (let k = 0; k <= 1000; k += 1) {
  const v = f32(k / 1000);
  out.push(`control ${k}${[port.ratioFromParam, port.emphasisDbFromParam, port.linkLevelDbFromParam,
    port.noiseFromParam, port.headroomFromParam, port.tiltFromParam, port.timeConstantUsFromParam,
    port.pivotFromParam, port.maxGainDbFromParam].map((c) => f(c(v))).join('')}`);
}

//-- the gain tables
function settings(label, v) {
  const s = port.toSettings(v);
  out.push(`settings ${label}${i(s.sidechain)}${[s.chroma, s.compressRatio, s.emphasisDb, s.linkLevelDb,
    s.noise, s.headroom, s.expandRatio, s.tilt, s.timeConstantUs, s.pivot, s.maxGainDb, s.audioAmount,
    s.audioBand, s.audioTilt, s.mix].map(f).join('')}`);
  const t = port.fillGainTables(s);
  out.push(`tables ${label}${[...t.compress, ...t.expand].map(f).join('')}`);
}

settings('defaults', { ...D });
port.PRESETS.forEach(([, row], p) => {
  const v = { ...D };
  port.PRESET_TARGETS.forEach((id, column) => { v[id] = f32(row[column]); });
  settings(`preset${p + 1}`, v);
});

let n = 0;
for (const pivot of [0.0, 0.25, 0.5, 0.7, 0.823, 1.0]) {
  for (const compress of [0.0, 0.333, 0.667, 1.0]) {
    for (const expand of [0.0, 0.333, 0.667, 1.0]) {
      for (const maxGain of [0.0, 0.25, 0.5, 1.0]) {
        settings(`grid${n}`, { ...D, pivot: f32(pivot), compress: f32(compress), expand: f32(expand), maxGain: f32(maxGain) });
        n += 1;
      }
    }
  }
}

//-- the plan, the follower's seconds and the band split
for (const width of [64, 333, 640, 960, 1280, 1920, 3840]) {
  for (let k = 0; k <= 200; k += 1) {
    const v = f32(k / 200);
    const us = port.timeConstantUsFromParam(v);
    const tau = port.timeConstantSamples(us, width);
    const det = Math.max(1, Math.trunc(width / 4));
    const plan = port.planScan(tau, width, det);
    const cross = port.crossoverFor(width);
    let line = `plan ${width} ${k}${f(tau)}${f(plan.tau)}${f(plan.frameBlend)}${f(port.samplesToSeconds(tau, width))}`;
    line += `${f(cross.narrow)}${f(cross.wide)}${f(port.releaseCoefficient(plan.tau))}${i(plan.passes.length)}`;
    for (const pass of plan.passes) line += `${i(pass.offset)}${f(pass.decay)}`;
    out.push(line);
  }
}

process.stdout.write(`${out.join('\n')}\n`);
