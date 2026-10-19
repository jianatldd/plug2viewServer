#ifndef P2V_RECORDING_POLICY_H
#define P2V_RECORDING_POLICY_H
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/file.h>
#ifndef MOTION_GUARD_FILE
#define MOTION_GUARD_FILE "/tmp/plug2view-motion-guard"
#endif
#define MOTION_SETTLE_MS 7000
#define MANUAL_RECORD_MS (300LL * 1000)
static long long recording_clock_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
/* The motor holds this lock while stepping. Its deadline survives short moves
 * between detector polls; monotonic time is unaffected by NTP clock updates. */
static int motion_guard_refresh(int fd) {
    char b[48]; int n=snprintf(b,sizeof b,"%lld\n",recording_clock_ms()+MOTION_SETTLE_MS);
    return pwrite(fd,b,n,0)==n && ftruncate(fd,n)==0;
}
static int motion_guard_begin(void) {
    int fd=open(MOTION_GUARD_FILE,O_RDWR|O_CREAT,0600);
    if(fd<0) return -1;
    if(flock(fd,LOCK_EX) || !motion_guard_refresh(fd)) { close(fd); return -1; }
    return fd;
}
static void motion_guard_end(int fd) {
    motion_guard_refresh(fd); close(fd);
}
static int motion_suppressed(void) {
    int fd=open(MOTION_GUARD_FILE,O_RDONLY);
    if(fd<0) return 0;
    if(flock(fd,LOCK_SH|LOCK_NB)) { close(fd); return 1; }
    char b[48]={0}; long long until=0;
    int n=read(fd,b,sizeof b-1);
    if(n>0) sscanf(b,"%lld",&until);
    close(fd);
    return until>recording_clock_ms();
}
#endif
