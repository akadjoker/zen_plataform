#!/usr/bin/env node
// run_clip_web.js - the web backend's clipboard and text keys, in a real browser.
//
//   node tests/web/run_clip_web.js <build dir with clipboard_web.html> [--chrome=PATH] [--only=secure|insecure] [--worker] [--keep]
//
// The page is examples/clipboard_web.c built with emcmake (examples/CMakeLists.txt, shell file tests/web/shell.html). It
// is served on two origins, one that is a secure context (http://localhost: navigator.clipboard exists) and one that is not
// (http://insecure.test, a name mapped to 127.0.0.1: plain http, navigator.clipboard is undefined, the case that broke). A
// headless Chrome (nothing is shown; its profile, HOME and XDG folders are in a temporary folder that is removed) is driven
// through the DevTools protocol: real key presses (Input.dispatchKeyEvent, with the editing command a real keyboard of the
// platform would run: paste, copy, cut, selectAll), a synthetic paste event, and a synthetic AltGr key. What reached the
// system clipboard is read back from a secure page. Exit code 0 when everything held, 1 when something did not, 77 when
// there is no Chrome (or Node has no WebSocket).
//
// Nothing here needs a package: Node 22 has WebSocket and fetch. Chrome is started by this script and stopped by its pid.
'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');
const os = require('os');
const cp = require('child_process');

const args = process.argv.slice(2);
const dir = args.find(a => !a.startsWith('--'));
const chromeArg = (args.find(a => a.startsWith('--chrome=')) || '').slice(9);
const keep = args.includes('--keep');
const worker = args.includes('--worker'); // the page is a build with the program on a worker (PROXY_TO_PTHREAD): a copy cannot be answered inside the event
const only = (args.find(a => a.startsWith('--only=')) || '').slice(7); // a build with threads needs SharedArrayBuffer: a secure context only
if (!dir || !fs.existsSync(path.join(dir, 'clipboard_web.html'))) {
  console.error('usage: node run_clip_web.js <dir with clipboard_web.html> [--chrome=PATH]');
  process.exit(2);
}
if (typeof WebSocket === 'undefined') { console.log('SKIP: this Node has no WebSocket'); process.exit(77); }
const chrome = chromeArg || ['google-chrome', 'chromium', 'chromium-browser'].map(n => {
  try { return cp.execFileSync('sh', ['-c', 'command -v ' + n]).toString().trim(); } catch (e) { return ''; }
}).find(Boolean);
if (!chrome) { console.log('SKIP: no Chrome or Chromium'); process.exit(77); }

let pass = 0, fail = 0;
const check = (name, ok, detail) => {
  if (ok) { pass++; console.log('  ok    ' + name); }
  else { fail++; console.log('  FAIL  ' + name + (detail !== undefined ? '   ' + JSON.stringify(detail) : '')); }
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

// ---- the static server -------------------------------------------------------------------------------------------
const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm' };
const server = http.createServer((req, res) => {
  const name = decodeURIComponent(req.url.split('?')[0]).replace(/^\/+/, '') || 'clipboard_web.html';
  const file = path.join(dir, path.basename(name));
  if (name === 'blank.html') { res.writeHead(200, { 'content-type': 'text/html' }); res.end('<!doctype html><title>blank</title><body>blank'); return; }
  fs.readFile(file, (err, data) => {
    if (err) { res.writeHead(404); res.end(); return; }
    // cross-origin isolation, for a build with threads (SharedArrayBuffer)
    res.writeHead(200, { 'content-type': types[path.extname(file)] || 'application/octet-stream',
      'cross-origin-opener-policy': 'same-origin', 'cross-origin-embedder-policy': 'require-corp' });
    res.end(data);
  });
});

// ---- Chrome and the DevTools protocol ----------------------------------------------------------------------------
let tmp, chromeProc, ws, nextId = 1;
const pending = new Map();
const send = (method, params = {}, sessionId) => new Promise((resolve, reject) => {
  const id = nextId++;
  pending.set(id, { resolve, reject, method });
  ws.send(JSON.stringify(Object.assign({ id, method, params }, sessionId ? { sessionId } : {})));
});

async function startChrome(port) {
  tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'zp-web-'));
  for (const d of ['profile', 'home', 'cfg', 'cache', 'data']) fs.mkdirSync(path.join(tmp, d));
  const env = Object.assign({}, process.env, { HOME: path.join(tmp, 'home'), XDG_CONFIG_HOME: path.join(tmp, 'cfg'),
    XDG_CACHE_HOME: path.join(tmp, 'cache'), XDG_DATA_HOME: path.join(tmp, 'data') });
  delete env.DISPLAY; delete env.WAYLAND_DISPLAY;
  chromeProc = cp.spawn(chrome, ['--headless=new', '--remote-debugging-port=0', '--user-data-dir=' + path.join(tmp, 'profile'),
    '--no-first-run', '--no-default-browser-check', '--disable-gpu', '--disable-dev-shm-usage', '--disable-extensions',
    '--host-resolver-rules=MAP insecure.test 127.0.0.1', 'about:blank'], { env, stdio: 'ignore' });
  const portFile = path.join(tmp, 'profile', 'DevToolsActivePort');
  for (let i = 0; i < 200 && !fs.existsSync(portFile); i++) await sleep(100);
  const [p, wsPath] = fs.readFileSync(portFile, 'utf8').split('\n');
  ws = new WebSocket('ws://127.0.0.1:' + p + wsPath);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  ws.onmessage = ev => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) {
      const { resolve, reject, method } = pending.get(m.id);
      pending.delete(m.id);
      if (m.error) reject(new Error(method + ': ' + JSON.stringify(m.error))); else resolve(m.result);
    }
  };
}

async function newPage(url) {
  const { targetId } = await send('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await send('Target.attachToTarget', { targetId, flatten: true });
  await send('Page.enable', {}, sessionId);
  await send('Runtime.enable', {}, sessionId);
  await send('Emulation.setFocusEmulationEnabled', { enabled: true }, sessionId); // a page with no window still "has the focus"
  await send('Page.bringToFront', {}, sessionId);
  await send('Page.navigate', { url }, sessionId);
  return sessionId;
}

async function ev(s, expression, opts = {}) {
  const r = await send('Runtime.evaluate', Object.assign({ expression, returnByValue: true, awaitPromise: !!opts.await,
    userGesture: !!opts.gesture }, {}), s);
  if (r.exceptionDetails) throw new Error('evaluate: ' + JSON.stringify(r.exceptionDetails.exception || r.exceptionDetails));
  return r.result.value;
}

const state = s => ev(s, 'JSON.parse(JSON.stringify(window.zpState || null))');
async function waitFor(s, cond, what, ms = 8000) {
  const end = Date.now() + ms;
  for (;;) {
    const st = await state(s);
    if (st && cond(st)) return st;
    if (Date.now() > end) {
      console.log('  (timeout waiting for ' + what + ')');
      try { console.log('  page output: ' + JSON.stringify(await ev(s, 'JSON.stringify({out: window.zpPrint, err: window.zpPrintErr, isolated: window.crossOriginIsolated})'))); } catch (e) {}
      return st;
    }
    await sleep(25);
  }
}
const frames = async s => ((await state(s)) || {}).frames || 0;
async function settle(s, n = 3) { // n more frames than now
  const f0 = await frames(s);
  await waitFor(s, st => st.frames >= f0 + n, n + ' frames');
  return state(s);
}

const KEYS = {
  v: { key: 'v', code: 'KeyV', vk: 86 }, c: { key: 'c', code: 'KeyC', vk: 67 }, x: { key: 'x', code: 'KeyX', vk: 88 },
  a: { key: 'a', code: 'KeyA', vk: 65, text: 'a' }, A: { key: 'A', code: 'KeyA', vk: 65, text: 'A' },
  one: { key: '1', code: 'Digit1', vk: 49, text: '1' }, space: { key: ' ', code: 'Space', vk: 32, text: ' ' },
  insert: { key: 'Insert', code: 'Insert', vk: 45 }, del: { key: 'Delete', code: 'Delete', vk: 46 },
  f2: { key: 'F2', code: 'F2', vk: 113 }, backspace: { key: 'Backspace', code: 'Backspace', vk: 8 },
};
const SHIFT = 8, CTRL = 2, ALT = 1, META = 4;
async function press(s, k, modifiers = 0, commands) {
  const base = { key: k.key, code: k.code, windowsVirtualKeyCode: k.vk, nativeVirtualKeyCode: k.vk, modifiers };
  await send('Input.dispatchKeyEvent', Object.assign({ type: k.text && !modifiers ? 'keyDown' : 'rawKeyDown' }, base,
    k.text && !modifiers ? { text: k.text, unmodifiedText: k.text } : {}, commands ? { commands } : {}), s);
  await send('Input.dispatchKeyEvent', Object.assign({ type: 'keyUp' }, base), s);
}
const clean = s => ev(s, 'window.zpState.log.length = 0; 1');
const afterCopy = async s => { await settle(s); if (worker) await sleep(400); }; // a worker's answer goes out through the async API

async function main() {
  await new Promise(r => server.listen(0, '127.0.0.1', r));
  const port = server.address().port;
  await startChrome();
  const secureOrigin = 'http://localhost:' + port;
  await send('Browser.grantPermissions', { permissions: ['clipboardReadWrite', 'clipboardSanitizedWrite'], origin: secureOrigin });
  const helper = await newPage(secureOrigin + '/blank.html'); // reads and writes the system clipboard
  await sleep(300);
  const setClip = t => ev(helper, 'navigator.clipboard.writeText(' + JSON.stringify(t) + ').then(() => 1)', { await: true, gesture: true });
  const getClip = () => ev(helper, 'navigator.clipboard.readText()', { await: true, gesture: true });

  const origins = [{ name: 'plain http (insecure.test)', url: 'http://insecure.test:' + port + '/clipboard_web.html', secure: false },
                   { name: 'secure origin (localhost)', url: secureOrigin + '/clipboard_web.html', secure: true }];
  for (const o of origins) {
    if ((only === 'secure' && !o.secure) || (only === 'insecure' && o.secure)) continue;
    console.log('\n== ' + o.name);
    const s = await newPage(o.url);
    await waitFor(s, st => st.frames > 3, 'the program to run', 15000);
    check('isSecureContext is ' + o.secure, (await ev(s, 'window.isSecureContext')) === o.secure);
    check('navigator.clipboard is ' + (o.secure ? 'there' : 'undefined (the plain http case)'),
      (await ev(s, 'typeof navigator.clipboard')) === (o.secure ? 'object' : 'undefined'));
    const log = async () => (await state(s)).log;

    // -- text: a key types, a shortcut does not (point 2)
    await clean(s);
    await press(s, KEYS.a);
    await press(s, KEYS.one);
    await press(s, KEYS.A, SHIFT);
    await settle(s);
    let st = await state(s);
    check('typed keys give text (a, 1, Shift+a)', st.doc === 'a1A' && st.log.includes('C:97') && st.log.includes('C:65'), st);
    await clean(s);
    await press(s, KEYS.a, CTRL, ['selectAll']);
    await press(s, KEYS.one, CTRL);
    await press(s, KEYS.space, CTRL);
    await press(s, KEYS.a, META);
    st = await settle(s);
    check('Ctrl+A, Ctrl+1, Ctrl+space, Meta+A type nothing', !st.log.some(l => l.startsWith('C:')) && st.doc === 'a1A', st.log);
    check('Ctrl+A is the key, with its modifier, and selects', st.log.includes('K:A:ctrl+:down') && st.selected === true, st.log);
    // AltGr, as Windows layouts report it: Ctrl and Alt together, and the browser says AltGraph
    await clean(s);
    await ev(s, "document.dispatchEvent(new KeyboardEvent('keydown', {key: '@', code: 'KeyQ', ctrlKey: true, altKey: true, modifierAltGraph: true, bubbles: true, cancelable: true})); 1");
    await ev(s, "document.dispatchEvent(new KeyboardEvent('keyup', {key: '@', code: 'KeyQ', ctrlKey: true, altKey: true, modifierAltGraph: true, bubbles: true, cancelable: true})); 1");
    st = await settle(s);
    check('AltGr+q (Ctrl+Alt with AltGraph) types "@"', st.log.includes('C:64'), st.log);
    await clean(s);
    await ev(s, "document.dispatchEvent(new KeyboardEvent('keydown', {key: 'e', code: 'KeyE', ctrlKey: true, altKey: true, bubbles: true, cancelable: true})); 1");
    st = await settle(s);
    check('Ctrl+Alt+e without AltGraph is a shortcut: no text', !st.log.some(l => l.startsWith('C:')), st.log);

    // -- paste: a real key press and the browser's own clipboard (point 1)
    await ev(s, "window.zpState.log.length = 0; 1");
    await press(s, KEYS.backspace); await press(s, KEYS.backspace); await press(s, KEYS.backspace); await settle(s);
    await setClip('pasted ção ✓ 1');
    await clean(s);
    await press(s, KEYS.v, CTRL, ['paste']);
    st = await settle(s);
    check('Ctrl+V pastes the browser clipboard (the key was not prevented, the paste event fired)', st.doc === 'pasted ção ✓ 1', st);
    const order = st.log.filter(l => l !== 'K:V:ctrl+:up');
    check('the application saw the key, then paste, no "v" typed', order[0] === 'K:V:ctrl+:down' && order[1] === 'paste' && !st.log.some(l => l.startsWith('C:')), st.log);
    await ev(s, 'window.zpState.log.length = 0; 1');
    await setClip('shift insert');
    await press(s, KEYS.a, CTRL, ['selectAll']);
    await press(s, KEYS.insert, SHIFT, ['paste']);
    st = await settle(s);
    check('Shift+Insert pastes', st.doc === 'shift insert', st);
    await setClip('meta v');
    await press(s, KEYS.a, CTRL, ['selectAll']);
    await press(s, KEYS.v, META, ['paste']);
    st = await settle(s);
    check('Meta+V pastes', st.doc === 'meta v', st);
    // a synthetic paste event (the browser's menu, a touch keyboard): no key came, Ctrl+V is made for the application
    await clean(s);
    await ev(s, "var dt = new DataTransfer(); dt.setData('text/plain', 'from a synthetic event'); document.dispatchEvent(new ClipboardEvent('paste', {clipboardData: dt, bubbles: true, cancelable: true})); 1");
    st = await settle(s);
    check('a paste event with no key pastes, with a Ctrl+V made for the program', st.doc === 'meta vfrom a synthetic event' &&
      st.log.includes('K:V:ctrl+:down') && st.log.includes('paste'), st);
    check('clipboard_get in the program is what the event brought', st.clip === 'from a synthetic event', st.clip);
    // a paste with no text (an image, a file): the clipboard of the program is left alone, the key still arrives
    await clean(s);
    await ev(s, "var dt2 = new DataTransfer(); document.dispatchEvent(new ClipboardEvent('paste', {clipboardData: dt2, bubbles: true, cancelable: true})); 1");
    st = await settle(s);
    check('a paste with no text keeps the clipboard of the program', st.clip === 'from a synthetic event' && st.log.includes('paste'), st);
    // a field of the page keeps its own paste
    await ev(s, "var inp = document.createElement('input'); inp.id = 'field'; document.body.appendChild(inp); 1");
    await clean(s);
    await ev(s, "var dt3 = new DataTransfer(); dt3.setData('text/plain', 'for the field'); document.getElementById('field').dispatchEvent(new ClipboardEvent('paste', {clipboardData: dt3, bubbles: true, cancelable: true})); 1");
    st = await settle(s);
    check('a paste in an <input> of the page is not taken', st.clip === 'from a synthetic event' && !st.log.includes('paste'), st);
    await ev(s, "document.getElementById('field').remove(); 1");

    // -- copy and cut: the application answers inside the browser's event, so the text is the one just selected (point 1)
    await setClip('old clipboard text');
    await press(s, KEYS.a, CTRL, ['selectAll']);
    await press(s, KEYS.backspace); await settle(s);
    await press(s, KEYS.a); await press(s, KEYS.one); await press(s, KEYS.space); await press(s, KEYS.a);
    await press(s, KEYS.a, CTRL, ['selectAll']);
    st = await settle(s);
    const doc1 = st.doc;
    await clean(s);
    await press(s, KEYS.c, CTRL, ['copy']);
    await afterCopy(s);
    let got = await getClip();
    check('Ctrl+C puts the text the program just copied on the system clipboard (first time, not the previous one)', got === doc1 && got !== 'old clipboard text', [got, await ev(s, 'String(Module.pfClipError)'), await ev(s, 'JSON.stringify(Module.pfClipLast)'), doc1]);
    st = await state(s);
    check('the program saw the key and its copy', st.log.includes('K:C:ctrl+:down') && st.log.includes('copy'), st.log);
    await setClip('other program text');
    await clean(s);
    await press(s, KEYS.backspace); // nothing selected now
    await press(s, KEYS.c, CTRL, ['copy']);
    await afterCopy(s);
    got = await getClip();
    check('Ctrl+C with nothing selected leaves the clipboard as it was', got === 'other program text', got);
    await press(s, KEYS.a, CTRL, ['selectAll']);
    await press(s, KEYS.insert, CTRL, ['copy']);
    await afterCopy(s);
    st = await state(s);
    got = await getClip();
    check('Ctrl+Insert copies', got === st.doc && st.doc.length > 0, got);
    await setClip('before cut');
    await press(s, KEYS.a, CTRL, ['selectAll']);
    const before = (await state(s)).doc;
    await press(s, KEYS.x, CTRL, ['cut']);
    await afterCopy(s);
    st = await state(s);
    got = await getClip();
    check('Ctrl+X cuts: the clipboard has the text, the program removed it', got === before && st.doc === '', { got, before, doc: st.doc });
    await press(s, KEYS.a); await press(s, KEYS.one);
    await setClip('before shift delete');
    await press(s, KEYS.a, CTRL, ['selectAll']);
    await press(s, KEYS.del, SHIFT, ['cut']);
    await afterCopy(s);
    st = await state(s);
    got = await getClip();
    check('Shift+Delete cuts', got === 'a1' && st.doc === '', { got, doc: st.doc });
    // a copy from the browser's menu: no key, the program is asked as if Ctrl+C was pressed
    await press(s, KEYS.a); await press(s, KEYS.one);
    await press(s, KEYS.a, CTRL, ['selectAll']);
    await setClip('before menu copy');
    await clean(s);
    await ev(s, "var cdt = new DataTransfer(); var ce = new ClipboardEvent('copy', {clipboardData: cdt, bubbles: true, cancelable: true}); document.dispatchEvent(ce); window.zpMenuCopy = [ce.defaultPrevented, cdt.getData('text/plain')]; 1");
    st = await settle(s);
    const menu = await ev(s, 'window.zpMenuCopy');
    if (!worker)
      check('a copy event with no key: the program is asked (Ctrl+C made for it) and its text goes into the event', menu && menu[0] === true && menu[1] === 'a1' && st.log.includes('K:C:ctrl+:down'), menu);
    else
      check('a copy event with no key: the program is asked (Ctrl+C made for it); its answer comes a frame later, outside the event', st.log.includes('K:C:ctrl+:down'), menu);

    // -- a copy outside of a copy event (a "Copy" button of the page's own GUI): F2 here (the point-1 fallback when
    //    navigator.clipboard is missing: execCommand from a hidden textarea)
    await setClip('before F2');
    await ev(s, "window.zpExec = []; var oe = document.execCommand.bind(document); document.execCommand = function(c) { var r = oe.apply(null, arguments); window.zpExec.push([c, r, document.activeElement && document.activeElement.tagName]); return r; }; 1");
    await clean(s);
    await press(s, KEYS.f2);
    await settle(s, 4);
    await sleep(300);
    got = await getClip();
    const exec = await ev(s, 'window.zpExec');
    st = await state(s);
    check('a clipboard_set outside of a copy event reaches the system clipboard', got === st.doc && st.doc === 'a1', { got, doc: st.doc });
    if (!o.secure)
      check('without navigator.clipboard the hidden textarea and execCommand("copy") did it', exec.length >= 1 && exec[0][0] === 'copy' && exec[0][1] === true && exec[0][2] === 'TEXTAREA', exec);
    else
      check('with navigator.clipboard the async API did it (no execCommand)', exec.length === 0, exec);
    check('the window keeps the focus state through the fallback (no blur or focus seen by the program)', !st.log.some(l => /focus|blur/i.test(l)), st.log);
  }
}

main().then(() => {
  console.log('\n' + pass + ' passed, ' + fail + ' failed');
}).catch(e => {
  console.log('ERROR ' + (e && e.stack || e));
  fail++;
}).finally(() => {
  try { ws && ws.close(); } catch (e) {}
  if (chromeProc) chromeProc.kill();
  server.close();
  setTimeout(() => {
    if (tmp && !keep) fs.rmSync(tmp, { recursive: true, force: true });
    process.exit(fail ? 1 : 0);
  }, 500);
});
