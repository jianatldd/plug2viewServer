/* Run on the MIPS camera. Exercises the real muxer with synthetic timestamps;
 * never opens the camera, changes its configuration, or touches live HLS files. */
#define main camd_application_main
#include "../camera/src/camd.c"
#undef main
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "FAIL at line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)

int main(void) {
    char directory[] = "/tmp/p2v-hls-test-XXXXXX";
    CHECK(mkdtemp(directory));
    snprintf(hs.dir, sizeof hs.dir, "%s", directory);
    strcpy(hs.prefix, "p2v-test");
    hs.active = 1; hs.fd = -1; hs.epoch = now_us() - 30000000;
    unsigned char sample[] = {0, 0, 0, 1, 0x65, 0x88, 0x84, 0x21};
    for (unsigned i = 1; i <= 140; i++) {
        long long t = hs.epoch + i * 100000LL + (i >= 25 ? 1400000 : 0) + (i >= 65 ? 5000000 : 0);
        unsigned next = i + 1;
        long long nt = hs.epoch + next * 100000LL + (next >= 25 ? 1400000 : 0) + (next >= 65 ? 5000000 : 0);
        struct hframe f = {.data=sample, .len=sizeof sample, .key=i % 10 == 1, .ts=t, .seq=i};
        struct hframe nx = {.ts=nt};
        hls_append(&f, &nx);
        CHECK(hs.active); /* Neither a 1.4 s nor a 5 s pause restarts HLS. */
    }
    CHECK(hs.completed >= 10 && hs.completed < HLS_KEEP);
    CHECK(hs.discontinuity == 1);
    for (unsigned i = 0; i < hs.completed; i++) {
        CHECK(hs.files[i].id == i + 1);
        CHECK(hs.files[i].duration < HLS_TARGET + 0.5);
        CHECK(access(hs.files[i].path, R_OK) == 0);
    }
    struct client c = {0};
    hls_request(&c, "/hls/live.m3u8", "");
    CHECK(c.phase == P_SEND && c.buf);
    char *response = calloc(c.len + 1, 1); CHECK(response);
    memcpy(response, c.buf, c.len);
    CHECK(strstr(response, "#EXT-X-DISCONTINUITY\n"));
    CHECK(strstr(response, "#EXT-X-PROGRAM-DATE-TIME:"));
    free(response); free(c.buf);
    hls_stop("test_finished");
    for (unsigned i = 0; i < hs.completed; i++) unlink(hs.files[i].path);
    CHECK(rmdir(directory) == 0);
    puts("PASS: 1.4 s/5 s capture stalls preserve HLS, retained files, consecutive IDs and discontinuity signaling");
    return 0;
}
