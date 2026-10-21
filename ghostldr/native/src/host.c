/* Host (SWRR process) symbol resolution and the libkernel function table. */
#include "ghost.h"

struct kfuncs K;
struct boot_ctx *g_ctx;

/*
 * Call fn(a1..a6) with a return address inside the SWRR eboot.  libkernel's
 * dlsym-style entry points inspect their caller, so calls from JIT memory go
 * through the eboot's "call rbx; ret" gadget (as Luac0re's dlsym test does).
 * The extra 8-byte adjustment keeps the callee's stack 16-byte aligned.
 */
__attribute__((naked)) static u64 gadget_call(u64 gadget, u64 fn, u64 a1, u64 a2, u64 a3, u64 a4,
                                              u64 a5, u64 a6)
{
    __asm__ volatile(
        ".intel_syntax noprefix\n"
        "push rbx\n"
        "sub rsp, 8\n"
        "mov rbx, rsi\n"
        "mov rax, rdi\n"
        "mov rdi, rdx\n"
        "mov rsi, rcx\n"
        "mov rdx, r8\n"
        "mov rcx, r9\n"
        /* entry [rsp+8]=a5, [rsp+16]=a6; push + sub moved rsp by 16 */
        "mov r8, [rsp + 24]\n"
        "mov r9, [rsp + 32]\n"
        "call rax\n"
        "add rsp, 8\n"
        "pop rbx\n"
        "ret\n"
        ".att_syntax prefix\n");
}

u64 call_gadget6(u64 fn, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6)
{
    return gadget_call(g_ctx->call_gadget, fn, a1, a2, a3, a4, a5, a6);
}

void *host_dlsym(u32 handle, const char *name)
{
    void *addr = NULL;
    int rc = (int)call_gadget6(g_ctx->dlsym, handle, (u64)name, (u64)&addr, 0, 0, 0);
    if (rc != 0)
        return NULL;
    return addr;
}

int host_find_module(const char *name, u32 *handle_out)
{
    u32 handles[256];
    size_t count = 0;
    if (!K.module_list || K.module_list(handles, ARRAY_SIZE(handles), &count) != 0)
        return -1;
    static struct sce_module_info info;
    for (size_t i = 0; i < count; i++) {
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (K.module_info(handles[i], &info) == 0 && strcmp(info.name, name) == 0) {
            *handle_out = handles[i];
            return 0;
        }
    }
    return -1;
}

#define RESOLVE(field, lib, sym)                                         \
    do {                                                                 \
        *(void **)&K.field = host_dlsym((u32)(lib), sym);                \
        if (!K.field)                                                    \
            missing++;                                                   \
    } while (0)

int host_init(struct boot_ctx *ctx)
{
    g_ctx = ctx;
    u32 lk = (u32)ctx->libkernel_handle;
    int missing = 0;
    RESOLVE(getpid, lk, "getpid");
    RESOLVE(sendto, lk, "sendto");
    RESOLVE(debug_out, lk, "sceKernelDebugOutText");
    log_init(ctx);
    RESOLVE(usleep, lk, "sceKernelUsleep");
    RESOLVE(pthread_create, lk, "scePthreadCreate");
    RESOLVE(pthread_attr_init, lk, "scePthreadAttrInit");
    RESOLVE(pthread_attr_setstacksize, lk, "scePthreadAttrSetstacksize");
    RESOLVE(pthread_attr_destroy, lk, "scePthreadAttrDestroy");
    RESOLVE(open, lk, "sceKernelOpen");
    RESOLVE(read, lk, "sceKernelRead");
    RESOLVE(pread, lk, "sceKernelPread");
    RESOLVE(write, lk, "sceKernelWrite");
    RESOLVE(close, lk, "sceKernelClose");
    RESOLVE(lseek, lk, "sceKernelLseek");
    RESOLVE(fstat, lk, "sceKernelFstat");
    RESOLVE(stat, lk, "sceKernelStat");
    RESOLVE(mmap, lk, "sceKernelMmap");
    RESOLVE(munmap, lk, "sceKernelMunmap");
    RESOLVE(mprotect, lk, "sceKernelMprotect");
    RESOLVE(reserve_va, lk, "sceKernelReserveVirtualRange");
    RESOLVE(map_flex, lk, "sceKernelMapFlexibleMemory");
    RESOLVE(alloc_direct, lk, "sceKernelAllocateDirectMemory");
    RESOLVE(map_direct, lk, "sceKernelMapDirectMemory");
    RESOLVE(direct_size, lk, "sceKernelGetDirectMemorySize");
    RESOLVE(avail_direct, lk, "sceKernelAvailableDirectMemorySize");
    RESOLVE(avail_flex, lk, "sceKernelAvailableFlexibleMemorySize");
    RESOLVE(module_list, lk, "sceKernelGetModuleList");
    RESOLVE(module_info, lk, "sceKernelGetModuleInfo");
    RESOLVE(module_info_from_addr, lk, "sceKernelGetModuleInfoFromAddr");
    RESOLVE(load_start_module, lk, "sceKernelLoadStartModule");
    RESOLVE(virtual_query, lk, "sceKernelVirtualQuery");
    RESOLVE(recvmsg, lk, "recvmsg");
    RESOLVE(jit_map, lk, "sceKernelJitMapSharedMemory");
    RESOLVE(errno_fn, lk, "__error");
    RESOLVE(get_proc_param, lk, "sceKernelGetProcParam");
    logf("host_init: libkernel handle %u, %d functions missing", lk, missing);
    return missing;
}
