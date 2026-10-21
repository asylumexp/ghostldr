/* Dump a loaded module's mapped image from a target title process to /data. */
#include <ps5/kernel.h>
#include <ps5/klog.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>


typedef struct app_info {
    uint32_t app_id;
    uint64_t unknown1;
    char title_id[14];
    char unknown2[0x3c];
} app_info_t;

/* SWRR US or EU, unless built for one title with -DTARGET_TITLE_ID */
static int is_target(const char *title_id)
{
#ifdef TARGET_TITLE_ID
    return strcmp(title_id, TARGET_TITLE_ID) == 0;
#else
    return strcmp(title_id, "CUSA03474") == 0 || strcmp(title_id, "CUSA03492") == 0;
#endif
}
int sceKernelGetAppInfo(pid_t pid, app_info_t *info);

static pid_t find_target(void)
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0)
        return -1;
    uint8_t *buf = malloc(size);
    pid_t r = -1;
    if (buf && sysctl(mib, 4, buf, &size, NULL, 0) == 0) {
        for (uint8_t *c = buf; c < buf + size;) {
            struct kinfo_proc *p = (struct kinfo_proc *)c;
            app_info_t info = {0};
            if (p->ki_structsize <= 0)
                break;
            if (strcmp(p->ki_comm, "eboot.bin") == 0 && sceKernelGetAppInfo(p->ki_pid, &info) == 0 &&
                is_target(info.title_id)) {
                r = p->ki_pid;
                break;
            }
            c += p->ki_structsize;
        }
    }
    free(buf);
    return r;
}

int main(void)
{
    pid_t pid = find_target();
    if (pid < 0) {
        klog_printf("[dumpmod] target not found\n");
        return 1;
    }
    uint32_t handle = 0;
    const char *names[] = {"eboot.bin", "ps2emu_eboot-final", NULL};
    int ok = -1;
    for (int i = 0; names[i] && ok != 0; i++)
        ok = kernel_dynlib_handle(pid, names[i], &handle);
    if (ok != 0)
        handle = 0; /* main executable is normally handle 0 */
    intptr_t base = kernel_dynlib_mapbase_addr(pid, handle);
    klog_printf("[dumpmod] pid %d handle %u base %lx\n", pid, handle, (long)base);
    size_t len = 0x400000;
    uint8_t *buf = malloc(len);
    int rc = kernel_proc_copyout(pid, base, buf, len);
    int fd = open("/data/swrr-eboot-mem.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    write(fd, buf, len);
    close(fd);
    klog_printf("[dumpmod] copyout rc %d first %02x%02x%02x%02x @0x31aa9 %02x%02x%02x\n", rc, buf[0], buf[1],
                buf[2], buf[3], buf[0x31aa9], buf[0x31aaa], buf[0x31aab]);
    return 0;
}
