/* Bounded SDP negotiation for H264 and optional silent PCMU. Other media are rejected
 * in place, preserving m-line order and MID values. No SDP/secrets are logged. */
#ifndef P2V_WEBRTC_SDP_H
#define P2V_WEBRTC_SDP_H
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>

#define RTC_SDP_MAX 14000
struct rtc_sdp_transport { char ufrag[257], pwd[257], fingerprint[96], setup[16]; };
struct rtc_sdp_media {
    char type[16], protocol[32], formats[384], mid[33];
    unsigned port;
    int recv, mux, bundle_only;
    struct rtc_sdp_transport transport;
    unsigned char offered[128], h264[128], pcmu[128], mode[128], profile[128], nack[128], pli[128];
};
struct rtc_sdp { struct rtc_sdp_media media[4]; int count, video, payload, audio, audio_payload; unsigned char fingerprint[32]; };
static int rtc_sdp_copy(char *out,size_t cap,const char *in) {
    size_t n=strlen(in); if(*out || !n || n>=cap) return 0; memcpy(out,in,n+1); return 1;
}
static int rtc_sdp_token(const char *list,const char *word) {
    size_t n=strlen(word);
    for(const char *p=list;*p;) { while(*p==' ') p++; const char *end=strchr(p,' '); if(!end) end=p+strlen(p); if((size_t)(end-p)==n && !memcmp(p,word,n)) return 1; p=end; }
    return 0;
}
static int rtc_sdp_ice_string(const char *s,size_t min) {
    if(strlen(s)<min) return 0;
    for(;*s;s++) if(!isalnum((unsigned char)*s) && *s!='+' && *s!='/') return 0;
    return 1;
}
static int rtc_sdp_hex(char c) { if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return c-'a'+10; if(c>='A'&&c<='F') return c-'A'+10; return -1; }
static void rtc_sdp_fmtp(struct rtc_sdp_media *m,unsigned pt,char *params) {
    for(char *p=params;*p;) {
        while(*p==' ') p++; char *end=strchr(p,';'); if(!end) end=p+strlen(p);
        char *trim=end; while(trim>p && trim[-1]==' ') trim--;
        char save=*trim; *trim=0;
        if(!strcmp(p,"packetization-mode=1")) m->mode[pt]=1;
        /* Both constraint bytes identify constrained baseline. Require the
         * receiver to support at least level 3.1 for our 720p encoder. */
        if(!strncmp(p,"profile-level-id=",17) && strlen(p+17)==6) {
            const char *v=p+17; int level=rtc_sdp_hex(v[4])*16+rtc_sdp_hex(v[5]);
            if((!strncasecmp(v,"42e0",4)||!strncasecmp(v,"42c0",4)) && rtc_sdp_hex(v[4])>=0 && rtc_sdp_hex(v[5])>=0 && level>=31) m->profile[pt]=1;
        }
        *trim=save; p=*end?end+1:end;
    }
}
static int rtc_sdp_parse(const char *sdp,struct rtc_sdp *out) {
    memset(out,0,sizeof *out); out->video=out->payload=out->audio=out->audio_payload=-1;
    if(strlen(sdp)>RTC_SDP_MAX || strncmp(sdp,"v=0\r\n",5)) return 0;
    struct rtc_sdp_transport global={0}; struct rtc_sdp_media *m=NULL;
    char bundle[256]=""; int recv=1,timing=0;
    for(const char *p=sdp;*p;) {
        const char *end=strstr(p,"\r\n"); if(!end || end-p>1024) return 0;
        size_t n=end-p; char line[1025]; memcpy(line,p,n); line[n]=0; p=end+2;
        for(size_t i=0;i<n;i++) if((unsigned char)line[i]<32 || (unsigned char)line[i]>126) return 0;
        if(!strncmp(line,"m=",2)) {
            if(out->count==4) return 0; m=&out->media[out->count++]; m->recv=recv;
            int used=0;
            if(sscanf(line+2,"%15s %u %31s %383[^\r\n]%n",m->type,&m->port,m->protocol,m->formats,&used)!=4 || line[used+2] || m->port>65535) return 0;
            if(!strcmp(m->type,"video") || !strcmp(m->type,"audio")) {
                for(const char *q=m->formats;*q;) {
                    while(*q==' ') q++; if(!*q) break;
                    if(!isdigit((unsigned char)*q)) return 0;
                    char *after; unsigned long pt=strtoul(q,&after,10);
                    if(pt>127 || (*after && *after!=' ')) return 0; m->offered[pt]=1; q=after;
                }
            }
            continue;
        }
        struct rtc_sdp_transport *t=m?&m->transport:&global;
        if(!strncmp(line,"a=ice-ufrag:",12)) { if(!rtc_sdp_copy(t->ufrag,sizeof t->ufrag,line+12)) return 0; }
        else if(!strncmp(line,"a=ice-pwd:",10)) { if(!rtc_sdp_copy(t->pwd,sizeof t->pwd,line+10)) return 0; }
        else if(!strncmp(line,"a=fingerprint:",14)) { if(strncmp(line+14,"sha-256 ",8) || !rtc_sdp_copy(t->fingerprint,sizeof t->fingerprint,line+22)) return 0; }
        else if(!strncmp(line,"a=setup:",8)) { if(!rtc_sdp_copy(t->setup,sizeof t->setup,line+8)) return 0; }
        else if(!strcmp(line,"a=ice-lite")) return 0; /* Two ICE-lite peers cannot connect. */
        else if(!strncmp(line,"t=",2)) { if(m || timing++ || strcmp(line,"t=0 0")) return 0; }
        else if(!strncmp(line,"a=group:BUNDLE ",15)) { if(m || !rtc_sdp_copy(bundle,sizeof bundle,line+15)) return 0; }
        else if(!strcmp(line,"a=sendrecv") || !strcmp(line,"a=recvonly")) { if(m) m->recv=1; else recv=1; }
        else if(!strcmp(line,"a=sendonly") || !strcmp(line,"a=inactive")) { if(m) m->recv=0; else recv=0; }
        else if(m && !strncmp(line,"a=mid:",6)) {
            if(!rtc_sdp_copy(m->mid,sizeof m->mid,line+6)) return 0;
            for(const char *v=m->mid;*v;v++) if(!isalnum((unsigned char)*v) && *v!='_' && *v!='-' && *v!='.') return 0;
        }
        else if(m && !strcmp(line,"a=rtcp-mux")) m->mux=1;
        else if(m && !strcmp(line,"a=bundle-only")) m->bundle_only=1;
        else if(m && !strncmp(line,"a=rtpmap:",9)) {
            unsigned pt; char codec[64]; int used=0;
            if(sscanf(line+9,"%u %63s%n",&pt,codec,&used)==2 && !line[9+used] && pt<128 && !strcasecmp(codec,"H264/90000")) m->h264[pt]=1;
            if(sscanf(line+9,"%u %63s%n",&pt,codec,&used)==2 && !line[9+used] && pt<128 && (!strcasecmp(codec,"PCMU/8000") || !strcasecmp(codec,"PCMU/8000/1"))) m->pcmu[pt]=1;
        }
        else if(m && !strncmp(line,"a=fmtp:",7)) { unsigned pt; int used=0; if(sscanf(line+7,"%u %n",&pt,&used)==1 && used && pt<128) rtc_sdp_fmtp(m,pt,line+7+used); }
        else if(m && !strncmp(line,"a=rtcp-fb:",10)) {
            unsigned pt; int used=0;
            if(sscanf(line+10,"%u %n",&pt,&used)==1 && used && pt<128) {
                if(!strcmp(line+10+used,"nack")) m->nack[pt]=1;
                if(!strcmp(line+10+used,"nack pli")) m->pli[pt]=1;
            }
        }
    }
    if(!timing || !*bundle) return 0;
    for(int i=0;i<out->count;i++) {
        m=&out->media[i]; if(!*m->mid) return 0;
        for(int j=0;j<i;j++) if(!strcmp(m->mid,out->media[j].mid)) return 0;
    }
    for(int i=0;i<out->count;i++) {
        m=&out->media[i];
        if(strcmp(m->type,"video") || strcmp(m->protocol,"UDP/TLS/RTP/SAVPF") || (!m->port&&!m->bundle_only) || !m->recv || !m->mux || !rtc_sdp_token(bundle,m->mid)) continue;
        struct rtc_sdp_transport *t=&m->transport;
        if(!*t->ufrag) strcpy(t->ufrag,global.ufrag);
        if(!*t->pwd) strcpy(t->pwd,global.pwd);
        if(!*t->fingerprint) strcpy(t->fingerprint,global.fingerprint);
        if(!*t->setup) strcpy(t->setup,global.setup);
        if(!rtc_sdp_ice_string(t->ufrag,4) || !rtc_sdp_ice_string(t->pwd,22) || strlen(t->fingerprint)!=95 || (strcmp(t->setup,"actpass")&&strcmp(t->setup,"active"))) return 0;
        for(int k=0;k<32;k++) { int a=rtc_sdp_hex(t->fingerprint[3*k]),b=rtc_sdp_hex(t->fingerprint[3*k+1]); if(a<0||b<0||(k<31&&t->fingerprint[3*k+2]!=':')) return 0; out->fingerprint[k]=(a<<4)|b; }
        for(int pt=96;pt<128;pt++) if(m->offered[pt] && m->h264[pt] && m->mode[pt] && m->profile[pt]) { out->video=i; out->payload=pt; break; }
        if(out->video>=0) break;
    }
    if(out->video<0) return 0;
    /* One shared DTLS/ICE transport: only accept audio with the same identity
     * and credentials. Otherwise retain the working video-only behavior. */
    const struct rtc_sdp_transport *v=&out->media[out->video].transport;
    for(int i=0;i<out->count;i++) {
        m=&out->media[i];
        if(strcmp(m->type,"audio") || strcmp(m->protocol,"UDP/TLS/RTP/SAVPF") || (!m->port&&!m->bundle_only) || !m->recv || !m->mux || !rtc_sdp_token(bundle,m->mid)) continue;
        struct rtc_sdp_transport *t=&m->transport;
        if(!*t->ufrag) strcpy(t->ufrag,global.ufrag);
        if(!*t->pwd) strcpy(t->pwd,global.pwd);
        if(!*t->fingerprint) strcpy(t->fingerprint,global.fingerprint);
        if(!*t->setup) strcpy(t->setup,global.setup);
        if(strcmp(t->ufrag,v->ufrag) || strcmp(t->pwd,v->pwd) || strcasecmp(t->fingerprint,v->fingerprint) || strcmp(t->setup,v->setup)) continue;
        /* Require an explicit codec mapping, even for static payload 0. */
        for(int pt=0;pt<128;pt++) if((pt==0 || pt>=96) && pt!=out->payload && m->offered[pt] && m->pcmu[pt]) { out->audio=i; out->audio_payload=pt; break; }
        if(out->audio>=0) break;
    }
    return 1;
}
static int rtc_sdp_add(char *out,size_t cap,size_t *used,const char *fmt,...) {
    if(*used>=cap) return 0; va_list args; va_start(args,fmt); int n=vsnprintf(out+*used,cap-*used,fmt,args); va_end(args);
    if(n<0 || (size_t)n>=cap-*used) return 0; *used+=n; return 1;
}
static int rtc_sdp_answer(const struct rtc_sdp *s,char *out,size_t cap,uint32_t ssrc,uint32_t audio_ssrc,uint64_t created,const char *ufrag,const char *pwd,const char *fp,const char *lan,const char *public_ip,int port) {
    size_t used=0; int pt=s->payload;
#define SDP_ADD(...) do { if(!rtc_sdp_add(out,cap,&used,__VA_ARGS__)) return 0; } while(0)
    SDP_ADD("v=0\r\no=- %u %llu IN IP4 0.0.0.0\r\ns=Plug2View\r\nt=0 0\r\na=ice-lite\r\na=group:BUNDLE",ssrc,(unsigned long long)created);
    for(int i=0;i<s->count;i++) if(i==s->video || i==s->audio) SDP_ADD(" %s",s->media[i].mid);
    SDP_ADD("\r\na=msid-semantic: WMS plug2view\r\n");
    for(int i=0;i<s->count;i++) {
        const struct rtc_sdp_media *m=&s->media[i];
        if(i!=s->video && i!=s->audio) { SDP_ADD("m=%s 0 %s %s\r\nc=IN IP4 0.0.0.0\r\na=mid:%s\r\na=inactive\r\n",m->type,m->protocol,m->formats,m->mid); continue; }
        int audio=i==s->audio, payload=audio?s->audio_payload:pt; uint32_t source=audio?audio_ssrc:ssrc;
        SDP_ADD("m=%s 9 UDP/TLS/RTP/SAVPF %d\r\nc=IN IP4 0.0.0.0\r\na=mid:%s\r\na=sendonly\r\na=rtcp-mux\r\na=ice-ufrag:%s\r\na=ice-pwd:%s\r\na=fingerprint:sha-256 %s\r\na=setup:passive\r\na=rtpmap:%d %s\r\n",audio?"audio":"video",payload,m->mid,ufrag,pwd,fp,payload,audio?"PCMU/8000":"H264/90000");
        if(audio) SDP_ADD("a=ptime:20\r\n");
        else SDP_ADD("a=fmtp:%d packetization-mode=1;profile-level-id=42e01f\r\n",pt);
        SDP_ADD("a=ssrc:%u cname:plug2view\r\na=ssrc:%u msid:plug2view %s\r\na=msid:plug2view %s\r\n",source,source,audio?"audio":"video",audio?"audio":"video");
        if(!audio && m->nack[pt]) SDP_ADD("a=rtcp-fb:%d nack\r\n",pt);
        if(!audio && m->pli[pt]) SDP_ADD("a=rtcp-fb:%d nack pli\r\n",pt);
        SDP_ADD("a=candidate:1 1 UDP 2130706431 %s %d typ host\r\n",lan,port);
        if(*public_ip && strcmp(public_ip,lan)) SDP_ADD("a=candidate:2 1 UDP 1694498815 %s %d typ srflx raddr %s rport %d\r\n",public_ip,port,lan,port);
        SDP_ADD("a=end-of-candidates\r\n");
    }
#undef SDP_ADD
    return 1;
}
#endif
