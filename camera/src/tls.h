/* TLS layer for camd (BearSSL). See tls.c. */
#ifndef CAMD_TLS_H
#define CAMD_TLS_H
#include <stddef.h>

struct tlsconn;
struct tlsio;

int  tls_init(const char *cert_der_path, const char *key_der_path);   /* 0 on success */
int  tls_ready(void);
struct tlsconn *tls_accept(int fd);                                   /* new server-side session on an accepted socket */
void tls_free(struct tlsconn *t);
int  tls_pump(struct tlsconn *t, int can_read, int can_write);        /* -1 when finished */
int  tls_want_events(struct tlsconn *t);                              /* POLLIN/POLLOUT the engine needs */
int  tls_recv(struct tlsconn *t, void *buf, size_t n);                /* plaintext in, 0 = none yet */
int  tls_send(struct tlsconn *t, const void *buf, size_t n);          /* plaintext out, bytes accepted */
int  tls_closed(struct tlsconn *t);
void tls_close(struct tlsconn *t);                                    /* start close_notify; keep pumping until tls_flushed() */
int  tls_flushed(struct tlsconn *t);
int  tls_error(struct tlsconn *t);

/* blocking wrapper for forked children */
struct tlsio *tls_blocking(struct tlsconn *t);
int  tls_blocking_write(struct tlsio *b, const void *p, size_t n);
int  tls_blocking_read(struct tlsio *b, void *p, size_t n);
void tls_blocking_finish(struct tlsio *b);

#endif
