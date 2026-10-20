/*
 * hkctl - drive the Plug2View camera's pan/tilt (/dev/neck) and IR-cut (/dev/ircut)
 * character drivers directly. Interface recovered from the vendor's /bin/ircut and
 * /bin/neck tools (both drivers share one small ioctl vocabulary).
 *
 * Built with:  zig cc -target mipsel-linux-musleabi -mcpu=mips32 -Os -static -s
 *
 *   hkctl ircut day|night          filter in (normal colours) / out (IR sensitive)
 *   hkctl light                    light sensor reading from the ircut driver
 *   hkctl neck status              limit-switch bits and the saved position
 *   hkctl neck step <axis> <dir> <n> [delay_us]
 *                                  axis h|v, dir +|-, up to n single steps; stops at
 *                                  the limit switch or the software range
 *   hkctl neck release             de-energise the motor coils
 *   hkctl neck home                drive to the limit switches to find position 0,0
 *                                  (slow, moves a lot - only when the position is lost)
 *   hkctl neck goto <x> <y>        absolute move within the software range
 *   hkctl ioctl <dev> <cmd> [arg]  raw call (exploration)
 *
 * Position is kept in /tmp/neck.pos (RAM) and mirrored to /mnt/sif/owrt/neck.pos so it
 * survives a reboot as long as nobody moves the head by hand.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include "camera-presets.h"
#include "recording-policy.h"

#define IOC_STEP_A   1018   /* one step: h axis = toward x 0, v axis = down (toward y 0) */
#define IOC_STEP_B   1019   /* one step: h axis = toward x max, v axis = up (toward y max) */
#define IOC_STATUS   1020   /* ircut: light sensor; neck: limit-switch bitmask */
#define IOC_STOP     1021
#define IOC_RELEASE  1022   /* ircut: register signal; neck: release coils */
#define AXIS_H 1
#define AXIS_V 0

/* Limit bits returned by IOC_STATUS on /dev/neck (set = switch pressed). */
#define LIM_V_A (1 << 0)    /* bottom stop: reached while stepping v with IOC_STEP_A (verified) */
#define LIM_V_B (1 << 1)    /* top stop:    reached while stepping v with IOC_STEP_B */
#define LIM_H_B (1 << 2)    /* x=max stop:  reached while stepping h with IOC_STEP_B */
#define LIM_H_A (1 << 3)    /* x=0 stop:    reached while stepping h with IOC_STEP_A */

/* Full mechanical travel measured by the vendor firmware: x 0..4040, y 0..1077.
 * Software range keeps ~10 degrees clear of each pan end stop (user request: ~160 of 180). */
#define X_MAX 4040
#define Y_MAX 1077
#define X_MIN_SOFT 220
#define X_MAX_SOFT (X_MAX - 220)
#define Y_MIN_SOFT 20
#define Y_MAX_SOFT (Y_MAX - 20)
#define DEFAULT_DELAY_US 4000   /* 3-10 ms/step all moved reliably in tests; 4 ms = ~16 s full pan */

static const char *POS_RAM = "/tmp/neck.pos";
static const char *POS_FLASH = "/mnt/sif/owrt/neck.pos";

static int open_dev(const char *path) {
    int fd = open(path, O_RDWR);
    if (fd < 0) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); exit(2); }
    return fd;
}

/* ---- position persistence ------------------------------------------------- */
static int load_pos(int *x, int *y) {
    FILE *f = fopen(POS_RAM, "r");
    if (!f) f = fopen(POS_FLASH, "r");
    if (!f) return -1;
    int ok = fscanf(f, "%d %d", x, y) == 2;
    fclose(f);
    return ok ? 0 : -1;
}

static void save_pos(int x, int y) {
    const char *paths[2] = { POS_RAM, POS_FLASH };
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(paths[i], "w");
        if (f) { fprintf(f, "%d %d\n", x, y); fclose(f); }
    }
}

/* "Park" = the user's preferred default view (level, facing forward), returned to by the
 * page's centre button and after homing. Falls back to the mechanical mid-point. */
static const char *PARK_FLASH = "/mnt/sif/owrt/park.pos";

static void load_park(int *x, int *y) {
    *x = (X_MIN_SOFT + X_MAX_SOFT) / 2; *y = (Y_MIN_SOFT + Y_MAX_SOFT) / 2;
    FILE *f = fopen(PARK_FLASH, "r");
    if (f) { if (fscanf(f, "%d %d", x, y) != 2) { *x = (X_MIN_SOFT + X_MAX_SOFT) / 2; *y = (Y_MIN_SOFT + Y_MAX_SOFT) / 2; } fclose(f); }
}

static void save_park(int x, int y) {
    FILE *f = fopen(PARK_FLASH, "w");
    if (f) { fprintf(f, "%d %d\n", x, y); fclose(f); }
}

/* ---- neck primitives ------------------------------------------------------- */
static int neck_status(int fd) { return ioctl(fd, IOC_STATUS, 0); }

/* Step `n` times on one axis in one direction. Returns steps actually taken.
 * dir > 0 moves toward the axis maximum, dir < 0 toward 0. */
static int neck_step(int fd, int axis, int dir, int n, int delay_us, int *pos, int lo, int hi) {
    if(n<=0 || (pos && ((dir>0 && *pos>=hi) || (dir<0 && *pos<=lo)))) return 0;
    int guard=motion_guard_begin();
    if(guard<0) { perror("motion guard"); return 0; }
    int cmd, lim;
    if (axis == AXIS_H) { cmd = dir > 0 ? IOC_STEP_B : IOC_STEP_A; lim = dir > 0 ? LIM_H_B : LIM_H_A; }
    else                { cmd = dir > 0 ? IOC_STEP_B : IOC_STEP_A; lim = dir > 0 ? LIM_V_B : LIM_V_A; }
    int done = 0;
    for (; done < n; done++) {
        if(done && done%250==0 && !motion_guard_refresh(guard)) break;
        if (pos && ((dir > 0 && *pos >= hi) || (dir < 0 && *pos <= lo))) break;  /* software range */
        int st = neck_status(fd);
        if (st < 0) { perror("status"); break; }
        if (st & lim) {                                                           /* hardware limit */
            /* The switches are the only absolute reference (no encoder): if we reach one,
             * the head has been moved by hand - resync the position to that end. */
            if (pos) *pos = dir > 0 ? (axis == AXIS_H ? X_MAX : Y_MAX) : 0;
            break;
        }
        if (ioctl(fd, cmd, axis) < 0) { perror("step"); break; }
        if (pos) *pos += dir > 0 ? 1 : -1;
        usleep(delay_us);
    }
    motion_guard_end(guard);
    return done;
}

static void neck_release(int fd) { ioctl(fd, IOC_RELEASE, 0); }

/* ---- commands --------------------------------------------------------------- */
static int cmd_neck(int argc, char **argv) {
    if (argc < 3) return 1;
    const char *sub = argv[2];
    int preset_lock=-1;
    if(!strcmp(sub,"preset") || !strcmp(sub,"move")) {
        preset_lock=open("/tmp/neck.lock",O_RDWR|O_CREAT,0644);
        if(preset_lock<0 || flock(preset_lock,LOCK_EX)<0) { if(preset_lock>=0) close(preset_lock); return 4; }
    }
    int fd = open_dev("/dev/neck");
    int x = -1, y = -1, have_pos = load_pos(&x, &y) == 0;

    if (!strcmp(sub, "status")) {
        int st = neck_status(fd);
        printf("limits=0x%x%s%s%s%s\n", st,
               st & LIM_H_A ? " pan-x0" : "", st & LIM_H_B ? " pan-xmax" : "",
               st & LIM_V_A ? " tilt-bottom" : "", st & LIM_V_B ? " tilt-top" : "");
        if (have_pos) printf("position=%d %d\n", x, y); else printf("position=unknown (run: hkctl neck home)\n");
        printf("range x=%d..%d y=%d..%d\n", X_MIN_SOFT, X_MAX_SOFT, Y_MIN_SOFT, Y_MAX_SOFT);
    } else if (!strcmp(sub, "release")) {
        neck_release(fd);
    } else if (!strcmp(sub, "step")) {
        if (argc < 6) { close(fd); return 1; }
        int axis = argv[3][0] == 'h' ? AXIS_H : AXIS_V;
        int dir = argv[4][0] == '-' ? -1 : 1;
        int n = atoi(argv[5]);
        int delay = argc > 6 ? atoi(argv[6]) : DEFAULT_DELAY_US;
        int *p = have_pos ? (axis == AXIS_H ? &x : &y) : NULL;
        int lo = axis == AXIS_H ? X_MIN_SOFT : Y_MIN_SOFT, hi = axis == AXIS_H ? X_MAX_SOFT : Y_MAX_SOFT;
        int done = neck_step(fd, axis, dir, n, delay, p, lo, hi);
        neck_release(fd);
        if (have_pos) save_pos(x, y);
        printf("stepped %d/%d limits=0x%x%s\n", done, n, neck_status(fd), have_pos ? "" : " (position unknown)");
    } else if (!strcmp(sub, "home")) {
        int delay = argc > 3 ? atoi(argv[3]) : DEFAULT_DELAY_US;
        /* Drive each axis to its 0-side switch, ignoring the software range. */
        int hx = neck_step(fd, AXIS_H, -1, X_MAX + 200, delay, NULL, 0, 0);
        int vy = neck_step(fd, AXIS_V, -1, Y_MAX + 200, delay, NULL, 0, 0);
        x = 0; y = 0; save_pos(x, y);
        /* Back off into the software range. */
        neck_step(fd, AXIS_H, 1, X_MIN_SOFT, delay, &x, X_MIN_SOFT - 1, X_MAX_SOFT);
        neck_step(fd, AXIS_V, 1, Y_MIN_SOFT, delay, &y, Y_MIN_SOFT - 1, Y_MAX_SOFT);
        neck_release(fd);
        save_pos(x, y);
        printf("homed (%d h, %d v steps to switches); position=%d %d\n", hx, vy, x, y);
    } else if (!strcmp(sub, "goto") || !strcmp(sub,"preset") || !strcmp(sub,"move")) {
        int is_preset=!strcmp(sub,"preset"),is_move=!strcmp(sub,"move");
        if (argc < (is_preset || is_move?4:5)) { close(fd); return 1; }
        if (!have_pos) { fprintf(stderr, "position unknown - run: hkctl neck home\n"); close(fd); return 3; }
        int tx,ty;
        if(is_move) {
            char *end; long direction=strtol(argv[3],&end,10); int px,py; load_park(&px,&py);
            if(*end || direction<0 || direction>4 || !camera_move_target((int)direction,x,y,px,py,&tx,&ty)) { close(fd); close(preset_lock); return 1; }
        } else if(is_preset) {
            char *end; long index=strtol(argv[3],&end,10); int px,py; load_park(&px,&py);
            if(*end || index<0 || index>10 || !camera_preset_target((int)index,px,py,&tx,&ty)) { close(fd); close(preset_lock); return 1; }
        } else { tx=atoi(argv[3]); ty=atoi(argv[4]); }
        int delay = !is_preset && !is_move && argc > 5 ? atoi(argv[5]) : DEFAULT_DELAY_US;
        if (tx < X_MIN_SOFT) tx = X_MIN_SOFT; if (tx > X_MAX_SOFT) tx = X_MAX_SOFT;
        if (ty < Y_MIN_SOFT) ty = Y_MIN_SOFT; if (ty > Y_MAX_SOFT) ty = Y_MAX_SOFT;
        neck_step(fd, AXIS_H, tx > x ? 1 : -1, abs(tx - x), delay, &x, X_MIN_SOFT, X_MAX_SOFT);
        neck_step(fd, AXIS_V, ty > y ? 1 : -1, abs(ty - y), delay, &y, Y_MIN_SOFT, Y_MAX_SOFT);
        neck_release(fd);
        save_pos(x, y);
        printf("position=%d %d limits=0x%x\n", x, y, neck_status(fd));
    } else { close(fd); return 1; }
    close(fd);
    if(preset_lock>=0) close(preset_lock);
    return 0;
}

/* Mirrors /bin/ircut: energise the filter solenoid one way for 500 ms, then stop. */
static int cmd_ircut(int argc, char **argv) {
    if (argc < 3) return 1;
    int drive;
    if (!strcmp(argv[2], "day") || !strcmp(argv[2], "p")) drive = IOC_STATUS;      /* ir_cut_p uses 1020 */
    else if (!strcmp(argv[2], "night") || !strcmp(argv[2], "n")) drive = IOC_STEP_B; /* ir_cut_n uses 1019 */
    else return 1;
    int fd = open_dev("/dev/ircut");
    ioctl(fd, drive, 0);
    usleep(500000);
    ioctl(fd, IOC_STOP, 0);
    close(fd);
    return 0;
}

static int cmd_light(void) {
    int fd = open_dev("/dev/ircut");
    int v = ioctl(fd, IOC_STEP_A, 0);  /* ircut_change polls 1018 for the sensor state */
    printf("light=%d\n", v);
    close(fd);
    return 0;
}

static int cmd_ioctl(int argc, char **argv) {
    if (argc < 4) return 1;
    int fd = open_dev(argv[2]);
    long cmd = strtol(argv[3], 0, 0), arg = argc > 4 ? strtol(argv[4], 0, 0) : 0;
    int r = ioctl(fd, cmd, arg);
    printf("ioctl(%s, %ld, %ld) = %d %s\n", argv[2], cmd, arg, r, r < 0 ? strerror(errno) : "");
    close(fd);
    return r < 0;
}

/* ---- minimal HTTP control server --------------------------------------------
 * GET /ptz?dx=<steps>&dy=<steps>      relative move (clamped to the software range)
 * GET /ptz?x=<abs>&y=<abs>            absolute move
 * GET /ptz?home=1                     find the limit switches, re-zero, return to park
 * GET /ptz?park=1                     go to the saved default view
 * GET /ptz?setpark=1                  save the current position as the default view
 * GET /ircut?mode=day|night
 * GET /status                         JSON: position, limits, light sensor
 * GET /wifi/scan                      nearby 2.4 GHz networks (takes ~5 s)
 * GET /wifi/status                    configured SSID, association state, Wi-Fi/Ethernet IPs
 * POST /wifi/join  ssid=&pass=&security=   write wifi.conf and connect (via wifi.sh)
 * Each request is handled in a forked child so a long move cannot block the next
 * status poll; moves are serialised with a lock file. CORS is open because the viewer
 * page is served from the streamer's port.
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>

static int qparam(const char *q, const char *key, int *out) {
    char pat[32]; snprintf(pat, sizeof pat, "%s=", key);
    const char *p = q;
    while ((p = strstr(p, pat))) {
        if (p == q || p[-1] == '&' || p[-1] == '?') { *out = atoi(p + strlen(pat)); return 1; }
        p++;
    }
    return 0;
}

/* motion detection (defined further down; used by the /motion endpoint) */
#define MOTION_CONF  "/mnt/sif/owrt/motion.conf"
#define MOTION_STATE "/tmp/motion.state"
struct mcfg { int on, threshold, zones, confirm; };
static int motion_enabled(void);
static void motion_cfg(struct mcfg *m);
static int motion_cfg_save(const struct mcfg *m);

/* ---- Wi-Fi helpers (shell out to the Ralink iwpriv tools via wifi.sh) ------- */
#define WIFI_CONF "/mnt/sif/owrt/wifi.conf"
#define WIFI_SH   "/mnt/sif/owrt/wifi.sh"

static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++, o++) {
        if (*s == '+') *o = ' ';
        else if (*s == '%' && s[1] && s[2]) { char h[3] = { s[1], s[2], 0 }; *o = (char)strtol(h, 0, 16); s += 2; }
        else *o = *s;
    }
    *o = 0;
}

/* Copy the value of key=... from a query/form string into out (decoded). */
static int sparam(const char *q, const char *key, char *out, int n) {
    char pat[32]; snprintf(pat, sizeof pat, "%s=", key);
    const char *p = q;
    while ((p = strstr(p, pat))) {
        if (p == q || p[-1] == '&' || p[-1] == '?') {
            p += strlen(pat);
            int i = 0;
            while (*p && *p != '&' && i < n - 1) out[i++] = *p++;
            out[i] = 0;
            url_decode(out);
            return 1;
        }
        p++;
    }
    return 0;
}

/* Append s to out as a JSON string literal (quotes/backslashes/control chars escaped). */
static void json_str(char *out, int n, const char *s) {
    int len = strlen(out);
    if (len < n - 1) out[len++] = '"';
    for (; *s && len < n - 8; s++) {
        if (*s == '"' || *s == '\\') { out[len++] = '\\'; out[len++] = *s; }
        else if ((unsigned char)*s < 0x20) len += snprintf(out + len, n - len, "\\u%04x", *s);
        else out[len++] = *s;
    }
    if (len < n - 1) out[len++] = '"';
    out[len] = 0;
}

static void rtrim(char *s) { int l = strlen(s); while (l && (s[l - 1] == ' ' || s[l - 1] == '\r' || s[l - 1] == '\n')) s[--l] = 0; }

/* iwpriv ra0 get_site_survey -> JSON array. Columns are fixed width in this driver:
 * Ch(0) SSID(4,33) BSSID(37,17) Security(57,23) Signal(80). */
static void wifi_scan(char *out, int n) {
    system("ifconfig ra0 up; iwpriv ra0 set SiteSurvey=1; sleep 4");
    FILE *p = popen("iwpriv ra0 get_site_survey 2>/dev/null", "r");
    strcpy(out, "[");
    char line[256]; int first = 1;
    while (p && fgets(line, sizeof line, p)) {
        if (strlen(line) < 82 || line[0] < '0' || line[0] > '9') continue;
        char ssid[34], bssid[18], sec[24];
        memcpy(ssid, line + 4, 33); ssid[33] = 0; rtrim(ssid);
        memcpy(bssid, line + 37, 17); bssid[17] = 0;
        memcpy(sec, line + 57, 23); sec[23] = 0; rtrim(sec);
        if (!ssid[0]) continue;                       /* hidden networks cannot be picked */
        int len = strlen(out);
        if (len > n - 160) break;
        snprintf(out + len, n - len, "%s{\"ch\":%d,\"ssid\":", first ? "" : ",", atoi(line));
        json_str(out, n, ssid);
        len = strlen(out);
        snprintf(out + len, n - len, ",\"bssid\":\"%s\",\"security\":\"%s\",\"signal\":%d}", bssid, sec, atoi(line + 80));
        first = 0;
    }
    if (p) pclose(p);
    strncat(out, "]", n - strlen(out) - 1);
}

/* Write wifi.conf (shell-sourced, single-quoted values) and start wifi.sh detached. */
static int wifi_join(const char *ssid, const char *pass, const char *sec) {
    const char *auth = "WPA2PSK", *enc = "AES";
    if (strstr(sec, "NONE") || !strcmp(sec, "OPEN") || !pass[0]) { auth = "OPEN"; enc = "NONE"; }
    else if (strstr(sec, "TKIP") && !strstr(sec, "AES")) { auth = strstr(sec, "WPA2") ? "WPA2PSK" : "WPAPSK"; enc = "TKIP"; }
    else if (strstr(sec, "WPA1") && !strstr(sec, "WPA2")) { auth = "WPAPSK"; }
    FILE *f = fopen(WIFI_CONF, "w");
    if (!f) return -1;
    const char *vals[2] = { ssid, pass }, *keys[2] = { "SSID", "PASS" };
    for (int k = 0; k < 2; k++) {
        fprintf(f, "%s='", keys[k]);
        for (const char *s = vals[k]; *s; s++) { if (*s == '\'') fputs("'\\''", f); else if (*s != '\n' && *s != '\r') fputc(*s, f); }
        fputs("'\n", f);
    }
    fprintf(f, "AUTH=%s\nENC=%s\n", auth, enc);
    fclose(f);
    system("sh " WIFI_SH " off >/dev/null 2>&1; sh " WIFI_SH " >/tmp/wifi.log 2>&1 &");
    return 0;
}

static void wifi_status(char *out, int n) {
    char ssid[64] = "", st[64] = "", ip[32] = "", eth[32] = "";
    FILE *p = popen("sed -n \"s/^SSID='\\(.*\\)'$/\\1/p\" " WIFI_CONF " 2>/dev/null", "r");
    if (p) { if (fgets(ssid, sizeof ssid, p)) rtrim(ssid); pclose(p); }
    p = popen("iwpriv ra0 connStatus 2>/dev/null | sed 's/.*connStatus://'", "r");
    if (p) { if (fgets(st, sizeof st, p)) rtrim(st); pclose(p); }
    p = popen("ifconfig ra0 2>/dev/null | sed -n 's/.*inet addr:\\([0-9.]*\\).*/\\1/p'", "r");
    if (p) { if (fgets(ip, sizeof ip, p)) rtrim(ip); pclose(p); }
    p = popen("ifconfig eth2 2>/dev/null | sed -n 's/.*inet addr:\\([0-9.]*\\).*/\\1/p'", "r");
    if (p) { if (fgets(eth, sizeof eth, p)) rtrim(eth); pclose(p); }
    strcpy(out, "{\"ssid\":"); json_str(out, n, ssid);
    int len = strlen(out);
    snprintf(out + len, n - len, ",\"state\":\"%s\",\"wifi_ip\":\"%s\",\"eth_ip\":\"%s\"}", st, ip, eth);
}

static void respond(int c, int code, const char *body) {
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr, "HTTP/1.0 %d %s\r\nContent-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n"
        "Content-Length: %d\r\n\r\n", code, code == 200 ? "OK" : code == 404 ? "Not Found" : "Error", (int)strlen(body));
    write(c, hdr, n);
    write(c, body, strlen(body));
}

static void handle(int c) {
    static char req[4096], big[8192];
    char body[900];
    memset(req, 0, sizeof req);
    int n = 0, r;
    /* Read headers (and body for POST) - requests are tiny and come from our own page. */
    while (n < (int)sizeof req - 1 && (r = read(c, req + n, sizeof req - 1 - n)) > 0) {
        n += r;
        char *hend = strstr(req, "\r\n\r\n");
        if (!hend) continue;
        char *cl = strstr(req, "Content-Length:");
        int want = cl ? atoi(cl + 15) : 0;
        if ((req + n) - (hend + 4) >= want) break;
    }
    if (n <= 0) return;
    int is_post = !strncmp(req, "POST ", 5);
    char *path = strchr(req, ' ');
    if (!path) return;
    path++;
    char *end = strchr(path, ' ');
    if (end) *end = 0;
    char *q = strchr(path, '?');
    if (q) *q++ = 0; else q = "";
    if (is_post) { char *b = strstr(end + 1, "\r\n\r\n"); if (b) q = b + 4; }

    if (!strcmp(path, "/wifi/scan")) {
        wifi_scan(big, sizeof big);
        respond(c, 200, big);
        return;
    } else if (!strcmp(path, "/wifi/status")) {
        wifi_status(body, sizeof body);
        respond(c, 200, body);
        return;
    } else if (!strcmp(path, "/wifi/join")) {
        char ssid[64] = "", pass[96] = "", sec[32] = "";
        sparam(q, "ssid", ssid, sizeof ssid);
        sparam(q, "pass", pass, sizeof pass);
        sparam(q, "security", sec, sizeof sec);
        if (!ssid[0]) { respond(c, 400, "{\"error\":\"ssid required\"}"); return; }
        if (wifi_join(ssid, pass, sec) < 0) { respond(c, 500, "{\"error\":\"cannot write wifi.conf\"}"); return; }
        respond(c, 200, "{\"joining\":true}");
        return;
    }

    int lock = open("/tmp/neck.lock", O_RDWR | O_CREAT, 0644);
    if (!strcmp(path, "/ptz")) {
        int v, fd = open("/dev/neck", O_RDWR);
        if (fd < 0) { respond(c, 500, "{\"error\":\"open /dev/neck\"}"); return; }
        flock(lock, LOCK_EX);
        int x = -1, y = -1, have = load_pos(&x, &y) == 0;
        int px, py; load_park(&px, &py);
        if (qparam(q, "setpark", &v) && have) {
            save_park(x, y); px = x; py = y;
        } else if (qparam(q, "home", &v)) {
            neck_step(fd, AXIS_H, -1, X_MAX + 200, DEFAULT_DELAY_US, NULL, 0, 0);
            neck_step(fd, AXIS_V, -1, Y_MAX + 200, DEFAULT_DELAY_US, NULL, 0, 0);
            x = 0; y = 0;
            neck_step(fd, AXIS_H, 1, X_MIN_SOFT, DEFAULT_DELAY_US, &x, -1, X_MAX_SOFT);
            neck_step(fd, AXIS_V, 1, Y_MIN_SOFT, DEFAULT_DELAY_US, &y, -1, Y_MAX_SOFT);
            have = 1;
            neck_step(fd, AXIS_H, px > x ? 1 : -1, abs(px - x), DEFAULT_DELAY_US, &x, X_MIN_SOFT, X_MAX_SOFT);
            neck_step(fd, AXIS_V, py > y ? 1 : -1, abs(py - y), DEFAULT_DELAY_US, &y, Y_MIN_SOFT, Y_MAX_SOFT);
        } else if (have) {
            int tx = x, ty = y;
            if (qparam(q, "park", &v)) { tx = px; ty = py; }
            if (qparam(q, "x", &v)) tx = v;
            if (qparam(q, "y", &v)) ty = v;
            if (qparam(q, "dx", &v)) tx = x + v;
            if (qparam(q, "dy", &v)) ty = y + v;
            if (tx < X_MIN_SOFT) tx = X_MIN_SOFT; if (tx > X_MAX_SOFT) tx = X_MAX_SOFT;
            if (ty < Y_MIN_SOFT) ty = Y_MIN_SOFT; if (ty > Y_MAX_SOFT) ty = Y_MAX_SOFT;
            neck_step(fd, AXIS_H, tx > x ? 1 : -1, abs(tx - x), DEFAULT_DELAY_US, &x, X_MIN_SOFT, X_MAX_SOFT);
            neck_step(fd, AXIS_V, ty > y ? 1 : -1, abs(ty - y), DEFAULT_DELAY_US, &y, Y_MIN_SOFT, Y_MAX_SOFT);
        }
        neck_release(fd);
        if (have) save_pos(x, y);
        snprintf(body, sizeof body, "{\"x\":%d,\"y\":%d,\"known\":%s,\"limits\":%d,\"park\":[%d,%d]}", x, y, have ? "true" : "false", neck_status(fd), px, py);
        close(fd);
        flock(lock, LOCK_UN);
        respond(c, 200, body);
    } else if (!strcmp(path, "/ircut")) {
        const char *mode = strstr(q, "mode=night") ? "night" : strstr(q, "mode=day") ? "day" : NULL;
        if (!mode) { respond(c, 400, "{\"error\":\"mode=day|night\"}"); return; }
        char *av[3] = { "hkctl", "ircut", (char *)mode };
        cmd_ircut(3, av);
        snprintf(body, sizeof body, "{\"ircut\":\"%s\"}", mode);
        respond(c, 200, body);
    } else if (!strcmp(path, "/motion")) {
        int v, changed = 0; struct mcfg m; motion_cfg(&m);
        if (qparam(q, "enable", &v)) { m.on = v ? 1 : 0; changed = 1; }
        if (qparam(q, "threshold", &v)) { m.threshold = v < 0 ? 0 : v > 65535 ? 65535 : v; changed = 1; }
        if (qparam(q, "zones", &v)) { m.zones = v & 511; changed = 1; }
        if (qparam(q, "confirm", &v)) { m.confirm = v < 1 ? 1 : v > 10 ? 10 : v; changed = 1; }
        if (changed && !motion_cfg_save(&m)) { respond(c,500,"{\"error\":\"cannot save motion settings\"}"); return; }
        char st[400] = "{}";
        FILE *f = fopen(MOTION_STATE, "r");
        if (f) { if (!fgets(st, sizeof st, f)) strcpy(st, "{}"); fclose(f); }
        char *nl = strchr(st, '\n'); if (nl) *nl = 0;
        snprintf(body, sizeof body, "{\"config_enabled\":%s,\"threshold\":%d,\"zones\":%d,\"confirm\":%d,\"state\":%s}",
                 m.on ? "true" : "false", m.threshold, m.zones, m.confirm, st);
        respond(c, 200, body);
    } else if (!strcmp(path, "/status")) {
        int x = -1, y = -1, have = load_pos(&x, &y) == 0;
        int fd = open("/dev/neck", O_RDWR), st = fd >= 0 ? neck_status(fd) : -1; if (fd >= 0) close(fd);
        int lf = open("/dev/ircut", O_RDWR), light = lf >= 0 ? ioctl(lf, IOC_STEP_A, 0) : -1; if (lf >= 0) close(lf);
        snprintf(body, sizeof body, "{\"x\":%d,\"y\":%d,\"known\":%s,\"limits\":%d,\"light\":%d,"
                 "\"range\":{\"x\":[%d,%d],\"y\":[%d,%d]}}", x, y, have ? "true" : "false", st, light,
                 X_MIN_SOFT, X_MAX_SOFT, Y_MIN_SOFT, Y_MAX_SOFT);
        respond(c, 200, body);
    } else {
        respond(c, 404, "{\"error\":\"unknown\"}");
    }
    close(lock);
}

static int cmd_serve(int argc, char **argv) {
    int port = argc > 2 ? atoi(argv[2]) : 8081;
    signal(SIGCHLD, SIG_IGN);
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    /* Localhost only: clients reach this API through camd's /api/ proxy on the video
     * port, which carries the HTTP basic auth. `serve <port> all` binds every interface. */
    int all = argc > 3 && !strcmp(argv[3], "all");
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = all ? INADDR_ANY : htonl(0x7f000001) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 8) < 0) { perror("listen"); return 2; }
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        if (fork() == 0) { close(s); handle(c); close(c); _exit(0); }
        close(c);
    }
}

/* ---- motion detection -----------------------------------------------------------
 * The Sonix module detects motion in hardware (9 zones). The vendor tool reads the
 * result through a UVC extension-unit control and prints "golllll <mask>"; polling it
 * once a second costs almost nothing and works while camd is streaming.
 *
 *   hkctl motion <video-port>   daemon: poll; while motion (and 5 s after) save one frame
 *                               per second via http://127.0.0.1:<port>/?action=save&tag=motion
 * Enabled/disabled through /mnt/sif/owrt/motion.conf (MOTION=1|0), toggled by the
 * /motion API below; state for the API is written to /tmp/motion.state. */
#define MOTION_POSTROLL 5

/* motion.conf: MOTION=0|1  THRESHOLD=<0..65535, default 273, higher = less sensitive>
 *              ZONES=<9-bit mask, bit0 = zone 1 (top-left) .. bit8 = zone 9>  CONFIRM=<polls> */
static void motion_cfg(struct mcfg *m) {
    m->on = 0; m->threshold = 273; m->zones = 511; m->confirm = 2;
    FILE *f = fopen(MOTION_CONF, "r"); char line[64];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "MOTION=", 7)) m->on = atoi(line + 7);
        else if (!strncmp(line, "THRESHOLD=", 10)) m->threshold = atoi(line + 10);
        else if (!strncmp(line, "ZONES=", 6)) m->zones = atoi(line + 6) & 511;
        else if (!strncmp(line, "CONFIRM=", 8)) m->confirm = atoi(line + 8);
    }
    fclose(f);
    if (m->threshold < 0 || m->threshold > 65535) m->threshold = 273;
    if (m->confirm < 1 || m->confirm > 10) m->confirm = 2;
}
static int motion_cfg_save(const struct mcfg *m) {
    char tmp[160]; snprintf(tmp,sizeof tmp,"%s.%ld.tmp",MOTION_CONF,(long)getpid());
    FILE *f = fopen(tmp, "w"); if(!f) return 0;
    int ok=fprintf(f, "MOTION=%d\nTHRESHOLD=%d\nZONES=%d\nCONFIRM=%d\n", m->on, m->threshold, m->zones, m->confirm)>=0;
    if(fclose(f)) ok=0;
    if(!ok || rename(tmp,MOTION_CONF)) { remove(tmp); return 0; } return 1;
}
static int motion_enabled(void) { struct mcfg m; motion_cfg(&m); return m.on; }

/* Push threshold and zone set into the Sonix detector via the vendor tool. The tool
 * takes zone *digits* ("123456789" = all), not a bitmask. */
static void motion_apply(const struct mcfg *m) {
    char cmd[200], zones[16] = ""; int n = 0;
    for (int z = 0; z < 9; z++) if (m->zones & (1 << z)) zones[n++] = '1' + z;
    zones[n] = 0;
    if (!n) strcpy(zones, "5");                 /* the tool cannot express "none"; keep centre */
    snprintf(cmd, sizeof cmd, "/mnt/sif/bin/SONiX_UVC_TestAP /dev/video0 --motionset-thre %d >/dev/null 2>&1", m->threshold);
    system(cmd);
    snprintf(cmd, sizeof cmd, "/mnt/sif/bin/SONiX_UVC_TestAP /dev/video0 --motionset-areas %s >/dev/null 2>&1", zones);
    system(cmd);
    fprintf(stderr, "motion config applied: threshold=%d zones=%s confirm=%d\n", m->threshold, zones, m->confirm);
}

static int motion_poll(void) {                /* returns zone mask, 0 = none, -1 = error */
    FILE *p = popen("/mnt/sif/bin/SONiX_UVC_TestAP /dev/video0 --motionget-result 2>/dev/null", "r");
    if (!p) return -1;
    char line[200]; int mask = -1;
    while (fgets(line, sizeof line, p)) { char *g = strstr(line, "golllll"); if (g) mask = atoi(g + 7); }
    pclose(p);
    return mask;
}

/* Minimal HTTP GET to the local video server; returns 1 if a 200 came back. */
static int local_get(int port, const char *path, char *resp, int n) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(0x7f000001) };
    struct timeval tv = { 5, 0 }; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return 0; }
    char req[300]; int l = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: localhost\r\n\r\n", path);
    write(s, req, l);
    int got = 0, r;
    while (got < n - 1 && (r = read(s, resp + got, n - 1 - got)) > 0) got += r;
    resp[got] = 0; close(s);
    return !strncmp(resp, "HTTP/1.0 200", 12);
}

static int cmd_motion(int argc, char **argv) {
    int port = argc > 2 ? atoi(argv[2]) : 8080;
    time_t active_until = 0, last_save = 0, last_event = 0; unsigned saved = 0, events = 0;
    int streak = 0;                              /* consecutive polls with motion (debounce) */
    struct mcfg m={-1,0,0,0};
    fprintf(stderr, "motion daemon: polling, saving to video port %d\n", port);
    for (;;) {
        struct mcfg next; motion_cfg(&next);
        if(next.on!=m.on || next.threshold!=m.threshold || next.zones!=m.zones || next.confirm!=m.confirm) { m=next; motion_apply(&m); }
        int suppressed=motion_suppressed();
        int mask = m.on ? motion_poll() : 0;       /* Drain hardware results even during movement. */
        suppressed = suppressed || motion_suppressed();
        if(suppressed || !m.on) { mask=0; streak=0; active_until=0; }
        time_t now = time(NULL);
        int started = 0;
        streak = mask > 0 ? streak + 1 : 0;
        if (m.on && mask > 0 && (streak >= m.confirm || now < active_until)) {
            if (now >= active_until) {
                char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
                events++; last_event = now; started = 1; fprintf(stderr, "%s motion start mask=0x%x\n", ts, mask);
            }
            active_until = now + MOTION_POSTROLL;
        }
        /* One MP4 per event: camd records continuously between start (with pre-roll) and stop;
         * the daemon only marks the beginning and the end of the event. */
        static int recording = 0;
        char resp[512];
        if (m.on && now < active_until) {
            if (started || !recording) { recording = local_get(port, "/?action=rec&start=1&tag=motion&pre=1", resp, sizeof resp); if (recording) saved++; }
            last_save = now;
        } else if (recording) {
            local_get(port, "/?action=rec&stop=1&tag=motion", resp, sizeof resp); recording = 0;
        }
        FILE *f = fopen(MOTION_STATE, "w");
        if (f) { fprintf(f, "{\"enabled\":%s,\"active\":%s,\"mask\":%d,\"events\":%u,\"saved\":%u,\"last_event\":%ld,"
                            "\"threshold\":%d,\"zones\":%d,\"confirm\":%d,\"suppressed\":%s}\n",
                         m.on ? "true" : "false", now < active_until ? "true" : "false", mask < 0 ? 0 : mask, events, saved, (long)last_event,
                         m.threshold, m.zones, m.confirm, suppressed?"true":"false"); fclose(f); }
        usleep(500000);                           /* 2 polls/s: ~1 s to trigger with confirm=2 */
    }
}

/* hkctl vidinfo <dev>: enumerate what a V4L2 device offers (formats, frame sizes,
 * current format). Used to find out what /dev/video1 (the Sonix second stream) carries.
 * These structs contain no time fields, so the header layouts match this old kernel. */
#include <linux/videodev2.h>
static int cmd_vidinfo(int argc, char **argv) {
    if (argc < 3) return 1;
    int fd = open(argv[2], O_RDWR | O_NONBLOCK);
    if (fd < 0) { perror(argv[2]); return 2; }
    struct v4l2_capability cap; memset(&cap, 0, sizeof cap);
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
        printf("driver=%s card=%s bus=%s caps=0x%08x\n", cap.driver, cap.card, cap.bus_info, cap.capabilities);
    for (unsigned i = 0; ; i++) {
        struct v4l2_fmtdesc d; memset(&d, 0, sizeof d); d.index = i; d.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &d) < 0) { if (i == 0) printf("ENUM_FMT: %s\n", strerror(errno)); break; }
        unsigned p = d.pixelformat;
        printf("format %u: %c%c%c%c (%s)%s\n", i, p & 255, p >> 8 & 255, p >> 16 & 255, p >> 24 & 255, d.description,
               d.flags & V4L2_FMT_FLAG_COMPRESSED ? " compressed" : "");
        for (unsigned j = 0; j < 32; j++) {
            struct v4l2_frmsizeenum fs; memset(&fs, 0, sizeof fs); fs.index = j; fs.pixel_format = p;
            if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) < 0) { if (j == 0) printf("   (frame sizes: %s)\n", strerror(errno)); break; }
            if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) printf("   %ux%u\n", fs.discrete.width, fs.discrete.height);
            else { printf("   %u-%u x %u-%u\n", fs.stepwise.min_width, fs.stepwise.max_width, fs.stepwise.min_height, fs.stepwise.max_height); break; }
        }
    }
    struct v4l2_format f; memset(&f, 0, sizeof f); f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_FMT, &f) == 0) {
        unsigned p = f.fmt.pix.pixelformat;
        printf("current: %ux%u %c%c%c%c sizeimage=%u\n", f.fmt.pix.width, f.fmt.pix.height, p & 255, p >> 8 & 255, p >> 16 & 255, p >> 24 & 255, f.fmt.pix.sizeimage);
    } else printf("G_FMT: %s\n", strerror(errno));
    close(fd);
    return 0;
}

/* hkctl daemon <logfile> <cmd> [args...]: run cmd fully detached (new session, no
 * controlling tty, stdio to logfile) so it survives the telnet session that started it.
 * The stock BusyBox has neither setsid nor nohup. */
static int cmd_daemon(int argc, char **argv) {
    if (argc < 4) return 1;
    pid_t p = fork();
    if (p < 0) { perror("fork"); return 2; }
    if (p > 0) return 0;                        /* parent returns immediately */
    setsid();
    if (fork() > 0) _exit(0);                   /* grandchild cannot reacquire a tty */
    chdir("/");
    int in = open("/dev/null", O_RDONLY), out = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (in >= 0) dup2(in, 0);
    if (out >= 0) { dup2(out, 1); dup2(out, 2); }
    execvp(argv[3], argv + 3);
    _exit(127);
}

int main(int argc, char **argv) {
    int rc = 1;
    if (argc >= 2) {
        if (!strcmp(argv[1], "daemon")) rc = cmd_daemon(argc, argv);
        else if (!strcmp(argv[1], "vidinfo")) rc = cmd_vidinfo(argc, argv);
        else if (!strcmp(argv[1], "motion")) rc = cmd_motion(argc, argv);
        else if (!strcmp(argv[1], "serve")) rc = cmd_serve(argc, argv);
        else if (!strcmp(argv[1], "neck")) rc = cmd_neck(argc, argv);
        else if (!strcmp(argv[1], "ircut")) rc = cmd_ircut(argc, argv);
        else if (!strcmp(argv[1], "light")) rc = cmd_light();
        else if (!strcmp(argv[1], "ioctl")) rc = cmd_ioctl(argc, argv);
    }
    if (rc == 1) fprintf(stderr,
        "usage: hkctl neck status|release|home [delay]|step h|v +|- <n> [delay]|goto <x> <y> [delay]|move <0=center,1=left,2=right,3=up,4=down>\n"
        "       hkctl ircut day|night | hkctl light | hkctl serve [port] | hkctl ioctl <dev> <cmd> [arg]\n");
    return rc;
}
