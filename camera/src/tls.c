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

struct tls_config {
    unsigned char *cert_der; size_t cert_len,chain_len;
    br_x509_certificate chain[5];
    br_skey_decoder_context skey;
    const br_ec_private_key *eckey;
    br_ssl_session_cache_lru lru;
    unsigned char lru_buf[8192];
    unsigned refs;
};
static struct tls_config *active;
static void config_release(struct tls_config *c) { if(c && !--c->refs) { free(c->cert_der);free(c); } }

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
    struct tls_config *c=calloc(1,sizeof *c);if(!c)return -1;c->refs=1;
    size_t klen; unsigned char *key = read_file(key_path, &klen);
    c->cert_der = read_file(cert_path, &c->cert_len);
    if (!key || !c->cert_der) { fprintf(stderr, "tls: cannot read %s / %s\n", cert_path, key_path); free(key); goto bad; }
    br_skey_decoder_init(&c->skey);
    br_skey_decoder_push(&c->skey, key, klen);
    free(key);
    if (br_skey_decoder_last_error(&c->skey) != 0 || br_skey_decoder_key_type(&c->skey) != BR_KEYTYPE_EC) {
        fprintf(stderr, "tls: invalid EC private key\n");goto bad;
    }
    c->eckey = br_skey_decoder_get_ec(&c->skey);
    if(c->eckey->curve!=BR_EC_secp256r1)goto bad;
    /* cert.der may contain concatenated DER certificates, leaf first. Public CAs
     * require the intermediate chain; the original one-certificate file still works. */
    size_t pos = 0;
    while (pos < c->cert_len) {
        if (c->chain_len == 5 || c->cert_len - pos < 2 || c->cert_der[pos] != 0x30) goto bad;
        size_t head = 2, len = c->cert_der[pos + 1];
        if (len & 0x80) {
            unsigned count = len & 0x7f; len = 0;
            if (!count || count > 3 || c->cert_len - pos < 2 + count) goto bad;
            for (unsigned i = 0; i < count; i++) len = (len << 8) | c->cert_der[pos + 2 + i];
            head += count;
        }
        if (!len || len > c->cert_len - pos - head) goto bad;
        c->chain[c->chain_len].data = c->cert_der + pos; c->chain[c->chain_len++].data_len = head + len;
        pos += head + len;
    }
    br_x509_decoder_context dc; br_x509_decoder_init(&dc,NULL,NULL);
    br_x509_decoder_push(&dc,c->chain[0].data,c->chain[0].data_len);
    const br_x509_pkey *pk=br_x509_decoder_get_pkey(&dc);
    unsigned char pub[65];size_t plen=br_ec_get_default()->mulgen(pub,c->eckey->x,c->eckey->xlen,c->eckey->curve);
    if(!pk||pk->key_type!=BR_KEYTYPE_EC||pk->key.ec.curve!=c->eckey->curve||pk->key.ec.qlen!=plen||memcmp(pub,pk->key.ec.q,plen))goto bad;
    br_ssl_session_cache_lru_init(&c->lru, c->lru_buf, sizeof c->lru_buf);
    struct tls_config *old=active;active=c;config_release(old);
    fprintf(stderr, "tls: EC key and %zu-byte certificate loaded\n", c->cert_len);
    return 0;
bad:
    config_release(c);return -1;
}

int tls_ready(void) { return active!=NULL; }

struct tlsconn {
    br_ssl_server_context cc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    int fd;
    struct tls_config *config;
};

struct tlsconn *tls_accept(int fd) {
    if (!active) return NULL;
    struct tlsconn *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fd = fd;
    t->config=active;active->refs++;
    br_ssl_server_init_full_ec(&t->cc, active->chain, active->chain_len, BR_KEYTYPE_EC, active->eckey);
    br_ssl_engine_set_buffer(&t->cc.eng, t->iobuf, sizeof t->iobuf, 1);
    br_ssl_server_set_cache(&t->cc, &active->lru.vtable);
    /* extra entropy for the engine's PRNG from the kernel */
    unsigned char seed[32]; int r = open("/dev/urandom", O_RDONLY);
    if (r >= 0) { if (read(r, seed, sizeof seed) == (ssize_t)sizeof seed) br_ssl_engine_inject_entropy(&t->cc.eng, seed, sizeof seed); close(r); }
    br_ssl_server_reset(&t->cc);
    return t;
}

void tls_free(struct tlsconn *t) { if(t)config_release(t->config);free(t); }

int tls_closed(struct tlsconn *t) { return (br_ssl_engine_current_state(&t->cc.eng) & BR_SSL_CLOSED) != 0; }
int tls_has_input(struct tlsconn *t) { return (br_ssl_engine_current_state(&t->cc.eng) & BR_SSL_RECVAPP) != 0; }
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
