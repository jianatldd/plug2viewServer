/* H264 ICE-lite / DTLS-SRTP sender. One authenticated session, UDP 50000.
 * ICE-lite uses fixed LAN/public candidates: the public UDP port must be forwarded.
 * Google receives our offer; its answer authenticates the peer certificate.
 * Crypto/network work runs on a worker; capture only copies to a bounded queue.
 * Optional generated PCMU silence for receiver offers; no microphone capture.
 * No TURN, data channel, transcoding, media relay or disk writes.
 */
#include "webrtc.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/sha256.h"
#include "mbedtls/md.h"
#include "srtp.h"
#include "webrtc-sdp.h"

#define RTC_PORT 50000
#define AU_MAX (128 * 1024)
#define QUEUE 4
#define CACHE 128
#define MTU 1200
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER, qm = PTHREAD_MUTEX_INITIALIZER;
/* This 2.6.21 kernel lacks getrandom(); Mbed TLS's /dev/random fallback blocks
 * indefinitely. Use the same kernel CSPRNG device as the working HTTPS server. */
int mbedtls_hardware_poll(void *ctx, unsigned char *out, size_t len, size_t *olen) {
    (void)ctx; *olen=0; int fd=open("/dev/urandom",O_RDONLY); if(fd<0) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    while(*olen<len) { ssize_t n=read(fd,out+*olen,len-*olen); if(n<0 && errno==EINTR) continue; if(n<=0) { close(fd); return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED; } *olen+=n; }
    close(fd); return 0;
}
static struct {
    int started, ready, fd, active, answered, nominated, connected, keys;
    char host[256], lan[INET_ADDRSTRLEN], public_ip[INET_ADDRSTRLEN], fingerprint[96];
    char ufrag[17], pwd[49], remote_ufrag[257];
    unsigned char remote_fingerprint[32];
    char offer[2048];
    char local_answer[4096];
    int remote_offer, payload_type;
    int audio_payload;
    uint32_t audio_ssrc, audio_base_ts, audio_packets;
    uint16_t audio_seq;
    uint64_t audio_next, audio_start;
    long long audio_epoch;
    unsigned char answer_hash[32];
    unsigned udp_received, stun_received, stun_valid;
    struct sockaddr_in peer;
    uint64_t created, consent, last_report, timer_start, last_diagnostic;
    uint32_t timer_intermediate, timer_final, ssrc, base_ts, packets, octets;
    uint16_t seq;
    long long epoch;
    unsigned frames, rtcp_ok, rtcp_bad, pli, nack, retransmitted, cache_miss, send_errors, loss_fraction, jitter;
    int lost;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context rng;
    mbedtls_pk_context key;
    mbedtls_x509_crt cert;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    srtp_t out, in;
    unsigned char local_key[30], remote_key[30], incoming[2048];
    size_t incoming_len;
    struct { unsigned char data[MTU + 64]; int len; uint16_t seq; } cache[CACHE];
} r = {.fd = -1};
static struct {
    int enabled, need_key; unsigned head, count, dropped;
    struct { unsigned char data[AU_MAX]; size_t len; long long ts; } frames[QUEUE];
} q;

static uint64_t monotonic_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static unsigned u16(const unsigned char *p) { return (p[0] << 8) | p[1]; }
static uint32_t u32(const unsigned char *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | (p[2] << 8) | p[3]; }
static void w16(unsigned char *p, unsigned v) { p[0] = v >> 8; p[1] = v; }
static void w32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static int same_peer(const struct sockaddr_in *a, const struct sockaddr_in *b) { return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port; }
static int bytes_equal(const unsigned char *a, const unsigned char *b, size_t n) { unsigned d = 0; for (size_t i=0;i<n;i++) d |= a[i]^b[i]; return d == 0; }
static uint32_t crc32(const unsigned char *p, size_t n) { uint32_t v=~0U; while(n--) { v ^= *p++; for(int i=0;i<8;i++) v=(v>>1)^(0xedb88320U & (0U-(v&1))); } return ~v; }
static int random_hex(char *s, size_t bytes) {
    unsigned char b[32]; if(bytes>sizeof b || mbedtls_ctr_drbg_random(&r.rng,b,bytes)) return 0;
    for(size_t i=0;i<bytes;i++) sprintf(s+2*i,"%02x",b[i]); return 1;
}
static int sha1_hmac(const unsigned char *p, size_t n, unsigned char out[20]) {
    return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), (unsigned char *)r.pwd, strlen(r.pwd), p,n,out)==0;
}
static void queue_reset(int enabled) { pthread_mutex_lock(&qm); q.enabled=enabled; q.count=q.head=0; q.need_key=1; pthread_mutex_unlock(&qm); }
void rtc_frame(const unsigned char *d,size_t n,long long ts,int key) {
    pthread_mutex_lock(&qm);
    if(q.enabled) {
        if(n>AU_MAX || q.count==QUEUE) { q.count=0; q.need_key=1; q.dropped++; }
        if(n<=AU_MAX && (!q.need_key || key)) {
            unsigned i=(q.head+q.count)%QUEUE; memcpy(q.frames[i].data,d,n); q.frames[i].len=n; q.frames[i].ts=ts;
            q.count++; q.need_key=0;
        }
    }
    pthread_mutex_unlock(&qm);
}
static void stop_session(const char *reason) {
    if(r.active) fprintf(stderr,"webrtc: ended reason=%s frames=%u packets=%u udp=%u stun=%u valid=%u\n",reason,r.frames,r.packets,r.udp_received,r.stun_received,r.stun_valid);
    queue_reset(0);
    r.active=r.answered=r.nominated=r.connected=r.keys=0; r.incoming_len=0;
    if(r.out) { srtp_dealloc(r.out); r.out=NULL; } if(r.in) { srtp_dealloc(r.in); r.in=NULL; }
    mbedtls_ssl_free(&r.ssl); mbedtls_ssl_init(&r.ssl);
    memset(r.local_key,0,sizeof r.local_key); memset(r.remote_key,0,sizeof r.remote_key);
    memset(r.cache,0,sizeof r.cache);
}
void rtc_end(void) { pthread_mutex_lock(&mu); stop_session("signaling"); pthread_mutex_unlock(&mu); }
static void status_locked(char *out, size_t cap) {
    pthread_mutex_lock(&qm); unsigned dropped=q.dropped, queued=q.count; int waiting=q.need_key; pthread_mutex_unlock(&qm);
    snprintf(out,cap,"{\"active\":%d,\"connected\":%d,\"frames\":%u,\"packets\":%u,\"rtcp\":%u,\"rtcp_bad\":%u,\"pli\":%u,\"nack\":%u,\"resent\":%u,\"cache_miss\":%u,\"send_errors\":%u,\"lost\":%d,\"loss_fraction\":%u,\"jitter_ms\":%u,\"queue_drops\":%u,\"queued\":%u,\"waiting_key\":%d,\"age_ms\":%llu,\"consent_age_ms\":%llu}",r.active,r.connected,r.frames,r.packets,r.rtcp_ok,r.rtcp_bad,r.pli,r.nack,r.retransmitted,r.cache_miss,r.send_errors,r.lost,r.loss_fraction,r.jitter/90,dropped,queued,waiting,(unsigned long long)(r.active?monotonic_ms()-r.created:0),(unsigned long long)(r.nominated?monotonic_ms()-r.consent:0));
}
void rtc_status(char *out, size_t cap) { pthread_mutex_lock(&mu); status_locked(out,cap); pthread_mutex_unlock(&mu); }
static void timer_set(void *ctx,uint32_t intermediate,uint32_t final) { (void)ctx; r.timer_start=monotonic_ms(); r.timer_intermediate=intermediate; r.timer_final=final; }
static int timer_get(void *ctx) { (void)ctx; if(!r.timer_final) return -1; uint64_t elapsed=monotonic_ms()-r.timer_start; return elapsed>=r.timer_final?2:elapsed>=r.timer_intermediate?1:0; }
static int dtls_send(void *ctx,const unsigned char *p,size_t n) {
    (void)ctx; int v=sendto(r.fd,p,n,0,(struct sockaddr *)&r.peer,sizeof r.peer);
    return v<0 ? (errno==EAGAIN || errno==EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_INTERNAL_ERROR) : v;
}
static int dtls_recv(void *ctx,unsigned char *p,size_t n) {
    (void)ctx; if(!r.incoming_len) return MBEDTLS_ERR_SSL_WANT_READ;
    if(n<r.incoming_len) { r.incoming_len=0; return MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL; }
    size_t len=r.incoming_len; memcpy(p,r.incoming,len); r.incoming_len=0; return (int)len;
}
/* SDP supplies identity, not a public CA or hostname, for WebRTC DTLS. */
static int cert_verify(void *ctx,mbedtls_x509_crt *crt,int depth,uint32_t *flags) {
    (void)ctx; unsigned char hash[32];
    if(depth==0 && !mbedtls_sha256(crt->raw.p,crt->raw.len,hash,0) && bytes_equal(hash,r.remote_fingerprint,32)) *flags=0;
    else *flags=MBEDTLS_X509_BADCERT_NOT_TRUSTED;
    return 0;
}
static void export_keys(void *ctx,mbedtls_ssl_key_export_type type,const unsigned char *secret,size_t len,const unsigned char cr[32],const unsigned char sr[32],mbedtls_tls_prf_types prf) {
    (void)ctx; if(type!=MBEDTLS_SSL_KEY_EXPORT_TLS12_MASTER_SECRET) return;
    unsigned char randoms[64], material[60]; memcpy(randoms,cr,32); memcpy(randoms+32,sr,32);
    if(mbedtls_ssl_tls_prf(prf,secret,len,"EXTRACTOR-dtls_srtp",randoms,64,material,60)) return;
    /* DTLS server: server key/salt outbound, client key/salt inbound (RFC 5764). */
    memcpy(r.local_key,material+16,16); memcpy(r.local_key+16,material+46,14);
    memcpy(r.remote_key,material,16); memcpy(r.remote_key+16,material+32,14);
    memset(material,0,sizeof material); r.keys=1;
}
static int setup_srtp(void) {
    mbedtls_dtls_srtp_info info; mbedtls_ssl_get_dtls_srtp_negotiation_result(&r.ssl,&info);
    if(!r.keys || info.MBEDTLS_PRIVATE(chosen_dtls_srtp_profile)!=MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80 || !mbedtls_ssl_get_peer_cert(&r.ssl) || mbedtls_ssl_get_verify_result(&r.ssl)) return 0;
    srtp_policy_t p; memset(&p,0,sizeof p);
    srtp_crypto_policy_set_rtp_default(&p.rtp); srtp_crypto_policy_set_rtcp_default(&p.rtcp);
    p.ssrc.type=ssrc_specific; p.ssrc.value=r.ssrc; p.key=r.local_key; p.window_size=128;
    if(srtp_create(&r.out,&p)!=srtp_err_status_ok) return 0;
    if(r.audio_payload>=0) { p.ssrc.value=r.audio_ssrc; if(srtp_add_stream(r.out,&p)!=srtp_err_status_ok) return 0; }
    p.ssrc.type=ssrc_any_inbound; p.key=r.remote_key;
    if(srtp_create(&r.in,&p)!=srtp_err_status_ok) return 0;
    r.connected=1; queue_reset(1); fprintf(stderr,"webrtc: DTLS authenticated; SRTP connected\n"); return 1;
}
static void handshake_step(void) {
    if(!r.nominated || !r.answered || r.connected) return;
    int v=mbedtls_ssl_handshake(&r.ssl);
    if(v==0) { if(!setup_srtp()) stop_session("SRTP setup"); }
    else if(v!=MBEDTLS_ERR_SSL_WANT_READ && v!=MBEDTLS_ERR_SSL_WANT_WRITE) { fprintf(stderr,"webrtc: DTLS error=-0x%x\n",-v); stop_session("DTLS handshake"); }
}

/* Strict RFC 5389 bounds and integrity checks before changing ICE state.
 * Only a nominated, authenticated pair can deliver DTLS/RTCP. */
static void stun(unsigned char *b,size_t n,const struct sockaddr_in *from) {
    if(!r.answered || n<20 || u16(b)!=1 || u32(b+4)!=0x2112a442 || u16(b+2)+20!=n || (n&3)) return;
    size_t mi=0,fp=0; int user=0,nominate=0,controlling=0; char expected[280];
    snprintf(expected,sizeof expected,"%s:%s",r.ufrag,r.remote_ufrag);
    for(size_t pos=20;pos<n;) {
        if(n-pos<4) return; unsigned type=u16(b+pos),len=u16(b+pos+2); size_t end=pos+4+((len+3)&~3U);
        if(end>n) return;
        if(mi && type!=0x8028) return;
        if(type==6) { if(user || strlen(expected)!=len || memcmp(b+pos+4,expected,len)) return; user=1; }
        else if(type==8) { if(mi || len!=20) return; mi=pos; }
        else if(type==0x25) { if(len) return; nominate=1; }
        else if(type==0x802a) { if(len!=8) return; controlling=1; }
        else if(type==0x8028) { if(fp || len!=4 || end!=n) return; fp=pos; }
        pos=end;
    }
    if(!user || !mi || !controlling) return;
    if(fp && u32(b+fp+4)!=(crc32(b,fp)^0x5354554e)) return;
    unsigned char hash[20]; unsigned orig=u16(b+2); w16(b+2,mi+24-20);
    int valid=sha1_hmac(b,mi,hash) && bytes_equal(hash,b+mi+4,20); w16(b+2,orig); if(!valid) return;
    if(!r.stun_valid++) fprintf(stderr,"webrtc: first authenticated ICE check\n");
    if(r.nominated && !same_peer(from,&r.peer)) return;
    unsigned char out[64]; memset(out,0,sizeof out); w16(out,0x101); w32(out+4,0x2112a442); memcpy(out+8,b+8,12);
    w16(out+20,0x20); w16(out+22,8); out[25]=1; w16(out+26,ntohs(from->sin_port)^0x2112); w32(out+28,ntohl(from->sin_addr.s_addr)^0x2112a442);
    w16(out+32,8); w16(out+34,20); w16(out+2,36); if(!sha1_hmac(out,32,out+36)) return;
    w16(out+56,0x8028); w16(out+58,4); w16(out+2,44); w32(out+60,crc32(out,56)^0x5354554e);
    sendto(r.fd,out,sizeof out,0,(const struct sockaddr *)from,sizeof *from);
    if(nominate && !r.nominated) { r.peer=*from; r.nominated=1; fprintf(stderr,"webrtc: ICE nominated\n"); }
    if(r.nominated) r.consent=monotonic_ms();
}
static void send_packet(unsigned char *b,int len) {
    uint16_t seq=u16(b+2); int payload=len-12;
    if(srtp_protect(r.out,b,&len)!=srtp_err_status_ok) { r.send_errors++; return; }
    if(sendto(r.fd,b,len,0,(struct sockaddr *)&r.peer,sizeof r.peer)!=len) { r.send_errors++; return; }
    unsigned i=seq%CACHE; memcpy(r.cache[i].data,b,len); r.cache[i].len=len; r.cache[i].seq=seq;
    r.packets++; r.octets+=payload;
}
static void rtp_header(unsigned char *b,uint32_t ts,int last) { b[0]=0x80; b[1]=r.payload_type|(last?0x80:0); w16(b+2,r.seq++); w32(b+4,ts); w32(b+8,r.ssrc); }
static void send_nal(const unsigned char *p,size_t n,uint32_t ts,int last) {
    unsigned char b[MTU+64];
    if(n<=MTU-12) { rtp_header(b,ts,last); memcpy(b+12,p,n); send_packet(b,(int)n+12); return; }
    unsigned char h=*p++; n--; int first=1;
    while(n) { size_t chunk=n>MTU-14?MTU-14:n; int end=chunk==n;
        rtp_header(b,ts,last&&end); b[12]=(h&0xe0)|28; b[13]=(h&31)|(first?0x80:0)|(end?0x40:0);
        memcpy(b+14,p,chunk); send_packet(b,(int)chunk+14); p+=chunk; n-=chunk; first=0;
    }
}
static size_t start_code(const unsigned char *p,size_t n,size_t pos,size_t *skip) {
    for(;pos+3<=n;pos++) if(!p[pos] && !p[pos+1]) {
        if(p[pos+2]==1) { *skip=3; return pos; }
        if(pos+4<=n && !p[pos+2] && p[pos+3]==1) { *skip=4; return pos; }
    }
    *skip=0; return n;
}
static void send_frame(const unsigned char *p,size_t n,long long ts) {
    if(!r.epoch) r.epoch=ts;
    uint32_t timestamp=r.base_ts+(uint32_t)((ts-r.epoch)*9/100);
    size_t skip,pos=start_code(p,n,0,&skip);
    while(pos<n) { size_t begin=pos+skip,next_skip; size_t next=start_code(p,n,begin,&next_skip),end=next;
        while(end>begin && p[end-1]==0) end--;
        if(end>begin) send_nal(p+begin,end-begin,timestamp,next==n);
        pos=next; skip=next_skip;
    }
    r.frames++;
}
static void send_silence(uint64_t now) {
    if(r.audio_payload<0 || (r.audio_next && now<r.audio_next)) return;
    struct timeval tv; gettimeofday(&tv,NULL); long long us=(long long)tv.tv_sec*1000000+tv.tv_usec;
    if(!r.audio_epoch) { r.audio_epoch=us; r.audio_start=now; r.audio_next=now; }
    /* One 20 ms G.711 mu-law frame (0xff is digital silence). Pace using the
     * monotonic clock; skip missed intervals instead of sending a catch-up burst. */
    uint64_t skipped=(now-r.audio_next)/20;
    uint32_t ts=r.audio_base_ts+(uint32_t)((r.audio_next+skipped*20-r.audio_start)*8);
    r.audio_next+=(skipped+1)*20;
    unsigned char b[192]; b[0]=0x80; b[1]=r.audio_payload|(r.audio_packets?0:0x80);
    w16(b+2,r.audio_seq++); w32(b+4,ts); w32(b+8,r.audio_ssrc); memset(b+12,0xff,160);
    int len=172;
    if(srtp_protect(r.out,b,&len)!=srtp_err_status_ok || sendto(r.fd,b,len,0,(struct sockaddr *)&r.peer,sizeof r.peer)!=len) r.send_errors++;
    else r.audio_packets++;
}
static void sender_report_track(uint32_t ssrc,uint32_t base_ts,long long epoch,unsigned rate,uint32_t packets,uint32_t octets) {
    if(!epoch) return;
    struct timeval tv; gettimeofday(&tv,NULL); long long now=(long long)tv.tv_sec*1000000+tv.tv_usec;
    unsigned char b[128]; memset(b,0,sizeof b); b[0]=0x80; b[1]=200; w16(b+2,6); w32(b+4,ssrc);
    w32(b+8,(uint32_t)tv.tv_sec+2208988800U); w32(b+12,((uint64_t)tv.tv_usec<<32)/1000000);
    w32(b+16,base_ts+(uint32_t)((now-epoch)*rate/1000000)); w32(b+20,packets); w32(b+24,octets);
    b[28]=0x81; b[29]=202; w32(b+32,ssrc); b[36]=1; b[37]=9; memcpy(b+38,"plug2view",9);
    /* SDES chunk: header + SSRC + CNAME + END + padding = 20 bytes. */
    w16(b+30,4); int len=48;
    if(srtp_protect_rtcp(r.out,b,&len)==srtp_err_status_ok) sendto(r.fd,b,len,0,(struct sockaddr *)&r.peer,sizeof r.peer);
}
static void sender_report(void) {
    sender_report_track(r.ssrc,r.base_ts,r.epoch,90000,r.packets,r.octets);
    if(r.audio_payload>=0) sender_report_track(r.audio_ssrc,r.audio_base_ts,r.audio_epoch,8000,r.audio_packets,r.audio_packets*160);
}
static void retransmit(uint16_t seq) { unsigned i=seq%CACHE; if(r.cache[i].len && r.cache[i].seq==seq) { if(sendto(r.fd,r.cache[i].data,r.cache[i].len,0,(struct sockaddr *)&r.peer,sizeof r.peer)==r.cache[i].len) r.retransmitted++; else r.send_errors++; } else r.cache_miss++; }
static void rtcp(unsigned char *b,int n) {
    if(srtp_unprotect_rtcp(r.in,b,&n)!=srtp_err_status_ok) { r.rtcp_bad++; return; }
    r.rtcp_ok++;
    for(size_t pos=0;pos+4<=(size_t)n;) {
        size_t len=(u16(b+pos+2)+1)*4; if(len<4 || pos+len>(size_t)n || (b[pos]>>6)!=2) return;
        if(b[pos+1]==201) {
            for(unsigned i=0;i<(b[pos]&31);i++) {
                size_t report=pos+8+i*24; if(report+24>pos+len) break;
                if(u32(b+report)!=r.ssrc) continue;
                r.loss_fraction=b[report+4]; int lost=(b[report+5]<<16)|(b[report+6]<<8)|b[report+7];
                r.lost=(lost&0x800000)?lost-0x1000000:lost; r.jitter=u32(b+report+12);
            }
        }
        if(b[pos+1]==205 && (b[pos]&31)==1 && len>=16 && u32(b+pos+8)==r.ssrc) {
            r.nack++;
            for(size_t k=pos+12;k+4<=pos+len;k+=4) { uint16_t pid=u16(b+k),mask=u16(b+k+2); retransmit(pid); for(int bit=0;bit<16;bit++) if(mask&(1<<bit)) retransmit(pid+bit+1); }
        }
        if(b[pos+1]==206 && (b[pos]&31)==1 && len>=12) { /* Resume at the next periodic IDR. */
            r.pli++;
            pthread_mutex_lock(&qm); q.need_key=1; q.count=0; pthread_mutex_unlock(&qm);
        }
        pos+=len;
    }
}
static int make_certificate(void) {
    int ret; unsigned char der[1024], serial[16], digest[32]; mbedtls_x509write_cert crt; mbedtls_mpi number;
    mbedtls_x509write_crt_init(&crt); mbedtls_mpi_init(&number);
    ret=mbedtls_ctr_drbg_seed(&r.rng,mbedtls_entropy_func,&r.entropy,(unsigned char *)"plug2view-dtls",14);
    if(ret) goto done;
    if((ret=mbedtls_pk_setup(&r.key,mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)))) goto done;
    if((ret=mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,mbedtls_pk_ec(r.key),mbedtls_ctr_drbg_random,&r.rng))) goto done;
    if((ret=mbedtls_ctr_drbg_random(&r.rng,serial,sizeof serial))) goto done;
    serial[0]&=0x7f; serial[0]|=1; if((ret=mbedtls_mpi_read_binary(&number,serial,sizeof serial))) goto done;
    mbedtls_x509write_crt_set_version(&crt,MBEDTLS_X509_CRT_VERSION_3); mbedtls_x509write_crt_set_md_alg(&crt,MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt,&r.key); mbedtls_x509write_crt_set_issuer_key(&crt,&r.key);
    if((ret=mbedtls_x509write_crt_set_subject_name(&crt,"CN=Plug2View WebRTC")) || (ret=mbedtls_x509write_crt_set_issuer_name(&crt,"CN=Plug2View WebRTC")) || (ret=mbedtls_x509write_crt_set_serial(&crt,&number)) || (ret=mbedtls_x509write_crt_set_validity(&crt,"20260101000000","20360101000000"))) goto done;
    ret=mbedtls_x509write_crt_der(&crt,der,sizeof der,mbedtls_ctr_drbg_random,&r.rng); if(ret<0) goto done;
    ret=mbedtls_x509_crt_parse_der(&r.cert,der+sizeof der-ret,ret); if(ret) goto done;
    ret=mbedtls_sha256(r.cert.raw.p,r.cert.raw.len,digest,0); if(ret) goto done;
    for(int i=0;i<32;i++) sprintf(r.fingerprint+3*i,i==31?"%02X":"%02X:",digest[i]);
done: mbedtls_mpi_free(&number); mbedtls_x509write_crt_free(&crt); return ret==0;
}
static int initialize(void) {
    mbedtls_entropy_init(&r.entropy); mbedtls_ctr_drbg_init(&r.rng); mbedtls_pk_init(&r.key); mbedtls_x509_crt_init(&r.cert); mbedtls_ssl_config_init(&r.conf); mbedtls_ssl_init(&r.ssl);
    if(!make_certificate() || srtp_init()!=srtp_err_status_ok) return 0;
    if(mbedtls_ssl_config_defaults(&r.conf,MBEDTLS_SSL_IS_SERVER,MBEDTLS_SSL_TRANSPORT_DATAGRAM,MBEDTLS_SSL_PRESET_DEFAULT)) return 0;
    static const int ciphers[]={MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,0};
    static const mbedtls_ssl_srtp_profile profiles[]={MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80,MBEDTLS_TLS_SRTP_UNSET};
    mbedtls_ssl_conf_ciphersuites(&r.conf,ciphers); mbedtls_ssl_conf_rng(&r.conf,mbedtls_ctr_drbg_random,&r.rng);
    mbedtls_ssl_conf_authmode(&r.conf,MBEDTLS_SSL_VERIFY_REQUIRED); mbedtls_ssl_conf_verify(&r.conf,cert_verify,NULL);
    /* Required-verification mode needs a nonempty CA slot. The verification
     * callback above replaces CA identity with the exact SDP fingerprint. */
    mbedtls_ssl_conf_ca_chain(&r.conf,&r.cert,NULL);
    mbedtls_ssl_conf_cert_req_ca_list(&r.conf,MBEDTLS_SSL_CERT_REQ_CA_LIST_DISABLED);
    if(mbedtls_ssl_conf_own_cert(&r.conf,&r.cert,&r.key) || mbedtls_ssl_conf_dtls_srtp_protection_profiles(&r.conf,profiles)) return 0;
    mbedtls_ssl_conf_handshake_timeout(&r.conf,500,4000);
    /* Host address selected by the route to the public Internet. No data sent. */
    int probe=socket(AF_INET,SOCK_DGRAM,0); struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(53)}; inet_pton(AF_INET,"8.8.8.8",&a.sin_addr);
    socklen_t alen=sizeof a; if(probe<0) return 0;
    int ok=!connect(probe,(struct sockaddr *)&a,sizeof a) && !getsockname(probe,(struct sockaddr *)&a,&alen) && inet_ntop(AF_INET,&a.sin_addr,r.lan,sizeof r.lan); close(probe); if(!ok) return 0;
    struct addrinfo hint={.ai_family=AF_INET,.ai_socktype=SOCK_DGRAM},*ai=NULL;
    if(!getaddrinfo(r.host,NULL,&hint,&ai)) { inet_ntop(AF_INET,&((struct sockaddr_in *)ai->ai_addr)->sin_addr,r.public_ip,sizeof r.public_ip); freeaddrinfo(ai); }
    r.fd=socket(AF_INET,SOCK_DGRAM,0); if(r.fd<0) return 0;
    int size=256*1024; setsockopt(r.fd,SOL_SOCKET,SO_SNDBUF,&size,sizeof size);
    a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons(RTC_PORT);
    if(bind(r.fd,(struct sockaddr *)&a,sizeof a) || fcntl(r.fd,F_SETFL,O_NONBLOCK)<0) return 0;
    return 1;
}
static void *worker(void *unused) {
    (void)unused; int ok=initialize(); pthread_mutex_lock(&mu); r.ready=ok; pthread_mutex_unlock(&mu);
    fprintf(stderr,"webrtc: %s (UDP %d, ICE-lite, H264)\n",ok?"ready":"initialization failed",RTC_PORT); if(!ok) return NULL;
    unsigned char *frame=malloc(AU_MAX); if(!frame) { pthread_mutex_lock(&mu); r.ready=0; pthread_mutex_unlock(&mu); return NULL; }
    for(;;) {
        struct pollfd p={.fd=r.fd,.events=POLLIN}; poll(&p,1,5);
        pthread_mutex_lock(&mu);
        uint64_t now=monotonic_ms();
        if(r.active && (now-r.created>3600000 || (!r.connected && now-r.created>30000) || (r.connected && now-r.consent>30000))) stop_session("timeout");
        for(int i=0;i<16;i++) {
            unsigned char b[2048]; struct sockaddr_in from; socklen_t len=sizeof from;
            int n=recvfrom(r.fd,b,sizeof b,0,(struct sockaddr *)&from,&len); if(n<=0) break;
            if(!r.active) continue;
            r.udp_received++;
            if(n>=20 && b[0]<4 && u32(b+4)==0x2112a442) { r.stun_received++; stun(b,n,&from); }
            else if(r.nominated && same_peer(&from,&r.peer)) {
                if(b[0]>=20 && b[0]<=63) {
                    memcpy(r.incoming,b,n); r.incoming_len=n;
                    if(!r.connected) handshake_step();
                    else { unsigned char junk[2048]; int v=mbedtls_ssl_read(&r.ssl,junk,sizeof junk); if(v==MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) stop_session("peer closed"); }
                    r.incoming_len=0;
                } else if(r.connected && n>=8 && b[1]>=192 && b[1]<=223) rtcp(b,n);
            }
        }
        if(r.active && !r.connected) handshake_step();
        if(r.connected) {
            size_t n=0; long long ts=0; pthread_mutex_lock(&qm);
            if(q.count) { unsigned i=q.head; n=q.frames[i].len; ts=q.frames[i].ts; memcpy(frame,q.frames[i].data,n); q.head=(q.head+1)%QUEUE; q.count--; }
            pthread_mutex_unlock(&qm);
            if(n) send_frame(frame,n,ts);
            send_silence(now);
            if(now-r.last_report>=1000) { sender_report(); r.last_report=now; }
            if(now-r.last_diagnostic>=10000) { char status[600]; status_locked(status,sizeof status); fprintf(stderr,"webrtc: stats %s silent_audio_packets=%u\n",status,r.audio_packets); r.last_diagnostic=now; }
        }
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}
int rtc_init(const char *host) {
    if(r.started) return 1;
    if(!host || strlen(host)>=sizeof r.host) return 0;
    strcpy(r.host,host); pthread_t t; pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr,256*1024);
    int rc=pthread_create(&t,&attr,worker,NULL); pthread_attr_destroy(&attr); if(rc) { fprintf(stderr,"webrtc: pthread_create=%d\n",rc); return 0; }
    pthread_detach(t); r.started=1; return 1;
}
int rtc_ready(void) { pthread_mutex_lock(&mu); int ready=r.ready; pthread_mutex_unlock(&mu); return ready; }
static int start_session_locked(void) {
    /* Maintenance refreshes this atomically after a verified DuckDNS update.
     * Read only when starting a new session so in-flight SDP stays stable. */
    FILE *ipfile=fopen("/tmp/plug2view-public-ip","r");
    if(ipfile) {
        char ip[INET_ADDRSTRLEN+2];struct in_addr a;
        if(fgets(ip,sizeof ip,ipfile)) { ip[strcspn(ip,"\r\n")]=0;if(inet_pton(AF_INET,ip,&a)==1 && a.s_addr!=INADDR_ANY)strcpy(r.public_ip,ip); }
        fclose(ipfile);
    }
    stop_session("new session"); r.remote_offer=0; r.payload_type=96; r.audio_payload=-1;
    r.audio_packets=0; r.audio_epoch=0; r.audio_next=0;
    if(!random_hex(r.ufrag,8) || !random_hex(r.pwd,24) || mbedtls_ctr_drbg_random(&r.rng,(unsigned char *)&r.ssrc,sizeof r.ssrc) || mbedtls_ctr_drbg_random(&r.rng,(unsigned char *)&r.base_ts,sizeof r.base_ts) || mbedtls_ctr_drbg_random(&r.rng,(unsigned char *)&r.seq,sizeof r.seq)) return 0;
    if(mbedtls_ctr_drbg_random(&r.rng,(unsigned char *)&r.audio_base_ts,sizeof r.audio_base_ts) || mbedtls_ctr_drbg_random(&r.rng,(unsigned char *)&r.audio_seq,sizeof r.audio_seq)) return 0;
    r.audio_ssrc=r.ssrc^0x80000000U;
    if(mbedtls_ssl_setup(&r.ssl,&r.conf)) return 0;
    mbedtls_ssl_set_bio(&r.ssl,NULL,dtls_send,dtls_recv,NULL); mbedtls_ssl_set_timer_cb(&r.ssl,NULL,timer_set,timer_get); mbedtls_ssl_set_mtu(&r.ssl,MTU); mbedtls_ssl_set_export_keys_cb(&r.ssl,export_keys,NULL);
    r.created=r.consent=monotonic_ms(); r.last_diagnostic=r.created; r.last_report=0; r.epoch=0; r.packets=r.octets=r.frames=0; r.timer_final=0;
    r.rtcp_ok=r.rtcp_bad=r.pli=r.nack=r.retransmitted=r.cache_miss=r.send_errors=r.loss_fraction=r.jitter=0; r.lost=0;
    r.udp_received=r.stun_received=r.stun_valid=0;
    pthread_mutex_lock(&qm); q.dropped=0; pthread_mutex_unlock(&qm);
    return 1;
}
int rtc_offer(char *sdp,size_t cap,int retry) {
    pthread_mutex_lock(&mu); int ok=0;
    if(!r.ready) goto done;
    /* Google can overlap fulfillment calls while setting up one viewer. Keep
     * the pending ICE credentials stable; do not extend the setup deadline.
     * Exact request retries also preserve an established connection. */
    uint64_t age=monotonic_ms()-r.created;
    if(r.active && !r.remote_offer && age<3600000 && (r.connected ? monotonic_ms()-r.consent<30000 && (retry || age<30000) : age<30000)) {
        if(strlen(r.offer)+1>cap) goto done;
        strcpy(sdp,r.offer); ok=2; fprintf(stderr,"webrtc: offer reused answered=%d connected=%d\n",r.answered,r.connected); goto done;
    }
    if(!start_session_locked()) goto done;
    int n=snprintf(sdp,cap,"v=0\r\no=- %u %llu IN IP4 0.0.0.0\r\ns=Plug2View\r\nt=0 0\r\na=ice-lite\r\na=group:BUNDLE 0\r\na=msid-semantic: WMS plug2view\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\nc=IN IP4 0.0.0.0\r\na=mid:0\r\na=sendonly\r\na=rtcp-mux\r\na=ice-ufrag:%s\r\na=ice-pwd:%s\r\na=fingerprint:sha-256 %s\r\na=setup:passive\r\na=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1;profile-level-id=42e01f;level-asymmetry-allowed=1\r\na=rtcp-fb:96 nack\r\na=rtcp-fb:96 nack pli\r\na=ssrc:%u cname:plug2view\r\na=ssrc:%u msid:plug2view video\r\na=msid:plug2view video\r\na=candidate:1 1 UDP 2130706431 %s %d typ host\r\n",r.ssrc,(unsigned long long)r.created,r.ufrag,r.pwd,r.fingerprint,r.ssrc,r.ssrc,r.lan,RTC_PORT);
    if(n<0 || (size_t)n>=cap) goto done;
    if(*r.public_ip && strcmp(r.public_ip,r.lan)) { int k=snprintf(sdp+n,cap-n,"a=candidate:2 1 UDP 1694498815 %s %d typ srflx raddr %s rport %d\r\n",r.public_ip,RTC_PORT,r.lan,RTC_PORT); if(k<0 || (size_t)k>=cap-n) goto done; n+=k; }
    if((size_t)n+strlen("a=end-of-candidates\r\n")+1>cap) goto done; strcpy(sdp+n,"a=end-of-candidates\r\n");
    if(strlen(sdp)+1>sizeof r.offer) goto done;
    strcpy(r.offer,sdp); r.active=1; ok=1; fprintf(stderr,"webrtc: offer created\n");
done: pthread_mutex_unlock(&mu); return ok;
}
static int hex_digit(char c) { if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return c-'a'+10; if(c>='A'&&c<='F') return c-'A'+10; return -1; }
int rtc_answer(const char *sdp) {
    char uf[257]="",fp[96]=""; int media=0,active=0,recv=0,mux=0,codec=0,mode=0; size_t total=strlen(sdp);
    if(total>7000 || strncmp(sdp,"v=0\r\n",5)) return 0;
    for(const char *p=sdp;*p;) {
        const char *end=strstr(p,"\r\n"); if(!end || end-p>1024) return 0; size_t n=end-p;
        if(n>=2 && p[0]=='m' && p[1]=='=') { if(++media>1 || n<10 || strncmp(p,"m=video ",8) || !strncmp(p,"m=video 0 ",10)) return 0; }
        if(n>=12 && !strncmp(p,"a=ice-ufrag:",12)) { if(*uf || n-12<4 || n-12>256) return 0; memcpy(uf,p+12,n-12); uf[n-12]=0; }
        if(n>=22 && !strncmp(p,"a=fingerprint:sha-256 ",22)) { if(*fp || n-22!=95) return 0; memcpy(fp,p+22,95); fp[95]=0; }
        if(n==14 && !memcmp(p,"a=setup:active",14)) active=1;
        if(n==10 && !memcmp(p,"a=recvonly",10)) recv=1;
        if(n==10 && !memcmp(p,"a=rtcp-mux",10)) mux=1;
        if(n==22 && !memcmp(p,"a=rtpmap:96 H264/90000",22)) codec=1;
        if(n>=10 && !strncmp(p,"a=fmtp:96 ",10)) { char line[1025]; memcpy(line,p,n); line[n]=0; if(strstr(line,"packetization-mode=1")) mode=1; }
        p=end+2;
    }
    unsigned char fingerprint[32];
    if(!*uf || !*fp || !active || !recv || !mux || !codec || !mode || media!=1) { fprintf(stderr,"webrtc: answer rejected fields=%d%d%d%d%d%d%d\n",!!*uf,!!*fp,active,recv,mux,codec,mode); return 0; }
    for(int i=0;i<32;i++) { int a=hex_digit(fp[3*i]),b=hex_digit(fp[3*i+1]); if(a<0||b<0||(i<31&&fp[3*i+2]!=':')) return 0; fingerprint[i]=(a<<4)|b; }
    unsigned char hash[32]; if(mbedtls_sha256((const unsigned char *)sdp,total,hash,0)) return 0;
    pthread_mutex_lock(&mu); int ok=0;
    if(r.remote_offer) { pthread_mutex_unlock(&mu); return 0; }
    if(r.active && r.answered) {
        ok=bytes_equal(hash,r.answer_hash,sizeof hash);
        fprintf(stderr,"webrtc: repeated answer %s\n",ok?"accepted":"conflicts with active peer");
    } else if(r.active && monotonic_ms()-r.created<30000) {
        strcpy(r.remote_ufrag,uf); memcpy(r.remote_fingerprint,fingerprint,32); memcpy(r.answer_hash,hash,sizeof hash);
        r.answered=1; ok=1; fprintf(stderr,"webrtc: answer accepted\n");
    }
    pthread_mutex_unlock(&mu); return ok;
}

int rtc_accept_offer(const char *sdp,char *answer,size_t cap) {
    struct rtc_sdp parsed;
    if(!rtc_sdp_parse(sdp,&parsed)) { fprintf(stderr,"webrtc: unsupported remote offer\n"); return 0; }
    unsigned char hash[32]; if(mbedtls_sha256((const unsigned char *)sdp,strlen(sdp),hash,0)) return 0;
    pthread_mutex_lock(&mu); int ok=0;
    if(!r.ready) goto done;
    if(r.active) {
        if(!r.remote_offer || !bytes_equal(hash,r.answer_hash,32)) { fprintf(stderr,"webrtc: remote offer conflicts with active peer\n"); goto done; }
        if(strlen(r.local_answer)+1>cap) goto done;
        strcpy(answer,r.local_answer); ok=1; fprintf(stderr,"webrtc: remote offer retry accepted\n"); goto done;
    }
    if(!start_session_locked()) goto done;
    if(!rtc_sdp_answer(&parsed,r.local_answer,sizeof r.local_answer,r.ssrc,r.audio_ssrc,r.created,r.ufrag,r.pwd,r.fingerprint,r.lan,r.public_ip,RTC_PORT) || strlen(r.local_answer)+1>cap) goto done;
    struct rtc_sdp_media *video=&parsed.media[parsed.video];
    strcpy(r.remote_ufrag,video->transport.ufrag); memcpy(r.remote_fingerprint,parsed.fingerprint,32);
    memcpy(r.answer_hash,hash,32); r.payload_type=parsed.payload; r.audio_payload=parsed.audio_payload; r.remote_offer=r.answered=r.active=1;
    strcpy(answer,r.local_answer); ok=1;
    fprintf(stderr,"webrtc: remote offer accepted media=%d video_mid=%s payload=%d silent_pcmu=%d\n",parsed.count,video->mid,parsed.payload,parsed.audio_payload);
done: pthread_mutex_unlock(&mu); return ok;
}
