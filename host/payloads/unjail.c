#include <errno.h>
#include <ps5/kernel.h>
#include <ps5/klog.h>
#include <stdint.h>
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
    uint8_t *buffer;
    pid_t result = -1;

    if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || size == 0) {
        return -1;
    }
    buffer = malloc(size);
    if (buffer == NULL || sysctl(mib, 4, buffer, &size, NULL, 0) != 0) {
        free(buffer);
        return -1;
    }
    for (uint8_t *cursor = buffer; cursor < buffer + size;) {
        struct kinfo_proc *process = (struct kinfo_proc *)cursor;
        app_info_t info = {0};

        if (process->ki_structsize <= 0 ||
            cursor + process->ki_structsize > buffer + size) {
            break;
        }
        if (strcmp(process->ki_comm, "eboot.bin") == 0 &&
            sceKernelGetAppInfo(process->ki_pid, &info) == 0 &&
            is_target(info.title_id)) {
            result = process->ki_pid;
            break;
        }
        cursor += process->ki_structsize;
    }
    free(buffer);
    return result;
}

int main(void)
{
    pid_t pid = find_target();
    intptr_t original_root;
    intptr_t system_root;
    int result;

    if (pid < 0) {
        klog_printf("[unjail] target-not-found errno=%d\n", errno);
        return 1;
    }
    original_root = kernel_get_proc_rootdir(pid);
    system_root = kernel_get_root_vnode();
    if (original_root == 0 || system_root == 0) {
        klog_printf(
            "[unjail] pointer lookup failed pid=%d original=0x%lx "
            "system=0x%lx\n",
            pid, original_root, system_root);
        return 2;
    }
    result = kernel_set_proc_rootdir(pid, system_root);
    if (result == 0)
        result = kernel_set_proc_jaildir(pid, system_root);
    klog_printf(
        "[unjail] pid=%d original=0x%lx system=0x%lx result=%d "
        "readback=0x%lx errno=%d\n",
        pid, original_root, system_root, result,
        kernel_get_proc_rootdir(pid), result == 0 ? 0 : errno);
    return result == 0 && kernel_get_proc_rootdir(pid) == system_root ? 0 : 3;
}
