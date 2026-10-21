/* Host OS side of the np_host harness (see np_host.c). */
#include <stdio.h>
#include <unistd.h>

long sys_lseek(int fd, long off, int whence) { return lseek(fd, off, whence); }
