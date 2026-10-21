/*
 * Per-library modes for system libraries SWRR may or may not let the title
 * use (hle_mode.c; used by hle_media.c and hle_peripheral.c).
 *
 * Each library runs in one mode, chosen once and logged:
 *   PASS    the host library does the work; the extension only forwards.
 *   MASKED  the host library is alive but SWRR owns its instance: the title's
 *           init answers 0 and its Terminate/Close is not forwarded.
 *   VIRT    the extension answers every call (handles, no-op I/O).
 * A library starts UNDECIDED (or VIRT, for libraries that never pass) and
 * goes VIRT when its sysmodule fails to load, when one of the title's imports
 * of it would otherwise get the logging stub (a function the host lacks), or
 * when its root call (Init/Open) is refused with a deny code.  Errors that
 * describe the device (camera not connected, headset disconnected) reach the
 * title unchanged.
 *
 * Most functions go through a trampoline (16 bytes each, emitted from the
 * family's route list) into hle_gen_entry: it saves the argument registers,
 * asks hle_route where the call goes, and tail-jumps there with the
 * registers and the stack as the title left them.  So the host function or
 * the virtual body gets every argument, float and stack ones included.
 * Root calls, whose result decides the mode, are ordinary C wrappers.
 */
#pragma once
#include "ext.h"

/* Crash-class calls stay virtual until a hardware probe clears them
 * (research spec §25); set to 1 once a probe has. */
#ifndef HLE_PROBED_CAMERA
#define HLE_PROBED_CAMERA 0 /* sceCameraOpen/Start: system flex memory (probe 6) */
#endif
#ifndef HLE_PROBED_HMD_REPROJECTION
#define HLE_PROBED_HMD_REPROJECTION 0 /* sceHmdReprojection*: a GPU thread with no headset (probe 8) */
#endif
#ifndef HLE_PROBED_VR_TRACKER
#define HLE_PROBED_VR_TRACKER 0 /* sceVrTrackerInit and GPU calls (probe 9) */
#endif
#ifndef HLE_VOICE_PASS
#define HLE_VOICE_PASS 0 /* libSceVoice: 1 lets the host library run (probe 10) */
#endif

#define EUNAVAIL ((s32)0x8002002D) /* SCE_KERNEL_ERROR_EOPNOTSUPP: a getter with no safe answer */

enum { HM_UNDECIDED, HM_PASS, HM_MASKED, HM_VIRT };

#define HL_MASK_ANY 1 /* SWRR initialised it: any refusal from the root call masks */

struct hle_lib {
    const struct ext_lib *pol; /* import library name and aliases (the family's policy) */
    const char *module;        /* host module; NULL: the library's name */
    u32 sysmod[2];             /* its sysmodule ids (0: none) */
    u8 init_mode;              /* HM_VIRT: never asks the host */
    u8 flags;                  /* HL_* */
    u8 probed;                 /* its crash-class calls (RF_CRASH) may reach the host */
    const char *why;           /* why it starts virtual */
    const s32 *already;        /* root results that mean "SWRR initialised it": MASKED, 0 */
    const s32 *deny;           /* root results that refuse the title: VIRT (DENY_COMMON too) */
    const s32 *legit;          /* root results that describe the device: to the title */
    /* state */
    const char *tag;
    volatile u8 mode, owned, ready;
};

/* how a routed function is answered */
#define RF_VIRT 0x01     /* never the host: UI launchers, system-wide calls, SWRR's Terminate */
#define RF_MASKSTUB 0x02 /* answers 0 while the library is MASKED: SWRR's Terminate/End */
#define RF_OWNED 0x04    /* the host only for the title's own session (hle_lib.owned) */
#define RF_READY 0x08    /* the host only while the device reports READY (hle_lib.ready) */
#define RF_CRASH 0x10    /* crash class: virtual until hle_lib.probed */
#define RF_NOGFS 0x20    /* reads a /app0 path in a system process: virtual under ghostfs */
#define RF_KEEPRP 0x40   /* virtual with ctx flag CTX_FLAG_KEEP_REMOTE_PLAY */
#define RF_PURE 0x80     /* a pure function: the host whenever it has it, whatever the mode */
#define RF_LOG 0x100     /* log each virtual call (UI launchers) */

struct hle_route {
    void *virt; /* the virtual body (it gets the call's arguments); NULL: return ret */
    s32 ret;
    u16 flags;
    u8 lib;
};

struct hle_router {
    const char *tag; /* log prefix, e.g. "hle_media" */
    struct hle_lib *libs;
    u32 nlibs;
    const struct ext_hle_ent *hle; /* the family's table; routes[i] goes with hle[first + i] */
    u32 nhle, first;
    const struct hle_route *routes;
    const char *tramps, *tramps_end; /* 16 bytes per route */
    void **cache;                    /* the host function of each route */
};

/* Route lists: X(lib, name, flags, ret, virt) rows.  A family defines
 *   #define N_<lib> "libSceFoo" and enum { K_<lib>, ... }
 * and expands its list with these. */
#define HLE_ENUM_ROW(lib, name, flags, ret, virt) R_##name,
#define HLE_TRAMP_ROW(lib, name, flags, ret, virt) "  .p2align 4\n1: leaq 1b(%rip), %r11\n  jmp hle_gen_entry\n"
#define HLE_ROUTE_ROW(lib, name, flags, ret, virt) {(void *)(virt), (s32)(ret), (flags), K_##lib},
#define HLE_TRAMPOLINES(sym, list)                                                                \
    __asm__(".text\n  .p2align 4\n  .globl " #sym "\n  .hidden " #sym "\n" #sym ":\n" list(HLE_TRAMP_ROW) \
            "  .p2align 4\n  .globl " #sym "_end\n  .hidden " #sym "_end\n" #sym "_end:\n")

/* checks the router, sets the starting modes, then looks at the title's
 * imports of its libraries (the host lacks one: that library is virtual) */
int hle_router_init(struct hle_router *r);
/* a sysmodule load the title asked for failed (EXT_FAMILY_HOOKS) */
void hle_router_sysmodule(struct hle_router *r, u32 id, s32 rc);

/* Settles a library's mode: from UNDECIDED only, except that VIRT may
 * follow PASS or MASKED (a refused root call).  Logged. */
void hle_set(struct hle_lib *L, u8 mode, const char *fn, s32 rc, const char *why);
/* The host's function, cached; NULL when the host lacks it (an undecided
 * library goes VIRT). */
void *hle_host(struct hle_lib *L, const char *name, void **cache);
/* A root call (Init/Open): forwards a[] to the host and settles the mode from
 * the result.  true: the host answered (*rc goes to the title); false: the
 * virtual body answers. */
bool hle_root(struct hle_lib *L, const char *name, void **cache, const u64 a[6], s32 *rc);
/* A setter: the host's result with the library's deny codes turned into 0;
 * virt_ret when the library is virtual. */
s32 hle_setter(struct hle_lib *L, const char *name, void **cache, const u64 a[6], s32 virt_ret);
/* A path the title passes to a system process: NULL when it is in ghostfs
 * (no system process can read it), else the path with /app0 remapped. */
const char *hle_sys_path(const char *path, char *buf, size_t size);
bool hle_in(const s32 *set, s32 v);
u64 hle_time_us(void); /* sceKernelGetProcessTime */

typedef s64 (*hle_fn6)(u64, u64, u64, u64, u64, u64);
#define HLE_CALL6(fn, a) ((hle_fn6)(void *)(fn))((a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5])

/* A root wrapper: fn's host function decides the mode; virt(args) answers
 * when the library is virtual. */
#define HLE_ROOT(fn, lib, virt)                                                                   \
    static s64 w_##fn(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f)                                  \
    {                                                                                             \
        static void *cache;                                                                       \
        const u64 v[6] = {a, b, c, d, e, f};                                                      \
        s32 rc;                                                                                   \
        return hle_root(lib, #fn, &cache, v, &rc) ? rc : HLE_CALL6(virt, v);                      \
    }
/* A setter wrapper (hle_setter) */
#define HLE_SETTER(fn, lib, virt_ret)                                                             \
    static s64 w_##fn(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f)                                  \
    {                                                                                             \
        static void *cache;                                                                       \
        const u64 v[6] = {a, b, c, d, e, f};                                                      \
        return hle_setter(lib, #fn, &cache, v, virt_ret);                                         \
    }

/* constant answers for libraries that never pass (shared by entries) */
#define HLE_RET_FN(name, val)                                                                     \
    static s64 name(void) { return (s32)(val); }
