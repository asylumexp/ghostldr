/*
 * Offline preview of the game selector: runs the module's own UI code
 * (ghostldr/native/selector/src/{ui,draw,font,sfo}.c) on this computer
 * against a fake catalogue, and writes PNG screenshots.
 *
 *   preview --font notosans.glf --out DIR [--meta TITLE=FILE]... [--art TITLE=DIR]...
 *           [--titles N] [--scene NAME] [--user NAME] [--ps4] [--start N]
 *           [--users NAME*,NAME+,NAME] [--scan TITLES:MS]
 *   preview --is-dump ID SFO_TITLE_ID CONTENT_ID   (prints "dump" or "homebrew")
 *
 * --ps4 runs the UI as on a PS4: game dumps marked, a question before one
 * boots.  The fake catalogue has games (CUSA IDs), one with a very long
 * name, two homebrew titles and a save tool (APOL00004, last).
 *
 * A save tool asks whose saves to open: --users lists the console's users
 * (* the one running SWRR, + another signed in, the rest not signed in;
 * the apollo scenes have four), and the scan after the choice is simulated
 * here with --scan TITLES:MS (titles to search, milliseconds each; a scan
 * cancelled with Circle leaves the rest for the next one, as the cache
 * does).  The scenes: apollo (the question, a user not signed in, a long
 * scan with its progress), apollo-only (the save tool alone: the question
 * at once, a short scan without a bar), apollo-cancel (Circle during the
 * scan, then the question again).
 *
 * Settings (setui.c): Square or the gear at the end of the row.  Its data
 * here is made up (struct sel_info: a PS5 on 13.60, two USB drives with
 * --usb, one exFAT with the games, one FAT32), and so is the network
 * installer, which installs one more title a few seconds after it starts.
 * The scenes: settings (every section, the log switch, Install over network
 * with its install, a new title in the row, Close ghostldr: No), empty (no
 * title: the gear alone, what to do, Settings), one (one title and the
 * gear), quit (Close ghostldr: Yes).
 *
 * Art: --meta TITLE=FILE reads a meta.bin (host/gfs_meta.py) as the console
 * reads <prefix>META, QOI and all; --art TITLE=DIR takes icon0.png / pic1.png
 * (a dump's sce_sys) or icon.png / backdrop.png (gfs_meta.py fetch) from DIR.
 * Otherwise the harness draws its own placeholder art (gradients and shapes)
 * or none, to exercise the fallbacks.  The art is read at run time and never
 * copied.  PNG decoding here uses zlib; on the console the module uses
 * libScePngDec.  Each run prints what drawing frames cost here.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "png_host.h"
#include "sel.h"

/* ---- platform: heap ---- */
struct plat {
    u8 *heap;
    u64 size, used, peak;
    struct art {
        const char *id, *dir;
        int meta; /* dir is a meta.bin */
    } art[16];
    int nart;
    u64 draw_ns, draw_max_ns;
    int frames, partial;
    int verify, mismatches; /* --verify: every partial frame against a whole redraw */
    u32 *check;
    int procedural[SEL_MAX_TITLES]; /* 0 none, 1.. kind */
};

void *sel_alloc(struct sel *S, u64 size)
{
    struct plat *P = S->plat;
    u64 at = (P->used + 63) & ~63ull;
    if (at + size > P->size)
        return NULL;
    P->used = at + size;
    if (P->used > P->peak)
        P->peak = P->used;
    memset(P->heap + at, 0, size);
    return P->heap + at;
}
u64 sel_mark(struct sel *S) { return ((struct plat *)S->plat)->used; }
void sel_release(struct sel *S, u64 mark) { ((struct plat *)S->plat)->used = mark; }

void sel_log(struct sel *S, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "selector: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

int sel_fmt(char *buf, int size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, (size_t)size, fmt, ap);
    va_end(ap);
    return n;
}

/* ---- PNG (png_host.c) ---- */
static int png_decode(struct sel *S, const u8 *d, u64 n, surface *out)
{
    unsigned w, h;
    if (png_host_decode(d, n, &w, &h, NULL))
        return -1;
    out->w = (int)w, out->h = (int)h, out->stride = (int)w, out->cy0 = out->cy1 = 0;
    out->px = sel_alloc(S, (u64)w * h * 4);
    return out->px ? png_host_decode(d, n, &w, &h, out->px) : -1;
}

static int png_write(const char *path, const surface *s) { return png_host_write(path, s->px, s->w, s->h, s->stride); }

/* ---- placeholder art: simple generated scenes (no third-party images) ---- */
static u32 rgb(float r, float g, float b)
{
    int R = (int)(r * 255), G = (int)(g * 255), B = (int)(b * 255);
    R = R < 0 ? 0 : R > 255 ? 255 : R, G = G < 0 ? 0 : G > 255 ? 255 : G, B = B < 0 ? 0 : B > 255 ? 255 : B;
    return 0xFF000000u | (u32)R << 16 | (u32)G << 8 | (u32)B;
}

static float hashf(int x, int y)
{
    u32 h = (u32)x * 374761393u + (u32)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

/* kinds: 1 mountains at dusk, 2 space, 3 neon grid, 4 ocean sunset, 5 forest mist */
static u32 scene(int kind, float u, float v, float aspect)
{
    float x = u * aspect;
    switch (kind) {
    case 1: {
        float r = 0.18f + (1 - v) * 0.1f, g = 0.12f + (1 - v) * 0.08f, b = 0.30f + (1 - v) * 0.1f;
        float glow = expf(-((x - aspect * 0.62f) * (x - aspect * 0.62f) * 6 + (v - 0.55f) * (v - 0.55f) * 30));
        r += glow * 0.9f, g += glow * 0.45f, b += glow * 0.15f;
        for (int l = 0; l < 4; l++) {
            float hgt = 0.52f + l * 0.1f + 0.06f * sinf(x * (3 + l * 2.1f) + l) + 0.03f * sinf(x * (11 + l * 3.7f));
            if (v > hgt) {
                float k = 0.6f - l * 0.13f;
                r = 0.10f * k + 0.04f, g = 0.08f * k + 0.03f, b = 0.18f * k + 0.06f;
            }
        }
        return rgb(r, g, b);
    }
    case 2: {
        float r = 0.02f + 0.05f * v, g = 0.02f + 0.03f * v, b = 0.07f + 0.08f * v;
        float neb = 0.5f + 0.5f * sinf(x * 3.1f + v * 2.3f) * sinf(x * 1.3f - v * 4.1f);
        r += neb * 0.25f * (1 - v), g += neb * 0.05f, b += neb * 0.3f;
        int gx = (int)(x * 300), gy = (int)(v * 300);
        if (hashf(gx, gy) > 0.996f)
            r = g = b = 0.9f;
        float dx = x - aspect * 0.7f, dy = v - 0.62f, d = sqrtf(dx * dx + dy * dy);
        if (d < 0.28f) {
            float l = 0.35f + 0.65f * (-(dx * 0.6f + dy * 0.8f) / 0.28f);
            r = 0.75f * l, g = 0.45f * l, b = 0.3f * l;
        } else if (d < 0.31f) {
            b += 0.4f * (0.31f - d) / 0.03f, r += 0.2f * (0.31f - d) / 0.03f;
        }
        return rgb(r, g, b);
    }
    case 3: {
        float hz = 0.55f;
        if (v < hz) {
            float t = v / hz;
            float sun = sqrtf((x - aspect * 0.5f) * (x - aspect * 0.5f) + (v - 0.42f) * (v - 0.42f));
            float r = 0.15f + 0.6f * t, g = 0.02f + 0.1f * t, b = 0.25f + 0.2f * t;
            if (sun < 0.2f && !((int)(v * 60) % 4 == 0 && v > 0.38f))
                r = 1.0f, g = 0.55f + 0.4f * (0.42f - v) * 3, b = 0.3f;
            return rgb(r, g, b);
        }
        float depth = (v - hz) / (1 - hz), z = 1.0f / (depth + 0.02f);
        float gxv = (x - aspect * 0.5f) * z, gzv = z * 0.5f;
        float lx = fabsf(gxv - floorf(gxv + 0.5f)), lz = fabsf(gzv - floorf(gzv + 0.5f));
        float line = (lx < 0.04f * z * 0.5f || lz < 0.02f * z) ? 1.0f : 0.0f;
        return rgb(0.05f + line * 0.9f, 0.02f + line * 0.3f * depth, 0.12f + line * 0.8f);
    }
    case 4: {
        float hz = 0.58f;
        if (v < hz) {
            float t = v / hz;
            float r = 0.98f - 0.2f * (1 - t), g = 0.45f + 0.25f * (1 - t), b = 0.35f + 0.35f * (1 - t);
            float sun = sqrtf((x - aspect * 0.35f) * (x - aspect * 0.35f) + (v - 0.48f) * (v - 0.48f));
            if (sun < 0.07f)
                r = 1, g = 0.9f, b = 0.7f;
            return rgb(r, g, b);
        }
        float t = (v - hz) / (1 - hz);
        float wave = 0.5f + 0.5f * sinf(x * 40 / (t + 0.1f) + t * 30);
        float refl = expf(-(x - aspect * 0.35f) * (x - aspect * 0.35f) * 40) * wave;
        return rgb(0.1f + 0.2f * (1 - t) + refl * 0.8f, 0.2f + 0.1f * (1 - t) + refl * 0.6f, 0.35f + 0.1f * t + refl * 0.3f);
    }
    default: {
        float r = 0.2f + 0.3f * v, g = 0.35f + 0.25f * v, b = 0.3f + 0.1f * v;
        for (int l = 0; l < 5; l++) {
            float tx = x * (4 + l) + l * 1.7f, row = 0.35f + l * 0.12f;
            float tree = row + 0.08f * fabsf(sinf(tx * 3.1f)) + 0.04f * sinf(tx * 7.3f);
            if (v > tree) {
                float k = 0.55f - l * 0.09f;
                r = 0.08f + k * 0.15f, g = 0.15f + k * 0.25f, b = 0.12f + k * 0.15f;
            }
        }
        return rgb(r, g, b);
    }
    }
}

static int procedural(struct sel *S, int kind, int big, surface *out)
{
    int w = big ? 1920 : 512, h = big ? 1080 : 512;
    out->w = w, out->h = h, out->stride = w, out->cy0 = out->cy1 = 0;
    out->px = sel_alloc(S, (u64)w * h * 4);
    if (!out->px)
        return -1;
    float aspect = (float)w / (float)h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            out->px[(u64)y * w + x] = scene(kind, (float)x / (float)w, (float)y / (float)h, aspect);
    if (!big) { /* the game's name on its icon, like key art */
        const char *name = S->titles[S->plat_title].name;
        if (!text_fits_font(S, name))
            name = S->titles[S->plat_title].id;
        int px = 50, tw = text_width(S, px, name);
        while (tw > w - 60 && px > 24)
            tw = text_width(S, --px, name);
        draw_rect_blend(out, 0, h - 120, w, 120, 0x60000000u);
        draw_text(S, out, px, (w - tw) / 2 + 2, h - 46, name, 0xA0000000u, w - 40);
        draw_text(S, out, px, (w - tw) / 2, h - 48, name, 0xFFFFFFFFu, w - 40);
    }
    return 0;
}

static u8 *slurp(struct sel *S, const char *path, u64 *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *n = (u64)ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *d = sel_alloc(S, *n);
    int ok = d && fread(d, 1, (size_t)*n, f) == (size_t)*n;
    fclose(f);
    return ok ? d : NULL;
}

/* a meta.bin, as the console reads <prefix>META (main.c sel_load_art) */
static int meta_art(struct sel *S, int t, const char *file, int which, surface *out)
{
    u64 n;
    const u8 *d = slurp(S, file, &n);
    struct meta_info mi;
    if (!d || meta_parse(d, n, &mi) != 0)
        return -1;
    struct title *ti = &S->titles[t];
    if (!ti->name[0] && mi.name[0])
        str_copy(ti->name, mi.name, sizeof(ti->name));
    if (!ti->version[0] && mi.version[0])
        str_copy(ti->version, mi.version, sizeof(ti->version));
    const struct meta_sect *m = &mi.s[which == SEL_ICON ? META_ICON : META_BACKDROP];
    int w, h;
    if (!m->size || m->format != META_FMT_QOI || (u64)m->off + m->size > n || qoi_header(d + m->off, m->size, &w, &h))
        return -1;
    *out = (surface){sel_alloc(S, (u64)w * h * 4), w, h, w, 0, 0};
    if (!out->px || qoi_decode(d + m->off, m->size, out))
        return -1;
    (which == SEL_ICON ? &ti->icon_src : &ti->bg_src)[0] = m->source;
    return 0;
}

int sel_load_art(struct sel *S, int t, int which, surface *out)
{
    struct plat *P = S->plat;
    struct title *ti = &S->titles[t];
    u8 *src = which == SEL_ICON ? &ti->icon_src : &ti->bg_src;
    for (int i = 0; i < P->nart; i++) {
        if (strcmp(P->art[i].id, ti->id))
            continue;
        if (P->art[i].meta)
            return meta_art(S, t, P->art[i].dir, which, out);
        /* a dump's sce_sys, or what gfs_meta.py fetch saved */
        const char *own[3] = {which == SEL_ICON ? "icon0.png" : "pic1.png", which == SEL_ICON ? NULL : "pic0.png", NULL};
        const char *store = which == SEL_ICON ? "icon.png" : "backdrop.png";
        for (int k = 0; k < 3; k++) {
            const char *leaf = k < 2 ? own[k] : store;
            if (!leaf)
                continue;
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", P->art[i].dir, leaf);
            u64 n;
            u8 *d = slurp(S, full, &n);
            if (d && png_decode(S, d, n, out) == 0) {
                *src = k < 2 ? ART_TITLE : ART_STORE;
                return 0;
            }
        }
        return -1;
    }
    int kind = P->procedural[t];
    if (!kind)
        return -1;
    int big = which == SEL_BACKDROP;
    S->plat_title = t;
    if (kind < 0 && big) /* icon only */
        return -1;
    *src = ART_TITLE;
    return procedural(S, kind < 0 ? -kind : kind, big, out);
}

/* ---- Settings: made-up console, drives, log and network installer ---- */
static int g_usb, g_ps4, g_net_state, g_net_installed, g_add_title;
static u64 g_net_t0, g_now;

/* Settings > Advanced: three made-up switches, as conf.c's (--safe: this
 * boot left them out) */
static u32 g_sw_on = 1 | 2 | 4;
static int g_safe;
int sel_switch(struct sel *S, int i, int want)
{
    if (i < 0) {
        g_sw_on = 1 | 2 | 4;
        return 0;
    }
    if (want >= 0)
        g_sw_on = (g_sw_on & ~(1u << i)) | ((u32)(want != 0) << i);
    return (int)((g_sw_on >> i) & 1);
}
static void fake_switches(struct sel_info *in)
{
    static const char labels[4][48] = {"Write logs to the USB drive", "Show the system's dialogs",
                                       "Plan memory automatically", "Keep Remote Play on"};
    static const char on[4][112] = {"On: each boot's log goes to the folder ghostldr/logs on the drive",
                                    "On: a game's save, message and keyboard dialogs show and wait for you",
                                    "On: a game without a memory plan of its own gets one, learned from its runs",
                                    "On: a game that turns Remote Play off can't (for streaming the TV to another device)"};
    static const char off[4][112] = {"Off: nothing is written to the drive",
                                     "Off: they are answered at once and not shown (a game stuck at one may get past it)",
                                     "Off: such a game gets the plain budget",
                                     "Off: a game may turn Remote Play off, as on its own"};
    in->nsw = 4, in->sw_on = g_sw_on, in->sw_safe = g_safe;
    static const char keys[4][16] = {"log_usb", "dialogs", "auto_memory", "remote_play"};
    for (int i = 0; i < 4; i++) {
        str_copy(in->sw[i].key, keys[i], sizeof(in->sw[i].key));
        str_copy(in->sw[i].label, labels[i], sizeof(in->sw[i].label));
        str_copy(in->sw[i].on, on[i], sizeof(in->sw[i].on));
        str_copy(in->sw[i].off, off[i], sizeof(in->sw[i].off));
        in->sw[i].def = i < 3, in->sw[i].logs = i == 0;
    }
}

void sel_info_load(struct sel *S, struct sel_info *in)
{
    __builtin_memset(in, 0, sizeof(*in));
    str_copy(in->version, "v1.0.0-preview", sizeof(in->version));
    str_copy(in->build, "3e4ddde6f8225863 (bundle)", sizeof(in->build));
    str_copy(in->cache, "ghostldr-ext.bin of this build (2.9 MB): send ghostldr.lua alone the next time", sizeof(in->cache));
    str_copy(in->console, g_ps4 ? "PS4  \xC2\xB7  firmware 12.50" : "PS5  \xC2\xB7  firmware 13.60", sizeof(in->console));
    str_copy(in->host, "Star Wars Racer Revenge  \xC2\xB7  US (CUSA03474)  \xC2\xB7  version 01.00  \xC2\xB7  Luac0re",
             sizeof(in->host));
    str_copy(in->ip, "192.168.0.20", sizeof(in->ip));
    static const struct sel_port ports[] = {{9026, "Luac0re's loader: send ghostldr.lua here"},
                                            {9021, "ghostldr's bundle port and ELF loader (9022 when 9021 was taken)"},
                                            {1212, "FTP, while a game runs: the USB drive, and the game's own files"},
                                            {8090, "The network installer, while it runs (its page and its uploads)"}};
    for (int i = 0; i < 4; i++)
        in->ports[i] = ports[i];
    in->nports = 4, in->netinst = 1;
    in->log_usb = (int)(g_sw_on & 1);
    str_copy(in->log_where, in->log_usb ? (g_usb ? "Example NVMe Enclosure (drive 1 of 2): ghostldr/logs/0012_2026-10-28_2147.log"
                                                 : "No USB drive: /temp0/ghostldr.log")
                                        : "Off: /temp0/ghostldr.log", sizeof(in->log_where));
    in->usb = g_ps4 ? USB_PS4 : USB_READ;
    fake_switches(in);
    in->ninst = -1;
    if (g_usb) { /* a USB boot: saved data holds two titles as well */
        in->ninst = 2;
        str_copy(in->inst[0].id, "CUSA00042", 16), str_copy(in->inst[0].name, "Skyward Harbour", 64);
        str_copy(in->inst[0].version, "01.00", 16), in->inst[0].size = 48ull << 30;
        str_copy(in->inst[1].id, "BREW00083", 16), str_copy(in->inst[1].name, "Hello World", 64);
        in->inst[1].size = 1 << 20, in->inst[1].incomplete = 1;
    }
    if (g_usb && !g_ps4) {
        struct sel_drive *d = &in->drives[0];
        str_copy(d->name, "Example NVMe Enclosure", sizeof(d->name));
        d->fs = FS_EXFAT, d->total = 4000787030016ull, d->free = 3612491005952ull;
        d->games = (s16)(S->ntitles > 1 ? S->ntitles - 1 : 0), d->bad = S->ntitles ? 1 : 0, d->listed = S->ntitles > 1;
        d = &in->drives[1];
        str_copy(d->name, "Example Flash Disk", sizeof(d->name));
        d->fs = FS_FAT32, d->total = 31914983424ull, d->free = 30064771072ull;
        in->ndrives = 2;
    }
}

/* starts at once; a browser starts installing a title 3 s later, 48 GB at
 * 40 MB/s sped up (done in 6 s); then waiting again with one installed */
int sel_netinst(struct sel *S, int op, struct sel_net *st)
{
    if (op == NETOP_START)
        g_net_state = NET_WAITING, g_net_t0 = g_now;
    if (op == NETOP_STOP) {
        g_net_state = NET_STOPPED;
        return 0;
    }
    if (!st)
        return 0;
    __builtin_memset(st, 0, sizeof(*st));
    u64 t = g_now - g_net_t0;
    int busy = g_net_state != NET_STOPPED && t >= 3000000 && t < 9000000;
    if (g_net_state != NET_STOPPED && t >= 9000000 && !g_net_installed)
        g_net_installed = 1, g_add_title = 1;
    st->state = g_net_state == NET_STOPPED ? NET_STOPPED : busy ? NET_BUSY : NET_WAITING;
    str_copy(st->url, "http://192.168.0.20:8090/", sizeof(st->url));
    str_copy(st->token, "K7QF-3MXP", sizeof(st->token));
    if (busy) {
        str_copy(st->title, "CUSA00042", sizeof(st->title));
        str_copy(st->name, "Skyward Harbour", sizeof(st->name));
        st->total = 48ull << 30, st->done = st->total * (t - 3000000) / 6000000, st->rate_kib = 40 * 1024;
        str_copy(st->message, "Writing GSKY017 (17 of 49 data containers)", sizeof(st->message));
    } else {
        str_copy(st->message, g_net_installed ? "Skyward Harbour (CUSA00042) installed: it's in the list."
                                              : "Waiting for a browser.", sizeof(st->message));
    }
    st->installed = (u32)g_net_installed, st->changes = (u32)g_net_installed;
    return 0;
}

/* ---- fake catalogue ---- */
struct fake {
    const char *id, *name, *version, *prefix;
    double gib;
    int complete, art;
    const char *content_id; /* param.sfo's CONTENT_ID */
};
static const struct fake FAKES[] = {
    {"CUSA11456", "Ghost of Tsushima", "02.18", "GHOST", 53.3, 1, 0, ""},
    {"CUSA00001", "Starfall Odyssey", "01.05", "G00001", 41.2, 1, 2, "UP0001-CUSA00001_00-STARFALLODYSSEY0"},
    {"CUSA00002", "Neon Drift: Night Circuit", "01.12", "G00002", 18.7, 1, 3, ""},
    {"CUSA00003", "Harbor Lights", "01.00", "G00003", 9.4, 1, 4, ""},
    {"CUSA00004", "Cascade Valley", "02.01", "G00004", 27.9, 1, 1, ""},
    {"CUSA00005", "Ironclad Tactics II", "01.03", "G00005", 6.1, 1, 0, ""},
    {"CUSA00006", "\xE6\x98\x9F\xE3\x81\xAE\xE5\xAE\x88\xE3\x82\x8A\xE4\xBA\xBA", "01.00", "G00006", 12.0, 1, -5, ""},
    {"CUSA00007", "Emberfall Chronicles", "", "G00007", 0, 0, 5, ""},
    {"CUSA00008", "The Remarkably Long and Winding Chronicles of the Very Small Robot Who Wanted to See the Sea: "
                  "Definitive Collector's Edition", "01.02", "G00008", 33.0, 1, 2, ""},
    {"HBPR00002", "hbprobe (system malloc)", "01.00", "G00002H", 0.004, 1, 0, "IV0000-HBPR00002_00-HBPROBE000000000"},
    {"LAPY20001", "PS4-Xplorer", "01.34", "G20001", 0.09, 1, 3, ""},
    {"APOL00004", "Apollo Save Tool", "02.40", "GAPOL00004", 0.03, 1, 2, "IV0000-APOL00004_00-APOLLO0000000PS4"},
};
#define FAKE_SAVE_TOOL 11 /* its profile has "save_tool" */

static u8 *read_file(const char *path, u64 *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *n = (u64)ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *d = malloc(*n);
    if (fread(d, 1, *n, f) != *n) {
        free(d);
        d = NULL;
    }
    fclose(f);
    return d;
}

/* t0 icon + background, t1 icon only, t2 both, t3 nothing, t4 both */
static int dump_art(struct sel *S, const char *dir, int n)
{
    static const int kinds[5] = {1, -2, 3, 0, 4};
    struct plat *P = S->plat;
    for (int i = 0; i < 5 && i < n; i++) {
        P->procedural[i] = kinds[i];
        for (int big = 0; big < 2; big++) {
            u64 m = sel_mark(S);
            surface img;
            if (sel_load_art(S, i, big ? SEL_BACKDROP : SEL_ICON, &img) == 0) {
                char path[1024];
                snprintf(path, sizeof(path), "%s/t%d/sce_sys/%s", dir, i, big ? "pic1.png" : "icon0.png");
                if (png_write(path, &img))
                    fprintf(stderr, "cannot write %s (create the directories first)\n", path);
            }
            sel_release(S, m);
        }
    }
    return 0;
}

/* ---- scripted runs ---- */
struct event {
    int ms;
    u32 buttons;
    int stick;
    const char *shot; /* write a screenshot at this time */
};

/* a scripted run: the events so far, as time goes on */
struct script {
    const struct event *ev;
    int nev, e;
    u32 buttons;
    int stick;
    const char *outdir, *prefix;
    u32 scan_titles, scan_ms; /* --scan: the simulated save scan */
};

static void events(struct sel *S, struct script *sc, u64 now)
{
    while (sc->e < sc->nev && (u64)sc->ev[sc->e].ms * 1000 <= now) {
        const struct event *ev = &sc->ev[sc->e];
        if (ev->shot && ev->shot[0] == '@') { /* "@clock": the minute changes */
            ui_set_clock(S, S->clock_h, (S->clock_m + 1) % 60, S->clock_24h);
        } else if (ev->shot) {
            ui_draw(S);
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s%s.png", sc->outdir, sc->prefix, ev->shot);
            png_write(path, &S->back);
            fprintf(stderr, "wrote %s\n", path);
        } else {
            sc->buttons = ev->buttons, sc->stick = ev->stick ? ev->stick : 128;
        }
        sc->e++;
    }
}

/* a step of the UI and its frame, timed, and checked against a whole redraw */
static void step_draw(struct sel *S, struct script *sc, u64 now, u64 dt, int *boot)
{
    if (!ui_step(S, now, dt, sc->buttons, sc->stick, 128, boot))
        return;
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    ui_draw(S);
    clock_gettime(CLOCK_MONOTONIC, &b);
    struct plat *P = S->plat;
    u64 ns = (u64)(b.tv_sec - a.tv_sec) * 1000000000ull + (u64)b.tv_nsec - (u64)a.tv_nsec;
    if (S->damage_y1 > S->damage_y0) {
        P->frames++;
        P->partial += S->damage_y1 - S->damage_y0 < SCREEN_H;
        P->draw_ns += ns;
        if (ns > P->draw_max_ns)
            P->draw_max_ns = ns;
    }
    if (P->verify && S->damage_y1 - S->damage_y0 < SCREEN_H) {
        u64 bytes = (u64)SCREEN_W * SCREEN_H * 4;
        memcpy(P->check, S->back.px, bytes);
        S->drawn_once = 0, S->changed = CH_ALL;
        ui_draw(S);
        if (memcmp(P->check, S->back.px, bytes)) {
            P->mismatches++;
            fprintf(stderr, "partial frame at %llu ms differs from a whole redraw\n", (unsigned long long)(now / 1000));
        }
    }
}

/* the save scan as the console's main.c runs it (ghost_api.save_prescan),
 * simulated: TITLES searches of MS each, a little uneven; a frame at most
 * every 33 ms; titles searched before a cancel are not searched again */
static void fake_scan(struct sel *S, struct script *sc, u64 *now)
{
    static u32 cached;
    u32 total = sc->scan_titles > cached ? sc->scan_titles - cached : 0, done = 0;
    u64 last = *now;
    ui_scan_start(S, *now);
    fprintf(stderr, "scan: user %x, %u titles to search\n", (u32)S->users[S->user_focus].id, total);
    for (int cancel = total ? ui_scan_progress(S, *now, 0, total) : 0; !cancel && done < total;) {
        *now += (u64)sc->scan_ms * (900 + (done * 37) % 200); /* 0.9 to 1.1 times MS */
        done++;
        events(S, sc, *now);
        cancel = ui_scan_progress(S, *now, done, total);
        if (*now - last >= 33333) {
            step_draw(S, sc, *now, *now - last, NULL);
            last = *now;
            cancel |= S->scan_cancel;
        }
    }
    cached += done;
    ui_scan_end(S, *now, (int)(done / 3));
    step_draw(S, sc, *now, *now - last, NULL);
}

/* an install finished: the platform reads the catalogue again; here the
 * new title joins the row (as main.c's reload does) */
static void add_title(struct sel *S)
{
    int n = S->ntitles + 1;
    struct title *t = sel_alloc(S, sizeof(struct title) * (u64)n);
    __builtin_memcpy(t, S->titles, sizeof(struct title) * (u64)S->ntitles);
    struct title *nt = &t[n - 1];
    str_copy(nt->id, "CUSA00042", sizeof(nt->id));
    str_copy(nt->name, "Skyward Harbour", sizeof(nt->name));
    str_copy(nt->version, "01.00", sizeof(nt->version));
    str_copy(nt->prefix, "GSKY", sizeof(nt->prefix));
    nt->size = 48ull << 30, nt->flags = TITLE_COMPLETE | TITLE_DUMP;
    ui_title_alloc(S, nt);
    ((struct plat *)S->plat)->procedural[n - 1] = 4;
    ui_set_titles(S, t, n);
    fprintf(stderr, "reload: %d title(s), the new one CUSA00042\n", n);
}

static void run(struct sel *S, struct script *sc)
{
    u64 now = 0, step = 33333;
    int boot = -1;
    sc->stick = 128;
    for (int frame = 0; frame < 30 * 120 && sc->e < sc->nev; frame++) {
        g_now = now;
        events(S, sc, now);
        step_draw(S, sc, now, step, &boot);
        if (S->scan_req)
            fake_scan(S, sc, &now);
        if (S->reload_req) {
            S->reload_req = 0;
            if (g_add_title)
                g_add_title = 0, add_title(S);
        }
        if (boot == UI_QUIT) {
            fprintf(stderr, "quit at %llu ms: back to Luac0re's loader\n", (unsigned long long)(now / 1000));
            break;
        }
        if (boot >= 0) {
            fprintf(stderr, "boot title %d (%s) at %llu ms\n", boot, S->titles[boot].id, (unsigned long long)(now / 1000));
            if (S->user_chosen)
                fprintf(stderr, "title_user %x\n", (u32)S->users[S->user_focus].id);
            break;
        }
        now += step;
    }
}

int main(int argc, char **argv)
{
    const char *font = NULL, *out = ".", *scene_name = "tour", *prefix = "", *write_art = NULL, *user = NULL;
    int ntitles = (int)(sizeof(FAKES) / sizeof(FAKES[0])), clock_h = 21, clock_m = 47, h24 = 1, usb = 0, ps4 = 0;
    int start = 0;
    const char *users = NULL;
    u32 scan_titles = 349, scan_ms = 400;
    int scan_given = 0;
    static struct plat P;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--font") && i + 1 < argc)
            font = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "--scene") && i + 1 < argc)
            scene_name = argv[++i];
        else if (!strcmp(argv[i], "--prefix") && i + 1 < argc)
            prefix = argv[++i];
        else if (!strcmp(argv[i], "--titles") && i + 1 < argc)
            ntitles = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--12h"))
            h24 = 0;
        else if (!strcmp(argv[i], "--usb")) /* the games on a USB drive: folders, a second copy, one that can't start */
            usb = 1;
        else if (!strcmp(argv[i], "--ps4")) /* as on a PS4: game dumps marked, a question before one boots */
            ps4 = 1;
        else if (!strcmp(argv[i], "--safe")) /* Settings > Advanced was left out of this boot (safe mode) */
            g_safe = 1;
        else if (!strcmp(argv[i], "--start") && i + 1 < argc)
            start = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--users") && i + 1 < argc)
            users = argv[++i];
        else if (!strcmp(argv[i], "--scan") && i + 1 < argc) {
            char *colon = NULL;
            scan_titles = (u32)strtoul(argv[++i], &colon, 10);
            scan_ms = colon && *colon == ':' ? (u32)strtoul(colon + 1, NULL, 10) : 400;
            scan_given = 1;
        }
        else if (!strcmp(argv[i], "--is-dump") && i + 3 < argc) { /* the selector's rule (sfo.c) */
            printf("%s\n", title_is_dump(argv[i + 1], argv[i + 2], argv[i + 3]) ? "dump" : "homebrew");
            return 0;
        }
        else if (!strcmp(argv[i], "--qr") && i + 2 < argc) { /* TEXT OUT.png: qr.c's code, 8 px a module */
            static u8 m[QR_N * QR_N];
            if (qr_encode(argv[i + 1], m) != 0) {
                fprintf(stderr, "too long for a version 3-M QR code\n");
                return 1;
            }
            static u32 px[(QR_N + 8) * 8 * (QR_N + 8) * 8];
            surface q = {px, (QR_N + 8) * 8, (QR_N + 8) * 8, (QR_N + 8) * 8, 0, 0};
            qr_draw(&q, 0, 0, q.w, m, 255);
            return png_write(argv[i + 2], &q);
        }
        else if (!strcmp(argv[i], "--write-art") && i + 1 < argc)
            write_art = argv[++i];
        else if (!strcmp(argv[i], "--decode-meta") && i + 2 < argc) { /* FILE DIR: its pictures through meta.c */
            static struct sel D;
            static struct plat DP;
            DP.size = 64ull << 20, DP.heap = malloc(DP.size), D.plat = &DP;
            struct title t;
            memset(&t, 0, sizeof(t));
            D.titles = &t, D.ntitles = 1;
            const char *file = argv[++i], *dir = argv[++i];
            for (int which = 0; which < 2; which++) {
                surface img;
                u64 m = sel_mark(&D);
                char path[1024];
                snprintf(path, sizeof(path), "%s/%s.png", dir, which == SEL_ICON ? "icon" : "backdrop");
                if (meta_art(&D, 0, file, which, &img) == 0)
                    png_write(path, &img);
                sel_release(&D, m);
            }
            printf("name '%s' version '%s' icon %d backdrop %d\n", t.name, t.version, t.icon_src, t.bg_src);
            return 0;
        } else if (!strcmp(argv[i], "--verify"))
            P.verify = 1;
        else if (!strcmp(argv[i], "--user") && i + 1 < argc)
            user = argv[++i];
        else if ((!strcmp(argv[i], "--art") || !strcmp(argv[i], "--meta")) && i + 1 < argc && P.nart < 16) {
            int meta = !strcmp(argv[i], "--meta");
            char *a = argv[++i], *eq = strchr(a, '=');
            if (eq)
                *eq = 0, P.art[P.nart].id = a, P.art[P.nart].meta = meta, P.art[P.nart++].dir = eq + 1;
        } else {
            fprintf(stderr, "usage: preview --font F.glf --out DIR [--scene tour|info|single|launch|usb|ps4|long|apollo|"
                            "apollo-only|apollo-cancel|settings|empty|one|quit] [--usb] [--ps4] [--titles N] [--start N] [--art TITLE=DIR] "
                            "[--meta TITLE=meta.bin] [--user NAME] [--users NAME*,NAME+,NAME] [--scan TITLES:MS] "
                            "[--12h] [--prefix P]\n       preview --is-dump ID SFO_TITLE_ID CONTENT_ID\n");
            return 2;
        }
    }
    u64 fn = 0;
    u8 *glf = font ? read_file(font, &fn) : NULL;
    if (!glf) {
        fprintf(stderr, "need --font (ghostldr/native/selector/font/notosans.glf)\n");
        return 2;
    }
    if (!strcmp(scene_name, "empty"))
        ntitles = 0;
    else if (!strcmp(scene_name, "one"))
        ntitles = 1;
    if (ntitles < 0 || ntitles > (int)(sizeof(FAKES) / sizeof(FAKES[0])))
        ntitles = (int)(sizeof(FAKES) / sizeof(FAKES[0]));
    g_usb = usb, g_ps4 = ps4;
    int apollo = !strncmp(scene_name, "apollo", 6), only = !strcmp(scene_name, "apollo-only");
    const struct fake *fakes = FAKES;
    if (only) /* the save tool alone, as when it is the only title installed; its saves mostly known already */
        fakes = &FAKES[FAKE_SAVE_TOOL], ntitles = 1, scan_titles = scan_given ? scan_titles : 3;
    else if (apollo)
        start = FAKE_SAVE_TOOL;
    P.size = 192ull << 20;
    P.heap = malloc(P.size);
    P.check = malloc((u64)SCREEN_W * SCREEN_H * 4);
    static struct sel S;
    S.plat = &P;
    struct title *titles = sel_alloc(&S, sizeof(struct title) * (ntitles ? ntitles : 1));
    for (int i = 0; i < ntitles; i++) {
        const struct fake *f = &fakes[i];
        str_copy(titles[i].id, f->id, 16);
        str_copy(titles[i].name, f->name, 128);
        str_copy(titles[i].version, f->version, 16);
        str_copy(titles[i].prefix, f->prefix, 16);
        titles[i].size = (u64)(f->gib * (1u << 30));
        titles[i].flags = (f->complete ? TITLE_COMPLETE : 0) | (title_is_dump(f->id, "", f->content_id) ? TITLE_DUMP : 0) |
                          (f == &FAKES[FAKE_SAVE_TOOL] ? TITLE_SAVE_TOOL : 0);
        P.procedural[i] = f->art;
        if (usb) { /* as main.c fills them from the extension's list */
            titles[i].size = 0;
            titles[i].prefix[0] = 0;
            snprintf(titles[i].where, sizeof(titles[i].where), i ? "My Games/%s" : "Ghost of Tsushima (CUSA11456)",
                     f->name);
            if (i == 1) {
                str_copy(titles[i].id, FAKES[0].id, 16);
                str_copy(titles[i].name, FAKES[0].name, 128);
                str_copy(titles[i].where, "ghostldr/CUSA11456", sizeof(titles[i].where));
                str_copy(titles[i].note, "a second copy: --title boots the one in Ghost of Tsushima (CUSA11456)",
                         sizeof(titles[i].note));
                titles[i].flags |= TITLE_COPY;
                /* what the row and the panel say of it, and of copies on two and three drives (USB 2/: approot.c's) */
                struct title two = titles[i], three = titles[i];
                str_copy(two.where, "USB drive 2: ghostldr/CUSA11456", sizeof(two.where));
                str_copy(three.where, "USB drive 3: ghostldr/CUSA11456", sizeof(three.where));
                str_copy(three.note, "a second copy: --title boots the one in USB 2/My Games/Ghost", sizeof(three.note));
                const struct title *ts[] = {&titles[i], &two, &three};
                for (int k = 0; k < 3; k++) {
                    char other[112];
                    if (title_copy_of(ts[k], other, sizeof(other)))
                        fprintf(stderr, "second copy %d: the same game is also in %s\n", k + 1, other);
                }
            }
            if (!f->complete)
                str_copy(titles[i].note, "no sce_sys/param.sfo: name the folder after the game's ID (CUSA12345)",
                         sizeof(titles[i].note));
        }
    }
    ui_set_platform(&S, ps4 ? SEL_PS4 : SEL_PS5);
    if (font_init(&S, glf, 2u << 20) || ui_init(&S, titles, ntitles, start)) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    ui_set_clock(&S, clock_h, clock_m, h24);
    if (!users && apollo)
        users = "User A*,User B+,User C,User D";
    if (users) { /* the console's users: NAME* runs SWRR, NAME+ is signed in too */
        struct sel_user u[SEL_MAX_USERS];
        int n = 0;
        memset(u, 0, sizeof(u));
        for (const char *p = users; *p && n < SEL_MAX_USERS; n++) {
            int len = 0;
            while (p[len] && p[len] != ',')
                len++;
            int mark = len && (p[len - 1] == '*' || p[len - 1] == '+') ? p[len - 1] : 0;
            snprintf(u[n].name, sizeof(u[n].name), "%.*s", len - (mark != 0), p);
            u[n].id = 0x10000001 + n;
            u[n].current = mark == '*', u[n].signed_in = mark != 0;
            p += len + (p[len] == ',');
        }
        ui_set_users(&S, u, n);
        if (!user)
            for (int i = 0; i < n; i++)
                if (u[i].current)
                    ui_set_user(&S, u[i].name);
    }
    if (user)
        ui_set_user(&S, user);
    if (only)
        ui_ask_user(&S, 1);
    if (write_art) /* sample art for selector_host: DIR/t<N>/sce_sys/{icon0,pic1}.png */
        return dump_art(&S, write_art, ntitles);
    u64 base = P.used;
    fprintf(stderr, "heap after init: %llu MiB\n", (unsigned long long)(base >> 20));
    struct script sc = {0};
    sc.outdir = out, sc.prefix = prefix, sc.scan_titles = scan_titles, sc.scan_ms = scan_ms;
#define RUN(EV) (sc.ev = (EV), sc.nev = (int)(sizeof(EV) / sizeof((EV)[0])), sc.e = 0, run(&S, &sc))

    if (!strcmp(scene_name, "tour")) {
        static const struct event ev[] = {
            {1000, 0, 0, "home"},
            {1100, PAD_RIGHT, 0, NULL}, {1180, 0, 0, NULL}, {1230, 0, 0, "moving"},
            {2200, 0, 0, "second"}, {2250, 0, 0, "@clock"},
            {2300, PAD_RIGHT, 0, NULL}, {2380, 0, 0, NULL},
            {2500, PAD_RIGHT, 0, NULL}, {2580, 0, 0, NULL},
            {3600, 0, 0, "fourth"},
            {3700, PAD_RIGHT, 0, NULL}, {3780, 0, 0, NULL}, {3900, PAD_RIGHT, 0, NULL}, {3980, 0, 0, NULL},
            {5000, 0, 0, "fallbacks"},
            {5100, PAD_RIGHT, 0, NULL}, {5180, 0, 0, NULL}, {6200, 0, 0, "cjk"},
            {6300, PAD_RIGHT, 0, NULL}, {6380, 0, 0, NULL}, {7400, 0, 0, "incomplete"},
            {7500, PAD_CROSS, 0, NULL}, {7580, 0, 0, NULL}, {7900, 0, 0, "toast"},
            {8000, 0, 0, NULL},
        };
        RUN(ev);
    } else if (!strcmp(scene_name, "info")) {
        static const struct event ev[] = {
            {800, PAD_RIGHT, 0, NULL}, {880, 0, 0, NULL},
            {2000, PAD_OPTIONS, 0, NULL}, {2080, 0, 0, NULL}, {2600, 0, 0, "info"},
            {2700, PAD_CIRCLE, 0, NULL}, {2780, 0, 0, NULL}, {3300, 0, 0, "info-closed"},
        };
        RUN(ev);
    } else if (!strcmp(scene_name, "usb")) { /* with --usb: a second copy's panel, a folder that can't start */
        static const struct event ev[] = {
            {800, PAD_RIGHT, 0, NULL}, {880, 0, 0, NULL},
            {2000, PAD_OPTIONS, 0, NULL}, {2080, 0, 0, NULL}, {2600, 0, 0, "usb-copy"},
            {2700, PAD_CIRCLE, 0, NULL}, {2780, 0, 0, NULL},
            {3000, PAD_R1, 0, NULL}, {3080, 0, 0, NULL}, {3200, PAD_RIGHT, 0, NULL}, {3280, 0, 0, NULL},
            {4400, 0, 0, "usb-bad"},
            {4500, PAD_CROSS, 0, NULL}, {4580, 0, 0, NULL}, {4900, 0, 0, "usb-why"},
            {5000, PAD_OPTIONS, 0, NULL}, {5080, 0, 0, NULL}, {5600, 0, 0, "usb-bad-info"},
        };
        RUN(ev);
    } else if (!strcmp(scene_name, "ps4")) { /* with --ps4: marked dumps; No, Circle, then Yes boots; homebrew unmarked */
        static const struct event ev[] = {
            {1000, 0, 0, "ps4-home"},
            {1100, PAD_CROSS, 0, NULL}, {1180, 0, 0, NULL}, {1600, 0, 0, "ps4-ask"},
            {1700, PAD_CROSS, 0, NULL}, {1780, 0, 0, NULL}, {2200, 0, 0, "ps4-no"},
            {2300, PAD_CROSS, 0, NULL}, {2380, 0, 0, NULL}, {2700, PAD_CIRCLE, 0, NULL}, {2780, 0, 0, NULL},
            {3000, PAD_R1, 0, NULL}, {3080, 0, 0, NULL}, {3200, PAD_R1, 0, NULL}, {3280, 0, 0, NULL},
            {3400, PAD_LEFT, 0, NULL}, {3480, 0, 0, NULL}, {4600, 0, 0, "ps4-homebrew"},
            {4700, PAD_OPTIONS, 0, NULL}, {4780, 0, 0, NULL}, {5300, 0, 0, "ps4-homebrew-info"},
            {5400, PAD_CIRCLE, 0, NULL}, {5480, 0, 0, NULL},
            {5600, PAD_L1, 0, NULL}, {5680, 0, 0, NULL}, {5800, PAD_L1, 0, NULL}, {5880, 0, 0, NULL},
            {7000, PAD_OPTIONS, 0, NULL}, {7080, 0, 0, NULL}, {7600, 0, 0, "ps4-info"},
            {7700, PAD_CROSS, 0, NULL}, {7780, 0, 0, NULL}, {8300, 0, 0, "ps4-ask-panel"},
            {8400, PAD_LEFT, 0, NULL}, {8480, 0, 0, NULL}, {8800, 0, 0, "ps4-ask-yes"},
            {8900, PAD_CROSS, 0, NULL}, {8980, 0, 0, NULL}, {10000, 0, 0, NULL},
        };
        RUN(ev);
    } else if (!strcmp(scene_name, "long")) { /* a very long name (start at it): the row, then its information */
        static const struct event ev[] = {
            {1000, 0, 0, "long"}, {1100, PAD_OPTIONS, 0, NULL}, {1180, 0, 0, NULL}, {1700, 0, 0, "long-info"},
        };
        RUN(ev);
    } else if (!strcmp(scene_name, "apollo")) { /* the save tool: whose saves, a user not signed in, a long scan */
        static const struct event ev[] = {
            {1000, 0, 0, "apollo-home"},
            {1100, PAD_CROSS, 0, NULL}, {1180, 0, 0, NULL}, {1700, 0, 0, "apollo-users"},
            {1800, PAD_DOWN, 0, NULL}, {1880, 0, 0, NULL}, {2000, PAD_DOWN, 0, NULL}, {2080, 0, 0, NULL},
            {2500, 0, 0, "apollo-users-greyed"},
            {2600, PAD_CROSS, 0, NULL}, {2680, 0, 0, NULL}, {3100, 0, 0, "apollo-not-signed-in"},
            {3200, PAD_UP, 0, NULL}, {3280, 0, 0, NULL}, {3400, PAD_UP, 0, NULL}, {3480, 0, 0, NULL},
            {6000, PAD_CROSS, 0, NULL}, {6080, 0, 0, NULL},
            {6500, 0, 0, "apollo-scan-start"}, {26000, 0, 0, "apollo-scan"}, {120000, 0, 0, "apollo-scan-late"},
            {200000, 0, 0, NULL},
        };
        RUN(ev);
    } else if (!strcmp(scene_name, "apollo-only")) { /* the save tool alone: asked at once, a short scan, no bar */
        static const struct event ev[] = {
            {1000, 0, 0, "apollo-only"}, {1100, PAD_CIRCLE, 0, NULL}, {1180, 0, 0, NULL}, {1500, 0, 0, "apollo-only-circle"},
            {1600, PAD_CROSS, 0, NULL}, {1680, 0, 0, NULL}, {1800, 0, 0, "apollo-only-scan"}, {5000, 0, 0, NULL},
        };
        RUN(ev);
    } else if (!strcmp(scene_name, "apollo-cancel")) { /* Circle during the scan: the question again, then the rest */
        static const struct event ev[] = {
            {1100, PAD_CROSS, 0, NULL}, {1180, 0, 0, NULL}, {1600, PAD_CROSS, 0, NULL}, {1680, 0, 0, NULL},
            {8000, 0, 0, "apollo-scan-hint"},
            {9000, PAD_CIRCLE, 0, NULL}, {9050, 0, 0, NULL},                 /* a tap between two titles: missed */
            {10000, PAD_CIRCLE, 0, NULL}, {10450, 0, 0, "apollo-cancelling"}, {10700, 0, 0, NULL}, /* held: seen */
            {12000, 0, 0, "apollo-cancelled"}, {12100, PAD_CIRCLE, 0, NULL}, {12180, 0, 0, NULL}, /* no list to go back to */
            {12500, PAD_CROSS, 0, NULL}, {12580, 0, 0, NULL}, {30000, 0, 0, "apollo-resumed"}, {200000, 0, 0, NULL},
        };
        RUN(ev);
    } else if (!strcmp(scene_name, "settings")) { /* every section; Install over network; Close ghostldr: No */
        static const struct event ev[] = {
            {1000, 0, 0, "set-home"},
            {1100, PAD_SQUARE, 0, NULL}, {1180, 0, 0, NULL}, {1700, 0, 0, "set-games"},
            {1800, PAD_RIGHT, 0, NULL}, {1880, 0, 0, NULL}, {2300, 0, 0, "set-games-in"},
            {2400, PAD_DOWN, 0, NULL}, {2480, 0, 0, NULL}, {2600, PAD_DOWN, 0, NULL}, {2680, 0, 0, NULL},
            {2800, PAD_DOWN, 0, NULL}, {2880, 0, 0, NULL}, {3000, PAD_DOWN, 0, NULL}, {3080, 0, 0, NULL},
            {3200, PAD_DOWN, 0, NULL}, {3280, 0, 0, NULL}, {3400, PAD_DOWN, 0, NULL}, {3480, 0, 0, NULL},
            {3600, PAD_DOWN, 0, NULL}, {3680, 0, 0, NULL}, {4200, 0, 0, "set-games-scrolled"},
            {4300, PAD_CIRCLE, 0, NULL}, {4380, 0, 0, NULL},
            {4500, PAD_DOWN, 0, NULL}, {4580, 0, 0, NULL}, {5100, 0, 0, "set-drives"},
            {5200, PAD_DOWN, 0, NULL}, {5280, 0, 0, NULL}, {5800, 0, 0, "set-network"},
            {5900, PAD_RIGHT, 0, NULL}, {5980, 0, 0, NULL}, {6400, 0, 0, "set-network-in"},
            {6500, PAD_CROSS, 0, NULL}, {6580, 0, 0, NULL}, {7200, 0, 0, "set-netinst"},
            {11000, 0, 0, "set-netinst-busy"}, {16500, 0, 0, "set-netinst-done"},
            {16600, PAD_CIRCLE, 0, NULL}, {16680, 0, 0, NULL}, {16800, PAD_LEFT, 0, NULL}, {16880, 0, 0, NULL},
            {17000, PAD_DOWN, 0, NULL}, {17080, 0, 0, NULL}, {17200, PAD_RIGHT, 0, NULL}, {17280, 0, 0, NULL},
            {17800, 0, 0, "set-logs"}, {17900, PAD_CROSS, 0, NULL}, {17980, 0, 0, NULL}, {18500, 0, 0, "set-logs-off"},
            {18600, PAD_CIRCLE, 0, NULL}, {18680, 0, 0, NULL}, {18800, PAD_DOWN, 0, NULL}, {18880, 0, 0, NULL},
            {19300, 0, 0, "set-advanced"}, {19400, PAD_RIGHT, 0, NULL}, {19480, 0, 0, NULL},
            {19600, PAD_CROSS, 0, NULL}, {19680, 0, 0, NULL}, {20100, 0, 0, "set-advanced-off"},  /* dialogs: off */
            {20200, PAD_DOWN, 0, NULL}, {20280, 0, 0, NULL}, {20400, PAD_DOWN, 0, NULL}, {20480, 0, 0, NULL},
            {20600, PAD_DOWN, 0, NULL}, {20680, 0, 0, NULL}, {20800, PAD_CROSS, 0, NULL}, {20880, 0, 0, NULL},
            {21400, 0, 0, "set-advanced-reset"},
            {21500, PAD_CIRCLE, 0, NULL}, {21580, 0, 0, NULL}, {21700, PAD_DOWN, 0, NULL}, {21780, 0, 0, NULL},
            {22300, 0, 0, "set-about"}, {22400, PAD_RIGHT, 0, NULL}, {22480, 0, 0, NULL},
            {22600, PAD_CROSS, 0, NULL}, {22680, 0, 0, NULL}, {23200, 0, 0, "set-close-ask"},
            {23300, PAD_CROSS, 0, NULL}, {23380, 0, 0, NULL},                     /* No (selected first) */
            {23500, PAD_CIRCLE, 0, NULL}, {23580, 0, 0, NULL}, {23700, PAD_CIRCLE, 0, NULL}, {23780, 0, 0, NULL},
            {24500, 0, 0, "set-closed"}, {24600, PAD_L1, 0, NULL}, {24680, 0, 0, NULL}, {25700, 0, 0, NULL},
        };
        RUN(ev);
    } else if (!strcmp(scene_name, "empty")) { /* no title: the gear alone; Cross opens Settings */
        static const struct event ev[] = {
            {1000, 0, 0, "empty"}, {1100, PAD_CROSS, 0, NULL}, {1180, 0, 0, NULL}, {1700, 0, 0, "empty-settings"},
            {1800, PAD_DOWN, 0, NULL}, {1880, 0, 0, NULL}, {2400, 0, 0, "empty-drives"},
            {2500, PAD_CIRCLE, 0, NULL}, {2580, 0, 0, NULL}, {3200, 0, 0, NULL},
        };
        RUN(ev);
    } else if (!strcmp(scene_name, "one")) { /* one title and the gear: Right, then Left, Cross boots it */
        static const struct event ev[] = {
            {1000, 0, 0, "one"}, {1100, PAD_RIGHT, 0, NULL}, {1180, 0, 0, NULL}, {2200, 0, 0, "one-gear"},
            {2300, PAD_LEFT, 0, NULL}, {2380, 0, 0, NULL}, {3400, PAD_CROSS, 0, NULL}, {3480, 0, 0, NULL},
            {4500, 0, 0, NULL},
        };
        RUN(ev);
    } else if (!strcmp(scene_name, "quit")) { /* Close ghostldr: Yes */
        static const struct event ev[] = {
            {1000, PAD_SQUARE, 0, NULL}, {1080, 0, 0, NULL}, {1300, PAD_UP, 0, NULL}, {1380, 0, 0, NULL},
            {1500, PAD_DOWN, 0, NULL}, {1580, 0, 0, NULL}, {1700, PAD_DOWN, 0, NULL}, {1780, 0, 0, NULL},
            {1900, PAD_DOWN, 0, NULL}, {1980, 0, 0, NULL}, {2100, PAD_DOWN, 0, NULL}, {2180, 0, 0, NULL},
            {2200, PAD_DOWN, 0, NULL}, {2280, 0, 0, NULL},
            {2300, PAD_CROSS, 0, NULL}, {2380, 0, 0, NULL}, {2500, PAD_CROSS, 0, NULL}, {2580, 0, 0, NULL},
            {2700, PAD_LEFT, 0, NULL}, {2780, 0, 0, NULL}, {3000, 0, 0, "quit-ask-yes"},
            {3100, PAD_CROSS, 0, NULL}, {3180, 0, 0, NULL}, {5000, 0, 0, NULL},
        };
        RUN(ev);
    } else if (!strcmp(scene_name, "launch")) {
        static const struct event ev[] = {
            {1000, 0, 0, "before"}, {1100, PAD_CROSS, 0, NULL}, {1180, 0, 0, NULL}, {1260, 0, 0, "launching"},
            {3000, 0, 0, NULL},
        };
        RUN(ev);
    } else {
        static const struct event ev[] = {{1000, 0, 0, "single"}};
        RUN(ev);
    }
    fprintf(stderr, "heap peak: %llu MiB (persistent %llu MiB), glyphs rendered %u\n",
            (unsigned long long)(P.peak >> 20), (unsigned long long)(base >> 20), S.fc.misses);
    if (P.frames)
        fprintf(stderr, "frames: %d drawn (%d partial), %.1f ms average, %.1f ms at most (this computer)\n", P.frames,
                P.partial, (double)P.draw_ns / P.frames / 1e6, (double)P.draw_max_ns / 1e6);
    if (P.verify)
        fprintf(stderr, "verify: %d of %d partial frames differ from a whole redraw\n", P.mismatches, P.partial);
    return 0;
}
