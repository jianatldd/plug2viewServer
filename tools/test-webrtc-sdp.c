/* Host-side bounds/negotiation tests for the actual camera SDP parser. */
#include "../camera/src/webrtc-sdp.h"
#include <assert.h>
#define FP "01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01"
static const char *fixture=
 "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0 1 2\r\n"
 "a=ice-ufrag:abcd\r\na=ice-pwd:abcdefghijklmnopqrstuvwx\r\na=fingerprint:sha-256 " FP "\r\na=setup:actpass\r\n"
 "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:0\r\na=recvonly\r\na=rtpmap:111 opus/48000/2\r\n"
 "m=video 9 UDP/TLS/RTP/SAVPF 96 107\r\na=mid:1\r\na=recvonly\r\na=rtcp-mux\r\n"
 "a=rtpmap:96 VP8/90000\r\na=rtpmap:107 H264/90000\r\na=fmtp:107 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\na=rtcp-fb:107 nack\r\na=rtcp-fb:107 nack pli\r\n"
 "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\na=mid:2\r\na=sctp-port:5000\r\n";
static void changed(char *out,const char *from,const char *to) {
 const char *p=strstr(fixture,from); assert(p); size_t n=p-fixture;
 memcpy(out,fixture,n); strcpy(out+n,to); strcat(out,p+strlen(from));
}
int main(void) {
 struct rtc_sdp s; char answer[4096],input[15000];
 assert(rtc_sdp_parse(fixture,&s)); assert(s.count==3 && s.video==1 && s.payload==107);
 assert(!strcmp(s.media[1].transport.ufrag,"abcd"));
 assert(s.audio==-1 && s.audio_payload==-1);
 assert(rtc_sdp_answer(&s,answer,sizeof answer,123,124,456,"abcd","abcdefghijklmnopqrstuvwx",FP,"192.0.2.1","198.51.100.1",50000));
 const char *audio=strstr(answer,"m=audio 0"),*video=strstr(answer,"m=video 9 UDP/TLS/RTP/SAVPF 107"),*data=strstr(answer,"m=application 0");
 assert(audio && video && data && audio<video && video<data);
 assert(strstr(answer,"a=group:BUNDLE 1\r\n") && strstr(answer,"a=rtcp-fb:107 nack pli\r\n"));
 assert(!rtc_sdp_answer(&s,answer,20,123,124,456,"abcd","abcdefghijklmnopqrstuvwx",FP,"192.0.2.1","",50000));
 changed(input,"m=audio 9 UDP/TLS/RTP/SAVPF 111","m=audio 9 UDP/TLS/RTP/SAVPF 111 0\r\na=rtcp-mux\r\na=rtpmap:0 PCMU/8000");
 assert(rtc_sdp_parse(input,&s) && s.audio==0 && s.audio_payload==0);
 assert(rtc_sdp_answer(&s,answer,sizeof answer,123,124,456,"abcd","abcdefghijklmnopqrstuvwx",FP,"192.0.2.1","198.51.100.1",50000));
 assert(strstr(answer,"m=audio 9 UDP/TLS/RTP/SAVPF 0\r\n") && strstr(answer,"a=group:BUNDLE 0 1\r\n"));
 assert(strstr(answer,"a=ssrc:124 msid:plug2view audio\r\n") && strstr(answer,"a=ssrc:123 msid:plug2view video\r\n"));
 changed(input,"m=audio 9 UDP/TLS/RTP/SAVPF 111","m=audio 9 UDP/TLS/RTP/SAVPF 111 112\r\na=rtcp-mux\r\na=rtpmap:112 PCMU/8000/1");
 assert(rtc_sdp_parse(input,&s) && s.audio_payload==112);
 changed(input,"m=audio 9 UDP/TLS/RTP/SAVPF 111","m=audio 9 UDP/TLS/RTP/SAVPF 0\r\na=rtcp-mux\r\na=rtpmap:0 PCMU/16000");
 assert(rtc_sdp_parse(input,&s) && s.audio==-1);
 changed(input,"m=audio 9 UDP/TLS/RTP/SAVPF 111","m=audio 9 UDP/TLS/RTP/SAVPF 0\r\na=rtcp-mux\r\na=rtpmap:0 PCMU/8000\r\na=ice-ufrag:other");
 assert(rtc_sdp_parse(input,&s) && s.audio==-1);
 changed(input,"m=audio 9 UDP/TLS/RTP/SAVPF 111","m=audio 9 UDP/TLS/RTP/SAVPF 0\r\na=rtpmap:0 PCMU/8000");
 assert(rtc_sdp_parse(input,&s) && s.audio==-1);
 changed(input,"m=audio 9 UDP/TLS/RTP/SAVPF 111","m=audio 9 UDP/TLS/RTP/SAVPF 107\r\na=rtcp-mux\r\na=rtpmap:107 PCMU/8000");
 assert(rtc_sdp_parse(input,&s) && s.audio==-1); /* Collision with video payload. */
 changed(input,"a=mid:2","a=mid:1"); assert(!rtc_sdp_parse(input,&s));
 changed(input,"a=mid:1","a=mid:bad mid"); assert(!rtc_sdp_parse(input,&s));
 changed(input,"a=group:BUNDLE 0 1 2","a=group:BUNDLE 0 2"); assert(!rtc_sdp_parse(input,&s));
 changed(input,"profile-level-id=42e01f","profile-level-id=640c1f"); assert(!rtc_sdp_parse(input,&s));
 changed(input,"profile-level-id=42e01f","profile-level-id=42e01e"); assert(!rtc_sdp_parse(input,&s));
 changed(input,"packetization-mode=1","packetization-mode=10"); assert(!rtc_sdp_parse(input,&s));
 changed(input,"a=rtcp-mux\r\n",""); assert(!rtc_sdp_parse(input,&s));
 changed(input,"a=setup:actpass","a=setup:passive"); assert(!rtc_sdp_parse(input,&s));
 changed(input,"a=ice-ufrag:abcd","a=ice-ufrag:abcd\r\na=ice-ufrag:efgh"); assert(!rtc_sdp_parse(input,&s));
 changed(input,"a=setup:actpass","a=setup:actpass\r\na=ice-lite"); assert(!rtc_sdp_parse(input,&s));
 changed(input,FP,"ZZ:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01:01"); assert(!rtc_sdp_parse(input,&s));
 changed(input,"a=mid:1\r\na=recvonly","a=mid:1\r\na=sendonly"); assert(!rtc_sdp_parse(input,&s));
 changed(input,"m=video 9","m=video 0"); assert(!rtc_sdp_parse(input,&s));
 changed(input,"m=video 9 UDP/TLS/RTP/SAVPF 96 107","m=video 0 UDP/TLS/RTP/SAVPF 96 107\r\na=bundle-only"); assert(rtc_sdp_parse(input,&s));
 memset(input,'x',sizeof input-1); input[sizeof input-1]=0; assert(!rtc_sdp_parse(input,&s));
 puts("PASS: SDP media ordering, dynamic H264 payload, ICE inheritance, bounds and malformed-offer rejection");
 return 0;
}
