/* Orbis (F)SELF / ELF parsing: program headers, segment data, Sony dynamic
 * table in PT_SCE_DYNLIBDATA, modules and libraries. */
#include "ghost.h"

#define DT_NULL 0
#define DT_INIT 12
#define DT_FINI 13
#define DT_SCE_FINGERPRINT 0x61000007
#define DT_SCE_MODULE_INFO 0x6100000D
#define DT_SCE_NEEDED_MODULE 0x6100000F
#define DT_SCE_EXPORT_LIB 0x61000013
#define DT_SCE_IMPORT_LIB 0x61000015
#define DT_SCE_JMPREL 0x61000029
#define DT_SCE_PLTRELSZ 0x6100002D
#define DT_SCE_RELA 0x6100002F
#define DT_SCE_RELASZ 0x61000031
#define DT_SCE_STRTAB 0x61000035
#define DT_SCE_STRSZ 0x61000037
#define DT_SCE_SYMTAB 0x61000039
#define DT_SCE_SYMTABSZ 0x6100003F

static const u8 SELF_MAGIC[4] = {0x4f, 0x15, 0x3d, 0x1d};
static const u8 ELF_MAGIC[4] = {0x7f, 'E', 'L', 'F'};

static const char *str_at(struct module *m, u64 off)
{
    return off < m->strsz ? m->strtab + off : "";
}

int module_parse(struct module *m, u8 *file, u64 size)
{
    m->file = file;
    m->file_size = size;
    u64 elf_off = 0;
    bool is_self = memcmp(file, SELF_MAGIC, 4) == 0;
    u16 nentries = 0;
    if (is_self) {
        nentries = *(u16 *)(file + 0x18);
        elf_off = 0x20 + (u64)nentries * 0x20;
    }
    u8 *ehdr = file + elf_off;
    if (memcmp(ehdr, ELF_MAGIC, 4) != 0) {
        logf("%s: no ELF header", m->name);
        return -1;
    }
    m->entry = *(u64 *)(ehdr + 0x18);
    u64 phoff = *(u64 *)(ehdr + 0x20);
    m->phnum = *(u16 *)(ehdr + 0x38);
    m->phdrs = (struct elf_phdr *)(ehdr + phoff);
    if (m->phnum > 32)
        return -1;

    if (is_self) {
        for (u32 i = 0; i < nentries; i++) {
            const u64 *e = (const u64 *)(file + 0x20 + i * 0x20);
            u64 props = e[0], off = e[1];
            if (!(props & 0x800))
                continue;
            if (props & 0xA) {
                logf("%s: SELF segment encrypted/compressed (props %lx)", m->name, props);
                return -1;
            }
            u32 seg = (props >> 20) & 0xFFF;
            if (seg < m->phnum)
                m->seg_data[seg] = file + off;
        }
        /* PT_DYNAMIC, PT_TLS, PT_SCE_PROCPARAM... have no SELF entry: their
         * bytes sit inside another segment's file range */
        for (u32 i = 0; i < m->phnum; i++) {
            struct elf_phdr *p = &m->phdrs[i];
            if (m->seg_data[i] || !p->filesz)
                continue;
            for (u32 j = 0; j < m->phnum; j++) {
                struct elf_phdr *q = &m->phdrs[j];
                if (m->seg_data[j] && j != i && p->offset >= q->offset &&
                    p->offset + p->filesz <= q->offset + q->filesz) {
                    m->seg_data[i] = m->seg_data[j] + (p->offset - q->offset);
                    break;
                }
            }
        }
    } else {
        for (u32 i = 0; i < m->phnum; i++)
            m->seg_data[i] = file + m->phdrs[i].offset;
    }

    const u64 *dyn = NULL;
    u64 ndyn = 0, dynlib_size = 0, fingerprint = ~0UL;
    for (u32 i = 0; i < m->phnum; i++) {
        struct elf_phdr *p = &m->phdrs[i];
        if (p->type == PT_DYNAMIC && m->seg_data[i]) {
            dyn = (const u64 *)m->seg_data[i];
            ndyn = p->filesz / 16;
        } else if (p->type == PT_SCE_DYNLIBDATA) {
            m->dynlib = m->seg_data[i];
            dynlib_size = p->filesz;
        } else if (p->type == PT_TLS) {
            m->tls_vaddr = p->vaddr;
            m->tls_filesz = p->filesz;
            m->tls_memsz = p->memsz;
            m->tls_align = p->align;
        } else if (p->type == PT_SCE_PROCPARAM) {
            m->proc_param = p->vaddr; /* rebased in module_map */
        }
    }
    if (!dyn || !m->dynlib) {
        logf("%s: missing dynamic data", m->name);
        return -1;
    }
    u64 symtabsz = 0, relasz = 0, pltrelsz = 0, strtab = 0, symtab = 0, rela = 0, jmprel = 0;
    for (u64 i = 0; i < ndyn; i++) {
        u64 tag = dyn[i * 2], val = dyn[i * 2 + 1];
        if (tag == DT_NULL)
            break;
        switch (tag) {
        case DT_SCE_STRTAB: strtab = val; break;
        case DT_SCE_STRSZ: m->strsz = val; break;
        case DT_SCE_SYMTAB: symtab = val; break;
        case DT_SCE_SYMTABSZ: symtabsz = val; break;
        case DT_SCE_RELA: rela = val; break;
        case DT_SCE_RELASZ: relasz = val; break;
        case DT_SCE_JMPREL: jmprel = val; break;
        case DT_SCE_PLTRELSZ: pltrelsz = val; break;
        case DT_INIT: m->init = val; break;
        case DT_FINI: m->fini = val; break;
        case DT_SCE_FINGERPRINT: fingerprint = val; break;
        }
    }
    /* the build of the executable: title profiles are bound to it (profile.c) */
    if (m->is_main)
        profile_bind(fingerprint + 20 <= dynlib_size ? m->dynlib + fingerprint : NULL);
    m->strtab = (const char *)(m->dynlib + strtab);
    m->symtab = (const struct elf_sym *)(m->dynlib + symtab);
    m->nsyms = symtabsz / sizeof(struct elf_sym);
    m->rela = (const struct elf_rela *)(m->dynlib + rela);
    m->nrela = relasz / sizeof(struct elf_rela);
    m->jmprel = (const struct elf_rela *)(m->dynlib + jmprel);
    m->njmprel = pltrelsz / sizeof(struct elf_rela);

    for (u64 i = 0; i < ndyn; i++) {
        u64 tag = dyn[i * 2], val = dyn[i * 2 + 1];
        if (tag == DT_NULL)
            break;
        struct libref ref = {(u16)(val >> 48), str_at(m, val & 0xFFFFFFFF)};
        if (tag == 1 /* DT_NEEDED */ && m->n_needed_files < MAX_LIBS)
            m->needed_files[m->n_needed_files++] = str_at(m, val & 0xFFFFFFFF);
        else if (tag == DT_SCE_MODULE_INFO)
            m->modname = ref.name;
        else if (tag == DT_SCE_NEEDED_MODULE && m->n_needed_mods < MAX_LIBS)
            m->needed_mods[m->n_needed_mods++] = ref;
        else if (tag == DT_SCE_IMPORT_LIB && m->n_import_libs < MAX_LIBS)
            m->import_libs[m->n_import_libs++] = ref;
        else if (tag == DT_SCE_EXPORT_LIB && m->n_export_libs < MAX_LIBS)
            m->export_libs[m->n_export_libs++] = ref;
    }

    u64 lo = ~0ULL, hi = 0;
    for (u32 i = 0; i < m->phnum; i++) {
        struct elf_phdr *p = &m->phdrs[i];
        if (p->type != PT_LOAD && p->type != PT_SCE_RELRO)
            continue;
        if (p->vaddr < lo)
            lo = p->vaddr;
        if (p->vaddr + p->memsz > hi)
            hi = p->vaddr + p->memsz;
        if ((p->flags & 1) && p->type == PT_LOAD)
            m->text_size = ALIGN_UP(p->vaddr + p->memsz, 0x4000);
    }
    m->image_size = ALIGN_UP(hi, 0x4000) - ALIGN_DOWN(lo, 0x4000);
    logf("%s: module %s image %lx text %lx syms %lu rela %lu jmprel %lu tls %lx/%lx", m->name,
         m->modname, m->image_size, m->text_size, m->nsyms, m->nrela, m->njmprel, m->tls_filesz,
         m->tls_memsz);
    return 0;
}

static const char ID_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";

static int decode_id(const char *s, const char *end)
{
    int v = 0;
    for (; s < end && *s; s++) {
        int d = -1;
        for (int i = 0; i < 64; i++)
            if (ID_ALPHABET[i] == *s)
                d = i;
        if (d < 0)
            return -1;
        v = v * 64 + d;
    }
    return v;
}

static const char *lib_name(const struct libref *refs, u32 n, int id)
{
    for (u32 i = 0; i < n; i++)
        if (refs[i].id == id)
            return refs[i].name;
    return NULL;
}

/* Split "NID#L#M" and return the import library name; nid gets 12 bytes. */
const char *module_import_lib(struct module *m, const char *encoded, char *nid, const char **modname)
{
    const char *h1 = encoded;
    while (*h1 && *h1 != '#')
        h1++;
    size_t n = (size_t)(h1 - encoded);
    if (n > 11)
        n = 11;
    memcpy(nid, encoded, n);
    nid[n] = 0;
    if (!*h1)
        return NULL;
    const char *h2 = h1 + 1;
    while (*h2 && *h2 != '#')
        h2++;
    int lib = decode_id(h1 + 1, h2);
    int mod = *h2 ? decode_id(h2 + 1, h2 + 1 + strlen(h2 + 1)) : -1;
    if (modname)
        *modname = lib_name(m->needed_mods, m->n_needed_mods, mod);
    return lib_name(m->import_libs, m->n_import_libs, lib);
}

const char *module_export_lib(struct module *m, const char *encoded)
{
    const char *h1 = encoded;
    while (*h1 && *h1 != '#')
        h1++;
    if (!*h1)
        return NULL;
    const char *h2 = h1 + 1;
    while (*h2 && *h2 != '#')
        h2++;
    return lib_name(m->export_libs, m->n_export_libs, decode_id(h1 + 1, h2));
}

const char *module_sym_name(struct module *m, const struct elf_sym *s)
{
    return str_at(m, s->name);
}
