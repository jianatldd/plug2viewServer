/* Isolated tests of the actual recording lifecycle, with an injected monotonic
 * clock. No camera controls, user recordings, or account configuration touched. */
#undef NDEBUG
#include <assert.h>
#include <time.h>
static long long test_ms=100000;
static int test_clock(clockid_t id,struct timespec *t) {
    (void)id; t->tv_sec=test_ms/1000; t->tv_nsec=(test_ms%1000)*1000000; return 0;
}
#define clock_gettime test_clock
#define MOTION_GUARD_FILE "/tmp/plug2view-policy-test/guard"
#define main camera_main
#include "../camera/src/camd.c"
#undef main
static void existing_motion_file(void) {
    FILE *f=tmpfile(); assert(f); rec_fd=dup(fileno(f)); fclose(f);
    assert(rec_fd>=0); rec_manual=0; rec_manual_until=0; rec_nsamples=0;
    rec_mdat_pos=0; rec_active=1; strcpy(rec_name,"isolated-policy-test");
}
int main(void) {
    assert(mkdir("/tmp/plug2view-policy-test",0700)==0);
    assert(!motion_suppressed());
    int guard=motion_guard_begin(); assert(guard>=0); assert(motion_suppressed());
    test_ms+=20000; assert(motion_suppressed()); /* lock covers long moves */
    motion_guard_end(guard); assert(motion_suppressed());
    test_ms+=6999; assert(motion_suppressed());
    test_ms++; assert(!motion_suppressed());
    guard=motion_guard_begin(); assert(guard>=0);
    save_dir="/tmp"; char name[256];
    assert(rec_start("motion",1,name,sizeof name)==-4);
    existing_motion_file();
    assert(rec_start("manual",1,name,sizeof name)==0); /* explicit recording allowed while moving */
    assert(rec_manual && rec_manual_until==test_ms+300000);
    long long deadline=rec_manual_until;
    test_ms+=100000; assert(rec_start("manual",1,name,sizeof name)==0);
    assert(rec_manual_until==deadline); /* repeated On must not extend */
    int state=0;
    assert(!google_camera_control(GOOGLE_RECORDING,1,&state) && state);
    assert(rec_manual_until==deadline);
    test_ms=deadline-1; rec_expire(); assert(rec_fd>=0);
    test_ms++; rec_expire(); assert(rec_fd<0 && !rec_manual && !rec_manual_until);
    assert(!google_camera_control(GOOGLE_RECORDING,-1,&state) && !state);
    motion_guard_end(guard); test_ms+=7000;
    existing_motion_file(); test_ms+=600000; rec_expire(); assert(rec_fd>=0); rec_stop();
    unlink(MOTION_GUARD_FILE); assert(rmdir("/tmp/plug2view-policy-test")==0);
    puts("PASS: movement lock/cooldown, motion start suppression, manual override, five-minute stop, repeated On deadline, Google query, motion ownership");
    return 0;
}
