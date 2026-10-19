/* Included by camd.c: on-demand fMP4 HLS using the existing encoder and muxer.
 * Segments live on the SD card, never internal flash. Retain 16 segments while the
 * playlist exposes at least three target durations, allowing slow clients time to fetch.
 * One access unit is buffered by the existing frame ring; no second video capture. */
#define HLS_KEEP 16
#define HLS_TARGET 3
static struct {
    int active, fd;
    unsigned serial, segment, fragment, last_frame, completed, discontinuity;
    int waiting_key;
    long long epoch, start;
    time_t touched, reported;
    char dir[512], prefix[64], init[640], partial[640];
    struct { unsigned id, discontinuity; double duration; long long start, end; char path[640]; } files[HLS_KEEP];
} hs = {.fd = -1};
static void hls_append(const struct hframe *f, const struct hframe *nx);

static int hls_write(int fd, const void *data, size_t len) {
    const unsigned char *p = data;
    while (len) { ssize_t n = write(fd, p, len); if (n < 0 && errno == EINTR) continue; if (n <= 0) return 0; p += n; len -= n; }
    return 1;
}
static void hls_stop(const char *reason) {
    fprintf(stderr, "hls: stopped reason=%s completed=%u errno=%d\n", reason, hs.completed, errno);
    if (hs.fd >= 0) close(hs.fd);
    hs.fd = -1;
    if (*hs.partial) unlink(hs.partial);
    /* Completed files remain available for clients; next start cleans our own files. */
    hs.active = 0;
}
/* A capture stall may make the unfinished GOP too long for this playlist's
 * fixed target duration. Drop only that partial segment, then resume at an IDR.
 * Keep the init, completed files and media sequence available to the player. */
static void hls_skip_partial(void) {
    if (hs.fd >= 0) {
        close(hs.fd); hs.fd = -1; unlink(hs.partial); hs.segment--;
    }
    if (!hs.waiting_key) {
        hs.discontinuity++; hs.waiting_key = 1;
        fprintf(stderr, "hls: capture stall; resuming at next keyframe discontinuity=%u\n", hs.discontinuity);
    }
}
static int hls_start(void) {
    hs.touched = time(NULL);
    if (hs.active) return 1;
    struct statfs fs;
    if (!save_dir || statfs(save_dir, &fs) || fs.f_type != 0x4d44 ||
        (unsigned long long)fs.f_bavail * fs.f_bsize < 32ULL * 1024 * 1024 || strlen(save_dir) > 460) return 0;
    if (*hs.init) unlink(hs.init);
    for (int i = 0; i < HLS_KEEP; i++) { if (*hs.files[i].path) unlink(hs.files[i].path); memset(&hs.files[i], 0, sizeof hs.files[i]); }
    snprintf(hs.dir, sizeof hs.dir, "%s/.hls", save_dir);
    if (mkdir(hs.dir, 0700) && errno != EEXIST) return 0;
    /* Only remove this prototype's regular segment files left by a previous run. */
    DIR *d = opendir(hs.dir); struct dirent *de;
    if (d) { while ((de = readdir(d))) {
        if (strncmp(de->d_name, "p2v-", 4) || strspn(de->d_name, "p2v-init0123456789abcdef.mp4srt-") != strlen(de->d_name)) continue;
        char full[800]; struct stat st; snprintf(full, sizeof full, "%s/%s", hs.dir, de->d_name);
        if (!lstat(full, &st) && S_ISREG(st.st_mode)) unlink(full);
    } closedir(d); }
    snprintf(hs.prefix, sizeof hs.prefix, "p2v-%lx-%x-%x", (unsigned long)time(NULL), (unsigned)getpid(), ++hs.serial);
    snprintf(hs.init, sizeof hs.init, "%s/%s-init.mp4", hs.dir, hs.prefix);
    hs.segment = hs.fragment = hs.last_frame = hs.completed = hs.discontinuity = 0;
    hs.waiting_key = 0;
    hs.epoch = hs.start = 0; hs.active = 1; hs.fd = -1; hs.partial[0] = 0;
    /* Ignore broken pre-roll before a capture gap or an overlong GOP. Otherwise
     * every retry replays the same bad timestamps until that history ages out. */
    unsigned first = 0, key = ring_count;
    for (unsigned i = 0; i + 1 < ring_count; i++) {
        const struct hframe *f = ring_at(i), *nx = ring_at(i + 1);
        if (nx->ts <= f->ts || nx->ts - f->ts > 1000000) { first = i + 1; key = ring_count; }
        if (f->key) {
            if (key < ring_count && f->ts - ring_at(key)->ts >= (HLS_TARGET * 1000000LL + 500000)) first = i;
            key = i;
        }
    }
    for (unsigned i = first; i + 1 < ring_count && hs.active; i++) hls_append(ring_at(i), ring_at(i + 1));
    fprintf(stderr, "hls: started on SD card\n"); return hs.active;
}
static void hls_append(const struct hframe *f, const struct hframe *nx) {
    if (f->seq == hs.last_frame) return;
    hs.last_frame = f->seq;
    if (hs.epoch && (f->ts <= hs.epoch || nx->ts <= f->ts)) { fprintf(stderr, "hls: timestamp delta_us=%lld epoch_delta_us=%lld\n", nx->ts - f->ts, f->ts - hs.epoch); hls_stop("timestamp"); return; }
    if (!hs.epoch) {
        if (!f->key) return;
        unsigned char sps[128], pps[64]; int sl, pl;
        if (!sps_pps_from_au(f->data, f->len, sps, &sl, pps, &pl) || sl < 4) return;
        struct bb b = {0}; fmp4_init_segment(&b, sps, sl, pps, pl);
        int fd = open(hs.init, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        int ok = fd >= 0 && hls_write(fd, b.d, b.n); if (fd >= 0) close(fd); free(b.d);
        if (!ok) { hls_stop("init_write"); return; } hs.epoch = f->ts;
    }
    if (hs.fd >= 0 && f->ts - hs.start >= HLS_TARGET * 1000000LL + 500000) hls_skip_partial();
    if (hs.fd >= 0 && f->key) {
        double duration = (f->ts - hs.start) / 1e6;
        /* RFC 8216 compares EXTINF rounded to the nearest integer. */
        close(hs.fd); hs.fd = -1;
        unsigned slot = hs.completed % HLS_KEEP;
        if (*hs.files[slot].path) unlink(hs.files[slot].path);
        snprintf(hs.files[slot].path, sizeof hs.files[slot].path, "%s/%s-%u.m4s", hs.dir, hs.prefix, hs.segment);
        if (rename(hs.partial, hs.files[slot].path)) { hls_stop("segment_rename"); return; }
        hs.files[slot].id = hs.segment; hs.files[slot].duration = duration;
        hs.files[slot].discontinuity = hs.discontinuity;
        hs.files[slot].start = hs.start; hs.files[slot].end = f->ts; hs.completed++;
    }
    if (hs.fd < 0) {
        if (!f->key) return;
        hs.waiting_key = 0;
        hs.segment++; hs.start = f->ts;
        snprintf(hs.partial, sizeof hs.partial, "%s/%s.part", hs.dir, hs.prefix);
        hs.fd = open(hs.partial, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (hs.fd < 0) { hls_stop("segment_open"); return; }
    }
    if (nx->ts - hs.start >= HLS_TARGET * 1000000LL + 500000) { hls_skip_partial(); return; }
    unsigned char *avcc = malloc(f->len + 64);
    if (!avcc) { hls_stop("allocation"); return; }
    size_t sl = au_to_avcc(f->data, f->len, avcc, f->len + 64);
    struct bb b = {0};
    fmp4_fragment(&b, ++hs.fragment, (unsigned long long)(f->ts - hs.epoch), (unsigned)(nx->ts - f->ts), f->key, avcc, sl);
    free(avcc); int ok = hls_write(hs.fd, b.d, b.n); free(b.d);
    if (!ok) { hls_stop("segment_write"); }
}
static void hls_tick(void) {
    if (!hs.active) return;
    if (time(NULL) - hs.touched > 90) { hls_stop("idle"); return; }
    if (ring_count >= 2) hls_append(ring_at(ring_count - 2), ring_newest());
}
static void hls_request(struct client *c, const char *path, const char *token) {
    if (!strcmp(path, "/hls/live.m3u8")) {
        if (!hls_start()) { send_json(c, 503, "{\"error\":\"HLS needs a mounted FAT32 SD card with 32 MB free\"}"); return; }
        unsigned first = hs.completed; double duration = 0;
        while (first && hs.completed - first < HLS_KEEP && duration < 3 * HLS_TARGET) duration += hs.files[--first % HLS_KEEP].duration;
        if (duration < 3 * HLS_TARGET) {
            if (token != c->hls_token) snprintf(c->hls_token, sizeof c->hls_token, "%s", token);
            c->phase = P_HLS_WAIT; return;
        }
        char body[8192], suffix[210]; suffix[0] = 0;
        if (*token) snprintf(suffix, sizeof suffix, "?token=%s", token);
        int n = snprintf(body, sizeof body, "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:%d\n#EXT-X-START:TIME-OFFSET=-%d,PRECISE=YES\n#EXT-X-INDEPENDENT-SEGMENTS\n#EXT-X-MEDIA-SEQUENCE:%u\n#EXT-X-DISCONTINUITY-SEQUENCE:%u\n#EXT-X-MAP:URI=\"%s-init.mp4%s\"\n", HLS_TARGET, 3 * HLS_TARGET, hs.files[first % HLS_KEEP].id, hs.files[first % HLS_KEEP].discontinuity, hs.prefix, suffix);
        for (unsigned i = first; i < hs.completed; i++) {
            unsigned slot = i % HLS_KEEP; char date[40]; time_t sec = hs.files[slot].start / 1000000;
            strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", gmtime(&sec));
            if (i > first && hs.files[slot].discontinuity != hs.files[(i - 1) % HLS_KEEP].discontinuity)
                n += snprintf(body + n, sizeof body - n, "#EXT-X-DISCONTINUITY\n");
            n += snprintf(body + n, sizeof body - n, "#EXT-X-PROGRAM-DATE-TIME:%s.%03dZ\n#EXTINF:%.6f,\n%s-%u.m4s%s\n", date, (int)(hs.files[slot].start / 1000 % 1000), hs.files[slot].duration, hs.prefix, hs.files[slot].id, suffix);
        }
        send_simple(c, 200, "application/vnd.apple.mpegurl", body, NULL); return;
    }
    hs.touched = time(NULL);
    const char *base = path + 5;
    if (*hs.init && !strcmp(base, strrchr(hs.init, '/') + 1)) { send_file_forked(c, hs.init); return; }
    for (int i = 0; i < HLS_KEEP; i++) if (*hs.files[i].path && !strcmp(base, strrchr(hs.files[i].path, '/') + 1)) {
        if (*token && time(NULL) - hs.reported >= 10) {
            hs.reported = time(NULL);
            fprintf(stderr, "hls: Google fetch segment=%u behind=%u start_age_ms=%lld end_age_ms=%lld\n", hs.files[i].id, hs.segment - hs.files[i].id - 1, (now_us() - hs.files[i].start) / 1000, (now_us() - hs.files[i].end) / 1000);
        }
        send_file_forked(c, hs.files[i].path); return;
    }
    send_simple(c, 404, "text/plain", "HLS segment expired or not found", NULL);
}
