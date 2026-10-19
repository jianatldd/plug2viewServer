// Camera address resolution shared by the tools: --camera <ip> > PLUG2VIEW_CAMERA env >
// config.local.json (git-ignored; see config.local.example.json). No address is hard-coded.
'use strict';
const path = require('path');

function cameraAddress(argv) {
  const i = argv.indexOf('--camera');
  if (i >= 0 && argv[i + 1]) return argv[i + 1];
  if (process.env.PLUG2VIEW_CAMERA) return process.env.PLUG2VIEW_CAMERA;
  const file = path.resolve(__dirname, '..', 'config.local.json');
  const c = require('./configuration').readLocal(file); if (c.camera) return c.camera;
  console.error('camera address unknown: pass --camera <ip>, set PLUG2VIEW_CAMERA, or create config.local.json (see config.local.example.json)');
  process.exit(2);
}

function servicePort(key) { return require('./configuration').readLocal().server?.[key] ?? require('./configuration').defaults.server[key]; }
module.exports = { cameraAddress, servicePort, ...require('./configuration') };
