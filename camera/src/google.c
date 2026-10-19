/* Personal, single-camera OAuth authorization-code integration. Disabled unless -g
 * supplies a complete config. Google URLs require HTTPS. Stream capabilities are
 * HMAC authenticated, expire after an hour, and authorize /hls/ only. Passwords,
 * authorization codes and tokens are never logged. */
#include "google.h"
#include "webrtc.h"
#include "bearssl/inc/bearssl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <stdarg.h>
static google_control_fn control_callback;
void google_set_controls(google_control_fn callback) { control_callback=callback; }

static struct {
    int enabled;
    char project[128], origin[256], client[128], secret[128], user[128], password[128];
    char key[65], generation[65], statefile[512];
    char rtc_token[65];
    char rtc_request[260];
    time_t rtc_exp, rtc_created;
    int rtc_remote, rtc_viewer;
    char nonce[65], redirect[256], state[GOOGLE_STATE_MAX + 1], code[65];
    time_t flow_exp, code_exp, retry_at;
    unsigned failures;
} g;

static int equal(const char *a, const char *b) {
    size_t al = strlen(a), bl = strlen(b); unsigned diff = (unsigned)(al ^ bl);
    for (size_t i = 0; i < al; i++) diff |= (unsigned char)a[i] ^ (i < bl ? (unsigned char)b[i] : 0);
    return diff == 0;
}
static int random_hex(char out[65]) {
    unsigned char b[32]; FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return 0;
    size_t n = fread(b, 1, sizeof b, f); fclose(f);
    if (n != sizeof b) return 0;
    for (unsigned i = 0; i < sizeof b; i++) sprintf(out + i * 2, "%02x", b[i]);
    return 1;
}
static int ishex(const char *s) {
    if (strlen(s) != 64) return 0;
    for (; *s; s++) if (!isxdigit((unsigned char)*s)) return 0;
    return 1;
}
static void mac(const char *s, char out[65]) {
    br_hmac_key_context k; br_hmac_context h; unsigned char b[32];
    br_hmac_key_init(&k, &br_sha256_vtable, g.key, strlen(g.key));
    br_hmac_init(&h, &k, 32); br_hmac_update(&h, s, strlen(s)); br_hmac_out(&h, b);
    for (unsigned i = 0; i < sizeof b; i++) sprintf(out + 2 * i, "%02x", b[i]);
}
static void token(char type, unsigned long expiry, char out[192]) {
    char message[256], digest[65];
    snprintf(message, sizeof message, "%c.%lu.%s", type, expiry, g.generation);
    mac(message, digest); snprintf(out, 192, "%c.%lu.%s", type, expiry, digest);
}
static int valid_token(const char *s, char type) {
    unsigned long exp; char hash[65], tail; char expected[192];
    if (!g.enabled || strlen(s) > 100 || sscanf(s, "%*c.%lu.%64[0123456789abcdef]%c", &exp, hash, &tail) != 2 || s[0] != type || strlen(hash) != 64) return 0;
    if (type != 'r' && (exp < (unsigned long)time(NULL) || exp > (unsigned long)time(NULL) + 3600)) return 0;
    if (type == 'r' && exp != 0) return 0;
    token(type, exp, expected); return equal(s, expected);
}
static int rotate_generation(void) {
    char next[65], tmp[540]; if (!random_hex(next)) return 0;
    snprintf(tmp, sizeof tmp, "%s.tmp", g.statefile);
    FILE *f = fopen(tmp, "w"); if (!f) return 0;
    int ok = fprintf(f, "%s\n", next) == 65;
    if (fclose(f)) ok = 0;
    chmod(tmp, 0600);
    if (!ok || rename(tmp, g.statefile)) { remove(tmp); return 0; }
    strcpy(g.generation, next); g.rtc_token[0] = 0; rtc_end(); return 1;
}
int google_init(const char *config) {
    memset(&g, 0, sizeof g); if (!config) return 0;
    FILE *f = fopen(config, "r"); if (!f) return 0;
    struct entry { const char *name; char *value; size_t cap; } entries[] = {
        {"PROJECT_ID", g.project, sizeof g.project}, {"PUBLIC_ORIGIN", g.origin, sizeof g.origin},
        {"CLIENT_ID", g.client, sizeof g.client}, {"CLIENT_SECRET", g.secret, sizeof g.secret},
        {"LOGIN_USER", g.user, sizeof g.user}, {"LOGIN_PASSWORD", g.password, sizeof g.password},
        {"TOKEN_KEY", g.key, sizeof g.key}
    };
    char line[512]; int ok = 1;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0; if (!*line || line[0] == '#') continue;
        char *eq = strchr(line, '='); if (!eq) { ok = 0; break; } *eq++ = 0;
        int found = 0;
        for (unsigned i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(line, entries[i].name)) {
            if (strlen(eq) >= entries[i].cap || *entries[i].value) ok = 0;
            else strcpy(entries[i].value, eq);
            found = 1; break;
        }
        if (!found) ok = 0;
    }
    fclose(f);
    for (unsigned i = 0; i < sizeof entries / sizeof *entries; i++) if (!*entries[i].value) ok = 0;
    for (const char *p = g.project; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-') ok = 0;
    if (strncmp(g.origin, "https://", 8) || !g.origin[8] || strspn(g.origin + 8, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:") != strlen(g.origin + 8)) ok = 0;
    /* Permit a manually chosen short password for this personal test setup.
     * The config generator still creates a strong random password by default. */
    if (!ishex(g.key) || strlen(g.secret) < 32 || strlen(g.password) < 6) ok = 0;
    if (!ok || strlen(config) + 7 >= sizeof g.statefile) { fprintf(stderr, "google: invalid config; disabled\n"); return 0; }
    snprintf(g.statefile, sizeof g.statefile, "%s.state", config);
    f = fopen(g.statefile, "r");
    if (f) { if (!fgets(g.generation, sizeof g.generation, f)) g.generation[0] = 0; fclose(f); }
    if (!ishex(g.generation) && !rotate_generation()) { fprintf(stderr, "google: cannot save token state; disabled\n"); return 0; }
    g.enabled = 1;
    char host[256]; snprintf(host, sizeof host, "%s", g.origin + 8); char *colon = strchr(host, ':'); if (colon) *colon = 0;
    rtc_init(host);
    fprintf(stderr, "google: experimental camera integration enabled\n"); return 1;
}
int google_enabled(void) { return g.enabled; }
int google_header(const char *headers, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name); out[0] = 0; int found = 0;
    for (const char *p = headers; p && *p; ) {
        const char *end = strstr(p, "\r\n"); if (!end || end == p) break;
        if ((size_t)(end - p) > nl && !strncasecmp(p, name, nl) && p[nl] == ':') {
            if (found++) { out[0] = 0; return 0; }
            p += nl + 1; while (p < end && (*p == ' ' || *p == '\t')) p++;
            size_t n = end - p; if (n >= cap) return 0;
            memcpy(out, p, n); out[n] = 0;
        }
        p = end + 2;
    }
    return found;
}
static int unhex(int c) { if (c >= '0' && c <= '9') return c - '0'; c = tolower(c); return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
int google_param(const char *form, const char *key, char *out, size_t cap) {
    out[0] = 0; size_t kl = strlen(key); int found = 0;
    for (const char *p = form; p && *p; ) {
        const char *end = strchr(p, '&'); if (!end) end = p + strlen(p);
        if ((size_t)(end - p) > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            if (found++) return 0; size_t n = 0;
            for (const char *s = p + kl + 1; s < end; s++) {
                int c = (unsigned char)*s;
                if (c == '%') { if (s + 2 >= end || unhex(s[1]) < 0 || unhex(s[2]) < 0) return 0; c = unhex(s[1]) * 16 + unhex(s[2]); s += 2; }
                else if (c == '+') c = ' ';
                if (c < 32 || c == 127 || n + 1 >= cap) return 0;
                out[n++] = c;
            }
            out[n] = 0;
        }
        p = *end ? end + 1 : end;
    }
    return found;
}
static void encode(const char *s, char *out) {
    while (*s) { unsigned char c = *s++; if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') *out++ = c; else { sprintf(out, "%%%02X", c); out += 3; } } *out = 0;
}
int google_stream_token(const char *query, char *out, size_t cap) { return google_param(query, "token", out, cap) && valid_token(out, 's'); }

/* Bounded JSON parser. Tokens retain exact JSON spelling so requestId can be echoed
 * without interpolation or unescaping. Reject malformed JSON, excess nesting and
 * duplicate object keys instead of matching command names inside arbitrary strings. */
struct jt { int start, end, next, count; char type; };
struct jp { const char *s; int pos, n; struct jt t[256]; };
static void ws(struct jp *p) { while (strchr(" \t\r\n", p->s[p->pos]) && p->s[p->pos]) p->pos++; }
static int value(struct jp *p, int depth) {
    ws(p); if (depth > 16 || p->n >= 256) return -1;
    int i = p->n++; struct jt *t = &p->t[i]; t->start = p->pos; t->type = p->s[p->pos++]; t->count = 0;
    if (t->type == '{' || t->type == '[') {
        char end = t->type == '{' ? '}' : ']'; ws(p);
        if (p->s[p->pos] != end) for (;;) {
            if (t->type == '{') {
                ws(p); if (p->s[p->pos] != '"') return -1;
                int k = value(p, depth + 1); if (k < 0) return -1;
                for (int j = i + 1; j < k; j = p->t[p->t[j].next].next)
                    if (p->t[j].end - p->t[j].start == p->t[k].end - p->t[k].start && !memcmp(p->s + p->t[j].start, p->s + p->t[k].start, p->t[k].end - p->t[k].start)) return -1;
                ws(p); if (p->s[p->pos++] != ':') return -1;
            }
            if (value(p, depth + 1) < 0) return -1; t->count++; ws(p);
            if (p->s[p->pos] == end) break;
            if (p->s[p->pos++] != ',') return -1;
        }
        p->pos++;
    } else if (t->type == '"') {
        while (p->s[p->pos] && p->s[p->pos] != '"') {
            unsigned char c = p->s[p->pos++]; if (c < 32) return -1;
            if (c == '\\') {
                c = p->s[p->pos++]; if (!c || !strchr("\"\\/bfnrtu", c)) return -1;
                if (c == 'u') for (int j = 0; j < 4; j++) { if (!p->s[p->pos] || unhex(p->s[p->pos++]) < 0) return -1; }
            }
        }
        if (p->s[p->pos++] != '"') return -1;
    } else {
        p->pos--;
        if (!strncmp(p->s + p->pos, "true", 4) || !strncmp(p->s + p->pos, "null", 4)) p->pos += 4;
        else if (!strncmp(p->s + p->pos, "false", 5)) p->pos += 5;
        else {
            if (p->s[p->pos] == '-') p->pos++;
            if (p->s[p->pos] == '0') p->pos++;
            else { if (!isdigit((unsigned char)p->s[p->pos])) return -1; while (isdigit((unsigned char)p->s[p->pos])) p->pos++; }
            if (p->s[p->pos] == '.') { p->pos++; if (!isdigit((unsigned char)p->s[p->pos])) return -1; while (isdigit((unsigned char)p->s[p->pos])) p->pos++; }
            if (p->s[p->pos] == 'e' || p->s[p->pos] == 'E') { p->pos++; if (p->s[p->pos] == '+' || p->s[p->pos] == '-') p->pos++; if (!isdigit((unsigned char)p->s[p->pos])) return -1; while (isdigit((unsigned char)p->s[p->pos])) p->pos++; }
        }
    }
    t->end = p->pos; t->next = p->n; return i;
}
static int jeq(struct jp *p, int i, const char *s) { return i >= 0 && p->t[i].type == '"' && p->t[i].end - p->t[i].start == (int)strlen(s) + 2 && !memcmp(p->s + p->t[i].start + 1, s, strlen(s)); }
static int field(struct jp *p, int i, const char *key) {
    if (i < 0 || p->t[i].type != '{') return -1;
    for (int j = i + 1; j < p->t[i].next; j = p->t[p->t[j].next].next) if (jeq(p, j, key)) return p->t[j].next;
    return -1;
}
static int single(struct jp *p, int i) { return i >= 0 && p->t[i].type == '[' && p->t[i].count == 1 ? i + 1 : -1; }
static int contains(struct jp *p, int i, const char *s) {
    if (i < 0 || p->t[i].type != '[') return 0;
    for (int j = i + 1; j < p->t[i].next; j = p->t[j].next) if (jeq(p, j, s)) return 1;
    return 0;
}
/* SDP is ASCII; accept JSON escaping without interpreting keys or commands. */
static int jtext(struct jp *p, int i, char *out, size_t cap) {
    if (i < 0 || p->t[i].type != '"') return 0; size_t n = 0;
    for (int k = p->t[i].start + 1; k < p->t[i].end - 1; k++) {
        unsigned c = (unsigned char)p->s[k];
        if (c == '\\') {
            c = p->s[++k];
            if (c == 'r') c = '\r'; else if (c == 'n') c = '\n'; else if (c == 't') c = '\t';
            else if (c == 'u') { c = 0; for (int z = 0; z < 4; z++) c = c * 16 + unhex(p->s[++k]); }
            else if (c != '"' && c != '\\' && c != '/') return 0;
        }
        if (!c || c > 127 || n + 1 >= cap) return 0; out[n++] = c;
    }
    out[n] = 0; return 1;
}
static int json_quote(const char *s, char *out, size_t cap) {
    size_t n = 0; if (cap < 3) return 0; out[n++] = '"';
    for (; *s; s++) {
        if (n + 3 >= cap) return 0;
        if (*s == '\r' || *s == '\n' || *s == '"' || *s == '\\') { out[n++] = '\\'; out[n++] = *s == '\r' ? 'r' : *s == '\n' ? 'n' : *s; }
        else { if ((unsigned char)*s < 32) return 0; out[n++] = *s; }
    }
    out[n++] = '"'; out[n] = 0; return 1;
}
static void error(struct google_response *r, int code, const char *s) { fprintf(stderr, "google: response error status=%d reason=%s\n", code, s); r->code = code; snprintf(r->body, sizeof r->body, "{\"error\":\"%s\"}", s); }
void google_viewer_webrtc(const char *body, struct google_response *r) {
    memset(r, 0, sizeof *r); r->code = 200; r->type = "application/json";
    struct jp p = {.s = body};
    if (value(&p, 0) < 0) { error(r, 400, "invalid_json"); return; } ws(&p);
    if (body[p.pos]) { error(r, 400, "invalid_json"); return; }
    int action = field(&p, 0, "action");
    if (jeq(&p, action, "end")) {
        char session[65];
        if (!g.rtc_viewer || !*g.rtc_token || !jtext(&p, field(&p, 0, "session"), session, sizeof session) || !equal(session, g.rtc_token)) {
            error(r, 409, "session_replaced"); return;
        }
        rtc_end(); g.rtc_token[0] = 0; g.rtc_viewer = 0;
        strcpy(r->body, "{}"); return;
    }
    if (!jeq(&p, action, "offer")) { error(r, 400, "invalid_action"); return; }
    if (!rtc_ready()) { error(r, 503, "stream_unavailable"); return; }
    char sdp[14001], answer[4096], quoted[8192], session[65];
    if (!jtext(&p, field(&p, 0, "sdp"), sdp, sizeof sdp)) { error(r, 400, "invalid_offer"); return; }
    if (!random_hex(session)) { error(r, 503, "stream_unavailable"); return; }
    /* Do not interrupt a Google Home viewer (or another browser). The page
     * falls back to MP4 when the single WebRTC slot is occupied. */
    if (!rtc_accept_offer(sdp, answer, sizeof answer)) { error(r, 409, "stream_busy_or_invalid_offer"); return; }
    if (!json_quote(answer, quoted, sizeof quoted)) { rtc_end(); error(r, 500, "invalid_answer"); return; }
    strcpy(g.rtc_token, session); g.rtc_viewer = g.rtc_remote = 1;
    g.rtc_request[0] = 0; g.rtc_created = time(NULL); g.rtc_exp = g.rtc_created + 3600;
    snprintf(r->body, sizeof r->body, "{\"sdp\":%s,\"session\":\"%s\"}", quoted, session);
}
static void rtc_signaling(const char *method, const char *body, const char *headers, struct google_response *r) {
    strcpy(r->extra, "Access-Control-Allow-Origin: https://www.gstatic.com\r\nAccess-Control-Allow-Methods: POST, OPTIONS\r\nAccess-Control-Allow-Headers: Authorization, Content-Type\r\nCache-Control: no-store\r\n");
    if (!strcmp(method, "OPTIONS")) { strcpy(r->body, "{}"); return; }
    if (strcmp(method, "POST")) { error(r, 405, "method_not_allowed"); return; }
    struct jp p = {.s = body};
    if (value(&p, 0) < 0) { error(r, 400, "invalid_json"); return; } ws(&p);
    int action = field(&p, 0, "action");
    char auth[128];
    int authorized = !g.rtc_viewer && *g.rtc_token && time(NULL) <= g.rtc_exp && google_header(headers, "Authorization", auth, sizeof auth) && !strncmp(auth, "Bearer ", 7) && equal(auth + 7, g.rtc_token);
    fprintf(stderr,"google: signaling action=%s authorized=%d\n",jeq(&p,action,"answer")?"answer":jeq(&p,action,"end")?"end":jeq(&p,action,"offer")?"offer":"unknown",authorized);
    if (!authorized) { error(r, 401, "invalid_stream_token"); return; }
    if (body[p.pos] || !jeq(&p, field(&p, 0, "deviceId"), "plug2view")) { error(r, 400, "invalid_device"); return; }
    if (jeq(&p, action, "end")) { rtc_end(); g.rtc_token[0] = 0; strcpy(r->body, "{}"); return; }
    char sdp[14001];
    if (jeq(&p, action, "offer") && g.rtc_remote) {
        char answer[4096], quoted[8192];
        if (!jtext(&p, field(&p, 0, "sdp"), sdp, sizeof sdp) || !rtc_accept_offer(sdp, answer, sizeof answer) || !json_quote(answer, quoted, sizeof quoted)) { error(r, 400, "invalid_offer"); return; }
        snprintf(r->body, sizeof r->body, "{\"action\":\"answer\",\"sdp\":%s}", quoted); return;
    }
    if (g.rtc_remote || !jeq(&p, action, "answer") || !jtext(&p, field(&p, 0, "sdp"), sdp, sizeof sdp) || !rtc_answer(sdp)) { error(r, 400, "invalid_answer"); return; }
    /* Google supplied the answer to our offer: acknowledge it, do not return
     * an answer action without an SDP (reserved for Google-originated offers). */
    strcpy(r->body, "{}");
}
static void fulfill_stream(const char *body, int online, struct google_response *r) {
    struct jp p = {.s = body};
    if (value(&p, 0) < 0) { error(r, 400, "invalid_json"); return; } ws(&p);
    int rid = field(&p, 0, "requestId"), input = single(&p, field(&p, 0, "inputs"));
    if (body[p.pos] || rid < 0 || p.t[rid].type != '"' || p.t[rid].end - p.t[rid].start > 256 || input < 0) { error(r, 400, "invalid_request"); return; }
    char id[260]; int n = p.t[rid].end - p.t[rid].start; memcpy(id, body + p.t[rid].start, n); id[n] = 0;
    int intent = field(&p, input, "intent");
    if (jeq(&p, intent, "action.devices.SYNC")) {
        fprintf(stderr, "google: SYNC\n");
        snprintf(r->body, sizeof r->body, "{\"requestId\":%s,\"payload\":{\"agentUserId\":\"plug2view-owner\",\"devices\":[{\"id\":\"plug2view\",\"type\":\"action.devices.types.CAMERA\",\"traits\":[\"action.devices.traits.CameraStream\"],\"name\":{\"name\":\"Plug2View Camera\"},\"willReportState\":false,\"attributes\":{\"cameraStreamSupportedProtocols\":[%s\"hls\"],\"cameraStreamNeedAuthToken\":false},\"deviceInfo\":{\"manufacturer\":\"Plug2View\",\"model\":\"HK camera\",\"swVersion\":\"experimental-stream-2\"}}]}}", id, rtc_ready() ? "\"webrtc\"," : "");
    } else if (jeq(&p, intent, "action.devices.DISCONNECT")) {
        fprintf(stderr, "google: DISCONNECT\n"); if (!rotate_generation()) { error(r, 500, "state_write_failed"); return; } strcpy(r->body, "{}");
    } else if (jeq(&p, intent, "action.devices.QUERY")) {
        int d = single(&p, field(&p, field(&p, input, "payload"), "devices"));
        if (!jeq(&p, field(&p, d, "id"), "plug2view")) { error(r, 400, "unknown_device"); return; }
        snprintf(r->body, sizeof r->body, "{\"requestId\":%s,\"payload\":{\"devices\":{\"plug2view\":{\"online\":%s,\"status\":\"SUCCESS\"}}}}", id, online ? "true" : "false");
    } else if (jeq(&p, intent, "action.devices.EXECUTE")) {
        int cmd = single(&p, field(&p, field(&p, input, "payload"), "commands"));
        int dev = single(&p, field(&p, cmd, "devices")), exe = single(&p, field(&p, cmd, "execution"));
        if (!jeq(&p, field(&p, dev, "id"), "plug2view")) { error(r, 400, "unknown_device"); return; }
        int params = field(&p, exe, "params"), protocols = field(&p, params, "SupportedStreamProtocols");
        int hls = contains(&p, protocols, "hls"), webrtc = contains(&p, protocols, "webrtc") && rtc_ready();
        int cast = field(&p, params, "StreamToChromecast");
        fprintf(stderr, "google: GetCameraStream offered hls=%d webrtc=%d dash=%d cast=%d at=%ld\n", hls, contains(&p, protocols, "webrtc"), contains(&p, protocols, "dash"), cast < 0 ? -1 : p.t[cast].type == 't' ? 1 : p.t[cast].type == 'f' ? 0 : -1, (long)time(NULL));
        const char *err = !online ? "deviceOffline" : !jeq(&p, field(&p, exe, "command"), "action.devices.commands.GetCameraStream") ? "functionNotSupported" : !hls && !webrtc ? "notSupported" : NULL;
        if (err) snprintf(r->body, sizeof r->body, "{\"requestId\":%s,\"payload\":{\"commands\":[{\"ids\":[\"plug2view\"],\"status\":\"ERROR\",\"errorCode\":\"%s\"}]}}", id, err);
        else if (webrtc) {
            if (g.rtc_viewer) { rtc_end(); g.rtc_token[0] = 0; g.rtc_viewer = 0; }
            /* Phone receivers may negotiate a different media layout. Omit
             * cameraStreamOffer so Google initiates SDP negotiation. */
            if (cast >= 0 && p.t[cast].type == 'f') {
                time_t now = time(NULL);
                if (!g.rtc_remote || !*g.rtc_token || now >= g.rtc_exp || (now - g.rtc_created >= 30 && strcmp(g.rtc_request, id))) {
                    rtc_end(); g.rtc_token[0] = 0;
                    if (!random_hex(g.rtc_token)) { error(r, 503, "stream_unavailable"); return; }
                    g.rtc_created = now; g.rtc_exp = now + 3600;
                }
                g.rtc_remote = 1; strcpy(g.rtc_request, id);
                snprintf(r->body, sizeof r->body, "{\"requestId\":%s,\"payload\":{\"commands\":[{\"ids\":[\"plug2view\"],\"status\":\"SUCCESS\",\"states\":{\"online\":true,\"cameraStreamProtocol\":\"webrtc\",\"cameraStreamSignalingUrl\":\"%s/google/webrtc\",\"cameraStreamAuthToken\":\"%s\"}}]}}", id, g.origin, g.rtc_token);
                fprintf(stderr, "google: awaiting receiver offer\n"); return;
            }
            if (g.rtc_remote) { rtc_end(); g.rtc_token[0] = 0; g.rtc_remote = 0; }
            char sdp[2048], quoted[4096];
            int offer = rtc_offer(sdp, sizeof sdp, !strcmp(g.rtc_request,id));
            if (!offer || !json_quote(sdp, quoted, sizeof quoted) || (offer == 1 && !random_hex(g.rtc_token)) || !*g.rtc_token) { g.rtc_token[0] = 0; rtc_end(); error(r, 503, "stream_unavailable"); return; }
            if (offer == 1) g.rtc_exp = time(NULL) + 3600;
            strcpy(g.rtc_request,id);
            snprintf(r->body, sizeof r->body, "{\"requestId\":%s,\"payload\":{\"commands\":[{\"ids\":[\"plug2view\"],\"status\":\"SUCCESS\",\"states\":{\"online\":true,\"cameraStreamProtocol\":\"webrtc\",\"cameraStreamSignalingUrl\":\"%s/google/webrtc\",\"cameraStreamAuthToken\":\"%s\",\"cameraStreamOffer\":%s}}]}}", id, g.origin, g.rtc_token, quoted);
        } else {
            char access[192]; token('s', (unsigned long)time(NULL) + 3600, access);
            snprintf(r->body, sizeof r->body, "{\"requestId\":%s,\"payload\":{\"commands\":[{\"ids\":[\"plug2view\"],\"status\":\"SUCCESS\",\"states\":{\"online\":true,\"cameraStreamProtocol\":\"hls\",\"cameraStreamAccessUrl\":\"%s/hls/live.m3u8?token=%s\"}}]}}", id, g.origin, access);
        }
    } else error(r, 400, "unsupported_intent");
}
#include "google-fulfillment.inc"
void google_handle(const char *method, const char *path, const char *query, const char *body, const char *headers, int secure, int online, struct google_response *r) {
    memset(r, 0, sizeof *r); r->code = 200; r->type = "application/json";
    if (!g.enabled) { error(r, 404, "integration_disabled"); return; }
    if (!secure) { error(r, 403, "https_required"); return; }
    if (!strcmp(path, "/google/webrtc")) { rtc_signaling(method, body, headers, r); return; }
    time_t now = time(NULL);
    if (!strcmp(path, "/google/auth")) {
        if (!strcmp(method, "GET")) {
            char client[128], redirect[256], state[GOOGLE_STATE_MAX + 1], response[32], want[256], sandbox[256];
            snprintf(want, sizeof want, "https://oauth-redirect.googleusercontent.com/r/%s", g.project);
            snprintf(sandbox, sizeof sandbox, "https://oauth-redirect-sandbox.googleusercontent.com/r/%s", g.project);
            const char *invalid = NULL;
            if (!google_param(query, "client_id", client, sizeof client)) invalid = "client_id_parse";
            else if (!equal(client, g.client)) invalid = "client_id_mismatch";
            else if (!google_param(query, "redirect_uri", redirect, sizeof redirect)) invalid = "redirect_uri_parse";
            else if (strcmp(redirect, want) && strcmp(redirect, sandbox)) invalid = "redirect_uri_mismatch";
            else if (!google_param(query, "state", state, sizeof state)) invalid = "state_parse";
            else if (!google_param(query, "response_type", response, sizeof response)) invalid = "response_type_parse";
            else if (strcmp(response, "code")) invalid = "response_type_mismatch";
            if (invalid) {
                fprintf(stderr, "google: authorization rejected check=%s query_bytes=%lu\n", invalid, (unsigned long)strlen(query));
                error(r, 400, "invalid_request"); return;
            }
            fprintf(stderr, "google: authorization accepted state_bytes=%lu\n", (unsigned long)strlen(state));
            if (!random_hex(g.nonce)) { error(r, 500, "random_failed"); return; }
            strcpy(g.redirect, redirect); strcpy(g.state, state); g.flow_exp = now + 600; g.code[0] = 0;
            r->type = "text/html; charset=utf-8";
            /* Chromium checks form-action against the post-login redirect too. */
            snprintf(r->extra, sizeof r->extra, "X-Frame-Options: DENY\r\nContent-Security-Policy: default-src 'none'; form-action 'self' %s %s; frame-ancestors 'none'\r\nReferrer-Policy: no-referrer\r\n", want, sandbox);
            snprintf(r->body, sizeof r->body, "<!doctype html><meta name=viewport content='width=device-width'><title>Link Plug2View</title><h1>Link Plug2View to Google</h1><p>Google will receive this camera's name and temporary access to its live video when requested.</p><form method=post action=/google/auth><input type=hidden name=nonce value='%s'><p><label>Plug2View username <input name=username autocomplete=username required></label></p><p><label>Plug2View password <input type=password name=password autocomplete=current-password required></label></p><p>By signing in, you are authorizing Google to control your devices.</p><button name=consent value=yes>Agree and link</button></form><p>To cancel, close this page.</p><a href=https://policies.google.com/privacy>Google Privacy Policy</a>", g.nonce);
        } else if (!strcmp(method, "POST")) {
            char nonce[65], user[128], password[128], consent[16];
            if (now < g.retry_at) { error(r, 429, "try_later"); strcpy(r->extra, "Retry-After: 30\r\n"); return; }
            if (!google_param(body, "nonce", nonce, sizeof nonce) || !g.nonce[0] || !equal(nonce, g.nonce) || now > g.flow_exp || !google_param(body, "consent", consent, sizeof consent) || strcmp(consent, "yes")) { error(r, 400, "invalid_session"); return; }
            if (!google_param(body, "username", user, sizeof user) || !google_param(body, "password", password, sizeof password) || !equal(user, g.user) || !equal(password, g.password)) {
                if (++g.failures >= 5) { g.retry_at = now + 30; g.failures = 0; } error(r, 401, "invalid_login"); return;
            }
            g.failures = 0; g.nonce[0] = 0;
            if (!random_hex(g.code)) { error(r, 500, "random_failed"); return; } g.code_exp = now + 600;
            char state[3 * GOOGLE_STATE_MAX + 1]; encode(g.state, state); r->code = 302;
            snprintf(r->extra, sizeof r->extra, "Location: %s?code=%s&state=%s\r\n", g.redirect, g.code, state);
            fprintf(stderr, "google: login accepted; redirecting to Google\n");
            strcpy(r->body, "{}");
        } else error(r, 405, "method_not_allowed");
    } else if (!strcmp(path, "/google/token") && !strcmp(method, "POST")) {
        char client[128], secret[128], grant[32], supplied[192], redirect[256];
        if (!google_param(body, "client_id", client, sizeof client) || !google_param(body, "client_secret", secret, sizeof secret) || !equal(client, g.client) || !equal(secret, g.secret)) { error(r, 401, "invalid_client"); return; }
        if (!google_param(body, "grant_type", grant, sizeof grant)) { error(r, 400, "unsupported_grant_type"); return; }
        int fresh = !strcmp(grant, "authorization_code");
        if (fresh) {
            if (!google_param(body, "code", supplied, sizeof supplied) || !g.code[0] || !equal(supplied, g.code) || now > g.code_exp || !google_param(body, "redirect_uri", redirect, sizeof redirect) || strcmp(redirect, g.redirect)) { error(r, 400, "invalid_grant"); return; }
            g.code[0] = 0; if (!rotate_generation()) { error(r, 500, "state_write_failed"); return; }
        } else if (!strcmp(grant, "refresh_token")) {
            if (!google_param(body, "refresh_token", supplied, sizeof supplied) || !valid_token(supplied, 'r')) { error(r, 400, "invalid_grant"); return; }
        } else { error(r, 400, "unsupported_grant_type"); return; }
        char access[192], refresh[192]; token('a', (unsigned long)now + 3600, access); token('r', 0, refresh);
        if (fresh) snprintf(r->body, sizeof r->body, "{\"token_type\":\"Bearer\",\"access_token\":\"%s\",\"refresh_token\":\"%s\",\"expires_in\":3600}", access, refresh);
        else snprintf(r->body, sizeof r->body, "{\"token_type\":\"Bearer\",\"access_token\":\"%s\",\"expires_in\":3600}", access);
    } else if (!strcmp(path, "/google/fulfillment") && !strcmp(method, "POST")) {
        char header[256];
        if (!google_header(headers, "Authorization", header, sizeof header) || strncmp(header, "Bearer ", 7) || !valid_token(header + 7, 'a')) { error(r, 401, "invalid_token"); return; }
        fulfill(body, online, r);
    } else error(r, 404, "not_found");
}
