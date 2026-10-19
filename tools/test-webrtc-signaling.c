/* Actual signaling handlers with isolated media slots; no account linking or
 * live viewers. Pair with test-webrtc-sessions.c for the real media path. */
#undef NDEBUG
#include <assert.h>
#include "../camera/src/google.c"
static unsigned live[RTC_MAX_SESSIONS],next_handle;
static int fail_create;
int rtc_init(const char *host) { (void)host; return 1; }
int rtc_ready(void) { return 1; }
unsigned rtc_create(void) { if(fail_create) return 0; for(int i=0;i<RTC_MAX_SESSIONS;i++) if(!live[i]) return live[i]=++next_handle; return 0; }
int rtc_full(void) { for(int i=0;i<RTC_MAX_SESSIONS;i++) if(!live[i]) return 0; return 1; }
int rtc_alive(unsigned h) { for(int i=0;i<RTC_MAX_SESSIONS;i++) if(h && live[i]==h) return 1; return 0; }
void rtc_end(unsigned h) { for(int i=0;i<RTC_MAX_SESSIONS;i++) if(live[i]==h) live[i]=0; }
void rtc_end_all(void) { memset(live,0,sizeof live); }
int rtc_offer(unsigned h,char *out,size_t cap) { if(!rtc_alive(h)) return 0; snprintf(out,cap,"offer %u",h); return 1; }
int rtc_answer(unsigned h,const char *sdp) { return rtc_alive(h) && !strcmp(sdp,"answer"); }
int rtc_accept_offer(unsigned h,const char *sdp,char *out,size_t cap) { if(!rtc_alive(h) || strcmp(sdp,"offer")) return 0; snprintf(out,cap,"answer %u",h); return 1; }
void rtc_status(char *out,size_t cap) { snprintf(out,cap,"{}"); }
void rtc_frame(const unsigned char *d,size_t n,long long t,int k) { (void)d;(void)n;(void)t;(void)k; }
static struct google_response response;
static void stream_request(const char *id,int remote) {
    char body[1024];
    snprintf(body,sizeof body,"{\"requestId\":\"%s\",\"inputs\":[{\"intent\":\"action.devices.EXECUTE\",\"payload\":{\"commands\":[{\"devices\":[{\"id\":\"plug2view\"}],\"execution\":[{\"command\":\"action.devices.commands.GetCameraStream\",\"params\":{\"StreamToChromecast\":%s,\"SupportedStreamProtocols\":[\"webrtc\"]}}]}]}}]}",id,remote?"false":"true");
    memset(&response,0,sizeof response); response.code=200; fulfill_stream(body,1,&response);
}
static void signal(const char *token,const char *action) {
    char body[160],headers[180];
    snprintf(headers,sizeof headers,"Authorization: Bearer %s\r\n\r\n",token);
    snprintf(body,sizeof body,"{\"action\":\"%s\",\"deviceId\":\"plug2view\",\"sdp\":\"%s\"}",action,action);
    memset(&response,0,sizeof response); response.code=200; rtc_signaling("POST",body,headers,&response);
}
int main(void) {
    strcpy(g.origin,"https://camera.example");
    fail_create=1;
    google_viewer_webrtc("{\"action\":\"offer\",\"sdp\":\"offer\"}",&response);
    assert(response.code==503 && strstr(response.body,"stream_unavailable")); fail_create=0;
    google_viewer_webrtc("{\"action\":\"offer\",\"sdp\":\"bad\"}",&response); assert(response.code==400 && !live[0]);
    google_viewer_webrtc("{\"action\":\"offer\",\"sdp\":\"offer\"}",&response); assert(response.code==200);
    char viewer[65]; strcpy(viewer,g.rtc[0].token); unsigned viewer_handle=g.rtc[0].handle;
    stream_request("hub",0); assert(response.code==200 && !strstr(response.body,"cameraStreamOffer")); char hub[65]; strcpy(hub,g.rtc[1].token); unsigned hub_handle=g.rtc[1].handle;
    stream_request("hub",0); assert(response.code==200 && strstr(response.body,hub) && !live[2]);
    stream_request("phone",1); assert(response.code==200 && !strstr(response.body,"cameraStreamOffer")); char phone[65]; strcpy(phone,g.rtc[2].token);
    assert(strcmp(phone,hub) && strcmp(phone,viewer) && rtc_alive(viewer_handle));
    signal(hub,"offer"); assert(response.code==200 && strstr(response.body,"answer"));
    signal(hub,"offer"); assert(response.code==200);
    signal(phone,"offer"); assert(response.code==200);
    signal(hub,"answer"); assert(response.code==400 && rtc_alive(hub_handle));
    signal(viewer,"end"); assert(response.code==401 && rtc_alive(viewer_handle));
    char body[160]; snprintf(body,sizeof body,"{\"action\":\"end\",\"session\":\"%s\"}",hub);
    google_viewer_webrtc(body,&response); assert(response.code==409 && rtc_alive(hub_handle));
    for(int i=3;i<RTC_MAX_SESSIONS;i++) { char id[32]; snprintf(id,sizeof id,"viewer-%d",i); stream_request(id,i&1); assert(response.code==200); }
    stream_request("eleventh",0); assert(response.code==503);
    google_viewer_webrtc("{\"action\":\"offer\",\"sdp\":\"offer\"}",&response); assert(response.code==409);
    stream_request("hub",0); assert(response.code==200 && strstr(response.body,hub));
    signal(phone,"end"); assert(response.code==200 && rtc_alive(hub_handle) && rtc_alive(viewer_handle));
    stream_request("replacement",1); assert(response.code==200 && strcmp(g.rtc[2].token,phone));
    signal(phone,"end"); assert(response.code==401 && rtc_alive(g.rtc[2].handle));
    snprintf(body,sizeof body,"{\"action\":\"end\",\"session\":\"%s\"}",viewer);
    google_viewer_webrtc(body,&response); assert(response.code==200 && rtc_alive(hub_handle));
    google_viewer_webrtc("{\"action\":\"offer\",\"sdp\":\"offer\"}",&response); assert(response.code==200);
    google_viewer_webrtc(body,&response); assert(response.code==409 && rtc_alive(g.rtc[0].handle));
    rtc_end(hub_handle); stream_request("after-timeout",0); assert(response.code==200);
    signal(hub,"end"); assert(response.code==401);
    puts("PASS: independent browser/Hub/phone tokens, exact request retries, capacity, failure cleanup and stale-token isolation");
    return 0;
}
