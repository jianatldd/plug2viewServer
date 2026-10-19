/* Isolated media/session regression test. Uses an ephemeral UDP port, never
 * the running camera daemon, encoder, credentials, or signaling endpoints. */
#undef NDEBUG
#include <assert.h>
#define RTC_PORT 0
#include "../camera/src/webrtc.c"

static const char *offer=
    "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0\r\n"
    "a=ice-ufrag:peer\r\na=ice-pwd:abcdefghijklmnopqrstuvwx\r\n"
    "a=fingerprint:sha-256 01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01\r\n"
    "a=setup:actpass\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:0\r\na=recvonly\r\na=rtcp-mux\r\n"
    "a=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1;profile-level-id=42e01f\r\n";
static size_t check_packet(struct rtc_session *r,unsigned char *b) {
    memset(b,0,512); w16(b,1); w32(b+4,0x2112a442);
    char user[280]; snprintf(user,sizeof user,"%s:%s",r->ufrag,r->remote_ufrag);
    size_t len=strlen(user),pos=24+((len+3)&~3U);
    w16(b+20,6); w16(b+22,len); memcpy(b+24,user,len);
    w16(b+pos,0x802a); w16(b+pos+2,8); pos+=12;
    w16(b+pos,0x25); pos+=4;
    w16(b+pos,8); w16(b+pos+2,20); w16(b+2,pos+24-20);
    assert(sha1_hmac(r,b,pos,b+pos+4)); return pos+24;
}
static srtp_t peer_rtp[RTC_MAX_SESSIONS],peer_rtcp[RTC_MAX_SESSIONS];
static int receivers[RTC_MAX_SESSIONS];
static void install_srtp(struct rtc_session *r,int i) {
    srtp_policy_t p={0};
    srtp_crypto_policy_set_rtp_default(&p.rtp); srtp_crypto_policy_set_rtcp_default(&p.rtcp);
    p.window_size=128; p.ssrc.type=ssrc_specific; p.ssrc.value=r->ssrc;
    memset(r->local_key,i+1,30); memset(r->remote_key,i+21,30);
    p.key=r->local_key; assert(srtp_create(&r->out,&p)==srtp_err_status_ok);
    p.ssrc.type=ssrc_any_inbound; assert(srtp_create(&peer_rtp[i],&p)==srtp_err_status_ok);
    p.key=r->remote_key; assert(srtp_create(&r->in,&p)==srtp_err_status_ok);
    p.ssrc.type=ssrc_specific; p.ssrc.value=1234; assert(srtp_create(&peer_rtcp[i],&p)==srtp_err_status_ok);
    r->connected=1; queue_membership();
}
static void receive_video(int i) {
    unsigned char b[1500]; int n=recv(receivers[i],b,sizeof b,0);
    assert(n>12); assert(srtp_unprotect(peer_rtp[i],b,&n)==srtp_err_status_ok);
    assert(u32(b+8)==sessions[i].ssrc && n==15 && b[12]==0x65);
}
int main(void) {
    strcpy(transport.host,"127.0.0.1"); assert(initialize()); transport.ready=1;
    unsigned ids[RTC_MAX_SESSIONS]; char answer[4096],retry[4096];
    for(int i=0;i<RTC_MAX_SESSIONS;i++) {
        ids[i]=rtc_create(); assert(ids[i]);
        assert(rtc_accept_offer(ids[i],offer,answer,sizeof answer));
        assert(rtc_accept_offer(ids[i],offer,retry,sizeof retry)); assert(!strcmp(answer,retry));
        assert(!rtc_offer(ids[i],retry,sizeof retry));
        if(i) assert(strcmp(sessions[i-1].ufrag,sessions[i].ufrag));
        receivers[i]=socket(AF_INET,SOCK_DGRAM,0); assert(receivers[i]>=0);
        struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
        assert(!bind(receivers[i],(struct sockaddr *)&a,sizeof a)); socklen_t alen=sizeof a;
        assert(!getsockname(receivers[i],(struct sockaddr *)&a,&alen));
        struct timeval timeout={.tv_sec=2}; setsockopt(receivers[i],SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
        unsigned char b[512]; size_t n=check_packet(&sessions[i],b);
        b[n-1]^=1; stun(&sessions[i],b,n,&a); assert(!sessions[i].nominated); b[n-1]^=1;
        for(int j=0;j<RTC_MAX_SESSIONS;j++) if(sessions[j].active) stun(&sessions[j],b,n,&a);
        assert(sessions[i].nominated && sessions[i].stun_valid==1);
        assert(recv(receivers[i],b,sizeof b,0)==64);
        install_srtp(&sessions[i],i);
    }
    assert(!rtc_create() && rtc_full()); assert(q.enabled==RTC_MAX_SESSIONS);
    /* Certificate verification and DTLS timers use their callback's owner. */
    assert(!mbedtls_sha256(transport.cert.raw.p,transport.cert.raw.len,sessions[0].remote_fingerprint,0));
    uint32_t flags=~0U; cert_verify(&sessions[0],&transport.cert,0,&flags); assert(!flags);
    cert_verify(&sessions[1],&transport.cert,0,&flags); assert(flags==MBEDTLS_X509_BADCERT_NOT_TRUSTED);
    timer_set(&sessions[0],100,200); timer_set(&sessions[1],0,0);
    assert(timer_get(&sessions[0])==0 && timer_get(&sessions[1])==-1);
    unsigned char frame[]={0,0,0,1,0x65,0x11,0x22};
    broadcast_frame(frame,sizeof frame,1000000,0);
    for(int i=0;i<RTC_MAX_SESSIONS;i++) assert(sessions[i].frames==0);
    broadcast_frame(frame,sizeof frame,1000000,1);
    for(int i=0;i<RTC_MAX_SESSIONS;i++) { receive_video(i); assert(sessions[i].frames==1); }
    /* A PLI holds only its owner until the next IDR; other peers keep moving. */
    unsigned char pli[64]={0x81,206,0,2}; w32(pli+4,1234); w32(pli+8,sessions[0].ssrc); int plen=12;
    assert(srtp_protect_rtcp(peer_rtcp[0],pli,&plen)==srtp_err_status_ok); rtcp(&sessions[0],pli,plen);
    assert(sessions[0].need_key && sessions[0].pli==1);
    broadcast_frame(frame,sizeof frame,1066666,0);
    assert(sessions[0].frames==1);
    for(int i=1;i<RTC_MAX_SESSIONS;i++) { receive_video(i); assert(sessions[i].frames==2); }
    /* Cleanup preserves the shared queue and all other DTLS/SRTP state. */
    rtc_frame(frame,sizeof frame,1100000,1); assert(q.count==1);
    rtc_end(ids[0]); assert(!rtc_alive(ids[0]) && q.count==1 && q.enabled==RTC_MAX_SESSIONS-1);
    assert(!rtc_full());
    unsigned replacement=rtc_create(); assert(replacement && replacement!=ids[0]);
    assert(q.count==1 && q.enabled==RTC_MAX_SESSIONS-1);
    rtc_end(ids[0]); assert(rtc_alive(replacement));
    assert(rtc_offer(replacement,answer,sizeof answer)==1);
    assert(rtc_offer(replacement,retry,sizeof retry)==2 && !strcmp(answer,retry));
    assert(!rtc_accept_offer(replacement,offer,retry,sizeof retry));
    sessions[0].created=monotonic_ms()-30001; assert(!rtc_alive(replacement));
    replacement=rtc_create(); assert(replacement && replacement!=ids[0]);
    sessions[1].consent=monotonic_ms()-30001; assert(!rtc_alive(ids[1]));
    for(int i=2;i<RTC_MAX_SESSIONS;i++) assert(rtc_alive(ids[i]));
    char status[600]; rtc_status(status,sizeof status); assert(strstr(status,"\"max_sessions\":10"));
    rtc_end_all(); assert(!q.enabled && !q.count);
    for(int i=0;i<RTC_MAX_SESSIONS;i++) { assert(!sessions[i].active && !sessions[i].cache); srtp_dealloc(peer_rtp[i]); srtp_dealloc(peer_rtcp[i]); close(receivers[i]); }
    close(transport.fd);
    puts("PASS: 10 sessions, ICE routing/integrity, independent SRTP fan-out, PLI isolation, retries, capacity, expiry and stale cleanup");
    return 0;
}
