#ifndef P2V_WEBRTC_H
#define P2V_WEBRTC_H
#include <stddef.h>
#define RTC_MAX_SESSIONS 10
#ifdef P2V_WEBRTC
int rtc_init(const char *public_host);
int rtc_ready(void);
/* Reserve a slot. Opaque nonzero handles are not reused during this run. */
unsigned rtc_create(void);
int rtc_alive(unsigned session);
int rtc_full(void);
/* Returns 1 for a new offer, 2 for an active offer reused by a retry. */
int rtc_offer(unsigned session, char *sdp, size_t cap);
int rtc_answer(unsigned session, const char *sdp);
int rtc_accept_offer(unsigned session, const char *sdp, char *answer, size_t cap);
void rtc_end(unsigned session);
void rtc_end_all(void);
void rtc_status(char *out, size_t cap);
void rtc_frame(const unsigned char *data, size_t len, long long timestamp, int key);
#else
static inline int rtc_init(const char *host) { (void)host; return 0; }
static inline int rtc_ready(void) { return 0; }
static inline unsigned rtc_create(void) { return 0; }
static inline int rtc_alive(unsigned session) { (void)session; return 0; }
static inline int rtc_full(void) { return 0; }
static inline int rtc_offer(unsigned session, char *s, size_t n) { (void)session; (void)s; (void)n; return 0; }
static inline int rtc_answer(unsigned session, const char *s) { (void)session; (void)s; return 0; }
static inline int rtc_accept_offer(unsigned session, const char *s, char *a, size_t n) { (void)session; (void)s; (void)a; (void)n; return 0; }
static inline void rtc_end(unsigned session) { (void)session; }
static inline void rtc_end_all(void) {}
static inline void rtc_status(char *out, size_t cap) { if (cap > 2) { out[0]='{'; out[1]='}'; out[2]=0; } }
static inline void rtc_frame(const unsigned char *d, size_t n, long long t, int k) { (void)d; (void)n; (void)t; (void)k; }
#endif
#endif
