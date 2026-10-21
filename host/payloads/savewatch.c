/*
 * Wait for SWRR to mount its own save (it mounts briefly while loading the
 * PS2 memory card), then either back the save up to /data or install staged
 * files into it.  Holding a file open inside the mount keeps the emulator's
 * unmount from completing until the copy is done.
 *
 *   -DMODE_BACKUP   copy savedata0/  ->  /data/swrr-save-backup/
 *   -DMODE_INSTALL  copy /data/swrr-save-stage/  ->  savedata0/
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ps5/klog.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SAVE "/mnt/sandbox/CUSA03474_000/savedata0"
#define BACKUP "/data/swrr-save-backup"
#define STAGE "/data/swrr-save-stage2"

static char buf[1 << 20];

static int copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) {
        close(in);
        return -2;
    }
    ssize_t n;
    long total = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, n) != n) {
            close(in);
            close(out);
            return -3;
        }
        total += n;
    }
    fsync(out);
    close(in);
    close(out);
    klog_printf("[savewatch] copied %s -> %s (%ld)\n", src, dst, total);
    return 0;
}

static int copy_file_logged(const char *src, const char *dst)
{
    int rc = copy_file(src, dst);
    if (rc != 0)
        klog_printf("[savewatch] FAILED %s -> %s rc=%d errno=%d\n", src, dst, rc, errno);
    return rc;
}

static int copy_tree(const char *src, const char *dst)
{
    DIR *d = opendir(src);
    if (!d)
        return -1;
    mkdir(dst, 0777);
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char s[512], t[512];
        snprintf(s, sizeof(s), "%s/%s", src, e->d_name);
        snprintf(t, sizeof(t), "%s/%s", dst, e->d_name);
        struct stat st;
        if (stat(s, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            rc |= copy_tree(s, t);
        else
            rc |= copy_file_logged(s, t);
    }
    closedir(d);
    return rc;
}

int main(void)
{
    time_t start = time(NULL);
    klog_printf("[savewatch] waiting for %s\n", SAVE);
    while (time(NULL) - start < 600) {
        int hold = open(SAVE "/VMC0.card", O_RDONLY);
        if (hold >= 0) {
            klog_printf("[savewatch] save mounted, holding fd %d\n", hold);
#ifdef MODE_BACKUP
            int rc = copy_tree(SAVE, BACKUP);
#else
            /* only a read-write mount (the emulator writing the card) works */
            int probe = open(SAVE "/.ghostldr_probe", O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (probe < 0) {
                close(hold);
                klog_printf("[savewatch] read-only mount (errno %d), waiting for a write\n", errno);
                while (access(SAVE "/VMC0.card", F_OK) == 0)
                    usleep(2000);
                continue;
            }
            close(probe);
            unlink(SAVE "/.ghostldr_probe");
            /* let the emulator finish writing its card before replacing it */
            struct stat a, b;
            stat(SAVE "/VMC0.card", &a);
            for (int stable = 0; stable < 10;) {
                usleep(100000);
                stat(SAVE "/VMC0.card", &b);
                if (b.st_mtime == a.st_mtime && b.st_size == a.st_size)
                    stable++;
                else
                    stable = 0, a = b;
            }
#ifdef DELETE_VMC1
            klog_printf("[savewatch] unlink VMC1.card -> %d\n", unlink(SAVE "/VMC1.card"));
#endif
            int rc = copy_tree(STAGE, SAVE);
            struct statfs fs;
            if (statfs(SAVE, &fs) == 0)
                klog_printf("[savewatch] save free %llu bytes\n", (unsigned long long)fs.f_bavail * fs.f_bsize);
#endif
            sync();
            close(hold);
            klog_printf("[savewatch] done rc=%d\n", rc);
            return rc;
        }
        usleep(2000);
    }
    klog_printf("[savewatch] timeout\n");
    return 1;
}
