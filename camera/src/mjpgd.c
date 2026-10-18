/*
 * mjpgd - minimal MJPEG-over-HTTP streamer for the Plug2View camera.
 *
 * Why not mjpg-streamer: it mmaps 4 full-size V4L2 buffers and dies from OOM at
 * 1280x720 on this 28 MB device. The Sonix module's 640x480 mode is a centre crop,
 * so 720p is the only way to get the lens's full field of view. mjpgd uses 2 kernel
 * buffers + 2 user frame slots, a single poll() loop with non-blocking sockets, no
 * libjpeg (the camera emits complete baseline JPEGs with Huffman tables), and is a
 * static binary, so the musl runtime is no longer needed either.
 *
 * URLs are kept compatible with mjpg-streamer so the viewer page and VLC don't change:
 *   /?action=stream     multipart/x-mixed-replace MJPEG
 *   /?action=snapshot   one JPEG
 *   /api/<...>          relayed to hkctl serve on localhost (pan/tilt, IR-cut, Wi-Fi)
 *   /<file>             static file from the www directory ("/" -> index.html)
 * With -c user:pass every URL requires HTTP basic auth (so one forwarded port is enough).
 *
 * Build: zig cc -target mipsel-linux-musleabi -mcpu=mips32 -Os -static -s -o mjpgd mjpgd.c
 * Usage: mjpgd [-d /dev/video0] [-r 1280x720] [-f 15] [-p 8080] [-w /www] [-c user:pass] [-l 60|50|0]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <linux/videodev2.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <dirent.h>
#include <time.h>

/* musl (as shipped with zig) has a 64-bit time_t even on 32-bit MIPS, which makes the
 * header's struct v4l2_buffer 76 bytes; this 2.6.21 driver only knows the classic
 * 68-byte layout with 32-bit timeval, and the ioctl number encodes the size. So the
 * buffer ioctls are issued with an explicit legacy layout. */
struct v4l2_buffer32 {
    __u32 index, type, bytesused, flags, field;
    struct { __s32 tv_sec, tv_usec; } timestamp;
    struct v4l2_timecode timecode;
    __u32 sequence, memory;
    union { __u32 offset, userptr; } m;
    __u32 length, input, reserved;
};
#define VIDIOC_QUERYBUF32 _IOWR('V', 9, struct v4l2_buffer32)
#define VIDIOC_QBUF32     _IOWR('V', 15, struct v4l2_buffer32)
#define VIDIOC_DQBUF32    _IOWR('V', 17, struct v4l2_buffer32)

#define NBUF 2            /* kernel capture buffers */
#define NSLOT 2           /* user-space frame slots handed to clients */
#define MAXCLI 8
#define BOUNDARY "boundarydonotcross"

struct slot { unsigned char *data; size_t cap, len; int refs; unsigned seq; };

enum phase { P_REQ, P_HDR, P_FRAME, P_TRAIL, P_WAIT, P_FILE };
struct client {
    int fd;
    int local;                     /* connection from 127.0.0.1 (auth not required) */
    enum phase phase;
    int stream;                    /* 1 = multipart stream, 0 = single response */
    char req[1024]; int reqlen;
    char hdr[256]; int hdrlen, hdroff;
    struct slot *s; size_t off;    /* frame being sent */
    unsigned last_seq;             /* last frame sequence sent on a stream */
    unsigned char *file; size_t filelen, fileoff;   /* static file body */
};

static struct slot slots[NSLOT];
static struct client cli[MAXCLI];
static int vfd, lfd;
static void *kbuf[NBUF]; static size_t klen[NBUF];
static char auth[128] = "";       /* "Basic xxxx" expected header value, or "" */
static const char *www = "/mnt/sif/owrt/www";

static void die(const char *m) { perror(m); exit(1); }

/* ---- V4L2 ------------------------------------------------------------------ */
static int xioctl(int fd, unsigned long req, void *arg) {
    int r; do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR); return r;
}

static void cam_open(const char *dev, int w, int h, int fps) {
    vfd = open(dev, O_RDWR | O_NONBLOCK);
    if (vfd < 0) die(dev);
    struct v4l2_format f; memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = w; f.fmt.pix.height = h;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG; f.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(vfd, VIDIOC_S_FMT, &f) < 0) die("VIDIOC_S_FMT");
    fprintf(stderr, "format %ux%u MJPEG\n", f.fmt.pix.width, f.fmt.pix.height);
    struct v4l2_streamparm p; memset(&p, 0, sizeof p);
    p.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    p.parm.capture.timeperframe.numerator = 1; p.parm.capture.timeperframe.denominator = fps;
    if (xioctl(vfd, VIDIOC_S_PARM, &p) < 0) perror("VIDIOC_S_PARM (ignored)");
    struct v4l2_requestbuffers rb; memset(&rb, 0, sizeof rb);
    rb.count = NBUF; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(vfd, VIDIOC_REQBUFS, &rb) < 0) die("VIDIOC_REQBUFS");
    fprintf(stderr, "REQBUFS granted %u buffers\n", rb.count);
    if (rb.count < 1) { fprintf(stderr, "no buffers\n"); exit(1); }
    for (unsigned i = 0; i < rb.count && i < NBUF; i++) {
        struct v4l2_buffer32 b; memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        if (xioctl(vfd, VIDIOC_QUERYBUF32, &b) < 0) die("VIDIOC_QUERYBUF");
        fprintf(stderr, "buffer %u: length %u offset %u\n", i, b.length, b.m.offset);
        klen[i] = b.length;
        kbuf[i] = mmap(0, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, b.m.offset);
        if (kbuf[i] == MAP_FAILED) die("mmap");
        if (xioctl(vfd, VIDIOC_QBUF32, &b) < 0) die("VIDIOC_QBUF");
    }
    for (int i = 0; i < NSLOT; i++) {
        slots[i].cap = klen[0]; slots[i].data = malloc(klen[0]);
        if (!slots[i].data) die("malloc slot");
    }
    fprintf(stderr, "buffers: %d x %zu bytes kernel, %d slots\n", NBUF, klen[0], NSLOT);
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(vfd, VIDIOC_STREAMON, &t) < 0) die("VIDIOC_STREAMON");
}

/* Set a V4L2 user control (struct v4l2_control has no time fields, so the header's
 * layout is fine). Used for the power-line frequency (anti-flicker) setting. */
static void cam_ctrl(unsigned id, int value, const char *name) {
    struct v4l2_control c = { .id = id, .value = value };
    if (xioctl(vfd, VIDIOC_S_CTRL, &c) < 0) fprintf(stderr, "set %s=%d failed: %s\n", name, value, strerror(errno));
    else fprintf(stderr, "set %s=%d\n", name, value);
}

static unsigned frame_seq = 0;
static struct slot *latest = NULL;
static time_t last_frame_time = 0;

/* ---- recordings on the SD card ------------------------------------------------
 *   /?action=save[&tag=manual|motion]   write the latest frame to
 *                                       <save_dir>/<tag>/YYYY-MM-DD/HHMMSS-<seq>.jpg
 *   /files?dir=<rel>                    JSON listing of a directory under save_dir
 *   /rec/<rel>                          serve a saved file
 * Requests from 127.0.0.1 skip basic auth so the motion detector (hkctl) can trigger
 * saves. When free space on the card drops below SAVE_MIN_FREE_MB the oldest day is
 * deleted. */
static const char *save_dir = NULL;
#define SAVE_MIN_FREE_MB 200
static unsigned save_count = 0;

static void url_decode(char *s) {             /* %XX and + in place */
    char *o = s;
    for (; *s; s++, o++) {
        if (*s == '%' && s[1] && s[2]) { char h[3] = { s[1], s[2], 0 }; *o = (char)strtol(h, 0, 16); s += 2; }
        else if (*s == '+') *o = ' ';
        else *o = *s;
    }
    *o = 0;
}

static int is_safe_rel(const char *p) {       /* no "..", no leading "/", printable */
    if (!p || strstr(p, "..") || p[0] == '/') return 0;
    for (; *p; p++) if ((unsigned char)*p < 0x20 || *p == '\\') return 0;
    return 1;
}

static int rm_tree(const char *dir) {         /* remove a directory of files (one level) */
    DIR *d = opendir(dir); struct dirent *e; char p[512];
    if (!d) return -1;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (unlink(p) < 0) rmdir(p);
    }
    closedir(d);
    return rmdir(dir);
}

/* Delete the oldest YYYY-MM-DD directory under <save_dir>/{manual,motion}. */
static void prune_oldest(void) {
    const char *tags[2] = { "manual", "motion" };
    char oldest[512] = "", name[64] = "";
    for (int t = 0; t < 2; t++) {
        char base[300]; snprintf(base, sizeof base, "%s/%s", save_dir, tags[t]);
        DIR *d = opendir(base); struct dirent *e;
        if (!d) continue;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.' || strlen(e->d_name) != 10) continue;   /* YYYY-MM-DD */
            if (!name[0] || strcmp(e->d_name, name) < 0) { strcpy(name, e->d_name); snprintf(oldest, sizeof oldest, "%s/%s", base, e->d_name); }
        }
        closedir(d);
    }
    if (oldest[0]) { fprintf(stderr, "pruning %s\n", oldest); rm_tree(oldest); }
}

/* Pre-roll: one frame per second from the last PRE_N seconds, so a motion event can be
 * written out starting *before* the detector fired (polling + confirm add ~1 s). */
#define PRE_N 6                 /* slots: current second + the 5 before it (5 s pre-roll) */
#define PRE_CAP (250 * 1024)    /* 720p MJPEG frames are 50-120 KB; bigger ones are skipped */
static struct { unsigned char *data; size_t len; time_t t; } pre[PRE_N];
static void pre_update(void) {
    time_t now = time(NULL);
    if (!latest || latest->len > PRE_CAP || pre[0].t == now) return;   /* one per second */
    free(pre[PRE_N - 1].data);                                          /* drop the oldest */
    for (int i = PRE_N - 1; i > 0; i--) pre[i] = pre[i - 1];
    pre[0].data = malloc(latest->len);                                  /* exact size, ~60 KB */
    if (!pre[0].data) { pre[0].len = 0; pre[0].t = 0; return; }
    memcpy(pre[0].data, latest->data, latest->len); pre[0].len = latest->len; pre[0].t = now;
}

static int write_frame(const char *tag, const unsigned char *data, size_t len, time_t t, char *out, int n) {
    struct tm *tm = localtime(&t);
    char day[16], hms[32]; strftime(day, sizeof day, "%Y-%m-%d", tm); strftime(hms, sizeof hms, "%Y-%m-%d_%H-%M-%S", tm);
    char dir[400]; snprintf(dir, sizeof dir, "%s/%s", save_dir, tag); mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/%s/%s", save_dir, tag, day); mkdir(dir, 0755);
    unsigned seq = ++save_count % 1000;
    char path[512]; snprintf(path, sizeof path, "%s/%s-%03u.jpg", dir, hms, seq);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -3;
    size_t off = 0; ssize_t w;
    while (off < len && (w = write(fd, data + off, len - off)) > 0) off += w;
    close(fd);
    if (out) snprintf(out, n, "%s/%s/%s-%03u.jpg", tag, day, hms, seq);
    return off == len ? 0 : -4;
}

/* Save the latest frame; with_pre also writes the buffered previous seconds first. */
static int save_frame(const char *tag, int with_pre, char *out, int n) {
    if (!save_dir || !latest) return -1;
    struct statfs sf;
    if (statfs(save_dir, &sf) < 0) return -2;                      /* card not mounted */
    for (int i = 0; i < 3 && (unsigned long long)sf.f_bavail * sf.f_bsize < (unsigned long long)SAVE_MIN_FREE_MB << 20; i++) {
        prune_oldest(); statfs(save_dir, &sf);
    }
    time_t now = time(NULL);
    if (with_pre)
        for (int i = PRE_N - 1; i >= 0; i--)
            if (pre[i].data && pre[i].t && pre[i].t < now && now - pre[i].t <= PRE_N + 1)
                write_frame(tag, pre[i].data, pre[i].len, pre[i].t, NULL, 0);
    return write_frame(tag, latest->data, latest->len, now, out, n);
}

/* ---- AVI (MJPEG) event recording -----------------------------------------------
 *   /?action=rec&start=1&tag=motion[&pre=1]   open <tag>/<day>/<start-time>.avi (pre-roll first)
 *   /?action=rec&frame=1                       append the latest frame
 *   /?action=rec&stop=1                        finalize (write index, patch sizes)
 * One file per event instead of dozens of JPEGs. Frames are written as they are
 * requested; the header declares REC_FPS so playback runs in real time when the
 * motion daemon sends 2 frames/s. Auto-finalized if no frame arrives for 30 s. */
#define REC_FPS 2
#define REC_MAX_FRAMES 7200           /* 1 h at 2 fps, then the file is closed */
static int rec_fd = -1;
static unsigned rec_frames = 0, rec_w = 0, rec_h = 0;
static off_t rec_movi_pos = 0;        /* file offset of the 'LIST....movi' header */
static unsigned *rec_idx = NULL;      /* (offset,len) pairs for idx1 */
static time_t rec_last = 0;
static char rec_name[256];

static void put32(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void wr(int fd, const void *p, size_t n) { size_t o = 0; ssize_t w; while (o < n && (w = write(fd, (const char *)p + o, n - o)) > 0) o += w; }

static void jpeg_size(const unsigned char *d, size_t n, unsigned *w, unsigned *h) {
    for (size_t i = 2; i + 9 < n && d[i] == 0xFF;) {
        unsigned m = d[i + 1], len = d[i + 2] << 8 | d[i + 3];
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) { *h = d[i + 5] << 8 | d[i + 6]; *w = d[i + 7] << 8 | d[i + 8]; return; }
        if (m == 0xD9 || m == 0xDA) break;
        i += 2 + len;
    }
}

static void rec_header(int fd) {               /* 4 + hdrl(…) ; sizes patched at stop */
    unsigned char h[224]; memset(h, 0, sizeof h);
    memcpy(h, "RIFF", 4); put32(h + 4, 0); memcpy(h + 8, "AVI ", 4);
    memcpy(h + 12, "LIST", 4); put32(h + 16, 4 + 64 + 8 + 4 + 64 + 48); memcpy(h + 20, "hdrl", 4);
    memcpy(h + 24, "avih", 4); put32(h + 28, 56);
    put32(h + 32, 1000000 / REC_FPS);           /* dwMicroSecPerFrame */
    put32(h + 36, 200000); put32(h + 40, 0);     /* dwMaxBytesPerSec, padding */
    put32(h + 44, 0x10);                         /* dwFlags: AVIF_HASINDEX */
    put32(h + 48, 0);                            /* dwTotalFrames (patched) */
    put32(h + 52, 0); put32(h + 56, 1); put32(h + 60, 0);   /* initial frames, streams, suggested buf */
    put32(h + 64, rec_w); put32(h + 68, rec_h);
    memcpy(h + 88, "LIST", 4); put32(h + 92, 4 + 64 + 48); memcpy(h + 96, "strl", 4);
    memcpy(h + 100, "strh", 4); put32(h + 104, 56);
    memcpy(h + 108, "vids", 4); memcpy(h + 112, "MJPG", 4);
    put32(h + 116, 0); put32(h + 120, 0); put32(h + 124, 0);          /* flags, prio/lang, initial */
    put32(h + 128, 1); put32(h + 132, REC_FPS);                        /* dwScale, dwRate */
    put32(h + 136, 0); put32(h + 140, 0);                              /* start, dwLength (patched) */
    put32(h + 144, 200000); put32(h + 148, 0xFFFFFFFF); put32(h + 152, 0);
    put16(h + 156, 0); put16(h + 158, 0); put16(h + 160, rec_w); put16(h + 162, rec_h);
    memcpy(h + 164, "strf", 4); put32(h + 168, 40);
    put32(h + 172, 40); put32(h + 176, rec_w); put32(h + 180, rec_h);
    put16(h + 184, 1); put16(h + 186, 24); memcpy(h + 188, "MJPG", 4);
    put32(h + 192, rec_w * rec_h * 3); put32(h + 196, 0); put32(h + 200, 0); put32(h + 204, 0); put32(h + 208, 0);
    memcpy(h + 212, "LIST", 4); put32(h + 216, 0); memcpy(h + 220, "movi", 4);   /* size patched */
    rec_movi_pos = 212;
    wr(fd, h, sizeof h);
}

static void rec_write_frame(const unsigned char *d, size_t n) {
    if (rec_fd < 0 || rec_frames >= REC_MAX_FRAMES) return;
    unsigned char ch[8]; memcpy(ch, "00dc", 4); put32(ch + 4, n);
    off_t pos = lseek(rec_fd, 0, SEEK_CUR);
    wr(rec_fd, ch, 8); wr(rec_fd, d, n);
    if (n & 1) wr(rec_fd, "", 1);
    rec_idx[rec_frames * 2] = (unsigned)(pos - (rec_movi_pos + 8));   /* relative to 'movi' tag */
    rec_idx[rec_frames * 2 + 1] = n;
    rec_frames++; rec_last = time(NULL);
}

static void rec_stop(void) {
    if (rec_fd < 0) return;
    off_t end = lseek(rec_fd, 0, SEEK_CUR);
    unsigned char ch[8]; memcpy(ch, "idx1", 4); put32(ch + 4, rec_frames * 16);
    wr(rec_fd, ch, 8);
    for (unsigned i = 0; i < rec_frames; i++) {
        unsigned char e[16]; memcpy(e, "00dc", 4); put32(e + 4, 0x10); put32(e + 8, rec_idx[i * 2]); put32(e + 12, rec_idx[i * 2 + 1]);
        wr(rec_fd, e, 16);
    }
    off_t total = lseek(rec_fd, 0, SEEK_CUR);
    unsigned char v[4];
    put32(v, (unsigned)(total - 8)); lseek(rec_fd, 4, SEEK_SET); wr(rec_fd, v, 4);                 /* RIFF size */
    put32(v, rec_frames); lseek(rec_fd, 48, SEEK_SET); wr(rec_fd, v, 4);                           /* dwTotalFrames */
    lseek(rec_fd, 140, SEEK_SET); wr(rec_fd, v, 4);                                               /* strh dwLength */
    put32(v, (unsigned)(end - (rec_movi_pos + 8))); lseek(rec_fd, rec_movi_pos + 4, SEEK_SET); wr(rec_fd, v, 4); /* movi size */
    close(rec_fd); rec_fd = -1;
    fprintf(stderr, "recording closed: %s (%u frames)\n", rec_name, rec_frames);
    free(rec_idx); rec_idx = NULL;
}

static int rec_start(const char *tag, int with_pre, char *out, int n) {
    if (!save_dir || !latest) return -1;
    rec_stop();
    struct statfs sf;
    if (statfs(save_dir, &sf) < 0) return -2;
    for (int i = 0; i < 3 && (unsigned long long)sf.f_bavail * sf.f_bsize < (unsigned long long)SAVE_MIN_FREE_MB << 20; i++) { prune_oldest(); statfs(save_dir, &sf); }
    time_t now = time(NULL), t0 = now;
    if (with_pre) for (int i = PRE_N - 1; i >= 0; i--) if (pre[i].data && pre[i].t && pre[i].t < now && now - pre[i].t <= PRE_N + 1) { t0 = pre[i].t; break; }
    struct tm *tm = localtime(&t0);
    char day[16], hms[32]; strftime(day, sizeof day, "%Y-%m-%d", tm); strftime(hms, sizeof hms, "%Y-%m-%d_%H-%M-%S", tm);
    char dir[400]; snprintf(dir, sizeof dir, "%s/%s", save_dir, tag); mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/%s/%s", save_dir, tag, day); mkdir(dir, 0755);
    snprintf(rec_name, sizeof rec_name, "%s/%s/%s.avi", tag, day, hms);
    char path[700]; snprintf(path, sizeof path, "%s/%s", save_dir, rec_name);
    rec_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (rec_fd < 0) return -3;
    rec_idx = calloc(REC_MAX_FRAMES * 2, sizeof(unsigned));
    if (!rec_idx) { close(rec_fd); rec_fd = -1; return -3; }
    rec_frames = 0; rec_w = rec_h = 0;
    jpeg_size(latest->data, latest->len, &rec_w, &rec_h);
    rec_header(rec_fd);
    if (with_pre) for (int i = PRE_N - 1; i >= 0; i--) if (pre[i].data && pre[i].t && pre[i].t < now && now - pre[i].t <= PRE_N + 1) {
        rec_write_frame(pre[i].data, pre[i].len);
        if (REC_FPS > 1) rec_write_frame(pre[i].data, pre[i].len);   /* pre-roll is 1 fps: hold each frame */
    }
    rec_write_frame(latest->data, latest->len);
    snprintf(out, n, "%s", rec_name);
    return 0;
}

/* JSON directory listing: {"dir":"...","entries":[{"name":..,"size":..,"dir":bool},...]} */
static void list_dir(const char *rel, char *out, int n) {
    char full[600]; snprintf(full, sizeof full, "%s/%s", save_dir, rel);
    int len = snprintf(out, n, "{\"dir\":\"%s\",\"entries\":[", rel);
    DIR *d = opendir(full); struct dirent *e; int first = 1;
    if (d) {
        while ((e = readdir(d)) && len < n - 200) {
            if (e->d_name[0] == '.') continue;
            char p[700]; struct stat st; snprintf(p, sizeof p, "%s/%s", full, e->d_name);
            if (stat(p, &st) < 0) continue;
            len += snprintf(out + len, n - len, "%s{\"name\":\"%s\",\"size\":%ld,\"dir\":%s}", first ? "" : ",",
                            e->d_name, (long)st.st_size, S_ISDIR(st.st_mode) ? "true" : "false");
            first = 0;
        }
        closedir(d);
    }
    snprintf(out + len, n - len, "]}");
}

/* Dequeue a frame; copy it into a free slot (drop it if both slots are busy). */
static void cam_grab(void) {
    struct v4l2_buffer32 b; memset(&b, 0, sizeof b);
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP;
    if (xioctl(vfd, VIDIOC_DQBUF32, &b) < 0) { if (errno != EAGAIN) perror("VIDIOC_DQBUF"); return; }
    size_t n = b.bytesused;
    const unsigned char *d = kbuf[b.index];
    /* A valid JPEG starts with FFD8 and is more than a few hundred bytes. */
    if (n > 512 && n <= slots[0].cap && d[0] == 0xFF && d[1] == 0xD8) {
        struct slot *s = NULL;
        for (int i = 0; i < NSLOT; i++) if (slots[i].refs == 0 && &slots[i] != latest) { s = &slots[i]; break; }
        /* (save_frame reads `latest` synchronously in the same loop, so no extra ref needed) */
        if (!s) for (int i = 0; i < NSLOT; i++) if (slots[i].refs == 0) { s = &slots[i]; break; }
        if (s) { memcpy(s->data, d, n); s->len = n; s->seq = ++frame_seq; latest = s; last_frame_time = time(NULL); }
    }
    xioctl(vfd, VIDIOC_QBUF32, &b);
}

/* ---- HTTP helpers ------------------------------------------------------------ */
static void cli_close(struct client *c) {
    if (c->fd >= 0) close(c->fd);
    if (c->s) { c->s->refs--; c->s = NULL; }
    free(c->file);
    memset(c, 0, sizeof *c); c->fd = -1;
}

static void set_resp(struct client *c, const char *hdr, unsigned char *body, size_t len) {
    c->hdrlen = snprintf(c->hdr, sizeof c->hdr, "%s", hdr); c->hdroff = 0;
    c->file = body; c->filelen = len; c->fileoff = 0;
    c->phase = P_HDR; c->stream = 0;
}

static void send_error(struct client *c, int code, const char *msg) {
    char h[200];
    snprintf(h, sizeof h, "HTTP/1.0 %d %s\r\nContent-Type: text/plain\r\nConnection: close\r\n"
             "Content-Length: %d\r\n%s\r\n%s", code, msg, (int)strlen(msg) + 1,
             code == 401 ? "WWW-Authenticate: Basic realm=\"camera\"\r\n" : "", "");
    unsigned char *b = malloc(strlen(msg) + 2);
    sprintf((char *)b, "%s\n", msg);
    set_resp(c, h, b, strlen(msg) + 1);
}

static const char *ctype(const char *p) {
    const char *e = strrchr(p, '.');
    if (!e) return "application/octet-stream";
    if (!strcmp(e, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(e, ".jpg") || !strcmp(e, ".jpeg")) return "image/jpeg";
    if (!strcmp(e, ".png")) return "image/png";
    if (!strcmp(e, ".css")) return "text/css";
    if (!strcmp(e, ".js")) return "application/javascript";
    if (!strcmp(e, ".json")) return "application/json";
    if (!strcmp(e, ".ico")) return "image/x-icon";
    if (!strcmp(e, ".avi")) return "video/x-msvideo";
    if (!strcmp(e, ".txt")) return "text/plain";
    return "application/octet-stream";
}

static void start_stream_part(struct client *c) {
    if (!latest || latest->seq == c->last_seq) { c->phase = P_WAIT; return; }   /* wait for a new frame */
    c->s = latest; c->s->refs++; c->off = 0; c->last_seq = c->s->seq;
    c->hdrlen = snprintf(c->hdr, sizeof c->hdr,
        "--" BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\nX-Frame: %u\r\n\r\n",
        c->s->len, c->s->seq);
    c->hdroff = 0; c->phase = P_HDR;
}

/* /api/... is relayed to the hkctl control server on localhost, so the page needs only
 * this one (password-protected) port. Done in a forked child with blocking I/O so a
 * long pan/tilt move or Wi-Fi scan never stalls the video loop. */
static int ctl_port = 8081;
static void proxy_api(struct client *c, const char *raw, int rawlen, const char *method, const char *path) {
    pid_t p = fork();
    if (p != 0) return;                         /* parent (or fork failure): caller closes */
    alarm(60);                                  /* never outlive a stuck request */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(ctl_port), .sin_addr.s_addr = htonl(0x7f000001) };
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        const char *m = "HTTP/1.0 502 Bad Gateway\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"control API offline\"}";
        write(c->fd, m, strlen(m)); _exit(0);
    }
    fcntl(c->fd, F_SETFL, 0);                   /* blocking from here on */
    /* Request line with /api stripped, then the original headers (+ body if any). */
    char line[1200]; int n = snprintf(line, sizeof line, "%s %s HTTP/1.0\r\n", method, path);
    write(s, line, n);
    const char *hdrs = strstr(raw, "\r\n");
    if (hdrs) {
        write(s, hdrs + 2, rawlen - (hdrs + 2 - raw));
        /* body bytes not yet read (POST): relay until Content-Length satisfied */
        const char *cl = strstr(raw, "Content-Length:"), *be = strstr(raw, "\r\n\r\n");
        if (cl && be) {
            long want = atol(cl + 15), have = rawlen - (be + 4 - raw);
            char buf[1024];
            while (have < want) { int r = read(c->fd, buf, sizeof buf); if (r <= 0) break; write(s, buf, r); have += r; }
        }
    }
    char buf[2048]; int r;
    while ((r = read(s, buf, sizeof buf)) > 0) if (write(c->fd, buf, r) < 0) break;
    _exit(0);
}

static void handle_request(struct client *c) {
    char raw[sizeof c->req]; memcpy(raw, c->req, c->reqlen); int rawlen = c->reqlen;
    char *line_end = strstr(c->req, "\r\n");
    if (line_end) *line_end = 0;
    char *path = strchr(c->req, ' ');
    int is_get = !strncmp(c->req, "GET ", 4), is_post = !strncmp(c->req, "POST ", 5);
    if (!path || (!is_get && !is_post)) { send_error(c, 400, "Bad Request"); return; }
    path++;
    char *sp = strchr(path, ' '); if (sp) *sp = 0;
    if (auth[0] && !c->local) {
        const char *a = line_end ? strstr(line_end + 1, "Authorization: ") : NULL;
        if (!a || strncmp(a + 15, auth, strlen(auth))) { send_error(c, 401, "Unauthorized"); return; }
    }
    if (!strncmp(path, "/api/", 5)) {
        proxy_api(c, raw, rawlen, is_post ? "POST" : "GET", path + 4);
        cli_close(c);                            /* parent: the child owns the connection now */
        return;
    }
    if (is_post) { send_error(c, 405, "Method Not Allowed"); return; }
    char *q = strchr(path, '?'); if (q) *q++ = 0;
    if (q && strstr(q, "action=rec")) {
        char body[400]; int r = 0;
        if (strstr(q, "start=1")) {
            char rel[256]; const char *tag = strstr(q, "tag=manual") ? "manual" : "motion";
            r = rec_start(tag, strstr(q, "pre=1") != NULL, rel, sizeof rel);
            snprintf(body, sizeof body, r == 0 ? "{\"recording\":\"%s\"}" : "{\"error\":\"cannot start recording (%s)\"}", r == 0 ? rel : r == -2 ? "no SD card" : "write failed");
        } else if (strstr(q, "frame=1")) {
            if (rec_fd >= 0 && latest) rec_write_frame(latest->data, latest->len); else r = -1;
            snprintf(body, sizeof body, r == 0 ? "{\"frames\":%u}" : "{\"error\":\"not recording\"}", rec_frames);
        } else if (strstr(q, "stop=1")) {
            unsigned fr = rec_frames; char nm[256]; snprintf(nm, sizeof nm, "%s", rec_name);
            rec_stop();
            snprintf(body, sizeof body, "{\"closed\":\"%s\",\"frames\":%u}", nm, fr);
        } else { r = -1; snprintf(body, sizeof body, "{\"error\":\"start=1|frame=1|stop=1\"}"); }
        char h[200]; snprintf(h, sizeof h, "HTTP/1.0 %d OK\r\nConnection: close\r\nContent-Type: application/json\r\n"
                 "Cache-Control: no-store\r\nContent-Length: %d\r\n\r\n", r == 0 ? 200 : 500, (int)strlen(body));
        set_resp(c, h, (unsigned char *)strdup(body), strlen(body));
        return;
    }
    if (q && strstr(q, "action=save")) {
        char rel[256], body[400]; const char *tag = strstr(q, "tag=motion") ? "motion" : "manual";
        int r = save_frame(tag, strstr(q, "pre=1") != NULL, rel, sizeof rel);
        if (r == 0) { snprintf(body, sizeof body, "{\"saved\":\"%s\"}", rel); }
        else snprintf(body, sizeof body, "{\"error\":\"%s\"}", r == -2 ? "no SD card mounted" : r == -1 ? "no frame or saving disabled" : "write failed");
        char h[200]; snprintf(h, sizeof h, "HTTP/1.0 %d OK\r\nConnection: close\r\nContent-Type: application/json\r\n"
                 "Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\nContent-Length: %d\r\n\r\n", r == 0 ? 200 : 500, (int)strlen(body));
        unsigned char *b = (unsigned char *)strdup(body);
        set_resp(c, h, b, strlen(body));
        return;
    }
    if (q && strstr(q, "action=delete")) {
        /* /?action=delete&path=<tag>/<day>[/<file>] — only inside the recordings tree */
        char rel[300] = ""; const char *d = strstr(q, "path=");
        if (d) { int i = 0; d += 5; while (*d && *d != '&' && i < 299) rel[i++] = *d++; rel[i] = 0; }
        url_decode(rel);
        int depth = 0; for (const char *s = rel; *s; s++) if (*s == '/') depth++;
        int okpath = save_dir && is_safe_rel(rel) && depth >= 1 && depth <= 2 &&
                     (!strncmp(rel, "manual/", 7) || !strncmp(rel, "motion/", 7));
        char body[400]; int r = -1;
        if (okpath) {
            char full[700]; struct stat st; snprintf(full, sizeof full, "%s/%s", save_dir, rel);
            if (stat(full, &st) == 0) r = S_ISDIR(st.st_mode) ? rm_tree(full) : unlink(full);
        }
        snprintf(body, sizeof body, r == 0 ? "{\"deleted\":\"%s\"}" : "{\"error\":\"cannot delete %s\"}", rel);
        char h[200]; snprintf(h, sizeof h, "HTTP/1.0 %d OK\r\nConnection: close\r\nContent-Type: application/json\r\n"
                 "Cache-Control: no-store\r\nContent-Length: %d\r\n\r\n", r == 0 ? 200 : 400, (int)strlen(body));
        set_resp(c, h, (unsigned char *)strdup(body), strlen(body));
        return;
    }
    if (!strcmp(path, "/files")) {
        char rel[256] = ""; static char big[8192];
        if (q) { const char *d = strstr(q, "dir="); if (d) { int i = 0; d += 4; while (*d && *d != '&' && i < 255) rel[i++] = *d++; rel[i] = 0; } }
        url_decode(rel);
        if (!save_dir || !is_safe_rel(rel)) { send_error(c, 400, "bad dir"); return; }
        list_dir(rel, big, sizeof big);
        char h[200]; snprintf(h, sizeof h, "HTTP/1.0 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\n"
                 "Cache-Control: no-store\r\nContent-Length: %d\r\n\r\n", (int)strlen(big));
        set_resp(c, h, (unsigned char *)strdup(big), strlen(big));
        return;
    }
    if (!strncmp(path, "/rec/", 5)) {
        url_decode(path + 5);
        if (!save_dir || !is_safe_rel(path + 5)) { send_error(c, 403, "Forbidden"); return; }
        char full[700]; snprintf(full, sizeof full, "%s/%s", save_dir, path + 5);
        int fd = open(full, O_RDONLY);
        off_t len = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
        if (fd < 0 || len <= 0) { if (fd >= 0) close(fd); send_error(c, 404, "Not Found"); return; }
        lseek(fd, 0, SEEK_SET);
        /* Recordings can be many MB: stream them from a forked child (blocking I/O) instead
         * of loading into RAM — the main loop must keep serving video. */
        pid_t p = fork();
        if (p == 0) {
            alarm(600);
            fcntl(c->fd, F_SETFL, 0);
            char h[256]; int hl = snprintf(h, sizeof h, "HTTP/1.0 200 OK\r\nConnection: close\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                     "Cache-Control: max-age=86400\r\n\r\n", ctype(path), (long)len);
            wr(c->fd, h, hl);
            static unsigned char buf[16384]; ssize_t r;
            while ((r = read(fd, buf, sizeof buf)) > 0) { size_t o = 0; ssize_t w; while (o < (size_t)r && (w = write(c->fd, buf + o, r - o)) > 0) o += w; if (o < (size_t)r) break; }
            _exit(0);
        }
        close(fd);
        cli_close(c);
        return;
    }
    if (q && strstr(q, "action=stream")) {
        c->stream = 1;
        c->hdrlen = snprintf(c->hdr, sizeof c->hdr,
            "HTTP/1.0 200 OK\r\nConnection: close\r\nCache-Control: no-store\r\nPragma: no-cache\r\n"
            "Access-Control-Allow-Origin: *\r\nContent-Type: multipart/x-mixed-replace;boundary=" BOUNDARY "\r\n\r\n");
        c->hdroff = 0; c->phase = P_HDR; c->s = NULL;      /* after header -> P_TRAIL -> next part */
        return;
    }
    if (q && strstr(q, "action=snapshot")) {
        if (!latest) { send_error(c, 503, "No frame yet"); return; }
        c->s = latest; c->s->refs++; c->off = 0;
        c->hdrlen = snprintf(c->hdr, sizeof c->hdr,
            "HTTP/1.0 200 OK\r\nConnection: close\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n"
            "Content-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", c->s->len);
        c->hdroff = 0; c->phase = P_HDR; c->stream = 0;
        return;
    }
    /* static file */
    if (strstr(path, "..")) { send_error(c, 403, "Forbidden"); return; }
    if (!strcmp(path, "/")) path = "/index.html";
    char full[300]; snprintf(full, sizeof full, "%s%s", www, path);
    int fd = open(full, O_RDONLY);
    if (fd < 0) { send_error(c, 404, "Not Found"); return; }
    off_t len = lseek(fd, 0, SEEK_END); lseek(fd, 0, SEEK_SET);
    if (len <= 0 || len > 4 * 1024 * 1024) { close(fd); send_error(c, 404, "Not Found"); return; }
    unsigned char *b = malloc(len);
    if (!b || read(fd, b, len) != len) { close(fd); free(b); send_error(c, 500, "Read error"); return; }
    close(fd);
    char h[256];
    snprintf(h, sizeof h, "HTTP/1.0 200 OK\r\nConnection: close\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
             "Cache-Control: no-cache\r\n\r\n", ctype(path), (long)len);
    set_resp(c, h, b, len);
}

/* Write as much as the socket accepts; advance the phase machine. Returns 0 to keep,
 * -1 to close. */
static int cli_write(struct client *c) {
    for (;;) {
        const unsigned char *p; size_t left;
        if (c->phase == P_HDR) { p = (unsigned char *)c->hdr + c->hdroff; left = c->hdrlen - c->hdroff; }
        else if (c->phase == P_FRAME) { p = c->s->data + c->off; left = c->s->len - c->off; }
        else if (c->phase == P_TRAIL) { p = (const unsigned char *)"\r\n"; left = 2 - c->off; p += c->off; }
        else if (c->phase == P_FILE) { p = c->file + c->fileoff; left = c->filelen - c->fileoff; }
        else return 0;
        if (left == 0) {
            /* advance */
            if (c->phase == P_HDR) {
                if (c->stream) { if (c->s) { c->off = 0; c->phase = P_FRAME; } else { c->off = 0; start_stream_part(c); } }
                else if (c->s) { c->off = 0; c->phase = P_FRAME; }
                else if (c->file) { c->phase = P_FILE; }
                else return -1;
            } else if (c->phase == P_FRAME) {
                if (c->stream) { c->s->refs--; c->s = NULL; c->off = 0; c->phase = P_TRAIL; }
                else return -1;                              /* snapshot done */
            } else if (c->phase == P_TRAIL) {
                c->off = 0; start_stream_part(c);            /* next frame or wait */
                if (c->phase == P_WAIT) return 0;
            } else if (c->phase == P_FILE) return -1;
            continue;
        }
        ssize_t n = write(c->fd, p, left);
        if (n < 0) { if (errno == EAGAIN || errno == EINTR) return 0; return -1; }
        if (c->phase == P_HDR) c->hdroff += n;
        else if (c->phase == P_FRAME || c->phase == P_TRAIL) c->off += n;
        else c->fileoff += n;
    }
}

/* ---- main --------------------------------------------------------------------- */
static void b64(const char *in, char *out) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(in); int o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = t[v >> 18 & 63]; out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < n ? t[v >> 6 & 63] : '='; out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[o] = 0;
}

int main(int argc, char **argv) {
    const char *dev = "/dev/video0"; int w = 1280, h = 720, fps = 15, port = 8080, plfreq = 60;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "-d")) dev = argv[i + 1];
        else if (!strcmp(argv[i], "-l")) plfreq = atoi(argv[i + 1]);   /* mains frequency: 50, 60 or 0 (off) */
        else if (!strcmp(argv[i], "-a")) ctl_port = atoi(argv[i + 1]); /* hkctl serve port for /api/ proxy */
        else if (!strcmp(argv[i], "-s")) save_dir = argv[i + 1];      /* recordings directory (SD card) */
        else if (!strcmp(argv[i], "-r")) sscanf(argv[i + 1], "%dx%d", &w, &h);
        else if (!strcmp(argv[i], "-f")) fps = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-p")) port = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-w")) www = argv[i + 1];
        else if (!strcmp(argv[i], "-c") && argv[i + 1][0]) { char e[128]; b64(argv[i + 1], e); snprintf(auth, sizeof auth, "Basic %s", e); }
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);                   /* proxy children are not waited for */
    for (int i = 0; i < MAXCLI; i++) cli[i].fd = -1;
    cam_open(dev, w, h, fps);
    last_frame_time = time(NULL);
    /* Anti-flicker: the vendor shipped the module set to 50 Hz; wrong for 60 Hz mains
     * (rolling dark band in the picture). 0 = disabled, 1 = 50 Hz, 2 = 60 Hz. */
    cam_ctrl(V4L2_CID_POWER_LINE_FREQUENCY, plfreq == 60 ? 2 : plfreq == 50 ? 1 : 0, "power_line_frequency");

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY };
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(lfd, 8) < 0) die("listen");
    fcntl(lfd, F_SETFL, O_NONBLOCK);
    fprintf(stderr, "mjpgd: %s %dx%d @%d fps, http port %d, www %s%s\n", dev, w, h, fps, port, www, auth[0] ? ", auth on" : "");

    struct pollfd pf[MAXCLI + 2];
    for (;;) {
        int np = 0;
        pf[np].fd = vfd; pf[np].events = POLLIN; np++;
        pf[np].fd = lfd; pf[np].events = POLLIN; np++;
        for (int i = 0; i < MAXCLI; i++) if (cli[i].fd >= 0) {
            pf[np].fd = cli[i].fd;
            pf[np].events = cli[i].phase == P_REQ ? POLLIN : (cli[i].phase == P_WAIT ? 0 : POLLOUT);
            np++;
        }
        if (poll(pf, np, 1000) < 0) { if (errno == EINTR) continue; die("poll"); }
        int k = 0;
        if (pf[k++].revents & POLLIN) {
            cam_grab();
            if (save_dir) pre_update();
            for (int i = 0; i < MAXCLI; i++) if (cli[i].fd >= 0 && cli[i].phase == P_WAIT) {
                start_stream_part(&cli[i]);
                if (cli[i].phase != P_WAIT && cli_write(&cli[i]) < 0) cli_close(&cli[i]);
            }
        }
        if (pf[k++].revents & POLLIN) {
            struct sockaddr_in peer; socklen_t pl = sizeof peer;
            int fd = accept(lfd, (struct sockaddr *)&peer, &pl);
            if (fd >= 0) {
                int j; for (j = 0; j < MAXCLI && cli[j].fd >= 0; j++);
                if (j == MAXCLI) close(fd);
                else {
                    fcntl(fd, F_SETFL, O_NONBLOCK);
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    memset(&cli[j], 0, sizeof cli[j]); cli[j].fd = fd; cli[j].phase = P_REQ;
                    cli[j].local = ntohl(peer.sin_addr.s_addr) == 0x7f000001;
                }
            }
        }
        /* Stall watchdog: the UVC stream has died (USB hiccup) if no frame arrives for
         * 10 s; exit so cron's hk-watch restarts us within a minute. */
        if (last_frame_time && time(NULL) - last_frame_time > 10) { fprintf(stderr, "no frames for 10 s, exiting for restart\n"); rec_stop(); exit(3); }
        /* Recording left open (daemon died mid-event): finalize so the file stays playable. */
        if (rec_fd >= 0 && time(NULL) - rec_last > 30) { fprintf(stderr, "recording idle, closing\n"); rec_stop(); }
        for (int i = 0; i < MAXCLI; i++) if (cli[i].fd >= 0) {
            struct pollfd *p = NULL;
            for (int m = 2; m < np; m++) if (pf[m].fd == cli[i].fd) { p = &pf[m]; break; }
            if (!p || !p->revents) continue;
            if (p->revents & (POLLERR | POLLHUP | POLLNVAL)) { cli_close(&cli[i]); continue; }
            struct client *c = &cli[i];
            if (c->phase == P_REQ && (p->revents & POLLIN)) {
                int n = read(c->fd, c->req + c->reqlen, sizeof c->req - 1 - c->reqlen);
                if (n <= 0) { if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue; cli_close(c); continue; }
                c->reqlen += n; c->req[c->reqlen] = 0;
                if (strstr(c->req, "\r\n\r\n")) { handle_request(c); if (c->fd >= 0 && cli_write(c) < 0) cli_close(c); }
                else if (c->reqlen >= (int)sizeof c->req - 1) cli_close(c);
            } else if (p->revents & POLLOUT) {
                if (cli_write(c) < 0) cli_close(c);
            }
        }
    }
}
