/* Isolated on-device checks: ephemeral sockets and a private temporary directory. */
#undef NDEBUG
#include <assert.h>
#define MOTION_HEARTBEAT "heartbeat"
#define WATCH_LOCK "watch.lock"
#define WATCH_SCRIPT "watch.sh"
#define main hkctl_main
#include "../camera/src/hkctl.c"
#undef main

static void probe_server(const char *response,int should_pass,int hangs) {
    int s=socket(AF_INET,SOCK_STREAM,0); assert(s>=0);
    struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(0x7f000001)};
    assert(!bind(s,(struct sockaddr *)&a,sizeof a)); assert(!listen(s,1));
    socklen_t len=sizeof a; assert(!getsockname(s,(struct sockaddr *)&a,&len));
    char port[16]; snprintf(port,sizeof port,"%u",ntohs(a.sin_port));
    pid_t server=fork(); assert(server>=0);
    if(!server) {
        int c=accept(s,NULL,NULL); if(c<0) _exit(2);
        char req[512]; read(c,req,sizeof req);
        if(hangs) { sleep(15); } else { write(c,response,strlen(response)); }
        close(c); close(s); _exit(0);
    }
    close(s); long long start=recording_clock_ms();
    pid_t probe=fork(); assert(probe>=0);
    if(!probe) { char *args[]={"hkctl","health","controls",port}; _exit(cmd_health(4,args)); }
    int status; assert(waitpid(probe,&status,0)==probe);
    if(should_pass) assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
    else assert(!WIFEXITED(status) || WEXITSTATUS(status)!=0);
    if(hangs) assert(recording_clock_ms()-start>=4500 && recording_clock_ms()-start<8000);
    kill(server,SIGKILL); waitpid(server,NULL,0);
}
int main(void) {
    char dir[]="/tmp/p2v-health-XXXXXX"; assert(mkdtemp(dir)); assert(!chdir(dir));
    probe_server("HTTP/1.0 200 OK\r\n\r\n{\"ok\":true}",1,0);
    probe_server("HTTP/1.0 200 OK\r\n\r\n{\"wrong\":true}",0,0);
    probe_server("HTTP/1.0 500 Error\r\n\r\n{\"ok\":true}",0,0);
    probe_server("",0,1);
    puts("PASS: successful HTTP probe, wrong body/status rejection, five-second hung-server deadline");
    char pid[24]; snprintf(pid,sizeof pid,"%ld",(long)getpid());
    char *args[]={"hkctl","health","motion",pid};
    assert(cmd_health(4,args)==2); motion_heartbeat(); assert(!cmd_health(4,args));
    FILE *f=fopen(MOTION_HEARTBEAT,"w"); assert(f);
    fprintf(f,"%s %lld\n",pid,recording_clock_ms()-31000); fclose(f); assert(cmd_health(4,args)==2);
    motion_heartbeat(); args[3]="1"; assert(cmd_health(4,args)==2);
    puts("PASS: motion heartbeat freshness, missing heartbeat and replacement-PID rejection");
    f=fopen(WATCH_SCRIPT,"w"); assert(f); fputs("echo ran > ran\n",f); fclose(f);
    int fd=open(WATCH_LOCK,O_RDWR|O_CREAT,0600); assert(fd>=0); assert(!flock(fd,LOCK_EX));
    assert(!cmd_watch()); assert(access("ran",F_OK)); close(fd);
    assert(!cmd_watch()); assert(!access("ran",F_OK));
    fd=open(WATCH_LOCK,O_RDWR); assert(fd>=0); assert(!flock(fd,LOCK_EX|LOCK_NB)); close(fd);
    unlink("ran"); unlink(WATCH_SCRIPT); unlink(WATCH_LOCK); unlink(MOTION_HEARTBEAT);
    assert(!chdir("/tmp")); assert(!rmdir(dir));
    puts("PASS: overlapping watchdog checks skipped and lock released after completion");
    return 0;
}
