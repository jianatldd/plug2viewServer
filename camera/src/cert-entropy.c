/* Linux 2.6.21 has /dev/urandom but no getrandom syscall. */
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>
int mbedtls_hardware_poll(void *ctx,unsigned char *out,size_t len,size_t *olen) {
    (void)ctx;*olen=0;int fd=open("/dev/urandom",O_RDONLY);if(fd<0)return -1;
    while(*olen<len) {ssize_t n=read(fd,out+*olen,len-*olen);if(n<0&&errno==EINTR)continue;if(n<=0){close(fd);return -1;}*olen+=n;}
    close(fd);return 0;
}
