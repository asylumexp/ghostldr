/*
 * hello: the ELF loader's first sample payload (docs/elfldr.md).
 *
 * Built with the OpenOrbis PS4 toolchain (Makefile here) and sent as it
 * comes out of the linker:
 *
 *     nc PS5 9021 < build/elfldr/hello.elf
 *
 * Its stdout and stderr come back on that connection: what it was started
 * as, threads with thread-local variables of their own, malloc, the time,
 * and then exit code 7, which the loader reports in its last line.  Send
 * the ELF as ld.lld writes it: create-fself sets the TLS segment's
 * alignment to 32, which moves the initial values of thread-local
 * variables that ld.lld placed for a smaller one (docs/elfldr.md).
 *
 * MIT license, like the rest of ghostldr.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static __thread int t_count = 100;
static int g_seen[3];

/* Each thread adds to its own counter.  It does not print: OpenOrbis's
 * libc leaves stdout unlocked when the threads are libkernel's, so threads
 * printing at once garble each other's lines. */
static void *worker(void *arg)
{
    long n = (long)arg;
    t_count += (int)n;
    g_seen[n - 1] = t_count;
    return NULL;
}

int main(int argc, char **argv)
{
    printf("hello from %s, running inside SWRR (pid %d)\n", argc > 0 ? argv[0] : "?", (int)getpid());
    pthread_t t[3];
    for (long i = 0; i < 3; i++)
        if (pthread_create(&t[i], NULL, worker, (void *)(i + 1)) != 0)
            printf("hello: pthread_create %ld failed\n", i);
    for (int i = 0; i < 3; i++)
        pthread_join(t[i], NULL);
    for (int i = 0; i < 3; i++)
        printf("hello: thread %d's thread-local counter ended at %d (100 + %d)\n", i + 1, g_seen[i], i + 1);
    printf("hello: main's counter is still %d\n", t_count);

    size_t size = 8 << 20;
    unsigned char *buf = malloc(size);
    if (buf) {
        memset(buf, 0xA5, size);
        unsigned sum = 0;
        for (size_t i = 0; i < size; i += 4096)
            sum += buf[i];
        printf("hello: malloc(8 MiB) at %p, checksum %u\n", (void *)buf, sum);
        free(buf);
    } else {
        printf("hello: malloc(8 MiB) failed\n");
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("hello: monotonic clock %ld.%03ld s\n", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    fprintf(stderr, "hello: this line went to stderr\n");
    printf("hello: exiting with code 7\n");
    return 7;
}
