/* The libc headers know newer flags than Linux 2.6.21 implements. curl falls
 * back to fcntl when these macros are absent. This affects certificate tools only. */
#include <sys/socket.h>
#undef SOCK_CLOEXEC
#undef SOCK_NONBLOCK
