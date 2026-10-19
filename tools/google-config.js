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
const file = opt('--output') ? path.resolve(opt('--output')) : null;
const hex = () => crypto.randomBytes(32).toString('hex');
const google={enabled:true,PROJECT_ID:project,PUBLIC_ORIGIN:origin,CLIENT_ID:'plug2view-google',CLIENT_SECRET:hex(),LOGIN_USER:'owner',LOGIN_PASSWORD:hex(),TOKEN_KEY:hex()};
if(file) {
  fs.writeFileSync(file,Object.entries(google).filter(([k])=>k!=='enabled').map(([k,v])=>k+'='+v+'\n').join(''),{flag:'wx',mode:0o600});
  console.log('Created explicit private Google config; existing files are never overwritten.');
} else {
  const settings=require('./configuration'),c=settings.readLocal();
  if(c.google?.enabled || c.google?.CLIENT_SECRET || c.google?.TOKEN_KEY || (!c.schemaVersion && fs.existsSync(path.join(settings.root,'camera/google.conf'))))throw Error('Existing Google settings found; import or edit them instead of generating new credentials');
  const next=settings.normalize({...c,schemaVersion:1,google});
  fs.writeFileSync(path.join(settings.root,'config.local.json'),JSON.stringify(next,null,2)+'\n',{mode:0o600});
  console.log('Created google settings in ignored config.local.json. Apply with node tools/configure.js --apply.');
}
