/*
 * Game art (Settings > Game art, docs/selector.md "Game art"): a game with
 * no tile picture or backdrop of its own gets them from the services the
 * player gave keys for (plansync/artfetch.c: SteamGridDB, then IGDB),
 * looked up by its name, and keeps them:
 *
 *  - which games: games, not homebrew (TITLE_DUMP), with a name, whose
 *    META, meta.bin or own sce_sys has no icon0.png, or no pic1.png and no
 *    pic0.png; one is looked at per frame, from the selection outwards;
 *  - the job (plansync_sel.c artsync_*) runs on a thread of its own, its
 *    sockets made here, on the boot thread, a few at a time;
 *  - a picture found shows at once (the tile), and goes into the title's
 *    name-and-art file as gfs_meta.py lays it out, QOI at the sizes the
 *    selector draws (the tile 384 x 384, the backdrop 1920 x 1080, both
 *    cropped to fill): <prefix>META for a title in saved data (made when
 *    missing, made again larger when it doesn't fit), meta.bin beside the
 *    app0 of a game on a USB drive.  What the file held (param.sfo's copy,
 *    the name, a picture it had) stays.  The next boots read it from there
 *    and ask no service.
 *
 * Nothing is asked when the switch is off (and run.py's --fetch-art isn't
 * given), or no key is set.
 */
#include "plat.h"
#include "plansync.h"

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int strcmp(const char *a, const char *b);
size_t strlcpy(char *d, const char *s, size_t n);
size_t strlen(const char *s);

int artsync_start(const struct ghost_api *api, const struct ps_art_keys *keys, void **handle);
int artsync_ask(void *handle, const char *name, int icon, int backdrop);
const struct ps_art_got *artsync_poll(void *handle, int slot, int *rc);
void artsync_free(void *handle, int slot);
void artsync_pump(void *handle);
void artsync_stop(void *handle);

#define SAVE_NOT_FOUND ((s32)0x809F0008)

void art_start(struct plat *P)
{
    P->art_nseen = P->art_nreq = P->art_poll = 0; /* every game looked at again */
    int on = ((P->conf_on >> CONF_FETCH_ART) & 1) || (P->ctx->flags & CTX_FLAG_FETCH_ART);
    char(*k)[KEY_MAX] = P->art_key;
    int keys = k[KEY_SGDB][0] || (k[KEY_IGDB_ID][0] && k[KEY_IGDB_SECRET][0]);
    if (!on || !keys || !P->png_decode) {
        if (on)
            LOG(P, "art: %s", !keys ? "on, but no key is set (Settings > Game art): nothing is fetched"
                                    : "on, but no PNG decoder: nothing is fetched");
        return;
    }
    struct ps_art_keys pk;
    memset(&pk, 0, sizeof(pk));
    str_copy(pk.sgdb, k[KEY_SGDB], sizeof(pk.sgdb));
    str_copy(pk.igdb_id, k[KEY_IGDB_ID], sizeof(pk.igdb_id));
    str_copy(pk.igdb_secret, k[KEY_IGDB_SECRET], sizeof(pk.igdb_secret));
    int rc = artsync_start(P->api, &pk, &P->art);
    LOG(P, "art: fetching missing art: %d", rc);
}

void art_stop(struct plat *P)
{
    artsync_stop(P->art);
    P->art = NULL;
}

/* the switch or a key changed in Settings: at once */
void art_restart(struct plat *P)
{
    if (!P->vo_open) /* not running yet (conf_load's own writes) */
        return;
    art_stop(P);
    art_start(P);
}

/* the title has a picture of its own (which: SEL_ICON or SEL_BACKDROP), by
 * its META, else by its files (its index, or its folder) */
static int has_art(struct sel *S, struct plat *P, int i, int which)
{
    struct cat_title *c = &P->cat[i];
    u64 m = sel_mark(S), hn;
    meta_head(S, P, i, &hn);
    sel_release(S, m);
    if (c->meta_state == 1) {
        if (c->meta.s[which == SEL_ICON ? META_ICON : META_BACKDROP].size)
            return 1;
        if (c->meta.flags & META_SCANNED)
            return 0;
    }
    if (which == SEL_ICON)
        return cat_file(S, P, i, FILE_ICON, NULL, 0) > 0;
    return cat_file(S, P, i, FILE_PIC, NULL, 0) > 0 || cat_file(S, P, i, FILE_PIC0, NULL, 0) > 0;
}

/* ---- keeping it: the title's META or meta.bin, written whole ---- */

static int write_all(struct plat *P, const char *path, const u8 *d, u64 n)
{
    int fd = P->k->open(path, 0x601 /* O_WRONLY | O_CREAT | O_TRUNC */, 0666);
    if (fd < 0)
        return fd;
    u64 done = 0;
    while (done < n) {
        ssize_t w = P->k->write(fd, d + done, n - done);
        if (w <= 0)
            break;
        done += (u64)w;
    }
    int c = P->k->close(fd);
    return done == n ? c : -1;
}

/* the container (create: made with room for n bytes, as gfs_meta.py container_blocks): 0, or an SCE error */
static int put_meta(struct plat *P, const char *dir, const u8 *d, u64 n, int create)
{
    char mp[16], path[64];
    u64 blocks = (2 * n + (4u << 20) + 32767) / 32768;
    int rc = P->api->gfs_mount(dir, create ? 6 : 2, create ? (blocks < 512 ? 512 : blocks) : 0, mp);
    if (rc != 0)
        return rc;
    P->mounts++;
    fmt(path, sizeof(path), "%s/meta.bin", mp);
    rc = write_all(P, path, d, n);
    int u = P->api->gfs_umount(mp, 2000); /* the commit */
    P->umounts++;
    return rc ? rc : u;
}

static int keep(struct plat *P, int i, const u8 *d, u64 n)
{
    struct cat_title *c = &P->cat[i];
    if (P->library) {
        char path[320];
        strlcpy(path, c->path, c->dir_len + 1u < sizeof(path) ? c->dir_len + 1u : sizeof(path));
        fmt(path + strlen(path), sizeof(path) - strlen(path), "/meta.bin");
        return write_all(P, path, d, n);
    }
    if (!P->ghostfs || !c->prefix[0])
        return -1;
    char dir[32];
    fmt(dir, sizeof(dir), "%sMETA", c->prefix);
    int rc = put_meta(P, dir, d, n, 0);
    if (rc == SAVE_NOT_FOUND)
        return put_meta(P, dir, d, n, 1);
    if (rc != 0) { /* too small for it now: made again, larger */
        LOG(P, "art: %s: %x: made again, larger", dir, (u32)rc);
        int del = cat_sd_delete(P, dir);
        rc = del == 0 || del == SAVE_NOT_FOUND ? put_meta(P, dir, d, n, 1) : del;
    }
    return rc;
}

/* The title's file with these pictures (QOI, n bytes each; NULL: as it
 * was) in it: what it held kept.  0, or < 0. */
static int store(struct sel *S, struct plat *P, int i, const u8 *icon, u32 icon_n, u8 icon_src, const u8 *bg,
                 u32 bg_n, u8 bg_src)
{
    struct cat_title *c = &P->cat[i];
    const struct ghost_api *a = P->api;
    struct meta_info mi;
    memset(&mi, 0, sizeof(mi));
    u8 *old = NULL;
    u64 old_n = 0;
    if (c->meta_state == 1 && c->meta.size >= META_HEADER && c->meta.size <= (64u << 20)) {
        old_n = c->meta.size;
        old = a->page_alloc(old_n);
        if (old && cat_meta_read(P, i, 0, old, old_n) == (s64)old_n && meta_parse(old, old_n, &mi) == 0) {
        } else {
            if (old)
                a->page_free(old, old_n);
            old = NULL, old_n = 0;
            memset(&mi, 0, sizeof(mi));
        }
    }
    const u8 *sect[META_SECTIONS] = {0};
    u32 len[META_SECTIONS] = {0};
    for (int s = 0; s < META_SECTIONS && old; s++)
        if (mi.s[s].size)
            sect[s] = old + mi.s[s].off, len[s] = mi.s[s].size;
    if (icon) {
        sect[META_ICON] = icon, len[META_ICON] = icon_n;
        mi.s[META_ICON] = (struct meta_sect){0, icon_n, TILE_BIG, TILE_BIG, META_FMT_QOI, icon_src};
    }
    if (bg) {
        sect[META_BACKDROP] = bg, len[META_BACKDROP] = bg_n;
        mi.s[META_BACKDROP] = (struct meta_sect){0, bg_n, SCREEN_W, SCREEN_H, META_FMT_QOI, bg_src};
    }
    u64 cap = META_HEADER + (u64)len[0] + len[1] + len[2] + 48;
    u8 *out = a->page_alloc(cap);
    s64 tv[2] = {0, 0};
    if (P->gettimeofday)
        P->gettimeofday(tv, NULL);
    u64 n = out ? meta_build(&mi, c->id, (u64)tv[0], sect, len, out, cap) : 0;
    int rc = n ? keep(P, i, out, n) : -1;
    if (out)
        a->page_free(out, cap);
    if (old)
        a->page_free(old, old_n);
    LOG(P, "art: title %d (%s): %s%s, %lu KiB: %x", i, c->id, P->library ? "meta.bin" : c->prefix,
        P->library ? "" : "META", n >> 10, (u32)rc);
    if (rc == 0)
        c->meta_state = 0; /* read again: the next loads come from it */
    return rc;
}

/* a PNG file as a w x h picture, cropped to fill, QOI-encoded into page
 * memory (*out, *out_n; freed by the caller), drawn into the tiles first
 * when it is the icon */
static int picture(struct sel *S, struct plat *P, int i, const u8 *png, u64 n, int w, int h, int icon, u8 src,
                   u8 **out, u32 *out_n, u64 *cap)
{
    u64 m = sel_mark(S);
    surface img, fit;
    *out = NULL;
    int ok = png_decode_to(S, P, png, n, &img) == 0 && img.w >= 16 && img.h >= 16;
    if (ok) {
        fit = (surface){sel_alloc(S, (u64)w * (u64)h * 4), w, h, w, 0, 0};
        ok = fit.px != NULL;
    }
    if (ok) {
        draw_cover(&fit, &img);
        if (icon)
            ui_set_icon(S, i, &fit, src);
        *cap = qoi_max(w, h);
        *out = P->api->page_alloc(*cap);
        if (*out)
            *out_n = (u32)qoi_encode(&fit, *out);
    }
    sel_release(S, m);
    return *out ? 0 : -1;
}

/* the job's answer for title i: shown, kept */
static void apply(struct sel *S, struct plat *P, int i, const struct ps_art_got *g)
{
    struct title *t = &S->titles[i];
    u8 *icon = NULL, *bg = NULL;
    u32 icon_n = 0, bg_n = 0;
    u64 icon_cap = 0, bg_cap = 0;
    u8 isrc = g->icon_from == PS_ART_SGDB ? ART_SGDB : ART_IGDB, bsrc = g->backdrop_from == PS_ART_SGDB ? ART_SGDB : ART_IGDB;
    if (g->icon)
        picture(S, P, i, g->icon, g->icon_len, TILE_BIG, TILE_BIG, 1, isrc, &icon, &icon_n, &icon_cap);
    if (g->backdrop)
        picture(S, P, i, g->backdrop, g->backdrop_len, SCREEN_W, SCREEN_H, 0, bsrc, &bg, &bg_n, &bg_cap);
    if (icon || bg) {
        P->art_fetched++;
        int rc = store(S, P, i, icon, icon_n, isrc, bg, bg_n, bsrc);
        if (bg && rc == 0) { /* the backdrop from the file: prepared again when it shows */
            t->bg_state = 0;
            if (S->bg_title == i)
                S->bg_title = -2;
        }
    }
    LOG(P, "art: title %d (%s '%s'): icon %s, backdrop %s", i, t->id, t->name,
        icon ? (isrc == ART_SGDB ? "SteamGridDB" : "IGDB") : g->icon ? "unreadable" : "none",
        bg ? (bsrc == ART_SGDB ? "SteamGridDB" : "IGDB") : g->backdrop ? "unreadable" : "none");
    if (icon)
        P->api->page_free(icon, icon_cap);
    if (bg)
        P->api->page_free(bg, bg_cap);
    S->changed |= CH_ALL;
}

static int seen(const struct plat *P, const char *id)
{
    for (int k = 0; k < P->art_nseen; k++)
        if (!strcmp(P->art_seen[k], id))
            return 1;
    return 0;
}

/* one step of the selector's loop: sockets for the job, its next answer
 * taken, one more game looked at */
void art_step(struct sel *S, struct plat *P)
{
    if (!P->art)
        return;
    artsync_pump(P->art);
    int rc = 0;
    const struct ps_art_got *g = P->art_poll < P->art_nreq ? artsync_poll(P->art, P->art_poll, &rc) : NULL;
    if (g) { /* its title by ID: the row may have been read again since */
        int i = -1;
        for (int k = 0; k < S->ntitles && i < 0; k++)
            if (!strcmp(S->titles[k].id, P->art_req[P->art_poll]))
                i = k;
        if (i >= 0 && rc == 0)
            apply(S, P, i, g);
        else
            LOG(P, "art: %s: %s", P->art_req[P->art_poll], i < 0 ? "not in the row any more" : "nothing found");
        artsync_free(P->art, P->art_poll++);
        return;
    }
    /* from the selection outwards: the next game not looked at yet */
    for (int d = 0; d < 2 * SEL_MAX_TITLES; d++) {
        int i = d & 1 ? S->sel - (d + 1) / 2 : S->sel + d / 2;
        if (i < 0 || i >= S->ntitles)
            continue;
        struct title *t = &S->titles[i];
        if (!t->id[0] || seen(P, t->id) || P->art_nseen >= (int)(sizeof(P->art_seen) / sizeof(P->art_seen[0])))
            continue;
        str_copy(P->art_seen[P->art_nseen++], t->id, 16);
        if (!(t->flags & TITLE_DUMP) || !t->name[0] || !strcmp(t->name, t->id))
            return; /* homebrew, or nothing to look it up by */
        int icon = !has_art(S, P, i, SEL_ICON), bg = !has_art(S, P, i, SEL_BACKDROP);
        if ((icon || bg) && P->art_nreq < SEL_MAX_TITLES) {
            int slot = artsync_ask(P->art, t->name, icon, bg);
            if (slot >= 0)
                str_copy(P->art_req[P->art_nreq++], t->id, 16);
            LOG(P, "art: title %d (%s '%s') lacks%s%s: asked (%d)", i, t->id, t->name, icon ? " a picture" : "",
                bg ? " a backdrop" : "", slot);
        }
        return;
    }
}
