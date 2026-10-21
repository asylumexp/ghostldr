/* Live probe of the SWRR host: modules, memory budgets, TLS, JIT mapping. */
#include "ghost.h"

/* Bring-up diagnostics (run.py probe).  Built only with `make PROBE=1`: the
 * core has to fit in 240 KiB of the IPU JIT buffer. */
#ifdef GHOSTLDR_PROBE

static u64 read_fs(s64 off)
{
    u64 v;
    __asm__ volatile("mov %%fs:(%1), %0" : "=r"(v) : "r"(off));
    return v;
}

static void probe_modules(void)
{
    u32 handles[256];
    size_t count = 0;
    int rc = K.module_list(handles, ARRAY_SIZE(handles), &count);
    logf("modules: rc=%x count=%lu", rc, (u64)count);
    static struct sce_module_info info;
    for (size_t i = 0; i < count; i++) {
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (K.module_info(handles[i], &info) != 0)
            continue;
        logf("  mod %3u %-32s seg0 %lx+%x seg1 %lx+%x nseg %u", handles[i], info.name,
             info.segs[0].base, info.segs[0].size, info.segs[1].base, info.segs[1].size, info.nsegs);
    }
}

static void probe_memory(void)
{
    off_t start = 0;
    size_t avail = 0, flex = 0;
    size_t total = K.direct_size();
    int rc = K.avail_direct(0, (off_t)total, 0x10000, &start, &avail);
    int rc2 = K.avail_flex(&flex);
    logf("memory: direct total %lx avail %lx (rc %x, at %lx) flex avail %lx (rc %x)", (u64)total,
         (u64)avail, rc, (u64)start, (u64)flex, rc2);
    void *pp = K.get_proc_param ? K.get_proc_param() : NULL;
    logf("proc param %p", pp);
}

static void probe_tls(void)
{
    u64 tcb = read_fs(0);
    logf("tls: fs:0=%lx fs:-8=%lx fs:-16=%lx", tcb, read_fs(-8), read_fs(-16));
}

__attribute__((unused)) static int probe_jit(void)
{
    void *va = NULL;
    int rc = K.reserve_va(&va, 0x8000000, 0, 0x200000);
    logf("reserve 128M: rc=%x va=%p", rc, va);
    struct jit_block b;
    u64 want = rc == 0 ? (u64)va + 0x400000 : 0;
    if (jit_block_create(&b, 0x100000, want) != 0) {
        logf("jit block create failed");
        return -1;
    }
    logf("jit block: rw(compiler)=%lx rx=%lx size=%lx fds %d/%d/%d", b.rw, b.rx, b.size, b.base_fd,
         b.rw_fd, b.rx_fd);
    static const u8 code[] = {0x48, 0xB8, 0xEF, 0xBE, 0xAD, 0xDE, 0x0D, 0xF0, 0xAD, 0x0B, 0xC3};
    jit_block_write(&b, 0x80, code, sizeof(code));
    u64 (*fn)(void) = (u64(*)(void))(b.rx + 0x80);
    u64 v = fn();
    logf("jit exec: %lx (want badf00ddeadbeef)", v);
    /* data right after the RX block, at a fixed offset like a game image */
    if (want) {
        void *data = (void *)(want + b.size);
        int mrc = K.map_flex(&data, 0x100000, PROT_READ | PROT_WRITE, MAP_FIXED);
        logf("flex RW fixed after block: rc=%x addr=%p", mrc, data);
        if (mrc == 0) {
            *(volatile u64 *)data = 0x1234;
            logf("flex RW write ok %lx", *(volatile u64 *)data);
        }
    }
    return v == 0x0badf00ddeadbeefULL ? 0 : -1;
}

struct vq_info {
    u64 start, end, offset;
    s32 prot, type;
    u8 flags;
    char name[32];
} __attribute__((packed));

static void walk_map(const char *who, u64 from, u64 to, bool compiler)
{
    u64 addr = from;
    struct vq_info *info = (struct vq_info *)(g_ctx->oob_scratch + 0x7E000);
    for (int i = 0; i < 64 && addr < to; i++) {
        memset(info, 0, sizeof(*info));
        int rc;
        if (compiler)
            rc = (int)jit_call(g_ctx->reserved[0], 0, addr, 1, (u64)info, 0x48, 0, 0);
        else
            rc = K.virtual_query((void *)addr, 1, info, 0x48);
        if (rc != 0) {
            logf("%s map: query %lx rc=%x", who, addr, rc);
            break;
        }
        logf("%s map: %lx-%lx (%lx) prot %x type %d flags %x off %lx %s", who, info->start, info->end,
             info->end - info->start, info->prot, info->type, info->flags, info->offset, info->name);
        if (info->end <= addr)
            break;
        addr = info->end;
    }
}

int ghost_probe(void)
{
    logf("probe: start");
    probe_modules();
    probe_memory();
    probe_tls();
    u64 bridge = g_ctx->reserved[1], ipu = g_ctx->reserved[2];
    {
        int (*qmp)(void *, void **, void **, int *) = host_dlsym((u32)g_ctx->libkernel_handle,
                                                                  "sceKernelQueryMemoryProtection");
        u64 last_end = 0;
        for (u64 a = 0x8F0000000; qmp && a < 0x920000000; a += 0x100000) {
            if (a < last_end)
                continue;
            void *st = 0, *en = 0;
            int prot = -1;
            int rc = qmp((void *)a, &st, &en, &prot);
            if (rc == 0) {
                logf("qmp %lx: %p-%p prot %x", a, st, en, prot);
                last_end = (u64)en;
            }
        }
    }
    logf("bridge %lx ipu_prog %lx staging %lx oob %lx", bridge, ipu, g_ctx->staging, g_ctx->oob_scratch);
    if (bridge) {
        walk_map("main", bridge - 0x1000000, ipu + 0x10000000, false);
        if (g_ctx->reserved[0])
            walk_map("jit", bridge - 0x1000000, ipu + 0x10000000, true);
    }
    {
        u32 lk = (u32)g_ctx->libkernel_handle;
        size_t flex = 0;
        K.avail_flex(&flex);
        logf("main flex avail %lx", (u64)flex);
        int (*sysflex)(void **, size_t, int, int, const char *) =
            host_dlsym(lk, "sceKernelMapNamedSystemFlexibleMemory");
        u64 at = 0x910000000, got = 0;
        int rc = 0;
        for (u64 chunk = 0x400000; got < 0x8000000; got += chunk) {
            void *va = (void *)(at + got);
            rc = sysflex ? sysflex(&va, chunk, PROT_READ | PROT_WRITE, MAP_FIXED, "ghostsys") : -1;
            if (rc != 0)
                break;
            memset(va, 0x5a, chunk);
        }
        logf("system flex fixed at %lx: got %lu MiB before rc=%x", at, got >> 20, rc);
        int (*avail_sys)(size_t *) = host_dlsym(lk, "sceKernelAvailableFlexibleMemorySize");
        (void)avail_sys;
        /* memory pool: direct memory committed at a reserved address */
        int (*pexpand)(off_t, off_t, size_t, size_t, off_t *) = host_dlsym(lk, "sceKernelMemoryPoolExpand");
        int (*preserve)(void *, size_t, size_t, int, void **) = host_dlsym(lk, "sceKernelMemoryPoolReserve");
        int (*pcommit)(void *, size_t, int, int, int) = host_dlsym(lk, "sceKernelMemoryPoolCommit");
        logf("pool fns %p %p %p", pexpand, preserve, pcommit);
        if (pexpand && preserve && pcommit) {
            off_t phys = 0;
            rc = pexpand(0, (off_t)K.direct_size(), 0x400000, 0x10000, &phys);
            logf("pool expand -> %x phys %lx", rc, (u64)phys);
            void *pva = 0;
            rc = preserve((void *)0x90a000000, 0x400000, 0, 0x10, &pva);
            logf("pool reserve fixed -> %x at %p", rc, pva);
            if (rc == 0) {
                rc = pcommit(pva, 0x400000, 0, PROT_READ | PROT_WRITE, 0);
                logf("pool commit -> %x", rc);
                if (rc == 0) {
                    *(volatile u64 *)pva = 0xabcdef;
                    logf("pool RW ok %lx", *(volatile u64 *)pva);
                }
            }
        }
        K.avail_flex(&flex);
        logf("main flex avail after %lx", (u64)flex);
    }
    int jit = 0;
    logf("probe: done jit=%d", jit);
    return jit;
}

#else
int ghost_probe(void)
{
    logf("probe: not built in (make -C ghostldr/native PROBE=1)");
    return -1;
}
#endif
