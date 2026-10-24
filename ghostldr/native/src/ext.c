/*
 * The runtime extension: build/ghostldr/ext.bin (ghostldr/native/ext and
 * tools/mkext.py; format and API in ghost_api.h).
 *
 * run.py serves the blob over TCP; ghostldr.lua connects a socket to it and
 * passes the socket, the blob's size and its CRC-32 in the boot context.
 *
 *   ext_fetch      first thing in ghost_boot: read the blob into direct
 *                  memory and verify it.  The core's only NID -> name table
 *                  is in it, so without it the boot stops here, before the
 *                  title or SWRR's state is touched.
 *   ext_start      after the title's modules are placed, before relocation:
 *                  put the code in executable memory, relocate it, call
 *                  ext_init(&g_ghost_api) and run its self-test.
 *   ext_boot_done  after relocation: free the blob (NID table and image
 *                  copy).  The extension's code and data stay.
 *
 * Placement: the code goes at the top of the small RX area after the core
 * (arena.c), written through the compiler like module text, and its data
 * pages right after it are punched RW like module data.  When that area
 * cannot hold it (keeping room for stubs), or with ctx flag
 * CTX_FLAG_EXT_JIT, a new JIT block from the compiler process holds it.
 * Placing it after the title's modules means a kernel-chosen JIT address
 * cannot fall where module placement later punches memory.
 */
#include "ghost_api.h"

extern const struct ghost_api g_ghost_api;

#define EXT_MAX_BLOB (64UL << 20)
#define EXT_MAX_IMAGE (16UL << 20)
#define STUB_AREA_KEEP 0x4000 /* small RX area kept for stubs and hook trampolines (resolve.c) */

static u8 *g_blob;
static u64 g_blob_size;
static bool g_blob_flex;
static const struct ext_blob *g_hdr;
static const u64 *g_nid_keys;
static const u32 *g_nid_names;
static const char *g_nid_str;
static u32 g_nid_count;
static const struct ext_exports *g_exp;
static u32 g_ext_hits;

static u32 crc32(const u8 *p, u64 n)
{
    u32 table[256];
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    u32 crc = 0xFFFFFFFFu;
    while (n--)
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* A NID's 11 base64 digits as the 64-bit value they encode (the last digit
 * carries 4 bits); -1 for anything that is not a canonical NID. */
static int nid_key(const char *nid, u64 *key)
{
    u64 v = 0;
    for (int i = 0; i < 11; i++) {
        char c = nid[i];
        int d = c >= 'A' && c <= 'Z'   ? c - 'A'
                : c >= 'a' && c <= 'z' ? c - 'a' + 26
                : c >= '0' && c <= '9' ? c - '0' + 52
                : c == '+'             ? 62
                : c == '-'             ? 63
                                       : -1;
        if (d < 0)
            return -1;
        if (i < 10)
            v = v << 6 | (u64)d;
        else if (d & 3)
            return -1;
        else
            v = v << 4 | (u64)(d >> 2);
    }
    if (nid[11])
        return -1;
    *key = v;
    return 0;
}

/* Names kept past ext_boot_done for modules loaded at run time (nid_keep):
 * the imports of the title's sce_module files the boot did not load. */
static u64 *g_kept;
static const char **g_kept_name;
static u32 g_nkept, g_kept_cap;

const char *nid_to_name(const char *nid)
{
    u64 key;
    if (nid_key(nid, &key) != 0)
        return NULL;
    if (!g_nid_keys) {
        for (u32 i = 0; i < g_nkept; i++)
            if (g_kept[i] == key)
                return g_kept_name[i];
        return NULL;
    }
    u32 lo = 0, hi = g_nid_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (g_nid_keys[mid] == key)
            return g_nid_str + g_nid_names[mid];
        if (g_nid_keys[mid] < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

__attribute__((minsize, cold)) void nid_keep(const char *nid)
{
    u64 key;
    const char *name = g_nid_keys ? nid_to_name(nid) : NULL;
    if (!name || nid_key(nid, &key) != 0)
        return;
    for (u32 i = 0; i < g_nkept; i++)
        if (g_kept[i] == key)
            return;
    if (g_nkept == g_kept_cap) {
        u32 cap = g_kept_cap ? g_kept_cap * 2 : 256;
        u64 *k = arena_alloc(cap * sizeof(*k));
        const char **n = arena_alloc(cap * sizeof(*n));
        if (!k || !n)
            return;
        memcpy(k, g_kept, g_nkept * sizeof(*k));
        memcpy(n, g_kept_name, g_nkept * sizeof(*n));
        g_kept = k, g_kept_name = n, g_kept_cap = cap;
    }
    size_t len = strlen(name) + 1;
    char *copy = arena_alloc(len);
    if (!copy)
        return;
    memcpy(copy, name, len);
    g_kept_name[g_nkept] = copy;
    g_kept[g_nkept++] = key;
}

/* The blob lives only until the imports are resolved.  It goes in app
 * flexible memory, which the title never gets (hle.c maps its flexible
 * requests to direct memory), so the direct memory the loader and the memory
 * plan see is laid out exactly as without it; direct memory only if that fails. */
static u8 *blob_alloc(u64 size)
{
    void *p = NULL;
    if (K.map_flex && K.map_flex(&p, ALIGN_UP(size, 0x4000), PROT_READ | PROT_WRITE, 0) == 0 && p) {
        g_blob_flex = true;
        return p;
    }
    g_blob_flex = false;
    return page_alloc(size);
}

static void ext_release(void)
{
    if (g_blob && g_blob_flex)
        K.munmap(g_blob, ALIGN_UP(g_blob_size, 0x4000));
    else if (g_blob)
        page_free(g_blob, g_blob_size);
    g_blob = NULL;
    g_hdr = NULL;
    g_nid_keys = NULL, g_nid_names = NULL, g_nid_str = NULL, g_nid_count = 0;
}

static bool in_blob(u64 off, u64 len) { return off <= g_blob_size && len <= g_blob_size - off; }

static int ext_validate(void)
{
    const struct ext_blob *h = (const struct ext_blob *)g_blob;
    if (h->magic != EXT_BLOB_MAGIC || h->format != EXT_BLOB_FORMAT || h->header_size < sizeof(*h)) {
        logf("ext: not an extension blob (magic %lx format %u)", h->magic, h->format);
        return -1;
    }
    if (h->api_version != GHOST_API_VERSION || h->abi != GHOST_ABI) {
        logf("ext: built for API %u/%08x, this core is %u/%08x: rebuild both (make -C ghostldr/native)",
             h->api_version, h->abi, GHOST_API_VERSION, GHOST_ABI);
        return -1;
    }
    if (h->blob_size != g_blob_size || !in_blob(h->image_off, h->image_filesz) || h->image_filesz > h->image_memsz ||
        h->image_memsz > EXT_MAX_IMAGE || (h->image_memsz & 0x3FFF) || (h->text_size & 0x3FFF) || !h->text_size ||
        h->text_size >= h->image_memsz || h->entry >= h->text_size || (h->reloc_off & 3) ||
        !in_blob(h->reloc_off, (u64)h->reloc_count * sizeof(struct ext_reloc))) {
        logf("ext: bad image layout (blob %lx image %x+%x/%x text %x entry %x relocs %x+%u)", h->blob_size,
             h->image_off, h->image_filesz, h->image_memsz, h->text_size, h->entry, h->reloc_off, h->reloc_count);
        return -1;
    }
    const struct ext_reloc *r = (const struct ext_reloc *)(g_blob + h->reloc_off);
    for (u32 i = 0; i < h->reloc_count; i++)
        if ((u64)r[i].offset + 8 > h->image_memsz || r[i].addend >= h->image_memsz) {
            logf("ext: relocation %u (%x -> %x) outside the image", i, r[i].offset, r[i].addend);
            return -1;
        }
    if ((h->nid_keys_off & 7) || (h->nid_names_off & 3) || !in_blob(h->nid_keys_off, (u64)h->nid_count * 8) ||
        !in_blob(h->nid_names_off, (u64)h->nid_count * 4) || !in_blob(h->str_off, h->str_size) || !h->str_size ||
        g_blob[h->str_off + h->str_size - 1] != 0) {
        logf("ext: bad NID table layout (%u NIDs at %x/%x, names %x+%x)", h->nid_count, h->nid_keys_off,
             h->nid_names_off, h->str_off, h->str_size);
        return -1;
    }
    const u64 *keys = (const u64 *)(g_blob + h->nid_keys_off);
    const u32 *names = (const u32 *)(g_blob + h->nid_names_off);
    for (u32 i = 0; i < h->nid_count; i++)
        if (names[i] >= h->str_size || (i && keys[i] <= keys[i - 1])) {
            logf("ext: NID table entry %u is out of order or has a bad name offset", i);
            return -1;
        }
    g_hdr = h;
    g_nid_keys = keys, g_nid_names = names, g_nid_str = (const char *)(g_blob + h->str_off);
    g_nid_count = h->nid_count;
    return 0;
}

int ext_fetch(void)
{
    struct boot_ctx *c = g_ctx;
    bool have = c->size >= __builtin_offsetof(struct boot_ctx, ext_crc) + 8;
    int fd = have ? (int)(s64)c->ext_fd : -1;
    u64 size = have ? c->ext_size : 0;
    if (fd < 0 || !size) {
        logf("ext: no extension from the computer (socket %d, %lu bytes). build/ghostldr/ext.bin holds the "
             "NID table, so the boot stops here, before the title is touched: run.py serves it over TCP",
             fd, size);
        return -1;
    }
    c->ext_fd = (u64)-1; /* consumed */
    if (size < sizeof(struct ext_blob) || size > EXT_MAX_BLOB) {
        logf("ext: implausible size %lu", size);
        K.close(fd);
        return -1;
    }
    g_blob_size = size;
    g_blob = blob_alloc(size);
    if (!g_blob) {
        logf("ext: no memory for %lu bytes", size);
        K.close(fd);
        return -1;
    }
    u64 got = 0;
    while (got < size) {
        u64 want = size - got < 0x100000 ? size - got : 0x100000;
        ssize_t n = K.read(fd, g_blob + got, want);
        if (n <= 0 || (u64)n > want) {
            logf("ext: receive failed after %lu of %lu bytes (%lx)", got, size, (u64)n);
            K.close(fd);
            ext_release();
            return -1;
        }
        got += (u64)n;
    }
    K.close(fd);
    u32 crc = crc32(g_blob, size);
    if (crc != (u32)c->ext_crc) {
        logf("ext: CRC-32 %08x, expected %08x: the blob is corrupt", crc, (u32)c->ext_crc);
        ext_release();
        return -1;
    }
    if (ext_validate() != 0) {
        ext_release();
        return -1;
    }
    logf("ext: received %lu bytes (CRC %08x) into %s memory: code %x bytes (text %x), %u relocations, %u NIDs",
         size, crc, g_blob_flex ? "flexible" : "direct", g_hdr->image_memsz, g_hdr->text_size, g_hdr->reloc_count,
         g_nid_count);
    return 0;
}

u64 ext_hle_lookup(const char *lib, const char *nid, const char *name)
{
    u64 fn = g_exp ? g_exp->lookup(lib, nid) : 0;
    if (fn && g_ext_hits++ < 64)
        logf("ext: %s:%s -> extension %lx", lib ? lib : "?", name ? name : nid, fn);
    return fn;
}

const struct ext_lib *ext_lib_policy(const char *lib)
{
    return g_exp && lib ? g_exp->lib_policy(lib) : NULL;
}

/* A sysmodule load the title asked for failed (hle.c): families that keep a
 * library's mode switch it to their virtual implementation. */
void ext_note_sysmodule(u32 id, s32 rc)
{
    if (g_exp && g_exp->size >= __builtin_offsetof(struct ext_exports, sysmodule_failed) + sizeof(void *) &&
        g_exp->sysmodule_failed)
        g_exp->sysmodule_failed(id, rc);
}

static int ext_selftest(void)
{
    char nid[12];
    compute_nid(EXT_SELFTEST_NAME, nid);
    u64 (*fn)(u64) = (u64 (*)(u64))g_exp->lookup(EXT_SELFTEST_LIB, nid);
    if (!fn) {
        logf("ext: the extension has no self-test");
        return 0;
    }
    const struct ext_lib *pol = g_exp->lib_policy(EXT_SELFTEST_ALIAS);
    bool found = g_exp->lookup(EXT_SELFTEST_ALIAS, nid) == (u64)fn && g_exp->lookup(NULL, nid) == (u64)fn &&
                 g_exp->lookup(EXT_SELFTEST_LIB, EXT_SELFTEST_NID) == (u64)fn && !g_exp->lookup("libkernel", nid) &&
                 pol && (pol->flags & EXT_LIB_NO_HOST) && pol->stub_ret == EXT_SELFTEST_STUB_RET;
    u64 a = fn(0x1234), b = fn(0x5678);
    bool ok = found && a == (EXT_SELFTEST_OK ^ 0x1234 ^ (1ULL << 32)) && b == (EXT_SELFTEST_OK ^ 0x5678 ^ (2ULL << 32));
    logf("ext: self-test %s (lookups %s, %lx %lx)", ok ? "ok" : "FAILED", found ? "ok" : "wrong", a, b);
    return ok ? 0 : -1;
}

/* Put the code in executable memory and relocate it, once: at ext_start, or
 * earlier for an app root the extension decides (before arena_init, so in a
 * new JIT block). */
static u64 g_base;
static struct jit_block g_jb; /* that JIT block, when the code is in one */
static bool g_in_jit;

static int ext_place(void)
{
    const struct ext_blob *h = g_hdr;
    if (g_base)
        return 0;
    if (!h)
        return -1;
    u64 total = h->image_memsz, data_size = total - h->text_size;
    struct jit_block jb;
    bool want_jit = (g_ctx->flags & CTX_FLAG_EXT_JIT) != 0, jit = false;
    u64 base = want_jit ? 0 : arena_small_alloc_top(total, STUB_AREA_KEEP);
    if (!base) {
        if (jit_block_create(&jb, total, 0) == 0)
            base = jb.rx, jit = true;
        else if (want_jit && (base = arena_small_alloc_top(total, STUB_AREA_KEEP)))
            logf("ext: no new JIT block; using the small RX area");
        if (!base) {
            logf("ext: no executable memory for %lx bytes", total);
            return -1;
        }
    }
    u8 *img = page_alloc(total);
    if (!img)
        return -1;
    memcpy(img, g_blob + h->image_off, h->image_filesz);
    const struct ext_reloc *r = (const struct ext_reloc *)(g_blob + h->reloc_off);
    for (u32 i = 0; i < h->reloc_count; i++)
        *(u64 *)(img + r[i].offset) = base + r[i].addend;
    int rc = jit ? jit_block_write(&jb, 0, img, h->text_size) : arena_write(base, img, h->text_size);
    if (rc == 0)
        rc = arena_punch_rw(base + h->text_size, data_size);
    if (rc == 0) {
        memcpy((void *)(base + h->text_size), img + h->text_size, data_size);
        if (memcmp((const void *)base, img, h->text_size) != 0)
            rc = -2;
    }
    page_free(img, total);
    logf("ext: code at %lx in %s: text %x, data %lx -> %d", base, jit ? "a new JIT block" : "the small RX area",
         h->text_size, data_size, rc);
    if (rc != 0)
        return -1;
    g_base = base;
    if (jit)
        g_jb = jb, g_in_jit = true;
    return 0;
}

/* App roots on USB drives and folders of titles (ext/src/approot.c,
 * docs/usb.md), before the title's modules load: the image is placed now
 * (a new JIT block: the arena is not set up yet); ext_init still runs at
 * ext_start. */
static const struct ext_early *g_early;

int ext_approot_init(const char **root)
{
    const struct ext_blob *h = g_hdr;
    if (!h || !(h->flags & EXT_BLOB_EARLY) || h->early_entry >= h->text_size) {
        logf("ext: the extension has no early entry: app root %s needs one", *root);
        return -1;
    }
    if (ext_place() != 0)
        return -1;
    g_early = ((ext_early_fn)(g_base + h->early_entry))(&g_ghost_api);
    if (!g_early || g_early->magic != EXT_EARLY_MAGIC || g_early->size < sizeof(*g_early)) {
        logf("ext: ext_early failed (%p)", g_early);
        return -1;
    }
    return g_early->approot_init(root);
}

int ext_approot_pick(const char *title, const char **root)
{
    return g_early ? g_early->approot_pick(title, root) : -1;
}

/* A boot that stops before the title loads (an app root that failed): the
 * code's JIT block goes back, since a 13.60 PS5 has only about 768 KiB of
 * new JIT memory for a whole SWRR session and the next boot needs it, and
 * the blob with it. */
void ext_abandon(void)
{
    if (g_in_jit)
        jit_block_destroy(&g_jb);
    logf("ext: released%s", g_in_jit ? " (and its JIT block)" : "");
    g_in_jit = false;
    g_base = 0;
    g_early = NULL;
    ext_release();
}

int ext_start(void)
{
    const struct ext_blob *h = g_hdr;
    if (ext_place() != 0)
        return -1;
    u64 base = g_base;
    const struct ext_exports *ex = ((ext_init_fn)(base + h->entry))(&g_ghost_api);
    if (!ex || ex->magic != EXT_EXPORTS_MAGIC || ex->size < sizeof(*ex) || !ex->lookup || !ex->lib_policy) {
        logf("ext: ext_init failed (%p)", ex);
        return -1;
    }
    g_exp = ex;
    logf("ext: %u HLE functions and %u library policies from %u families", ex->nhle, ex->nlibs, ex->nfamilies);
    for (u32 i = 0; i < ex->nhle && i < 32; i++)
        logf("ext:   %s:%s (%s)", ex->hle[i].lib, ex->hle[i].name ? ex->hle[i].name : "?", ex->hle[i].nid);
    if (ext_selftest() != 0) {
        g_exp = NULL;
        return -1;
    }
    return 0;
}

void ext_boot_done(void)
{
    logf("ext: %u imports bound to the extension; NID table freed (%lu KiB), %u names kept for run-time loads",
         g_ext_hits, g_blob_size >> 10, g_nkept);
    ext_release();
}
