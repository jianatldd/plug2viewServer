// Read-only check for known local configuration values in publishable files.
// node tools/check-public.js [--history]
// Does not print matched values or inspect private Codex snapshot refs.
'use strict';
const fs=require('fs'),path=require('path'),os=require('os'),{execFileSync}=require('child_process');
const root=path.resolve(__dirname,'..');
process.chdir(root);
const git=(args,input)=>execFileSync('git',args,{input,maxBuffer:256*1024*1024});
const patterns=[], findings=new Set();
function add(label,value) {
  if(typeof value==='string' && value.length>=4) patterns.push({label,bytes:Buffer.from(value)});
}
for(const file of ['camera/google.conf','camera/duckdns.conf','camera/wifi.conf','camera/mjpg.conf']) {
  if(!fs.existsSync(file))continue;
  for(const line of fs.readFileSync(file,'utf8').split(/\r?\n/)) {
    const m=line.match(/^([A-Z_]+)=(.*)$/);if(!m)continue;
    const value=m[2].replace(/^(['"])(.*)\1$/,'$2');
    if(['TOKEN_KEY','CLIENT_SECRET','LOGIN_PASSWORD','TOKEN','HOSTNAME','SSID','PASS','CREDENTIALS'].includes(m[1])) add(file+':'+m[1],value);
    if(m[1]==='PUBLIC_ORIGIN') {
      try {add('configured public hostname',new URL(value).hostname);} catch {throw new Error('Invalid PUBLIC_ORIGIN in local config');}
    }
    if(m[1]==='CREDENTIALS'&&value) add('encoded website credentials',Buffer.from(value).toString('base64'));
  }
}
function localValues(obj,prefix) {
  for(const [key,value]of Object.entries(obj)) {
    if(key.startsWith('_'))continue;
    if(value&&typeof value==='object')localValues(value,prefix+'.'+key);
    else add(prefix+'.'+key,value);
  }
}
if(fs.existsSync('config.local.json')) localValues(JSON.parse(fs.readFileSync('config.local.json','utf8')),'config.local.json');
add('local home directory',os.homedir());
add('local home directory',os.homedir().replaceAll('\\','/'));
function privatePath(file) {
  return /^(?:\.?config\.local\.json|camera\/(?:\.?google\.conf|\.?duckdns\.conf|\.?wifi\.conf|tls\/)|\.build\/)/.test(file) && file!=='camera/wifi.conf.example';
}
function scan(data,where) {
  for(const {label,bytes}of patterns)if(data.includes(bytes))findings.add(where+' contains '+label);
  if(data.includes(Buffer.from('-----BEGIN PRIVATE KEY-----')) || data.includes(Buffer.from('-----BEGIN EC PRIVATE KEY-----')) || data.includes(Buffer.from('-----BEGIN RSA PRIVATE KEY-----'))) {
    // Source code may name PEM markers; flag complete PEM blocks instead.
    if(/-----BEGIN (?:EC |RSA )?PRIVATE KEY-----[\r\n]+[A-Za-z0-9+/=\r\n]+-----END (?:EC |RSA )?PRIVATE KEY-----/.test(data.toString())) findings.add(where+' contains a private key');
  }
}
function blobs(entries,scope) {
  if(!entries.length)return;
  const output=git(['cat-file','--batch'],entries.map(e=>e.oid).join('\n')+'\n');let offset=0;
  for(const e of entries) {
    const end=output.indexOf(10,offset);if(end<0)throw new Error('Incomplete Git object response');
    const fields=output.subarray(offset,end).toString().split(' '),size=Number(fields[2]);
    if(!Number.isFinite(size))throw new Error('Missing Git object');
    const data=output.subarray(end+1,end+1+size);offset=end+size+2;
    if(fields[1]==='blob') {
      const where=scope+':'+e.file;
      if(privatePath(e.file))findings.add(where+' is a private configuration/artifact path');
      scan(data,where);
    }
  }
}
const files=new Set(git(['ls-files','--cached','--others','--exclude-standard','-z']).toString().split('\0').filter(Boolean));
for(const file of files) {
  if(privatePath(file))findings.add('working:'+file+' is a private configuration/artifact path');
  if(fs.existsSync(file)&&fs.statSync(file).isFile())scan(fs.readFileSync(file),'working:'+file);
}
const staged=git(['ls-files','--stage','-z']).toString().split('\0').filter(Boolean).map(line=>{
  const [meta,file]=line.split('\t'),[,oid,stage]=meta.split(' ');
  if(stage!=='0')throw new Error('Resolve merge conflicts before the public-config check');
  return {oid,file};
});
blobs(staged,'index');
if(process.argv.includes('--history')) {
  const entries=git(['rev-list','--objects','--branches','--remotes','--tags']).toString().split('\n').filter(Boolean).map(line=>{
    const i=line.indexOf(' ');return {oid:i<0?line:line.slice(0,i),file:i<0?'(object)':line.slice(i+1)};
  });
  blobs(entries,'branch-history');
}
for(const finding of findings)console.error(finding);
if(findings.size){console.error('FAIL: local configuration may be exposed; values withheld.');process.exitCode=1;}
else console.log('PASS: no known local config values or private config paths found in '+files.size+' publishable files, the Git index'+(process.argv.includes('--history')?', and locally available branch/tag history':'')+'.');
console.log('This checks locally available values/history; it is not a general secret scanner or a remote-host audit.');
