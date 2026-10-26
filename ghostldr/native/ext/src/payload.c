/*
 * What the ELF loader takes (elfldr.h, docs/elfldr.md): a payload's format,
 * checks, layout, relocation and TLS sites.  No console calls: elfldr_sys.c
 * places the result, and tests/test_elfldr.py runs this file on the
 * computer.
 *
 * Taken:
 *  - an ELF with its own dynamic section (ld.lld's output: OpenOrbis before
 *    create-fself, or anything else position-independent); imports are
 *    resolved by name (the resolver), DT_NEEDED names the libraries;
 *  - create-fself's OELF or a fake-signed SELF: Orbis dynamic tables,
 *    parsed by the core's elf.c as a title's are (pl_parse_sce);
 *  - a freestanding ELF: no imports at all (no dynamic section, or one
 *    with relative relocations only).
 * Refused, with the reason: fixed-address executables (ET_EXEC), objects,
 * libraries, other machines, a segment both writable and executable, data
 * in the code's 16 KiB pages, code over a JIT object's size, ps5-payload-sdk
 * payloads (their startup needs kernel R/W), REL relocations, ifuncs.
 *
 * Payloads are placed at a 16 KiB boundary (p_align above that is not kept).
 * Layout: [0, exec_end) is code and read-only data in a view of one of the
 * emulator's JIT objects (written through the compiler); [exec_end,
 * image_size) is direct memory.  The image is built in a buffer first, so
 * relocations may land anywhere in it (text relocations too).
 */
#include "ghost.h"
#include "elfldr.h"

#define ET_REL 1
#define ET_EXEC 2
#define ET_DYN 3
#define ET_SCE_EXEC 0xFE00
#define ET_SCE_DYNEXEC 0xFE10
#define ET_SCE_DYNAMIC 0xFE18
#define EM_X86_64 62
#define PT_GNU_RELRO 0x6474E552
#define PF_X 1
#define PF_W 2

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_INIT 12
#define DT_FINI 13
#define DT_REL 17
#define DT_PLTREL 20
#define DT_JMPREL 23
#define DT_RELRSZ 35
#define DT_RELR 36

#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define R_X86_64_DTPMOD64 16
#define R_X86_64_DTPOFF64 17
#define R_X86_64_TPOFF64 18
#define R_X86_64_DTPOFF32 21
#define R_X86_64_TPOFF32 23
#define R_X86_64_IRELATIVE 37
#define STB_WEAK 2

#define PAGE 0x4000UL
#define MAX_IMAGE (1UL << 30)
#define MAX_EXEC (32UL << 20) /* the emulator's largest JIT object */

static int fail(char *why, u32 n, const char *f, ...) __attribute__((format(printf, 3, 4)));
static int fail(char *why, u32 n, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    vfmt(why, n, f, ap);
    __builtin_va_end(ap);
    return -1;
}

static const u8 k_self[4] = {0x4f, 0x15, 0x3d, 0x1d};

/* file bytes behind [vaddr, vaddr + len) of a loaded segment, or NULL */
static const u8 *at_vaddr(const struct module *m, u64 vaddr, u64 len)
{
    for (u32 i = 0; i < m->phnum; i++) {
        const struct elf_phdr *p = &m->phdrs[i];
        if (p->type == PT_LOAD && vaddr >= p->vaddr && vaddr - p->vaddr <= p->filesz &&
            len <= p->filesz - (vaddr - p->vaddr))
            return m->seg_data[i] ? m->seg_data[i] + (vaddr - p->vaddr) : NULL;
    }
    return NULL;
}

/* ELF's own dynamic section (no Orbis tables) */
static int parse_gnu(struct module *m, struct pl_info *info, char *why, u32 n)
{
    const struct elf_phdr *dynp = NULL;
    for (u32 i = 0; i < m->phnum; i++) {
        const struct elf_phdr *p = &m->phdrs[i];
        if (p->filesz && (p->offset > m->file_size || p->filesz > m->file_size - p->offset))
            return fail(why, n, "segment %u (file 0x%lx+0x%lx) runs past the end of the file (0x%lx bytes)", i,
                        p->offset, p->filesz, m->file_size);
        m->seg_data[i] = p->filesz ? m->file + p->offset : NULL;
        if (p->type == PT_DYNAMIC)
            dynp = p;
        else if (p->type == PT_TLS)
            m->tls_vaddr = p->vaddr, m->tls_filesz = p->filesz, m->tls_memsz = p->memsz, m->tls_align = p->align;
        else if (p->type == PT_GNU_EH_FRAME)
            m->eh_frame_hdr = p->vaddr, m->eh_frame_hdr_size = p->memsz;
    }
    if (!dynp)
        return 0; /* freestanding, nothing to relocate */
    const u64 *dyn = (const u64 *)(m->file + dynp->offset);
    u64 ndyn = dynp->filesz / 16, strtab = 0, symtab = 0, rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0;
    u64 hash = 0, relr = 0, relrsz = 0, needed[MAX_LIBS];
    u32 nneeded = 0;
    for (u64 i = 0; i < ndyn && dyn[2 * i] != DT_NULL; i++) {
        u64 tag = dyn[2 * i], val = dyn[2 * i + 1];
        switch (tag) {
        case DT_NEEDED:
            if (nneeded < MAX_LIBS)
                needed[nneeded++] = val;
            break;
        case DT_STRTAB: strtab = val; break;
        case DT_STRSZ: m->strsz = val; break;
        case DT_SYMTAB: symtab = val; break;
        case DT_RELA: rela = val; break;
        case DT_RELASZ: relasz = val; break;
        case DT_JMPREL: jmprel = val; break;
        case DT_PLTRELSZ: pltrelsz = val; break;
        case DT_HASH: hash = val; break;
        case DT_RELR: relr = val; break;
        case DT_RELRSZ: relrsz = val; break;
        case DT_INIT: m->init = val, m->has_init = true; break;
        case DT_FINI: m->fini = val; break;
        case DT_REL:
            return fail(why, n, "it has REL relocations; x86-64 payloads use RELA");
        case DT_PLTREL:
            if (val != DT_RELA)
                return fail(why, n, "its PLT relocations are not RELA");
            break;
        case DT_SYMENT:
        case DT_RELAENT:
            if (val != 24)
                return fail(why, n, "dynamic tag %lu is %lu, not 24", tag, val);
            break;
        }
    }
    if (strtab && !(m->strtab = (const char *)at_vaddr(m, strtab, m->strsz)))
        return fail(why, n, "its string table (0x%lx+0x%lx) is not in the file", strtab, m->strsz);
    if (m->strsz && m->strtab[m->strsz - 1])
        return fail(why, n, "its string table does not end in a NUL");
    if (symtab) {
        /* the number of symbols: DT_HASH says; else as many as fit before the
         * string table (ld.lld puts .dynsym first), else up to the segment's end */
        const u32 *h = hash ? (const u32 *)at_vaddr(m, hash, 8) : NULL;
        u64 room = 0;
        for (u32 i = 0; i < m->phnum; i++) {
            const struct elf_phdr *p = &m->phdrs[i];
            if (p->type == PT_LOAD && symtab >= p->vaddr && symtab - p->vaddr < p->filesz)
                room = (p->filesz - (symtab - p->vaddr)) / sizeof(struct elf_sym);
        }
        u64 count = h ? h[1] : strtab > symtab ? (strtab - symtab) / sizeof(struct elf_sym) : room;
        if (count > room)
            count = room;
        m->nsyms = count;
        if (!(m->symtab = (const struct elf_sym *)at_vaddr(m, symtab, count * sizeof(struct elf_sym))))
            return fail(why, n, "its symbol table (0x%lx) is not in the file", symtab);
    }
    if (relasz && !(m->rela = (const struct elf_rela *)at_vaddr(m, rela, relasz)))
        return fail(why, n, "its relocations (0x%lx+0x%lx) are not in the file", rela, relasz);
    if (pltrelsz && !(m->jmprel = (const struct elf_rela *)at_vaddr(m, jmprel, pltrelsz)))
        return fail(why, n, "its PLT relocations (0x%lx+0x%lx) are not in the file", jmprel, pltrelsz);
    if (relrsz && !(info->relr = (const u64 *)at_vaddr(m, relr, relrsz)))
        return fail(why, n, "its packed relocations (0x%lx+0x%lx) are not in the file", relr, relrsz);
    m->nrela = relasz / sizeof(struct elf_rela);
    m->njmprel = pltrelsz / sizeof(struct elf_rela);
    info->nrelr = relrsz / 8;
    for (u32 i = 0; i < nneeded; i++) {
        if (needed[i] >= m->strsz || !m->strtab)
            return fail(why, n, "DT_NEEDED %u names no string", i);
        m->needed_files[m->n_needed_files++] = m->strtab + needed[i];
    }
    return 0;
}

/* Orbis tables: a SELF must be fake-signed (plain segments) */
static int parse_sce(struct module *m, u8 *file, u64 size, char *why, u32 n)
{
    if (memcmp(file, k_self, 4) == 0) {
        u16 count = *(const u16 *)(file + 0x18);
        if (0x20 + (u64)count * 0x20 + 0x40 > size)
            return fail(why, n, "its SELF header (%u entries) runs past the end of the file", count);
        for (u32 i = 0; i < count; i++) {
            const u64 *e = (const u64 *)(file + 0x20 + i * 0x20);
            if ((e[0] & 0x800) && (e[0] & 0xA))
                return fail(why, n, "a SELF with encrypted or compressed segments: only fake-signed ones "
                                    "(create-fself) can be loaded");
            if ((e[0] & 0x800) && (e[1] > size || e[3] > size - e[1]))
                return fail(why, n, "SELF segment %u runs past the end of the file", i);
        }
    }
    if (pl_parse_sce(m, file, size) != 0)
        return fail(why, n, "its Orbis dynamic tables could not be read (the log says why)");
    for (u32 i = 0; i < m->phnum; i++) {
        const struct elf_phdr *p = &m->phdrs[i];
        if ((p->type == PT_LOAD || p->type == PT_SCE_RELRO) && p->filesz && !m->seg_data[i])
            return fail(why, n, "segment %u has no data in the file", i);
    }
    return 0;
}

/* where everything goes, and the checks that need the whole picture */
static int layout(struct module *m, struct pl_info *info, char *why, u32 n)
{
    u64 exec_hi = 0, data_lo = ~0UL, hi = 0, relro_lo = 0, relro_hi = 0;
    for (u32 i = 0; i < m->n_needed_files; i++)
        if (strncmp(m->needed_files[i], "libkernel_web", 13) == 0 || strncmp(m->needed_files[i], "libkernel_sys", 13) == 0)
            return fail(why, n, "built with ps5-payload-sdk (it needs %s): its startup needs kernel R/W, which this "
                                "loader does not have; build it for the PS4 ABI (OpenOrbis) or freestanding",
                        m->needed_files[i]);
    for (u32 i = 0; i < m->phnum; i++) {
        const struct elf_phdr *p = &m->phdrs[i];
        bool relro = p->type == PT_SCE_RELRO || p->type == PT_GNU_RELRO;
        if (relro && p->memsz)
            relro_lo = p->vaddr, relro_hi = p->vaddr + p->memsz;
        if ((p->type != PT_LOAD && p->type != PT_SCE_RELRO) || !p->memsz)
            continue;
        if (p->filesz > p->memsz || p->vaddr >= MAX_IMAGE || p->memsz > MAX_IMAGE - p->vaddr)
            return fail(why, n, "segment %u (0x%lx+0x%lx) is malformed or larger than 1 GiB", i, p->vaddr, p->memsz);
        if ((p->flags & PF_X) && (p->flags & PF_W))
            return fail(why, n, "segment %u is both writable and executable: SWRR maps no such memory (keep code "
                                "read-only: ld.lld's default layout, not ps5-payload-sdk's RWE one)", i);
        u64 end = p->vaddr + p->memsz;
        if (p->flags & PF_X)
            exec_hi = end > exec_hi ? end : exec_hi;
        if (((p->flags & PF_W) || p->type == PT_SCE_RELRO) && p->vaddr < data_lo)
            data_lo = p->vaddr;
        hi = end > hi ? end : hi;
    }
    if (!exec_hi)
        return fail(why, n, "it has no executable segment");
    info->exec_end = ALIGN_UP(exec_hi, PAGE);
    info->image_size = ALIGN_UP(hi, PAGE);
    if (info->exec_end > MAX_EXEC - PL_HEADER)
        return fail(why, n, "%lu KiB of code and read-only data: at most %lu KiB fit one JIT object",
                    info->exec_end >> 10, (MAX_EXEC - PL_HEADER) >> 10);
    if (data_lo < info->exec_end)
        return fail(why, n, "writable data at 0x%lx shares the code's 16 KiB pages (code ends at 0x%lx): link "
                            "with 16 KiB pages (-z max-page-size=0x4000 -z separate-code)", data_lo, exec_hi);
    bool entry_ok = false;
    for (u32 i = 0; i < m->phnum; i++) {
        const struct elf_phdr *p = &m->phdrs[i];
        entry_ok |= p->type == PT_LOAD && (p->flags & PF_X) && m->entry >= p->vaddr && m->entry < p->vaddr + p->memsz;
    }
    if (!entry_ok)
        return fail(why, n, "its entry point 0x%lx is not in an executable segment", m->entry);
    if (m->tls_memsz > PL_TLS_MAX || m->tls_align > PL_TLS_ALIGN || m->tls_filesz > m->tls_memsz ||
        (m->tls_filesz && (m->tls_vaddr >= info->image_size || m->tls_filesz > info->image_size - m->tls_vaddr)))
        return fail(why, n, "its TLS (0x%lx bytes, aligned 0x%lx) is larger than the 0x%x bytes (aligned 0x%x) "
                            "each thread has, or malformed", m->tls_memsz, m->tls_align, PL_TLS_MAX, PL_TLS_ALIGN);
    if (relro_hi) {
        info->relro_lo = ALIGN_UP(relro_lo, PAGE);
        info->relro_hi = ALIGN_DOWN(relro_hi, PAGE);
        if (info->relro_lo < info->exec_end || info->relro_hi <= info->relro_lo)
            info->relro_lo = info->relro_hi = 0;
    }
    info->abi = info->sce || m->n_needed_files ? PL_ORBIS : PL_FREE;
    m->text_size = info->exec_end;
    m->image_size = info->image_size;
    return 0;
}

int pl_parse(struct module *m, u8 *file, u64 size, struct pl_info *info, char *why, u32 n)
{
    memset(info, 0, sizeof(*info));
    why[0] = 0;
    if (size < 64)
        return fail(why, n, "only %lu bytes: not an ELF", size);
    if (memcmp(file, k_self, 4) == 0) {
        info->sce = true;
        if (parse_sce(m, file, size, why, n) != 0)
            return -1;
        return layout(m, info, why, n);
    }
    if (file[0] != 0x7f || file[1] != 'E' || file[2] != 'L' || file[3] != 'F')
        return fail(why, n, "not an ELF (it starts %02x %02x %02x %02x)", file[0], file[1], file[2], file[3]);
    if (file[4] != 2 || file[5] != 1 || *(const u16 *)(file + 0x12) != EM_X86_64)
        return fail(why, n, "not a 64-bit little-endian x86-64 ELF (class %u, data %u, machine %u)", file[4], file[5],
                    *(const u16 *)(file + 0x12));
    u16 type = *(const u16 *)(file + 0x10);
    if (type == ET_EXEC || type == ET_SCE_EXEC)
        return fail(why, n, "linked at a fixed address (ET_EXEC): SWRR has executable memory only where this "
                            "loader maps it; link it position-independent (-pie, or -static-pie)");
    if (type == ET_REL)
        return fail(why, n, "an object file (ET_REL): link it first");
    if (type == ET_SCE_DYNAMIC)
        return fail(why, n, "a library (an Orbis prx), not a program");
    if (type != ET_DYN && type != ET_SCE_DYNEXEC)
        return fail(why, n, "ELF type 0x%x is not a program this loader runs", type);
    u64 phoff = *(const u64 *)(file + 0x20);
    u16 phent = *(const u16 *)(file + 0x36), phnum = *(const u16 *)(file + 0x38);
    if (phent != sizeof(struct elf_phdr) || !phnum || phnum > 32 || phoff > size ||
        (u64)phnum * phent > size - phoff)
        return fail(why, n, "its program headers (%u at 0x%lx) are malformed", phnum, phoff);
    const struct elf_phdr *ph = (const struct elf_phdr *)(file + phoff);
    for (u32 i = 0; i < phnum; i++)
        info->sce |= ph[i].type == PT_SCE_DYNLIBDATA;
    if (info->sce) {
        if (parse_sce(m, file, size, why, n) != 0)
            return -1;
    } else {
        m->file = file, m->file_size = size;
        m->entry = *(const u64 *)(file + 0x18);
        m->phdrs = (struct elf_phdr *)ph, m->phnum = phnum;
        if (parse_gnu(m, info, why, n) != 0)
            return -1;
    }
    return layout(m, info, why, n);
}

/* ---- building the image ---- */

static u64 sym_value(struct module *m, u32 index, bool plt, pl_resolver resolve, void *ctx, bool *bad)
{
    if (index == 0)
        return 0;
    if (index >= m->nsyms || !m->symtab) {
        *bad = true;
        return 0;
    }
    const struct elf_sym *s = &m->symtab[index];
    if (s->shndx)
        return m->base + s->value;
    if (s->name >= m->strsz) {
        *bad = true;
        return 0;
    }
    u32 type = s->info & 0xf; /* STT_NOTYPE 0, STT_OBJECT 1, STT_FUNC 2 */
    int kind = type == 1 || (type == 0 && !plt) ? IMPORT_OBJECT : plt && type == 2 ? IMPORT_PLT : IMPORT_DATA;
    return resolve(ctx, m, m->strtab + s->name, (s->info >> 4) == STB_WEAK, kind);
}

static int apply(struct module *m, const struct pl_info *info, u8 *img, const struct elf_rela *r, u64 count,
                 pl_resolver resolve, void *ctx, char *why, u32 n)
{
    for (u64 i = 0; i < count; i++) {
        u32 type = (u32)r[i].info, sym = (u32)(r[i].info >> 32);
        u64 off = r[i].offset;
        bool bad = false, wide = type != R_X86_64_PC32 && type != R_X86_64_DTPOFF32 && type != R_X86_64_TPOFF32;
        if (type == R_X86_64_NONE)
            continue;
        if (off > info->image_size || (wide ? 8 : 4) > info->image_size - off)
            return fail(why, n, "relocation %lu writes at 0x%lx, outside the image (0x%lx bytes)", i, off,
                        info->image_size);
        u8 *where = img + off;
        u64 v;
        switch (type) {
        case R_X86_64_RELATIVE:
            v = m->base + (u64)r[i].addend;
            break;
        case R_X86_64_64:
            v = sym_value(m, sym, false, resolve, ctx, &bad) + (u64)r[i].addend;
            break;
        case R_X86_64_PC32:
            v = sym_value(m, sym, false, resolve, ctx, &bad) + (u64)r[i].addend - (m->base + off);
            break;
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
            v = sym_value(m, sym, type == R_X86_64_JUMP_SLOT, resolve, ctx, &bad);
            break;
        case R_X86_64_DTPMOD64:
        case R_X86_64_DTPOFF64:
        case R_X86_64_DTPOFF32:
        case R_X86_64_TPOFF64:
        case R_X86_64_TPOFF32: {
            /* the payload's own thread-local variables: static TLS (elfldr_sys.c) */
            const struct elf_sym *s = sym && sym < m->nsyms && m->symtab ? &m->symtab[sym] : NULL;
            if (sym && (!s || !s->shndx))
                return fail(why, n, "relocation %lu (type %u) refers to another module's thread-local variable",
                            i, type);
            if (!m->tls_memsz || (!m->tls_offset && (type == R_X86_64_TPOFF64 || type == R_X86_64_TPOFF32)))
                return fail(why, n, "relocation %lu (type %u) at 0x%lx without TLS", i, type, off);
            u64 value = (s ? s->value : 0) + (u64)r[i].addend;
            v = type == R_X86_64_DTPMOD64                             ? m->tls_id
                : type == R_X86_64_TPOFF64 || type == R_X86_64_TPOFF32 ? value - m->tls_offset
                                                                       : value;
            break;
        }
        case R_X86_64_IRELATIVE:
            return fail(why, n, "relocation %lu is an ifunc (IRELATIVE): its resolver would have to run first", i);
        default:
            return fail(why, n, "relocation %lu at 0x%lx has type %u, which this loader does not apply", i, off, type);
        }
        if (bad)
            return fail(why, n, "relocation %lu at 0x%lx refers to symbol %u, which is not in its table", i, off, sym);
        if (wide)
            memcpy(where, &v, 8);
        else {
            u32 lo = (u32)v;
            memcpy(where, &lo, 4);
        }
    }
    return 0;
}

/* DT_RELR: a word address, then bitmaps of the 63 words after it */
static int apply_relr(struct module *m, const struct pl_info *info, u8 *img, char *why, u32 n)
{
    u64 where = 0;
    for (u64 i = 0; i < info->nrelr; i++) {
        u64 e = info->relr[i];
        u64 count = (e & 1) ? 63 : 1, at = (e & 1) ? where : e;
        for (u64 k = 0; k < count; k++) {
            if ((e & 1) && !((e >> (k + 1)) & 1))
                continue;
            u64 off = at + k * 8;
            if (off > info->image_size - 8)
                return fail(why, n, "a packed relocation writes at 0x%lx, outside the image", off);
            u64 v;
            memcpy(&v, img + off, 8);
            v += m->base;
            memcpy(img + off, &v, 8);
        }
        where = (e & 1) ? where + 63 * 8 : e + 8;
    }
    return 0;
}

/* Each thread-pointer load "mov/add r64, fs:[0]" reads fs:[-8] instead: the
 * thread's block (tls.c), as loader.c does for a title's executable. */
static bool tls_form(const u8 *c)
{
    return c[0] == 0x64 && (c[1] == 0x48 || c[1] == 0x4C) && (c[2] == 0x8B || c[2] == 0x03) && (c[3] & 0xC7) == 0x04 &&
           c[4] == 0x25;
}

static u32 patch_tls(u8 *code, u64 size)
{
    u32 k = 0;
    for (u64 i = 0; i + 9 <= size; i++)
        if (tls_form(code + i) && *(u32 *)(code + i + 5) == 0) {
            *(u32 *)(code + i + 5) = 0xFFFFFFF8u;
            k++;
            i += 8;
        }
    return k;
}

int pl_build(struct module *m, const struct pl_info *info, u8 *img, pl_resolver resolve, void *ctx, char *why,
             u32 n)
{
    why[0] = 0;
    for (u32 i = 0; i < m->phnum; i++) {
        const struct elf_phdr *p = &m->phdrs[i];
        if ((p->type == PT_LOAD || p->type == PT_SCE_RELRO) && p->filesz && m->seg_data[i])
            memcpy(img + p->vaddr, m->seg_data[i], p->filesz);
    }
    if (apply(m, info, img, m->rela, m->nrela, resolve, ctx, why, n) != 0 ||
        apply(m, info, img, m->jmprel, m->njmprel, resolve, ctx, why, n) != 0 ||
        apply_relr(m, info, img, why, n) != 0)
        return -1;
    if (m->tls_offset) {
        u32 k = patch_tls(img, info->exec_end);
        fmt(why, n, "%u TLS sites", k);
    }
    return 0;
}
