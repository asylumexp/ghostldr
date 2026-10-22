/* Host OS side of the NP online harness (np_online.c): threads and sleep. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void sys_thread_create(void *(*fn)(void *), void *arg, uint64_t *tid)
{
    pthread_t t;
    if (pthread_create(&t, NULL, fn, arg) != 0) {
        perror("pthread_create");
        exit(3);
    }
    pthread_detach(t);
    if (tid)
        *tid = (uint64_t)(uintptr_t)t;
}

uint64_t sys_thread_self(void) { return (uint64_t)(uintptr_t)pthread_self(); }

void sys_usleep(unsigned us) { usleep(us); }
