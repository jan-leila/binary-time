#!/usr/bin/env node
// Stand-in for a web browser during `pebble emu-app-config` (set as $BROWSER by tools/screenshots.sh).
//
//   fake-config-browser.js <config page URL>
//
// Instead of showing the page it "submits" it: it reads the JSON settings in $SCENE_CONFIG (the same object the real
// config page returns) and sends them to the page's return_to URL, so src/pkjs/index.js gets a normal 'webviewclosed'
// with that response. pebble-tool starts its return_to server only after the browser was launched, so this detaches
// and retries for a few seconds.
'use strict';
const fs = require('fs');
const http = require('http');
const { spawn } = require('child_process');

if (process.argv[2] === '--child') {
  const [, , , returnTo, configFile] = process.argv;
  const body = encodeURIComponent(fs.readFileSync(configFile, 'utf8').trim());
  let tries = 0;
  const attempt = () => {
    http.get(returnTo + body, (res) => { res.resume(); process.exit(0); })
      .on('error', () => { if (++tries < 40) setTimeout(attempt, 250); else process.exit(1); });
  };
  attempt();
} else {
  const url = process.argv[2] || '';
  const m = /[?&]return_to=([^&]+)/.exec(url);
  if (!m || !process.env.SCENE_CONFIG) {
    console.error('fake-config-browser: need a return_to URL and $SCENE_CONFIG');
    process.exit(1);
  }
  spawn(process.execPath, [__filename, '--child', decodeURIComponent(m[1]), process.env.SCENE_CONFIG],
    { detached: true, stdio: 'ignore' }).unref();
}
