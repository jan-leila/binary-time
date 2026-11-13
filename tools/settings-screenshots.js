#!/usr/bin/env node
// Visual regression screenshots of the settings page (config/config-page.html, bundled as src/pkjs/config-page.js)
// in headless Chromium.
//
//   node tools/settings-screenshots.js [--update] [--threshold F] [--only REGEX] [platform...]
//                                                        # default platforms: aplite basalt chalk diorite emery
//
// Rebuilds src/pkjs/config-page.js (tools/build-config.js), then for every platform x SCENE loads the page with the
// same injected parameters src/pkjs/index.js passes (settings, watch platform, version), and saves a full-page
// screenshot at phone width to test/settings-screenshots/current/<platform>/<scene>.png (with --update: .../baseline/).
// Without --update the images are compared with the baseline (tools/lib/png-diff.js): diff images go to
// test/settings-screenshots/diff/, a table is printed, and the exit status is non-zero when any image differs by more
// than F (fraction of pixels, default 0.001) or is missing. Page script errors also fail the run.
//
// Determinism: Date is frozen at 2026-12-28 21:37:46 UTC before the page's scripts run (the watch previews draw the
// current time every second), timezone and locale are pinned, and CSS transitions, animations and the text caret are
// turned off.
'use strict';
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');
const { openPage, findChrome } = require('./lib/chrome');

const ROOT = path.join(__dirname, '..');
const OUT = path.join(ROOT, 'test', 'settings-screenshots');
const FIXED_NOW = 1798493866000;  // 2026-12-28 21:37:46 UTC, as tools/screenshots.sh
const ALL_PLATFORMS = ['aplite', 'basalt', 'chalk', 'diorite', 'emery'];

// Settings as the app stores them (src/pkjs/index.js keeps the object the page returned).
const BASE = { storage_version: 4, truebinary: 0, displayfalse: 1, showimpossible: 0,
  backgroundcolor: '000000', bitcolor: '00aaff', offcolor: '555555', hintcolor: '00aaff',
  displayseconds: 1, timealignment: 0, timeformat: 1, showhint: 0, datebitcolor: '00ff55',
  dateoffcolor: '555555', datehintcolor: '00ff55', showdate: 0, dateposition: 0, datealignment: 0,
  dateformat: 0, dateshowhint: 0, btloss: 1, btlossvibration: 3, btreconnectvibration: 3, btlevel: 1,
  btlevelvibration: 0, vibratehourly: 0, vibratehourlyasleep: 0, dndwhilecharging: 0, tappingtimeout: 5000, watchtimeformat: 1,
  timedeco: 0, datedeco: 0, trueborder: 0, falseborder: 0, secint: 1, quickview: 0 };

// name, settings merged over BASE (null = no saved settings: the page's own defaults), optional page action
const SCENES = [
  ['01-defaults', null],
  ['02-saved', {}],
  ['03-true-binary', { truebinary: 1, showimpossible: 1 }],
  ['04-date-hints', { showdate: 1, dateposition: 1, showhint: 2, dateshowhint: 2, dateformat: 2 }],
  ['05-colors-borders', { backgroundcolor: 'ffffff', bitcolor: 'ff0055', offcolor: 'aaaaaa', displayfalse: 4,
    trueborder: 1, falseborder: 1, showdate: 1, datebitcolor: '0000aa' }],
  ['06-12h-align-deco', { timeformat: 2, displayseconds: 0, timealignment: 1, timedeco: 1, showdate: 1,
    datealignment: 2, datedeco: 1, dateformat: 4 }],
  ['07-tapped', { showhint: 1, showdate: 1, dateshowhint: 1, btlevel: 2 }, 'handleTapping()'],
  ['08-hourly-vibe-asleep', { vibratehourly: 1, vibratehourlyasleep: 1 }],  // toggle hidden on aplite
];

const FREEZE_DATE = `(() => {
  const RealDate = Date, NOW = ${FIXED_NOW};
  class FrozenDate extends RealDate {
    constructor(...a) { if (a.length) super(...a); else super(NOW); }
    static now() { return NOW; }
  }
  window.Date = FrozenDate;
})();`;
const STILL_CSS = '*, *::before, *::after { transition: none !important; animation: none !important; caret-color: transparent !important; }';

function pageHtml(platform, settings) {
  delete require.cache[require.resolve('../src/pkjs/config-page')];
  const page = require('../src/pkjs/config-page');
  const params = { current_version: '2.4', current_language: 'en-US', current_watch: JSON.stringify({ platform }) };
  if (settings) params.current_config = JSON.stringify(Object.assign({}, BASE, settings));
  // the same injection src/pkjs/index.js performs
  const injected = '<script>window.__bcdParams = ' + JSON.stringify(params).replace(/</g, '\\u003c') + ';</script>';
  return page.replace('<!--BCD-PARAMS-->', injected);
}

async function main(argv) {
  let update = false, threshold = 0.001, only = null;
  const platforms = [];
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--update') update = true;
    else if (argv[i] === '--threshold') threshold = Number(argv[++i]);
    else if (argv[i] === '--only') only = new RegExp(argv[++i]);
    else if (argv[i] === '-h' || argv[i] === '--help') {
      console.log(fs.readFileSync(__filename, 'utf8').split('\n').slice(1, 18).join('\n'));
      return 0;
    } else if (ALL_PLATFORMS.includes(argv[i])) platforms.push(argv[i]);
    else { console.error('unknown argument ' + argv[i]); return 2; }
  }
  if (!platforms.length) platforms.push(...ALL_PLATFORMS);
  if (!findChrome()) { console.error('[settings-screenshots] no chromium found (run inside nix-shell or set $CHROME)'); return 2; }

  execFileSync(process.execPath, [path.join(ROOT, 'tools', 'build-config.js')], { stdio: 'inherit' });
  const dest = path.join(OUT, update ? 'baseline' : 'current');
  const tmp = fs.mkdtempSync(path.join(require('os').tmpdir(), 'binary-watch-settings-'));
  let fail = 0;
  try {
    for (const platform of platforms) {
      const dir = path.join(dest, platform);
      fs.mkdirSync(dir, { recursive: true });
      for (const f of fs.readdirSync(dir)) if (f.endsWith('.png') && (!only || only.test(f.slice(0, -4)))) fs.rmSync(path.join(dir, f));
      for (const [name, settings, action] of SCENES) {
        if (only && !only.test(name)) continue;
        const file = path.join(tmp, `${platform}-${name}.html`);
        fs.writeFileSync(file, pageHtml(platform, settings));
        const page = await openPage('file://' + file, { width: 400, height: 800, initScript: FREEZE_DATE, css: STILL_CSS });
        try {
          await page.eval('updatePreview()');  // the page itself first draws the previews on a 1 s timer
          if (action) await page.eval(action);
          await page.settle();
          fs.writeFileSync(path.join(dir, name + '.png'), await page.screenshot());
          if (page.errors.length) { fail = 1; console.error(`[settings-screenshots ${platform}] FAIL: ${name}: page errors: ${page.errors.join(' | ')}`); }
          else console.log(`[settings-screenshots ${platform}] ${name}`);
        } finally {
          await page.close();
        }
      }
    }
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
  if (update) { console.log('[settings-screenshots] baseline written to ' + dest); return fail; }

  // compare only the platforms (and with --only, the scenes) that ran
  const cmpBase = fs.mkdtempSync(path.join(require('os').tmpdir(), 'binary-watch-cmp-'));
  try {
    for (const platform of platforms) {
      fs.mkdirSync(path.join(cmpBase, platform), { recursive: true });
      const bdir = path.join(OUT, 'baseline', platform);
      if (!fs.existsSync(bdir)) continue;
      for (const f of fs.readdirSync(bdir)) {
        if (f.endsWith('.png') && (!only || only.test(f.slice(0, -4)))) fs.copyFileSync(path.join(bdir, f), path.join(cmpBase, platform, f));
      }
    }
    const curBase = path.join(OUT, 'current');
    const cmpCur = fs.mkdtempSync(path.join(require('os').tmpdir(), 'binary-watch-cur-'));
    for (const platform of platforms) fs.cpSync(path.join(curBase, platform), path.join(cmpCur, platform), { recursive: true });
    fs.rmSync(path.join(OUT, 'diff'), { recursive: true, force: true });
    const code = execFileSyncStatus(process.execPath, [path.join(ROOT, 'tools', 'lib', 'png-diff.js'), 'compare',
      cmpBase, cmpCur, path.join(OUT, 'diff'), '--threshold', String(threshold)]);
    fs.rmSync(cmpCur, { recursive: true, force: true });
    return code || fail;
  } finally {
    fs.rmSync(cmpBase, { recursive: true, force: true });
  }
}

function execFileSyncStatus(cmd, args) {
  try { execFileSync(cmd, args, { stdio: 'inherit' }); return 0; } catch (e) { return e.status || 1; }
}

if (require.main === module) main(process.argv.slice(2)).then((c) => process.exit(c), (e) => { console.error(e); process.exit(1); });
