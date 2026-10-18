/*
 * tls.c - HTTPS for camd using BearSSL (https://bearssl.org, MIT licence), built as a
 * static library for mipsel with zig cc (see tools/build.js). The camera keeps plain HTTP
 * on its LAN port and additionally serves the same thing over TLS on TLS_PORT, which is
 * the port to forward from the router.
 *
 * Non-blocking model: the BearSSL engine exposes four buffers (encrypted in/out, plain
 * in/out). tls_pump() moves encrypted bytes between the socket and the engine, the HTTP
 * code reads/writes plaintext through tls_recv()/tls_send(), and tls_want_events() tells
 * the poll loop what the engine is waiting for. Forked children (file downloads, API
 * proxy) inherit the engine state and use BearSSL's blocking sslio wrapper.
 *
 * Key: EC P-256, PKCS#8 DER. Certificate: X.509 DER (self-signed is fine; the browser
 * warns once, or the cert can be installed as trusted on the viewing devices).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include "bearssl/inc/bearssl.h"
#include "tls.h"

static unsigned char *cert_der = NULL; static size_t cert_len = 0;
static br_x509_certificate chain[1];
static br_skey_decoder_context skey;
static const br_ec_private_key *eckey = NULL;
static br_ssl_session_cache_lru lru;
static unsigned char lru_buf[8192];
static int ready = 0;

static unsigned char *read_file(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    off_t n = lseek(fd, 0, SEEK_END); lseek(fd, 0, SEEK_SET);
    if (n <= 0 || n > 65536) { close(fd); return NULL; }
    unsigned char *b = malloc(n);
    if (!b || read(fd, b, n) != n) { close(fd); free(b); return NULL; }
    close(fd); *len = n;
    return b;
}

int tls_init(const char *cert_path, const char *key_path) {
    size_t klen; unsigned char *key = read_file(key_path, &klen);
    cert_der = read_file(cert_path, &cert_len);
    if (!key || !cert_der) { fprintf(stderr, "tls: cannot read %s / %s\n", cert_path, key_path); free(key); return -1; }
    br_skey_decoder_init(&skey);
    br_skey_decoder_push(&skey, key, klen);
    free(key);
    if (br_skey_decoder_last_error(&skey) != 0 || br_skey_decoder_key_type(&skey) != BR_KEYTYPE_EC) {
        fprintf(stderr, "tls: key must be an EC (P-256) private key in PKCS#8 DER (error %d)\n", br_skey_decoder_last_error(&skey));
        return -1;
    }
    eckey = br_skey_decoder_get_ec(&skey);
    chain[0].data = cert_der; chain[0].data_len = cert_len;
    br_ssl_session_cache_lru_init(&lru, lru_buf, sizeof lru_buf);
    ready = 1;
    fprintf(stderr, "tls: EC key and %zu-byte certificate loaded\n", cert_len);
    return 0;
}

int tls_ready(void) { return ready; }

struct tlsconn {
    br_ssl_server_context cc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    int fd;
};

struct tlsconn *tls_accept(int fd) {
    if (!ready) return NULL;
    struct tlsconn *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fd = fd;
    br_ssl_server_init_full_ec(&t->cc, chain, 1, BR_KEYTYPE_EC, eckey);
    br_ssl_engine_set_buffer(&t->cc.eng, t->iobuf, sizeof t->iobuf, 1);
    br_ssl_server_set_cache(&t->cc, &lru.vtable);
    /* extra entropy for the engine's PRNG from the kernel */
    unsigned char seed[32]; int r = open("/dev/urandom", O_RDONLY);
    if (r >= 0) { if (read(r, seed, sizeof seed) == (ssize_t)sizeof seed) br_ssl_engine_inject_entropy(&t->cc.eng, seed, sizeof seed); close(r); }
    br_ssl_server_reset(&t->cc);
    return t;
}

void tls_free(struct tlsconn *t) { free(t); }

int tls_closed(struct tlsconn *t) { return (br_ssl_engine_current_state(&t->cc.eng) & BR_SSL_CLOSED) != 0; }
void tls_close(struct tlsconn *t) { br_ssl_engine_close(&t->cc.eng); }
int tls_flushed(struct tlsconn *t) { unsigned st = br_ssl_engine_current_state(&t->cc.eng); return (st & BR_SSL_CLOSED) || !(st & BR_SSL_SENDREC); }
int tls_error(struct tlsconn *t) { return br_ssl_engine_last_error(&t->cc.eng); }

/* Move encrypted bytes socket <-> engine. Call on POLLIN/POLLOUT. Returns -1 when the
 * connection is finished (closed or error), 0 otherwise. */
int tls_pump(struct tlsconn *t, int can_read, int can_write) {
    br_ssl_engine_context *eng = &t->cc.eng;
    for (int loop = 0; loop < 8; loop++) {
        unsigned st = br_ssl_engine_current_state(eng);
        if (st & BR_SSL_CLOSED) return -1;
        int progressed = 0;
        if (can_write && (st & BR_SSL_SENDREC)) {
            size_t len; unsigned char *buf = br_ssl_engine_sendrec_buf(eng, &len);
            ssize_t w = write(t->fd, buf, len);
            if (w > 0) { br_ssl_engine_sendrec_ack(eng, w); progressed = 1; }
            else if (w < 0 && errno != EAGAIN && errno != EINTR) return -1;
            else can_write = 0;
        }
        if (can_read && (st & BR_SSL_RECVREC)) {
            size_t len; unsigned char *buf = br_ssl_engine_recvrec_buf(eng, &len);
            ssize_t r = read(t->fd, buf, len);
            if (r > 0) { br_ssl_engine_recvrec_ack(eng, r); progressed = 1; }
            else if (r == 0) return -1;
            else if (errno != EAGAIN && errno != EINTR) return -1;
            else can_read = 0;
        }
        if (!progressed) break;
    }
    return (br_ssl_engine_current_state(eng) & BR_SSL_CLOSED) ? -1 : 0;
}

/* Which socket events the engine needs next. */
int tls_want_events(struct tlsconn *t) {
    unsigned st = br_ssl_engine_current_state(&t->cc.eng);
    int ev = 0;
    if (st & BR_SSL_SENDREC) ev |= POLLOUT;
    if (st & BR_SSL_RECVREC) ev |= POLLIN;
    return ev;
}

/* Plaintext received from the peer (0 = nothing available yet). */
int tls_recv(struct tlsconn *t, void *buf, size_t n) {
    br_ssl_engine_context *eng = &t->cc.eng;
    if (!(br_ssl_engine_current_state(eng) & BR_SSL_RECVAPP)) return 0;
    size_t len; unsigned char *src = br_ssl_engine_recvapp_buf(eng, &len);
    if (len > n) len = n;
    memcpy(buf, src, len);
    br_ssl_engine_recvapp_ack(eng, len);
    return (int)len;
}

/* Plaintext to send: copies into the engine's buffer, returns bytes accepted (0 = full,
 * try again after a pump). */
int tls_send(struct tlsconn *t, const void *buf, size_t n) {
    br_ssl_engine_context *eng = &t->cc.eng;
    if (!(br_ssl_engine_current_state(eng) & BR_SSL_SENDAPP)) return 0;
    size_t len; unsigned char *dst = br_ssl_engine_sendapp_buf(eng, &len);
    if (len > n) len = n;
    memcpy(dst, buf, len);
    br_ssl_engine_sendapp_ack(eng, len);
    br_ssl_engine_flush(eng, 0);
    return (int)len;
}

/* ---- blocking helpers for forked children (they own the socket from here on) ------- */
static int sock_read(void *ctx, unsigned char *buf, size_t len) {
    for (;;) { ssize_t r = read(*(int *)ctx, buf, len); if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; } return (int)r; }
}
static int sock_write(void *ctx, const unsigned char *buf, size_t len) {
    for (;;) { ssize_t w = write(*(int *)ctx, buf, len); if (w <= 0) { if (w < 0 && errno == EINTR) continue; return -1; } return (int)w; }
}

struct tlsio { br_sslio_context io; int fd; };

struct tlsio *tls_blocking(struct tlsconn *t) {
    struct tlsio *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->fd = t->fd;
    fcntl(t->fd, F_SETFL, 0);
    br_sslio_init(&b->io, &t->cc.eng, sock_read, &b->fd, sock_write, &b->fd);
    return b;
}
int tls_blocking_write(struct tlsio *b, const void *p, size_t n) { return br_sslio_write_all(&b->io, p, n); }
int tls_blocking_read(struct tlsio *b, void *p, size_t n) { return br_sslio_read(&b->io, p, n); }
void tls_blocking_finish(struct tlsio *b) { br_sslio_flush(&b->io); br_sslio_close(&b->io); }
