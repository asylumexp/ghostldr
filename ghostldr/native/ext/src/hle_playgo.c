/*
 * Where the title's content is: libScePlayGo (+ libSceDbgPlayGo) and
 * libSceDiscMap, all answered here.  SWRR's PlayGo would describe SWRR's
 * package (it has none); a ghostldr title is never on a disc.  Every chunk is
 * local (LOCAL_FAST) with nothing left to download.
 * Spec: research/hle-specs/system-appcontent.md sections 7 and 8.
 */
#include "sys_title.h"

#define PG_UNKNOWN ((s32)0x80B20001)
#define PG_INVALID_ARGUMENT ((s32)0x80B20004)
#define PG_BAD_POINTER ((s32)0x80B2000A)
#define PG_BAD_SIZE ((s32)0x80B2000B)
#define PG_BAD_LOCUS ((s32)0x80B20010)
#define LOCUS_NOT_DOWNLOADED 0
#define LOCUS_LOCAL_SLOW 2
#define LOCUS_LOCAL_FAST 3
#define SPEED_FULL 2
#define CHUNK_MAX 1000
#define PLGO_MAX (4u << 20)

/* ---- the title's chunks: sce_sys/playgo-chunk.dat, else just chunk 0 ---- */

static volatile int g_pg_lock;
static volatile bool g_pg_loaded;
static u32 g_chunks = 1;
static u64 *g_chunk_bytes; /* per chunk, when the file gives them */
static u64 g_lang_mask;
static s32 g_speed = SPEED_FULL;

static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

/* Header: magic "plgo" @0, chunk count u16 @0x0A, file size u32 @0x10, then
 * {u32 offset, length} tables @0xC0 (chunk attributes, 32 bytes each), @0xC8
 * (chunk -> mchunk id lists, u16) and @0xD8 (mchunk attributes, 16 bytes:
 * location, then a 48-bit size).  A chunk attribute holds its mchunk count
 * u16 @0x0E and the byte offset of its id list u32 @0x18. */
static bool chunk_sizes(const u8 *f, u32 size, u32 n)
{
    u32 attrs = rd32(f + 0xC0), attrs_len = rd32(f + 0xC4), lists = rd32(f + 0xC8), lists_len = rd32(f + 0xCC);
    u32 mattrs = rd32(f + 0xD8), mattrs_len = rd32(f + 0xDC);
    if (attrs > size || attrs_len > size - attrs || attrs_len < 32 * n || lists > size || lists_len > size - lists ||
        mattrs > size || mattrs_len > size - mattrs)
        return false;
    u64 *bytes = arena_alloc(n * sizeof(u64));
    if (!bytes)
        return false;
    memset(bytes, 0, n * sizeof(u64));
    for (u32 i = 0; i < n; i++) {
        const u8 *a = f + attrs + 32 * i;
        u32 cnt = rd16(a + 0x0E), at = rd32(a + 0x18);
        if (at > lists_len || cnt > (lists_len - at) / 2)
            return false;
        for (u32 j = 0; j < cnt; j++) {
            u32 m = rd16(f + lists + at + 2 * j);
            if (16 * (u64)m + 16 > mattrs_len)
                return false;
            const u8 *ms = f + mattrs + 16 * m + 8;
            bytes[i] += (u64)rd32(ms) | (u64)rd16(ms + 4) << 32;
        }
    }
    g_chunk_bytes = bytes;
    return true;
}

static void playgo_load(void)
{
    if (__atomic_load_n(&g_pg_loaded, __ATOMIC_ACQUIRE))
        return;
    spin_lock(&g_pg_lock);
    if (!g_pg_loaded) {
        char buf[256];
        int fd = vfs_open(remap_path("/app0/sce_sys/playgo-chunk.dat", buf, sizeof(buf)), O_RDONLY, 0);
        s64 size = fd >= 0 ? vfs_lseek(fd, 0, 2) : -1;
        u8 *f = size >= 0x100 && size <= PLGO_MAX ? page_alloc((u64)size) : NULL;
        if (f && vfs_lseek(fd, 0, 0) == 0 && vfs_read(fd, f, (u64)size) == size && rd32(f) == 0x6F676C70) {
            u32 n = rd16(f + 0x0A);
            if (n && n <= CHUNK_MAX) {
                g_chunks = n;
                if (!chunk_sizes(f, (u32)size, n))
                    logf("ext: playgo-chunk.dat: chunk sizes unreadable");
            }
        }
        if (f)
            page_free(f, (u64)size);
        if (fd >= 0)
            vfs_close(fd);
        s32 lang = system_lang();
        g_lang_mask = lang >= 0 && lang < 48 ? 1ULL << (63 - lang) : 0;
        logf("ext: PlayGo: %u chunks%s, all local; language mask %lx", g_chunks,
             fd < 0 ? " (no playgo-chunk.dat)" : "", g_lang_mask);
        __atomic_store_n(&g_pg_loaded, true, __ATOMIC_RELEASE);
    }
    spin_unlock(&g_pg_lock);
}

/* Handles are always 1; any handle is accepted. */
static s32 hle_scePlayGoInitialize(const u64 *param)
{
    if (!param || !param[0])
        return PG_BAD_POINTER;
    playgo_load();
    return 0;
}

static s32 hle_scePlayGoOpen(u32 *handle, const void *param)
{
    if (!handle)
        return PG_BAD_POINTER;
    playgo_load();
    *handle = 1;
    return 0;
}

static s32 hle_scePlayGoGetChunkId(u32 h, u16 *ids, u32 n, u32 *out)
{
    if (!out)
        return PG_BAD_POINTER;
    if (ids && !n)
        return PG_BAD_SIZE;
    playgo_load();
    u32 got = ids ? (n < g_chunks ? n : g_chunks) : g_chunks;
    for (u32 i = 0; ids && i < got; i++)
        ids[i] = (u16)i;
    *out = got;
    return 0;
}

static s32 hle_scePlayGoGetLocus(u32 h, const u16 *ids, u32 n, s8 *loci)
{
    if (!ids || !loci)
        return PG_BAD_POINTER;
    if (!n)
        return PG_BAD_SIZE;
    for (u32 i = 0; i < n; i++)
        loci[i] = LOCUS_LOCAL_FAST;
    return 0;
}

static s32 hle_scePlayGoGetEta(u32 h, const u16 *ids, u32 n, s64 *eta)
{
    if (!ids || !eta)
        return PG_BAD_POINTER;
    if (!n)
        return PG_BAD_SIZE;
    *eta = 0;
    return 0;
}

/* one {progressSize, totalSize} for all of ids: everything is here */
static s32 hle_scePlayGoGetProgress(u32 h, const u16 *ids, u32 n, u64 *progress)
{
    if (!ids || !progress)
        return PG_BAD_POINTER;
    if (!n)
        return PG_BAD_SIZE;
    playgo_load();
    u64 total = 0;
    for (u32 i = 0; g_chunk_bytes && i < n; i++)
        if (ids[i] < g_chunks)
            total += g_chunk_bytes[ids[i]];
    progress[0] = progress[1] = total ? total : 1;
    return 0;
}

static s32 hle_scePlayGoGetToDoList(u32 h, void *list, u32 n, u32 *out)
{
    if (!out)
        return PG_BAD_POINTER;
    *out = 0; /* nothing to download */
    return 0;
}

static s32 hle_scePlayGoPrefetch(u32 h, const u16 *ids, u32 n, s8 min_locus)
{
    if (min_locus != LOCUS_NOT_DOWNLOADED && min_locus != LOCUS_LOCAL_SLOW && min_locus != LOCUS_LOCAL_FAST)
        return PG_BAD_LOCUS;
    return 0;
}

static s32 hle_scePlayGoGetInstallSpeed(u32 h, s32 *speed)
{
    if (!speed)
        return PG_BAD_POINTER;
    *speed = g_speed;
    return 0;
}

static s32 hle_scePlayGoSetInstallSpeed(u32 h, s32 speed)
{
    if (speed < 0 || speed > SPEED_FULL)
        return PG_INVALID_ARGUMENT;
    g_speed = speed;
    return 0;
}

static s32 hle_scePlayGoGetLanguageMask(u32 h, u64 *mask)
{
    if (!mask)
        return PG_BAD_POINTER;
    playgo_load();
    *mask = g_lang_mask;
    return 0;
}

static s32 hle_scePlayGoSetLanguageMask(u32 h, u64 mask)
{
    playgo_load();
    g_lang_mask = mask;
    return 0;
}

static s32 pg_ok(void) { return 0; }

/* ---- libSceDiscMap: nothing is on a disc ---- */

#define DM_INVALID_ARGUMENT ((s32)0x81100001)
#define DM_NO_BITMAP_INFO ((s32)0x81100004)

static s32 hle_sceDiscMapIsRequestOnHDD(const char *path, s64 off, s64 len, s32 *on_hdd)
{
    if (!on_hdd)
        return DM_INVALID_ARGUMENT;
    *on_hdd = 1;
    return 0;
}

/* Func_7C980FFB0AA27E7A, which the title's libSceFios2.prx imports: no
 * known name, so it is matched by NID */
static s32 hle_discmap_7c98(const char *path, s64 off, s64 len, s32 *flags, s32 *r1, s32 *r2)
{
    if (flags)
        *flags = 0;
    if (r1)
        *r1 = 0;
    if (r2)
        *r2 = 0;
    return 0;
}

static s32 dm_no_bitmap(void) { return DM_NO_BITMAP_INFO; }

#define PG "libScePlayGo"
#define DM "libSceDiscMap"
#define E(lib, fn) {lib, #fn, hle_##fn, NULL}
static const struct ext_hle_ent k_hle[] = {
    E(PG, scePlayGoInitialize),
    E(PG, scePlayGoOpen),
    {PG, "scePlayGoClose", pg_ok, NULL},
    {PG, "scePlayGoTerminate", pg_ok, NULL},
    E(PG, scePlayGoGetChunkId),
    E(PG, scePlayGoGetLocus),
    E(PG, scePlayGoGetEta),
    E(PG, scePlayGoGetProgress),
    E(PG, scePlayGoGetToDoList),
    {PG, "scePlayGoSetToDoList", pg_ok, NULL},
    E(PG, scePlayGoPrefetch),
    E(PG, scePlayGoGetInstallSpeed),
    E(PG, scePlayGoSetInstallSpeed),
    E(PG, scePlayGoGetLanguageMask),
    E(PG, scePlayGoSetLanguageMask),
    {"libSceDbgPlayGo", "sceDbgPlayGoRequestNextChunk", pg_ok, NULL},
    {"libSceDbgPlayGo", "sceDbgPlayGoSnapshot", pg_ok, NULL},
    E(DM, sceDiscMapIsRequestOnHDD),
    {DM, NULL, hle_discmap_7c98, "fJgP+wqifno"},
    {DM, NULL, dm_no_bitmap, "ioKMruft1ek"}, /* Func_8A828CAEE7EDD5E9 */
    {DM, NULL, dm_no_bitmap, "5+vOlukvkfg"}, /* Func_E7EBCE96E92F91F8 */
    {DM, "sceDiscMapGetPackageSize", dm_no_bitmap, NULL},
    {DM, "sceDiscMapBitmapInfoServerStart", pg_ok, NULL},
    {DM, "sceDiscMapBitmapInfoServerStop", pg_ok, NULL},
};

/* never the host's: SWRR's PlayGo is not open, and DiscMap would look for a disc */
static const struct ext_lib k_libs[] = {
    {PG, "libSceDbgPlayGo", EXT_LIB_NO_HOST, PG_UNKNOWN},
    {DM, NULL, EXT_LIB_NO_HOST, DM_NO_BITMAP_INFO},
};

EXT_FAMILY_LIBS("playgo", k_hle, k_libs, NULL);
