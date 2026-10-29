/* Isolated startup recovery: no real interface changes, capture, or live ports. */
#undef NDEBUG
#include <assert.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <fcntl.h>

static int test_connect(int fd,const struct sockaddr *address,socklen_t len);
static int test_bind(int fd,const struct sockaddr *address,socklen_t len);
static int test_fcntl(int fd,int command,...);
static void *test_malloc(size_t size);
#define connect test_connect
#define bind test_bind
#define fcntl test_fcntl
#define malloc test_malloc
#define RTC_PORT 0
#define RTC_STARTUP_RETRY_MS 100
#include "../camera/src/webrtc.c"
#undef connect
#undef bind
#undef fcntl
#undef malloc

/* Faults are set before starting the worker and then owned by that worker. */
static int route_failures,bind_failure,fcntl_failure,frame_failures;
static int test_connect(int fd,const struct sockaddr *address,socklen_t len) {
    if(route_failures>0) { route_failures--; errno=ENETUNREACH; return -1; }
    return connect(fd,address,len);
}
static int test_bind(int fd,const struct sockaddr *address,socklen_t len) {
    if(bind_failure) { errno=EADDRINUSE; return -1; }
    return bind(fd,address,len);
}
static int test_fcntl(int fd,int command,...) {
    va_list args; va_start(args,command); int flags=va_arg(args,int); va_end(args);
    if(fcntl_failure) { errno=EIO; return -1; }
    return fcntl(fd,command,flags);
}
static void *test_malloc(size_t size) {
    if(size==AU_MAX && frame_failures>0) { frame_failures--; return NULL; }
    return malloc(size);
}
static int fd_count(void) {
    DIR *dir=opendir("/proc/self/fd"); assert(dir); int count=0;
    while(readdir(dir)) count++;
    closedir(dir); return count;
}
int main(void) {
    strcpy(transport.host,"127.0.0.1");
    int before=fd_count();
    for(int fault=0;fault<3;fault++) {
        for(int repeat=0;repeat<8;repeat++) {
            route_failures=fault==0; bind_failure=fault==1; fcntl_failure=fault==2;
            assert(!initialize()); assert(transport.fd==-1 && !transport.srtp_started);
            assert(!transport.lan[0] && !transport.public_ip[0]);
            assert(fd_count()==before); assert(!rtc_ready() && !rtc_create());
        }
    }
    route_failures=bind_failure=fcntl_failure=0;
    assert(initialize()); assert(transport.fd>=0 && transport.srtp_started);
    cleanup_initialization(); assert(fd_count()==before);
    puts("PASS: repeated route/bind/nonblocking failures release descriptors and crypto state");

    route_failures=3; frame_failures=1;
    uint64_t start=monotonic_ms();
    assert(rtc_init("127.0.0.1")); assert(rtc_init("127.0.0.1"));
    while(!rtc_ready() && monotonic_ms()-start<15000) {
        unsigned char frame[]={0,0,0,1,0x65,0x11};
        rtc_frame(frame,sizeof frame,1000000,1);
        char status[600]; rtc_status(status,sizeof status);
        assert(strstr(status,"\"ready\":"));
        poll(NULL,0,10);
    }
    assert(rtc_ready());
    assert(monotonic_ms()-start>=4*RTC_STARTUP_RETRY_MS);
    pthread_mutex_lock(&mu); assert(transport.startup_attempts==5); pthread_mutex_unlock(&mu);
    unsigned session=rtc_create(); assert(session);
    char offer[2048]; assert(rtc_offer(session,offer,sizeof offer));
    unsigned char key[32]; assert(!mbedtls_ctr_drbg_random(&transport.rng,key,sizeof key));
    srtp_policy_t policy={0}; srtp_t stream=NULL;
    srtp_crypto_policy_set_rtp_default(&policy.rtp); srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
    policy.ssrc.type=ssrc_any_outbound; policy.key=key;
    assert(srtp_create(&stream,&policy)==srtp_err_status_ok); srtp_dealloc(stream);
    poll(NULL,0,3*RTC_STARTUP_RETRY_MS);
    assert(rtc_alive(session));
    pthread_mutex_lock(&mu); assert(transport.startup_attempts==5); pthread_mutex_unlock(&mu);
    rtc_end(session);
    puts("PASS: timed network/frame-allocation recovery; one worker; no restart of healthy sessions");
    return 0;
}
