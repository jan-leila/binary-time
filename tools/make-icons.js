#!/usr/bin/env node
// Draws the app icon: a dark rounded tile holding the watchface's BCD dot grid, reading 12:34.
// Columns are hour tens, hour units, minute tens, minute units, bottom-aligned with heights 2/4/3/4
// like the face itself (with impossible dots hidden). Lit dots are 1, 2, 1+2, 4 from the bottom.
//
//   resources/icon.png      25x25 menu icon, colour platforms (Pebble palette only, no anti-aliasing)
//   resources/icon~bw.png   25x25 menu icon, aplite/diorite (black and white only)
//   store/icon-48.png       store small icon
//   store/icon-144.png      store large icon
//
// Usage: node tools/make-icons.js
'use strict';
const fs = require('fs');
const path = require('path');
const { encode } = require('./lib/png-diff');

const ROOT = path.join(__dirname, '..');
const HEIGHTS = [2, 4, 3, 4];
const DIGITS = [1, 2, 3, 4];
const ROWS = 4;

const TILE = [0, 0, 0, 255];
const CLEAR = [0, 0, 0, 0];
const OFF = [0x55, 0x55, 0x55, 255]; // GColorDarkGray
const ON = [0x00, 0xaa, 0xff, 255]; // GColorVividCerulean
const WHITE = [255, 255, 255, 255];

// Layout in 25ths of the icon: 3-unit dots on a 6-unit pitch, 2-unit margin, so the grid is centred.
const UNIT = 1 / 25;
const DOT = 3 * UNIT;
const PITCH = 6 * UNIT;
const MARGIN = 2 * UNIT;

function dots() {
  const out = [];
  HEIGHTS.forEach((h, col) => {
    for (let bit = 0; bit < h; bit++) {
      const row = ROWS - 1 - bit;
      out.push({ x: MARGIN + col * PITCH, y: MARGIN + row * PITCH, on: (DIGITS[col] >> bit) & 1 });
    }
  });
  return out;
}

function inRoundRect(px, py, x, y, w, h, r) {
  if (px < x || py < y || px >= x + w || py >= y + h) return false;
  const cx = Math.min(Math.max(px, x + r), x + w - r);
  const cy = Math.min(Math.max(py, y + r), y + h - r);
  return (px - cx) ** 2 + (py - cy) ** 2 <= r * r;
}

// Colour at a point in unit coordinates, for the smooth (store) icons.
function sample(u, v) {
  for (const d of dots()) {
    if (inRoundRect(u, v, d.x, d.y, DOT, DOT, DOT * 0.18)) return d.on ? ON : OFF;
  }
  return inRoundRect(u, v, 0, 0, 1, 1, 0.18) ? TILE : CLEAR;
}

function smooth(size, ss = 8) {
  const data = new Uint8Array(size * size * 4);
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      const acc = [0, 0, 0, 0];
      for (let j = 0; j < ss; j++) {
        for (let i = 0; i < ss; i++) {
          const c = sample((x + (i + 0.5) / ss) / size, (y + (j + 0.5) / ss) / size);
          const a = c[3];
          acc[0] += c[0] * a; acc[1] += c[1] * a; acc[2] += c[2] * a; acc[3] += a;
        }
      }
      const o = (y * size + x) * 4;
      const n = ss * ss;
      if (acc[3]) for (let k = 0; k < 3; k++) data[o + k] = Math.round(acc[k] / acc[3]);
      data[o + 3] = Math.round(acc[3] / n);
    }
  }
  return { width: size, height: size, data };
}

// Pixel-exact 25x25: tile with its corner pixels cut, 3x3 dots. In black and white an unlit dot is its
// centre pixel only.
function pixel(bw) {
  const size = 25;
  const data = new Uint8Array(size * size * 4);
  const put = (x, y, c) => data.set(c, (y * size + x) * 4);
  for (let y = 0; y < size; y++) for (let x = 0; x < size; x++) put(x, y, TILE);
  for (const [x, y] of [[0, 0], [24, 0], [0, 24], [24, 24]]) put(x, y, CLEAR);
  for (const d of dots()) {
    const x0 = Math.round(d.x * size);
    const y0 = Math.round(d.y * size);
    for (let j = 0; j < 3; j++) {
      for (let i = 0; i < 3; i++) {
        if (bw) {
          if (d.on || (i === 1 && j === 1)) put(x0 + i, y0 + j, WHITE);
        } else {
          put(x0 + i, y0 + j, d.on ? ON : OFF);
        }
      }
    }
  }
  return { width: size, height: size, data };
}

const outputs = [
  ['resources/icon.png', pixel(false)],
  ['resources/icon~bw.png', pixel(true)],
  ['store/icon-48.png', smooth(48)],
  ['store/icon-144.png', smooth(144)],
];
for (const [rel, img] of outputs) {
  const file = path.join(ROOT, rel);
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, encode(img));
  console.log(`wrote ${rel}`);
}
