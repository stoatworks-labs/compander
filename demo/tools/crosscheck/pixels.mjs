#!/usr/bin/env node
// The browser demo against the REAL plugin class, picture for picture.
//
//     node demo/tools/crosscheck/pixels.mjs [--port N (or PIXELS_PORT)] [--angle metal|swiftshader]
//                                           [--cmtest build/cmtest] [--out DIR]
//
// For each scenario below: headless Chrome loads harness.html, which drives
// demo/plugin.js's own renderer on a fixed clock (time = frame / fps) and reads
// back every input frame and every output frame. The input frames are then piped
// through `cmtest --pipe` -- the real compander::Plugin, its real shaders, in a
// GL 4.1 core context, with SetTime on the same clock -- at the same settings,
// and the two outputs are compared frame by frame.
//
// This is the only check of the page's GL side (the buffers, the pass order,
// the bindings) and of its frame logic as a whole (the clock, the frame
// follower's slot, decay and First flag, the noise's frame counter): the
// envelope state carried across frames has to be carried the same way or the
// later frames drift apart. What it cannot separate is the two GL stacks
// themselves -- desktop GL against WebGL2 through ANGLE -- so it measures
// rather than requires identity, against tolerances set in TOLERANCE below.
//
// It carries its own negative controls: the same comparison against the plugin
// run at a slightly different setting, or on a different clock, must FAIL the
// same tolerance. A comparer that cannot fail proves nothing.
//
// Needs Google Chrome, node, and a built cmtest (cmake --build build).
// Serves demo/ itself on --port (0, the default, lets the OS pick a free one);
// Chrome is driven over --remote-debugging-pipe, so it opens no port at all.

import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { mkdtempSync, readFileSync, rmSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, extname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { crc32, deflateSync } from 'node:zlib';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, '..', '..', '..');
const DEMO = join(REPO, 'demo');

const args = process.argv.slice(2);
const option = (name, fallback) => {
  const i = args.indexOf(name);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback;
};
const PORT = Number(option('--port', process.env.PIXELS_PORT ?? '0'));
const ANGLE = option('--angle', 'metal');
const CMTEST = resolve(REPO, option('--cmtest', 'build/cmtest'));
const OUT = option('--out', null);
const CHROME = process.env.CHROME ?? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';

// Host-facing names, for cmtest --set. The page's ids are its own.
const HOST_NAMES = {
  sidechain: 'Sidechain', chroma: 'Chroma', compress: 'Compress', emphasis: 'Emphasis',
  linkLevel: 'Link Level', noise: 'Noise', headroom: 'Headroom', expand: 'Expand', tilt: 'Tilt',
  timeConstant: 'Time Constant', pivot: 'Pivot', maxGain: 'Max Gain', audioAmount: 'Audio Amount', mix: 'Mix',
};

// Time Constant 0.61 is inside the octave where the frame follower fades in at
// 640 wide (frameBlend between 0 and 1); 0.85 and 1.0 are past it, where the
// envelope is the frame follower's alone and is carried from frame to frame.
const SCENARIOS = [
  { name: 'defaults, lights on black', clip: 'spot', width: 640, height: 360, fps: 30, frames: 24, values: {} },
  { name: 'defaults, synthetic scene', clip: 'scene', width: 640, height: 360, fps: 30, frames: 24, values: {} },
  { name: 'blend octave (TC 0.61)', clip: 'spot', width: 640, height: 360, fps: 30, frames: 24, values: { timeConstant: 0.61 } },
  { name: 'frame follower (TC 1.0), 30 fps', clip: 'spot', width: 640, height: 360, fps: 30, frames: 48, values: { timeConstant: 1.0 } },
  { name: 'frame follower (TC 0.85), 25 fps', clip: 'scene', width: 640, height: 360, fps: 25, frames: 40, values: { timeConstant: 0.85 } },
  { name: 'preset Satellite Link, bars', clip: 'bars', width: 640, height: 360, fps: 30, frames: 12, preset: 7 },
  { name: 'preset Encode Only, geometry', clip: 'grid', width: 640, height: 360, fps: 30, frames: 12, preset: 9 },
  { name: 'preset Decode Only, geometry', clip: 'grid', width: 640, height: 360, fps: 30, frames: 12, preset: 10 },
  { name: 'preset Diversity Fade, scene', clip: 'scene', width: 640, height: 360, fps: 30, frames: 24, preset: 4 },
  { name: 'Sidechain Audio (silent), ramp', clip: 'ramp', width: 640, height: 360, fps: 30, frames: 12, values: { sidechain: 1, audioAmount: 0.5 } },
  { name: 'Sidechain Picture x Audio, detail', clip: 'detail', width: 640, height: 360, fps: 30, frames: 12, values: { sidechain: 2 } },
  { name: 'preset Cheap Handheld, 960 wide', clip: 'spot', width: 960, height: 540, fps: 30, frames: 16, preset: 5 },
  // The follower's RELEASE, which none of the clips above reach (see harness.mjs).
  { name: 'follower release (TC 1.0), light switched down, 30 fps', synthetic: 'step', stepAt: 8, high: 1.0, low: 0.15,
    width: 640, height: 360, fps: 30, frames: 30, values: { timeConstant: 1.0 } },
  { name: 'follower release (TC 0.9), light switched down, 50 fps', synthetic: 'step', stepAt: 10, high: 0.9, low: 0.3,
    width: 640, height: 360, fps: 50, frames: 40, values: { timeConstant: 0.9 } },
];

// The same page frames against the plugin run WRONG on purpose. Each must
// exceed the tolerance, or the tolerance is too loose to mean anything.
const NEGATIVE = [
  { of: 0, name: 'plugin at Time Constant 0.56, page at 0.55', plugin: { timeConstant: 0.56 } },
  { of: 12, name: 'plugin told 25 fps, page ran 30 (follower release)', fps: 25 },
  { of: 13, name: 'plugin told 60 fps, page ran 50 (follower release)', fps: 60 },
  { of: 12, name: 'plugin at Time Constant 0.98, page at 1.0 (follower release)', plugin: { timeConstant: 0.98 } },
  { of: 1, name: 'plugin at Noise 0.14, page at 0.13', plugin: { noise: 0.14 } },
  { of: 6, name: 'plugin on Two-to-One VCA, page on Encode Only', preset: 1 },
];

// Per scenario, over every frame and every RGB channel value: the largest
// difference, in 255ths, and the mean. Set from what was measured on an M4 Max
// (ANGLE on Metal against Apple's GL 4.1, 2026-10-28): every one of 14
// scenarios came out with no channel more than 1/255 apart and a mean of at most
// 0.0008/255 -- the two GL stacks round a handful of values to the other side of
// a level, and that is all. The negative controls all differ by 2 to 124 levels.
// A different GPU may round differently; if this fails on one, look at the
// pictures (--out) before loosening it.
const TOLERANCE = { max: 1, mean: 0.005 };

//---------------------------------------------------------------------------
// The demo, served.
//---------------------------------------------------------------------------
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css' };

function serve() {
  const server = createServer((request, response) => {
    const path = decodeURIComponent(new URL(request.url, 'http://x').pathname);
    const file = resolve(DEMO, `.${path}`);
    if (!file.startsWith(DEMO) || !existsSync(file)) {
      response.writeHead(404).end();
      return;
    }
    response.writeHead(200, { 'Content-Type': MIME[extname(file)] ?? 'application/octet-stream' });
    response.end(readFileSync(file));
  });
  return new Promise((ok) => server.listen(PORT, '127.0.0.1', () => ok(server)));
}

//---------------------------------------------------------------------------
// Chrome, over the DevTools protocol on a pipe.
//---------------------------------------------------------------------------
function launchChrome() {
  const profile = mkdtempSync(join(tmpdir(), 'compander-pixels-'));
  const angle = ANGLE === 'swiftshader'
    ? ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader']
    : ['--use-angle=metal', '--ignore-gpu-blocklist'];
  const child = spawn(CHROME, [
    '--headless=new', '--no-first-run', '--no-default-browser-check', ...angle,
    `--user-data-dir=${profile}`, '--remote-debugging-pipe', 'about:blank',
  ], { stdio: ['ignore', 'ignore', 'ignore', 'pipe', 'pipe'] });

  const toChrome = child.stdio[3];
  const fromChrome = child.stdio[4];
  let nextId = 0;
  const waiting = new Map();
  let pending = Buffer.alloc(0);

  fromChrome.on('data', (chunk) => {
    pending = Buffer.concat([pending, chunk]);
    let end;
    while ((end = pending.indexOf(0)) >= 0) {
      const message = JSON.parse(pending.subarray(0, end).toString('utf8'));
      pending = pending.subarray(end + 1);
      if (message.id !== undefined && waiting.has(message.id)) {
        const { ok, fail, method } = waiting.get(message.id);
        waiting.delete(message.id);
        if (message.error) fail(new Error(`${method}: ${JSON.stringify(message.error)}`));
        else ok(message.result);
      }
    }
  });

  const send = (method, params = {}, sessionId = undefined) => new Promise((ok, fail) => {
    nextId += 1;
    waiting.set(nextId, { ok, fail, method });
    toChrome.write(`${JSON.stringify({ id: nextId, method, params, sessionId })}\0`);
  });

  // Chrome writes to its profile until it has gone, so wait for that.
  const close = () => new Promise((ok) => {
    child.once('exit', () => {
      rmSync(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 });
      ok();
    });
    child.kill();
  });
  return { send, close };
}

async function openPage(chrome, url) {
  const { targetId } = await chrome.send('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await chrome.send('Target.attachToTarget', { targetId, flatten: true });
  const call = (method, params) => chrome.send(method, params, sessionId);
  await call('Runtime.enable');
  await call('Page.navigate', { url });

  const evaluate = async (expression) => {
    const r = await call('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) {
      throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
    }
    return r.result.value;
  };

  for (let i = 0; i < 200; i += 1) {
    try {
      if (await evaluate('window.harnessReady === true')) return evaluate;
    } catch {
      // navigation in flight
    }
    await new Promise((ok) => setTimeout(ok, 100));
  }
  throw new Error('the harness never came up');
}

//---------------------------------------------------------------------------
// cmtest --pipe: the real plugin class.
//---------------------------------------------------------------------------
function flipRows(bytes, width, height) {
  const out = Buffer.alloc(bytes.length);
  const stride = width * 4;
  for (let y = 0; y < height; y += 1) {
    bytes.copy(out, y * stride, (height - 1 - y) * stride, (height - y) * stride);
  }
  return out;
}

function runPlugin(scenario, values, preset, fps, inputsBottomUp) {
  const { width, height } = scenario;
  const sets = [];
  for (const [id, name] of Object.entries(HOST_NAMES)) sets.push('--set', `${name}=${values[id]}`);
  // Last, so the values above are plain writes and the preset then applies
  // over them, as the operator picking it would.
  if (preset) sets.push('--set', `Preset=${preset}`);

  return new Promise((ok, fail) => {
    const child = spawn(CMTEST, ['--pipe', '--size', `${width}x${height}`, '--fps', String(fps), ...sets],
      { stdio: ['pipe', 'pipe', 'pipe'] });
    const chunks = [];
    let errors = '';
    child.stdout.on('data', (c) => chunks.push(c));
    child.stderr.on('data', (c) => { errors += c; });
    child.on('error', fail);
    child.on('close', (code) => {
      if (code !== 0) return fail(new Error(`cmtest exited ${code}: ${errors}`));
      const all = Buffer.concat(chunks);
      const size = width * height * 4;
      const frames = [];
      for (let i = 0; i + size <= all.length; i += size) frames.push(flipRows(all.subarray(i, i + size), width, height));
      ok(frames);
    });
    for (const frame of inputsBottomUp) child.stdin.write(flipRows(frame, width, height));
    child.stdin.end();
  });
}

//---------------------------------------------------------------------------
// Comparison.
//---------------------------------------------------------------------------
function compare(pageFrames, pluginFrames) {
  let total = 0;
  let sum = 0;
  let over1 = 0;
  let over2 = 0;
  let max = 0;
  let worstFrame = 0;
  let worstFrameMean = -1;
  const frames = Math.min(pageFrames.length, pluginFrames.length);
  for (let f = 0; f < frames; f += 1) {
    const a = pageFrames[f];
    const b = pluginFrames[f];
    let frameSum = 0;
    for (let i = 0; i < a.length; i += 4) {
      for (let c = 0; c < 3; c += 1) {
        const d = Math.abs(a[i + c] - b[i + c]);
        frameSum += d;
        if (d > 1) over1 += 1;
        if (d > 2) over2 += 1;
        if (d > max) max = d;
      }
    }
    sum += frameSum;
    total += (a.length / 4) * 3;
    const frameMean = frameSum / ((a.length / 4) * 3);
    if (frameMean > worstFrameMean) {
      worstFrameMean = frameMean;
      worstFrame = f;
    }
  }
  return {
    frames,
    mean: sum / total,
    over1: over1 / total,
    over2: over2 / total,
    max,
    worstFrame,
    worstFrameMean,
    missing: pageFrames.length !== pluginFrames.length,
  };
}

const within = (s) => !s.missing && s.max <= TOLERANCE.max && s.mean <= TOLERANCE.mean;

function describe(s) {
  return `${s.frames} frames, mean |d| ${s.mean.toFixed(4)}/255, ` +
    `${(100 * (1 - s.over1)).toFixed(3)}% within 1, ${(100 * (1 - s.over2)).toFixed(3)}% within 2, ` +
    `max ${s.max}, worst frame ${s.worstFrame} (mean ${s.worstFrameMean.toFixed(4)})`;
}

//---------------------------------------------------------------------------
// Pictures, for a reader: page | plugin | difference x16, last frame.
//---------------------------------------------------------------------------
function png(path, width, height, rgbaTopDown) {
  const raw = Buffer.alloc(height * (1 + width * 4));
  for (let y = 0; y < height; y += 1) {
    raw[y * (1 + width * 4)] = 0;
    rgbaTopDown.copy(raw, y * (1 + width * 4) + 1, y * width * 4, (y + 1) * width * 4);
  }
  const chunk = (type, data) => {
    const length = Buffer.alloc(4);
    length.writeUInt32BE(data.length);
    const body = Buffer.concat([Buffer.from(type), data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(body) >>> 0);
    return Buffer.concat([length, body, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8;
  ihdr[9] = 6;
  writeFileSync(path, Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw)), chunk('IEND', Buffer.alloc(0)),
  ]));
}

function sideBySide(path, width, height, page, plugin) {
  const out = Buffer.alloc(width * 3 * height * 4);
  const a = flipRows(page, width, height);
  const b = flipRows(plugin, width, height);
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const i = (y * width + x) * 4;
      const row = y * width * 3 * 4;
      for (let c = 0; c < 4; c += 1) {
        out[row + x * 4 + c] = a[i + c];
        out[row + (width + x) * 4 + c] = b[i + c];
        out[row + (2 * width + x) * 4 + c] = c === 3 ? 255 : Math.min(255, 16 * Math.abs(a[i + c] - b[i + c]));
      }
    }
  }
  png(path, width * 3, height, out);
}

//---------------------------------------------------------------------------
async function main() {
  if (!existsSync(CMTEST)) {
    console.error(`pixels: no cmtest at ${CMTEST} -- cmake --build build`);
    process.exit(2);
  }
  if (!existsSync(CHROME)) {
    console.error(`pixels: no Chrome at ${CHROME} (set CHROME)`);
    process.exit(2);
  }
  if (OUT) mkdirSync(OUT, { recursive: true });

  const server = await serve();
  const url = `http://127.0.0.1:${server.address().port}/tools/crosscheck/harness.html`;
  const chrome = launchChrome();
  let failures = 0;

  try {
    const evaluate = await openPage(chrome, url);
    const pageRuns = [];

    for (const [index, scenario] of SCENARIOS.entries()) {
      const started = await evaluate(`window.begin(${JSON.stringify(scenario)})`);
      if (index === 0) console.log(`page renderer: ${started.renderer}, ANGLE backend asked for: ${ANGLE}\n`);

      const inputs = [];
      const outputs = [];
      const notes = [];
      for (let k = 0; k < scenario.frames; k += 1) {
        const r = await evaluate(`window.frame(${k})`);
        if (r.glError !== 0) throw new Error(`${scenario.name}: GL error 0x${r.glError.toString(16)} at frame ${k}`);
        inputs.push(Buffer.from(r.input, 'base64'));
        outputs.push(Buffer.from(r.output, 'base64'));
        if (k === 0 || k === scenario.frames - 1) notes.push(`f${k}: ${r.passes} passes, blend ${r.frameBlend.toFixed(3)}, decay ${r.decay.toFixed(4)}, first ${r.first}`);
      }
      pageRuns.push({ scenario, values: started.values, inputs, outputs });

      const plugin = await runPlugin(scenario, started.values, scenario.preset, scenario.fps, inputs);
      const stats = compare(outputs, plugin);
      const ok = within(stats);
      if (!ok) failures += 1;
      console.log(`${ok ? 'ok  ' : 'FAIL'} ${scenario.name}`);
      console.log(`     ${describe(stats)}`);
      console.log(`     ${notes.join('; ')}`);

      if (OUT) {
        const slug = scenario.name.replace(/[^a-z0-9]+/gi, '-').toLowerCase();
        const last = scenario.frames - 1;
        sideBySide(join(OUT, `${String(index).padStart(2, '0')}-${slug}.png`), scenario.width, scenario.height, outputs[last], plugin[last]);
      }
    }

    console.log('\nnegative controls (each must FAIL the same tolerance):');
    for (const negative of NEGATIVE) {
      const run = pageRuns[negative.of];
      const values = { ...run.values, ...(negative.plugin ?? {}) };
      const preset = negative.preset ?? run.scenario.preset;
      const fps = negative.fps ?? run.scenario.fps;
      const plugin = await runPlugin(run.scenario, values, preset, fps, run.inputs);
      const stats = compare(run.outputs, plugin);
      const caught = !within(stats);
      if (!caught) failures += 1;
      console.log(`${caught ? 'ok  ' : 'FAIL'} caught: ${negative.name}`);
      console.log(`     ${describe(stats)}`);
    }
  } finally {
    await chrome.close();
    server.close();
  }

  console.log(failures === 0
    ? `\nall ${SCENARIOS.length} scenarios within tolerance and all ${NEGATIVE.length} negative controls caught`
    : `\n${failures} failure(s)`);
  process.exit(failures === 0 ? 0 : 1);
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
