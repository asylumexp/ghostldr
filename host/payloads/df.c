/* Print free space of the main writable filesystems to klog. */
#include <ps5/klog.h>
#include <stdio.h>
#include <sys/mount.h>

int main(void)
{
    static const char *paths[] = {"/data", "/user", "/user/app", "/mnt/usb0", "/mnt/usb1", "/mnt/ext0", "/system_data"};
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        struct statfs fs;
        if (statfs(paths[i], &fs) == 0)
            klog_printf("[df] %s %s on %s free %llu MiB of %llu MiB\n", paths[i], fs.f_fstypename,
                        fs.f_mntonname, (unsigned long long)fs.f_bavail * fs.f_bsize >> 20,
                        (unsigned long long)fs.f_blocks * fs.f_bsize >> 20);
        else
            klog_printf("[df] %s statfs failed\n", paths[i]);
    }
    return 0;
}
