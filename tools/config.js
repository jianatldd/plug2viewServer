// Camera address resolution shared by the tools: --camera <ip> > PLUG2VIEW_CAMERA env >
// config.local.json (git-ignored; see config.local.example.json). No address is hard-coded.
'use strict';
const fs = require('fs');
const path = require('path');

function cameraAddress(argv) {
  const i = argv.indexOf('--camera');
  if (i >= 0 && argv[i + 1]) return argv[i + 1];
  if (process.env.PLUG2VIEW_CAMERA) return process.env.PLUG2VIEW_CAMERA;
  const file = path.resolve(__dirname, '..', 'config.local.json');
  try { const c = JSON.parse(fs.readFileSync(file, 'utf8')); if (c.camera) return c.camera; } catch (e) { /* missing is fine */ }
  console.error('camera address unknown: pass --camera <ip>, set PLUG2VIEW_CAMERA, or create config.local.json (see config.local.example.json)');
  process.exit(2);
}

module.exports = { cameraAddress };
