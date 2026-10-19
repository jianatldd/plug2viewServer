// Fetch pinned Windows build tools. Executables come only from upstream releases.
'use strict';
const fs=require('fs'),path=require('path'),crypto=require('crypto');
const {execFileSync}=require('child_process');
const dir=path.resolve('.build/certificates');fs.mkdirSync(dir,{recursive:true});
const tools=[
 ['Kitware/CMake','v4.4.4','cmake-4.4.4-windows-x86_64.zip','bace36e94b31c68ab6fa295f26dfa11219e0701cf7c94b0284a7d1cb13dac536',dir],
 ['ninja-build/ninja','v1.13.2','ninja-win.zip','07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65',path.join(dir,'ninja')]
];
(async()=>{for(const [repo,tag,name,hash,dest]of tools){const file=path.join(dir,name);if(!fs.existsSync(file)){const r=await fetch(`https://github.com/${repo}/releases/download/${tag}/${name}`);if(!r.ok)throw Error('Download failed: '+r.status);fs.writeFileSync(file,Buffer.from(await r.arrayBuffer()));}if(crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex')!==hash)throw Error(name+' checksum mismatch');fs.mkdirSync(dest,{recursive:true});execFileSync('tar',['-xf',file,'-C',dest]);console.log(name+' verified');}})().catch(e=>{console.error(e.message);process.exitCode=1;});
