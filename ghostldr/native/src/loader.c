/* Map, relocate, and publish Orbis modules inside the host process.
 *
 * Code (the executable PT_LOAD, including its rodata) lives in a JIT block
 * created by the ps2emu compiler process and mapped RX at the module base;
 * everything else is flexible memory mapped RW at its fixed offset. */
#include "ghost.h"

#define COLD GHOST_COLD /* boot-time code: small beats fast */

#define R_X86_64_64 1
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define R_X86_64_DTPMOD64 16
#define R_X86_64_DTPOFF64 17
#define R_X86_64_TPOFF64 18
#define R_X86_64_DTPOFF32 21
#define R_X86_64_TPOFF32 23
#define STB_WEAK 2

const char *module_import_lib(struct module *m, const char *encoded, char *nid, const char **modname);
const char *module_export_lib(struct module *m, const char *encoded);

struct module *g_modules;
static u32 g_next_tls_id = 0x1000;
static u32 g_next_handle = 0x7000;

COLD struct module *module_open(const char *path, bool is_main)
{
    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0) {
        logf("open %s failed %x", path, fd);
        return NULL;
    }
    off_t size = (off_t)vfs_lseek(fd, 0, 2);
    if (size <= 0) {
        /* A failed seek used to fall through as a huge size (and a short
         * read then skipped the parse): fail here, with the file named. */
        logf("size %s failed %lx", path, size);
        vfs_close(fd);
        return NULL;
    }
    if (vfs_lseek(fd, 0, 0) < 0) {
        logf("seek %s failed", path);
        vfs_close(fd);
        return NULL;
    }
    u8 *buf = page_alloc((size_t)size);
    if (!buf) {
        vfs_close(fd);
        return NULL;
    }
    off_t got = 0;
    while (got < size) {
        ssize_t n = vfs_read(fd, buf + got, (size_t)(size - got) > 0x1000000 ? 0x1000000 : (size_t)(size - got));
        if (n <= 0) {
            logf("read %s failed at %lx: %lx", path, got, n);
            vfs_close(fd);
            page_free(buf, (size_t)size);
            return NULL;
        }
        got += n;
    }
    vfs_close(fd);
    struct module *m = arena_alloc(sizeof(*m));
    const char *slash = strrchr_c(path, '/');
    if (m)
        m->name = arena_strdup(slash ? slash + 1 : path);
    if (!m || !m->name) {
        page_free(buf, (size_t)size);
        return NULL;
    }
    m->is_main = is_main;
    if (module_parse(m, buf, (u64)size) != 0) {
        page_free(buf, (size_t)size);
        return NULL;
    }
    return m;
}

static void module_ids(struct module *m)
{
    if (m->fake_handle)
        return;
    m->fake_handle = g_next_handle++;
    if (m->tls_memsz) /* the executable's DTPMOD64 gets 1 (tls.c), not the host's module 0 */
        m->tls_id = m->is_main ? 1 : g_next_tls_id++;
}

/* Append to g_modules, keeping load order.  tls.c and crash.c walk the list
 * without a lock: the module is complete before the release store links it. */
void module_publish(struct module *m)
{
    module_ids(m);
    struct module **pp = &g_modules;
    while (*pp)
        pp = &(*pp)->next;
    __atomic_store_n(pp, m, __ATOMIC_RELEASE);
}

/* The ELF loader's payload is done (ext/src/elfldr_sys.c): readers that
 * walk the list without a lock may be on m, whose next stays as it was. */
void module_unpublish(struct module *m)
{
    for (struct module **pp = &g_modules; *pp; pp = &(*pp)->next)
        if (*pp == m) {
            __atomic_store_n(pp, m->next, __ATOMIC_RELEASE);
            return;
        }
}

int module_place(struct module *m, u64 base)
{
    if (m->bx) /* code in whole JIT objects, the rest in direct memory (bigexec.c) */
        return bx_place(m);
    m->base = base;
    if (!arena_is_rx(base, m->text_size)) {
        logf("%s: text %lx+%lx not inside the RX arena", m->name, base, m->text_size);
        return -1;
    }
    for (u32 i = 0; i < m->phnum; i++) {
        struct elf_phdr *p = &m->phdrs[i];
        if (p->type == PT_SCE_RELRO) {
            m->relro_lo = ALIGN_DOWN(p->vaddr, 0x4000);
            m->relro_size = ALIGN_UP(p->vaddr + p->memsz, 0x4000) - m->relro_lo;
            if (!arena_is_rx(base + m->relro_lo, m->relro_size)) {
                logf("%s: relro not inside the RX arena", m->name);
                return -1;
            }
            m->relro_shadow = page_alloc(m->relro_size);
            if (!m->relro_shadow)
                return -1;
            if (m->seg_data[i])
                memcpy(m->relro_shadow + (p->vaddr - m->relro_lo), m->seg_data[i], p->filesz);
        } else if (p->type == PT_LOAD && !(p->flags & 1)) {
            u64 lo = ALIGN_DOWN(p->vaddr, 0x4000), hi = ALIGN_UP(p->vaddr + p->memsz, 0x4000);
            if (arena_punch_rw(base + lo, hi - lo) != 0)
                return -1;
            if (p->filesz && m->seg_data[i])
                memcpy((void *)(base + p->vaddr), m->seg_data[i], p->filesz);
        }
    }
    if (m->proc_param)
        m->proc_param += m->base;
    logf("%s: placed at %lx (text %lx, relro %lx+%lx)", m->name, m->base, m->text_size, m->relro_lo,
         m->relro_size);
    return 0;
}

/* Export table, sorted by NID for binary search. */
static void sort_exports(struct export_ent *e, u32 n)
{
    for (u32 gap = n / 2; gap; gap /= 2)
        for (u32 i = gap; i < n; i++) {
            struct export_ent t = e[i];
            u32 j = i;
            for (; j >= gap && strcmp(e[j - gap].nid, t.nid) > 0; j -= gap)
                e[j] = e[j - gap];
            e[j] = t;
        }
}

static void build_exports(struct module *m)
{
    u32 n = 0;
    for (u64 i = 0; i < m->nsyms; i++) {
        const struct elf_sym *s = &m->symtab[i];
        if (s->shndx && (s->info >> 4) != 0 && s->name)
            n++;
    }
    m->exports = arena_alloc(sizeof(struct export_ent) * (n + 1));
    for (u64 i = 0; i < m->nsyms; i++) {
        const struct elf_sym *s = &m->symtab[i];
        if (!s->shndx || (s->info >> 4) == 0 || !s->name)
            continue;
        const char *enc = module_sym_name(m, s);
        struct export_ent *e = &m->exports[m->nexports++];
        u32 k = 0;
        for (; k < 11 && enc[k] && enc[k] != '#'; k++)
            e->nid[k] = enc[k];
        e->nid[k] = 0;
        /* copied: the executable's file buffer is freed after loading */
        const char *lib = module_export_lib(m, enc);
        e->lib = (n > 0 && m->nexports > 1 && e[-1].lib && lib && strcmp(e[-1].lib, lib) == 0) ? e[-1].lib
                 : lib ? arena_strdup(lib) : NULL;
        e->addr = m->base + s->value;
    }
    sort_exports(m->exports, m->nexports);
}

u64 module_export(struct module *m, const char *nid)
{
    u32 lo = 0, hi = m->nexports;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        int c = strcmp(m->exports[mid].nid, nid);
        if (c == 0)
            return m->exports[mid].addr;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

static u64 sym_value(struct module *m, u32 index, u32 *missing, bool plt)
{
    if (index == 0 || index >= m->nsyms)
        return 0;
    const struct elf_sym *s = &m->symtab[index];
    if (s->shndx)
        return m->base + s->value;
    u32 type = s->info & 0xf; /* STT_NOTYPE 0, STT_OBJECT 1, STT_FUNC 2 */
    int kind = type == 1 || (type == 0 && !plt) ? IMPORT_OBJECT : plt && type == 2 ? IMPORT_PLT : IMPORT_DATA;
    u64 v = resolve_import(m, module_sym_name(m, s), (s->info >> 4) == STB_WEAK, kind);
    if (!v)
        (*missing)++;
    return v;
}

/* an imported TLS variable: the title module that exports it, and its
 * offset in that module's block (exports are base + st_value) */
COLD static struct module *tls_import(struct module *m, u32 sym, u64 *value)
{
    char nid[12];
    const char *lib = module_import_lib(m, module_sym_name(m, &m->symtab[sym]), nid, NULL);
    for (struct module *o = g_modules; o && lib; o = o->next)
        for (u32 i = 0; i < o->n_export_libs; i++)
            if (strcmp(o->export_libs[i].name, lib) == 0) {
                u64 a = module_export(o, nid);
                if (a && o->tls_memsz) {
                    *value = a - o->base;
                    return o;
                }
            }
    return NULL;
}

static int apply(struct module *m, const struct elf_rela *r, u64 n, u32 *missing, u32 *unsupported)
{
    for (u64 i = 0; i < n; i++) {
        u32 type = (u32)r[i].info, sym = (u32)(r[i].info >> 32);
        u64 *where;
        if (r[i].offset < m->text_size) {
            (*unsupported)++;
            continue;
        }
        if (m->relro_shadow && r[i].offset >= m->relro_lo && r[i].offset < m->relro_lo + m->relro_size)
            where = (u64 *)(m->relro_shadow + (r[i].offset - m->relro_lo));
        else
            where = (u64 *)(m->base + r[i].offset);
        switch (type) {
        case R_X86_64_RELATIVE:
            *where = m->base + r[i].addend;
            break;
        case R_X86_64_64:
            *where = sym_value(m, sym, missing, false) + r[i].addend;
            break;
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
            *where = sym_value(m, sym, missing, type == R_X86_64_JUMP_SLOT);
            break;
        case R_X86_64_DTPMOD64:
        case R_X86_64_DTPOFF64:
        case R_X86_64_DTPOFF32:
        case R_X86_64_TPOFF64:
        case R_X86_64_TPOFF32: {
            /* the variable's module: this one, or for an imported thread_local
             * the title module that exports it (its offset in that block) */
            u64 value = sym ? m->symtab[sym].value : 0;
            struct module *d = sym && !m->symtab[sym].shndx ? tls_import(m, sym, &value) : m;
            bool tp = type == R_X86_64_TPOFF64 || type == R_X86_64_TPOFF32;
            if (!d) {
                /* not a title module's variable (a host library's, or an
                 * undefined weak one): this module's own block, as the
                 * loader always did (Ghost's libSceFios2 has one) */
                logf("%s: TLS reloc type %u at %lx: imported variable is no title module's; using this module's block",
                     m->name, type, r[i].offset);
                d = m;
            }
            if (tp && !d->tls_offset) {
                (*unsupported)++;
                logf("%s: TLS reloc type %u at %lx: no static TLS", m->name, type, r[i].offset);
                break;
            }
            u64 v = type == R_X86_64_DTPMOD64 ? d->tls_id
                    : tp                      ? value + (u64)r[i].addend - d->tls_offset
                                              : value + (u64)r[i].addend;
            if (type == R_X86_64_DTPOFF32 || type == R_X86_64_TPOFF32)
                *(u32 *)where = (u32)v;
            else
                *where = v;
            break;
        }
        default:
            (*unsupported)++;
            logf("%s: unsupported reloc type %u at %lx", m->name, type, r[i].offset);
        }
    }
    return 0;
}

int module_relocate(struct module *m)
{
    u32 missing = 0, unsupported = 0;
    apply(m, m->rela, m->nrela, &missing, &unsupported);
    apply(m, m->jmprel, m->njmprel, &missing, &unsupported);
    logf("%s: relocated (%lu rela, %lu plt) missing %u unsupported %u", m->name, m->nrela, m->njmprel,
         missing, unsupported);
    return unsupported ? -1 : 0;
}

/* Rewrite each thread-pointer load "mov r64, fs:[0]" (and "add r64, fs:[0]"),
 * with or without the 66 66 66 padding of the relaxed form, to read fs:[-8]:
 * the host's 8-byte main-module TLS slot then points at a per-thread title
 * TLS block (see tls.c).  Other fs: loads are counted and left alone. */
static bool tls_form(const u8 *c) /* fs, REX.W, mov/add, [disp32], disp32 follows */
{
    return c[0] == 0x64 && (c[1] == 0x48 || c[1] == 0x4C) && (c[2] == 0x8B || c[2] == 0x03) && (c[3] & 0xC7) == 0x04 &&
           c[4] == 0x25;
}

COLD static u32 patch_tls(u8 *code, u64 size, u32 *other)
{
    u32 n = 0;
    for (u64 i = 0; i + 9 <= size; i++) {
        if (!tls_form(code + i))
            continue;
        if (*(u32 *)(code + i + 5) == 0) {
            *(u32 *)(code + i + 5) = 0xFFFFFFF8u;
            n++;
            i += 8;
        } else {
            (*other)++;
        }
    }
    return n;
}

/* The title profile's list for the module (tools/tls_sites.py: only sites
 * inside functions, by disassembly), applied instead of the scan.  Each
 * listed site must still hold the form with displacement 0; others are
 * counted in *bad and left alone.  Returns -1 when the profile has no list. */
COLD static s64 patch_tls_listed(const char *name, u8 *code, u64 size, u32 *bad)
{
    s64 n = -1;
    const struct prof_tls *t;
    for (u32 k = 0; (t = profile_record(REC_TLS, k)); k++) {
        if (strncmp(t->module, name, sizeof(t->module)) != 0)
            continue;
        n = n < 0 ? 0 : n;
        for (u32 i = 0; i < t->count; i++) {
            u8 *c = code + t->off[i];
            if (t->off[i] + 9 <= size && tls_form(c) && *(u32 *)(c + 5) == 0)
                *(u32 *)(c + 5) = 0xFFFFFFF8u, n++;
            else
                (*bad)++;
        }
    }
    return n;
}

int module_finish(struct module *m)
{
    build_exports(m);
    return 0;
}

int module_write_text(struct module *m)
{
    u8 *buf = page_alloc(m->text_size);
    if (!buf)
        return -1;
    for (u32 i = 0; i < m->phnum; i++) {
        struct elf_phdr *p = &m->phdrs[i];
        if (p->type == PT_LOAD && (p->flags & 1) && m->seg_data[i])
            memcpy(buf + p->vaddr, m->seg_data[i], p->filesz);
    }
    if (m->tls_offset) { /* static TLS: the executable, or a prx with initial-exec relocations */
        u32 other = 0;
        s64 n = patch_tls_listed(m->name, buf, m->text_size, &other);
        if (n >= 0) {
            logf("%s: patched %u TLS sites from the profile's list (%u listed sites did not match)", m->name,
                 (u32)n, other);
        } else {
            /* a module with a code map (bigexec.c): only its code, not the data around it */
            const struct bx_range *r = NULL;
            u32 nr = bx_code(m, &r), k = nr ? 0 : patch_tls(buf, m->text_size, &other);
            for (u32 i = 0; i < nr; i++)
                k += patch_tls(buf + r[i].lo, (u64)(r[i].hi - r[i].lo), &other);
            logf("%s: patched %u TLS sites (%u other fs: loads left as they are)%s", m->name, k, other,
                 nr ? " in its code runs" : "");
        }
    }
    if (m->is_main && m->file) {
        /* everything that needed the file (relocation, exports, TLS image,
         * text) is done: give its ~26 MiB back before the memory plan.  The
         * module and library names stay (budget.c reads them): copied out */
        m->modname = m->modname ? arena_strdup(m->modname) : NULL;
        for (u32 i = 0; i < m->n_import_libs; i++)
            m->import_libs[i].name = arena_strdup(m->import_libs[i].name);
        for (u32 i = 0; i < m->n_export_libs; i++)
            m->export_libs[i].name = arena_strdup(m->export_libs[i].name);
        for (u32 i = 0; i < m->n_needed_mods; i++)
            m->needed_mods[i].name = arena_strdup(m->needed_mods[i].name);
        m->n_needed_files = 0;
        page_free(m->file, m->file_size);
        m->file = NULL;
        m->strtab = NULL, m->symtab = NULL, m->dynlib = NULL, m->nsyms = 0;
        for (u32 i = 0; i < ARRAY_SIZE(m->seg_data); i++)
            m->seg_data[i] = NULL;
    }
    if (m->is_main) {
        if (budget_patch_text(m, buf) != 0) { /* the title cannot fit: stop before it starts */
            page_free(buf, m->text_size);
            return -1;
        }
        hooks_patch_text(m, buf);
    }
    ext_patch_text(m, buf, m->text_size);
    int rc = m->bx ? bx_write_text(m, buf) : arena_write(m->base, buf, m->text_size);
    page_free(buf, m->text_size);
    if (rc == 0 && m->relro_shadow) {
        rc = arena_write(m->base + m->relro_lo, m->relro_shadow, m->relro_size);
        page_free(m->relro_shadow, m->relro_size);
        m->relro_shadow = NULL;
    }
    u64 probe = *(volatile u64 *)m->base;
    logf("%s: text written rc=%d first qword %lx", m->name, rc, probe);
    return rc;
}

void module_protect_relro(struct module *m)
{
    (void)m; /* RELRO lives in the RX arena: already read-only here */
}

/* ---- a title prx loaded after boot (sceKernelLoadStartModule, sysmodule) ----
 * Placed after the modules already in the code region; the whole image must
 * lie in executable memory, since its data pages are punched like boot
 * modules' and nothing past the region may be unmapped.  Punching maps
 * system flexible memory, which freezes the console when exhausted: run-time
 * loads stop at a total well inside what was measured to work. */
#define RUNTIME_PUNCH_LIMIT (128UL << 20)
#define E_NOENT 0x80020002
#define E_NOEXEC 0x80020008
#define E_NOMEM 0x8002000C

COLD static int runtime_load(const char *path, struct module **out)
{
    u8 st[0x78];
    if (vfs_stat(path, st) != 0)
        return (int)E_NOENT;
    struct module *m = module_open(path, false);
    if (!m)
        return (int)E_NOEXEC;
    if (m->tls_memsz && module_has_tpoff(m)) {
        /* every thread's static block is already laid out */
        logf("%s: initial-exec TLS cannot load after boot", m->name);
        page_free(m->file, m->file_size);
        return (int)E_NOEXEC;
    }
    u64 rw = 0;
    for (u32 i = 0; i < m->phnum; i++)
        if (m->phdrs[i].type == PT_LOAD && !(m->phdrs[i].flags & 1))
            rw += ALIGN_UP(m->phdrs[i].vaddr + m->phdrs[i].memsz, 0x4000) - ALIGN_DOWN(m->phdrs[i].vaddr, 0x4000);
    u64 at = arena_code_alloc(m->image_size);
    if (!at || !arena_is_rx(at, m->image_size) || g_punched + rw > RUNTIME_PUNCH_LIMIT) {
        logf("%s: no room at run time (base %lx, image %lx, data %lx, %lu KiB punched)", m->name, at, m->image_size,
             rw, g_punched >> 10);
        page_free(m->file, m->file_size);
        return (int)E_NOMEM;
    }
    if (module_place(m, at) != 0)
        return (int)E_NOMEM;
    module_finish(m);
    module_ids(m);
    if (module_relocate(m) != 0 || module_write_text(m) != 0)
        return (int)E_NOEXEC;
    stubs_flush();
    module_publish(m);
    *out = m;
    return 0;
}

/* one user of the compiler-process RPC at a time: it is not reentrant (the
 * run-time loader and tls.c's callback thunks write code after boot) */
static volatile int g_rpc_lock;
void rpc_lock(void)
{
    while (__atomic_exchange_n(&g_rpc_lock, 1, __ATOMIC_ACQUIRE))
        K.usleep(1000);
}
void rpc_unlock(void) { __atomic_store_n(&g_rpc_lock, 0, __ATOMIC_RELEASE); }

/* The loaded module of that file name, or the module loaded from path now,
 * one at a time.  *err: 0 or an SCE kernel error. */
COLD struct module *module_load_runtime(const char *path, int *err)
{
    const char *base = strrchr_c(path, '/');
    base = base ? base + 1 : path;
    rpc_lock();
    struct module *m = g_modules;
    while (m && strcmp(m->name, base) != 0)
        m = m->next;
    int e = m ? 0 : runtime_load(path, &m);
    rpc_unlock();
    logf("load %s at run time -> %x%s", path, e, m && !e ? "" : " (failed)");
    if (err)
        *err = e;
    return e ? NULL : m;
}
