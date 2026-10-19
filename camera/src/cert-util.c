/* DuckDNS hook and validated certificate conversion. Secrets stay in a 0600
 * configuration file, never argv, diagnostics, or the HTTP viewer tree. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <arpa/inet.h>
#include <curl/curl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#define BASE "/mnt/sif/owrt"
#define CA BASE "/certificates/ca.pem"
static char host[128],token[128];
static int debug;
static void fail(const char *s){fprintf(stderr,"cert-util: %s\n",s);exit(1);}
static void config(void){
 FILE *f=fopen(BASE "/duckdns.conf","r");if(!f)fail("missing DuckDNS config");
 char line[300];while(fgets(line,sizeof line,f)){line[strcspn(line,"\r\n")]=0;
  if(!strncmp(line,"HOSTNAME=",9)&&strlen(line+9)<sizeof host)strcpy(host,line+9);
  else if(!strncmp(line,"TOKEN=",6)&&strlen(line+6)<sizeof token)strcpy(token,line+6);
 }fclose(f);
 size_t n=strlen(host);if(n<13||strcmp(host+n-12,".duckdns.org")||!token[0])fail("invalid DuckDNS config");
 for(size_t i=0;i<n-12;i++)if(!isalnum((unsigned char)host[i])&&host[i]!='-')fail("invalid DuckDNS hostname");
 for(char *p=token;*p;p++)if(!isalnum((unsigned char)*p)&&*p!='-')fail("invalid DuckDNS token");
}
struct reply {char data[8192];size_t n;};
static size_t receive(char *b,size_t size,size_t count,void *arg){struct reply *r=arg;size_t n=size*count;if(n>=sizeof r->data-r->n)return 0;memcpy(r->data+r->n,b,n);r->n+=n;r->data[r->n]=0;return n;}
static int https(const char *url,struct reply *r){
 CURL *c=curl_easy_init();if(!c)return 0;long status=0;
 if(debug){fprintf(stderr,"cert-util: HTTPS handle ready\n");curl_easy_setopt(c,CURLOPT_VERBOSE,1L);}
 curl_easy_setopt(c,CURLOPT_URL,url);curl_easy_setopt(c,CURLOPT_CAINFO,CA);
 curl_easy_setopt(c,CURLOPT_SSL_VERIFYPEER,1L);curl_easy_setopt(c,CURLOPT_SSL_VERIFYHOST,2L);
 curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"https");curl_easy_setopt(c,CURLOPT_IPRESOLVE,CURL_IPRESOLVE_V4);
 curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,15L);curl_easy_setopt(c,CURLOPT_TIMEOUT,45L);
 curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
 curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,receive);curl_easy_setopt(c,CURLOPT_WRITEDATA,r);
 CURLcode e=curl_easy_perform(c);curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);curl_easy_cleanup(c);
 if(e){fprintf(stderr,"cert-util: HTTPS failed (%d)\n",(int)e);return 0;}return status==200;
}
static void write_file(const char *path,const void *data,size_t size){
 int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)fail("cannot create output");
 size_t done=0;while(done<size){ssize_t n=write(fd,(const char *)data+done,size-done);if(n<0&&errno==EINTR)continue;if(n<=0)fail("cannot write output");done+=n;}
 if(fsync(fd)||close(fd))fail("cannot flush output");
}
static int duck(const char *txt,int clear){
 char url[1024],domain[128];snprintf(domain,sizeof domain,"%.*s",(int)strlen(host)-12,host);
 if(txt){for(const char *p=txt;*p;p++)if(!isalnum((unsigned char)*p)&&*p!='-'&&*p!='_')fail("invalid challenge");if(strlen(txt)>128)fail("challenge too long");}
 snprintf(url,sizeof url,"https://www.duckdns.org/update?domains=%s&token=%s&%s=%s%s&verbose=true",domain,token,txt?"txt":"ip",txt?txt:"",clear?"&clear=true":"");
 struct reply r={0};if(!https(url,&r)||strncmp(r.data,"OK\n",3))return 0;
 if(!txt){char ip[INET_ADDRSTRLEN];struct in_addr a;char *end=strchr(r.data+3,'\n');if(!end||(size_t)(end-(r.data+3))>=sizeof ip)return 0;memcpy(ip,r.data+3,end-(r.data+3));ip[end-(r.data+3)]=0;if(inet_pton(AF_INET,ip,&a)!=1)return 0;
  write_file("/tmp/plug2view-public-ip.next",ip,strlen(ip));if(rename("/tmp/plug2view-public-ip.next","/tmp/plug2view-public-ip"))return 0;printf("DuckDNS updated: %s -> %s\n",host,ip);
 }return 1;
}
static void convert(const char *name,const char *certfile,const char *keyfile,const char *out){
 mbedtls_x509_crt crt,ca;mbedtls_pk_context key;mbedtls_entropy_context entropy;mbedtls_ctr_drbg_context rng;
 mbedtls_x509_crt_init(&crt);mbedtls_x509_crt_init(&ca);mbedtls_pk_init(&key);mbedtls_entropy_init(&entropy);mbedtls_ctr_drbg_init(&rng);
 if(mbedtls_ctr_drbg_seed(&rng,mbedtls_entropy_func,&entropy,(const unsigned char *)"cert-util",9))fail("random source failed");
 if(mbedtls_x509_crt_parse_file(&crt,certfile)||mbedtls_x509_crt_parse_file(&ca,CA)<0||!ca.raw.p)fail("cannot parse certificate/roots");
 uint32_t flags=0;if(mbedtls_x509_crt_verify(&crt,&ca,NULL,name,&flags,NULL,NULL))fail("certificate hostname, validity, or trust check failed");
 if(mbedtls_pk_parse_keyfile(&key,keyfile,NULL,mbedtls_ctr_drbg_random,&rng)||!mbedtls_pk_can_do(&key,MBEDTLS_PK_ECKEY)||mbedtls_pk_get_bitlen(&key)!=256||mbedtls_pk_check_pair(&crt.pk,&key,mbedtls_ctr_drbg_random,&rng))fail("certificate requires matching P-256 key");
 unsigned char cert[16384],priv[1024];size_t used=0;int count=0;
 for(mbedtls_x509_crt *p=&crt;p&&p->raw.p;p=p->next){if(++count>5||p->raw.len>sizeof cert-used)fail("certificate chain too large");memcpy(cert+used,p->raw.p,p->raw.len);used+=p->raw.len;}
 int n=mbedtls_pk_write_key_der(&key,priv,sizeof priv);if(n<=0)fail("cannot encode private key");
 char path[512];if(strlen(out)>450)fail("output path too long");
 snprintf(path,sizeof path,"%s/cert.der",out);write_file(path,cert,used);
 snprintf(path,sizeof path,"%s/key.der",out);write_file(path,priv+sizeof priv-n,n);
 printf("Validated %s certificate; expires %04d-%02d-%02d UTC\n",name,crt.valid_to.year,crt.valid_to.mon,crt.valid_to.day);
 mbedtls_pk_free(&key);mbedtls_x509_crt_free(&crt);mbedtls_x509_crt_free(&ca);mbedtls_ctr_drbg_free(&rng);mbedtls_entropy_free(&entropy);
}
int main(int argc,char **argv){
 if(argc==3&&!strcmp(argv[1],"rmdir"))return rmdir(argv[2])?1:0;
 if(argc==3&&!strcmp(argv[1],"readlink")){char target[512];ssize_t n=readlink(argv[2],target,sizeof target-1);if(n<0)return 1;target[n]=0;puts(target);return 0;}
 if(argc==4&&!strcmp(argv[1],"same")) {
  FILE *a=fopen(argv[2],"rb"),*b=fopen(argv[3],"rb");if(!a||!b){if(a)fclose(a);if(b)fclose(b);return 1;}
  unsigned char x[2048],y[2048];int equal=1;
  for(;;){size_t n=fread(x,1,sizeof x,a),m=fread(y,1,sizeof y,b);if(n!=m||memcmp(x,y,n)){equal=0;break;}if(!n){equal=!ferror(a)&&!ferror(b);break;}}
  fclose(a);fclose(b);return equal?0:1;
 }
 if(argc==4&&!strcmp(argv[1],"due")) {
  struct stat st;long interval=strtol(argv[3],NULL,10);if(interval<=0)return 1;
  if(stat(argv[2],&st))return errno==ENOENT?0:1;
  return time(NULL)-st.st_mtime>=interval?0:1;
 }
 if(argc==3&&!strcmp(argv[1],"select")) {
  if(strncmp(argv[2],"renewal-",8)||!argv[2][8])return 1;
  for(char *p=argv[2]+8;*p;p++)if(!isdigit((unsigned char)*p)&&*p!='-')return 1;
  unlink(BASE "/tls/managed.next");
  if(symlink(argv[2],BASE "/tls/managed.next")||rename(BASE "/tls/managed.next",BASE "/tls/managed"))fail("cannot switch certificate pair");return 0;
 }
 if(argc>=4&&!strcmp(argv[1],"run")) {
  int seconds=atoi(argv[2]);if(seconds<1||seconds>900)return 1;
  pid_t pid=fork();if(pid<0)return 1;if(!pid){setpgid(0,0);execv(argv[3],argv+3);_exit(127);}setpgid(pid,pid);
  int status=0;for(int i=0;i<seconds;i++){pid_t got=waitpid(pid,&status,WNOHANG);if(got==pid)return WIFEXITED(status)?WEXITSTATUS(status):1;if(got<0)return 1;sleep(1);}
  kill(-pid,SIGTERM);sleep(2);kill(-pid,SIGKILL);waitpid(pid,&status,0);fail("operation timed out");
 }
 if(argc==2&&!strcmp(argv[1],"ddns")){config();return duck(NULL,0)?0:1;}
 if(argc==2&&!strcmp(argv[1],"hostname")){config();puts(host);return 0;}
 if(argc==3&&!strcmp(argv[1],"check-https")){if(strncmp(argv[2],"https://",8))return 1;debug=getenv("P2V_CERT_DEBUG")!=NULL;struct reply r={0};if(!https(argv[2],&r))return 1;puts("HTTPS verification passed");return 0;}
 if(argc==6&&!strcmp(argv[1],"convert")){convert(argv[2],argv[3],argv[4],argv[5]);return 0;}
 /* uacme invokes the hook directly with method/type/identifier/token/auth. */
 if(argc==6){config();if(strcmp(argv[2],"dns-01")||strcmp(argv[3],host))return 1;
  if(!strcmp(argv[1],"begin")){if(!duck(argv[5],0))return 1;write_file("/tmp/plug2view-acme-txt",argv[5],strlen(argv[5]));sleep(90);return 0;}
  if(!strcmp(argv[1],"done")||!strcmp(argv[1],"failed")){FILE *f=fopen("/tmp/plug2view-acme-txt","r");char old[129]={0};if(!f)return 0;fread(old,1,sizeof old-1,f);fclose(f);if(strcmp(old,argv[5]))return 1;if(!duck("",1))return 1;unlink("/tmp/plug2view-acme-txt");return 0;}
 }
 fail("invalid arguments");return 1;
}
