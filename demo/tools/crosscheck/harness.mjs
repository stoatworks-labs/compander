// The page's side of pixels.mjs: this page's renderer (demo/plugin.js's
// createRenderer, the same object the demo page draws with) driven frame by
// frame on a fixed clock, with the input it was handed and the output it drew
// both read back as RGBA8, bottom row first -- GL's order, and cmtest's.
//
// Nothing here is a second copy of anything: the renderer, the parameter model,
// the preset apply and the generated clips are imported from the page and its
// vendored kit. What this adds is a host with a fixed clock (time = frame / fps,
// as cmtest --pipe hands the plugin SetTime) and an RGBA8 target of its own in
// place of the canvas, so a frame can be read back after it is drawn.

import { PARAMS, createRenderer, applyPreset } from '../../plugin.js';
import { createContext, Quad, PassBuffer } from '../../vendor/gl.js';
import { Params } from '../../vendor/params.js';
import { SOURCES, SourceRenderer } from '../../vendor/sources.js';

let run = null;

/**
 * A synthetic input, for the one thing no generated clip does: let the
 * picture's peak FALL. Every kit clip keeps a bright object in frame, so the
 * frame follower only ever attacks, and its release -- the state carried from
 * frame to frame, stepped by the clock -- never shows. This is a grey field with
 * a box that is `high` until frame `stepAt` and `low` after, with a few
 * vertical bars in it so the emphasis networks have edges to work on.
 */
function stepFrame(spec, k) {
  const { width, height } = spec;
  const level = k < spec.stepAt ? spec.high : spec.low;
  const pixels = new Uint8Array(width * height * 4);
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const inBox = x > width * 0.3 && x < width * 0.7 && y > height * 0.3 && y < height * 0.7;
      const bar = Math.floor(x / 24) % 2 === 0 ? 1.0 : 0.8;
      const v = inBox ? level * bar : 0.06 + 0.1 * (x / width);
      const i = (y * width + x) * 4;
      pixels[i] = Math.round(255 * v);
      pixels[i + 1] = Math.round(255 * v * 0.95);
      pixels[i + 2] = Math.round(255 * v * 0.9);
      pixels[i + 3] = 255;
    }
  }
  return pixels;
}

function toBase64(bytes) {
  let binary = '';
  const chunk = 0x8000;
  for (let i = 0; i < bytes.length; i += chunk) {
    binary += String.fromCharCode.apply(null, bytes.subarray(i, i + chunk));
  }
  return btoa(binary);
}

function readBack(gl, fbo, width, height) {
  const pixels = new Uint8Array(width * height * 4);
  gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
  gl.pixelStorei(gl.PACK_ALIGNMENT, 1);
  gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  return pixels;
}

/** Start a scenario: a fresh instance, as a new layer in a host would be. */
window.begin = (spec) => {
  if (run) {
    if (run.synthetic) run.gl.deleteTexture(run.synthetic);
    run.renderer.dispose();
    run.sources.dispose();
    run.target.dispose();
  }

  const canvas = document.createElement('canvas');
  canvas.width = spec.width;
  canvas.height = spec.height;
  const gl = createContext(canvas, { needFloatRender: true });
  const quad = new Quad(gl);

  const params = new Params(PARAMS);
  for (const [id, value] of Object.entries(spec.values ?? {})) params.set(id, value, { silent: true });
  if (spec.preset) {
    params.set('preset', spec.preset, { silent: true });
    applyPreset(params, spec.preset);
  }

  run = {
    spec,
    gl,
    quad,
    params,
    sources: new SourceRenderer(gl, quad),
    renderer: createRenderer(gl, quad),
    target: new PassBuffer(gl, { filter: 'nearest' }).ensure(spec.width, spec.height, gl.RGBA8),
    source: SOURCES.find((s) => s.id === spec.clip),
    synthetic: null,
  };
  if (spec.synthetic === 'step') {
    run.synthetic = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, run.synthetic);
    gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, spec.width, spec.height);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.bindTexture(gl.TEXTURE_2D, null);
  } else if (!run.source) {
    throw new Error(`no clip ${spec.clip}`);
  }
  return { renderer: gl.getParameter(gl.RENDERER), values: params.values };
};

/** Draw frame k. Returns the input and the output, base64 RGBA8, bottom row first. */
window.frame = (k) => {
  const { gl, spec, params, sources, renderer, target, source } = run;
  const { width, height } = spec;
  const time = k / spec.fps;

  let input;
  let inputPixels;
  if (run.synthetic) {
    inputPixels = stepFrame(spec, k);
    gl.bindTexture(gl.TEXTURE_2D, run.synthetic);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, inputPixels);
    gl.bindTexture(gl.TEXTURE_2D, null);
    input = { texture: run.synthetic, width, height };
  } else {
    input = sources.render(source, width, height, time);
    inputPixels = readBack(gl, sources.buffer.fbo, width, height);
  }

  target.bind();
  gl.clearColor(0, 0, 0, 0);
  gl.clear(gl.COLOR_BUFFER_BIT);
  gl.disable(gl.BLEND);
  const f = renderer.render({ input, params, width, height, time });
  const outputPixels = readBack(gl, target.fbo, width, height);

  const error = gl.getError();
  return {
    input: toBase64(inputPixels),
    output: toBase64(outputPixels),
    glError: error,
    passes: f.plan.passes.length,
    frameBlend: f.plan.frameBlend,
    decay: f.follower.decay,
    first: f.follower.first,
  };
};

window.harnessReady = true;
