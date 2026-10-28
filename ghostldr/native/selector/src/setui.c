/*
 * Settings: a sheet over the selector, in its look.  The sections are on the
 * left (Up / Down); Right or Cross goes into the open one, whose rows are on
 * the right (Up / Down between those that do something or say something per
 * item, the list scrolling with the focus); Left or Circle comes back out,
 * and Circle on the sections (or Square) closes the sheet.
 *
 *  - Games: every title the selector has, each with its problem and what to
 *    do about it, in plain words (what `ghostldr.py check` said on a
 *    computer).
 *  - USB drives: each drive the console has, its file system, size and free
 *    space, the games found on it, FAT32's 4 GiB limit, where logs go.
 *  - Network: the console's address, Install over network (the extension's
 *    web installer: a card with the address to open, its token and its
 *    progress), the ports ghostldr's features use.
 *  - Logs: whether logs are written to the USB drive, and where they go
 *    (the core's ghost_api.log_usb: rel-diag's log sink).
 *  - Advanced: switches that were run.py flags (conf.c: boot context flags
 *    kept in GLDRCONF/settings.txt), and Reset to defaults.
 *  - About: ghostldr's version and build, the bundle cache, the console and
 *    SWRR, and Close ghostldr.
 *
 * What is shown comes from the platform (struct sel_info: main.c on the
 * console, preview.c on a computer), read again each time the sheet opens.
 */
#include "sel.h"

#define SEC_GAMES 0
#define SEC_DRIVES 1
#define SEC_NETWORK 2
#define SEC_LOGS 3
#define SEC_ADVANCED 4
#define SEC_ABOUT 5
#define NSEC 6
static const char SEC_NAME[NSEC][16] = {"Games", "USB drives", "Network", "Logs", "Advanced", "About"};

#define SACT_LOG_USB 1
#define SACT_NETINST 2
#define SACT_CLOSE 3
#define SACT_RECHECK 4
#define SACT_RESET 5
#define SACT_SWITCH 16 /* + the switch's index */

/* layout */
#define MARGIN 96
#define LIST_X MARGIN
#define LIST_W 400
#define LIST_Y 216
#define LIST_ROW 76
#define PANE_X 560                       /* the line between the sections and the rows */
#define CX 640                           /* the rows */
#define CW (1920 - CX - MARGIN)
#define VIEW_Y0 196                      /* rows are drawn between these */
#define VIEW_Y1 968
#define NET_W 1120
#define NET_H 620
#define HOLD_US 1000000u                 /* Circle held this long stops a busy install */
#define POLL_US 250000u

#define WHITE 0xFFFFFFFFu
#define SOFT 0xDCFFFFFFu
#define DIM 0xA8FFFFFFu
#define FAINT 0x80FFFFFFu
#define INK 0xFF14171Cu
#define GREEN 0xFF6FD68Bu
#define AMBER 0xFFFFB547u
#define RED 0xFFFF7A6Bu
#define BLUE 0xFF8EC2FFu
#define GREY 0xFFA9B0BBu

static u32 fade(u32 c, u32 a) { return ((c >> 24) * a / 255) << 24 | (c & 0xFFFFFFu); }
static int focusable(const struct srow *r) { return r->kind == SROW_ITEM || r->kind == SROW_TOGGLE || r->kind == SROW_ACTION; }

/* ---- the rows of a section ---- */
static struct srow *row(struct sel *S, u8 kind, const char *text)
{
    if (S->nrows >= SET_MAX_ROWS)
        return &S->rows[SET_MAX_ROWS - 1]; /* full: the last is written over */
    struct srow *r = &S->rows[S->nrows++];
    __builtin_memset(r, 0, sizeof(*r));
    r->kind = kind;
    str_copy(r->text, text, sizeof(r->text));
    return r;
}

static void sub(struct srow *r, const char *line)
{
    if (r->nsub < 3 && line && line[0])
        str_copy(r->sub[r->nsub++], line, sizeof(r->sub[0]));
}

static void text_row(struct sel *S, const char *label, const char *value)
{
    struct srow *r = row(S, SROW_TEXT, label);
    sub(r, value);
}

static void cap_first(char *s)
{
    if (s[0] >= 'a' && s[0] <= 'z')
        s[0] = (char)(s[0] - 32);
}

/* a title: its name, what it is, and its problem with the fix */
static void game_row(struct sel *S, const struct title *t)
{
    char line[160], size[24];
    struct srow *r = row(S, SROW_ITEM, t->name[0] && text_fits_font(S, t->name) ? t->name : t->id[0] ? t->id
                                       : t->name[0] ? t->name : "Unknown title");
    int n = 0;
    line[0] = 0;
    if (t->id[0])
        n += sel_fmt(line + n, (int)sizeof(line) - n, "%s", t->id);
    if (t->version[0])
        n += sel_fmt(line + n, (int)sizeof(line) - n, "%sVersion %s", n ? "  \xC2\xB7  " : "", t->version);
    if (t->size) {
        ui_size(size, sizeof(size), t->size);
        n += sel_fmt(line + n, (int)sizeof(line) - n, "%s%s", n ? "  \xC2\xB7  " : "", size);
    }
    if (t->where[0])
        sel_fmt(line + n, (int)sizeof(line) - n, "%s%s%s", n ? "  \xC2\xB7  " : "",
                t->where[0] == 'U' && t->where[1] == 'S' && t->where[2] == 'B' ? "" : "USB drive: ", t->where);
    else if (t->prefix[0])
        sel_fmt(line + n, (int)sizeof(line) - n, "%sInstalled in saved data (%s)", n ? "  \xC2\xB7  " : "", t->prefix);
    sub(r, line);
    char note[160];
    str_copy(note, t->note, sizeof(note));
    cap_first(note);
    if (!(t->flags & TITLE_COMPLETE) && t->where[0]) {
        str_copy(r->chip, "Can't start", sizeof(r->chip)), r->chip_c = RED;
        sub(r, note[0] ? note : "This folder can't be started");
        sub(r, "Fix the folder on a computer, then start ghostldr again.");
    } else if (!(t->flags & TITLE_COMPLETE)) {
        str_copy(r->chip, "Install incomplete", sizeof(r->chip)), r->chip_c = RED;
        sub(r, "The install stopped before the end, so it can't start.");
        sub(r, "Install it again (Network \xE2\x80\xBA Install over network, or gfs_install.py): it goes on where it stopped.");
    } else if (t->flags & TITLE_COPY) {
        str_copy(r->chip, "Second copy", sizeof(r->chip)), r->chip_c = BLUE;
        char first[112];
        int other = title_copy_of(t, first, sizeof(first)); /* ui.c: where the other copy is */
        if (other)
            sel_fmt(line, sizeof(line), "The same game is also in %s", first);
        sub(r, other ? line : note[0] ? note : "Another folder holds the same game");
        sub(r, "Both start from this list; remove one on a computer if you don't need it.");
    } else if (S->platform == SEL_PS4 && (t->flags & TITLE_DUMP)) {
        str_copy(r->chip, "May not run on PS4", sizeof(r->chip)), r->chip_c = AMBER;
        sub(r, "A PS4 gives SWRR 2.5 GiB of memory: most games need more. Homebrew and small games run.");
    } else {
        str_copy(r->chip, "Ready", sizeof(r->chip)), r->chip_c = GREEN;
    }
}

static const char *fs_name(u8 fs) { return fs == FS_EXFAT ? "exFAT" : fs == FS_FAT32 ? "FAT32" : "File system unknown"; }

static void drive_row(struct sel *S, const struct sel_info *in, int i)
{
    const struct sel_drive *d = &in->drives[i];
    char name[80], line[160], a[24], b[24];
    sel_fmt(name, sizeof(name), "%s", d->name[0] ? d->name : "USB drive");
    if (in->ndrives > 1)
        sel_fmt(name, sizeof(name), "%d. %s", i + 1, d->name[0] ? d->name : "USB drive");
    struct srow *r = row(S, SROW_ITEM, name);
    str_copy(r->chip, fs_name(d->fs), sizeof(r->chip));
    r->chip_c = d->fs == FS_EXFAT ? GREEN : d->fs == FS_FAT32 ? AMBER : GREY;
    if (d->total) {
        ui_size(a, sizeof(a), d->total), ui_size(b, sizeof(b), d->free);
        sel_fmt(line, sizeof(line), "%s  \xC2\xB7  %s free", a, b);
    } else {
        str_copy(line, "Size unknown", sizeof(line));
    }
    sub(r, line);
    int n = 0;
    if (d->games > 0)
        n = sel_fmt(line, sizeof(line), "%d game%s found on it%s", d->games, d->games == 1 ? "" : "s",
                    d->listed ? ", listed here" : "");
    else if (d->games == 0)
        n = sel_fmt(line, sizeof(line), "No games found on it");
    else
        n = sel_fmt(line, sizeof(line), "Not looked at: the games came from %s", in->ndrives > 1 ? "the other drive" : "elsewhere");
    if (d->bad > 0)
        n += sel_fmt(line + n, (int)sizeof(line) - n, "; %d folder%s can't start (Games)", d->bad, d->bad == 1 ? "" : "s");
    (void)n;
    sub(r, line);
    if (d->fs == FS_FAT32)
        sub(r, "FAT32 can't hold a file of 4 GiB or more, and most PS4 games have one: put those on an exFAT drive.");
    else if (d->fs == FS_UNKNOWN)
        sub(r, "ghostldr reads exFAT and FAT32 drives; format it exFAT on a computer if its games don't show.");
}

static void build(struct sel *S)
{
    const struct sel_info *in = S->info;
    char line[200];
    S->nrows = 0;
    switch (S->set_sec) {
    case SEC_GAMES: {
        if (!S->ntitles) {
            text_row(S, "NO GAMES YET",
                     "Copy a game's folder (the one with eboot.bin) to an exFAT USB drive, or install one over the "
                     "network: Network \xE2\x80\xBA Install over network.");
            break;
        }
        int ready = 0;
        for (int i = 0; i < S->ntitles; i++)
            ready += (S->titles[i].flags & TITLE_COMPLETE) && !(S->titles[i].flags & TITLE_COPY);
        sel_fmt(line, sizeof(line), "%d GAME%s  \xC2\xB7  %d READY  \xC2\xB7  %d TO LOOK AT", S->ntitles,
                S->ntitles == 1 ? "" : "S", ready, S->ntitles - ready);
        row(S, SROW_HEAD, line);
        for (int i = 0; i < S->ntitles; i++)
            game_row(S, &S->titles[i]);
        if (in->ninst > 0) { /* a USB boot: what saved data holds as well */
            sel_fmt(line, sizeof(line), "%d INSTALLED IN SAVED DATA", in->ninst);
            row(S, SROW_HEAD, line);
            for (int i = 0; i < in->ninst; i++) {
                const struct sel_inst *t = &in->inst[i];
                struct srow *r = row(S, SROW_ITEM, t->name[0] && text_fits_font(S, t->name) ? t->name : t->id);
                char size[24];
                ui_size(size, sizeof(size), t->size);
                sel_fmt(line, sizeof(line), "%s%s%s  \xC2\xB7  %s  \xC2\xB7  Installed in saved data", t->id,
                        t->version[0] ? "  \xC2\xB7  Version " : "", t->version, size);
                sub(r, line);
                if (t->incomplete) {
                    str_copy(r->chip, "Install incomplete", sizeof(r->chip)), r->chip_c = RED;
                    sub(r, "The install stopped before the end: install it again, it goes on where it stopped.");
                } else {
                    str_copy(r->chip, "Not in this list", sizeof(r->chip)), r->chip_c = GREY;
                    sub(r, "Installed titles show when no USB drive in the console has games.");
                }
            }
        }
        break;
    }
    case SEC_DRIVES:
        if (in->usb == USB_PS4) {
            text_row(S, "USB DRIVES", "ghostldr doesn't read USB drives on a PS4: games go in saved data "
                                      "(Network \xE2\x80\xBA Install over network).");
        } else if (in->usb == USB_JAKX) {
            text_row(S, "USB DRIVES", "ghostldr doesn't read USB drives in Jak X.");
        } else if (in->usb != USB_READ) {
            text_row(S, "USB DRIVES", "The console's USB storage service didn't answer: no drive could be read. "
                                      "Plug the drive in again and start ghostldr again.");
        } else if (!in->ndrives) {
            text_row(S, "NO USB DRIVE", "Plug in a drive formatted exFAT with your games on it, then start ghostldr "
                                        "again.");
        } else {
            for (int i = 0; i < in->ndrives; i++)
                drive_row(S, in, i);
        }
        text_row(S, "WHAT GHOSTLDR LOOKS FOR",
                 "A game is a folder with eboot.bin in it, or with app0/eboot.bin, at most four folders down from the "
                 "drive's top. Its ID comes from sce_sys/param.sfo, else from the folder's name (CUSA12345).");
        if (in->usb == USB_READ && in->log_usb >= 0)
            text_row(S, "LOGS", in->log_usb ? (in->log_where[0] ? in->log_where : "On") : "Not written to a drive (Logs)");
        if (in->usb == USB_READ)
            row(S, SROW_ACTION, "Look again")->act = SACT_RECHECK;
        if (in->usb == USB_READ)
            sub(&S->rows[S->nrows - 1], "Read the drives again (a game newly copied shows when ghostldr starts again)");
        break;
    case SEC_NETWORK: {
        text_row(S, "THIS CONSOLE'S ADDRESS", in->ip[0] ? in->ip : "Not connected to a network");
        struct srow *r;
        if (in->netinst) {
            r = row(S, SROW_ACTION, "Install over network");
            r->act = SACT_NETINST;
            sub(r, "Install a game into saved data from a browser on a phone or computer on this network");
        } else {
            text_row(S, "INSTALL OVER NETWORK", "This build of ghostldr has no network installer.");
        }
        row(S, SROW_HEAD, "PORTS ON THIS CONSOLE");
        for (int i = 0; i < in->nports; i++) {
            sel_fmt(line, sizeof(line), "TCP %u", in->ports[i].port);
            text_row(S, line, in->ports[i].what);
        }
        break;
    }
    case SEC_LOGS: {
        if (in->usb == USB_PS4 || in->usb == USB_JAKX) {
            text_row(S, "LOGS ON THE USB DRIVE", "ghostldr doesn't read USB drives here: the log stays on the console.");
            break;
        }
        struct srow *r = row(S, SROW_TOGGLE, "Write logs to the USB drive");
        r->act = SACT_LOG_USB, r->on = in->log_usb > 0;
        sub(r, in->log_usb ? "On: each boot's log goes to the folder ghostldr/logs on the drive"
                           : "Off: nothing is written to the drive");
        text_row(S, "WHERE THE LOG GOES", in->log_where[0] ? in->log_where : in->log_usb ? "On the USB drive" : "Not to a drive");
        text_row(S, "IF SOMETHING GOES WRONG",
                 "Plug the drive into a computer and send the newest file in ghostldr/logs with your report.");
        break;
    }
    case SEC_ADVANCED: {
        if (in->sw_safe)
            text_row(S, "LEFT OUT OF THIS BOOT", "The last boot with these switches stopped before this screen, so this "
                                                 "one started without them. Change them here, or reset them.");
        for (int i = 0; i < in->nsw; i++) {
            const struct sel_switch *w = &in->sw[i];
            if (w->logs)
                continue;
            struct srow *r = row(S, SROW_TOGGLE, w->label);
            r->act = (u16)(SACT_SWITCH + i), r->on = (u8)((in->sw_on >> i) & 1);
            sub(r, r->on ? w->on : w->off);
        }
        struct srow *r = row(S, SROW_ACTION, "Reset to defaults");
        r->act = SACT_RESET;
        sub(r, "Every switch, the log's too, back as it was when ghostldr came");
        text_row(S, "KEPT IN", in->sw_live == 0 && !in->sw_safe
                                   ? "This console's saved data (GLDRCONF). This boot started without them (a boot from "
                                     "a computer's run.py): the release's boots use them"
                                   : "This console's saved data (GLDRCONF): this boot and the next ones use them");
        break;
    }
    default: {
        text_row(S, "GHOSTLDR", in->version[0] ? in->version : "Unknown version");
        text_row(S, "BUILD", in->build[0] ? in->build : "Unknown");
        text_row(S, "BUNDLE CACHE", in->cache[0] ? in->cache : "Unknown");
        text_row(S, "CONSOLE", in->console[0] ? in->console : "Unknown");
        text_row(S, "HOST GAME", in->host[0] ? in->host : "Unknown");
        struct srow *r = row(S, SROW_ACTION, "Close ghostldr");
        r->act = SACT_CLOSE;
        sub(r, "Back to Luac0re's loader without starting a game");
        break;
    }
    }
}

/* row heights and places, from the top of the section */
static void layout(struct sel *S)
{
    int y = 0;
    for (int i = 0; i < S->nrows; i++) {
        struct srow *r = &S->rows[i];
        int h;
        if (r->kind == SROW_HEAD) {
            h = 62;
        } else if (r->kind == SROW_TEXT) {
            char lines[3][128];
            int n = r->nsub ? ui_wrap(S, 27, CW, r->sub[0], lines, 3) : 1;
            h = 72 + 38 * (n - 1) + 30;
        } else if (r->kind == SROW_ITEM) {
            h = 66 + 34 * r->nsub + 22;
        } else {
            h = 118;
        }
        r->y = y, r->h = h;
        y += h;
    }
}

static int first_focus(const struct sel *S, int from, int dir)
{
    for (int i = from; i >= 0 && i < S->nrows; i += dir)
        if (focusable(&S->rows[i]))
            return i;
    return -1;
}

static void scroll_to_focus(struct sel *S)
{
    int view = VIEW_Y1 - VIEW_Y0 - 24, total = S->nrows ? S->rows[S->nrows - 1].y + S->rows[S->nrows - 1].h : 0;
    float to = S->set_scroll_to;
    if (S->set_in && S->set_row >= 0 && S->set_row < S->nrows) {
        const struct srow *r = &S->rows[S->set_row];
        int top = r->y, bottom = r->y + r->h;
        if (first_focus(S, S->set_row - 1, -1) < 0)
            top = 0; /* the first: what is above it shows too */
        if ((float)top < to)
            to = (float)top;
        if ((float)bottom > to + (float)view)
            to = (float)(bottom - view);
    } else {
        to = 0;
    }
    if (to > (float)(total - view))
        to = (float)(total - view);
    if (to < 0)
        to = 0;
    S->set_scroll_to = to;
}

static void section(struct sel *S, int sec)
{
    S->set_sec = sec;
    build(S);
    layout(S);
    S->set_row = first_focus(S, 0, 1);
    S->set_scroll = S->set_scroll_to = 0;
    S->set_row_y = S->set_row >= 0 ? (float)S->rows[S->set_row].y : 0;
    S->changed |= CH_ALL;
}

void set_open(struct sel *S, u64 now)
{
    if (!S->info && !(S->info = sel_alloc(S, sizeof(*S->info))))
        return;
    sel_info_load(S, S->info);
    S->set_open = 1, S->set_in = 0, S->toast_until_us = 0;
    S->set_sec_y = 0;
    section(S, SEC_GAMES);
    sel_log(S, "settings: opened (%d title(s); USB %d, %d drive(s); logs to USB %d; network installer %d; %s)",
            S->ntitles, S->info->usb, S->info->ndrives, S->info->log_usb, S->info->netinst,
            S->info->ip[0] ? S->info->ip : "no address");
}

/* the network installer's card: Circle stops it (held, while a browser installs) */
static void net_step(struct sel *S, u64 now, u32 pressed, u32 held)
{
    if (now >= S->net_poll) {
        struct sel_net st;
        __builtin_memset(&st, 0, sizeof(st));
        if (sel_netinst(S, NETOP_STATUS, &st) == 0 && __builtin_memcmp(&st, &S->net, sizeof(st)) != 0) {
            if (st.installed != S->net_seen) { /* its name: from the snapshot taken while it installed */
                const char *name = st.name[0] ? st.name : S->net.name[0] ? S->net.name : st.title[0] ? st.title
                                 : S->net.title[0] ? S->net.title : "a game";
                char msg[96];
                int usb = S->ntitles && S->titles[0].where[0];
                sel_fmt(msg, sizeof(msg), usb ? "Installed: %s (it shows when no drive has games)" : "Installed: %s", name);
                sel_log(S, "settings: network install finished (%u this session): %s", st.installed, name);
                S->net_seen = st.installed, S->reload_req = 1;
                ui_toast_ok(S, now, msg);
            }
            if (st.changes != S->net_changes) { /* installs and removals: the row again */
                sel_log(S, "settings: the catalogue changed (%u writes this session)", st.changes);
                S->net_changes = st.changes, S->reload_req = 1;
            }
            if (st.state != S->net.state)
                sel_log(S, "settings: network installer %u -> %u: %s", S->net.state, st.state, st.message);
            S->net = st;
            S->changed |= CH_ALL;
        }
        S->net_poll = now + POLL_US;
    }
    int busy = S->net.state == NET_BUSY;
    if ((held & PAD_CIRCLE) && busy) {
        if (!S->net_hold)
            S->net_hold = now;
        S->changed |= CH_ALL; /* the hold's ring fills */
    } else {
        S->net_hold = 0;
    }
    if (((pressed & PAD_CIRCLE) && !busy) || (S->net_hold && now - S->net_hold >= HOLD_US)) {
        sel_netinst(S, NETOP_STOP, 0);
        sel_log(S, "settings: network installer stopped%s", busy ? " during an install (it can go on later)" : "");
        S->net_open = 0, S->net_hold = 0;
        S->net.state = NET_STOPPED;
        S->changed |= CH_ALL;
    }
}

static void act(struct sel *S, u64 now, struct srow *r)
{
    struct sel_info *in = S->info;
    if (r->act == SACT_LOG_USB) {
        int want = !in->log_usb, got = sel_switch(S, CONF_LOG_USB, want);
        sel_log(S, "settings: logs to the USB drive %s: now %d", want ? "on" : "off", got);
        if (got != want)
            ui_toast(S, now, "The setting couldn't be saved");
        else
            sel_info_load(S, in);
    } else if (r->act == SACT_NETINST) {
        __builtin_memset(&S->net, 0, sizeof(S->net));
        S->net_open = 1, S->net_poll = 0, S->net_hold = 0;
        int rc = sel_netinst(S, NETOP_START, &S->net);
        sel_log(S, "settings: network installer started: %d", rc);
        if (rc != 0 && S->net.state != NET_FAILED) {
            S->net.state = NET_FAILED;
            if (!S->net.message[0])
                sel_fmt(S->net.message, sizeof(S->net.message), "It didn't start (%x).", (u32)rc);
        }
        S->net_seen = S->net.installed, S->net_changes = S->net.changes;
    } else if (r->act == SACT_CLOSE) {
        S->ask_open = 1, S->ask_yes = 0, S->ask_focus = 0, S->ask_kind = ASK_CLOSE;
        sel_log(S, "settings: asking before closing ghostldr");
    } else if (r->act >= SACT_SWITCH || r->act == SACT_RESET) {
        int i = r->act == SACT_RESET ? -1 : r->act - SACT_SWITCH;
        int got = sel_switch(S, i, i < 0 ? 0 : !r->on);
        sel_log(S, "settings: %s: %d", i < 0 ? "reset to defaults" : in->sw[i].key, got);
        if (got < 0) {
            ui_toast(S, now, "The setting couldn't be saved");
        } else {
            in->sw_on = 0;
            for (int k = 0; k < in->nsw; k++)
                in->sw_on |= (u32)(sel_switch(S, k, -1) > 0) << k;
            in->sw_safe = 0;
            if (i < 0)
                ui_toast_ok(S, now, "Every switch is back to its default");
        }
    } else if (r->act == SACT_RECHECK) {
        sel_info_load(S, in);
        ui_toast(S, now, "The drives were read again");
    } else {
        return;
    }
    u16 keep = r->act; /* the focus stays on the same entry (rows above it may come or go) */
    build(S);
    layout(S);
    S->set_row = first_focus(S, 0, 1);
    for (int i = 0; i < S->nrows; i++)
        if (focusable(&S->rows[i]) && S->rows[i].act == keep && keep)
            S->set_row = i;
    scroll_to_focus(S);
    S->changed |= CH_ALL;
}

void set_step(struct sel *S, u64 now, u64 dt, u32 pressed, u32 held, int step)
{
    S->set_now = now;
    if (S->net_open) {
        net_step(S, now, pressed, held);
    } else if (!S->set_in) {
        int sec = clampi(S->set_sec + step, 0, NSEC - 1);
        if (sec != S->set_sec)
            section(S, sec);
        if ((pressed & (PAD_RIGHT | PAD_CROSS)) && S->set_row >= 0) {
            S->set_in = 1;
            scroll_to_focus(S);
            S->changed |= CH_ALL;
        } else if (pressed & (PAD_CIRCLE | PAD_SETTINGS)) {
            S->set_open = 0;
            sel_log(S, "settings: closed");
        }
    } else {
        if (step) {
            int f = first_focus(S, S->set_row + step, step);
            if (f >= 0)
                S->set_row = f;
            else if (step < 0)
                S->set_scroll_to = 0; /* above the first: its heading */
            scroll_to_focus(S);
            S->changed |= CH_ALL;
        }
        if (pressed & (PAD_LEFT | PAD_CIRCLE)) {
            S->set_in = 0;
            S->changed |= CH_ALL;
        } else if ((pressed & PAD_CROSS) && S->set_row >= 0) {
            act(S, now, &S->rows[S->set_row]);
        } else if (pressed & PAD_SETTINGS) {
            S->set_open = 0, S->set_in = 0;
        }
    }
    /* animation: the section's pill, the focused row's light, the scroll */
    float k = (float)dt / 90000.0f;
    k = k > 1 ? 1 : k;
    float sy = (float)S->set_sec, ry = S->set_row >= 0 ? (float)S->rows[S->set_row].y : 0;
    S->set_sec_y += (sy - S->set_sec_y) * k;
    S->set_row_y += (ry - S->set_row_y) * k;
    S->set_scroll += (S->set_scroll_to - S->set_scroll) * k;
    if ((S->set_sec_y - sy) * (S->set_sec_y - sy) < 0.0001f)
        S->set_sec_y = sy;
    if ((S->set_row_y - ry) * (S->set_row_y - ry) < 0.25f)
        S->set_row_y = ry;
    if ((S->set_scroll - S->set_scroll_to) * (S->set_scroll - S->set_scroll_to) < 0.25f)
        S->set_scroll = S->set_scroll_to;
    float na = S->net_a;
    S->net_a += ((S->net_open ? 1.0f : 0.0f) - S->net_a) * (dt >= 160000 ? 1.0f : (float)dt / 160000.0f);
    if (S->net_a < 0.01f && !S->net_open)
        S->net_a = 0;
    if (S->net_a > 0.99f && S->net_open)
        S->net_a = 1;
    if (S->net_a != na)
        S->changed |= CH_ALL;
}

int set_busy(const struct sel *S)
{
    if (!S->set_open && S->set_a == 0)
        return 0;
    float ry = S->set_row >= 0 && S->set_row < S->nrows ? (float)S->rows[S->set_row].y : 0;
    return S->set_sec_y != (float)S->set_sec || S->set_row_y != ry || S->set_scroll != S->set_scroll_to ||
           (S->net_a > 0 && S->net_a < 1) || S->net_hold;
}

/* ---- drawing ---- */
static void switch_glyph(surface *s, int x, int cy, int on, u32 a)
{
    int w = 76, h = 40;
    draw_rrect(s, x, cy - h / 2, w, h, h / 2, fade(on ? 0xFF4FB86Cu : 0x40FFFFFFu, a));
    draw_disc(s, (float)(on ? x + w - h / 2 : x + h / 2), (float)cy, (float)h / 2 - 4, 0, fade(0xFFF4F6F8u, a));
}

static void draw_row(struct sel *S, surface *s, const struct srow *r, int y, int focus, u32 a)
{
    int x = CX;
    if (focus) {
        draw_rrect(s, x - 28, y + 4, CW + 56, r->h - 8, 20, fade(S->set_in ? 0x26FFFFFFu : 0x10FFFFFFu, a));
        if (S->set_in)
            draw_rring(s, x - 28, y + 4, CW + 56, r->h - 8, 20, 2, fade(0x70FFFFFFu, a));
    }
    if (r->kind == SROW_HEAD) {
        ui_text_tracked(S, s, 18, x, y + 44, r->text, fade(FAINT, a), 2);
    } else if (r->kind == SROW_TEXT) {
        ui_text_tracked(S, s, 18, x, y + 30, r->text, fade(FAINT, a), 2);
        char lines[3][128];
        int n = r->nsub ? ui_wrap(S, 27, CW, r->sub[0], lines, 3) : 0;
        for (int i = 0; i < n; i++)
            draw_text(S, s, 27, x, y + 72 + 38 * i, lines[i], fade(WHITE, a), CW);
    } else if (r->kind == SROW_ITEM) {
        int sign = r->chip_c == RED || r->chip_c == AMBER;
        int cw = r->chip[0] ? text_width(S, 21, r->chip) + 36 + (sign ? 30 : 0) : 0;
        draw_text(S, s, 29, x, y + 48, r->text, fade(WHITE, a), CW - cw - 24);
        if (cw)
            ui_chip(S, s, x + CW - cw, y + 18, r->chip_c, sign, r->chip, a);
        for (int i = 0; i < r->nsub; i++)
            draw_text(S, s, 23, x, y + 88 + 34 * i, r->sub[i],
                      fade(i == 0 ? DIM : r->chip_c == RED && i == 1 ? 0xFFFFB4AAu : SOFT, a), CW);
    } else {
        draw_text(S, s, 29, x, y + 50, r->text, fade(WHITE, a), CW - 140);
        if (r->nsub)
            draw_text(S, s, 23, x, y + 88, r->sub[0], fade(DIM, a), CW - 140);
        if (r->kind == SROW_TOGGLE)
            switch_glyph(s, x + CW - 76, y + r->h / 2, r->on, a);
        else
            draw_text(S, s, 40, x + CW - 24, y + r->h / 2 + 14, "\xE2\x80\xBA", fade(DIM, a), 0);
    }
}

/* the bar of an install: done of total */
static void bar(surface *s, int x, int y, int w, u64 done, u64 total, u32 a)
{
    int h = 16, fill = total ? (int)((u64)w * (done < total ? done : total) / total) : 0;
    draw_rrect(s, x, y, w, h, h / 2, fade(0x30FFFFFFu, a));
    if (fill > 0)
        draw_rrect(s, x, y, fill < h ? h : fill, h, h / 2, fade(0xFFF4F6F8u, a));
}

static void net_card(struct sel *S, surface *s, float k)
{
    float e = 1 - (1 - k) * (1 - k);
    u32 a = (u32)(e * 255);
    draw_dim(s, 0, SCREEN_W, 0, SCREEN_H, (int)(190 * e));
    int x = (SCREEN_W - NET_W) / 2, y = (SCREEN_H - NET_H) / 2 - 24 + (int)((1 - e) * 28), w = NET_W - 112;
    draw_rrect(s, x, y, NET_W, NET_H, 28, fade(0xFF202530u, a)); /* opaque: Settings' text stays behind it */
    ui_card(s, x, y, NET_W, NET_H, e);
    draw_ghost(s, x + 84, y + 84, 56, fade(0xF0FFFFFFu, a), fade(0xFF1B1F27u, a));
    ui_text_bold(S, s, 36, x + 136, y + 98, "Install over network", fade(WHITE, a), w);
    const struct sel_net *n = &S->net;
    int tx = x + 56, ty = y + 186;
    char line[200], d[24], t[24];
    if (n->state == NET_WAITING && n->url[0]) { /* a QR code of the address, for a phone's camera */
        if (!str_same(S->qr_text, n->url)) {
            str_copy(S->qr_text, n->url, sizeof(S->qr_text));
            if (qr_encode(n->url, S->qr) != 0)
                S->qr_text[0] = 0;
        }
        if (S->qr_text[0]) {
            int qs = 296;
            qr_draw(s, x + NET_W - 56 - qs, y + 150, qs, S->qr, a);
            int cw = text_width(S, 21, "Or scan it with a phone");
            draw_text(S, s, 21, x + NET_W - 56 - qs + (qs - cw) / 2, y + 150 + qs + 34, "Or scan it with a phone",
                      fade(DIM, a), 0);
            w -= qs + 40;
        }
    }
    if (n->state == NET_WAITING || n->state == NET_BUSY) {
        draw_text(S, s, 25, tx, ty, n->state == NET_BUSY ? "Installing from" : "On a phone or computer on the same network, open",
                  fade(DIM, a), w);
        ui_text_bold(S, s, n->state == NET_BUSY ? 34 : text_width(S, 56, n->url) <= w ? 56 : 44, tx,
                     ty + (n->state == NET_BUSY ? 52 : 74), n->url, fade(WHITE, a), w);
        ty += n->state == NET_BUSY ? 104 : 150;
        if (n->state == NET_WAITING) {
            ui_text_tracked(S, s, 18, tx, ty, "TOKEN", fade(FAINT, a), 2);
            ui_text_bold(S, s, 44, tx, ty + 56, n->token[0] ? n->token : "-", fade(WHITE, a), w);
            draw_text(S, s, 23, tx, ty + 98, "The page asks for it before it changes anything.", fade(DIM, a), w);
            ty += 140;
        } else {
            sel_fmt(line, sizeof(line), "%s%s%s", n->name[0] ? n->name : n->title, n->name[0] && n->title[0] ? "  \xC2\xB7  " : "",
                    n->name[0] ? n->title : "");
            draw_text(S, s, 30, tx, ty, line, fade(WHITE, a), w);
            bar(s, tx, ty + 30, w, n->done, n->total, a);
            ui_size(d, sizeof(d), n->done), ui_size(t, sizeof(t), n->total);
            int len = sel_fmt(line, sizeof(line), "%s of %s", d, t);
            if (n->rate_kib) {
                len += sel_fmt(line + len, (int)sizeof(line) - len, "  \xC2\xB7  %u.%u MB/s", n->rate_kib / 1024,
                               n->rate_kib % 1024 * 10 / 1024);
                if (n->total > n->done) {
                    u64 sec = (n->total - n->done) / 1024 / n->rate_kib;
                    sel_fmt(line + len, (int)sizeof(line) - len, sec < 90 ? "  \xC2\xB7  about %u s left"
                                                                          : "  \xC2\xB7  about %u min left",
                            (u32)(sec < 90 ? sec : (sec + 30) / 60));
                }
            }
            draw_text(S, s, 25, tx, ty + 92, line, fade(SOFT, a), w);
            ty += 130;
        }
    } else if (n->state == NET_FAILED) {
        draw_warning(s, (float)tx + 22, (float)ty - 8, 40, fade(RED, a), fade(0xFF2A1E08u, a));
        draw_text(S, s, 30, tx + 56, ty, "The network installer couldn't start", fade(WHITE, a), w - 56);
        ty += 60;
    } else {
        draw_text(S, s, 30, tx, ty, n->state == NET_STOPPED ? "The network installer stopped" : "Starting the installer\xE2\x80\xA6",
                  fade(WHITE, a), w);
        ty += 60;
    }
    if (n->message[0]) {
        char lines[2][128];
        int m = ui_wrap(S, 24, w, n->message, lines, 2);
        for (int i = 0; i < m; i++)
            draw_text(S, s, 24, tx, y + NET_H - 96 + 34 * i, lines[i], fade(n->state == NET_FAILED ? 0xFFFFB4AAu : DIM, a), w);
    }
    if (n->installed) {
        sel_fmt(line, sizeof(line), "Installed this time: %u", n->installed);
        draw_text(S, s, 24, tx, y + NET_H - 36, line, fade(GREEN, a), w);
    }
}

void set_draw(struct sel *S, surface *s)
{
    float k = S->set_a, e = 1 - (1 - k) * (1 - k);
    u32 a = (u32)(e * 255);
    int dx = (int)((1 - e) * 48);
    draw_dim(s, 0, SCREEN_W, 0, SCREEN_H, (int)(120 * e));
    draw_vgradient(s, 0, 0, SCREEN_W, SCREEN_H, fade(0xFF171B22u, a), fade(0xFF0A0C10u, a));
    draw_rect_blend(s, PANE_X, VIEW_Y0, 1, VIEW_Y1 - VIEW_Y0, fade(0x30FFFFFFu, a));
    /* the heading, where the top bar's mark is */
    ui_gear(s, (float)MARGIN + 16, 70, 15, fade(0xF0FFFFFFu, a), fade(0xFF161A21u, a));
    ui_text_shadowed(S, s, 34, MARGIN + 50, 82, "Settings", fade(WHITE, a), 0);
    /* the sections */
    float py = (float)LIST_Y + S->set_sec_y * LIST_ROW;
    draw_rrect(s, LIST_X - 24 + dx, (int)py, LIST_W, LIST_ROW - 10, (LIST_ROW - 10) / 2,
               fade(S->set_in ? 0x26FFFFFFu : 0xFFF4F6F8u, a));
    for (int i = 0; i < NSEC; i++) {
        float d = S->set_sec_y - (float)i;
        int lit = S->set_in ? 0 : (int)(clampf(1 - (d < 0 ? -d : d), 0, 1) * 256);
        draw_text(S, s, 30, LIST_X + dx, LIST_Y + i * LIST_ROW + 44, SEC_NAME[i], fade(mix_rgb(WHITE, INK, lit), a), 0);
    }
    /* the rows, scrolled, between VIEW_Y0 and VIEW_Y1 */
    int cy0 = s->cy0, cy1 = s->cy1;
    int v0 = surf_y0(s) > VIEW_Y0 ? surf_y0(s) : VIEW_Y0, v1 = surf_y1(s) < VIEW_Y1 ? surf_y1(s) : VIEW_Y1;
    if (v0 < v1) {
        s->cy0 = v0, s->cy1 = v1;
        int top = VIEW_Y0 + 8 - (int)S->set_scroll;
        for (int i = 0; i < S->nrows; i++) {
            const struct srow *r = &S->rows[i];
            int y = top + r->y;
            if (y > VIEW_Y1 || y + r->h < VIEW_Y0)
                continue;
            draw_row(S, s, r, y - dx / 2, i == S->set_row && S->set_in, a);
        }
        s->cy0 = cy0, s->cy1 = cy1;
        int total = S->nrows ? S->rows[S->nrows - 1].y + S->rows[S->nrows - 1].h : 0;
        /* more above or below: the edge fades, and a chevron */
        float ax = (float)(SCREEN_W - MARGIN + 44);
        if (S->set_scroll > 1) {
            draw_vgradient(s, PANE_X + 1, VIEW_Y0, SCREEN_W - PANE_X - 1, 36, fade(0xFF14181Eu, a), 0x0014181Eu);
            draw_line(s, ax - 10, VIEW_Y0 + 22, ax, VIEW_Y0 + 12, 3, fade(DIM, a));
            draw_line(s, ax, VIEW_Y0 + 12, ax + 10, VIEW_Y0 + 22, 3, fade(DIM, a));
        }
        if ((float)total - S->set_scroll > (float)(VIEW_Y1 - VIEW_Y0 - 24) + 1) {
            draw_vgradient(s, PANE_X + 1, VIEW_Y1 - 36, SCREEN_W - PANE_X - 1, 36, 0x000D1014u, fade(0xFF0D1014u, a));
            draw_line(s, ax - 10, VIEW_Y1 - 22, ax, VIEW_Y1 - 12, 3, fade(DIM, a));
            draw_line(s, ax, VIEW_Y1 - 12, ax + 10, VIEW_Y1 - 22, 3, fade(DIM, a));
        }
    }
    if (S->net_a > 0)
        net_card(S, s, S->net_a);
}

void set_hints(struct sel *S, surface *s)
{
    int base = SCREEN_H - 64, x = SCREEN_W - MARGIN;
    if (S->net_a > 0.5f) {
        if (S->net.state == NET_BUSY) {
            char label[48];
            u64 held = S->net_hold && S->set_now > S->net_hold ? S->set_now - S->net_hold : 0;
            sel_fmt(label, sizeof(label), held ? "Stopping in %u.%u s" : "Hold to stop", (u32)((HOLD_US - held) / 1000000),
                    (u32)((HOLD_US - held) / 100000 % 10));
            ui_hint(S, s, x, base, PAD_CIRCLE, held < HOLD_US ? label : "Stopping", SOFT);
        } else {
            ui_hint(S, s, x, base, PAD_CIRCLE, "Stop", SOFT);
        }
        return;
    }
    if (!S->set_in) {
        x = ui_hint(S, s, x, base, PAD_CIRCLE, "Close", SOFT) - 44;
        if (S->set_row >= 0)
            ui_hint(S, s, x, base, PAD_CROSS, "Open", SOFT);
        return;
    }
    x = ui_hint(S, s, x, base, PAD_CIRCLE, "Back", SOFT) - 44;
    const struct srow *r = S->set_row >= 0 ? &S->rows[S->set_row] : 0;
    if (r && r->kind == SROW_TOGGLE)
        ui_hint(S, s, x, base, PAD_CROSS, "Change", SOFT);
    else if (r && r->kind == SROW_ACTION)
        ui_hint(S, s, x, base, PAD_CROSS, r->act == SACT_CLOSE ? "Close ghostldr" : r->act == SACT_RECHECK ? "Look again"
                                          : r->act == SACT_RESET ? "Reset" : "Start", SOFT);
}
