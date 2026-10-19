#ifndef P2V_WEBRTC_H
#define P2V_WEBRTC_H
#include <stddef.h>
#ifdef P2V_WEBRTC
int rtc_init(const char *public_host);
int rtc_ready(void);
/* Returns 1 for a new offer, 2 for an active offer reused by a retry. */
int rtc_offer(char *sdp, size_t cap, int retry);
int rtc_answer(const char *sdp);
int rtc_accept_offer(const char *sdp, char *answer, size_t cap);
void rtc_end(void);
void rtc_status(char *out, size_t cap);
void rtc_frame(const unsigned char *data, size_t len, long long timestamp, int key);
#else
static inline int rtc_init(const char *host) { (void)host; return 0; }
static inline int rtc_ready(void) { return 0; }
static inline int rtc_offer(char *s, size_t n, int retry) { (void)s; (void)n; (void)retry; return 0; }
static inline int rtc_answer(const char *s) { (void)s; return 0; }
static inline int rtc_accept_offer(const char *s, char *a, size_t n) { (void)s; (void)a; (void)n; return 0; }
static inline void rtc_end(void) {}
static inline void rtc_status(char *out, size_t cap) { if (cap > 2) { out[0]='{'; out[1]='}'; out[2]=0; } }
static inline void rtc_frame(const unsigned char *d, size_t n, long long t, int k) { (void)d; (void)n; (void)t; (void)k; }
#endif
#endif
