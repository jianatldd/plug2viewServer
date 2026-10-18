/*
 * vidcap - dump N seconds of raw frames from a V4L2 device to a file, for probing the
 * Sonix module's second stream (/dev/video1, fourcc H264). Same old-kernel buffer layout
 * trick as camd. Prints per-frame sizes so the frame rate and bitrate can be judged.
 *
 * Build: zig cc -target mipsel-linux-musleabi -mcpu=mips32 -Os -static -s -o vidcap vidcap.c
 * Usage: vidcap <dev> <fourcc> <WxH> <seconds> <outfile>     e.g. vidcap /dev/video1 H264 1280x720 5 /tmp/t.h264
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

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

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: vidcap <dev> <fourcc> <WxH> <seconds> <outfile>\n"); return 1; }
    int w = 0, h = 0; sscanf(argv[3], "%dx%d", &w, &h);
    int secs = atoi(argv[4]);
    const char *fcc = argv[2];
    int fd = open(argv[1], O_RDWR | O_NONBLOCK);
    if (fd < 0) { perror(argv[1]); return 2; }
    struct v4l2_format f; memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; f.fmt.pix.width = w; f.fmt.pix.height = h;
    f.fmt.pix.pixelformat = v4l2_fourcc(fcc[0], fcc[1], fcc[2], fcc[3]); f.fmt.pix.field = V4L2_FIELD_ANY;
    if (ioctl(fd, VIDIOC_S_FMT, &f) < 0) { perror("S_FMT"); return 2; }
    fprintf(stderr, "format set: %ux%u sizeimage=%u\n", f.fmt.pix.width, f.fmt.pix.height, f.fmt.pix.sizeimage);
    struct v4l2_requestbuffers rb; memset(&rb, 0, sizeof rb);
    rb.count = NBUF; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &rb) < 0) { perror("REQBUFS"); return 2; }
    void *buf[NBUF]; size_t len[NBUF];
    for (unsigned i = 0; i < rb.count && i < NBUF; i++) {
        struct v4l2_buffer32 b; memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        if (ioctl(fd, VIDIOC_QUERYBUF32, &b) < 0) { perror("QUERYBUF"); return 2; }
        len[i] = b.length;
        buf[i] = mmap(0, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        if (buf[i] == MAP_FAILED) { perror("mmap"); return 2; }
        if (ioctl(fd, VIDIOC_QBUF32, &b) < 0) { perror("QBUF"); return 2; }
    }
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &t) < 0) { perror("STREAMON"); return 2; }
    int out = open(argv[5], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { perror(argv[5]); return 2; }
    time_t end = time(NULL) + secs; unsigned frames = 0; unsigned long long bytes = 0; unsigned minsz = ~0u, maxsz = 0;
    while (time(NULL) < end) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 2000) <= 0) { fprintf(stderr, "timeout waiting for frame\n"); break; }
        struct v4l2_buffer32 b; memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_DQBUF32, &b) < 0) { if (errno == EAGAIN) continue; perror("DQBUF"); break; }
        if (b.bytesused > 0) {
            write(out, buf[b.index], b.bytesused);
            frames++; bytes += b.bytesused;
            if (b.bytesused < minsz) minsz = b.bytesused;
            if (b.bytesused > maxsz) maxsz = b.bytesused;
            if (frames <= 5) { const unsigned char *d = buf[b.index]; fprintf(stderr, "frame %u: %u bytes, head %02x %02x %02x %02x %02x %02x\n", frames, b.bytesused, d[0], d[1], d[2], d[3], d[4], d[5]); }
        }
        ioctl(fd, VIDIOC_QBUF32, &b);
    }
    ioctl(fd, VIDIOC_STREAMOFF, &t);
    close(out);
    fprintf(stderr, "%u frames in %d s (%.1f fps), %llu bytes (%.2f Mbit/s), frame size %u..%u\n",
            frames, secs, frames / (double)secs, bytes, bytes * 8.0 / secs / 1e6, minsz, maxsz);
    return 0;
}
