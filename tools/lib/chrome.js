// Minimal headless-Chromium driver over the DevTools protocol (Node >= 22: global WebSocket/fetch, no npm deps).
//   const page = await openPage(url, { width, height, initScript, css });
//   await page.eval('1+1'); const png = await page.screenshot(); await page.close();
// initScript runs before any page script (e.g. to freeze Date); css is injected once the page has loaded.
'use strict';
const { spawn, spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function findChrome() {
  for (const c of [process.env.CHROME, 'chromium', 'chromium-browser', 'google-chrome']) {
    if (c && spawnSync(c, ['--version']).status === 0) return c;
  }
  return null;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function openPage(url, opts = {}) {
  const bin = findChrome();
  if (!bin) throw new Error('no chromium found (set $CHROME)');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'binary-watch-chrome-'));
  const proc = spawn(bin, ['--headless=new', '--disable-gpu', '--no-sandbox', '--no-first-run', '--hide-scrollbars',
    '--font-render-hinting=none', '--force-color-profile=srgb', '--user-data-dir=' + dir,
    '--remote-debugging-port=0', 'about:blank'],
  { stdio: 'ignore', detached: true, env: Object.assign({}, process.env, { TZ: 'UTC', LANG: 'en_US.UTF-8' }) });
  const portFile = path.join(dir, 'DevToolsActivePort');
  for (let i = 0; i < 100 && !fs.existsSync(portFile); i++) await sleep(100);
  const port = fs.readFileSync(portFile, 'utf8').split('\n')[0];
  let tab;
  for (let i = 0; i < 50 && !tab; i++) {
    try { tab = (await (await fetch(`http://127.0.0.1:${port}/json`)).json()).find((t) => t.type === 'page'); } catch (e) {}
    if (!tab) await sleep(100);
  }
  const ws = new WebSocket(tab.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  let id = 0;
  const waiters = {}, listeners = [], errors = [];
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && waiters[d.id]) { waiters[d.id](d); delete waiters[d.id]; return; }
    if (d.method === 'Runtime.exceptionThrown') {
      errors.push(d.params.exceptionDetails.exception?.description || d.params.exceptionDetails.text);
    }
    listeners.forEach((l) => l(d));
  };
  const send = (method, params = {}) => new Promise((r) => {
    const i = ++id; waiters[i] = r; ws.send(JSON.stringify({ id: i, method, params }));
  });
  await send('Runtime.enable');
  await send('Page.enable');
  await send('Emulation.setDeviceMetricsOverride', {
    width: opts.width || 400, height: opts.height || 800, deviceScaleFactor: 1, mobile: true });
  await send('Emulation.setTimezoneOverride', { timezoneId: 'UTC' });
  await send('Emulation.setLocaleOverride', { locale: 'en-US' });
  if (opts.initScript) await send('Page.addScriptToEvaluateOnNewDocument', { source: opts.initScript });
  const loaded = new Promise((r) => listeners.push((d) => d.method === 'Page.loadEventFired' && r()));
  await send('Page.navigate', { url });
  await loaded;

  const page = {
    errors,
    async eval(expr) {
      const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
      if (r.result.exceptionDetails) {
        throw new Error('page eval failed: ' + (r.result.exceptionDetails.exception?.description || r.result.exceptionDetails.text));
      }
      return r.result.result.value;
    },
    // Waits for web fonts and two animation frames so the page has settled.
    async settle() {
      await page.eval('document.fonts.ready.then(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))))');
    },
    // Full-page PNG (Buffer) at the emulated width.
    async screenshot() {
      const m = await send('Page.getLayoutMetrics');
      const size = m.result.cssContentSize || m.result.contentSize;
      const r = await send('Page.captureScreenshot', {
        format: 'png', captureBeyondViewport: true,
        clip: { x: 0, y: 0, width: Math.ceil(size.width), height: Math.ceil(size.height), scale: 1 } });
      return Buffer.from(r.result.data, 'base64');
    },
    async close() {
      try { ws.close(); } catch (e) {}
      const exited = proc.exitCode !== null ? Promise.resolve() : new Promise((r) => proc.once('exit', r));
      try { process.kill(-proc.pid); } catch (e) {}
      await Promise.race([exited, sleep(3000)]);
      fs.rmSync(dir, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 });
    },
  };
  if (opts.css) {
    await page.eval(`(() => { const s = document.createElement('style'); s.textContent = ${JSON.stringify(opts.css)};
      document.head.appendChild(s); })()`);
  }
  return page;
}

module.exports = { openPage, findChrome };
