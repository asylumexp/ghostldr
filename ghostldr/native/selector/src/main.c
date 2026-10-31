/*
 * The game selector module: entry point and console side.
 *
 * sel_main() runs on the boot thread before the title is loaded.  It
 *   1. reads the catalogue of installed titles (cat.c) and returns at once
 *      when there is nothing to pick;
 *   2. allocates two whole direct-memory blocks at the top of the largest
 *      free run: the heap (write-back: frames are drawn there) and the video
 *      block (write-combined, GPU-readable: two 1920x1080 scan-out buffers);
 *   3. opens VideoOut (through the core's override, which takes the output
 *      over from SWRR when needed) and reads the shared pad;
 *   4. runs the UI (ui.c) at up to 30 frames per second, redrawing only when
 *      something moves; for a save tool (its profile's "save_tool"), asks
 *      whose saves to open, mounts the title (ghost_api.title_pick) and runs
 *      the save scan (ghost_api.save_prescan) with its progress on screen;
 *      Settings (setui.c, info.c) at the end of the row, also with one
 *      title or none: the drive check, the network installer (whose new
 *      titles join the row), the log on the USB drive, deleting a game
 *      installed in saved data, Close ghostldr; Refresh (Triangle) reads
 *      the row again, and Game art (art.c) fetches missing art meanwhile;
 *   5. tears everything down: closes VideoOut, the system font, unloads the
 *      system modules it loaded, and unmaps and releases both blocks, then
 *      returns the chosen title.  The core then overwrites this image.
 *
 * No writable static data: everything lives in struct plat (on this stack)
 * and struct sel (in the heap).
 */
#include "plat.h"
#include "plansync.h"

/* community memory plans (plansync/plansync_sel.c, docs/community-plans.md) */
int plansync_start(const struct ghost_api *api, const char (*titles)[16], unsigned n, int share, int fetch,
                   void **handle);
const struct ps_job *plansync_poll(void *handle);
void plansync_stop(void *handle);

/* the plans the job found: offered in their games' Information panels */
static void community_offer(struct sel *S, struct plat *P, const struct ps_job *job)
{
    for (unsigned k = 0; k < job->nplans; k++) {
        const struct ps_plan *p = &job->plans[k];
        if ((int)p->platform != (P->ps4 ? SEED_PS4 : SEED_PS5))
            continue;
        for (int i = 0; i < S->ntitles; i++) {
            struct title *t = &S->titles[i];
            if (strcmp(t->id, p->title) != 0)
                continue;
            if (!t->community)
                t->community_off = (u8)conf_community_off(P, t->id);
            t->community = 1;
            if (p->contributors > t->community_from)
                t->community_from = p->contributors;
            S->changed |= CH_ALL;
        }
    }
    LOG(P, "plans: %u community plans for the games here (GitHub: HTTP %d); %u shared, %u waiting", job->nplans,
        job->github, job->shared, job->unshared);
}

/* the picked game's downloaded plans to the core (ghost_api.seed_add), unless
 * community plans are off for this boot (conf_start decided) */
static void community_give(struct plat *P, const struct ps_job *job, const char *id)
{
    if (!job || (P->ctx->flags & CTX_FLAG_NO_COMMUNITY_MEM) || !P->api->seed_add)
        return;
    for (unsigned k = 0; k < job->nplans; k++) {
        const struct ps_plan *p = &job->plans[k];
        if (strcmp(p->title, id) == 0 && (int)p->platform == (P->ps4 ? SEED_PS4 : SEED_PS5))
            P->api->seed_add(p->title, p->fingerprint, p->platform, p->contributors, p->plan, PS_PLAN_SIZE);
    }
}

/* ---- logging and formatting (rt.c's vfmt) ---- */
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int memcmp(const void *a, const void *b, size_t n);
size_t strlcpy(char *d, const char *s, size_t n);

void sel_log(struct sel *S, const char *f, ...)
{
    char line[400];
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    vfmt(line, sizeof(line), f, ap);
    __builtin_va_end(ap);
    PLAT(S)->api->logf("selector: %s", line);
}

int sel_fmt(char *buf, int size, const char *f, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int n = vfmt(buf, (size_t)size, f, ap);
    __builtin_va_end(ap);
    return n;
}

/* ---- the heap: a bump allocator inside the heap block ---- */
void *sel_alloc(struct sel *S, u64 size)
{
    struct plat *P = PLAT(S);
    u64 at = (P->heap_used + 63) & ~63ull;
    if (at + size > P->heap_size) {
        LOG(P, "heap full (%lu + %lu of %lu bytes)", at, size, P->heap_size);
        return NULL;
    }
    P->heap_used = at + size;
    if (P->heap_used > P->heap_peak)
        P->heap_peak = P->heap_used;
    memset(P->heap + at, 0, size);
    return P->heap + at;
}
u64 sel_mark(struct sel *S) { return PLAT(S)->heap_used; }
void sel_release(struct sel *S, u64 mark) { PLAT(S)->heap_used = mark; }

/* ---- direct memory: whole blocks at the top of the largest free run ---- */
static int (*release_fn(struct plat *P))(off_t, size_t)
{
    char name[40];
    strlcpy(name, "sceKernelReleaseDirectMemory", sizeof(name));
    return (int (*)(off_t, size_t))P->api->host_dlsym((u32)P->ctx->libkernel_handle, name);
}

static void *block_alloc(struct plat *P, u64 len, u64 below, int type, int prot, u64 *phys)
{
    off_t ph = 0;
    int rc = P->k->alloc_direct(below > len ? (off_t)(below - len) : 0, (off_t)below, len, 0x200000, type, &ph);
    if (rc != 0) /* not where we wanted it: anywhere */
        rc = P->k->alloc_direct(0, (off_t)P->k->direct_size(), len, 0x200000, type, &ph);
    if (rc != 0) {
        LOG(P, "cannot allocate %lu MiB of direct memory (type %d): %x", len >> 20, type, rc);
        return NULL;
    }
    void *va = NULL;
    rc = P->k->map_direct(&va, len, prot, 0, ph, 0x200000);
    if (rc != 0) {
        LOG(P, "cannot map %lu MiB at phys %lx: %x", len >> 20, (u64)ph, rc);
        release_fn(P)(ph, len);
        return NULL;
    }
    *phys = (u64)ph;
    return va;
}

static u64 block_free(struct plat *P, void *va, u64 len, u64 phys, const char *what)
{
    if (!va)
        return 0;
    int u = P->k->munmap(va, len);
    int r = release_fn(P)((off_t)phys, len);
    LOG(P, "freed %s: %lu MiB at %p (phys %lx): munmap %x, release %x", what, len >> 20, va, phys, u, r);
    return r == 0 ? len : 0;
}

/* ---- system modules ---- */
u32 sys_module(struct plat *P, const char *file, int load, u32 *loaded)
{
    char name[64];
    strlcpy(name, file, sizeof(name));
    u32 handles[256];
    size_t count = 0;
    struct sce_module_info info;
    if (P->k->module_list && P->k->module_list(handles, 256, &count) == 0)
        for (size_t i = 0; i < count; i++) {
            memset(&info, 0, sizeof(info));
            info.size = sizeof(info);
            if (P->k->module_info(handles[i], &info) == 0 && strcmp(info.name, name) == 0)
                return handles[i];
        }
    if (!load)
        return 0;
    int res = 0;
    s32 h = (s32)P->api->call_gadget6((u64)P->k->load_start_module, (u64)name, 0, 0, 0, 0, (u64)&res);
    LOG(P, "loaded %s: handle %x", name, (u32)h);
    if (h <= 0)
        return 0;
    if (loaded)
        *loaded = (u32)h;
    return (u32)h;
}

void sys_unload(struct plat *P, u32 *handle, const char *what)
{
    if (!*handle)
        return;
    char name[48];
    strlcpy(name, "sceKernelStopUnloadModule", sizeof(name));
    u64 fn = (u64)P->api->host_dlsym((u32)P->ctx->libkernel_handle, name);
    int res = 0;
    s32 rc = fn ? (s32)P->api->call_gadget6(fn, *handle, 0, 0, 0, 0, (u64)&res) : -1;
    LOG(P, "unloaded %s (handle %x): %x", what, *handle, (u32)rc);
    *handle = 0;
}

void *sys_sym(struct plat *P, u32 handle, const char *name)
{
    char n[64];
    strlcpy(n, name, sizeof(n));
    void *fn = handle ? P->api->host_dlsym(handle, n) : NULL;
    if (!fn)
        LOG(P, "missing %s", n);
    return fn;
}

static const char *name_src(u8 src)
{
    return src == NAME_SFO ? "param.sfo" : src == NAME_PROFILE ? "profile" : src == NAME_GIVEN ? "given"
         : src == NAME_STORE ? "store" : src == NAME_CATALOGUE ? "catalogue" : "none";
}

/* ---- a title's art ---- */
#define PNG_FILE_MAX (8u << 20)
#define META_HEAD (1u << 20) /* read with META's header: param.sfo and the icon fit in it */

/* TITLE_* from what the catalogue (or the extension's list) and param.sfo say */
static u32 title_flags(const struct cat_title *c)
{
    return (c->flags & (CAT_INCOMPLETE | CAT_BAD) ? 0 : TITLE_COMPLETE) | (c->flags & CAT_DUP ? TITLE_COPY : 0) |
           (title_is_dump(c->id, c->sfo_id, c->content_id) ? TITLE_DUMP : 0) |
           (c->prof_flags & PF_SAVE_TOOL ? TITLE_SAVE_TOOL : 0);
}

/* what reading a title's META, index or param.sfo taught us */
static void sync_title(struct sel *S, struct plat *P, int i)
{
    struct title *t = &S->titles[i];
    const struct cat_title *c = &P->cat[i];
    if (c->name[0])
        str_copy(t->name, c->name, sizeof(t->name));
    if (!t->version[0] && c->version[0])
        str_copy(t->version, c->version, sizeof(t->version));
    if (!t->id[0] && c->id[0])
        str_copy(t->id, c->id, sizeof(t->id));
    t->flags |= title_flags(c) & TITLE_SAVE_TOOL; /* an ID first learnt from param.sfo */
    if (c->seed && !t->community)                 /* its profile, by that ID */
        t->community = 1, t->community_from = c->seed_from, t->community_off = (u8)conf_community_off(P, t->id);
    u32 dump = title_flags(c) & TITLE_DUMP;
    if ((t->flags & TITLE_DUMP) != dump) {
        LOG(P, "title %d (%s): %s, by its param.sfo (%s, %s)", i, c->id, dump ? "a game's dump" : "homebrew",
            c->sfo_id[0] ? c->sfo_id : "no TITLE_ID", c->content_id[0] ? c->content_id : "no CONTENT_ID");
        t->flags = (t->flags & ~TITLE_DUMP) | dump;
        S->changed |= CH_ALL;
    }
}

/* <prefix>META: its header once (with whatever follows it in the first MiB) */
const u8 *meta_head(struct sel *S, struct plat *P, int i, u64 *n)
{
    struct cat_title *c = &P->cat[i];
    *n = 0;
    if (c->meta_state || (P->ghostfs ? !c->prefix[0] : !P->library)) {
        if (!c->meta_state)
            c->meta_state = 2;
        return NULL;
    }
    u8 *buf = sel_alloc(S, META_HEAD);
    s64 got = buf ? cat_meta_read(P, i, 0, buf, META_HEAD) : -1;
    if (got >= META_HEADER && meta_parse(buf, (u64)got, &c->meta) == 0) {
        c->meta_state = 1;
        cat_meta_names(P, i, buf, (u64)got);
        LOG(P, "title %d (%s): %s%s: '%s' (%s), icon %ux%u, backdrop %ux%u, param.sfo %u bytes%s", i, c->id,
            c->prefix, P->library ? "meta.bin" : "META", c->meta.name, c->meta.version, c->meta.s[META_ICON].w, c->meta.s[META_ICON].h,
            c->meta.s[META_BACKDROP].w, c->meta.s[META_BACKDROP].h, c->meta.s[META_SFO].size,
            c->meta.flags & META_SCANNED ? "" : ", not scanned");
        *n = (u64)got;
        return buf;
    }
    c->meta_state = 2;
    return NULL;
}

static int load_qoi(struct sel *S, const u8 *d, u64 n, surface *out)
{
    int w, h;
    if (qoi_header(d, n, &w, &h) != 0)
        return -1;
    *out = (surface){sel_alloc(S, (u64)w * (u64)h * 4), w, h, w, 0, 0};
    return out->px ? qoi_decode(d, n, out) : -1;
}

int sel_load_art(struct sel *S, int title, int which, surface *out)
{
    struct plat *P = PLAT(S);
    struct cat_title *c = &P->cat[title];
    struct title *t = &S->titles[title];
    u8 *src = which == SEL_ICON ? &t->icon_src : &t->bg_src;
    u64 hn = 0;
    const u8 *head = meta_head(S, P, title, &hn);
    sync_title(S, P, title);
    if (c->meta_state == 1) {
        const struct meta_sect *m = &c->meta.s[which == SEL_ICON ? META_ICON : META_BACKDROP];
        if (m->size && m->format == META_FMT_QOI) {
            const u8 *d = NULL;
            if (head && (u64)m->off + m->size <= hn) {
                d = head + m->off;
            } else {
                u8 *b = sel_alloc(S, m->size);
                if (b && cat_meta_read(P, title, m->off, b, m->size) == (s64)m->size)
                    d = b;
            }
            if (d && load_qoi(S, d, m->size, out) == 0) {
                *src = m->source;
                return 0;
            }
            LOG(P, "title %d: %sMETA's %s is unreadable", title, c->prefix, which == SEL_ICON ? "icon" : "backdrop");
        }
        if (c->meta.flags & META_SCANNED)
            return -1; /* the title has none of its own: no need to search its index */
    }
    /* the title's own sce_sys PNGs, through its index (installs without META) */
    if (!P->png_decode)
        return -1;
    int files[2] = {which == SEL_ICON ? FILE_ICON : FILE_PIC, which == SEL_ICON ? -1 : FILE_PIC0};
    for (int k = 0; k < 2 && files[k] >= 0; k++) {
        s64 n = cat_file(S, P, title, files[k], NULL, 0);
        sync_title(S, P, title);
        if (n <= 0 || (u64)n > PNG_FILE_MAX)
            continue;
        u8 *data = sel_alloc(S, (u64)n);
        if (!data || cat_file(S, P, title, files[k], data, (u64)n) != n)
            continue;
        if (png_decode_to(S, P, data, (u64)n, out) == 0) {
            *src = ART_TITLE;
            return 0;
        }
    }
    return -1;
}

/* ---- VideoOut ---- */
#define FB_BYTES ((u64)SCREEN_W * SCREEN_H * 4)
#define FB_SLOT (((FB_BYTES) + 0x1FFFFF) & ~0x1FFFFFull)

struct vo_attr {
    u32 pixel_format, tiling, aspect, width, height, pitch, option, reserved0;
    u64 reserved1;
};

static int video_open(struct plat *P)
{
    u32 h = sys_module(P, "libSceVideoOut.sprx", 0, NULL);
    /* the core's override takes the output over from SWRR when needed */
    s32 (*open)(s32, s32, s32, const void *) =
        (s32(*)(s32, s32, s32, const void *))P->api->hle_lookup("libSceVideoOut", "sceVideoOutOpen");
    if (!open)
        open = (s32(*)(s32, s32, s32, const void *))sys_sym(P, h, "sceVideoOutOpen");
    void (*set_attr)(struct vo_attr *, u32, u32, u32, u32, u32, u32) =
        (void (*)(struct vo_attr *, u32, u32, u32, u32, u32, u32))sys_sym(P, h, "sceVideoOutSetBufferAttribute");
    s32 (*reg)(s32, s32, void *const *, s32, const struct vo_attr *) =
        (s32(*)(s32, s32, void *const *, s32, const struct vo_attr *))sys_sym(P, h, "sceVideoOutRegisterBuffers");
    s32 (*rate)(s32, s32) = (s32(*)(s32, s32))sys_sym(P, h, "sceVideoOutSetFlipRate");
    P->vo_close = (s32(*)(s32))sys_sym(P, h, "sceVideoOutClose");
    P->vo_submit = (s32(*)(s32, s32, s32, s64))sys_sym(P, h, "sceVideoOutSubmitFlip");
    P->vo_pending = (s32(*)(s32))sys_sym(P, h, "sceVideoOutIsFlipPending");
    if (!open || !set_attr || !reg || !P->vo_close || !P->vo_submit || !P->vo_pending)
        return -1;
    P->vo = open(0xFF /* SCE_USER_SERVICE_USER_ID_SYSTEM */, 0 /* main */, 0, NULL);
    if (P->vo < 0) {
        LOG(P, "sceVideoOutOpen -> %x", (u32)P->vo);
        return -1;
    }
    P->vo_open = 1;
    if (rate)
        rate(P->vo, 1); /* 30 Hz */
    struct vo_attr attr;
    memset(&attr, 0, sizeof(attr));
    set_attr(&attr, 0x80000000u /* A8R8G8B8 sRGB */, 1 /* linear */, 0 /* 16:9 */, SCREEN_W, SCREEN_H, SCREEN_W);
    P->fb[0] = (u32 *)P->video;
    P->fb[1] = (u32 *)(P->video + FB_SLOT);
    memset(P->video, 0, P->video_size);
    void *addrs[2] = {P->fb[0], P->fb[1]};
    s32 rc = reg(P->vo, 0, addrs, 2, &attr);
    LOG(P, "VideoOut handle %d, buffers %p %p registered -> %x", P->vo, P->fb[0], P->fb[1], (u32)rc);
    if (rc < 0)
        return -1;
    P->fb_shown = -1;
    P->prev_damage_y0 = 0, P->prev_damage_y1 = SCREEN_H; /* both buffers start black: the first frames are whole */
    return 0;
}

static void wait_flips(struct plat *P, u32 max_ms)
{
    for (u32 ms = 0; ms < max_ms && P->vo_pending(P->vo) > 0; ms++)
        P->k->usleep(1000);
}

/* Show the frame in S->back.  Only rows that changed are copied: the
 * buffer about to be shown last held the frame before the previous one, so
 * it gets this frame's changed rows and the previous frame's. */
static void present(struct sel *S)
{
    struct plat *P = PLAT(S);
    if (!P->vo_open || S->damage_y0 >= S->damage_y1)
        return;
    int next = P->fb_shown == 0 ? 1 : 0;
    int y0 = S->damage_y0 < P->prev_damage_y0 ? S->damage_y0 : P->prev_damage_y0;
    int y1 = S->damage_y1 > P->prev_damage_y1 ? S->damage_y1 : P->prev_damage_y1;
    wait_flips(P, 100); /* the buffer we draw into must not be on its way to the screen */
    memcpy(P->fb[next] + (u64)y0 * SCREEN_W, S->back.px + (u64)y0 * SCREEN_W, (u64)(y1 - y0) * SCREEN_W * 4);
    P->present_rows += (u64)(y1 - y0);
    s32 rc = P->vo_submit(P->vo, next, 1 /* vsync */, (s64)++P->flips);
    if (rc < 0 && P->flips < 4)
        LOG(P, "sceVideoOutSubmitFlip -> %x", (u32)rc);
    P->fb_shown = next;
    P->prev_damage_y0 = S->damage_y0, P->prev_damage_y1 = S->damage_y1;
}

/* draw what changed and show it, timing both */
static void frame(struct sel *S)
{
    struct plat *P = PLAT(S);
    u64 t0 = P->proc_time();
    ui_draw(S);
    u64 t1 = P->proc_time();
    if (S->damage_y0 >= S->damage_y1)
        return;
    present(S);
    u64 t2 = P->proc_time();
    P->frames++;
    P->partial_frames += S->damage_y1 - S->damage_y0 < SCREEN_H;
    P->draw_us += t1 - t0, P->present_us += t2 - t1;
    if (t1 - t0 > P->draw_max_us)
        P->draw_max_us = t1 - t0;
    if (t2 - t1 > P->present_max_us)
        P->present_max_us = t2 - t1;
}

static void video_close(struct plat *P)
{
    if (!P->vo_open)
        return;
    wait_flips(P, 200);
    s32 rc = P->vo_close(P->vo);
    LOG(P, "VideoOut handle %d closed: %x (the title opens its own)", P->vo, (u32)rc);
    P->vo_open = 0;
}

/* Three seconds after the selector first appears, say where the FTP server
 * which boot.c started can be reached.  The request is consumed before this
 * call returns, so it can live on this stack. */
static void ftp_notice(struct plat *P, u32 port)
{
    struct {
        s32 type, req_id, priority, msg_id, target_id, user_id, unk1, unk2, app_id, error_num, unk3;
        u8 use_icon;
        char message[1024];
        char icon[1024];
        char unk[1024];
        u8 pad[3];
    } __attribute__((packed)) r;
    _Static_assert(sizeof(r) == 0xC30, "notification request");
    s32 (*notify)(s32, void *, u64, s32) =
        (s32(*)(s32, void *, u64, s32))P->api->host_dlsym((u32)P->ctx->libkernel_handle,
                                                           "sceKernelSendNotificationRequest");
    char ip[16];
    info_network_ip(P, ip);
    if (!notify || !ip[0]) {
        LOG(P, "FTP service started on %s:%u; notification unavailable", ip[0] ? ip : "unknown IP", port);
        return;
    }
    memset(&r, 0, sizeof(r));
    r.target_id = -1;
    fmt(r.message, sizeof(r.message), "ghostldr: FTP service started on %s:%u", ip, port);
    s32 rc = notify(0, &r, sizeof(r), 0);
    LOG(P, "FTP service started on %s:%u: notification %x", ip, port, (u32)rc);
}

/* ---- pad ---- */
#define PAD_INTERCEPTED 0x80000000u

static int pad_open(struct plat *P, struct sel *S)
{
    u32 us = sys_module(P, "libSceUserService.sprx", 0, NULL), pad = sys_module(P, "libScePad.sprx", 0, NULL);
    s32 (*initial)(s32 *) = (s32(*)(s32 *))sys_sym(P, us, "sceUserServiceGetInitialUser");
    /* the core's override returns SWRR's handle when the pad is already open */
    s32 (*open)(s32, s32, s32, const void *) =
        (s32(*)(s32, s32, s32, const void *))P->api->hle_lookup("libScePad", "scePadOpen");
    s32 (*get)(s32, s32, s32) = open ? NULL : (s32(*)(s32, s32, s32))sys_sym(P, pad, "scePadGetHandle");
    if (!open)
        open = (s32(*)(s32, s32, s32, const void *))sys_sym(P, pad, "scePadOpen");
    P->pad_read = (s32(*)(s32, u8 *))sys_sym(P, pad, "scePadReadState");
    s32 user = -1;
    if (!initial || !open || !P->pad_read || initial(&user) != 0)
        return -1;
    P->user = user;
    P->pad = open(user, 0, 0, NULL);
    if (P->pad < 0 && get)
        P->pad = get(user, 0, 0);
    /* the user's name for the top bar */
    P->us_name = (s32(*)(s32, char *, size_t))sys_sym(P, us, "sceUserServiceGetUserName");
    char name[64];
    memset(name, 0, sizeof(name));
    if (P->us_name && P->us_name(user, name, sizeof(name) - 1) == 0)
        ui_set_user(S, name);
    LOG(P, "pad: user %x handle %x", (u32)user, (u32)P->pad);
    if (P->pad >= 0) { /* a save tool asks whose saves: every user the console has, and who is signed in */
        P->us_registered = (s32(*)(s32 *))sys_sym(P, us, "sceUserServiceGetRegisteredUserIdList");
        P->us_login = (s32(*)(s32 *))sys_sym(P, us, "sceUserServiceGetLoginUserIdList");
    }
    return P->pad >= 0 ? 0 : -1;
}

/* ---- the console's users (a save tool asks whose saves to open) ---- */
static int user_add(struct sel_user *u, int n, s32 id)
{
    if (id == -1 || id == 0 || id == 0xFF || n >= SEL_MAX_USERS) /* none, or SCE_USER_SERVICE_USER_ID_SYSTEM */
        return n;
    for (int i = 0; i < n; i++)
        if (u[i].id == id)
            return n;
    u[n].id = id;
    return n + 1;
}

/* the registered users when SWRR may list them (else the signed-in ones),
 * each with its name and whether it is signed in now; logged when it changes */
static void users_load(struct sel *S, struct plat *P)
{
    struct sel_user u[SEL_MAX_USERS];
    s32 reg[16], login[4];
    memset(u, 0, sizeof(u));
    memset(reg, 0xFF, sizeof(reg));
    memset(login, 0xFF, sizeof(login));
    s32 rr = P->us_registered ? P->us_registered(reg) : -1, lr = P->us_login ? P->us_login(login) : -1;
    if (lr != 0) /* at least the user running SWRR */
        login[0] = P->user;
    int n = 0;
    for (int i = 0; rr == 0 && i < 16; i++)
        n = user_add(u, n, reg[i]);
    for (int i = 0; i < 4; i++)
        n = user_add(u, n, login[i]);
    n = user_add(u, n, P->user);
    for (int i = 0; i < n; i++) {
        u[i].current = u[i].id == P->user;
        u[i].signed_in = u[i].current;
        for (int k = 0; k < 4; k++)
            u[i].signed_in |= login[k] == u[i].id;
        if (P->us_name && P->us_name(u[i].id, u[i].name, sizeof(u[i].name) - 1) != 0)
            u[i].name[0] = 0;
    }
    if (!ui_set_users(S, u, n))
        return;
    LOG(P, "users: %d (registered list %x, signed-in list %x)", n, (u32)rr, (u32)lr);
    for (int i = 0; i < n; i++)
        LOG(P, "user %x '%s'%s%s", (u32)u[i].id, u[i].name, u[i].signed_in ? ", signed in" : ", not signed in",
            u[i].current ? ", running SWRR" : "");
}

/* ---- time ---- */
static void clock_now(struct sel *S, struct plat *P)
{
    struct {
        s64 sec, usec;
    } tv = {0, 0};
    s64 local = 0;
    u8 ts[32];
    u64 dst = 0;
    s32 h24 = 1;
    if (!P->gettimeofday || P->gettimeofday(&tv, NULL) != 0)
        return;
    if (!P->utc2local || P->utc2local(tv.sec, &local, ts, &dst) != 0)
        local = tv.sec;
    if (P->param_int && P->param_int(3 /* time format */, &h24) != 0)
        h24 = 1;
    s64 day = local % 86400;
    if (day < 0)
        day += 86400;
    ui_set_clock(S, (int)(day / 3600), (int)(day / 60 % 60), h24 != 0);
}

/* a title of the row as the catalogue (or the library) has it, tiles aside */
static void title_from(struct plat *P, struct title *t, const struct cat_title *c)
{
    str_copy(t->id, c->id, sizeof(t->id));
    str_copy(t->name, c->name, sizeof(t->name));
    str_copy(t->version, c->version, sizeof(t->version));
    str_copy(t->prefix, P->ghostfs ? c->prefix : "", sizeof(t->prefix));
    t->size = c->total;
    t->flags = title_flags(c);
    t->community = c->seed, t->community_from = c->seed_from;
    t->community_off = (u8)conf_community_off(P, c->id);
    str_copy(t->note, c->note, sizeof(t->note));
    if (P->library) { /* its folder, as the drive shows it: without the mount point and app0 */
        info_title_where(P, c->path, c->dir_len, t->where, (int)sizeof(t->where));
        if (!t->name[0] && !t->id[0]) { /* nothing names it: its folder's name */
            const char *base = t->where;
            for (const char *k = base; *k; k++)
                if (*k == '/' && k[1])
                    base = k + 1;
            str_copy(t->name, base, sizeof(t->name));
        }
    }
}

/* the same title in the row before: by its folder in a library (two copies
 * share an ID), else by its ID */
static int same_title(const struct plat *P, const struct cat_title *a, const struct cat_title *b)
{
    return !strcmp(a->id, b->id) && (!P->library || !strcmp(a->path, b->path));
}

/* ---- the row again: the network installer changed the catalogue, or
 * Refresh (Triangle, Settings' Look again): a library's games looked for
 * again on the drives (ghost_api.library_scan, again), else GFSCATALOG read
 * again.  A title still there keeps its tiles and what was read of it; a
 * new one takes the tiles of one that went, else new ones (the heap has
 * room for ntitle_cap).  The arrays are two of ntitle_cap, in turn.  *added
 * and *gone: how many; -1 in *added when nothing could be read (the row
 * stays). ---- */
static struct title *reload(struct sel *S, struct plat *P, struct title *titles, int again, int *added, int *gone)
{
    *added = *gone = 0;
    if (!P->reload_ok) {
        *added = -1;
        return titles;
    }
    struct cat_title *old = P->cat;
    int had = P->ncat, n;
    if (P->library)
        n = P->api->size >= __builtin_offsetof(struct ghost_api, library_scan) + 8 && P->api->library_scan
                ? P->api->library_scan(&P->lib, &P->ps5, again) : -1;
    else
        n = cat_count(P);
    if (n > P->ntitle_cap) {
        LOG(P, "refresh: %d titles: the first %d are listed (the heap has room for so many)", n, P->ntitle_cap);
        n = P->ntitle_cap;
    }
    int k = P->buf_turn;
    for (int i = 0; i < 2 && !P->title_buf[1]; i++) {
        P->title_buf[i] = sel_alloc(S, sizeof(struct title) * (u64)P->ntitle_cap);
        P->cat_buf[i] = sel_alloc(S, sizeof(struct cat_title) * (u64)P->ntitle_cap);
        if (!P->title_buf[i] || !P->cat_buf[i])
            P->title_buf[0] = P->title_buf[1] = NULL;
    }
    struct title *t = P->title_buf[k];
    P->cat_into = P->cat_buf[k];
    int bad = n < 0 || !t || (n > 0 && cat_load(S, P, n) != 0);
    P->cat_into = NULL;
    if (bad) {
        P->cat = old, P->ncat = had;
        LOG(P, "refresh: the %s: unreadable, kept as it was", P->library ? "drives" : "catalogue");
        *added = -1;
        return titles;
    }
    if (n == 0)
        P->cat = P->cat_buf[k], P->ncat = 0;
    u8 kept[SEL_MAX_TITLES], have[SEL_MAX_TITLES]; /* kept: an old title still here (2: its tiles taken); have: a new one that was */
    memset(kept, 0, sizeof(kept));
    memset(have, 0, sizeof(have));
    for (int i = 0; i < P->ncat; i++) {
        const struct cat_title *c = &P->cat[i];
        int j = -1;
        for (int m = 0; m < had && j < 0; m++)
            if (!kept[m] && same_title(P, &old[m], c))
                j = m;
        if (j < 0)
            continue;
        kept[j] = 1;
        t[i] = titles[j]; /* its tiles as they are */
        P->cat[i].meta = old[j].meta, P->cat[i].meta_state = old[j].meta_state, P->cat[i].indexed = old[j].indexed;
        memcpy(P->cat[i].file, old[j].file, sizeof(old[j].file));
        P->cat[i].cap = old[j].cap;
        u32 dump = t[i].flags & TITLE_DUMP;
        title_from(P, &t[i], &P->cat[i]);
        if (P->cat[i].name[0] == 0 && titles[j].name[0])
            str_copy(t[i].name, titles[j].name, sizeof(t[i].name));
        t[i].flags |= dump; /* as its param.sfo said */
        have[i] = 1;
    }
    int spare = 0; /* the tiles of the titles that went */
    for (int i = 0; i < P->ncat; i++) {
        if (have[i])
            continue;
        const struct cat_title *c = &P->cat[i];
        surface big = {0}, small = {0};
        while (spare < had && kept[spare])
            spare++;
        if (spare < had) {
            big = titles[spare].icon, small = titles[spare].icon_small;
            kept[spare++] = 2;
        }
        memset(&t[i], 0, sizeof(t[i]));
        title_from(P, &t[i], c);
        if (big.px) {
            t[i].icon = big, t[i].icon_small = small;
            t[i].tint = title_tint(t[i].id, t[i].name);
        } else if (P->tiles_used >= P->ntitle_cap || ui_title_alloc(S, &t[i]) != 0) {
            LOG(P, "refresh: no room for title %d's tiles: the row stops before it", i);
            P->ncat = i;
            break;
        } else {
            P->tiles_used++;
        }
        (*added)++;
        LOG(P, "title %d: %s '%s' %s%s%s: new in the row", i, c->id, c->name, c->version,
            P->library ? " at " : "", P->library ? c->path : "");
    }
    for (int m = 0; m < had; m++)
        if (!kept[m]) {
            (*gone)++;
            LOG(P, "refresh: %s '%s' is gone from the %s", old[m].id, old[m].name, P->library ? "drives" : "catalogue");
        } else if (kept[m] == 2) {
            (*gone)++;
            LOG(P, "refresh: %s '%s' is gone from the %s (its tiles reused)", old[m].id, old[m].name,
                P->library ? "drives" : "catalogue");
        }
    P->buf_turn = !k;
    ui_set_titles(S, t, P->ncat);
    LOG(P, "refresh: %d title(s) (had %d: %d new, %d gone)%s", P->ncat, had, *added, *gone,
        P->ps5 ? "; PS5 games left out of the library" : "");
    return t;
}

/* Refresh's words: what changed */
static void refresh_said(struct sel *S, struct plat *P, u64 now, int added, int gone)
{
    char msg[96];
    const char *where = P->library ? "on the USB drives" : "installed";
    if (added < 0) {
        ui_toast(S, now, P->library || P->ghostfs ? "The games couldn't be read again" : "This boot's game was given: nothing to look for");
        return;
    }
    if (!added && !gone)
        sel_fmt(msg, sizeof(msg), "No change: %d game%s %s", P->ncat, P->ncat == 1 ? "" : "s", where);
    else if (added && gone)
        sel_fmt(msg, sizeof(msg), "%d new game%s, %d gone", added, added == 1 ? "" : "s", gone);
    else if (added)
        sel_fmt(msg, sizeof(msg), "%d new game%s %s", added, added == 1 ? "" : "s", where);
    else
        sel_fmt(msg, sizeof(msg), "%d game%s gone", gone, gone == 1 ? "" : "s");
    if (added > 0)
        ui_toast_ok(S, now, msg);
    else
        ui_toast(S, now, msg);
}

/* ---- one step: the pad, the clock, the users, the UI, a frame if it changed ---- */
static void tick(struct sel *S, struct plat *P, u64 now, int *boot)
{
    u8 pad[0x80];
    memset(pad, 0, sizeof(pad));
    u32 buttons = 0;
    int sx = 128, sy = 128;
    if (P->pad_read(P->pad, pad) == 0) {
        buttons = *(u32 *)pad & ~PAD_INTERCEPTED;
        sx = pad[4], sy = pad[5];
    }
    if (now >= P->next_clock) {
        clock_now(S, P);
        P->next_clock = now + 1000000;
    }
    if (S->users_open && !S->user_chosen && now >= P->next_users) { /* someone signs in with another controller */
        users_load(S, P);
        P->next_users = now + 1000000;
    }
    if (ui_step(S, now, now - P->prev, buttons, sx, sy, boot))
        frame(S);
    P->prev = now;
}

/* the save scan's progress: a step of the screen at most every 33 ms;
 * nonzero when Circle cancelled it */
static int scan_progress(void *arg, u32 done, u32 total)
{
    struct sel *S = arg;
    struct plat *P = PLAT(S);
    u64 now = P->proc_time();
    ui_scan_progress(S, now, done, total);
    if (now - P->scan_frame >= 33333) {
        P->scan_frame = now;
        tick(S, P, now, NULL);
    }
    return S->scan_cancel;
}

/* A save tool's user is chosen: the title is mounted (once) and its save
 * scan runs while this screen shows its progress. */
static void run_scan(struct sel *S, struct plat *P, int t, int index)
{
    const struct ghost_api *a = P->api;
    const struct sel_user *u = &S->users[S->user_focus];
    u64 now = P->proc_time();
    ui_scan_start(S, now);
    P->scan_frame = now;
    if (P->pick_rc == 1) {
        P->pick_rc = a->title_pick(P->cat[t].id, index);
        LOG(P, "title %d (%s) mounted for its save scan: %d", index, P->cat[t].id, P->pick_rc);
    }
    int rc = P->pick_rc == 0 ? a->save_prescan((u32)u->id, scan_progress, S) : -1;
    ui_scan_end(S, P->proc_time(), rc);
    frame(S);
}

/* ---- entry ---- */
extern const u8 sel_font_glf[];

#define HEAP_BASE (3 * FB_BYTES) /* the frame being drawn, two backdrops */
#define HEAP_TITLE ((u64)(TILE_BIG * TILE_BIG + TILE_SMALL * TILE_SMALL) * 4 + sizeof(struct title) + \
                    sizeof(struct cat_title))
#define HEAP_FONT (3u << 20)  /* glyph cache, rasterizer */
#define HEAP_TEMP (24u << 20) /* META's head, a backdrop's QOI and its pixels; or a title's index, a PNG file
                                 and its decoded image, PngDec's work memory */
#define HEAP_SYSFONT (8u << 20)
#define HEAP_SETTINGS (1u << 20) /* Settings' rows and what they show */

__attribute__((section(".text.entry"), used)) int sel_main(const struct ghost_api *api, char chosen[16])
{
    if (!api || api->version != GHOST_API_VERSION || api->size < sizeof(*api))
        return -1;
    struct plat P;
    memset(&P, 0, sizeof(P));
    P.api = api;
    void *ps = NULL;            /* the community plans' job, once started */
    const struct ps_job *job = NULL;
    api->logf("selector: entered");
    P.ctx = *api->ctx;
    P.k = api->k;
    int force = (P.ctx->flags & CTX_FLAG_SELECT) != 0;
    /* the core has the title already and it is a save tool (the only title,
     * or run.py --title): only whose saves is asked */
    int ask_only = !force && (api->profile_has(PF_SAVE_TOOL) & PF_SAVE_TOOL);
    P.ps4 = (P.ctx->flags & CTX_FLAG_PS4) != 0;
    P.pick_rc = 1;
    u32 lk = (u32)P.ctx->libkernel_handle;
    P.proc_time = (u64(*)(void))sys_sym(&P, lk, "sceKernelGetProcessTime");
    P.gettimeofday = (s32(*)(void *, void *))sys_sym(&P, lk, "sceKernelGettimeofday");
    P.utc2local = (s32(*)(s64, s64 *, void *, u64 *))sys_sym(&P, lk, "sceKernelConvertUtcToLocaltime");
    P.param_int = (s32(*)(s32, s32 *))sys_sym(&P, sys_module(&P, "libSceSystemService.sprx", 0, NULL),
                                              "sceSystemServiceParamGetInt");

    api->logf("selector: symbols %p %p %p %p", (void *)P.proc_time, (void *)P.gettimeofday, (void *)P.utc2local, (void *)P.param_int);
    /* 1. what is installed */
    int n = cat_count(&P);
    LOG(&P, "module at %p: %d title(s) installed (%s)%s%s", (void *)sel_main, n,
        P.library ? P.root : !P.ghostfs ? "app root" : P.catalogue ? "GFSCATALOG" : P.legacy[0] ? P.legacy : "nothing",
        force ? ", shown on request" : ask_only ? ", a save tool: whose saves to open" : "",
        P.ps4 ? "; a PS4: game dumps are marked and ask before they boot" : "");
    if (n < 0) /* the core shows the selector with one title or none too (Settings) */
        return -1;
    if (n > SEL_MAX_TITLES)
        n = SEL_MAX_TITLES;
    /* the row may be read again: an install over the network (ghostfs), Refresh (a library's drives too) */
    P.reload_ok = (P.ghostfs || P.library) && !ask_only;
    P.ntitle_cap = n + (P.reload_ok ? NET_EXTRA : 0);
    if (P.ntitle_cap > SEL_MAX_TITLES)
        P.ntitle_cap = SEL_MAX_TITLES;

    /* 2. memory: two whole blocks at the top of the largest free run */
    int sysfont = !(P.ctx->flags & CTX_FLAG_SEL_FONT_BUILTIN);
    off_t run = 0;
    size_t run_len = 0;
    P.k->avail_direct(0, (off_t)P.k->direct_size(), 0x200000, &run, &run_len);
    P.video_size = 2 * FB_SLOT;
    P.heap_size = (HEAP_BASE + (u64)P.ntitle_cap * HEAP_TITLE +
                   (P.reload_ok ? 2 * (u64)P.ntitle_cap * (sizeof(struct title) + sizeof(struct cat_title)) : 0) +
                   HEAP_FONT + HEAP_TEMP + HEAP_SETTINGS + (sysfont ? HEAP_SYSFONT : 0) + sizeof(struct sel) +
                   0x1FFFFF) & ~0x1FFFFFull;
    u64 top = (u64)run + run_len;
    P.video = block_alloc(&P, P.video_size, top, 3 /* write-combined, GPU */, 0x33 /* CPU+GPU RW */, &P.video_phys);
    P.heap = block_alloc(&P, P.heap_size, top - P.video_size, 0 /* write-back */, 0x03, &P.heap_phys);
    LOG(&P, "largest free run %lx+%lu MiB; heap %lu MiB at %p (phys %lx), video %lu MiB at %p (phys %lx)",
        (u64)run, (u64)run_len >> 20, P.heap_size >> 20, P.heap, P.heap_phys, P.video_size >> 20, P.video,
        P.video_phys);
    int choice = -1, first = 0; /* first: the title shown alone (ask_only) */
    struct sel *S = NULL;
    if (!P.heap || !P.video)
        goto out;

    /* 3. state, catalogue, font, images */
    {
        struct sel boot;
        memset(&boot, 0, sizeof(boot));
        boot.plat = &P;
        S = sel_alloc(&boot, sizeof(*S));
    }
    S->plat = &P;
    if (n && cat_load(S, &P, n) != 0)
        goto out;
    n = P.ncat;
    struct title *titles = sel_alloc(S, sizeof(struct title) * (u64)(n ? n : 1));
    if (!titles)
        goto out;
    for (int i = 0; i < n; i++) {
        const struct cat_title *c = &P.cat[i];
        title_from(&P, &titles[i], c);
        P.tiles_used++;
        if (!P.ghostfs && !P.library && !strncmp(P.root, "/ghostldr_usb", 13)) { /* the one game on a USB drive */
            u32 len = (u32)strlen(P.root);
            if (len > 5 && !strcmp(P.root + len - 5, "/app0"))
                len -= 5;
            info_title_where(&P, P.root, len, titles[i].where, (int)sizeof(titles[i].where));
        }
        LOG(&P, "title %d: %s '%s' (name %s) %s, %lu MiB, %s %s%s, %s%s", i, c->id, c->name, name_src(c->name_src),
            c->version, c->total >> 20, P.library ? "at" : "index", P.library ? c->path : c->index[0] ? c->index : "-",
            c->flags & CAT_INCOMPLETE ? ", incomplete" : "", titles[i].flags & TITLE_DUMP ? "a game's dump" : "homebrew",
            c->note[0] ? ": " : "");
        if (c->note[0])
            LOG(&P, "title %d: %s", i, c->note);
    }
    if (font_init(S, sel_font_glf, 2u << 20) != 0) {
        LOG(&P, "built-in font unusable");
        goto out;
    }
    if (sysfont && sysfont_open(S, &P, HEAP_SYSFONT) != 0)
        LOG(&P, "system font unavailable: using the built-in one (Noto Sans)");
    if (png_init(&P) != 0)
        LOG(&P, "libScePngDec unavailable: tiles and backgrounds use the built-in fallback");
    ui_set_platform(S, P.ps4 ? SEL_PS4 : SEL_PS5);
    S->can_refresh = P.reload_ok;
    S->can_delete = !ask_only && (P.ghostfs ? P.catalogue : P.library); /* a USB boot: Settings lists GFSCATALOG's */
    if (ask_only) { /* the core's title alone */
        const char *id = api->title_id();
        for (int i = 0; n > 1 && i < n; i++)
            if (id[0] && strcmp(P.cat[i].id, id) == 0)
                first = i;
        P.cat += first, P.ncat = n = 1; /* index 0 is it from here; the choice goes back as first */
        titles[first].flags |= TITLE_SAVE_TOOL;
    }
    if (ui_init(S, titles + first, n, 0) != 0)
        goto out;
    for (int i = 0; i < n && i < 2; i++) /* the first icons; the rest load while the row is on screen */
        ui_load_icon(S, i);

    /* Settings > Advanced: the switches, and a safe-mode boot's notice; the
     * games whose community memory plan the player turned off */
    conf_load(&P);
    for (int i = 0; i < S->ntitles; i++)
        S->titles[i].community_off = (u8)conf_community_off(&P, S->titles[i].id);
    {   /* community memory plans: shared and fetched on a thread while the games show */
        int share = ((P.conf_on >> CONF_SHARE_MEM) & 1) || (P.ctx->flags & CTX_FLAG_SHARE_MEM); /* or run.py's */
        int fetch = ((P.conf_on >> CONF_COMMUNITY_MEM) & 1) && !P.conf_no_community;
        char ids[PS_MAX_TITLES][16];
        unsigned nid = 0;
        for (int i = 0; i < P.ncat && nid < PS_MAX_TITLES; i++)
            if (P.cat[i].id[0])
                memcpy(ids[nid++], P.cat[i].id, 16);
        if (share || fetch)
            plansync_start(api, (const char (*)[16])ids, nid, share, fetch, &ps);
    }

    art_start(&P); /* Settings > Game art: missing art fetched while the games show */

    /* 4. screen and pad */
    if (video_open(&P) != 0) {
        LOG(&P, "no video output: booting the default title");
        goto out;
    }
    if (pad_open(&P, S) != 0) {
        LOG(&P, "no pad: booting the default title");
        goto out;
    }
    users_load(S, &P);
    if (ask_only)
        ui_ask_user(S, 1);

    /* 5. the loop: 30 fps at most, frames drawn only when something changed */
    u64 now = P.proc_time(), started = now;
    u32 ftp_port = CTX_SELECTOR_FTP_PORT(P.ctx->flags);
    u64 ftp_notice_at = ftp_port ? started + 3000000 : 0;
    P.prev = now;
    if (P.conf_safe) /* ghostldr.lua left the switches out: the last boot with them never got here */
        ui_toast(S, now, "Settings \xE2\x80\xBA Advanced was left out of this boot: the last one with it stopped early");
    frame(S);
    for (;;) {
        now = P.proc_time();
        int boot = -1;
        tick(S, &P, now, &boot);
        if (ftp_notice_at && now >= ftp_notice_at) {
            ftp_notice_at = 0;
            ftp_notice(&P, ftp_port);
        }
        if (S->scan_req)
            run_scan(S, &P, S->sel, first + S->sel);
        if (ps && !job && (job = plansync_poll(ps)) != NULL)
            community_offer(S, &P, job);
        art_step(S, &P);
        if (S->reload_req) { /* an install over the network finished */
            S->reload_req = 0;
            int added, gone;
            if (P.ghostfs)
                titles = reload(S, &P, titles, 0, &added, &gone);
            n = P.ncat;
        }
        if (S->delete_req && now >= S->delete_at) { /* Settings > Games: Yes, once its message has shown */
            S->delete_req = 0;
            char why[96];
            int rc = cat_delete(S, &P, S->del_id, S->del_prefix, why, sizeof(why)), added, gone;
            if (P.ghostfs) /* the row without it */
                titles = reload(S, &P, titles, 0, &added, &gone);
            n = P.ncat;
            char msg[96];
            if (rc == 0 && !why[0])
                sel_fmt(msg, sizeof(msg), "Deleted %s: its saves are kept", S->del_name);
            else
                sel_fmt(msg, sizeof(msg), rc == 0 ? "Deleted %s; %s" : "%s: not deleted: %s", S->del_name, why);
            if (rc == 0)
                ui_toast_ok(S, P.proc_time(), msg);
            else
                ui_toast(S, P.proc_time(), msg);
            if (S->set_open)
                set_rebuild(S);
        }
        if (S->refresh_req && now >= S->refresh_at) { /* Refresh: once its message has shown */
            S->refresh_req = 0;
            int added, gone;
            titles = reload(S, &P, titles, 1, &added, &gone);
            n = P.ncat;
            refresh_said(S, &P, P.proc_time(), added, gone);
            if (S->set_open)
                set_rebuild(S);
        }
        if (boot == UI_QUIT) {
            choice = SEL_EXIT_LOADER;
            LOG(&P, "Close ghostldr after %lu s: back to Luac0re's loader", (now - started) / 1000000);
            break;
        }
        if (boot >= 0) {
            choice = first + boot;
            memcpy(chosen, P.cat[boot].id, 16);
            conf_start(&P, &S->titles[boot]);
            community_give(&P, job ? job : plansync_poll(ps), P.cat[boot].id);
            if (S->user_chosen) {
                *api->title_user = (u32)S->users[S->user_focus].id;
                LOG(&P, "title %d (%s) opens the saves of user %x", choice, P.cat[boot].id, *api->title_user);
            }
            LOG(&P, "Cross on title %d (%s) after %lu s; heap peak %lu MiB of %lu, %u glyphs, %u mounts", choice,
                P.cat[boot].id, (now - started) / 1000000, P.heap_peak >> 20, P.heap_size >> 20, S->fc.misses,
                P.mounts);
            if (P.frames)
                LOG(&P, "%u frames drawn (%u partial): draw %lu ms average, %lu ms at most; present %lu ms average, "
                        "%lu ms at most, %lu rows average",
                    P.frames, P.partial_frames, P.draw_us / P.frames / 1000, P.draw_max_us / 1000,
                    P.present_us / P.frames / 1000, P.present_max_us / 1000, P.present_rows / P.frames);
            break;
        }
        u64 spent = P.proc_time() - now;
        if (spent < 33333)
            P.k->usleep((u32)(33333 - spent));
    }

out:
    /* 6. give everything back */
    plansync_stop(ps);
    art_stop(&P);
    info_netinst_stop(&P);
    video_close(&P);
    sysfont_close(&P);
    sys_unload(&P, &P.mod_png, "libScePngDec");
    u64 freed = block_free(&P, P.video, P.video_size, P.video_phys, "video buffers") +
                block_free(&P, P.heap, P.heap_size, P.heap_phys, "heap");
    off_t run2 = 0;
    size_t run2_len = 0;
    P.k->avail_direct(0, (off_t)P.k->direct_size(), 0x200000, &run2, &run2_len);
    LOG(&P, "released %lu MiB of direct memory; largest free run %lu MiB (was %lu MiB); %u mounts, %u unmounts; "
            "booting title %d",
        freed >> 20, (u64)run2_len >> 20, (u64)run_len >> 20, P.mounts, P.umounts, choice);
    return choice;
}
