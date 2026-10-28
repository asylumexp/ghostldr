/* overlay.c's one extra host call: write a file (the frames it checks). */
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

int sys_thread(void *(*fn)(void *), void *arg)
{
    pthread_t t;
    return pthread_create(&t, NULL, fn, arg);
}

int sys_usleep(unsigned us) { return usleep(us); }

int sys_write_file(const char *path, const void *data, unsigned long n)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t w = fwrite(data, 1, n, f);
    fclose(f);
    return w == n ? 0 : -1;
}
