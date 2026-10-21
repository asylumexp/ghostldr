/* List processes (pid, state, comm, rss) and free memory to klog. */
#include <ps5/klog.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>

int main(void)
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t len = 0;
    if (sysctl(mib, 3, NULL, &len, NULL, 0) != 0) {
        klog_printf("[ps] sysctl size failed\n");
        return 1;
    }
    len += 16 * sizeof(struct kinfo_proc);
    struct kinfo_proc *kp = malloc(len);
    if (!kp || sysctl(mib, 3, kp, &len, NULL, 0) != 0) {
        klog_printf("[ps] sysctl failed\n");
        return 1;
    }
    size_t n = len / sizeof(struct kinfo_proc);
    klog_printf("[ps] %zu processes (kinfo_proc %zu bytes, first ki_structsize %d)\n", n, sizeof(struct kinfo_proc),
                kp[0].ki_structsize);
    for (size_t i = 0; i < n; i++)
        klog_printf("[ps] pid %5d stat %d flag %08x rss %6ld KiB %s\n", kp[i].ki_pid, kp[i].ki_stat, kp[i].ki_flag,
                    (long)kp[i].ki_rssize * 16, kp[i].ki_comm);
    unsigned int fc = 0;
    size_t fl = sizeof(fc);
    if (sysctlbyname("vm.stats.vm.v_free_count", &fc, &fl, NULL, 0) == 0)
        klog_printf("[ps] v_free_count %u\n", fc);
    return 0;
}
