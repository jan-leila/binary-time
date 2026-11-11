// Loads the bundled configuration page in headless Chromium with injected settings, changes a few
// controls, presses Save and checks the JSON handed back to the app.
// usage: node tools/test-config.js [platform]     (platform: aplite|basalt|chalk|diorite|emery)
const fs = require('fs'), os = require('os'), path = require('path'), http = require('http');
const { spawn } = require('child_process');
const page = require('../src/pkjs/config-page');
const platform = process.argv[2] || 'basalt';

const current_config = { storage_version: 4, truebinary: 0, displayfalse: 1, showimpossible: 0,
  backgroundcolor: '000000', bitcolor: '00aaff', offcolor: '555555', hintcolor: '00aaff',
  displayseconds: 1, timealignment: 0, timeformat: 0, showhint: 0, datebitcolor: '00ff55',
  dateoffcolor: '555555', datehintcolor: '00ff55', showdate: 0, dateposition: 0, datealignment: 0,
  dateformat: 0, dateshowhint: 0, btloss: 1, btlossvibration: 3, btreconnectvibration: 3, btlevel: 1,
  btlevelvibration: 0, vibratehourly: 0, dndwhilecharging: 0, tappingtimeout: 5000, watchtimeformat: 1,
  timedeco: 0, datedeco: 0, trueborder: 0, falseborder: 0, secint: 1, quickview: 0 };

const PORT = 18765;
const params = { current_version: '2.4', current_language: 'en-US',
  current_config: JSON.stringify(current_config), current_watch: JSON.stringify({ platform }),
  return_to: `http://127.0.0.1:${PORT}/close?` };

// the same injection the phone-side code performs
const injected = '<script>window.__bcdParams = ' + JSON.stringify(params).replace(/</g, '\\u003c') + ';</script>';
const testScript = `<script>
window.addEventListener('load', function () { setTimeout(function () {
  var out = {};
  try {
    out.loaded = { displayfalse: document.getElementById('displayfalse').selectedIndex,
                   bitcolor: minimalcolorpicker_getColorValue(document.getElementById('onbitscolorpicker_preview').style.backgroundColor),
                   seconds: document.getElementById('displayseconds').checked };
    document.getElementById('showdate').checked = true;
    document.getElementById('showhint').selectedIndex = 2;
    document.getElementById('dateformat').selectedIndex = 2;
    save();
  } catch (e) { out.error = String(e && e.stack || e); console.log('TESTRESULT ' + JSON.stringify(out)); }
  console.log('TESTRESULT ' + JSON.stringify(out));
}, 1500); });
</script>`;
const html = page.replace('<!--BCD-PARAMS-->', injected).replace('</body>', testScript + '</body>');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'bcdcfg-'));
fs.writeFileSync(path.join(dir, 'page.html'), html);

let returned = null;
const srv = http.createServer((req, res) => { returned = req.url; res.end('ok'); }).listen(PORT, () => {
  const chrome = spawn('chromium', ['--headless=new', '--disable-gpu', '--no-sandbox',
    '--user-data-dir=' + path.join(dir, 'profile'), '--virtual-time-budget=10000', '--enable-logging=stderr', '--v=0',
    'file://' + path.join(dir, 'page.html')], { stdio: ['ignore', 'ignore', 'pipe'], detached: true });
  let log = '';
  chrome.stderr.on('data', d => log += d);
  const killAll = () => { try { process.kill(-chrome.pid); } catch (e) {} };
  setTimeout(killAll, 20000);
  chrome.on('exit', () => {
    killAll();
    srv.close();
    const fails = [];
    const m = log.match(/TESTRESULT (\{.*\})/);
    const result = m ? JSON.parse(m[1]) : null;
    if (!result) fails.push('page produced no test result');
    else if (result.error) fails.push('script error: ' + result.error);
    else if (result.loaded.displayfalse !== 1 || result.loaded.bitcolor !== '00aaff') fails.push('injected settings not shown: ' + JSON.stringify(result.loaded));
    const errs = (log.match(/Uncaught[^\n]*/g) || []);
    if (errs.length) fails.push('page errors: ' + errs.join(' | '));
    if (returned === null) fails.push('Save did not return anything to the app');
    else {
      const cfg = JSON.parse(decodeURIComponent(returned.replace('/close?', '')));
      if (cfg.showdate !== 1 || cfg.showhint !== 2 || cfg.dateformat !== 2) fails.push('changes not saved: ' + JSON.stringify(cfg));
      if (cfg.bitcolor !== '00aaff' || cfg.displayfalse !== 1) fails.push('unchanged settings altered: ' + JSON.stringify(cfg));
      console.log('returned keys:', Object.keys(cfg).length, '(expected', Object.keys(current_config).length - 1, '+/-)');
    }
    console.log(fails.length ? 'FAIL\n - ' + fails.join('\n - ') : 'PASS (' + platform + ')');
    fs.rmSync(dir, { recursive: true, force: true });
    process.exit(fails.length ? 1 : 0);
  });
});
