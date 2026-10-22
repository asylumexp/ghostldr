/*
 * Offline preview of the game selector: runs the module's own UI code
 * (ghostldr/native/selector/src/{ui,draw,font,sfo}.c) on this computer
 * against a fake catalogue, and writes PNG screenshots.
 *
 *   preview --font notosans.glf --out DIR [--art TITLE=DIR]... [--titles N] [--scene NAME]
 *
 * Art: a title's icon0.png / pic1.png come from --art TITLE=DIR (a dump's
 * sce_sys directory) when given; otherwise the harness draws its own
 * placeholder art (gradients and shapes) or none, to exercise the fallbacks.
 * PNG decoding here uses zlib; on the console the module uses libScePngDec.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "png_host.h"
#include "sel.h"

/* ---- platform: heap ---- */
struct plat {
    u8 *heap;
    u64 size, used, peak;
    struct art {
        const char *id, *dir;
    } art[16];
    int nart;
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
    out->w = (int)w, out->h = (int)h, out->stride = (int)w;
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
    out->w = w, out->h = h, out->stride = w;
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

int sel_load_png(struct sel *S, int t, const char *path, surface *out)
{
    struct plat *P = S->plat;
    const struct title *ti = &S->titles[t];
    for (int i = 0; i < P->nart; i++)
        if (!strcmp(P->art[i].id, ti->id)) {
            char full[1024];
            const char *leaf = strrchr(path, '/');
            snprintf(full, sizeof(full), "%s/%s", P->art[i].dir, leaf ? leaf + 1 : path);
            FILE *f = fopen(full, "rb");
            if (!f)
                return -1;
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            u8 *d = sel_alloc(S, (u64)n);
            int ok = d && fread(d, 1, (size_t)n, f) == (size_t)n;
            fclose(f);
            return ok ? png_decode(S, d, (u64)n, out) : -1;
        }
    int kind = P->procedural[t];
    if (!kind)
        return -1;
    int big = strstr(path, "pic1") != NULL;
    S->plat_title = t;
    if (kind < 0 && big) /* icon only */
        return -1;
    return procedural(S, kind < 0 ? -kind : kind, big, out);
}

/* ---- fake catalogue ---- */
struct fake {
    const char *id, *name, *version, *prefix;
    double gib;
    int complete, art;
};
static const struct fake FAKES[] = {
    {"CUSA11456", "Ghost of Tsushima", "02.18", "GHOST", 53.3, 1, 0},
    {"CUSA00001", "Starfall Odyssey", "01.05", "G00001", 41.2, 1, 2},
    {"CUSA00002", "Neon Drift: Night Circuit", "01.12", "G00002", 18.7, 1, 3},
    {"CUSA00003", "Harbor Lights", "01.00", "G00003", 9.4, 1, 4},
    {"CUSA00004", "Cascade Valley", "02.01", "G00004", 27.9, 1, 1},
    {"CUSA00005", "Ironclad Tactics II", "01.03", "G00005", 6.1, 1, 0},
    {"CUSA00006", "\xE6\x98\x9F\xE3\x81\xAE\xE5\xAE\x88\xE3\x82\x8A\xE4\xBA\xBA", "01.00", "G00006", 12.0, 1, -5},
    {"CUSA00007", "Emberfall Chronicles", "", "G00007", 0, 0, 5},
};

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
            if (sel_load_png(S, i, big ? "sce_sys/pic1.png" : "sce_sys/icon0.png", &img) == 0) {
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

static void run(struct sel *S, const struct event *ev, int nev, const char *outdir, const char *prefix)
{
    u64 now = 0, step = 33333;
    int e = 0, boot = -1;
    u32 buttons = 0;
    int stick = 128;
    for (int frame = 0; frame < 30 * 60 && e < nev; frame++) {
        while (e < nev && (u64)ev[e].ms * 1000 <= now) {
            if (ev[e].shot) {
                ui_draw(S);
                char path[1024];
                snprintf(path, sizeof(path), "%s/%s%s.png", outdir, prefix, ev[e].shot);
                png_write(path, &S->back);
                fprintf(stderr, "wrote %s\n", path);
            } else {
                buttons = ev[e].buttons, stick = ev[e].stick ? ev[e].stick : 128;
            }
            e++;
        }
        if (ui_step(S, now, step, buttons, stick, &boot))
            ui_draw(S);
        if (boot >= 0) {
            fprintf(stderr, "boot title %d (%s) at %llu ms\n", boot, S->titles[boot].id, (unsigned long long)(now / 1000));
            break;
        }
        now += step;
    }
}

int main(int argc, char **argv)
{
    const char *font = NULL, *out = ".", *scene_name = "tour", *prefix = "", *write_art = NULL;
    int ntitles = (int)(sizeof(FAKES) / sizeof(FAKES[0])), clock_h = 21, clock_m = 47, h24 = 1;
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
        else if (!strcmp(argv[i], "--write-art") && i + 1 < argc)
            write_art = argv[++i];
        else if (!strcmp(argv[i], "--art") && i + 1 < argc && P.nart < 16) {
            char *a = argv[++i], *eq = strchr(a, '=');
            if (eq)
                *eq = 0, P.art[P.nart].id = a, P.art[P.nart++].dir = eq + 1;
        } else {
            fprintf(stderr, "usage: preview --font F.glf --out DIR [--scene tour|info|single|launch] [--titles N] "
                            "[--art TITLE=DIR] [--12h] [--prefix P]\n");
            return 2;
        }
    }
    u64 fn = 0;
    u8 *glf = font ? read_file(font, &fn) : NULL;
    if (!glf) {
        fprintf(stderr, "need --font (ghostldr/native/selector/font/notosans.glf)\n");
        return 2;
    }
    if (ntitles < 1 || ntitles > (int)(sizeof(FAKES) / sizeof(FAKES[0])))
        ntitles = (int)(sizeof(FAKES) / sizeof(FAKES[0]));
    P.size = 192ull << 20;
    P.heap = malloc(P.size);
    static struct sel S;
    S.plat = &P;
    struct title *titles = sel_alloc(&S, sizeof(struct title) * ntitles);
    for (int i = 0; i < ntitles; i++) {
        const struct fake *f = &FAKES[i];
        str_copy(titles[i].id, f->id, 16);
        str_copy(titles[i].name, f->name, 128);
        str_copy(titles[i].version, f->version, 16);
        str_copy(titles[i].prefix, f->prefix, 16);
        titles[i].size = (u64)(f->gib * (1u << 30));
        titles[i].flags = f->complete ? TITLE_COMPLETE : 0;
        P.procedural[i] = f->art;
    }
    if (font_init(&S, glf, 2u << 20) || ui_init(&S, titles, ntitles, 0)) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    ui_set_clock(&S, clock_h, clock_m, h24);
    if (write_art) /* sample art for selector_host: DIR/t<N>/sce_sys/{icon0,pic1}.png */
        return dump_art(&S, write_art, ntitles);
    u64 base = P.used;
    fprintf(stderr, "heap after init: %llu MiB\n", (unsigned long long)(base >> 20));

    if (!strcmp(scene_name, "tour")) {
        static const struct event ev[] = {
            {1000, 0, 0, "home"},
            {1100, PAD_RIGHT, 0, NULL}, {1180, 0, 0, NULL}, {1230, 0, 0, "moving"},
            {2200, 0, 0, "second"},
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
        run(&S, ev, sizeof(ev) / sizeof(ev[0]), out, prefix);
    } else if (!strcmp(scene_name, "info")) {
        static const struct event ev[] = {
            {800, PAD_RIGHT, 0, NULL}, {880, 0, 0, NULL},
            {2000, PAD_OPTIONS, 0, NULL}, {2080, 0, 0, NULL}, {2600, 0, 0, "info"},
            {2700, PAD_CIRCLE, 0, NULL}, {2780, 0, 0, NULL}, {3300, 0, 0, "info-closed"},
        };
        run(&S, ev, sizeof(ev) / sizeof(ev[0]), out, prefix);
    } else if (!strcmp(scene_name, "launch")) {
        static const struct event ev[] = {
            {1000, 0, 0, "before"}, {1100, PAD_CROSS, 0, NULL}, {1180, 0, 0, NULL}, {1260, 0, 0, "launching"},
            {3000, 0, 0, NULL},
        };
        run(&S, ev, sizeof(ev) / sizeof(ev[0]), out, prefix);
    } else {
        static const struct event ev[] = {{1000, 0, 0, "single"}};
        run(&S, ev, 1, out, prefix);
    }
    fprintf(stderr, "heap peak: %llu MiB (persistent %llu MiB), glyphs rendered %u\n",
            (unsigned long long)(P.peak >> 20), (unsigned long long)(base >> 20), S.fc.misses);
    return 0;
}
