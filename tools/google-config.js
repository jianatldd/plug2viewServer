// Create private on-camera Google integration settings. Never prints passwords.
// node tools/google-config.js --project PROJECT_ID --origin https://camera.example.com
'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const args = process.argv.slice(2);
const opt = n => { const i = args.indexOf(n); return i < 0 ? null : args[i + 1]; };
const project = opt('--project'), origin = opt('--origin');
if (!project || !/^[a-z][a-z0-9-]{4,62}$/.test(project) || !origin || !/^https:\/\/[a-z0-9.-]+(?::\d+)?$/i.test(origin)) {
  console.error('Usage: node tools/google-config.js --project PROJECT_ID --origin https://camera.example.com[:port]');
  process.exit(2);
}
const file = opt('--output') ? path.resolve(opt('--output')) : path.resolve(__dirname, '../camera/google.conf');
const hex = () => crypto.randomBytes(32).toString('hex');
fs.writeFileSync(file, `# Private single-camera Google Home prototype; do not commit.\nPROJECT_ID=${project}\nPUBLIC_ORIGIN=${origin}\nCLIENT_ID=plug2view-google\nCLIENT_SECRET=${hex()}\nLOGIN_USER=owner\nLOGIN_PASSWORD=${hex()}\nTOKEN_KEY=${hex()}\n`, {flag: 'wx', mode: 0o600});
console.log(`Created ${file}. Read this private file for the client secret and login password; do not paste them into chat.\nAuthorization URL: ${origin}/google/auth\nToken URL: ${origin}/google/token\nFulfillment URL: ${origin}/google/fulfillment\nClient ID: plug2view-google\nGoogle must send client credentials in the POST body.`);
