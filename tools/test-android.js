// Loads the bundled configuration page, as the same data: URI the phone-side code builds, into the
// Chrome that runs inside an Android emulator (driven over the DevTools protocol), changes settings,
// presses Save and checks the JSON handed back.
//
// usage (emulator already running, ANDROID_ADB_SERVER_PORT / serial set below): node tools/test-android.js
const http = require('http');
const { execFileSync } = require('child_process');
const page = require('../src/pkjs/config-page');

const SERIAL = process.env.ANDROID_SERIAL || 'emulator-5560';
const HOST_PORT = 18766;                 // the emulator reaches the host as 10.0.2.2
const CDP_PORT = 9333;
const adb = (...a) => execFileSync('adb', ['-s', SERIAL, ...a], { encoding: 'utf8' });
const sleep = ms => new Promise(r => setTimeout(r, ms));

const current_config = { storage_version: 4, truebinary: 0, displayfalse: 1, showimpossible: 0,
  backgroundcolor: '000000', bitcolor: '00aaff', offcolor: '555555', hintcolor: '00aaff', displayseconds: 1,
  timealignment: 0, timeformat: 0, showhint: 0, datebitcolor: '00ff55', dateoffcolor: '555555',
  datehintcolor: '00ff55', showdate: 0, dateposition: 0, datealignment: 0, dateformat: 0, dateshowhint: 0,
  btloss: 1, btlossvibration: 3, btreconnectvibration: 3, btlevel: 1, btlevelvibration: 0, vibratehourly: 0,
  dndwhilecharging: 0, tappingtimeout: 5000, watchtimeformat: 1, timedeco: 0, datedeco: 0, trueborder: 0,
  falseborder: 0, secint: 1, quickview: 0 };
const params = { current_version: '2.4', current_language: 'en-US',
  current_config: JSON.stringify(current_config), current_watch: JSON.stringify({ platform: 'basalt' }),
  return_to: `http://10.0.2.2:${HOST_PORT}/close?` };
const injected = '<script>window.__bcdParams = ' + JSON.stringify(params).replace(/</g, '\\u003c') + ';</script>';
const dataUri = 'data:text/html;charset=utf-8,' + encodeURIComponent(page.replace('<!--BCD-PARAMS-->', injected));

(async () => {
  const fails = [];
  let returned = null;
  const srv = http.createServer((req, res) => { if (req.url.startsWith('/close')) returned = req.url; res.end('ok'); }).listen(HOST_PORT, '0.0.0.0');
  try {
    console.log('data URI length:', (dataUri.length / 1024).toFixed(0), 'KB');
    adb('root'); await sleep(2000);
    adb('shell', 'echo "chrome --no-first-run --disable-fre --no-default-browser-check" > /data/local/tmp/chrome-command-line; chmod 755 /data/local/tmp/chrome-command-line');
    adb('shell', 'am force-stop com.android.chrome');
    adb('shell', 'am start -n com.android.chrome/com.google.android.apps.chrome.Main -d about:blank');
    await sleep(6000);
    adb('forward', `tcp:${CDP_PORT}`, 'localabstract:chrome_devtools_remote');
    const tabs = await (await fetch(`http://127.0.0.1:${CDP_PORT}/json`)).json();
    const tab = tabs.find(t => t.type === 'page');
    if (!tab) throw new Error('no Chrome tab found');
    const ws = new WebSocket(tab.webSocketDebuggerUrl);
    await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
    let id = 0; const waiters = {}; const events = [];
    ws.onmessage = m => { const d = JSON.parse(m.data); if (d.id && waiters[d.id]) waiters[d.id](d); else events.push(d); };
    const send = (method, params = {}) => new Promise(r => { const i = ++id; waiters[i] = r; ws.send(JSON.stringify({ id: i, method, params })); });
    const evalJs = async expr => (await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true })).result;

    await send('Page.enable'); await send('Runtime.enable');
    await send('Page.navigate', { url: dataUri });
    await sleep(6000);
    const state = await evalJs(`JSON.stringify({ title: document.title, nodes: document.getElementsByTagName('*').length,
      displayfalse: document.getElementById('displayfalse') && document.getElementById('displayfalse').selectedIndex,
      bitcolor: typeof minimalcolorpicker_getColorValue === 'function' ? minimalcolorpicker_getColorValue(document.getElementById('onbitscolorpicker_preview').style.backgroundColor) : null,
      canvas: !!document.querySelector('canvas'), fonts: document.fonts ? document.fonts.size : -1 })`);
    const st = JSON.parse(state.result.value || '{}');
    console.log('page state:', JSON.stringify(st));
    if (!st.nodes || st.displayfalse !== 1 || st.bitcolor !== '00aaff') fails.push('page did not render with the injected settings');
    const err = events.filter(e => e.method === 'Runtime.exceptionThrown').map(e => e.params.exceptionDetails.text + ' ' + (e.params.exceptionDetails.exception && e.params.exceptionDetails.exception.description || ''));
    if (err.length) fails.push('page exceptions: ' + err.slice(0, 3).join(' | '));

    await evalJs(`document.getElementById('showdate').checked = true; document.getElementById('showhint').selectedIndex = 2;
                  document.getElementById('dateformat').selectedIndex = 2; save(); 'saved'`);
    await sleep(3000);
    if (returned === null) fails.push('Save did not return anything to the app');
    else {
      const cfg = JSON.parse(decodeURIComponent(returned.replace('/close?', '')));
      console.log('returned keys:', Object.keys(cfg).length);
      if (cfg.showdate !== 1 || cfg.showhint !== 2 || cfg.dateformat !== 2) fails.push('changes not saved: ' + JSON.stringify(cfg));
      if (cfg.bitcolor !== '00aaff') fails.push('unchanged settings altered');
    }
    ws.close();
  } catch (e) { fails.push('harness error: ' + (e && e.message || e)); }
  finally {
    try { adb('shell', 'am force-stop com.android.chrome'); } catch (e) {}
    try { execFileSync('adb', ['-s', SERIAL, 'forward', '--remove', `tcp:${CDP_PORT}`]); } catch (e) {}
    srv.close();
  }
  console.log(fails.length ? 'FAIL\n - ' + fails.join('\n - ') : 'PASS (Android Chrome, data: URI)');
  process.exit(fails.length ? 1 : 0);
})();
