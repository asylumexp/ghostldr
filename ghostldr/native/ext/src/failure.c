/*
 * Every failure said on the TV: what went wrong and what to do
 * (docs/failures.md has them all).  The log keeps the detail; the messages
 * name where it is.
 *
 *  - Before the title runs (a boot that stops, a crash of the loader): the
 *    system's message dialog, the one Luac0re's loader shows its text in,
 *    with an OK button.  The boot returns to ghostldr.lua once it is
 *    dismissed.  The core places this code just for that when it isn't
 *    (fail.c, ext.c ext_show).
 *  - While the title runs:
 *    - a crash: a page of ghostldr's own, drawn into a buffer it registers on
 *      the title's VideoOut handle and flips (any title, GPU ones too), kept
 *      on screen until SWRR is closed;
 *    - the title quits (_exit, or its entry returns): the same page, then
 *      SWRR closes, at Cross or after 15 seconds;
 *    - no new picture for a minute, or none at all two minutes after the
 *      start: a system notification (the title may still come back, so the
 *      screen stays the title's).
 *    The core sends a notification first on a crash, in case no page can be
 *    shown (no direct memory left for its buffer).
 *  - The monitor thread (once a second while the title runs): the drive's log
 *    synced, the drive's log attached again after the drive came back
 *    (rel-usb's usb_live_gen), the flips counted for the hang checks.
 *  - diag_notice: a short message over the running title, for other families
 *    (rel-usb's drive messages): a notification and the overlay's banner.
 */
#include "ext.h"

#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

struct ghost_diag *diag_state(void) HIDDEN;
bool log_attach(void) HIDDEN;
void log_detach(void) HIDDEN;
void log_sync(void) HIDDEN;
void log_placed(void) HIDDEN;
/* dlg_overlay.c */
s32 ov_vo_handle(void) HIDDEN;
u32 ov_buttons(void) HIDDEN;
bool ov_open(void) HIDDEN;
void ov_page(u32 *px, u32 w, u32 h, u32 pitch, const char *head, const char *body, const char *foot) HIDDEN;
bool ov_page_shown(const char *head, const char *body, const char *foot) HIDDEN;
/* rel-usb (usb_live.c): the title's drive went away and is waited for; recoveries so far */
bool usb_live_waiting(void) HIDDEN __attribute__((weak));
u32 usb_live_gen(void) HIDDEN __attribute__((weak));

#define P_CROSS 0x4000u
#define T_NO_PICTURE 120 /* monitor ticks (seconds) */
#define T_HANG 60

/* ---- the messages: "headline|what happened (%s: the detail)|what to do" ---- */
#define AGAIN "Close SWRR, start it again and send ghostldr.lua again."
#define CLOSE "close SWRR from the PS button's menu"
/* CTX_FLAG_RETURN: after a crash or a quit, the game selector comes back (title_back.c) */
#define BACK "Back to the game selector: press Cross, or wait a few seconds."
#define BACK_TICKS 24 /* 6 s of the page */
static const char *const k_msg[FAIL_COUNT] = {
    [FAIL_BOOT] = "The game didn't start.|Something went wrong while ghostldr was starting it.|" AGAIN
                  " If it happens again, the log says why.",
    [FAIL_NO_DRIVE] = "No USB drive found.|No USB drive is plugged into the console.|Plug in the drive with your "
                      "games, wait a few seconds, then send ghostldr.lua again.",
    [FAIL_DRIVE_REFUSED] = "The USB drive can't be opened.|The console didn't give the drive to SWRR.|Plug the drive "
                           "in again and send ghostldr.lua again, or install the game into SWRR's saved data.",
    [FAIL_NO_FOLDER] = "The folder isn't on the USB drive.|No USB drive has the folder %s.|Check the folder's name, "
                       "or leave it out to search the whole drive.",
    [FAIL_NO_GAMES] = "No games on the USB drive.|A game is its dump's folder, with eboot.bin in it, and the drive "
                      "has none.|Copy the game's whole dump folder onto the drive (at most 4 folders down).",
    [FAIL_NOT_ON_DRIVE] = "The game isn't on the USB drive.|%s isn't among the games on the drive.|Copy it onto "
                          "the drive, or start a game that is there.",
    [FAIL_NO_TITLE_ID] = "The game's folder doesn't say which game it is.|%s has no sce_sys/param.sfo, and its "
                         "folder name has no game ID.|Name the folder like \"My Game (CUSA12345)\", or copy "
                         "sce_sys/param.sfo from the dump.",
    [FAIL_NOTHING_INSTALLED] = "No games found.|%s, and no game is installed in SWRR's saved data.|Copy a game's "
                               "dump folder onto a USB drive and plug it in, or install a game over the network.",
    [FAIL_NOT_INSTALLED] = "The game isn't installed.|%s isn't installed in SWRR's saved data.|Install it, or "
                           "start it from a USB drive.",
    [FAIL_INSTALL_INCOMPLETE] = "The game's install isn't finished.|%s isn't completely installed.|Install it "
                                "again: it goes on where it stopped.",
    [FAIL_INSTALL_DAMAGED] = "The installed game can't be read.|%s's install is damaged.|Remove it and install it "
                             "again.",
    [FAIL_FILE_MISSING] = "A file of the game is missing.|%s isn't there.|Copy the game's whole dump folder again.",
    [FAIL_FILE_READ] = "A file of the game can't be read.|%s couldn't be read.|Plug the drive in again, or copy "
                       "the game again.",
    [FAIL_ENCRYPTED] = "The game's files are encrypted.|%s is encrypted: ghostldr needs a decrypted dump.|Dump the "
                       "game again with a dumper that decrypts eboot.bin and sce_module.",
    [FAIL_FILE_BAD] = "A file of the game can't be used.|%s isn't a PS4 program ghostldr can load (damaged or cut "
                      "short?).|Copy the game's whole dump folder again.",
    [FAIL_MEMORY] = "The game needs more memory than SWRR has.|%s doesn't fit in the memory ghostldr can give it.|"
                    "This game can't run with ghostldr yet.",
    [FAIL_CODE_SPACE] = "There's no room for the game's code.|ghostldr ran out of memory it can run code from.|"
                        AGAIN " If it keeps happening, the game is too big for now.",
    [FAIL_LINK] = "The game's code couldn't be prepared.|Linking it failed.|This game may not work with ghostldr "
                  "yet: the log says which part.",
    [FAIL_CODE_WRITE] = "The game's code couldn't be loaded.|Writing it into memory failed.|" AGAIN,
    [FAIL_THREAD] = "The game couldn't be started.|Its main thread couldn't be created.|" AGAIN,
    [FAIL_CRASH] = "The game crashed.|It stopped working and can't go on.|To play again, " CLOSE " and start it "
                   "again. If it crashes at the same place each time, this game may not work with ghostldr yet.",
    [FAIL_EXIT] = "The game has quit.|It ended itself%s.|SWRR closes now (a PS5 starts it again): press Cross, or "
                  "wait.",
    [FAIL_RETURNED] = "The game has ended.|Its program finished%s.|SWRR closes now (a PS5 starts it again): press "
                      "Cross, or wait.",
    [FAIL_HANG] = "The game seems stuck.|It hasn't drawn a new picture for a minute.|If nothing changes, " CLOSE ".",
    [FAIL_NO_PICTURE] = "The game hasn't shown anything yet.|Two minutes and no picture.|It may still be loading. "
                        "If the screen stays black, " CLOSE ".",
};

static bool back_mode(u32 kind)
{
    return (g_ctx->flags & CTX_FLAG_RETURN) && kind >= FAIL_CRASH && kind <= FAIL_RETURNED;
}

/* part i (0 headline, 1 what, 2 what to do) of kind's message */
static void part(u32 kind, u32 i, char *out, size_t size)
{
    const char *m = k_msg[kind < FAIL_COUNT && k_msg[kind] ? kind : FAIL_BOOT];
    if (i == 2 && back_mode(kind)) {
        strlcpy(out, BACK, size);
        return;
    }
    for (; i; i--)
        while (*m && *m++ != '|')
            ;
    size_t n = 0;
    while (m[n] && m[n] != '|')
        n++;
    strlcpy(out, m, n + 1 < size ? n + 1 : size);
}

/* a boot's result (core fail.c: FAIL_BOOT with ghost_boot's code) as a failure */
static u32 boot_kind(s64 rc, u32 note)
{
    bool file = note >= FAIL_FILE_MISSING && note <= FAIL_FILE_BAD;
    switch (rc) {
    case -7:
        return FAIL_EXT;
    case -8:
        return FAIL_EXT_START;
    case -6:
        return note >= FAIL_NO_DRIVE && note <= FAIL_INSTALL_DAMAGED ? note : FAIL_BOOT;
    case -3:
        return FAIL_CODE_SPACE;
    case -4:
        return FAIL_LINK;
    case -5:
        return note == FAIL_MEMORY ? FAIL_MEMORY : FAIL_CODE_WRITE;
    case -2:
    case -1:
        return file ? note : FAIL_FILE_BAD;
    }
    return rc > 0 || rc < -64 ? FAIL_THREAD : FAIL_BOOT;
}

/* the detail a message's %s takes */
static void detail(u32 kind, s64 code, const struct ghost_diag *D, char *out, size_t size)
{
    const char *p = D->note_path, *rel = strstr(p, "/ghostldr_usb");
    out[0] = 0;
    if (kind == FAIL_NOTHING_INSTALLED)
        strlcpy(out, D->drives ? "The USB drive has no game ghostldr can start" : "No USB drive is plugged in",
                size);
    else if (kind == FAIL_EXIT || kind == FAIL_RETURNED)
        fmt(out, size, code ? " with an error (code %ld)" : "", code);
    else if (rel) { /* a path on a drive: from its top */
        while (*++rel && *rel != '/')
            ;
        strlcpy(out, *rel ? rel + 1 : p, size);
    }
    else
        strlcpy(out, p[0] ? p : kind == FAIL_INSTALL_INCOMPLETE ? "No installed game" : "The game", size);
}

static void compose(u32 kind, s64 code, struct ghost_diag *D, char *what, size_t wsize, char *foot, size_t fsize)
{
    char d[200], f[256], todo[400];
    detail(kind, code, D, d, sizeof(d));
    part(kind, 1, f, sizeof(f));
    part(kind, 2, todo, sizeof(todo));
    if (kind == FAIL_CODE_SPACE && D->note == FAIL_CODE_SPACE && D->note_path[0])
        strlcpy(what, D->note_path, wsize); /* bigexec.c's own sentence */
    else
        fmt(what, wsize, f, d);
    size_t n = strlen(what);
    fmt(what + n, wsize - n, "\n\nWhat to do: %s", todo);
    n = fmt(foot, fsize, "The log: %s", D->where);
    if (kind == FAIL_CRASH)
        fmt(foot + n, fsize - n, "\nCrash: signal %d at %s", D->sig, D->at);
    else if (kind == FAIL_BOOT || kind == FAIL_THREAD)
        fmt(foot + n, fsize - n, "\nCode: %ld", code);
}

/* ---- the system's message dialog (before the title runs) ---- */
/* open text in it (Luac0re's show_dialog's parameters): 0, or the error */
static s32 dialog_open(const char *text)
{
    s32 (*term)(void) = host_real_in("libSceMsgDialog", "sceMsgDialogTerminate");
    s32 (*init)(void) = host_real_in("libSceMsgDialog", "sceMsgDialogInitialize");
    s32 (*open)(void *) = host_real_in("libSceMsgDialog", "sceMsgDialogOpen");
    s32 (*status)(void) = host_real_in("libSceMsgDialog", "sceMsgDialogUpdateStatus");
    s32 (*close)(void) = host_real_in("libSceMsgDialog", "sceMsgDialogClose");
    s32 (*user)(s32 *) = host_real_in("libSceUserService", "sceUserServiceGetInitialUser");
    if (!term || !init || !open || !status)
        return -1;
    if (status() == 2 && close) /* Luac0re's own text, before the boot closed it */
        for (u32 i = 0, rc = (u32)close(); !rc && i < 200 && status() != 3; i++)
            K.usleep(10000);
    term();
    init();
    static u8 p[0x88] __attribute__((aligned(16))), m[0x40] __attribute__((aligned(16)));
    memset(p, 0, sizeof(p));
    memset(m, 0, sizeof(m));
    s32 uid = -1;
    if (user)
        user(&uid);
    *(u64 *)(p + 0x00) = 0x30;                         /* baseParam.size */
    *(u32 *)(p + 0x2C) = 0xC0D1A109u + (u32)(u64)p;    /* magic */
    *(u64 *)(p + 0x30) = sizeof(p);
    *(u32 *)(p + 0x38) = 1;                            /* USER_MSG */
    *(u64 *)(p + 0x40) = (u64)m;
    *(s32 *)(p + 0x58) = uid;
    *(u64 *)(m + 0x08) = (u64)text;                    /* button type 0: OK */
    return open(p);
}

/* text until it is dismissed: an hour at most (nothing else can go on
 * meanwhile); a minute when run.py's log reaches a computer, which has it
 * all too.  Then a line on the way back to Luac0re's loader, which stays
 * (ghostldr.lua replaces it with its own).  false: no dialog */
static bool msg_dialog(const char *text, bool back_after)
{
    static const char back[] = "ghostldr: the game didn't start.\n\nLuac0re's loader takes scripts again: send "
                               "ghostldr.lua to try again.";
    s32 (*status)(void) = host_real_in("libSceMsgDialog", "sceMsgDialogUpdateStatus");
    s32 (*term)(void) = host_real_in("libSceMsgDialog", "sceMsgDialogTerminate");
    s32 rc = dialog_open(text);
    logf("failure: message dialog -> %x", rc);
    u32 limit = g_ctx->log_addr ? 1200 : 72000;
    for (u32 i = 0; rc == 0 && i < limit && status() != 3; i++)
        K.usleep(50000);
    if (rc == 0 && status() != 3 && term)
        term();
    if (rc == 0 && back_after)
        dialog_open(back);
    return rc == 0;
}

/* ---- a page of ghostldr's own, over the title ---- */
static struct {
    s32 vo, idx;
    u32 *px;
    s32 (*flip)(s32, s32, u32, s64);
    s32 (*st)(s32, void *);
} P = {-1, -1, NULL, NULL, NULL};

#define PAGE_W 1920
#define PAGE_H 1080
#define PAGE_BYTES ((u64)PAGE_W * PAGE_H * 4)

static bool page_open(void)
{
    static bool tried;
    if (P.px || tried)
        return P.px != NULL;
    tried = true;
    typedef s64 (*fn6)(u64, u64, u64, u64, u64, u64);
    fn6 open = (fn6)hle_lookup("libSceVideoOut", "sceVideoOutOpen");
    fn6 reg = (fn6)host_real_in("libSceVideoOut", "sceVideoOutRegisterBuffers");
    void (*attr)(void *, u32, u32, u32, u32, u32, u32) = host_real_in("libSceVideoOut", "sceVideoOutSetBufferAttribute");
    P.flip = host_real_in("libSceVideoOut", "sceVideoOutSubmitFlip");
    P.st = host_real_in("libSceVideoOut", "sceVideoOutGetFlipStatus");
    s32 vo = ov_vo_handle();
    if (vo < 0 && open) /* the title has none yet: the core's override takes it from SWRR */
        vo = (s32)open(0xFF, 0, 0, 0, 0, 0);
    off_t phys = 0;
    void *va = NULL;
    u64 size = ALIGN_UP(PAGE_BYTES, 0x200000);
    if (vo < 0 || !reg || !attr || !P.flip ||
        K.alloc_direct(0, (off_t)K.direct_size(), size, 0x200000, 3 /* write-combined */, &phys) != 0) {
        logf("failure: no page of its own (VideoOut %d, no direct memory or no VideoOut calls)", vo);
        return false;
    }
    if (K.map_direct(&va, size, 0x33 /* CPU and GPU read-write */, 0, phys, 0x200000) != 0 || !va) {
        logf("failure: the page's memory can't be mapped");
        return false;
    }
    u8 a[0x40];
    memset(a, 0, sizeof(a));
    attr(a, 0x80000000u /* A8R8G8B8 sRGB */, 1 /* linear */, 0, PAGE_W, PAGE_H, PAGE_W);
    s32 rc = -1;
    for (s32 i = 15; i >= 2 && P.idx < 0 && rc != (s32)0x8029000F; i--)
        if ((rc = (s32)reg((u64)vo, (u64)i, (u64)&va, 1, (u64)a, 0)) >= 0)
            P.idx = i;
    if (P.idx < 0 && rc == (s32)0x8029000F) { /* NO_EMPTY_SLOT: the title's sets go (it can't draw any more) */
        s32 (*unreg)(s32, s32) = host_real_in("libSceVideoOut", "sceVideoOutUnregisterBuffers");
        for (s32 set = 0; unreg && set < 4; set++)
            logf("failure: the title's buffer set %d unregistered: %x", set, (u32)unreg(vo, set));
        if ((rc = (s32)reg((u64)vo, 0, (u64)&va, 1, (u64)a, 0)) >= 0)
            P.idx = 0;
    }
    logf("failure: page buffer %p on VideoOut handle %d, index %d (%x)", va, vo, P.idx, rc);
    if (P.idx < 0)
        return false;
    P.vo = vo, P.px = va;
    return true;
}

/* shows the page now, and again whenever something else was flipped since */
static void page_keep(void)
{
    if (!P.px)
        return;
    u8 st[0x40];
    memset(st, 0, sizeof(st));
    if (P.st && P.st(P.vo, st) == 0 && *(const s32 *)(st + 0x38) == P.idx)
        return;
    P.flip(P.vo, P.idx, 1 /* vsync */, 0);
}

/* the page on screen: drawn into the title's own frame on screen when the
 * CPU can (its frames are linear), else in a buffer of its own */
static bool page(const char *head, const char *body, const char *foot)
{
    if (!P.px && ov_page_shown(head, body, foot))
        return true;
    if (!page_open())
        return false;
    ov_page(P.px, PAGE_W, PAGE_H, PAGE_W, head, body, foot);
    P.flip(P.vo, P.idx, 1, 0);
    return true;
}

static void notify(const char *head, const char *what)
{
    u8 b[0xC30];
    memset(b, 0, sizeof(b));
    *(s32 *)(b + 0x10) = -1;
    b[0x2C] = 1;
    fmt((char *)b + 0x2D, 0x3FF, "ghostldr: %s %s", head, what);
    strlcpy((char *)b + 0x42D, "cxml://psnotification/tex_icon_system", 0x100);
    int fd = K.open("/dev/notification0", O_WRONLY, 0);
    if (fd >= 0) {
        K.write(fd, b, sizeof(b));
        K.close(fd);
    }
}

/* How full the title's memory was when it crashed: it starts zeroed (hle.c),
 * so a 64 KiB block that still reads as zeros at every 8 KiB was never
 * handed out (or never written).  A heap written to its end ran out; one
 * with room can give some to another (a memory plan's tables).  Each of the
 * title's direct allocations (hle.c's table), where this process maps it;
 * GPU-only heaps read uncached: a second or so in all. */
static void log_written_memory(void)
{
    struct {
        u64 start, end, offset;
        s32 prot, type;
        u8 flags;
        char name[32];
    } __attribute__((packed)) vq;
    u32 n = *G->dm_nusage < 32 ? *G->dm_nusage : 32;
    for (u64 va = 0x10000; K.virtual_query((void *)va, 1, &vq, sizeof(vq)) == 0 && vq.end > va; va = vq.end) {
        if (!(vq.flags & 2) || !(vq.prot & 1) || vq.end - vq.start < 0x100000)
            continue;
        /* one mapping can hold several allocations (adjacent heaps): each its own line */
        for (u32 i = 0; i < n; i++) {
            const struct dm_usage *u = &G->dm_usage[i];
            u64 lo = vq.offset > u->phys ? vq.offset : u->phys;
            u64 hi = vq.offset + (vq.end - vq.start) < u->phys + u->len ? vq.offset + (vq.end - vq.start)
                                                                        : u->phys + u->len;
            if (lo >= hi || hi - lo < 0x100000)
                continue;
            u64 start = vq.start + (lo - vq.offset), len = hi - lo, written = 0, top = 0;
            for (u64 b = 0; b < len; b += 0x10000) {
                const volatile u64 *p = (const volatile u64 *)(start + b);
                u64 m = len - b < 0x10000 ? len - b : 0x10000;
                bool any = false;
                for (u64 o = 0; o < m && !any; o += 0x2000)
                    any = p[o / 8] != 0;
                if (any)
                    written += m, top = b + m;
            }
            logf("memory: %lx-%lx (%lu MiB, prot %x, phys %lx, allocated from eboot+%lx type %d): %lu MiB written, "
                 "the last at +%lu MiB",
                 start, start + len, len >> 20, vq.prot, lo, u->from, u->type, written >> 20, top >> 20);
        }
    }
}

void swap_log(void) HIDDEN __attribute__((weak)); /* swap.c: the heaps it backs in part */

/* ---- the screens ---- */
static void show(u32 kind, s64 code)
{
    struct ghost_diag *D = diag_state();
    if (!D)
        return;
    if (kind == FAIL_BOOT)
        kind = boot_kind(code, D->note);
    if (kind == FAIL_CRASH && g_main_mod) {
        log_written_memory();
        if (swap_log)
            swap_log();
    }
    if (kind == FAIL_CRASH) {
        u64 base = 0;
        const char *name = "?";
        for (struct module *m = g_modules; m; m = m->next)
            if (D->rip >= m->base && D->rip < m->base + m->image_size)
                base = m->base, name = m->name;
        if (D->rip < 0x10000) /* rip 0: a call through a null pointer (caught on a PS5 13.60) */
            fmt(D->at, sizeof(D->at), "address 0x%lx (a call through a null pointer)", D->rip);
        else
            fmt(D->at, sizeof(D->at), "%s+0x%lx", name, D->rip - base);
    }
    if (!D->title || kind < FAIL_CRASH)
        log_attach(); /* a boot that stops: its log on the drive too */
    log_sync();
    char *what = page_alloc(0x4000), foot[400], head[100], todo[400];
    if (!what)
        return;
    part(kind, 0, head, sizeof(head));
    part(kind, 2, todo, sizeof(todo));
    compose(kind, code, D, what, 0x3000, foot, sizeof(foot));
    logf("failure %u (%ld): %s %s", kind, code, head, D->note_path);
    D->shown = 1;
    if (!D->title) { /* before the title: the system's dialog */
        char *text = what + 0x3000;
        if (kind == FAIL_CRASH)
            fmt(text, 0x1000, "ghostldr crashed while starting the game.\n\n%s\n\n%s", AGAIN, foot);
        else
            fmt(text, 0x1000, "%s\n\n%s\n\n%s", head, what, foot);
        if (!msg_dialog(text, kind < FAIL_CRASH)) /* (a crash parks Luac0re's thread: no way back) */
            notify(head, todo);
        if (kind < FAIL_CRASH)
            log_detach();
    } else if (kind == FAIL_HANG || kind == FAIL_NO_PICTURE) {
        notify(head, todo);
    } else if (page(head, what, foot)) {
        bool back = back_mode(kind);
        if (kind == FAIL_CRASH && !back)
            for (;;) { /* until SWRR is closed */
                K.usleep(250000);
                page_keep();
            }
        /* the title quit: 15 s, or Cross; back to the game selector: 6 s */
        for (u32 t = 0; t < (back ? BACK_TICKS : 60) && !(ov_buttons() & P_CROSS); t++) {
            K.usleep(250000);
            page_keep();
        }
    } else {
        /* nothing drawn: notifications (a crash's, each 30 s, unless the game selector comes back) */
        for (u32 t = 0; (kind == FAIL_CRASH && !back_mode(kind)) || t < 1; t++) {
            notify(head, todo);
            K.usleep(t ? 30000000 : 3000000);
        }
    }
    log_sync();
    page_free(what, 0x4000);
    if (back_mode(kind)) { /* the boot thread stops the title (title_back.c); the caller parks */
        __atomic_store_n(&D->back, 1, __ATOMIC_RELEASE);
        return;
    }
    if (kind == FAIL_EXIT || kind == FAIL_RETURNED) { /* SWRR ends, as the title asked: as an app closes */
        s32 (*exec)(const char *, const char *const *) = host_real_in("libSceSystemService", "sceSystemServiceLoadExec");
        s32 rc = exec ? exec("exit", NULL) : -1; /* (_exit is a crash to the system: CE-34878-0) */
        logf("failure: SWRR closes (LoadExec exit -> %x)", (u32)rc);
        for (u32 t = 0; rc == 0 && t < 100; t++)
            K.usleep(100000);
        void (*ex)(int) = host_real("_exit");
        if (ex)
            ex((int)code);
    }
}

/* ---- the title's run ---- */
static volatile int g_mon;

static void *monitor(void *arg)
{
    struct ghost_diag *D = diag_state();
    s32 (*st)(s32, void *) = host_real_in("libSceVideoOut", "sceVideoOutGetFlipStatus");
    u64 last = 0;
    u32 still = 0, ticks = 0, gen = usb_live_gen ? usb_live_gen() : 0;
    bool told = false, lost_said = false;
    for (;;) {
        K.usleep(1000000);
        if (D->crashed)
            continue;
        log_sync();
        if (usb_live_gen && usb_live_gen() != gen) { /* the drive came back: its log file too */
            gen = usb_live_gen();
            log_attach();
        }
        if (D->f[1].lost && !lost_said) {
            lost_said = true;
            fmt(D->where, sizeof(D->where), "/temp0/ghostldr.log (the USB drive went away)");
            logf("log: the USB drive's log file can't be written any more: %s goes on", LOG_TEMP);
        }
        if (D->f[1].fd >= 0)
            lost_said = false;
        /* hangs: flips counted while nothing else explains a still screen */
        u8 s[0x40];
        memset(s, 0, sizeof(s));
        s32 vo = ov_vo_handle();
        u64 count = vo >= 0 && st && st(vo, s) == 0 ? *(u64 *)s : 0;
        if (ov_open() || (usb_live_waiting && usb_live_waiting()) || count != last) {
            still = 0, last = count, told = false;
            ticks++;
            continue;
        }
        ticks++, still++;
        if (!told && ((!count && ticks == T_NO_PICTURE) || (count && still == T_HANG))) {
            told = true;
            logf("failure: %s (flips %lu, %u s)", count ? "no new picture" : "no picture yet", count, ticks);
            show(count ? FAIL_HANG : FAIL_NO_PICTURE, 0);
        }
    }
    return arg;
}

/* ext_main.c: the title's main thread is next */
HIDDEN void diag_title_starting(void)
{
    struct ghost_diag *D = diag_state();
    if (!D || __atomic_exchange_n(&g_mon, 1, __ATOMIC_ACQ_REL))
        return;
    D->title = 1;
    s32 (*create)(u64 *, const void *, void *(*)(void *), void *, const char *) = host_real("scePthreadCreate");
    u64 th = 0;
    s32 rc = create ? create(&th, NULL, monitor, NULL, "ghostldr-monitor") : -1;
    if (rc == 0)
        ext_thread_note(th, "ghostldr-monitor");
    logf("failure: monitor thread %x", rc);
}

/* a short message over the running title (other families: rel-usb's drive
 * messages): a notification, and the overlay's banner where it draws */
HIDDEN void diag_notice(const char *text, u32 seconds)
{
    notify(text, "");
    ov_banner(text, seconds);
}

/* ---- the extension's entries (ext_main.c) ---- */
HIDDEN void diag_placed(void)
{
    struct ghost_diag *D = diag_state();
    if (!D)
        return;
    log_placed();
    D->show = show;
}

/* the drive's games (approot.c) before the title loads: a boot that stops
 * there says why now, while this code is still placed (the core gives it
 * back right after) */
static const struct ext_early *g_inner;

static int early_init(const char **root)
{
    int rc = g_inner->approot_init(root);
    if (rc < 0)
        show(FAIL_BOOT, -6);
    return rc;
}

static int early_pick(const char *title, int index, const char **root)
{
    int rc = g_inner->approot_pick(title, index, root);
    if (rc < 0)
        show(FAIL_BOOT, -6);
    return rc;
}

static int early_library(const struct lib_title **list) { return g_inner->library(list); }

static int early_scan(u32 user, int (*progress)(void *arg, u32 done, u32 total), void *arg)
{
    return g_inner->save_prescan ? g_inner->save_prescan(user, progress, arg) : -1;
}

static struct ext_early g_wrap;

HIDDEN const struct ext_early *diag_early(const struct ext_early *inner)
{
    diag_placed();
    if (!inner || inner->magic != EXT_EARLY_MAGIC || !inner->approot_init)
        return inner;
    g_inner = inner;
    memcpy(&g_wrap, inner, inner->size < sizeof(g_wrap) ? inner->size : sizeof(g_wrap));
    g_wrap.size = sizeof(g_wrap);
    g_wrap.approot_init = early_init;
    g_wrap.approot_pick = early_pick;
    g_wrap.library = early_library;
    g_wrap.save_prescan = inner->save_prescan ? early_scan : NULL;
    return &g_wrap;
}

static int diag_family_init(void)
{
    diag_placed();
    if (!(g_ctx->flags & CTX_FLAG_ELFLDR)) /* the ELF loader: /temp0 and its connections (no drive mounted for it) */
        log_attach();
    return 0;
}

/* A title's own handler for a crash signal replaces ghostldr's (Unity's Mono
 * installs them): its crashes then end as that handler decides, without
 * ghostldr's report and page.  Said in the log, once per signal. */
static s32 hle_sigaction(s32 sig, const void *act, void *old)
{
    static s32 (*real)(s32, const void *, void *);
    static u32 said;
    if (!real)
        real = host_real("sigaction");
    u32 bit = sig > 0 && sig < 32 ? 1u << sig : 0;
    if (act && (bit & (1u << 4 | 1u << 6 | 1u << 8 | 1u << 10 | 1u << 11)) &&
        !(__atomic_fetch_or(&said, bit, __ATOMIC_RELAXED) & bit))
        logf("failure: the title installs its own handler for signal %d: its crashes there get no report or page "
             "from ghostldr",
             sig);
    return real ? real(sig, act, old) : -1;
}

#pragma clang attribute pop

static const struct ext_hle_ent k_hle[] = {
    {"libkernel", "sigaction", hle_sigaction, NULL},
    /* not a system library: for the tests */
    {"libGhostldrDiag", "ghostldrDiagNotice", diag_notice, NULL},
};

EXT_FAMILY("diag", k_hle, diag_family_init);
