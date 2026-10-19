// Build pinned uacme/curl/Mbed TLS for the camera, separately from video firmware.
// Prerequisites: portable CMake + Ninja in .build/certificates; --fetch clones sources.
'use strict';
const fs=require('fs'),path=require('path'),crypto=require('crypto');
const {spawn,spawnSync}=require('child_process');
const root=path.resolve(__dirname,'..'),dir=path.join(root,'.build/certificates');
const args=process.argv.slice(2),zig=path.resolve(args.includes('--zig')?args[args.indexOf('--zig')+1]:'.build/zig-x86_64-windows-0.14.1/zig.exe');
function run(exe,a){const r=spawnSync(exe,a,{cwd:root,encoding:'utf8',maxBuffer:8*1024*1024});if(r.error||r.status)throw Error(r.error||r.stdout+'\n'+r.stderr);return r.stdout.trim();}
const deps=[['uacme','ndilieto/uacme','upstream/latest','625445e2c316bbf7ce2ef70f3cfb56c00c3bfd7c'],['curl','curl/curl','curl-8_23_0','3163ec065643ba20709f190111f42c3e1fe95273']];
fs.mkdirSync(dir,{recursive:true});
for(const [name,repo,tag,commit]of deps){const dest=path.join(dir,name);if(!fs.existsSync(dest)&&args.includes('--fetch')){fs.mkdirSync(dest,{recursive:true});run('git',['-C',dest,'init']);run('git',['-C',dest,'remote','add','origin',`https://github.com/${repo}.git`]);run('git',['-C',dest,'fetch','--depth','1','origin',commit]);run('git',['-C',dest,'checkout','--detach','FETCH_HEAD']);}if(run('git',['-C',dest,'rev-parse','HEAD'])!==commit)throw Error(name+' revision mismatch');if(run('git',['-C',dest,'status','--porcelain','--untracked-files=no']))throw Error(name+' source modified');}
const mb=path.join(root,'.build/webrtc/mbedtls');
if(run('git',['-C',mb,'rev-parse','HEAD'])!=='068ff080b369adfac81509f9b57b2afabaf82dc5')throw Error('Mbed TLS revision mismatch');
const common=['-target','mipsel-linux-musleabi','-mcpu=mips32','-Os','-ffunction-sections','-fdata-sections'];
const inc=['-I'+path.join(root,'camera/src'),'-I'+path.join(mb,'include'),'-DMBEDTLS_CONFIG_FILE="cert-mbedtls.h"'];
const objdir=path.join(dir,'obj');fs.mkdirSync(objdir,{recursive:true});
const hash=crypto.createHash('sha256').update(fs.readFileSync('camera/src/cert-mbedtls.h')).update(fs.readFileSync('camera/src/webrtc-mbedtls.h')).digest('hex');
const jobs=fs.readdirSync(path.join(mb,'library')).filter(f=>f.endsWith('.c')).map(f=>path.join(mb,'library',f));jobs.push(path.join(root,'camera/src/cert-entropy.c'));
const objects=jobs.map(f=>path.join(objdir,path.basename(f)+'.o'));
const pending=jobs.map((src,i)=>({src,obj:objects[i],hash:crypto.createHash('sha256').update(hash).update(fs.readFileSync(src)).digest('hex')})).filter(j=>!fs.existsSync(j.obj)||!fs.existsSync(j.obj+'.hash')||fs.readFileSync(j.obj+'.hash','utf8')!==j.hash);
async function worker(){for(;;){const j=pending.pop();if(!j)return;await new Promise((res,rej)=>{const p=spawn(zig,['cc',...common,...inc,'-c',j.src,'-o',j.obj],{cwd:root,stdio:['ignore','ignore','pipe']});let err='';p.stderr.on('data',d=>err+=d);p.on('error',rej);p.on('exit',code=>{if(code)rej(Error(err));else{fs.writeFileSync(j.obj+'.hash',j.hash);res();}});});}}
(async()=>{
 console.log('Certificate TLS: compiling '+pending.length+' objects');await Promise.all([worker(),worker(),worker(),worker()]);
 const lib=path.join(dir,'libcerttls.a');run(zig,['ar','rcs',lib,...objects]);
 const cmake=path.join(dir,'cmake-4.4.4-windows-x86_64/bin/cmake.exe'),ninja=path.join(dir,'ninja/ninja.exe');
 const norm=s=>s.replaceAll('\\','/');const flags=common.concat(inc.map(s=>norm(s).replaceAll('"','\\"')),'-include',norm(path.join(root,'camera/src/cert-compat.h'))).join(' ');
 const ar=path.join(dir,'zig-ar.cmd'),ranlib=path.join(dir,'zig-ranlib.cmd');
 fs.writeFileSync(ar,'@"'+zig+'" ar %*\r\n');fs.writeFileSync(ranlib,'@"'+zig+'" ranlib %*\r\n');
 const build=path.join(dir,'curl-build');
 const opts={CMAKE_SYSTEM_NAME:'Linux',CMAKE_SYSTEM_PROCESSOR:'mipsel',CMAKE_C_COMPILER:norm(zig),CMAKE_C_COMPILER_ARG1:'cc',CMAKE_C_FLAGS:flags,CMAKE_AR:norm(zig),CMAKE_C_ARCHIVE_CREATE:'<CMAKE_AR> ar qc <TARGET> <LINK_FLAGS> <OBJECTS>',CMAKE_C_ARCHIVE_FINISH:'',CMAKE_MAKE_PROGRAM:norm(ninja),CMAKE_TRY_COMPILE_TARGET_TYPE:'STATIC_LIBRARY',BUILD_SHARED_LIBS:'OFF',BUILD_STATIC_LIBS:'ON',BUILD_CURL_EXE:'OFF',BUILD_TESTING:'OFF',BUILD_LIBCURL_DOCS:'OFF',BUILD_MISC_DOCS:'OFF',ENABLE_CURL_MANUAL:'OFF',HTTP_ONLY:'ON',CURL_USE_MBEDTLS:'ON',CURL_USE_OPENSSL:'OFF',CURL_USE_LIBPSL:'OFF',CURL_ZLIB:'OFF',CURL_BROTLI:'OFF',CURL_ZSTD:'OFF',USE_LIBIDN2:'OFF',USE_NGHTTP2:'OFF',CURL_USE_LIBSSH2:'OFF',ENABLE_THREADED_RESOLVER:'OFF',CURL_DISABLE_PROXY:'ON',MBEDTLS_INCLUDE_DIR:norm(path.join(mb,'include')),MBEDTLS_LIBRARY:norm(lib),MBEDX509_LIBRARY:norm(lib),MBEDCRYPTO_LIBRARY:norm(lib),CURL_CA_BUNDLE:'/mnt/sif/owrt/certificates/ca.pem',CURL_CA_PATH:'none'};
 opts.CMAKE_AR=norm(ar);opts.CMAKE_RANLIB=norm(ranlib);opts.CMAKE_C_ARCHIVE_CREATE='<CMAKE_AR> qc <TARGET> <LINK_FLAGS> <OBJECTS>';opts.CMAKE_C_ARCHIVE_FINISH='<CMAKE_RANLIB> <TARGET>';
 // libc exposes these calls but the camera's Linux 2.6.21 kernel predates them.
 opts.HAVE_EVENTFD='0';opts.HAVE_PIPE2='0';opts.HAVE_ACCEPT4='0';
 // Avoid the MIPS sigsetjmp resolver path; the supervisor bounds the whole job.
 opts.HAVE_SIGSETJMP='0';
 console.log('Configuring HTTPS transport');run(cmake,['-S',path.join(dir,'curl'),'-B',build,'-G','Ninja',...Object.entries(opts).map(([k,v])=>'-D'+k+'='+v)]);
 console.log('Building HTTPS transport');run(cmake,['--build',build,'--parallel','4']);
 const cfg=path.join(dir,'config');fs.mkdirSync(cfg,{recursive:true});
 const template=fs.readFileSync(path.join(dir,'uacme/config.h.in'),'utf8');
 const version=fs.readFileSync(path.join(dir,'uacme/.tarball-version'),'utf8').trim();
 const macros={USE_MBEDTLS:'1',HAVE_STRCASESTR:'1',HAVE_ASPRINTF:'1',HAVE_OPEN_MEMSTREAM:'1',HAVE_CLOCK_GETTIME:'1',PACKAGE_NAME:'"uacme"',PACKAGE_VERSION:'"'+version+'"',PACKAGE_STRING:'"uacme '+version+'"',VERSION:'"'+version+'"',_GNU_SOURCE:'1'};
 fs.writeFileSync(path.join(cfg,'config.h'),template.replace(/^#undef (\w+)$/gm,(s,k)=>k in macros?'#define '+k+' '+macros[k]:s)+'\n#define _GNU_SOURCE 1\n');
 const libs=[path.join(build,'lib/libcurl.a'),lib];const uc=['uacme.c','base64.c','crypto.c','curlwrap.c','json.c','msg.c','read-file.c'].map(f=>path.join(dir,'uacme',f));
 run(zig,['cc',...common,...inc,'-I'+cfg,'-I'+path.join(dir,'curl/include'),'-DSYSCONFDIR="/mnt/sif/owrt"','-DRUNSTATEDIR="/tmp"','-static','-s','-Wl,--gc-sections',...uc,...libs,'-o',path.join(dir,'uacme-camera')]);
 console.log('uacme built: '+fs.statSync(path.join(dir,'uacme-camera')).size+' bytes');
 run(zig,['cc',...common,...inc,'-I'+path.join(dir,'curl/include'),'-static','-s','-Wl,--gc-sections','camera/src/cert-util.c',...libs,'-o',path.join(dir,'cert-util')]);
 console.log('cert-util built: '+fs.statSync(path.join(dir,'cert-util')).size+' bytes');
})().catch(e=>{console.error(e.message);process.exitCode=1;});
