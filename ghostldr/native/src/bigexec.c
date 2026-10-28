/*
 * Large executables: a module whose code the arena (arena.c) cannot hold,
 * such as an executable segment larger than the emulator's largest JIT
 * buffer, runs from several of the emulator's own JIT objects, each mapped
 * whole as a read-execute view at the module's code addresses, back to back
 * where the code continues.  The rest of its executable segment (read-only
 * data, unwind tables) and its data segments are direct memory next to the
 * views.  bxplan.c decides the layout; docs/compatibility.md, "Large
 * executables".  Only when the modules do not fit the arena as they are, and
 * never on a PS4.
 *
 * What the console allows (PS5 13.60, measured):
 *  - new JIT memory is nearly used up (768 KiB), but views of the emulator's
 *    objects cost none of it;
 *  - a view is a whole object at file offset 0 (any offset or shorter length
 *    is refused); it maps MAP_FIXED at any address inside a range reserved
 *    first, also above 0x10_0000_0000, where fixed direct memory maps too
 *    (fixed direct maps are refused lower down);
 *  - each object is reachable through two compiler descriptors; only its
 *    base descriptor takes a read-write alias, and that alias, mapped in the
 *    compiler, lands at the object's home address and makes the whole object
 *    writable there, its first 16 KiB included;
 *  - never sceKernelJitMapSharedMemory on an existing object in this process:
 *    it maps it back at its home address over whatever is there (on the IPU
 *    object, the core's data).
 *
 * Only the views are executable: the direct memory next to them is mapped
 * read-write, then read-only, and never asked for execute permission (a
 * console with a kernel patch may grant that where a stock one would not).
 * The steps are separate: the code map (bxplan.c bx_code_runs), the backing
 * (which objects go where), then mapping and writing.  The backing is whole
 * emulator buffers when they cover the code (bx_cover, find_objects), else
 * the reclaim: every reference to the emulator's buffers is dropped, which
 * gives their JIT memory back (the console's 65 MiB cap is global, and they
 * hold all of it but 768 KiB), and fresh objects are made, one per code run
 * (bx_fresh), plus one for the modules that stay in the arena, mapped where
 * the largest buffer was.  The extension does that part (ext/src/reclaim.c,
 * through ghost_api.h struct bx_reclaim), to keep the core small; it
 * measures what is free before and after and drops nothing unless the plan
 * fits.  Buffers the ELF loader still holds are left alone and not counted.
 *
 * The objects used leave the arena (arena_drop); their home mappings stay and
 * show the same bytes.  The game selector borrowed the largest buffer's start
 * before this runs (sel.c, before ghost_boot loads the modules) and cleared
 * it again.
 */
#include "bxplan.h"
#include "ghost_api.h"

#define COLD GHOST_COLD
#define MiB (1UL << 20)
#define SYS_close 6
/* where the module's range is reserved (a hint): high in the app map area
 * (0x10_0000_0000-0xFC_0000_0000), away from the fixed addresses titles map
 * their own memory at (Ghost: 0x10, 0x11, 0x20 and 0x30_0000_0000) */
#define BX_HINT 0xE000000000UL
#define BX_HIGH 0x1000000000UL /* fixed direct memory maps only above this */
#define BX_FLEX_MAX (128UL << 20) /* system flexible memory punched in all, as loader.c's run-time limit */
/* compiler-visible out parameters (jitrpc.c's private scratch: 0x300-0x30f is free) */
#define OUT(off) (g_ctx->oob_scratch + 0x70000 + 0x300 + (off))

struct bx_state {
    struct bx_layout lay;
    struct jit_obj obj[BX_MAX_VIEWS]; /* the candidate buffers, largest first; or the fresh objects, by view */
    u32 nobj, nruns, nforbid;
    bool fresh;                       /* reclaimed: obj[] are fresh objects (home 0) */
    struct bx_range runs[BX_MAX_RUNS], forbid[BX_MAX_FORBID];
};

u32 bx_code(const struct module *m, const struct bx_range **runs)
{
    if (!m->bx)
        return 0;
    *runs = m->bx->runs;
    return m->bx->nruns;
}

COLD static void close_compiler(int fd)
{
    if (fd >= 0)
        jit_call(g_ctx->jit_syscall, SYS_close, (u64)fd, 0, 0, 0, 0, 0);
}

/* Each wanted object's base descriptor in the compiler (it takes an RW
 * alias; mapped, that lands at the object's home: which object it is), and
 * an RX alias of it here.  Only objects of a wanted size get an RW alias; one
 * that turns out not to be wanted stays mapped at its home in the compiler,
 * where the compiler maps that object anyway.  Also the ELF loader's
 * (ext/src/elfldr_sys.c). */
COLD int jit_objects_find(struct jit_obj *o, u32 n, u32 want)
{
    u32 have = 0;
    for (int fd = 0; fd < 1024 && have != want; fd++) {
        *(volatile u64 *)OUT(0) = (u64)-1;
        if ((int)jit_call(g_ctx->jit_shm_alias, 0, (u64)fd, PROT_READ | PROT_EXEC, OUT(0), 0, 0, 0) != 0)
            continue;
        int rx_c = (int)*(volatile u32 *)OUT(0), rx = jit_recv_fd(rx_c), rw_c = -1, j = -1;
        close_compiler(rx_c);
        u8 st[0x78];
        u64 size = rx >= 0 && K.fstat(rx, st) == 0 ? *(u64 *)(st + 0x48) : 0, at = 0;
        bool sized = false;
        for (u32 i = 0; i < n; i++)
            sized |= ((want & ~have) >> i & 1) && o[i].size == size;
        if (sized) {
            *(volatile u64 *)OUT(0) = (u64)-1;
            if ((int)jit_call(g_ctx->jit_shm_alias, 0, (u64)fd, PROT_READ | PROT_WRITE, OUT(0), 0, 0, 0) == 0) {
                rw_c = (int)*(volatile u32 *)OUT(0);
                *(volatile u64 *)OUT(8) = 0;
                if ((int)jit_call(g_ctx->jit_shm_map, 0, (u64)rw_c, PROT_READ | PROT_WRITE, OUT(8), 0, 0, 0) == 0)
                    at = *(volatile u64 *)OUT(8);
            }
            for (u32 i = 0; i < n; i++)
                if (((want & ~have) >> i & 1) && o[i].size == size && o[i].home == at)
                    j = (int)i;
        }
        close_compiler(rw_c); /* its mapping stays */
        if (j < 0) {
            if (rx >= 0)
                K.close(rx);
            continue;
        }
        o[j].fd = fd, o[j].rx = rx, o[j].wr = at;
        have |= 1u << j;
        logf("bigexec: compiler fd %d: the %lu MiB object at %lx (here %d)", fd, size / MiB, at, rx);
    }
    if (have != want)
        logf("bigexec: JIT objects %x of %x found", have, want);
    return have == want ? 0 : -1;
}

COLD static int find_objects(struct bx_state *s)
{
    u32 want = 0;
    for (u32 v = 0; v < s->lay.nviews; v++)
        want |= 1u << s->lay.view[v].obj;
    return jit_objects_find(s->obj, s->nobj, want);
}

/* addr is in one of the module's JIT views: the only executable part of it */
bool bx_in_view(const struct module *m, u64 addr)
{
    const struct bx_state *s = m->bx;
    for (u32 v = 0; s && v < s->lay.nviews; v++) {
        u64 at = m->base + (u64)s->lay.view[v].off;
        if (addr >= at && addr - at < s->obj[s->lay.view[v].obj].size)
            return true;
    }
    return false;
}

/* bxplan.c's code map for the module, with the profile's ranges if it has some */
COLD static u32 code_runs(const struct module *m, struct bx_range *runs)
{
    const struct prof_exec *x;
    struct bx_code_in in;
    struct bx_code_stats st;
    u32 nexec = 0;
    const u32 *exec = NULL;
    for (u32 k = 0; (x = profile_record(REC_EXEC, k)); k++)
        if (strncmp(x->module, m->name, sizeof(x->module)) == 0)
            exec = x->range, nexec = x->count;
    bx_module_in(m, exec, nexec, &in);
    u32 n = in.seg ? bx_code_runs(&in, runs, &st) : 0;
    if (nexec)
        logf("bigexec: %s: %u code ranges from the profile", m->name, nexec);
    else
        logf("bigexec: %s: code map from %u FDEs (%lu KiB%s), %u prologues, %u PLT entries", m->name, st.fdes,
             st.fde_bytes >> 10, st.eh ? "" : "; no usable .eh_frame_hdr", st.prologues, st.plt);
    for (u32 i = 0; i < n; i++)
        logf("bigexec: %s: code +%lx-+%lx", m->name, runs[i].lo, runs[i].hi);
    return n;
}

/* ---- the reclaim: fresh JIT objects sized to the code ---- */

static struct bx_reclaim *g_rec; /* its answer, why it failed included */

const char *bx_why(void) { return g_rec && g_rec->why[0] ? g_rec->why : NULL; }

/* Instead of whole buffers: fresh objects sized to the code, made by the
 * extension once every reference to the emulator's buffers is dropped
 * (ghost_api.h struct bx_reclaim; ext/src/reclaim.c), and one for the other
 * modules, which becomes the arena. */
COLD static int reclaim(struct bx_state *s, struct module *big, u64 keep)
{
    struct bx_reclaim *r = g_rec = arena_alloc(sizeof(*r));
    if (!r)
        return -1;
    r->name = big->name, r->nruns = s->nruns, r->nforbid = s->nforbid, r->text = big->text_size, r->keep = keep;
    memcpy(r->runs, s->runs, sizeof(s->runs)), memcpy(r->forbid, s->forbid, sizeof(s->forbid));
    if (ext_reclaim(r) != 0) {
        logf("bigexec: no fresh JIT objects: %s", r->why);
        return -1;
    }
    memcpy(s->obj, r->obj, sizeof(r->obj));
    s->nobj = s->lay.nviews = r->n, s->fresh = true, s->lay.used = 0;
    for (u32 v = 0; v < r->n; v++)
        s->lay.view[v] = (struct bx_view){v, r->off[v]}, s->lay.used += r->size[v];
    return 0;
}

/* A module the arena cannot hold (or every title, with CTX_FLAG_RECLAIM):
 * its code runs from whole emulator buffers mapped over it, or, when they
 * do not cover it, from fresh objects after a reclaim. */
COLD int bx_prepare(void)
{
    u64 keep, st[8], en[8], sizes[BX_MAX_OBJS], total = 0;
    bool force = (g_ctx->flags & CTX_FLAG_RECLAIM) != 0, ps4 = is_ps4();
    u32 nb = arena_buffers(st, en, ARRAY_SIZE(st)), l = 0;
    for (u32 i = 1; i < nb; i++) /* the largest: where the modules go, past its header */
        l = en[i] - st[i] > en[l] - st[l] ? i : l;
    if (!g_modules || !nb || (!force && bx_arena_holds(g_modules, st[l] + JIT_BUF_HEADER, en[l])))
        return 0; /* placed in the arena as always (or failing there as always) */
    struct module *big = bx_pick(g_modules, &keep);
    struct bx_state *s = arena_alloc(sizeof(*s));
    if (!s)
        return -1;
    for (u32 i = 0; i < nb && s->nobj < BX_MAX_OBJS; i++) { /* largest first, then by address */
        u32 at = s->nobj++;
        for (; at && s->obj[at - 1].size < en[i] - st[i]; at--)
            s->obj[at] = s->obj[at - 1];
        s->obj[at] = (struct jit_obj){st[i], en[i] - st[i], 0, -1, -1};
    }
    for (u32 i = 0; i < s->nobj; i++)
        sizes[i] = s->obj[i].size, total += sizes[i];
    logf(force ? "bigexec: %s (text %lx): reclaim asked for" : "bigexec: %s (text %lx) does not fit the arena",
         big->name, big->text_size);
    s->nruns = code_runs(big, s->runs);
    s->nforbid = bx_data_ranges(big, s->forbid);
    if (force || ps4 || !s->nruns ||
        bx_cover(s->runs, s->nruns, s->forbid, s->nforbid, sizes, s->nobj, keep, &s->lay) != 0) {
        /* views are a PS5's (on a PS4 the compiler's mapping of a buffer may not cover it whole) */
        logf("bigexec: %s: no cover by %u buffers (%lu MiB)", big->name, s->nobj, total / MiB);
        if (reclaim(s, big, keep) != 0)
            return -1;
        big->bx = s;
        return 0;
    }
    for (u32 v = 0; v < s->lay.nviews; v++) {
        const struct jit_obj *o = &s->obj[s->lay.view[v].obj];
        logf("bigexec: %s: +%lx-+%lx executable: the %lu MiB JIT object at %lx", big->name, s->lay.view[v].off,
             s->lay.view[v].off + (s64)o->size, o->size / MiB, o->home);
    }
    if (find_objects(s) != 0)
        return -1;
    for (u32 v = 0; v < s->lay.nviews; v++)
        arena_drop(s->obj[s->lay.view[v].obj].home);
    big->bx = s;
    return 0;
}

/* the parts of [0, end) no view covers */
COLD static u32 gaps(const struct bx_state *s, s64 end, struct bx_range *g)
{
    s64 x = 0;
    u32 n = 0;
    for (u32 v = 0; v < s->lay.nviews; v++) {
        s64 lo = s->lay.view[v].off, hi = lo + (s64)s->obj[s->lay.view[v].obj].size;
        if (lo > end)
            lo = end;
        if (lo > x)
            g[n++] = (struct bx_range){x, lo};
        if (hi > x)
            x = hi;
    }
    if (x < end)
        g[n++] = (struct bx_range){x, end};
    return n;
}

COLD static u64 map_rw(u64 at, u64 len)
{
    void *p = direct_alloc(len, PROT_READ | PROT_WRITE, at);
    if ((u64)p != at) {
        logf("bigexec: no direct memory at %lx+%lx", at, len);
        return 0;
    }
    memset(p, 0, len);
    return len;
}

/* Reserve the module's range above 0x10_0000_0000, map the views and direct
 * memory, copy the data segments. */
COLD int bx_place(struct module *m)
{
    struct bx_state *s = m->bx;
    s64 lo = 0, hi = 0;
    for (u32 i = 0; i < m->phnum; i++)
        if ((m->phdrs[i].type == PT_LOAD || m->phdrs[i].type == PT_SCE_RELRO) &&
            (s64)ALIGN_UP(m->phdrs[i].vaddr + m->phdrs[i].memsz, 0x4000) > hi)
            hi = (s64)ALIGN_UP(m->phdrs[i].vaddr + m->phdrs[i].memsz, 0x4000);
    for (u32 v = 0; v < s->lay.nviews; v++) {
        s64 a = s->lay.view[v].off, b = a + (s64)s->obj[s->lay.view[v].obj].size;
        lo = a < lo ? a : lo, hi = b > hi ? b : hi;
    }
    u64 size = ALIGN_UP((u64)(hi - lo), 0x200000);
    void *va = (void *)BX_HINT;
    int rc = K.reserve_va(&va, size, 0, 0x200000);
    if (rc != 0 || (u64)va < BX_HIGH) {
        logf("bigexec: %s: no address range of %lx bytes above %lx (rc %x, got %p)", m->name, size, BX_HIGH, rc, va);
        return -1;
    }
    m->base = (u64)va - (u64)lo;
    for (u32 v = 0; v < s->lay.nviews; v++) {
        const struct jit_obj *o = &s->obj[s->lay.view[v].obj];
        u64 at = m->base + (u64)s->lay.view[v].off;
        void *res = NULL;
        rc = K.mmap((void *)at, o->size, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_FIXED, o->rx, 0, &res);
        if (rc != 0 || (u64)res != at) {
            logf("bigexec: %s: view of %lx at %lx: rc %x got %p", m->name, o->home, at, rc, res);
            return -1;
        }
    }
    struct bx_range g[BX_MAX_VIEWS + 1];
    u32 ng = gaps(s, (s64)m->text_size, g);
    u64 ro = 0, rw = 0, fl = 0, got;
    for (u32 i = 0; i < ng; i++) {
        if (!(got = map_rw(m->base + (u64)g[i].lo, (u64)(g[i].hi - g[i].lo))))
            return -1;
        ro += got;
        logf("bigexec: %s: +%lx-+%lx not executable (direct memory)", m->name, g[i].lo, g[i].hi);
    }
    /* data segments from system flexible memory, as the arena's modules' are:
     * each its own size, once, while ghostldr's punched total stays within
     * what was measured to be safe (no API tells what is left of the
     * console's pool, and mapping it until it fails freezes the console);
     * else direct memory */
    for (u32 i = 0; i < s->nforbid; i++) {
        u64 at = m->base + (u64)s->forbid[i].lo, len = (u64)(s->forbid[i].hi - s->forbid[i].lo);
        if (g_punched + len <= BX_FLEX_MAX && arena_punch_rw(at, len) == 0)
            fl += len;
        else if ((got = map_rw(at, len)))
            rw += got;
        else
            return -1;
    }
    for (u32 i = 0; i < m->phnum; i++) {
        const struct elf_phdr *p = &m->phdrs[i];
        if (((p->type == PT_LOAD && !(p->flags & 1)) || p->type == PT_SCE_RELRO) && p->filesz && m->seg_data[i])
            memcpy((void *)(m->base + p->vaddr), m->seg_data[i], p->filesz);
    }
    if (m->proc_param)
        m->proc_param += m->base;
    logf("%s: placed at %lx (text %lx in %u JIT objects, %lu MiB); direct memory %lu KiB read-only + %lu KiB data, "
         "%lu KiB data flexible (system flexible memory punched: %lu of %lu KiB)", m->name, m->base, m->text_size,
         s->lay.nviews, s->lay.used / MiB, ro >> 10, rw >> 10, fl >> 10, g_punched >> 10, BX_FLEX_MAX >> 10);
    return 0;
}

COLD static void jit_zero(u64 dst, u64 len)
{
    u64 chunk = len < g_ctx->staging_size ? len : g_ctx->staging_size;
    memset((void *)g_ctx->staging, 0, chunk);
    for (u64 n; len; dst += n, len -= n) {
        n = len < chunk ? len : chunk;
        jit_call(g_ctx->jit_memcpy, 0, dst, g_ctx->staging, n, 0, 0, 0);
    }
}

/* The text through each object's RW alias in the compiler (the parts of a
 * view outside the segment zeroed), read back through the views; the rest of
 * the segment into its direct memory, then read-only. */
COLD int bx_write_text(struct module *m, const u8 *text)
{
    struct bx_state *s = m->bx;
    s64 end = (s64)m->text_size;
    for (u32 v = 0; v < s->lay.nviews; v++) {
        const struct jit_obj *o = &s->obj[s->lay.view[v].obj];
        /* [off, a) before the segment, [a, b) in it, [b, top) after it */
        s64 off = s->lay.view[v].off, top = off + (s64)o->size, a = off < 0 ? 0 : off, b = top < end ? top : end;
        if (a > top)
            a = top;
        if (b < a)
            b = a;
        jit_zero(o->wr, (u64)(a - off));
        if (b > a)
            jit_copy_to(o->wr + (u64)(a - off), text + a, (u64)(b - a));
        jit_zero(o->wr + (u64)(b - off), (u64)(top - b));
        if (b > a && memcmp((const void *)(m->base + (u64)a), text + a, (u64)(b - a)) != 0) {
            logf("bigexec: %s: +%lx-+%lx written through %lx does not show in its view", m->name, a, b, o->wr);
            return -1;
        }
    }
    struct bx_range g[BX_MAX_VIEWS + 1];
    u32 ng = gaps(s, end, g);
    for (u32 i = 0; i < ng; i++) {
        memcpy((void *)(m->base + (u64)g[i].lo), text + g[i].lo, (u64)(g[i].hi - g[i].lo));
        K.mprotect((void *)(m->base + (u64)g[i].lo), (u64)(g[i].hi - g[i].lo), PROT_READ);
    }
    return 0;
}
