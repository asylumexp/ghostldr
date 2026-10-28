/* ghostldr entry point.  Lua calls ghost_entry(ctx) through call_rop. */
#include "ghost_api.h"

extern char __image_start[] HIDDEN;
extern char __rela_start[] HIDDEN;
extern char __rela_end[] HIDDEN;

int ghost_probe(void);
int ghost_boot(void);

#define R_X86_64_RELATIVE 8

static void self_relocate(void)
{
    u64 base = (u64)__image_start;
    struct { u64 off, info; s64 addend; } *r = (void *)__rela_start, *end = (void *)__rela_end;
    for (; r < end; r++)
        if ((u32)r->info == R_X86_64_RELATIVE)
            *(u64 *)(base + r->off) = base + r->addend;
}

static int g_relocated;

__attribute__((section(".text.entry"), force_align_arg_pointer, used)) u64 ghost_entry(struct boot_ctx *ctx)
{
    if (!ctx || ctx->magic != BOOT_MAGIC)
        return 0xBAD0;
    if (!g_relocated) {
        self_relocate();
        g_relocated = 1;
    }
    if (host_init(ctx) > 8)
        return 0xBAD1;
    logf("ghostldr core at %p, ctx flags %lx", __image_start, ctx->flags);
    if (ctx->flags & CTX_FLAG_PS4)
        logf("ghostldr: on a PS4, firmware %lx.%02lx: experimental, see docs/ps4.md",
             ctx->size >= __builtin_offsetof(struct boot_ctx, fw) + 8 ? ctx->fw >> 8 : 0,
             ctx->size >= __builtin_offsetof(struct boot_ctx, fw) + 8 ? ctx->fw & 0xff : 0);
    if (ctx->flags & CTX_FLAG_DIAG)
        return (u64)ghost_diag();
    if (ctx->flags & CTX_FLAG_ELFLDR) { /* beside the selector, its root can fail as a boot's does */
        int rc = ghost_elfldr();
        return (u64)(rc == -6 || rc == -8 ? fail_result(rc) : rc);
    }
    if (ctx->flags & 1)
        return (u64)ghost_probe();
    return (u64)fail_result(ghost_boot());
}
