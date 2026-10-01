#!/usr/bin/env node
// Renders scene files with a real browser canvas and writes 32-bit BMPs.
//   node tools/ref_render.js scene.txt out.bmp [scene2.txt out2.bmp ...]
// The scene format is documented in tools/scene.h. Every command is forwarded to
// the canvas context, so new scene commands need no change here.
'use strict';
const fs = require('fs');

function loadPlaywright() {
  const candidates = [process.env.PLAYWRIGHT_MODULE, 'playwright', 'playwright-core', '/opt/node-tools/node_modules/playwright', '/opt/node-tools/node_modules/playwright-core'];
  for (const name of candidates) {
    if (!name) continue;
    try { return require(name); } catch (e) { /* try the next one */ }
  }
  console.error('playwright is not installed (set PLAYWRIGHT_MODULE to its path)');
  process.exit(2);
}

function writeBmp(path, width, height, rgba) {
  const rowBytes = width * 4;
  const header = Buffer.alloc(54);
  header.write('BM', 0);
  header.writeUInt32LE(54 + rowBytes * height, 2);
  header.writeUInt32LE(54, 10);
  header.writeUInt32LE(40, 14);
  header.writeInt32LE(width, 18);
  header.writeInt32LE(height, 22);
  header.writeUInt16LE(1, 26);
  header.writeUInt16LE(32, 28);
  header.writeUInt32LE(0, 30);
  header.writeUInt32LE(rowBytes * height, 34);
  const body = Buffer.alloc(rowBytes * height);
  for (let y = 0; y < height; y++) {
    const src = (height - 1 - y) * rowBytes;
    const dst = y * rowBytes;
    for (let x = 0; x < width; x++) {
      body[dst + x * 4 + 0] = rgba[src + x * 4 + 2];
      body[dst + x * 4 + 1] = rgba[src + x * 4 + 1];
      body[dst + x * 4 + 2] = rgba[src + x * 4 + 0];
      body[dst + x * 4 + 3] = rgba[src + x * 4 + 3];
    }
  }
  fs.writeFileSync(path, Buffer.concat([header, body]));
}

// Runs inside the page. Returns { width, height, data } with data as base64 RGBA.
function renderInPage(text) {
  const properties = new Set(['fillStyle', 'strokeStyle', 'lineWidth', 'lineCap', 'lineJoin', 'miterLimit',
    'globalAlpha', 'globalCompositeOperation', 'lineDashOffset', 'imageSmoothingEnabled', 'shadowColor', 'shadowBlur', 'shadowOffsetX', 'shadowOffsetY', 'textAlign', 'textBaseline']);
  let canvas = null, ctx = null;
  let gradient = null, patternImage = null, pattern = null;
  const makePatternImage = (w, h) => {
    const c = document.createElement('canvas');
    c.width = w; c.height = h;
    const g = c.getContext('2d');
    const img = g.createImageData(w, h);
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const i = (y * w + x) * 4;
        img.data[i] = w > 1 ? Math.floor(x * 255 / (w - 1)) : 0;
        img.data[i + 1] = h > 1 ? Math.floor(y * 255 / (h - 1)) : 0;
        img.data[i + 2] = (((x >> 2) + (y >> 2)) & 1) ? 255 : 0;
        img.data[i + 3] = (x < Math.floor(w / 2) && y < Math.floor(h / 2)) ? 128 : 255;
      }
    }
    g.putImageData(img, 0, 0);
    return c;
  };
  const lines = text.split('\n');
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i].trim();
    if (!line || line[0] === '#') continue;
    const tokens = line.split(/\s+/);
    const cmd = tokens[0];
    const args = tokens.slice(1);
    const where = 'line ' + (i + 1) + ': ';
    if (cmd === 'size') {
      if (canvas) throw new Error(where + 'size may appear only once');
      canvas = document.createElement('canvas');
      canvas.width = parseInt(args[0], 10);
      canvas.height = parseInt(args[1], 10);
      ctx = canvas.getContext('2d', { willReadFrequently: true, colorSpace: 'srgb' });
      continue;
    }
    if (!ctx) throw new Error(where + "'" + cmd + "' before size");
    const nums = args.map(Number);
    if (cmd === 'gradientLinear') { gradient = ctx.createLinearGradient(...nums); continue; }
    if (cmd === 'gradientRadial') { gradient = ctx.createRadialGradient(...nums); continue; }
    if (cmd === 'gradientStop') { gradient.addColorStop(Number(args[0]), args[1]); continue; }
    if (cmd === 'fillGradient') { ctx.fillStyle = gradient; continue; }
    if (cmd === 'strokeGradient') { ctx.strokeStyle = gradient; continue; }
    if (cmd === 'patternImage') { patternImage = makePatternImage(nums[0], nums[1]); continue; }
    if (cmd === 'fillPattern') { pattern = ctx.createPattern(patternImage, args[0]); ctx.fillStyle = pattern; continue; }
    if (cmd === 'patternTransform') { pattern.setTransform(new DOMMatrix(nums)); continue; }
    if (cmd === 'setLineDash') { ctx.setLineDash(nums); continue; }
    if (cmd === 'drawImage') { ctx.drawImage(patternImage, ...nums); continue; }
    if (cmd === 'font') { ctx.font = nums[0] + 'px ZvTestFont'; continue; }
    if (cmd === 'fillText' || cmd === 'strokeText') { ctx[cmd](args.slice(2).join(' '), nums[0], nums[1]); continue; }
    if (cmd === 'arc' || cmd === 'ellipse') { ctx[cmd](...args.map((a) => (a === 'true' ? true : a === 'false' ? false : Number(a)))); continue; }
    if (properties.has(cmd)) {
      const value = args.join(' ');
      ctx[cmd] = /^-?[0-9.]+$/.test(value) ? Number(value) : (value === 'true' ? true : value === 'false' ? false : value);
      continue;
    }
    if (typeof ctx[cmd] !== 'function') throw new Error(where + "unknown command '" + cmd + "'");
    ctx[cmd](...args.map((a) => (/^-?[0-9.]+(e-?[0-9]+)?$/.test(a) ? Number(a) : a)));
  }
  if (!canvas) throw new Error('the scene has no size');
  const data = ctx.getImageData(0, 0, canvas.width, canvas.height).data;
  let binary = '';
  for (let i = 0; i < data.length; i += 8192) binary += String.fromCharCode.apply(null, data.subarray(i, i + 8192));
  return { width: canvas.width, height: canvas.height, data: btoa(binary) };
}

(async () => {
  const args = process.argv.slice(2);
  if (args.length === 0 || args.length % 2 !== 0) {
    console.error('usage: ref_render.js scene.txt out.bmp [scene2.txt out2.bmp ...]');
    process.exit(2);
  }
  const playwright = loadPlaywright();
  const browser = await playwright.chromium.launch({ args: ['--disable-gpu', '--force-color-profile=srgb'] });
  let status = 0;
  try {
    const page = await browser.newPage({ deviceScaleFactor: 1 });
    const fontPath = process.env.ZV_SCENE_FONT || '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf';
    const fontData = fs.readFileSync(fontPath).toString('base64');
    await page.setContent('<html><body></body></html>');
    await page.evaluate(async (data) => {
      const face = new FontFace('ZvTestFont', 'url(data:font/ttf;base64,' + data + ')');
      await face.load();
      document.fonts.add(face);
    }, fontData);
    for (let i = 0; i < args.length; i += 2) {
      try {
        const result = await page.evaluate(renderInPage, fs.readFileSync(args[i], 'utf8'));
        writeBmp(args[i + 1], result.width, result.height, Buffer.from(result.data, 'base64'));
      } catch (e) {
        console.error(args[i] + ': ' + e.message.split('\n')[0]);
        status = 1;
      }
    }
  } finally {
    await browser.close();
  }
  process.exit(status);
})();
