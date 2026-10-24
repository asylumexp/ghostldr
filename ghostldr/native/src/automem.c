/*
 * Automatic memory plans: what a manual memory plan gives a title (budget.c,
 * docs/compatibility.md), worked out from the title's own behaviour, for
 * every title without one.
 *
 *  1. Lazy commit.  Every direct allocation of AM_LAZY_MIN or more is backed
 *     2 MiB at a time when the title first maps that part (vdm.c), not up
 *     front: a heap the title fills a little at a time holds only what it
 *     uses.  New chunks are zeroed, as all of a plan-free title's memory is.
 *     Past what the title was told, such an allocation is admitted all the
 *     same (up to AM_ADMIT_MIB more), so one boot shows its whole demand
 *     where a refusal would stop most titles at the first pool too many.
 *  2. A learned plan per executable build, kept next to the title's saves
 *     (savedata.c, AM_FILE): each large allocation site's size, type and
 *     peak commit, the most the title reserved and held, and whether it ran
 *     out, of what it was told or of real memory.  The next boot tells the
 *     title what it reserved, more than is physically free (overcommit) only
 *     as far as the last run showed its heaps leave unused.
 *  3. Pool shrinking.  A pool the title maps whole gains nothing from lazy
 *     commit.  When the title needs more than that overcommit covers, or ran
 *     out of real memory, its pools are looked for in its executable as
 *     manual plans' tables are: the pool's size as a u64 among other sizes,
 *     one per memory mode.  The pool whose size varies most across modes (a
 *     cache the title sizes to the memory it has) shrinks first, none below
 *     half.  The pool's allocation then shows whether those cells were its
 *     size: a guess that changes nothing is never patched again, nor one
 *     after which the title twice stopped within two minutes.  When nothing
 *     is left to shrink after running out of real memory, allocations past
 *     what the title is told are refused again (the plan-free budget).
 *
 * The first boot of a build is the plan-free budget with lazy commit.
 * run.py --no-auto-memory turns all of it off, --auto-memory uses it instead
 * of a profile's manual plan, and --forget-memory starts the learned plan
 * over.
 */
#include "ghost.h"

#define MiB (1UL << 20)
#define AM_FILE "ghostldr-memory.bin"
#define AM_MAGIC 0x314d454d52444c47ULL /* "GLDRMEM1" */
#define AM_VERSION 1
#define AM_LAZY_MIN (64 * MiB)
#define AM_ADMIT_MIB 2048 /* lazily admitted past what the title was told, at most */
#define AM_POOL_MIN (256 * MiB)
#define AM_MARGIN_MIB 64
#define AM_STEP_MIB 128 /* the least the pools shrink by after running out of real memory */
#define AM_SITES 32
#define AM_SHRINKS 8
#define AM_CELLS 8
#define AM_RUN 64 /* size cells looked at either way for a pool's table */
#define COLD GHOST_COLD

enum { AM_TRY = 1, AM_OK = 2, AM_WRONG = 3, AM_FRAGILE = 4 };

struct am_site {
    u32 site;     /* eboot offset the allocation was called from */
    u32 len_mib;  /* the largest allocation from there */
    u32 peak_mib; /* the most of it the title committed (lazy) or held */
    u8 type, lazy, pad[2];
};

struct am_shrink {
    u32 site, orig_mib, new_mib;
    u32 prev_mib;        /* new_mib before its last change */
    u32 ncells;
    u32 cells[AM_CELLS]; /* eboot offsets of the u64 cells that held orig */
    u16 since;           /* the boot new_mib last changed in */
    u8 state, strikes;   /* strikes: boots in a row that stopped within two minutes */
    u8 locked, pad[3];   /* locked: it shrinks no further */
};

struct am_plan {
    u64 magic;
    u32 version, size;
    u8 fingerprint[20];
    u32 boots;
    u32 phys_mib;   /* direct memory the title could have for real, at plan time */
    u32 told_mib;   /* what it was told */
    u32 peak_mib;   /* the most it held: up-front allocations, lazy commits, flex */
    u32 demand_mib; /* the most it reserved */
    u32 oom;        /* AM_OOM_TOLD: refused past what it was told; AM_OOM_REAL: a lazy commit failed */
    u32 short_mib;  /* AM_OOM_TOLD: by how much at most */
    u32 left_mib;   /* AM_OOM_REAL: what its lazy heaps had left uncommitted then */
    u32 alive_min;  /* minutes the title ran */
    u32 strict;     /* nothing admitted past what the title is told */
    u32 nsites, nshrink;
    struct am_site sites[AM_SITES];
    struct am_shrink shrink[AM_SHRINKS];
};

static struct am_plan g_last, g_now;
static bool g_have_last, g_on, g_dirty;
static volatile u64 g_eager;                /* direct memory held by up-front allocations */
static u64 g_site_off[AM_SITES];            /* this boot: each site's latest lazy allocation */
static u64 g_start_us;

u64 vdm_lazy_bytes(u64 off);
bool vdm_lazy(u64 off);
u32 vdm_lazy_committed_mib(void);
u64 budget_title_dmem(void);
u64 budget_flex_used(void);
s64 savedata_aux_read(const char *name, void *buf, u64 size);
s64 savedata_aux_write(const char *name, const void *buf, u64 size);

bool automem_on(void) { return g_on; }

bool automem_lazy(u64 len) { return g_on && len >= AM_LAZY_MIN; }

static u64 now_us(void)
{
    static u64 (*pt)(void);
    if (!pt)
        pt = host_dlsym((u32)g_ctx->libkernel_handle, "sceKernelGetProcessTime");
    return pt ? pt() : 0;
}

COLD static void save(void)
{
    g_dirty = false;
    s64 n = savedata_aux_write(AM_FILE, &g_now, sizeof(g_now));
    if (n != (s64)sizeof(g_now))
        logf("automem: saving the learned plan failed (%ld)", n);
}

/* ---- pool size cells ---- */

/* a pool's size in a size table: the cell's neighbours hold sizes too
 * (whole MiB, up to 16 GiB), as manual plans' tables do (one u64 per memory
 * mode).  Read-only data sits in the text segment; nothing past it is read. */
COLD static bool sizeish(u64 v) { return v >= MiB && v <= (16UL << 30) && !(v & (MiB - 1)); }

COLD static u64 *cell(struct module *m, u8 *text, u64 off) { return off + 8 <= m->text_size ? (u64 *)(text + off) : NULL; }

COLD static bool size_at(struct module *m, u8 *text, u64 off)
{
    const u64 *c = cell(m, text, off);
    return c && sizeish(*c);
}

COLD static u32 find_cells(struct module *m, u8 *text, u64 value, u32 *cells, u32 max)
{
    u32 n = 0;
    for (u64 off = 16; off + 24 <= m->text_size; off += 8) {
        if (*(const u64 *)(text + off) != value)
            continue;
        u32 near = size_at(m, text, off - 16) + size_at(m, text, off - 8) + size_at(m, text, off + 8) +
                   size_at(m, text, off + 16);
        if (near < 2)
            continue;
        if (n == max)
            return max + 1; /* too many: no way to tell which is the pool's */
        cells[n++] = (u32)off;
    }
    return n;
}

COLD static struct am_shrink *shrink_of(struct am_plan *p, u32 site)
{
    for (u32 i = 0; i < p->nshrink && i < AM_SHRINKS; i++)
        if (p->shrink[i].site == site)
            return &p->shrink[i];
    return NULL;
}

COLD static bool active(const struct am_shrink *s) { return s->state == AM_TRY || s->state == AM_OK; }

/* MiB the active shrinks take (state: only those in that state) */
COLD static u32 shrunk(const struct am_plan *p, u8 state)
{
    u32 n = 0;
    for (u32 i = 0; i < p->nshrink && i < AM_SHRINKS; i++)
        if (state ? p->shrink[i].state == state : active(&p->shrink[i]))
            n += p->shrink[i].orig_mib - p->shrink[i].new_mib;
    return n;
}

struct cand {
    const struct am_site *s;
    struct am_shrink *k;    /* its shrink from an earlier boot, or NULL: fresh */
    struct am_shrink fresh; /* the cells found this boot */
    u32 elastic;
};

COLD static struct am_shrink *kof(struct cand *c) { return c->k ? c->k : &c->fresh; }

/* a pool located in the executable: its size cells, and the run of size
 * cells around the first */
struct wit {
    const u32 *cells;
    u32 n, lo, hi;
};

COLD static struct wit witness(struct module *m, u8 *text, const struct am_shrink *k)
{
    u32 first = k->cells[0];
    struct wit w = {.cells = k->cells, .n = k->ncells, .lo = first, .hi = first + 8};
    while (w.lo >= 8 && first - w.lo < AM_RUN * 8 && size_at(m, text, w.lo - 8))
        w.lo -= 8;
    while (w.hi - first < AM_RUN * 8 && size_at(m, text, w.hi))
        w.hi += 8;
    return w;
}

/* the table of width p in the run from lo that holds the cell at off */
COLD static u32 table_of(u32 lo, u32 off, u32 p) { return lo + (off - lo) / 8 / p * p * 8; }

/* The title's size tables stand side by side, one size per memory mode, and
 * the pools it allocated sit in the same column: its memory mode, the base
 * PS4's, in which every one of them is its table's smallest size.  The
 * widest table (up to 8) that cuts the run into whole tables and has a
 * column like that for every pool located in the run gives each pool its
 * own table; how much the pool's size varies across it (largest over
 * smallest, percent) says how elastic it is: a cache the title sizes to the
 * memory it has (Ghost's streaming pool, 1120 MiB in base mode, 4696 at the
 * most) varies most, a heap it needs whole (its GPU heap, 1390-2438) far
 * less.  100 when the run holds no other pool to tell the tables apart by. */
COLD static u32 elasticity(struct module *m, u8 *text, const struct wit *w, u32 nw, u32 i)
{
    u32 len = (w[i].hi - w[i].lo) / 8, others = 0;
    for (u32 j = 0; j < nw; j++)
        others += j != i && w[j].lo == w[i].lo;
    for (u32 p = 8; others && p >= 2; p--) {
        if (len % p)
            continue;
        for (u32 col = 0; col < p; col++) {
            u32 own = 0;
            bool all = true;
            for (u32 j = 0; all && j < nw; j++) {
                if (w[j].lo != w[i].lo)
                    continue;
                u32 at = 0;
                for (u32 x = 0; x < w[j].n && !at; x++) {
                    u32 off = w[j].cells[x];
                    if (off < w[i].lo || off >= w[i].hi || (off - w[i].lo) / 8 % p != col)
                        continue;
                    bool least = true;
                    for (u32 t = table_of(w[i].lo, off, p), y = 0; y < p && least; y++)
                        least = *cell(m, text, t + y * 8) >= *cell(m, text, off);
                    at = least ? off : 0;
                }
                own = j == i ? at : own;
                all = at != 0;
            }
            if (!all)
                continue;
            u64 lo = ~0UL, hi = 0;
            for (u32 t = table_of(w[i].lo, own, p), y = 0; y < p; y++) {
                u64 v = *cell(m, text, t + y * 8);
                lo = v < lo ? v : lo;
                hi = v > hi ? v : hi;
            }
            return (u32)(hi * 100 / lo);
        }
    }
    return 100;
}

/* shrink the title's pools by want MiB more than before: the most elastic
 * first, then the largest, none below half its size or 128 MiB.  Returns
 * the MiB taken. */
COLD static u32 plan_shrinks(struct module *m, u8 *text, u32 want)
{
    struct cand cand[AM_SITES];
    u32 nc = 0, taken = 0;
    for (u32 i = 0; i < g_last.nsites && i < AM_SITES; i++) {
        const struct am_site *s = &g_last.sites[i];
        if ((u64)s->len_mib * MiB < AM_POOL_MIN || s->peak_mib * 10 < s->len_mib * 9)
            continue; /* not a pool used whole */
        struct cand *c = &cand[nc];
        *c = (struct cand){.s = s, .k = shrink_of(&g_now, s->site)};
        if (c->k && (!active(c->k) || c->k->locked))
            continue; /* the wrong cells, or the title stopped after it */
        if (!c->k) {
            u32 cells[AM_CELLS + 1];
            u32 n = find_cells(m, text, (u64)s->len_mib * MiB, cells, AM_CELLS);
            if (!n || n > AM_CELLS) {
                logf("automem: the %u MiB pool from eboot+%x: %s in the executable", s->len_mib, s->site,
                     n ? "too many size cells" : "no size cell");
                continue;
            }
            c->fresh = (struct am_shrink){.site = s->site, .orig_mib = s->len_mib, .new_mib = s->len_mib,
                                          .prev_mib = s->len_mib, .ncells = n, .state = AM_TRY};
            memcpy(c->fresh.cells, cells, n * sizeof(u32));
        }
        nc++;
    }
    /* the candidates, and the pools located before (that keep their size now) */
    struct wit w[AM_SITES + AM_SHRINKS];
    u32 nw = 0;
    for (u32 i = 0; i < nc; i++)
        w[nw++] = witness(m, text, kof(&cand[i]));
    for (u32 i = 0; i < g_now.nshrink && i < AM_SHRINKS; i++) {
        bool known = false;
        for (u32 j = 0; j < nc; j++)
            known |= cand[j].s->site == g_now.shrink[i].site;
        if (!known && g_now.shrink[i].ncells && g_now.shrink[i].state != AM_WRONG)
            w[nw++] = witness(m, text, &g_now.shrink[i]);
    }
    for (u32 i = 0; i < nc; i++)
        cand[i].elastic = elasticity(m, text, w, nw, i);
    for (u32 i = 1; i < nc; i++) /* most elastic first, then the largest */
        for (u32 j = i; j > 0 && (cand[j].elastic > cand[j - 1].elastic ||
                                  (cand[j].elastic == cand[j - 1].elastic &&
                                   kof(&cand[j])->orig_mib > kof(&cand[j - 1])->orig_mib));
             j--) {
            struct cand t = cand[j];
            cand[j] = cand[j - 1], cand[j - 1] = t;
        }
    for (u32 i = 0; i < nc && want >= 32; i++) {
        struct am_shrink *k = cand[i].k;
        u32 orig = kof(&cand[i])->orig_mib, floor = orig / 2 > 128 ? orig / 2 : 128;
        if (kof(&cand[i])->new_mib <= floor + 32)
            continue;
        if (!k) { /* a pool shrinking for the first time */
            if (g_now.nshrink >= AM_SHRINKS)
                continue;
            k = &g_now.shrink[g_now.nshrink++];
            *k = cand[i].fresh;
        }
        u32 take = (k->new_mib - floor < want ? k->new_mib - floor : want) & ~1u;
        k->prev_mib = k->new_mib;
        k->new_mib -= take;
        k->since = (u16)g_now.boots;
        want -= take;
        taken += take;
        logf("automem: the %u MiB pool from eboot+%x (its size varies %u%% across memory modes) shrinks to %u MiB, "
             "%u size cells",
             k->orig_mib, k->site, cand[i].elastic, k->new_mib, k->ncells);
    }
    return taken;
}

/* write the active shrinks into the executable before it runs */
COLD static void apply_shrinks(struct module *m, u8 *text)
{
    for (u32 i = 0; i < g_now.nshrink && i < AM_SHRINKS; i++) {
        struct am_shrink *k = &g_now.shrink[i];
        if (!active(k) || k->new_mib == k->orig_mib)
            continue;
        u32 done = 0;
        for (u32 c = 0; c < k->ncells && c < AM_CELLS; c++) {
            u64 *v = cell(m, text, k->cells[c]);
            if (v && (*v == (u64)k->orig_mib * MiB || *v == (u64)k->new_mib * MiB)) {
                *v = (u64)k->new_mib * MiB;
                done++;
            }
        }
        logf("automem: eboot+%x pool %u -> %u MiB: %u of %u cells written", k->site, k->orig_mib, k->new_mib, done,
             k->ncells);
    }
}

/* ---- plan time ---- */

/* phys: direct memory the title can have for real.  Returns what it is told. */
COLD u64 automem_plan(struct module *m, u8 *text, u64 phys)
{
    g_on = true;
    struct am_plan *l = &g_last;
    if (!(g_ctx->flags & CTX_FLAG_AUTO_MEM_FORGET) &&
        savedata_aux_read(AM_FILE, l, sizeof(*l)) == (s64)sizeof(*l) && l->magic == AM_MAGIC &&
        l->version == AM_VERSION && l->size == sizeof(*l) && !memcmp(l->fingerprint, m->fingerprint, 20))
        g_have_last = true;
    else
        memset(l, 0, sizeof(*l));
    g_now = (struct am_plan){.magic = AM_MAGIC, .version = AM_VERSION, .size = sizeof(g_now),
                             .boots = l->boots + 1, .phys_mib = (u32)(phys / MiB), .strict = l->strict};
    memcpy(g_now.fingerprint, m->fingerprint, 20);
    g_now.nshrink = l->nshrink < AM_SHRINKS ? l->nshrink : AM_SHRINKS;
    memcpy(g_now.shrink, l->shrink, sizeof(g_now.shrink));

    u64 told = phys;
    if (g_have_last) {
        /* what the title's heaps left unused last time: it can be told that much more */
        u64 savings = 0;
        for (u32 i = 0; i < l->nsites && i < AM_SITES; i++)
            if (l->sites[i].lazy && l->sites[i].peak_mib < l->sites[i].len_mib)
                savings += (u64)(l->sites[i].len_mib - l->sites[i].peak_mib) * MiB;
        /* stopped early: the pools shrunk last are to blame */
        bool early = l->alive_min < 2 && !l->oom;
        u16 latest = 0;
        for (u32 i = 0; i < g_now.nshrink; i++)
            if (active(&g_now.shrink[i]) && g_now.shrink[i].since > latest)
                latest = g_now.shrink[i].since;
        for (u32 i = 0; i < g_now.nshrink; i++) {
            struct am_shrink *k = &g_now.shrink[i];
            if (!active(k))
                continue;
            k->strikes = early && k->since == latest ? k->strikes + 1 : 0;
            if (k->strikes < 2)
                continue;
            k->strikes = 0;
            if (k->prev_mib < k->orig_mib) {
                k->new_mib = k->prev_mib, k->locked = 1;
                logf("automem: the title twice stopped within two minutes after eboot+%x's pool shrank: back to "
                     "%u MiB, the size it ran with, and no smaller from now on",
                     k->site, k->new_mib);
            } else {
                k->state = AM_FRAGILE;
                logf("automem: the title twice stopped within two minutes after eboot+%x's pool shrank: it keeps "
                     "its %u MiB from now on",
                     k->site, k->orig_mib);
            }
        }
        logf("automem: learned plan, boot %u of this build: last time told %u MiB, reserved at most %u, held %u, "
             "%s%s; its heaps left %lu MiB unused",
             g_now.boots, l->told_mib, l->demand_mib, l->peak_mib,
             l->oom & AM_OOM_REAL ? "ran out of memory" : l->oom ? "ran out of what it was told" : "did not run out",
             early ? " but stopped within two minutes" : "", savings / MiB);
        /* what it reserves now: last time's demand, refusals included, with
         * the shrinks that took effect then undone and this boot's applied */
        s64 need = l->demand_mib;
        if ((l->oom & AM_OOM_TOLD) && l->told_mib + l->short_mib > need)
            need = l->told_mib + l->short_mib;
        need += AM_MARGIN_MIB + (s64)shrunk(l, AM_OK) - shrunk(&g_now, 0);
        s64 cap = (s64)((phys + savings) / MiB);
        u32 more = 0;
        if (l->oom & AM_OOM_REAL)
            more = (l->left_mib / 2 > AM_STEP_MIB ? l->left_mib / 2 : AM_STEP_MIB) + AM_MARGIN_MIB;
        else if (need > cap)
            more = (u32)(need - cap);
        if (more) {
            u32 got = plan_shrinks(m, text, more);
            need -= got;
            if ((l->oom & AM_OOM_REAL) && !got && !g_now.strict) {
                g_now.strict = 1;
                logf("automem: out of memory with no pool left to shrink: allocations past what the title is "
                     "told are refused from now on");
            }
        }
        if (!l->demand_mib && !l->oom)
            need = 0; /* nothing allocated last time: nothing learned */
        told = need > 0 ? (u64)(need < cap ? need : cap) * MiB : 0;
        if ((l->oom & AM_OOM_REAL) && told > (u64)l->told_mib * MiB)
            told = (u64)l->told_mib * MiB;
        if (told < phys || g_now.strict)
            told = phys;
    }
    told &= ~(2 * MiB - 1);
    apply_shrinks(m, text);
    g_now.told_mib = (u32)(told / MiB);
    logf("automem: told %lu MiB of direct memory (%lu MiB for real); allocations of %lu MiB or more are committed "
         "as they are mapped%s",
         told / MiB, phys / MiB, AM_LAZY_MIN / MiB, g_now.strict ? "" : ", and admitted past that much");
    save();
    return told;
}

/* ---- run time ---- */

COLD static struct am_site *site_of(u32 site, bool add)
{
    for (u32 i = 0; i < g_now.nsites && i < AM_SITES; i++)
        if (g_now.sites[i].site == site)
            return &g_now.sites[i];
    if (!add || g_now.nsites >= AM_SITES)
        return NULL;
    struct am_site *s = &g_now.sites[g_now.nsites++];
    *s = (struct am_site){.site = site};
    return s;
}

/* past what the title was told (told): admit a large allocation anyway, it
 * is committed only as it is mapped */
bool automem_admit(u64 total, u64 len, u64 told)
{
    if (!g_on || g_now.strict || len < AM_LAZY_MIN || total > told + AM_ADMIT_MIB * MiB)
        return false;
    static bool said;
    if (!said) {
        said = true;
        logf("automem: %lu MiB reserved, past the %lu MiB the title was told: admitted, committed as it is mapped",
             total / MiB, told / MiB);
    }
    return true;
}

COLD static void measure(void)
{
    for (u32 i = 0; i < g_now.nsites && i < AM_SITES; i++) {
        struct am_site *s = &g_now.sites[i];
        u32 c = s->lazy && g_site_off[i] ? (u32)(vdm_lazy_bytes(g_site_off[i]) / MiB) : 0;
        if (c > s->peak_mib)
            s->peak_mib = c, g_dirty = true;
    }
    u32 demand = (u32)(budget_title_dmem() / MiB);
    u32 held = (u32)((g_eager + budget_flex_used()) / MiB) + vdm_lazy_committed_mib();
    if (demand > g_now.demand_mib)
        g_now.demand_mib = demand, g_dirty = true;
    if (held > g_now.peak_mib)
        g_now.peak_mib = held, g_dirty = true;
}

void automem_note_alloc(u64 site, u64 len, int type, bool lazy, u64 off)
{
    if (!g_on)
        return;
    if (!lazy)
        __atomic_fetch_add(&g_eager, len, __ATOMIC_RELAXED);
    u32 demand = (u32)(budget_title_dmem() / MiB);
    if (demand > g_now.demand_mib)
        g_now.demand_mib = demand, g_dirty = true;
    struct am_shrink *k = shrink_of(&g_now, (u32)site);
    if (k && active(k) && k->new_mib != k->orig_mib) {
        u8 was = k->state;
        if (len == (u64)k->new_mib * MiB)
            k->state = AM_OK;
        else if (len == (u64)k->orig_mib * MiB)
            k->state = AM_WRONG;
        if (k->state != was) {
            logf("automem: eboot+%x allocated %lu MiB: the size cells were %s", k->site, len / MiB,
                 k->state == AM_OK ? "its pool's" : "not its pool's (never patched again)");
            save(); /* kept even if the title stops before the next save */
        }
    }
    if (len < 16 * MiB)
        return;
    struct am_site *s = site_of((u32)site, true);
    if (!s)
        return;
    if (len / MiB > s->len_mib)
        s->len_mib = (u32)(len / MiB);
    s->type = (u8)type, s->lazy = lazy;
    if (!lazy && len / MiB > s->peak_mib)
        s->peak_mib = (u32)(len / MiB);
    if (lazy)
        g_site_off[s - g_now.sites] = off;
    g_dirty = true;
}

void automem_note_release(u64 off, u64 len)
{
    if (g_on && !vdm_lazy(off))
        __atomic_fetch_sub(&g_eager, len, __ATOMIC_RELAXED);
}

/* out of memory (budget.c, vdm.c): AM_OOM_TOLD, an allocation refused,
 * short = how far past what the title was told; AM_OOM_REAL, a lazy commit
 * found no memory.  Saved at once: the title may not outlive it. */
void automem_note_out_of_memory(int kind, u64 short_bytes)
{
    if (!g_on)
        return;
    u32 mib = (u32)((short_bytes + MiB - 1) / MiB);
    bool news = !(g_now.oom & (u32)kind) || (kind == AM_OOM_TOLD && mib > g_now.short_mib);
    if (!news)
        return;
    g_now.oom |= (u32)kind;
    if (kind == AM_OOM_TOLD)
        g_now.short_mib = mib;
    measure();
    if (kind == AM_OOM_REAL) { /* what more the lazy heaps could still ask for */
        u32 left = 0;
        for (u32 i = 0; i < g_now.nsites && i < AM_SITES; i++)
            if (g_now.sites[i].lazy && g_site_off[i])
                left += g_now.sites[i].len_mib - (u32)(vdm_lazy_bytes(g_site_off[i]) / MiB);
        g_now.left_mib = left;
    }
    save();
}

/* every few seconds while the title runs: peaks, minutes alive, and the
 * learned plan saved when it changed */
COLD void automem_tick(void)
{
    if (!g_on)
        return;
    u64 t = now_us();
    if (!g_start_us)
        g_start_us = t;
    measure();
    u32 alive = (u32)((t - g_start_us) / 60000000);
    static u64 last_save;
    bool minute = alive != g_now.alive_min && alive <= 2; /* past the early-stop window soon */
    if (alive != g_now.alive_min)
        g_now.alive_min = alive, g_dirty = true;
    if (minute || (g_dirty && t - last_save >= 300000000)) {
        last_save = t;
        save();
    }
}

static void *automem_thread(void *arg)
{
    for (;;) {
        automem_tick();
        K.usleep(5000000);
    }
    return arg;
}

COLD void automem_start(void)
{
    if (!g_on)
        return;
    u64 tid = 0;
    K.pthread_create(&tid, NULL, automem_thread, NULL, "ghostldr-automem");
}
