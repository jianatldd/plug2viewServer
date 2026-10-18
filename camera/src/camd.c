/*
 * camd - camera daemon for the Plug2View camera: H.264 live view + MP4 recording.
 *
 * Captures the Sonix module's hardware H.264 stream (/dev/video1, 1280x720, Annex-B with
 * SPS/PPS before every frame) and serves it to browsers and VLC; records motion events
 * and manual clips as MP4 with a pre-roll kept in RAM; grabs a JPEG from the MJPEG stream
 * (/dev/video0) on demand for snapshots. Successor of mjpgd: one open V4L2 stream instead
 * of two keeps RAM free on this 28 MB device. Static binary, no libraries.
 *
 *   /                        viewer page (static files from the www directory)
 *   /?action=fmp4            live stream as fragmented MP4 (the page plays it via MSE)
 *   /?action=raw             live stream as plain Annex-B (VLC: vlc --demux h264 <url>)
 *   /?action=snapshot        one JPEG (grabbed from /dev/video0 on demand)
 *   /?action=save            snapshot -> <save_dir>/manual/<day>/<time>.jpg
 *   /?action=rec&start=1[&tag=motion|manual][&pre=1]   open an MP4, pre-roll first
 *   /?action=rec&stop=1      finalize it (also auto-finalized after 30 s without stop)
 *   /?action=status          JSON: fps, bitrate, viewers, recording, memory
 *   /files?dir=<rel>  /rec/<rel>  /?action=delete&path=<rel>   recordings browser
 *   /api/<...>               relayed to hkctl serve on localhost
 * With -c user:pass every URL needs HTTP basic auth (loopback is exempt).
 *
 * Build: zig cc -target mipsel-linux-musleabi -mcpu=mips32 -Os -static -s -o camd camd.c tls.c bearssl/libbearssl.a
 * Usage: camd [-d /dev/video1] [-j /dev/video0] [-p 8080] [-w /www] [-s /media/sda1/plug2view]
 *             [-c user:pass] [-l 60|50|0] [-a 8081] [-t 8443 -C cert.der -K key.der]   (HTTPS, optional)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <linux/videodev2.h>
#include "tls.h"

/* ---- old-kernel V4L2 buffer layout (musl has 64-bit time_t; the 2.6.21 driver wants
 * the classic 68-byte struct; the ioctl number encodes the size) --------------------- */
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

#define NBUF 2
#define MAXCLI 10
#define RING_MAX 256                 /* frames kept for pre-roll / late joiners */
#define RING_BYTES (2000 * 1024)     /* ~9 s at 1.7 Mbit/s: pre-roll + slack for live viewers */
#define PRE_SECONDS 5
#define SAVE_MIN_FREE_MB 200
#define REC_MAX_SECONDS 3600
#define REC_IDLE_CLOSE 30

static int xioctl(int fd, unsigned long req, void *arg) { int r; do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR); return r; }
static void die(const char *m) { perror(m); exit(1); }
static long long now_us(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec * 1000000LL + tv.tv_usec; }
static void wr(int fd, const void *p, size_t n) { size_t o = 0; ssize_t w; while (o < n && (w = write(fd, (const char *)p + o, n - o)) > 0) o += w; }

/* ---- H.264 frame ring ------------------------------------------------------------ */
struct hframe { unsigned char *data; size_t len; int key; long long ts; unsigned seq; };
static struct hframe ring[RING_MAX];
static unsigned ring_head = 0, ring_count = 0;   /* ring[(head - 1) % RING_MAX] is newest */
static size_t ring_bytes = 0;
static unsigned frame_seq = 0;
static time_t last_frame_time = 0;
static unsigned stat_frames = 0; static unsigned long long stat_bytes = 0; static long long stat_t0 = 0;
static double stat_fps = 0, stat_mbps = 0;

static struct hframe *ring_at(unsigned i) { return &ring[(ring_head + RING_MAX - ring_count + i) % RING_MAX]; } /* i = 0 oldest */
static struct hframe *ring_newest(void) { return ring_count ? ring_at(ring_count - 1) : NULL; }

/* NAL walk: Annex-B start codes. Returns 1 if the access unit contains an IDR slice. */
static int au_is_key(const unsigned char *d, size_t n) {
    for (size_t i = 0; i + 4 < n; i++)
        if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (d[i + 2] == 0 && d[i + 3] == 1))) {
            size_t o = d[i + 2] == 1 ? i + 3 : i + 4;
            if ((d[o] & 31) == 5) return 1;
            i = o;
        }
    return 0;
}

static void ring_push(const unsigned char *d, size_t n, long long ts) {
    while (ring_count && (ring_count >= RING_MAX || ring_bytes + n > RING_BYTES)) {
        struct hframe *old = ring_at(0); ring_bytes -= old->len; free(old->data); old->data = NULL; old->len = 0; old->seq = 0; ring_count--;
    }
    struct hframe *f = &ring[ring_head];
    f->data = malloc(n);
    if (!f->data) return;
    memcpy(f->data, d, n); f->len = n; f->key = au_is_key(d, n); f->ts = ts; f->seq = ++frame_seq;
    ring_head = (ring_head + 1) % RING_MAX; ring_count++; ring_bytes += n;
}

/* ---- V4L2 capture (H.264 from video1) ------------------------------------------- */
static int vfd = -1; static void *kbuf[NBUF]; static size_t klen[NBUF];
static const char *h264_dev = "/dev/video1";
static void cam_close(void) {
    if (vfd < 0) return;
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE; xioctl(vfd, VIDIOC_STREAMOFF, &t);
    for (int i = 0; i < NBUF; i++) if (kbuf[i]) { munmap(kbuf[i], klen[i]); kbuf[i] = NULL; }
    struct v4l2_requestbuffers rb; memset(&rb, 0, sizeof rb); rb.type = t; rb.memory = V4L2_MEMORY_MMAP; xioctl(vfd, VIDIOC_REQBUFS, &rb);
    close(vfd); vfd = -1;
}
static int rec_active = 0;
static void rec_frame(const struct hframe *f);

static int cam_open(const char *dev, unsigned fourcc, int w, int h, int nbuf) {
    int fd = open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) { perror(dev); return -1; }
    struct v4l2_format f; memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; f.fmt.pix.width = w; f.fmt.pix.height = h;
    f.fmt.pix.pixelformat = fourcc; f.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(fd, VIDIOC_S_FMT, &f) < 0) { perror("VIDIOC_S_FMT"); close(fd); return -1; }
    struct v4l2_requestbuffers rb; memset(&rb, 0, sizeof rb);
    rb.count = nbuf; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 1) { perror("VIDIOC_REQBUFS"); close(fd); return -1; }
    for (unsigned i = 0; i < rb.count && i < NBUF; i++) {
        struct v4l2_buffer32 b; memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF32, &b) < 0) { perror("VIDIOC_QUERYBUF"); close(fd); return -1; }
        klen[i] = b.length;
        kbuf[i] = mmap(0, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        if (kbuf[i] == MAP_FAILED) { perror("mmap"); close(fd); return -1; }
        xioctl(fd, VIDIOC_QBUF32, &b);
    }
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) { perror("VIDIOC_STREAMON"); close(fd); return -1; }
    return fd;
}

static void cam_grab(void) {
    struct v4l2_buffer32 b; memset(&b, 0, sizeof b);
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP;
    if (xioctl(vfd, VIDIOC_DQBUF32, &b) < 0) { if (errno != EAGAIN) perror("VIDIOC_DQBUF"); return; }
    size_t n = b.bytesused; const unsigned char *d = kbuf[b.index];
    if (n > 8 && n <= klen[b.index] && d[0] == 0 && d[1] == 0) {
        long long ts = now_us();
        ring_push(d, n, ts);
        last_frame_time = time(NULL);
        stat_frames++; stat_bytes += n;
        if (!stat_t0) stat_t0 = ts;
        if (ts - stat_t0 >= 5000000) { stat_fps = stat_frames * 1e6 / (ts - stat_t0); stat_mbps = stat_bytes * 8.0 / (ts - stat_t0); stat_frames = 0; stat_bytes = 0; stat_t0 = ts; }
        if (rec_active) rec_frame(ring_newest());
    }
    xioctl(vfd, VIDIOC_QBUF32, &b);
}

static void cam_ctrl(int fd, unsigned id, int value, const char *name) {
    struct v4l2_control c = { .id = id, .value = value };
    if (xioctl(fd, VIDIOC_S_CTRL, &c) < 0) fprintf(stderr, "set %s=%d failed: %s\n", name, value, strerror(errno));
    else fprintf(stderr, "set %s=%d\n", name, value);
}

/* ---- on-demand JPEG from the MJPEG stream (video0) --------------------------------
 * Opens, grabs a few frames (the first ones can be dark while exposure settles), keeps
 * the last, closes - so the second set of kernel buffers exists only for ~1 s. */
static const char *jpeg_dev = "/dev/video0";
static unsigned char *jpeg_grab(size_t *outlen) {
    int fd = open(jpeg_dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) { fprintf(stderr, "snapshot: open %s: %s\n", jpeg_dev, strerror(errno)); return NULL; }
    struct v4l2_format f; memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; f.fmt.pix.width = 1280; f.fmt.pix.height = 720;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG; f.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(fd, VIDIOC_S_FMT, &f) < 0) { fprintf(stderr, "snapshot: S_FMT: %s\n", strerror(errno)); close(fd); return NULL; }
    struct v4l2_requestbuffers rb; memset(&rb, 0, sizeof rb);
    rb.count = 1; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 1) { fprintf(stderr, "snapshot: REQBUFS: %s (count %u)\n", strerror(errno), rb.count); close(fd); return NULL; }
    struct v4l2_buffer32 b; memset(&b, 0, sizeof b);
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP; b.index = 0;
    if (xioctl(fd, VIDIOC_QUERYBUF32, &b) < 0) { fprintf(stderr, "snapshot: QUERYBUF: %s\n", strerror(errno)); close(fd); return NULL; }
    size_t blen = b.length;
    unsigned char *m = mmap(0, blen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
    if (m == MAP_FAILED) { fprintf(stderr, "snapshot: mmap: %s\n", strerror(errno)); close(fd); return NULL; }
    xioctl(fd, VIDIOC_QBUF32, &b);
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    unsigned char *out = NULL; *outlen = 0;
    if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) fprintf(stderr, "snapshot: STREAMON: %s\n", strerror(errno));
    else {
        for (int i = 0; i < 4; i++) {                 /* take the 4th frame */
            struct pollfd p = { fd, POLLIN, 0 };
            if (poll(&p, 1, 1500) <= 0) { fprintf(stderr, "snapshot: no frame within 1.5 s\n"); break; }
            struct v4l2_buffer32 q; memset(&q, 0, sizeof q); q.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; q.memory = V4L2_MEMORY_MMAP;
            if (xioctl(fd, VIDIOC_DQBUF32, &q) < 0) { if (errno == EAGAIN) { i--; continue; } break; }
            if (i == 3 && q.bytesused > 512 && m[0] == 0xFF && m[1] == 0xD8) { out = malloc(q.bytesused); if (out) { memcpy(out, m, q.bytesused); *outlen = q.bytesused; } }
            xioctl(fd, VIDIOC_QBUF32, &q);
        }
        xioctl(fd, VIDIOC_STREAMOFF, &t);
    }
    munmap(m, blen);
    rb.count = 0; xioctl(fd, VIDIOC_REQBUFS, &rb);  /* release kernel buffers */
    close(fd);
    return out;
}

/* ---- recordings directory helpers ----------------------------------------------- */
static const char *save_dir = NULL;

static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++, o++) {
        if (*s == '%' && s[1] && s[2]) { char h[3] = { s[1], s[2], 0 }; *o = (char)strtol(h, 0, 16); s += 2; }
        else if (*s == '+') *o = ' ';
        else *o = *s;
    }
    *o = 0;
}
static int is_safe_rel(const char *p) {
    if (!p || strstr(p, "..") || p[0] == '/') return 0;
    for (; *p; p++) if ((unsigned char)*p < 0x20 || *p == '\\') return 0;
    return 1;
}
static int rm_tree(const char *dir) {
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
static void prune_oldest(void) {
    const char *tags[2] = { "manual", "motion" };
    char oldest[512] = "", name[64] = "";
    for (int t = 0; t < 2; t++) {
        char base[300]; snprintf(base, sizeof base, "%s/%s", save_dir, tags[t]);
        DIR *d = opendir(base); struct dirent *e;
        if (!d) continue;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.' || strlen(e->d_name) != 10) continue;
            if (!name[0] || strcmp(e->d_name, name) < 0) { strcpy(name, e->d_name); snprintf(oldest, sizeof oldest, "%s/%s", base, e->d_name); }
        }
        closedir(d);
    }
    if (oldest[0]) { fprintf(stderr, "pruning %s\n", oldest); rm_tree(oldest); }
}
static int ensure_space(void) {
    struct statfs sf;
    if (!save_dir || statfs(save_dir, &sf) < 0) return -1;
    for (int i = 0; i < 3 && (unsigned long long)sf.f_bavail * sf.f_bsize < (unsigned long long)SAVE_MIN_FREE_MB << 20; i++) { prune_oldest(); statfs(save_dir, &sf); }
    return 0;
}
/* <save_dir>/<tag>/<YYYY-MM-DD>/<YYYY-MM-DD_HH-MM-SS>.<ext>; returns fd, fills rel */
static int open_recfile(const char *tag, const char *ext, time_t t, char *rel, int n) {
    struct tm *tm = localtime(&t);
    char day[16], stamp[32]; strftime(day, sizeof day, "%Y-%m-%d", tm); strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", tm);
    char dir[400]; snprintf(dir, sizeof dir, "%s/%s", save_dir, tag); mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/%s/%s", save_dir, tag, day); mkdir(dir, 0755);
    snprintf(rel, n, "%s/%s/%s.%s", tag, day, stamp, ext);
    char path[700]; snprintf(path, sizeof path, "%s/%s", save_dir, rel);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0 && errno == EEXIST) {                 /* second file in the same second */
        snprintf(rel, n, "%s/%s/%s-%u.%s", tag, day, stamp, (unsigned)(now_us() % 1000), ext);
        snprintf(path, sizeof path, "%s/%s", save_dir, rel);
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    return fd;
}
static void list_dir(const char *rel, char *out, int n) {
    char full[600]; snprintf(full, sizeof full, "%s/%s", save_dir, rel);
    int len = snprintf(out, n, "{\"dir\":\"%s\",\"entries\":[", rel);
    DIR *d = opendir(full); struct dirent *e; int first = 1;
    if (d) {
        while ((e = readdir(d)) && len < n - 200) {
            if (e->d_name[0] == '.') continue;
            char p[700]; struct stat st; snprintf(p, sizeof p, "%s/%s", full, e->d_name);
            if (stat(p, &st) < 0) continue;
            len += snprintf(out + len, n - len, "%s{\"name\":\"%s\",\"size\":%ld,\"dir\":%s}", first ? "" : ",", e->d_name, (long)st.st_size, S_ISDIR(st.st_mode) ? "true" : "false");
            first = 0;
        }
        closedir(d);
    }
    snprintf(out + len, n - len, "]}");
}

/* ---- MP4 writer (ISO BMFF, one video track, avc1) ---------------------------------
 * Samples are written to mdat as they arrive (AVCC: 4-byte length prefixes, SPS/PPS/AUD
 * stripped and kept for avcC); the moov with the sample tables is written at stop. */
#define REC_MAX_SAMPLES (REC_MAX_SECONDS * 30)
static int rec_fd = -1;
static unsigned rec_nsamples = 0, rec_nkeys = 0;
static unsigned *rec_size = NULL, *rec_dur = NULL, *rec_keys = NULL;
static unsigned long long *rec_off = NULL;
static long long rec_last_ts = 0, rec_start_ts = 0;
static time_t rec_last_write = 0;
static unsigned char rec_sps[128], rec_pps[64]; static int rec_sps_len = 0, rec_pps_len = 0;
static unsigned rec_w = 1280, rec_h = 720;
static char rec_name[256];
static unsigned long long rec_mdat_pos = 0;
static int rec_waiting_key = 0;

static void be32(unsigned char *p, unsigned v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void be16(unsigned char *p, unsigned v) { p[0] = v >> 8; p[1] = v; }

/* Box builder into a growable buffer. */
struct bb { unsigned char *d; size_t n, cap; };
static void bb_put(struct bb *b, const void *p, size_t n) { if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 1024; b->d = realloc(b->d, b->cap); } memcpy(b->d + b->n, p, n); b->n += n; }
static void bb_u32(struct bb *b, unsigned v) { unsigned char t[4]; be32(t, v); bb_put(b, t, 4); }
static void bb_u16(struct bb *b, unsigned v) { unsigned char t[2]; be16(t, v); bb_put(b, t, 2); }
static void bb_u8(struct bb *b, unsigned v) { unsigned char t = v; bb_put(b, &t, 1); }
static size_t box_begin(struct bb *b, const char *type) { size_t at = b->n; bb_u32(b, 0); bb_put(b, type, 4); return at; }
static void box_end(struct bb *b, size_t at) { be32(b->d + at, (unsigned)(b->n - at)); }
static void bb_zero(struct bb *b, int n) { for (int i = 0; i < n; i++) bb_u8(b, 0); }

static void rec_write_moov(void) {
    struct bb b = { 0 };
    unsigned timescale = 1000000;
    unsigned long long dur = 0; for (unsigned i = 0; i < rec_nsamples; i++) dur += rec_dur[i];
    size_t moov = box_begin(&b, "moov");
    /* mvhd */
    size_t mvhd = box_begin(&b, "mvhd"); bb_u32(&b, 0); bb_u32(&b, 0); bb_u32(&b, 0); bb_u32(&b, timescale); bb_u32(&b, (unsigned)dur);
    bb_u32(&b, 0x00010000); bb_u16(&b, 0x0100); bb_zero(&b, 10);
    { unsigned m[9] = { 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000 }; for (int i = 0; i < 9; i++) bb_u32(&b, m[i]); }
    bb_zero(&b, 24); bb_u32(&b, 2); box_end(&b, mvhd);
    /* trak */
    size_t trak = box_begin(&b, "trak");
    size_t tkhd = box_begin(&b, "tkhd"); bb_u32(&b, 7); bb_u32(&b, 0); bb_u32(&b, 0); bb_u32(&b, 1); bb_u32(&b, 0); bb_u32(&b, (unsigned)dur);
    bb_zero(&b, 8); bb_u16(&b, 0); bb_u16(&b, 0); bb_u16(&b, 0); bb_u16(&b, 0);
    { unsigned m[9] = { 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000 }; for (int i = 0; i < 9; i++) bb_u32(&b, m[i]); }
    bb_u32(&b, rec_w << 16); bb_u32(&b, rec_h << 16); box_end(&b, tkhd);
    size_t mdia = box_begin(&b, "mdia");
    size_t mdhd = box_begin(&b, "mdhd"); bb_u32(&b, 0); bb_u32(&b, 0); bb_u32(&b, 0); bb_u32(&b, timescale); bb_u32(&b, (unsigned)dur); bb_u16(&b, 0x55c4); bb_u16(&b, 0); box_end(&b, mdhd);
    size_t hdlr = box_begin(&b, "hdlr"); bb_u32(&b, 0); bb_u32(&b, 0); bb_put(&b, "vide", 4); bb_zero(&b, 12); bb_put(&b, "VideoHandler", 13); box_end(&b, hdlr);
    size_t minf = box_begin(&b, "minf");
    size_t vmhd = box_begin(&b, "vmhd"); bb_u32(&b, 1); bb_zero(&b, 8); box_end(&b, vmhd);
    size_t dinf = box_begin(&b, "dinf"); size_t dref = box_begin(&b, "dref"); bb_u32(&b, 0); bb_u32(&b, 1);
    size_t url = box_begin(&b, "url "); bb_u32(&b, 1); box_end(&b, url); box_end(&b, dref); box_end(&b, dinf);
    size_t stbl = box_begin(&b, "stbl");
    /* stsd / avc1 / avcC */
    size_t stsd = box_begin(&b, "stsd"); bb_u32(&b, 0); bb_u32(&b, 1);
    size_t avc1 = box_begin(&b, "avc1"); bb_zero(&b, 6); bb_u16(&b, 1); bb_zero(&b, 16);
    bb_u16(&b, rec_w); bb_u16(&b, rec_h); bb_u32(&b, 0x00480000); bb_u32(&b, 0x00480000); bb_u32(&b, 0); bb_u16(&b, 1);
    bb_zero(&b, 32); bb_u16(&b, 0x18); bb_u16(&b, 0xffff);
    size_t avcC = box_begin(&b, "avcC"); bb_u8(&b, 1); bb_u8(&b, rec_sps_len > 3 ? rec_sps[1] : 0x42); bb_u8(&b, rec_sps_len > 3 ? rec_sps[2] : 0); bb_u8(&b, rec_sps_len > 3 ? rec_sps[3] : 0x1e);
    bb_u8(&b, 0xff); bb_u8(&b, 0xe1); bb_u16(&b, rec_sps_len); bb_put(&b, rec_sps, rec_sps_len); bb_u8(&b, 1); bb_u16(&b, rec_pps_len); bb_put(&b, rec_pps, rec_pps_len);
    box_end(&b, avcC); box_end(&b, avc1); box_end(&b, stsd);
    /* stts: one entry per sample (simple, 8 bytes each) */
    size_t stts = box_begin(&b, "stts"); bb_u32(&b, 0); bb_u32(&b, rec_nsamples);
    for (unsigned i = 0; i < rec_nsamples; i++) { bb_u32(&b, 1); bb_u32(&b, rec_dur[i]); }
    box_end(&b, stts);
    size_t stss = box_begin(&b, "stss"); bb_u32(&b, 0); bb_u32(&b, rec_nkeys); for (unsigned i = 0; i < rec_nkeys; i++) bb_u32(&b, rec_keys[i]); box_end(&b, stss);
    size_t stsc = box_begin(&b, "stsc"); bb_u32(&b, 0); bb_u32(&b, 1); bb_u32(&b, 1); bb_u32(&b, 1); bb_u32(&b, 1); box_end(&b, stsc); /* one sample per chunk */
    size_t stsz = box_begin(&b, "stsz"); bb_u32(&b, 0); bb_u32(&b, 0); bb_u32(&b, rec_nsamples); for (unsigned i = 0; i < rec_nsamples; i++) bb_u32(&b, rec_size[i]); box_end(&b, stsz);
    size_t stco = box_begin(&b, "stco"); bb_u32(&b, 0); bb_u32(&b, rec_nsamples); for (unsigned i = 0; i < rec_nsamples; i++) bb_u32(&b, (unsigned)rec_off[i]); box_end(&b, stco);
    box_end(&b, stbl); box_end(&b, minf); box_end(&b, mdia); box_end(&b, trak); box_end(&b, moov);
    wr(rec_fd, b.d, b.n);
    free(b.d);
}

static void rec_stop(void) {
    if (rec_fd < 0) return;
    rec_active = 0;
    unsigned long long end = lseek(rec_fd, 0, SEEK_CUR);
    unsigned char sz[4]; be32(sz, (unsigned)(end - rec_mdat_pos));
    lseek(rec_fd, rec_mdat_pos, SEEK_SET); wr(rec_fd, sz, 4);   /* mdat size */
    lseek(rec_fd, end, SEEK_SET);
    if (rec_nsamples) rec_write_moov();
    close(rec_fd); rec_fd = -1;
    fprintf(stderr, "recording closed: %s (%u samples, %u keyframes)\n", rec_name, rec_nsamples, rec_nkeys);
    free(rec_size); free(rec_dur); free(rec_keys); free(rec_off); rec_size = rec_dur = rec_keys = NULL; rec_off = NULL;
}

/* Write one access unit as an MP4 sample (AVCC), harvesting SPS/PPS. */
static void rec_frame(const struct hframe *f) {
    if (rec_fd < 0) return;
    if (rec_waiting_key) { if (!f->key) return; rec_waiting_key = 0; }
    if (rec_nsamples >= REC_MAX_SAMPLES) { rec_stop(); return; }
    unsigned long long pos = lseek(rec_fd, 0, SEEK_CUR);
    unsigned total = 0;
    const unsigned char *d = f->data; size_t n = f->len;
    for (size_t i = 0; i + 4 < n;) {
        if (!(d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (d[i + 2] == 0 && d[i + 3] == 1)))) { i++; continue; }
        size_t s = d[i + 2] == 1 ? i + 3 : i + 4, e = s;
        while (e + 3 <= n && !(d[e] == 0 && d[e + 1] == 0 && (d[e + 2] == 1 || (e + 3 < n && d[e + 2] == 0 && d[e + 3] == 1)))) e++;
        if (e + 3 > n) e = n;
        while (e > s && d[e - 1] == 0 && e + 3 <= n) e--;          /* trailing zero of next start code */
        size_t nl = e - s; int t = d[s] & 31;
        if (t == 7) { if (nl <= sizeof rec_sps) { memcpy(rec_sps, d + s, nl); rec_sps_len = nl; } }
        else if (t == 8) { if (nl <= sizeof rec_pps) { memcpy(rec_pps, d + s, nl); rec_pps_len = nl; } }
        else if (t != 9 && nl > 0) { unsigned char l[4]; be32(l, (unsigned)nl); wr(rec_fd, l, 4); wr(rec_fd, d + s, nl); total += 4 + nl; }
        i = e;
    }
    if (!total) return;
    rec_size[rec_nsamples] = total; rec_off[rec_nsamples] = pos;
    if (rec_nsamples) rec_dur[rec_nsamples - 1] = (unsigned)(f->ts > rec_last_ts ? f->ts - rec_last_ts : 33333);
    rec_dur[rec_nsamples] = 66667;                                  /* provisional; fixed by the next frame */
    if (f->key) rec_keys[rec_nkeys++] = rec_nsamples + 1;
    rec_nsamples++; rec_last_ts = f->ts; rec_last_write = time(NULL);
}

static int rec_start(const char *tag, int with_pre, char *out, int n) {
    if (!save_dir) return -1;
    rec_stop();
    if (ensure_space() < 0) return -2;
    /* pre-roll: start at the newest keyframe that is at least PRE_SECONDS old (or the oldest key) */
    long long now = now_us(); unsigned first = ring_count;
    if (with_pre) {
        for (unsigned i = 0; i < ring_count; i++) { struct hframe *f = ring_at(i); if (f->key && now - f->ts <= (PRE_SECONDS + 1) * 1000000LL) { first = i; break; } }
        if (first == ring_count) for (unsigned i = 0; i < ring_count; i++) if (ring_at(i)->key) { first = i; break; }
    }
    time_t t0 = time(NULL);
    if (first < ring_count) t0 -= (time_t)((now - ring_at(first)->ts) / 1000000);
    rec_fd = open_recfile(tag, "mp4", t0, rec_name, sizeof rec_name);
    if (rec_fd < 0) return -3;
    rec_size = calloc(REC_MAX_SAMPLES, 4); rec_dur = calloc(REC_MAX_SAMPLES, 4); rec_keys = calloc(REC_MAX_SAMPLES, 4); rec_off = calloc(REC_MAX_SAMPLES, 8);
    if (!rec_size || !rec_dur || !rec_keys || !rec_off) { close(rec_fd); rec_fd = -1; return -3; }
    rec_nsamples = rec_nkeys = 0; rec_sps_len = rec_pps_len = 0; rec_last_ts = 0;
    static const unsigned char ftyp[] = { 0,0,0,0x20, 'f','t','y','p', 'i','s','o','m', 0,0,2,0, 'i','s','o','m', 'i','s','o','2', 'a','v','c','1', 'm','p','4','1' };
    wr(rec_fd, ftyp, sizeof ftyp);
    rec_mdat_pos = sizeof ftyp;
    static const unsigned char mdat[] = { 0,0,0,0, 'm','d','a','t' };
    wr(rec_fd, mdat, 8);
    rec_waiting_key = 1; rec_active = 1; rec_last_write = time(NULL);
    for (unsigned i = first; i < ring_count; i++) rec_frame(ring_at(i));
    snprintf(out, n, "%s", rec_name);
    return 0;
}

/* ---- fragmented MP4 for live view over MSE -----------------------------------------
 * Browsers only allow WebCodecs on HTTPS pages, but Media Source Extensions work on plain
 * HTTP. /?action=fmp4 sends an init segment (ftyp + moov with mvex/trex, avcC from the
 * stream's SPS/PPS) followed by one moof+mdat per frame. A frame is sent once the next one
 * has arrived so its real duration is known (one frame of extra latency). */
static int sps_pps_from_au(const unsigned char *d, size_t n, unsigned char *sps, int *spsl, unsigned char *pps, int *ppsl) {
    *spsl = *ppsl = 0;
    for (size_t i = 0; i + 4 < n;) {
        if (!(d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (d[i + 2] == 0 && d[i + 3] == 1)))) { i++; continue; }
        size_t s = d[i + 2] == 1 ? i + 3 : i + 4, e = s;
        while (e + 3 <= n && !(d[e] == 0 && d[e + 1] == 0 && (d[e + 2] == 1 || (e + 3 < n && d[e + 2] == 0 && d[e + 3] == 1)))) e++;
        if (e + 3 > n) e = n;
        while (e > s && d[e - 1] == 0 && e + 3 <= n) e--;
        int t = d[s] & 31; size_t nl = e - s;
        if (t == 7 && nl <= 128) { memcpy(sps, d + s, nl); *spsl = nl; }
        else if (t == 8 && nl <= 64) { memcpy(pps, d + s, nl); *ppsl = nl; }
        i = e;
    }
    return *spsl && *ppsl;
}

/* Convert an Annex-B access unit to AVCC (length-prefixed), dropping SPS/PPS/AUD. */
static size_t au_to_avcc(const unsigned char *d, size_t n, unsigned char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i + 4 < n;) {
        if (!(d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (d[i + 2] == 0 && d[i + 3] == 1)))) { i++; continue; }
        size_t s = d[i + 2] == 1 ? i + 3 : i + 4, e = s;
        while (e + 3 <= n && !(d[e] == 0 && d[e + 1] == 0 && (d[e + 2] == 1 || (e + 3 < n && d[e + 2] == 0 && d[e + 3] == 1)))) e++;
        if (e + 3 > n) e = n;
        while (e > s && d[e - 1] == 0 && e + 3 <= n) e--;
        int t = d[s] & 31; size_t nl = e - s;
        if (t != 7 && t != 8 && t != 9 && nl && o + 4 + nl <= cap) { be32(out + o, (unsigned)nl); memcpy(out + o + 4, d + s, nl); o += 4 + nl; }
        i = e;
    }
    return o;
}

static void fmp4_init_segment(struct bb *b, const unsigned char *sps, int spsl, const unsigned char *pps, int ppsl) {
    static const unsigned char ftyp[] = { 0,0,0,0x1c, 'f','t','y','p', 'i','s','o','m', 0,0,2,0, 'i','s','o','m', 'i','s','o','5', 'a','v','c','1' };
    bb_put(b, ftyp, sizeof ftyp);
    unsigned timescale = 1000000;
    size_t moov = box_begin(b, "moov");
    size_t mvhd = box_begin(b, "mvhd"); bb_u32(b, 0); bb_u32(b, 0); bb_u32(b, 0); bb_u32(b, timescale); bb_u32(b, 0);
    bb_u32(b, 0x00010000); bb_u16(b, 0x0100); bb_zero(b, 10);
    { unsigned m[9] = { 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000 }; for (int i = 0; i < 9; i++) bb_u32(b, m[i]); }
    bb_zero(b, 24); bb_u32(b, 2); box_end(b, mvhd);
    size_t trak = box_begin(b, "trak");
    size_t tkhd = box_begin(b, "tkhd"); bb_u32(b, 7); bb_u32(b, 0); bb_u32(b, 0); bb_u32(b, 1); bb_u32(b, 0); bb_u32(b, 0);
    bb_zero(b, 8); bb_u16(b, 0); bb_u16(b, 0); bb_u16(b, 0); bb_u16(b, 0);
    { unsigned m[9] = { 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000 }; for (int i = 0; i < 9; i++) bb_u32(b, m[i]); }
    bb_u32(b, 1280 << 16); bb_u32(b, 720 << 16); box_end(b, tkhd);
    size_t mdia = box_begin(b, "mdia");
    size_t mdhd = box_begin(b, "mdhd"); bb_u32(b, 0); bb_u32(b, 0); bb_u32(b, 0); bb_u32(b, timescale); bb_u32(b, 0); bb_u16(b, 0x55c4); bb_u16(b, 0); box_end(b, mdhd);
    size_t hdlr = box_begin(b, "hdlr"); bb_u32(b, 0); bb_u32(b, 0); bb_put(b, "vide", 4); bb_zero(b, 12); bb_put(b, "VideoHandler", 13); box_end(b, hdlr);
    size_t minf = box_begin(b, "minf");
    size_t vmhd = box_begin(b, "vmhd"); bb_u32(b, 1); bb_zero(b, 8); box_end(b, vmhd);
    size_t dinf = box_begin(b, "dinf"); size_t dref = box_begin(b, "dref"); bb_u32(b, 0); bb_u32(b, 1);
    size_t url = box_begin(b, "url "); bb_u32(b, 1); box_end(b, url); box_end(b, dref); box_end(b, dinf);
    size_t stbl = box_begin(b, "stbl");
    size_t stsd = box_begin(b, "stsd"); bb_u32(b, 0); bb_u32(b, 1);
    size_t avc1 = box_begin(b, "avc1"); bb_zero(b, 6); bb_u16(b, 1); bb_zero(b, 16);
    bb_u16(b, 1280); bb_u16(b, 720); bb_u32(b, 0x00480000); bb_u32(b, 0x00480000); bb_u32(b, 0); bb_u16(b, 1);
    bb_zero(b, 32); bb_u16(b, 0x18); bb_u16(b, 0xffff);
    size_t avcC = box_begin(b, "avcC"); bb_u8(b, 1); bb_u8(b, sps[1]); bb_u8(b, sps[2]); bb_u8(b, sps[3]);
    bb_u8(b, 0xff); bb_u8(b, 0xe1); bb_u16(b, spsl); bb_put(b, sps, spsl); bb_u8(b, 1); bb_u16(b, ppsl); bb_put(b, pps, ppsl);
    box_end(b, avcC); box_end(b, avc1); box_end(b, stsd);
    size_t stts = box_begin(b, "stts"); bb_u32(b, 0); bb_u32(b, 0); box_end(b, stts);
    size_t stsc = box_begin(b, "stsc"); bb_u32(b, 0); bb_u32(b, 0); box_end(b, stsc);
    size_t stsz = box_begin(b, "stsz"); bb_u32(b, 0); bb_u32(b, 0); bb_u32(b, 0); box_end(b, stsz);
    size_t stco = box_begin(b, "stco"); bb_u32(b, 0); bb_u32(b, 0); box_end(b, stco);
    box_end(b, stbl); box_end(b, minf); box_end(b, mdia); box_end(b, trak);
    size_t mvex = box_begin(b, "mvex");
    size_t trex = box_begin(b, "trex"); bb_u32(b, 0); bb_u32(b, 1); bb_u32(b, 1); bb_u32(b, 0); bb_u32(b, 0); bb_u32(b, 0x01010000); box_end(b, trex);
    box_end(b, mvex); box_end(b, moov);
}

/* moof + mdat for one sample. dts in timescale units (1e6). */
static void fmp4_fragment(struct bb *b, unsigned seqno, unsigned long long dts, unsigned duration, int key, const unsigned char *sample, size_t slen) {
    size_t moof = box_begin(b, "moof");
    size_t mfhd = box_begin(b, "mfhd"); bb_u32(b, 0); bb_u32(b, seqno); box_end(b, mfhd);
    size_t traf = box_begin(b, "traf");
    size_t tfhd = box_begin(b, "tfhd"); bb_u32(b, 0x020000); bb_u32(b, 1); box_end(b, tfhd);              /* default-base-is-moof */
    size_t tfdt = box_begin(b, "tfdt"); bb_u32(b, 0x01000000); bb_u32(b, (unsigned)(dts >> 32)); bb_u32(b, (unsigned)dts); box_end(b, tfdt);
    size_t trun = box_begin(b, "trun"); bb_u32(b, 0x000701); bb_u32(b, 1); size_t dataoff = b->n; bb_u32(b, 0);
    bb_u32(b, duration); bb_u32(b, (unsigned)slen); bb_u32(b, key ? 0x02000000 : 0x01010000); box_end(b, trun);
    box_end(b, traf); box_end(b, moof);
    be32(b->d + dataoff, (unsigned)(b->n - moof + 8));                                                   /* data offset: start of mdat payload */
    size_t mdat = box_begin(b, "mdat"); bb_put(b, sample, slen); box_end(b, mdat);
}

/* ---- HTTP ----------------------------------------------------------------------- */
enum phase { P_REQ, P_SEND, P_LIVE_WAIT, P_LIVE_SEND, P_CLOSING };
struct client {
    int fd, local;
    struct tlsconn *tls;                     /* non-NULL for HTTPS clients */
    enum phase phase;
    char req[2048]; int reqlen;
    unsigned char *body; size_t body_len, body_want;   /* POST body (browser-made JPEG for /?action=save) */
    unsigned char *buf; size_t len, off;     /* current response / live chunk being sent */
    int live, live_raw, live_fmp4;           /* live stream client; raw = plain Annex-B; fmp4 = MSE */
    unsigned live_seq;                       /* last frame seq sent */
    unsigned fmp4_seqno; unsigned long long fmp4_dts; long long fmp4_t0;   /* fragment state */
    unsigned char live_hdr[12]; int live_hdr_off;
    const struct hframe *lf;                 /* live frame being sent (ring is only appended, so the pointer stays valid long enough; we also check seq) */
};
static struct client cli[MAXCLI];
static int lfd;
static char auth[128] = "";
static const char *www = "/mnt/sif/owrt/www";
static int ctl_port = 8081;

static void cli_close(struct client *c) {
    if (c->fd >= 0) close(c->fd);
    if (c->tls) tls_free(c->tls);
    free(c->buf); free(c->body);
    memset(c, 0, sizeof *c); c->fd = -1;
}

/* Socket I/O through TLS when the client is HTTPS. Returns bytes, 0 = would block /
 * nothing yet, -1 = closed or error. */
static int cli_recv(struct client *c, void *buf, size_t n) {
    if (c->tls) return tls_closed(c->tls) ? -1 : tls_recv(c->tls, buf, n);
    ssize_t r = read(c->fd, buf, n);
    if (r > 0) return (int)r;
    if (r == 0) return -1;
    return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
}
static int cli_send(struct client *c, const void *p, size_t n) {
    if (c->tls) return tls_closed(c->tls) ? -1 : tls_send(c->tls, p, n);
    ssize_t w = write(c->fd, p, n);
    if (w >= 0) return (int)w;
    return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
}

/* Save a JPEG (from the browser's decoded frame, or from jpeg_grab) under manual/. */
static int save_jpeg(const unsigned char *j, size_t jl, char *rel, int n) {
    if (!save_dir || jl < 512 || j[0] != 0xFF || j[1] != 0xD8 || ensure_space() < 0) return -1;
    int fd = open_recfile("manual", "jpg", time(NULL), rel, n);
    if (fd < 0) return -1;
    wr(fd, j, jl); close(fd);
    return 0;
}
static void set_resp(struct client *c, const char *hdr, const unsigned char *body, size_t len) {
    size_t hl = strlen(hdr);
    free(c->buf); c->buf = malloc(hl + len);
    memcpy(c->buf, hdr, hl); if (len) memcpy(c->buf + hl, body, len);
    c->len = hl + len; c->off = 0; c->phase = P_SEND;
}
static void send_simple(struct client *c, int code, const char *ctype, const char *body, const char *extra) {
    char h[400]; snprintf(h, sizeof h, "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nConnection: close\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n%sContent-Length: %d\r\n\r\n",
                          code, code == 200 ? "OK" : code == 401 ? "Unauthorized" : code == 404 ? "Not Found" : "Error", ctype, extra ? extra : "", (int)strlen(body));
    set_resp(c, h, (const unsigned char *)body, strlen(body));
}
static void send_json(struct client *c, int code, const char *body) { send_simple(c, code, "application/json", body, NULL); }
static const char *ctype_of(const char *p) {
    const char *e = strrchr(p, '.');
    if (!e) return "application/octet-stream";
    if (!strcmp(e, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(e, ".jpg") || !strcmp(e, ".jpeg")) return "image/jpeg";
    if (!strcmp(e, ".mp4")) return "video/mp4";
    if (!strcmp(e, ".avi")) return "video/x-msvideo";
    if (!strcmp(e, ".h264")) return "video/h264";
    if (!strcmp(e, ".png")) return "image/png";
    if (!strcmp(e, ".css")) return "text/css";
    if (!strcmp(e, ".js")) return "application/javascript";
    if (!strcmp(e, ".json")) return "application/json";
    if (!strcmp(e, ".ico")) return "image/x-icon";
    return "application/octet-stream";
}

/* Stream a file from a forked child (recordings are MBs; the main loop must not stall). */
static void send_file_forked(struct client *c, const char *full) {
    int fd = open(full, O_RDONLY);
    off_t len = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
    if (fd < 0 || len <= 0) { if (fd >= 0) close(fd); send_simple(c, 404, "text/plain", "Not Found", NULL); return; }
    lseek(fd, 0, SEEK_SET);
    if (fork() == 0) {
        alarm(900); fcntl(c->fd, F_SETFL, 0);
        char h[256]; int hl = snprintf(h, sizeof h, "HTTP/1.0 200 OK\r\nConnection: close\r\nContent-Type: %s\r\nContent-Length: %ld\r\nCache-Control: max-age=86400\r\n\r\n", ctype_of(full), (long)len);
        struct tlsio *tio = c->tls ? tls_blocking(c->tls) : NULL;
        if (tio) tls_blocking_write(tio, h, hl); else wr(c->fd, h, hl);
        static unsigned char buf[16384]; ssize_t r;
        while ((r = read(fd, buf, sizeof buf)) > 0) {
            if (tio) { if (tls_blocking_write(tio, buf, r) < 0) break; }
            else { size_t o = 0; ssize_t w; while (o < (size_t)r && (w = write(c->fd, buf + o, r - o)) > 0) o += w; if (o < (size_t)r) break; }
        }
        if (tio) tls_blocking_finish(tio);
        _exit(0);
    }
    close(fd); cli_close(c);
}

static void proxy_api(struct client *c, const char *raw, int rawlen, const char *method, const char *path) {
    if (fork() != 0) { cli_close(c); return; }
    alarm(60);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(ctl_port), .sin_addr.s_addr = htonl(0x7f000001) };
    fcntl(c->fd, F_SETFL, 0);
    struct tlsio *tio = c->tls ? tls_blocking(c->tls) : NULL;
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        const char *m = "HTTP/1.0 502 Bad Gateway\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"control API offline\"}";
        if (tio) { tls_blocking_write(tio, m, strlen(m)); tls_blocking_finish(tio); } else write(c->fd, m, strlen(m));
        _exit(0);
    }
    char line[1200]; int n = snprintf(line, sizeof line, "%s %s HTTP/1.0\r\n", method, path); write(s, line, n);
    const char *hdrs = strstr(raw, "\r\n");
    if (hdrs) {
        write(s, hdrs + 2, rawlen - (hdrs + 2 - raw));
        const char *cl = strstr(raw, "Content-Length:"), *be = strstr(raw, "\r\n\r\n");
        if (cl && be) {
            long want = atol(cl + 15), have = rawlen - (be + 4 - raw); char buf[1024];
            while (have < want) { int r = tio ? tls_blocking_read(tio, buf, sizeof buf) : (int)read(c->fd, buf, sizeof buf); if (r <= 0) break; write(s, buf, r); have += r; }
        }
    }
    char buf[2048]; int r;
    while ((r = read(s, buf, sizeof buf)) > 0) { if (tio) { if (tls_blocking_write(tio, buf, r) < 0) break; } else if (write(c->fd, buf, r) < 0) break; }
    if (tio) tls_blocking_finish(tio);
    _exit(0);
}

/* Find the frame following f in the ring (NULL if f is the newest). */
static const struct hframe *ring_after(const struct hframe *f) {
    for (unsigned i = 0; i + 1 < ring_count; i++) if (ring_at(i) == f) return ring_at(i + 1);
    return NULL;
}

/* Queue frame f for a live client: fMP4 fragment (page) or the bare Annex-B access unit (raw). */
static void live_prepare(struct client *c, const struct hframe *f) {
    if (c->live_fmp4) {
        /* Build moof+mdat for f into c->buf and send it like a normal response body. The
         * caller guarantees a following frame exists, so the duration is real. */
        const struct hframe *nx = ring_after(f);
        unsigned dur = nx && nx->ts > f->ts ? (unsigned)(nx->ts - f->ts) : 66667;
        static unsigned char avcc[1843200];
        size_t sl = au_to_avcc(f->data, f->len, avcc, sizeof avcc);
        if (!c->fmp4_t0) c->fmp4_t0 = f->ts;
        struct bb b = { 0 };
        fmp4_fragment(&b, ++c->fmp4_seqno, (unsigned long long)(f->ts - c->fmp4_t0), dur, f->key, avcc, sl);
        free(c->buf); c->buf = b.d; c->len = b.n; c->off = 0;
        c->lf = NULL; c->live_seq = f->seq; c->phase = P_SEND;
        return;
    }
    c->lf = f; c->live_seq = f->seq;
    c->live_hdr_off = 12;                        /* no per-frame header (the framed variant was removed) */
    c->off = 0; c->phase = P_LIVE_SEND;
}
/* Pick the next frame for a live client: the frame after live_seq, or (if it fell behind
 * or is new) the newest keyframe. */
static const struct hframe *live_next(struct client *c) {
    if (!ring_count) return NULL;
    /* fMP4 clients stay one frame behind so each fragment carries its real duration */
    unsigned avail = c->live_fmp4 ? ring_count - 1 : ring_count;
    if (!avail) return NULL;
    if (c->live_seq == 0) {                         /* new viewer: start at the newest key */
        for (int i = (int)avail - 1; i >= 0; i--) if (ring_at(i)->key) return ring_at(i);
        return NULL;
    }
    const struct hframe *newest = ring_at(avail - 1);
    if (newest->seq <= c->live_seq) return NULL;
    if (newest->seq - c->live_seq > 15) {           /* too far behind: resync on a keyframe */
        for (int i = (int)avail - 1; i >= 0; i--) if (ring_at(i)->key && ring_at(i)->seq > c->live_seq) return ring_at(i);
        return NULL;
    }
    for (unsigned i = 0; i < avail; i++) if (ring_at(i)->seq == c->live_seq + 1) return ring_at(i);
    return newest;
}

static int cli_write(struct client *c) {
    for (;;) {
        const unsigned char *p; size_t left;
        if (c->phase == P_SEND) { p = c->buf + c->off; left = c->len - c->off; }
        else if (c->phase == P_LIVE_SEND) {
            if (c->live_hdr_off < 12) { p = c->live_hdr + c->live_hdr_off; left = 12 - c->live_hdr_off; }
            else {
                /* The ring may have recycled this slot (slow client, old keyframe): resync. */
                if (!c->lf || !c->lf->data || c->lf->seq != c->live_seq) { c->phase = P_LIVE_WAIT; const struct hframe *f = live_next(c); if (f) { live_prepare(c, f); continue; } return 0; }
                p = c->lf->data + c->off; left = c->lf->len - c->off;
            }
        } else return 0;
        if (left == 0) {
            if (c->phase == P_SEND) {
                if (!c->live) return -2;            /* response complete */
                c->phase = P_LIVE_WAIT; const struct hframe *f = live_next(c);
                if (f) { live_prepare(c, f); continue; }
                return 0;
            }
            if (c->live_hdr_off < 12) { c->live_hdr_off = 12; continue; }
            c->phase = P_LIVE_WAIT; const struct hframe *f = live_next(c); if (f) live_prepare(c, f); else return 0; continue;
        }
        int n = cli_send(c, p, left);
        if (n < 0) return -1;
        if (n == 0) return 0;                       /* would block (socket or TLS buffer full) */
        if (c->phase == P_SEND) c->off += n;
        else if (c->live_hdr_off < 12) c->live_hdr_off += n;
        else c->off += n;
    }
}

static int live_viewers(void) { int n = 0; for (int i = 0; i < MAXCLI; i++) if (cli[i].fd >= 0 && cli[i].live) n++; return n; }

/* Response finished: plain sockets close now; TLS sends close_notify and lingers in
 * P_CLOSING until the engine has flushed (otherwise the tail of the response is lost). */
static void cli_finish(struct client *c) {
    if (!c->tls) { cli_close(c); return; }
    tls_close(c->tls); c->phase = P_CLOSING;
    if (tls_pump(c->tls, 0, 1) < 0 || tls_flushed(c->tls)) cli_close(c);
}
/* Common tail after cli_write(): -1 error, -2 done, otherwise push TLS output. */
static void cli_after_write(struct client *c, int r) {
    if (c->fd < 0) return;
    if (r == -1) cli_close(c);
    else if (r == -2) cli_finish(c);
    else if (c->tls && tls_pump(c->tls, 0, 1) < 0) cli_close(c);
}

static void handle_request(struct client *c) {
    char raw[sizeof c->req]; memcpy(raw, c->req, c->reqlen); int rawlen = c->reqlen;
    char *line_end = strstr(c->req, "\r\n"); if (line_end) *line_end = 0;
    int is_get = !strncmp(c->req, "GET ", 4), is_post = !strncmp(c->req, "POST ", 5);
    char *path = strchr(c->req, ' ');
    if (!path || (!is_get && !is_post)) { send_simple(c, 400, "text/plain", "Bad Request", NULL); return; }
    path++; char *sp = strchr(path, ' '); if (sp) *sp = 0;
    if (auth[0] && !c->local) {
        const char *a = line_end ? strstr(line_end + 1, "Authorization: ") : NULL;
        if (!a || strncmp(a + 15, auth, strlen(auth))) { send_simple(c, 401, "text/plain", "Unauthorized", "WWW-Authenticate: Basic realm=\"camera\"\r\n"); return; }
    }
    if (!strncmp(path, "/api/", 5)) { proxy_api(c, raw, rawlen, is_post ? "POST" : "GET", path + 4); return; }
    char *q = strchr(path, '?'); if (q) *q++ = 0; else q = "";
    char body[600];
    if (is_post) {
        /* POST /?action=save with a JPEG body: the page renders the decoded H.264 frame to a
         * canvas and uploads it, so no second V4L2 stream is needed for snapshots. */
        if (strstr(q, "action=save") && c->body && c->body_len == c->body_want) {
            char rel[256]; int r = save_jpeg(c->body, c->body_len, rel, sizeof rel);
            snprintf(body, sizeof body, r == 0 ? "{\"saved\":\"%s\"}" : "{\"error\":\"save failed (no SD card or bad image)\"}", rel);
            send_json(c, r == 0 ? 200 : 500, body);
        } else send_simple(c, 405, "text/plain", "Method Not Allowed", NULL);
        return;
    }

    if (strstr(q, "action=fmp4")) {
        /* init segment needs SPS/PPS: take them from the newest keyframe in the ring */
        unsigned char sps[128], pps[64]; int spsl = 0, ppsl = 0; const struct hframe *kf = NULL;
        for (int i = (int)ring_count - 1; i >= 0; i--) if (ring_at(i)->key) { kf = ring_at(i); break; }
        if (!kf || !sps_pps_from_au(kf->data, kf->len, sps, &spsl, pps, &ppsl)) { send_json(c, 503, "{\"error\":\"no keyframe yet\"}"); return; }
        struct bb b = { 0 }; fmp4_init_segment(&b, sps, spsl, pps, ppsl);
        char h[300]; snprintf(h, sizeof h, "HTTP/1.0 200 OK\r\nConnection: close\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n"
                 "Content-Type: video/mp4\r\nX-Codec: avc1.%02x%02x%02x\r\n\r\n", sps[1], sps[2], sps[3]);
        set_resp(c, h, b.d, b.n); free(b.d);
        c->live = 1; c->live_fmp4 = 1; c->live_seq = 0; c->fmp4_seqno = 0; c->fmp4_t0 = 0;
        return;
    }
    if (strstr(q, "action=raw")) {               /* plain Annex-B H.264 for VLC / ffmpeg */
        c->live = 1; c->live_raw = 1; c->live_seq = 0;
        set_resp(c, "HTTP/1.0 200 OK\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Type: video/h264\r\n\r\n", NULL, 0);
        return;
    }
    if (strstr(q, "action=snapshot") || strstr(q, "action=save")) {
        size_t jl; unsigned char *j = jpeg_grab(&jl);
        if (!j) { send_json(c, 503, "{\"error\":\"snapshot failed\"}"); return; }
        if (strstr(q, "action=save")) {
            char rel[256]; int r = -1;
            if (save_dir && ensure_space() == 0) { int fd = open_recfile("manual", "jpg", time(NULL), rel, sizeof rel); if (fd >= 0) { wr(fd, j, jl); close(fd); r = 0; } }
            free(j);
            snprintf(body, sizeof body, r == 0 ? "{\"saved\":\"%s\"}" : "{\"error\":\"no SD card mounted%s\"}", r == 0 ? rel : "");
            send_json(c, r == 0 ? 200 : 500, body);
            return;
        }
        char h[200]; snprintf(h, sizeof h, "HTTP/1.0 200 OK\r\nConnection: close\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", jl);
        set_resp(c, h, j, jl); free(j);
        return;
    }
    if (strstr(q, "action=rec")) {
        int r = 0;
        if (strstr(q, "start=1")) {
            char rel[256]; const char *tag = strstr(q, "tag=manual") ? "manual" : "motion";
            r = rec_start(tag, strstr(q, "pre=1") != NULL, rel, sizeof rel);
            snprintf(body, sizeof body, r == 0 ? "{\"recording\":\"%s\"}" : "{\"error\":\"cannot start recording (%s)\"}", r == 0 ? rel : r == -2 ? "no SD card" : "write failed");
        } else if (strstr(q, "stop=1")) {
            unsigned ns = rec_nsamples; char nm[256]; snprintf(nm, sizeof nm, "%s", rec_fd >= 0 ? rec_name : "");
            rec_stop(); snprintf(body, sizeof body, "{\"closed\":\"%s\",\"samples\":%u}", nm, ns);
        } else if (strstr(q, "frame=1")) {           /* compatibility with older hkctl: frames are appended automatically */
            snprintf(body, sizeof body, "{\"samples\":%u}", rec_nsamples);
        } else { r = -1; snprintf(body, sizeof body, "{\"error\":\"start=1|stop=1\"}"); }
        send_json(c, r == 0 ? 200 : 500, body);
        return;
    }
    if (strstr(q, "action=status")) {
        struct hframe *nf = ring_newest();
        snprintf(body, sizeof body, "{\"fps\":%.1f,\"mbps\":%.2f,\"viewers\":%d,\"recording\":%s,\"rec_samples\":%u,\"ring_frames\":%u,\"ring_kb\":%zu,\"frame_age_ms\":%lld}",
                 stat_fps, stat_mbps, live_viewers(), rec_fd >= 0 ? "true" : "false", rec_nsamples, ring_count, ring_bytes / 1024, nf ? (now_us() - nf->ts) / 1000 : -1LL);
        send_json(c, 200, body);
        return;
    }
    if (strstr(q, "action=delete")) {
        char rel[300] = ""; const char *d = strstr(q, "path=");
        if (d) { int i = 0; d += 5; while (*d && *d != '&' && i < 299) rel[i++] = *d++; rel[i] = 0; }
        url_decode(rel);
        int depth = 0; for (const char *s = rel; *s; s++) if (*s == '/') depth++;
        int ok = save_dir && is_safe_rel(rel) && depth >= 1 && depth <= 2 && (!strncmp(rel, "manual/", 7) || !strncmp(rel, "motion/", 7));
        int r = -1;
        if (ok) { char full[700]; struct stat st; snprintf(full, sizeof full, "%s/%s", save_dir, rel); if (stat(full, &st) == 0) r = S_ISDIR(st.st_mode) ? rm_tree(full) : unlink(full); }
        snprintf(body, sizeof body, r == 0 ? "{\"deleted\":\"%s\"}" : "{\"error\":\"cannot delete %s\"}", rel);
        send_json(c, r == 0 ? 200 : 400, body);
        return;
    }
    if (!strcmp(path, "/files")) {
        char rel[256] = ""; static char big[8192];
        const char *d = strstr(q, "dir="); if (d) { int i = 0; d += 4; while (*d && *d != '&' && i < 255) rel[i++] = *d++; rel[i] = 0; }
        url_decode(rel);
        if (!save_dir || !is_safe_rel(rel)) { send_json(c, 400, "{\"error\":\"bad dir\"}"); return; }
        list_dir(rel, big, sizeof big); send_json(c, 200, big);
        return;
    }
    if (!strncmp(path, "/rec/", 5)) {
        url_decode(path + 5);
        if (!save_dir || !is_safe_rel(path + 5)) { send_simple(c, 403, "text/plain", "Forbidden", NULL); return; }
        char full[700]; snprintf(full, sizeof full, "%s/%s", save_dir, path + 5);
        send_file_forked(c, full);
        return;
    }
    if (strstr(q, "action=")) { send_json(c, 404, "{\"error\":\"unknown action\"}"); return; }
    /* static file */
    if (strstr(path, "..")) { send_simple(c, 403, "text/plain", "Forbidden", NULL); return; }
    if (!strcmp(path, "/")) path = "/index.html";
    char full[300]; snprintf(full, sizeof full, "%s%s", www, path);
    int fd = open(full, O_RDONLY);
    off_t len = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
    if (fd < 0 || len <= 0 || len > 1024 * 1024) { if (fd >= 0) close(fd); send_simple(c, 404, "text/plain", "Not Found", NULL); return; }
    lseek(fd, 0, SEEK_SET);
    unsigned char *b = malloc(len);
    if (!b || read(fd, b, len) != len) { close(fd); free(b); send_simple(c, 500, "text/plain", "Read error", NULL); return; }
    close(fd);
    char h[256]; snprintf(h, sizeof h, "HTTP/1.0 200 OK\r\nConnection: close\r\nContent-Type: %s\r\nContent-Length: %ld\r\nCache-Control: no-cache\r\n\r\n", ctype_of(path), (long)len);
    set_resp(c, h, b, len); free(b);
}

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
    const char *dev = "/dev/video1"; int port = 8080, plfreq = 60, tls_port = 0;
    const char *tls_cert = NULL, *tls_key = NULL;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "-d")) dev = h264_dev = argv[i + 1];
        else if (!strcmp(argv[i], "-t")) tls_port = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-C")) tls_cert = argv[i + 1];
        else if (!strcmp(argv[i], "-K")) tls_key = argv[i + 1];
        else if (!strcmp(argv[i], "-j")) jpeg_dev = argv[i + 1];
        else if (!strcmp(argv[i], "-p")) port = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-w")) www = argv[i + 1];
        else if (!strcmp(argv[i], "-s")) save_dir = argv[i + 1];
        else if (!strcmp(argv[i], "-a")) ctl_port = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-l")) plfreq = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-c") && argv[i + 1][0]) { char e[128]; b64(argv[i + 1], e); snprintf(auth, sizeof auth, "Basic %s", e); }
    }
    signal(SIGPIPE, SIG_IGN); signal(SIGCHLD, SIG_IGN);
    for (int i = 0; i < MAXCLI; i++) cli[i].fd = -1;
    vfd = cam_open(dev, v4l2_fourcc('H', '2', '6', '4'), 1280, 720, NBUF);
    if (vfd < 0) die("H.264 capture");
    last_frame_time = time(NULL);
    cam_ctrl(vfd, V4L2_CID_POWER_LINE_FREQUENCY, plfreq == 60 ? 2 : plfreq == 50 ? 1 : 0, "power_line_frequency");

    lfd = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY };
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(lfd, 8) < 0) die("listen");
    fcntl(lfd, F_SETFL, O_NONBLOCK);
    /* optional HTTPS listener (BearSSL) */
    int tfd = -1;
    if (tls_port > 0 && tls_cert && tls_key && tls_init(tls_cert, tls_key) == 0) {
        tfd = socket(AF_INET, SOCK_STREAM, 0); setsockopt(tfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in ta = { .sin_family = AF_INET, .sin_port = htons(tls_port), .sin_addr.s_addr = INADDR_ANY };
        if (bind(tfd, (struct sockaddr *)&ta, sizeof ta) < 0 || listen(tfd, 8) < 0) { perror("tls listen"); close(tfd); tfd = -1; }
        else fcntl(tfd, F_SETFL, O_NONBLOCK);
    }
    fprintf(stderr, "camd: %s H.264 1280x720 -> http port %d%s, www %s, recordings %s%s\n", dev, port, tfd >= 0 ? " + https" : "", www, save_dir ? save_dir : "(disabled)", auth[0] ? ", auth on" : "");
    if (tfd >= 0) fprintf(stderr, "camd: https on port %d\n", tls_port);

    struct pollfd pf[MAXCLI + 3];
    for (;;) {
        int np = 0;
        pf[np].fd = vfd; pf[np].events = POLLIN; np++;
        pf[np].fd = lfd; pf[np].events = POLLIN; np++;
        int tidx = -1; if (tfd >= 0) { tidx = np; pf[np].fd = tfd; pf[np].events = POLLIN; np++; }
        int firstcli = np;
        for (int i = 0; i < MAXCLI; i++) if (cli[i].fd >= 0) {
            short ev = cli[i].phase == P_REQ ? POLLIN : (cli[i].phase == P_LIVE_WAIT || cli[i].phase == P_CLOSING) ? 0 : POLLOUT;
            if (cli[i].tls) ev |= tls_want_events(cli[i].tls);
            pf[np].fd = cli[i].fd; pf[np].events = ev; np++;
        }
        if (poll(pf, np, 1000) < 0) { if (errno == EINTR) continue; die("poll"); }
        int k = 0;
        if (pf[k++].revents & POLLIN) {
            cam_grab();
            for (int i = 0; i < MAXCLI; i++) if (cli[i].fd >= 0 && cli[i].phase == P_LIVE_WAIT) {
                const struct hframe *f = live_next(&cli[i]);
                if (f) { live_prepare(&cli[i], f); cli_after_write(&cli[i], cli_write(&cli[i])); }
            }
        }
        for (int li = 0; li < 2; li++) {
            int lidx = li == 0 ? k : tidx; if (lidx < 0) continue;
            if (!(pf[lidx].revents & POLLIN)) continue;
            struct sockaddr_in peer; socklen_t pl = sizeof peer;
            int fd = accept(pf[lidx].fd, (struct sockaddr *)&peer, &pl);
            if (fd < 0) continue;
            int j; for (j = 0; j < MAXCLI && cli[j].fd >= 0; j++);
            if (j == MAXCLI) { close(fd); continue; }
            fcntl(fd, F_SETFL, O_NONBLOCK); setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            memset(&cli[j], 0, sizeof cli[j]); cli[j].fd = fd; cli[j].phase = P_REQ; cli[j].local = ntohl(peer.sin_addr.s_addr) == 0x7f000001;
            if (li == 1) { cli[j].tls = tls_accept(fd); if (!cli[j].tls) { close(fd); cli[j].fd = -1; } }
        }
        for (int i = 0; i < MAXCLI; i++) if (cli[i].fd >= 0) {
            struct pollfd *p = NULL; for (int m = firstcli; m < np; m++) if (pf[m].fd == cli[i].fd) { p = &pf[m]; break; }
            if (!p || !p->revents) continue;
            if (p->revents & (POLLERR | POLLHUP | POLLNVAL)) { cli_close(&cli[i]); continue; }
            struct client *c = &cli[i];
            if (c->tls && tls_pump(c->tls, p->revents & POLLIN, p->revents & POLLOUT) < 0) { cli_close(c); continue; }
            if (c->phase == P_CLOSING) { if (tls_flushed(c->tls)) cli_close(c); continue; }
            if (c->phase == P_REQ && ((p->revents & POLLIN) || c->tls)) {
                if (c->body) {                                   /* collecting a POST body */
                    int n = cli_recv(c, c->body + c->body_len, c->body_want - c->body_len);
                    if (n < 0) { cli_close(c); continue; }
                    if (n == 0) { if (c->tls && tls_pump(c->tls, 0, 1) < 0) cli_close(c); continue; }
                    c->body_len += n;
                    if (c->body_len == c->body_want) { handle_request(c); if (c->fd >= 0 && c->phase != P_REQ) cli_after_write(c, cli_write(c)); }
                    else if (c->tls && tls_pump(c->tls, 0, 1) < 0) cli_close(c);
                    continue;
                }
                int n = cli_recv(c, c->req + c->reqlen, sizeof c->req - 1 - c->reqlen);
                if (n < 0) { cli_close(c); continue; }
                if (n == 0) { if (c->tls && tls_pump(c->tls, 0, 1) < 0) cli_close(c); continue; }
                c->reqlen += n; c->req[c->reqlen] = 0;
                char *hend = strstr(c->req, "\r\n\r\n");
                if (hend && !strncmp(c->req, "POST ", 5) && strstr(c->req, "action=save")) {
                    /* Start collecting the body (headers stay in req; any body bytes already read move over). */
                    const char *cl = strstr(c->req, "Content-Length:"); size_t want = cl ? strtoul(cl + 15, NULL, 10) : 0;
                    if (!want || want > 2 * 1024 * 1024) { cli_close(c); continue; }
                    c->body = malloc(want); c->body_want = want; c->body_len = c->reqlen - (hend + 4 - c->req);
                    if (!c->body) { cli_close(c); continue; }
                    memcpy(c->body, hend + 4, c->body_len); c->reqlen = hend + 4 - c->req; c->req[c->reqlen] = 0;
                    if (c->body_len == c->body_want) { handle_request(c); if (c->fd >= 0 && c->phase != P_REQ) cli_after_write(c, cli_write(c)); }
                    continue;
                }
                if (hend) { handle_request(c); if (c->fd >= 0 && c->phase != P_REQ) cli_after_write(c, cli_write(c)); }
                else if (c->reqlen >= (int)sizeof c->req - 1) cli_close(c);
                else if (c->tls && tls_pump(c->tls, 0, 1) < 0) cli_close(c);
            } else if ((p->revents & POLLOUT) && c->phase != P_REQ && c->phase != P_LIVE_WAIT) {
                cli_after_write(c, cli_write(c));
            } else if (c->tls && c->fd >= 0 && tls_pump(c->tls, 0, 1) < 0) cli_close(c);
        }
        time_t now = time(NULL);
        if (last_frame_time && now - last_frame_time > 10) { fprintf(stderr, "no frames for 10 s, exiting for restart\n"); rec_stop(); exit(3); }
        if (rec_fd >= 0 && now - rec_last_write > REC_IDLE_CLOSE) { fprintf(stderr, "recording idle, closing\n"); rec_stop(); }
    }
}
