/*
 * Large executables: the code map of a module's executable segment and the
 * JIT objects that cover it: whole emulator buffers, or fresh objects sized
 * to the code once the buffers are reclaimed (bxplan.c; no console calls).
 * bigexec.c uses it on the console; tools/exec_fit.py is its twin for the
 * fit check before an install, and tests/test_bigexec.py runs both on the
 * same inputs.  docs/compatibility.md, "Large executables".
 */
#pragma once
#include "ghost.h"

#define BX_CHUNK 0x10000UL           /* code evidence is kept per 64 KiB */
#define BX_JOIN 0x100000UL           /* code closer than this is one run */
#define BX_PAD 0x40000UL             /* margin on both sides of a run */
#define BX_MAX_RUNS 6
#define BX_MAX_OBJS 5                /* emulator buffers bx_cover tries */
#define BX_MAX_VIEWS 6               /* views of one module: a buffer cover's, or a fresh object per run */
#define BX_MAX_FORBID 8
#define BX_TEXT_MAX (512UL << 20)    /* larger executable segments are not mapped */

struct bx_code_in {
    const u8 *seg;                   /* the executable segment's file bytes */
    u64 seg_vaddr, seg_filesz;
    u64 text_size;                   /* its end in memory, 16 KiB aligned (struct module text_size) */
    u64 eh_hdr;                      /* .eh_frame_hdr (PT_GNU_EH_FRAME), 0: none */
    u64 point[3];                    /* entry, DT_INIT, DT_FINI */
    u32 npoints;
    u64 got_lo, got_hi;              /* the PLT relocations' GOT slots [lo, hi); equal: none */
    const u32 *exec;                 /* a profile's code ranges (start, end pairs) instead of evidence */
    u32 nexec;
};

struct bx_code_stats {
    u32 fdes, prologues, plt, points;
    u64 fde_bytes;
    bool eh;                         /* .eh_frame_hdr was usable */
};

/* The code runs, sorted, at most BX_MAX_RUNS; 0: none (or the segment is
 * larger than BX_TEXT_MAX). */
u32 bx_code_runs(const struct bx_code_in *in, struct bx_range *runs, struct bx_code_stats *st);

struct bx_view {
    u32 obj;                         /* index into the objects given to bx_cover */
    s64 off;                         /* module offset of the object's first byte */
};

struct bx_layout {
    u32 nviews;
    struct bx_view view[BX_MAX_VIEWS]; /* by offset; neighbours touch where code continues */
    u64 used;                         /* bytes of objects used */
};

/* Whole objects (sizes, largest first) at 16 KiB-aligned offsets covering
 * every run, none over a forbidden range (the data segments, sorted), the
 * largest object left holding at least `keep` bytes.  0, or -1: no layout. */
int bx_cover(const struct bx_range *runs, u32 nruns, const struct bx_range *forbid, u32 nforbid, const u64 *obj,
             u32 nobj, u64 keep, struct bx_layout *out);

/* Fresh objects instead (after a reclaim; ext/src/reclaim.c): one per run,
 * of the run's own size, at its offset, none over a data segment.  size[i]
 * is view i's object's size.  0, or -1: a run overlaps a data segment.
 * Inline: the core itself never uses it (its image must stay small). */
static inline int bx_fresh(const struct bx_range *runs, u32 nruns, const struct bx_range *forbid, u32 nforbid, u64 *size,
                           struct bx_layout *out)
{
    out->nviews = 0, out->used = 0;
    if (!nruns || nruns > BX_MAX_VIEWS || nforbid > BX_MAX_FORBID)
        return -1;
    for (u32 i = 0; i < nruns; i++) {
        s64 lo = (s64)ALIGN_DOWN((u64)runs[i].lo, 0x4000), hi = (s64)ALIGN_UP((u64)runs[i].hi, 0x4000);
        for (u32 f = 0; f < nforbid; f++)
            if (forbid[f].lo < hi && forbid[f].hi > lo)
                return -1;
        size[i] = (u64)(hi - lo);
        out->view[out->nviews++] = (struct bx_view){i, lo};
        out->used += size[i];
    }
    return 0;
}

/* modules as elf.c parsed them (their files still loaded) */
bool bx_arena_holds(const struct module *mods, u64 lo, u64 hi); /* boot.c's arena placement works */
struct module *bx_pick(struct module *mods, u64 *keep);           /* who gets objects; the arena's need */
void bx_module_in(const struct module *m, const u32 *exec, u32 nexec, struct bx_code_in *in);
u32 bx_data_ranges(const struct module *m, struct bx_range *r);   /* at most BX_MAX_FORBID */
