/*
 * Large executables, the plan (bxplan.h): which parts of a module's
 * executable segment are code, and where whole emulator JIT objects go so
 * that every part of it is executable.  No console calls; bigexec.c maps
 * what this decides.  tools/exec_fit.py is a line-by-line twin (the fit check
 * before an install): change both together.
 *
 * Code map.  Evidence is marked per 64 KiB chunk of the segment:
 *   - every FDE in .eh_frame, through .eh_frame_hdr's sorted table;
 *   - frame-pointer prologues (push rbp; mov rbp, rsp) at 16-byte boundaries;
 *   - the entry point, DT_INIT and DT_FINI;
 *   - PLT entries: jmp [rip+x] into the GOT slots of the PLT relocations.
 * Marked chunks less than 1 MiB apart form one run, at most 6 runs stay (the
 * closest are joined), and each gets 256 KiB of margin on both sides: small
 * functions without unwind information sit between and next to the others.
 * A profile can name the ranges instead ("exec", REC_EXEC).
 *
 * Cover.  An object is mapped whole (a view of part of one is refused) at a
 * 16 KiB-aligned module offset; objects next to each other make one longer
 * executable run.  Every split of the runs into consecutive groups and every
 * assignment of the objects to groups is tried.  A group's objects lie back
 * to back in the given order, starting at the lowest offset that still covers
 * the group (at offset 0 or above when that works too), after the previous
 * group's objects, never over a data segment.  The fewest bytes of objects
 * win, then the fewest objects, then the earliest objects (the lowest mask of
 * object indices), then the first found; the largest object left over must
 * hold `keep` bytes (the modules that stay in the arena).
 *
 * Fresh objects (bxplan.h bx_fresh), when no cover works and the emulator's
 * buffers are reclaimed (ext/src/reclaim.c): any 16 KiB multiple can be
 * created then, so each run gets an object of its own size at its own
 * offset.  That is the fewest bytes there are; the runs are already 1 MiB or
 * more apart.
 */
#include "bxplan.h"

#define COLD GHOST_COLD

static u32 ld32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u64 ld64(const u8 *p) { return ld32(p) | (u64)ld32(p + 4) << 32; }

/* the segment's bytes at [v, v + n), or NULL */
COLD static const u8 *seg_at(const struct bx_code_in *in, u64 v, u64 n)
{
    if (v < in->seg_vaddr || n > in->seg_filesz || v - in->seg_vaddr > in->seg_filesz - n)
        return NULL;
    return in->seg + (v - in->seg_vaddr);
}

/* bytes of a DW_EH_PE value of this format (low nibble); 0: unsupported */
COLD static u32 enc_size(u32 enc)
{
    switch (enc & 0x0F) {
    case 0x0: case 0x4: case 0xC: return 8;
    case 0x3: case 0xB: return 4;
    case 0x2: case 0xA: return 2;
    }
    return 0;
}

COLD static u64 ld_n(const u8 *p, u32 n) { return n == 8 ? ld64(p) : n == 4 ? ld32(p) : (u64)(p[0] | p[1] << 8); }

COLD static const u8 *skip_leb(const u8 *p, const u8 *end)
{
    while (p < end && (*p++ & 0x80))
        ;
    return p;
}

/* the FDE pointer encoding a CIE gives its FDEs; -1: unreadable */
COLD static int cie_enc(const struct bx_code_in *in, u64 cie)
{
    const u8 *h = seg_at(in, cie, 9);
    u32 len = h ? ld32(h) : 0;
    if (!h || len < 5 || len == 0xFFFFFFFFu || !seg_at(in, cie, (u64)len + 4) || ld32(h + 4) != 0)
        return -1;
    const u8 *end = h + 4 + len, *aug = h + 9, *p = aug;
    while (p < end && *p)
        p++;
    if (p++ >= end)
        return -1;
    if (aug[0] != 'z')
        return 0;
    p = skip_leb(p, end); /* code alignment */
    p = skip_leb(p, end); /* data alignment */
    p = h[8] == 1 ? p + 1 : skip_leb(p, end); /* return address register */
    p = skip_leb(p, end); /* augmentation data length */
    for (const u8 *a = aug + 1; *a; a++) {
        if (p >= end)
            return -1;
        if (*a == 'R')
            return *p;
        if (*a == 'L') {
            p++;
        } else if (*a == 'P') {
            u32 n = enc_size(*p);
            if (!n || (*p & 0x70) == 0x50) /* no size, or aligned */
                return -1;
            p += 1 + n;
        } else if (*a != 'S' && *a != 'B') {
            return -1;
        }
    }
    return 0;
}

COLD static void mark(u8 *map, u64 text, u64 lo, u64 hi)
{
    if (lo >= text || hi <= lo)
        return;
    if (hi > text)
        hi = text;
    for (u64 c = lo / BX_CHUNK; c <= (hi - 1) / BX_CHUNK; c++)
        map[c >> 3] |= (u8)(1 << (c & 7));
}

COLD static void mark_fdes(const struct bx_code_in *in, u8 *map, struct bx_code_stats *st)
{
    const u8 *h = in->eh_hdr ? seg_at(in, in->eh_hdr, 4) : NULL;
    /* version 1, a table of datarel sdata4 pairs, a 4-byte count */
    u32 en = h ? enc_size(h[1]) : 0;
    if (!h || h[0] != 1 || h[3] != 0x3B || !en || enc_size(h[2]) != 4 || !seg_at(in, in->eh_hdr + 4 + en, 4))
        return;
    u64 count = ld32(h + 4 + en), tab = in->eh_hdr + 8 + en;
    if (!seg_at(in, tab, count * 8))
        return;
    st->eh = true;
    u64 last = ~0UL;
    int enc = -1;
    for (u64 i = 0; i < count; i++) {
        const u8 *e = seg_at(in, tab + 8 * i, 8);
        u64 loc = in->eh_hdr + (u64)(s64)(s32)ld32(e), fde = in->eh_hdr + (u64)(s64)(s32)ld32(e + 4);
        const u8 *f = seg_at(in, fde, 8);
        if (!f)
            continue;
        u64 cie = fde + 4 - ld32(f + 4);
        if (cie != last)
            last = cie, enc = cie_enc(in, cie);
        u32 n = enc >= 0 ? enc_size((u32)enc) : 0;
        const u8 *r = n ? seg_at(in, fde + 8 + n, n) : NULL; /* pc_range follows pc_begin */
        u64 range = r ? ld_n(r, n) : 0;
        if (!range)
            continue;
        st->fdes++, st->fde_bytes += range;
        mark(map, in->text_size, loc, loc + range);
    }
}

/* join the two runs with the smallest gap (the first such pair) */
COLD static void join_closest(struct bx_range *r, u32 *n)
{
    u32 best = 1;
    for (u32 i = 2; i < *n; i++)
        if (r[i].lo - r[i - 1].hi < r[best].lo - r[best - 1].hi)
            best = i;
    r[best - 1].hi = r[best].hi;
    for (u32 i = best; i + 1 < *n; i++)
        r[i] = r[i + 1];
    (*n)--;
}

#define TMP_RUNS 32

COLD u32 bx_code_runs(const struct bx_code_in *in, struct bx_range *runs, struct bx_code_stats *st)
{
    u8 map[BX_TEXT_MAX / BX_CHUNK / 8]; /* 1 KiB */
    struct bx_range tmp[TMP_RUNS];
    u64 text = in->text_size;
    u32 n = 0, k = 0;
    memset(st, 0, sizeof(*st));
    if (!text || text > BX_TEXT_MAX)
        return 0;
    if (in->nexec) { /* the profile's ranges, as they are */
        for (u32 i = 0; i < in->nexec; i++) {
            s64 lo = (s64)ALIGN_DOWN((u64)in->exec[2 * i], 0x4000), hi = (s64)ALIGN_UP((u64)in->exec[2 * i + 1], 0x4000);
            if (hi > (s64)text)
                hi = (s64)text;
            if (lo >= hi)
                continue;
            if (n && lo <= tmp[n - 1].hi) {
                if (hi > tmp[n - 1].hi)
                    tmp[n - 1].hi = hi;
                continue;
            }
            if (n == TMP_RUNS)
                join_closest(tmp, &n);
            tmp[n++] = (struct bx_range){lo, hi};
        }
        while (n > BX_MAX_RUNS)
            join_closest(tmp, &n);
        memcpy(runs, tmp, n * sizeof(*tmp));
        return n;
    }
    u64 nchunks = (text + BX_CHUNK - 1) / BX_CHUNK;
    memset(map, 0, (nchunks + 7) / 8);
    mark_fdes(in, map, st);
    for (u64 v = ALIGN_UP(in->seg_vaddr, 16); v + 4 <= in->seg_vaddr + in->seg_filesz; v += 16)
        if (ld32(in->seg + (v - in->seg_vaddr)) == 0xE5894855u)
            st->prologues++, mark(map, text, v, v + 1);
    for (u64 i = 0; in->got_hi > in->got_lo && i + 6 <= in->seg_filesz; i++)
        if (in->seg[i + 1] == 0x25 && in->seg[i] == 0xFF) {
            u64 t = in->seg_vaddr + i + 6 + (u64)(s64)(s32)ld32(in->seg + i + 2);
            if (t >= in->got_lo && t < in->got_hi)
                st->plt++, mark(map, text, in->seg_vaddr + i, in->seg_vaddr + i + 6);
        }
    for (u32 i = 0; i < in->npoints; i++)
        st->points++, mark(map, text, in->point[i], in->point[i] + 1);
    for (u64 c = 0; c < nchunks; c++) {
        if (!(map[c >> 3] >> (c & 7) & 1))
            continue;
        s64 lo = (s64)(c * BX_CHUNK), hi = lo + (s64)BX_CHUNK;
        if (n && lo - tmp[n - 1].hi < (s64)BX_JOIN) {
            tmp[n - 1].hi = hi;
            continue;
        }
        if (n == TMP_RUNS)
            join_closest(tmp, &n);
        tmp[n++] = (struct bx_range){lo, hi};
    }
    while (n > BX_MAX_RUNS)
        join_closest(tmp, &n);
    for (u32 i = 0; i < n; i++) {
        s64 lo = tmp[i].lo - (s64)BX_PAD, hi = tmp[i].hi + (s64)BX_PAD;
        if (lo < 0)
            lo = 0;
        if (hi > (s64)text)
            hi = (s64)text;
        if (k && lo <= runs[k - 1].hi)
            runs[k - 1].hi = hi;
        else
            runs[k++] = (struct bx_range){lo, hi};
    }
    return k;
}

COLD int bx_cover(const struct bx_range *runs, u32 nruns, const struct bx_range *forbid, u32 nforbid, const u64 *obj,
                  u32 nobj, u64 keep, struct bx_layout *out)
{
    u64 best_used = ~0UL;
    u32 best_n = ~0u, best_mask = ~0u;
    if (!nruns || nruns > BX_MAX_RUNS || nobj > BX_MAX_OBJS || nforbid > BX_MAX_FORBID)
        return -1;
    for (u32 mask = 0; mask < 1u << (nruns - 1); mask++) {
        /* bit i - 1 set: run i starts a new group */
        s64 ga[BX_MAX_RUNS], gb[BX_MAX_RUNS];
        u32 k = 0, combos = 1;
        for (u32 i = 0; i < nruns; i++) {
            if (!i || (mask >> (i - 1) & 1))
                ga[k++] = runs[i].lo;
            gb[k - 1] = runs[i].hi;
        }
        if (k > nobj)
            continue;
        for (u32 j = 0; j < nobj; j++)
            combos *= k + 1;
        /* object j goes to group d[j]; k: left over */
        for (u32 code = 0; code < combos; code++) {
            u32 d[BX_MAX_OBJS], nused = 0, umask = 0;
            u64 t[BX_MAX_RUNS] = {0}, used = 0, spare = 0;
            for (u32 j = 0, c = code; j < nobj; j++, c /= k + 1) {
                d[j] = c % (k + 1);
                if (d[j] < k)
                    t[d[j]] += obj[j], used += obj[j], nused++, umask |= 1u << j;
                else if (obj[j] > spare)
                    spare = obj[j];
            }
            if (used > best_used || (used == best_used && (nused > best_n || (nused == best_n && umask >= best_mask))) ||
                spare < keep)
                continue;
            s64 at[BX_MAX_RUNS], prev = -(s64)BX_TEXT_MAX;
            bool ok = true;
            for (u32 g = 0; g < k && ok; g++) {
                s64 lo = gb[g] - (s64)t[g], stop = (s64)(~0UL >> 1);
                ok = (s64)t[g] >= gb[g] - ga[g];
                if (lo < prev)
                    lo = prev;
                for (u32 f = 0; f < nforbid; f++) {
                    if (forbid[f].hi <= ga[g]) {
                        if (forbid[f].hi > lo)
                            lo = forbid[f].hi;
                    } else if (forbid[f].lo >= gb[g]) {
                        if (forbid[f].lo < stop)
                            stop = forbid[f].lo;
                    } else {
                        ok = false;
                    }
                }
                s64 s = lo < 0 ? 0 : lo;
                if (s > ga[g] || s + (s64)t[g] > stop)
                    s = lo;
                ok = ok && s <= ga[g] && s + (s64)t[g] <= stop;
                at[g] = s, prev = s + (s64)t[g];
            }
            if (!ok)
                continue;
            best_used = used, best_n = nused, best_mask = umask;
            out->nviews = 0, out->used = used;
            for (u32 g = 0; g < k; g++)
                for (u32 j = 0; j < nobj; j++)
                    if (d[j] == g)
                        out->view[out->nviews++] = (struct bx_view){j, at[g]}, at[g] += (s64)obj[j];
        }
    }
    return best_n == ~0u ? -1 : 0;
}

/* ---- modules as elf.c parsed them ---- */

/* boot.c's rule: the text and RELRO of a module placed at `at` end by hi */
COLD static bool fits_at(const struct module *m, u64 at, u64 hi)
{
    if (at + m->text_size > hi)
        return false;
    for (u32 i = 0; i < m->phnum; i++)
        if (m->phdrs[i].type == PT_SCE_RELRO && at + ALIGN_UP(m->phdrs[i].vaddr + m->phdrs[i].memsz, 0x4000) > hi)
            return false;
    return true;
}

/* boot.c's placement in the code region [lo, hi): the other modules in load
 * order, each at the next 64 KiB boundary after the previous one's image,
 * then the executable */
COLD bool bx_arena_holds(const struct module *mods, u64 lo, u64 hi)
{
    u64 at = ALIGN_UP(lo, 0x10000);
    for (u32 pass = 0; pass < 2; pass++)
        for (const struct module *m = mods; m; m = m->next) {
            if (m->is_main != (pass == 1))
                continue;
            if (!fits_at(m, at, hi))
                return false;
            at = ALIGN_UP(at + m->image_size, 0x10000);
        }
    return true;
}

/* the module that gets JIT objects of its own: the most text (ties: the
 * executable); *keep: what the others need in the arena (its header and
 * 64 KiB alignment, their whole images) */
COLD struct module *bx_pick(struct module *mods, u64 *keep)
{
    struct module *big = NULL;
    for (struct module *m = mods; m; m = m->next)
        if (!big || m->text_size > big->text_size || (m->text_size == big->text_size && m->is_main))
            big = m;
    *keep = 0x10000;
    for (struct module *m = mods; m; m = m->next)
        if (m != big)
            *keep += ALIGN_UP(m->image_size, 0x10000);
    return big;
}

/* the code map's input for a module whose file is still loaded */
COLD void bx_module_in(const struct module *m, const u32 *exec, u32 nexec, struct bx_code_in *in)
{
    memset(in, 0, sizeof(*in));
    for (u32 i = 0; i < m->phnum; i++)
        if (m->phdrs[i].type == PT_LOAD && (m->phdrs[i].flags & 1) && m->seg_data[i])
            in->seg = m->seg_data[i], in->seg_vaddr = m->phdrs[i].vaddr, in->seg_filesz = m->phdrs[i].filesz;
    in->text_size = m->text_size;
    in->eh_hdr = m->eh_frame_hdr;
    in->point[in->npoints++] = m->entry;
    if (m->has_init)
        in->point[in->npoints++] = m->init;
    if (m->fini)
        in->point[in->npoints++] = m->fini;
    for (u64 i = 0; i < m->njmprel; i++) {
        u64 o = m->jmprel[i].offset;
        if (!in->got_hi || o < in->got_lo)
            in->got_lo = o;
        if (o + 8 > in->got_hi)
            in->got_hi = o + 8;
    }
    in->exec = exec, in->nexec = nexec;
}

/* the data segments and RELRO: views stay off them; page-aligned, sorted, merged */
COLD u32 bx_data_ranges(const struct module *m, struct bx_range *r)
{
    u32 n = 0, k = 0;
    for (u32 i = 0; i < m->phnum && n < BX_MAX_FORBID; i++) {
        const struct elf_phdr *p = &m->phdrs[i];
        if (!((p->type == PT_LOAD && !(p->flags & 1)) || p->type == PT_SCE_RELRO) || !p->memsz)
            continue;
        struct bx_range x = {(s64)ALIGN_DOWN(p->vaddr, 0x4000), (s64)ALIGN_UP(p->vaddr + p->memsz, 0x4000)};
        u32 at = n++;
        for (; at && r[at - 1].lo > x.lo; at--)
            r[at] = r[at - 1];
        r[at] = x;
    }
    for (u32 i = 0; i < n; i++) {
        if (k && r[i].lo <= r[k - 1].hi) {
            if (r[i].hi > r[k - 1].hi)
                r[k - 1].hi = r[i].hi;
        } else {
            r[k++] = r[i];
        }
    }
    return k;
}
