/*
 * Settings' switches (Settings > Advanced): what run.py's flags did on a
 * computer, kept on the console.  Each switch is a boot context flag
 * (ghost.h CTX_FLAG_*).  They are kept in a save container of ghostldr's
 * own, GLDRCONF/settings.txt (no drive needed):
 *
 *     # ghostldr settings: the game selector's Settings writes this file
 *     dialogs=off
 *     flags_on=0x400000
 *     flags_off=0x0
 *
 * A key per switch that isn't at its default, and the two masks they make:
 * the release's ghostldr.lua sets flags_on and clears flags_off in the
 * flags it boots with (both calls), and the selector applies a change to
 * this boot's flags at once.  Switches at their defaults leave the masks
 * empty, so run.py's own flags stand (run.py doesn't read the file).
 *
 * Safe mode: ghostldr.lua writes pending=1 before it applies the masks, and
 * the selector clears it when it comes up.  A boot that finds pending=1
 * (the last one with the masks never reached the selector) leaves them out
 * once, writes pending=safe and says so; the selector then says so too.
 * Reset to defaults empties the file's switches.
 */
#include "plat.h"

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
size_t strlen(const char *s);

#define CONF_DIR "GLDRCONF"
#define CONF_FILE "settings.txt"
#define CONF_BLOCKS 96 /* the smallest save container */
#ifndef CTX_FLAG_NO_USB_LOG /* rel-diag's (ghost.h): the log is not written to a USB drive */
#define CTX_FLAG_NO_USB_LOG (1ull << 24)
#endif
#define SAVE_NOT_FOUND ((s32)0x809F0008)

/* switch i: its key, words, flag, whether On sets the flag, and its default
 * (a function, not a table: the module has no relocations) */
int conf_switch(int i, struct sel_switch *w)
{
    memset(w, 0, sizeof(*w));
    switch (i) {
    case CONF_LOG_USB: /* shown under Logs */
        str_copy(w->key, "log_usb", sizeof(w->key));
        str_copy(w->label, "Write logs to the USB drive", sizeof(w->label));
        str_copy(w->on, "On: each boot's log goes to the folder ghostldr/logs on the drive", sizeof(w->on));
        str_copy(w->off, "Off: nothing is written to the drive", sizeof(w->off));
        w->flag = CTX_FLAG_NO_USB_LOG, w->on_sets = 0, w->def = 1, w->logs = 1;
        return 0;
    case 1:
        str_copy(w->key, "dialogs", sizeof(w->key));
        str_copy(w->label, "Show the system's dialogs", sizeof(w->label));
        str_copy(w->on, "On: a game's save, message and keyboard dialogs show and wait for you", sizeof(w->on));
        str_copy(w->off, "Off: they are answered at once and not shown (a game stuck at one may get past it)",
                 sizeof(w->off));
        w->flag = 0x400000, w->on_sets = 0, w->def = 1; /* run.py --dialog-auto */
        return 0;
    case 2:
        str_copy(w->key, "auto_memory", sizeof(w->key));
        str_copy(w->label, "Plan memory automatically", sizeof(w->label));
        str_copy(w->on, "On: a game without a memory plan of its own gets one, learned from its runs", sizeof(w->on));
        str_copy(w->off, "Off: such a game gets the plain budget", sizeof(w->off));
        w->flag = CTX_FLAG_NO_AUTO_MEM, w->on_sets = 0, w->def = 1; /* run.py --no-auto-memory */
        return 0;
    case 3:
        str_copy(w->key, "remote_play", sizeof(w->key));
        str_copy(w->label, "Keep Remote Play on", sizeof(w->label));
        str_copy(w->on, "On: a game that turns Remote Play off can't (for streaming the TV to another device)",
                 sizeof(w->on));
        str_copy(w->off, "Off: a game may turn Remote Play off, as on its own", sizeof(w->off));
        w->flag = CTX_FLAG_KEEP_REMOTE_PLAY, w->on_sets = 1, w->def = 0; /* run.py --keep-remote-play */
        return 0;
    }
    return -1;
}

/* the value of key in text: its length, or -1 */
static int get(const char *t, int n, const char *key, char *val, int cap)
{
    for (int at = 0; at < n;) {
        int end = at, k = 0;
        while (end < n && t[end] != '\n')
            end++;
        while (key[k] && at + k < end && t[at + k] == key[k])
            k++;
        if (!key[k] && at + k < end && t[at + k] == '=') {
            int v = at + k + 1, len = 0;
            while (v + len < end && t[v + len] != '\r' && len + 1 < cap)
                val[len] = t[v + len], len++;
            val[len] = 0;
            return len;
        }
        at = end + 1;
    }
    return -1;
}

static u64 hex(const char *s)
{
    u64 v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    for (; *s; s++) {
        int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10
              : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
        if (d < 0)
            break;
        v = v << 4 | (u64)d;
    }
    return v;
}

static int conf_write(struct plat *P);

/* GLDRCONF/settings.txt: the switches' states (P->conf_on); a pending
 * marker goes (this boot reached the selector) */
void conf_load(struct plat *P)
{
    char mp[16], path[48], t[CONF_MAX + 1], v[24];
    int n = 0;
    if (P->api->gfs_mount(CONF_DIR, 1, 0, mp) == 0) {
        P->mounts++;
        fmt(path, sizeof(path), "%s/%s", mp, CONF_FILE);
        int fd = P->k->open(path, O_RDONLY, 0);
        if (fd >= 0) {
            ssize_t r = P->k->read(fd, t, CONF_MAX);
            n = r > 0 ? (int)r : 0;
            P->k->close(fd);
        }
        P->api->gfs_umount(mp, 2000);
        P->umounts++;
    }
    struct sel_switch w;
    for (int i = 0; conf_switch(i, &w) == 0; i++) {
        int on = w.def;
        if (get(t, n, w.key, v, sizeof(v)) > 0)
            on = str_same(v, "on");
        P->conf_on |= (u32)on << i;
    }
    u64 on = get(t, n, "flags_on", v, sizeof(v)) > 0 ? hex(v) : 0, off = get(t, n, "flags_off", v, sizeof(v)) > 0 ? hex(v) : 0;
    int pending = get(t, n, "pending", v, sizeof(v)) > 0;
    P->conf_safe = pending && str_same(v, "safe");
    /* this boot's flags: with them (ghostldr.lua applied them), without, or nothing to apply */
    P->conf_live = !(on | off) ? -1 : (P->ctx->flags & on) == on && !(P->ctx->flags & off);
    LOG(P, "settings: %s/%s: %d bytes, switches %x, flags_on %lx, flags_off %lx (this boot %s them)%s", CONF_DIR,
        CONF_FILE, n, P->conf_on, on, off, P->conf_live < 0 ? "needs none of" : P->conf_live ? "has" : "lacks", !pending ? "" : P->conf_safe ? "; the last boot with them stopped before the selector: "
                                                             "left out of this one" : "; pending: cleared");
    if (pending)
        conf_write(P);
}

/* the file for the switches' states: a key and a mask bit per switch off its default */
static int conf_text(struct plat *P, char *t, int cap, u64 *on_mask, u64 *off_mask)
{
    int n = fmt(t, (size_t)cap, "# ghostldr settings: the game selector's Settings writes this file\n");
    struct sel_switch w;
    *on_mask = *off_mask = 0;
    for (int i = 0; conf_switch(i, &w) == 0; i++) {
        int on = (P->conf_on >> i) & 1;
        if (on == w.def)
            continue;
        n += fmt(t + n, (size_t)(cap - n), "%s=%s\n", w.key, on ? "on" : "off");
        if (on == w.on_sets)
            *on_mask |= w.flag;
        else
            *off_mask |= w.flag;
    }
    n += fmt(t + n, (size_t)(cap - n), "flags_on=0x%lx\nflags_off=0x%lx\n", *on_mask, *off_mask);
    return n;
}

/* the file for P->conf_on (the container made when missing): 0, or an SCE error */
static int conf_write(struct plat *P)
{
    char t[CONF_MAX], mp[16], path[48];
    u64 on_mask, off_mask;
    int n = conf_text(P, t, sizeof(t), &on_mask, &off_mask);
    int rc = P->api->gfs_mount(CONF_DIR, 2 /* read-write */, 0, mp);
    if (rc == SAVE_NOT_FOUND)
        rc = P->api->gfs_mount(CONF_DIR, 6 /* create, read-write */, CONF_BLOCKS, mp);
    if (rc == 0) {
        P->mounts++;
        fmt(path, sizeof(path), "%s/%s", mp, CONF_FILE);
        int fd = P->k->open(path, 0x601 /* O_WRONLY | O_CREAT | O_TRUNC */, 0666);
        rc = fd < 0 ? fd : P->k->write(fd, t, (size_t)n) == n ? 0 : -1;
        if (fd >= 0)
            P->k->close(fd);
        int u = P->api->gfs_umount(mp, 2000); /* the commit */
        P->umounts++;
        rc = rc ? rc : u;
    }
    LOG(P, "settings: %s/%s written: %x (switches %x: flags_on %lx, flags_off %lx)", CONF_DIR, CONF_FILE, (u32)rc,
        P->conf_on, on_mask, off_mask);
    return rc;
}

/* switch i on or off: saved and applied to this boot; 0, or an SCE error */
int conf_set(struct plat *P, int i, int on)
{
    struct sel_switch w;
    if (conf_switch(i, &w) != 0)
        return -1;
    u32 was = P->conf_on;
    P->conf_on = (P->conf_on & ~(1u << i)) | ((u32)(on != 0) << i);
    int rc = conf_write(P);
    if (rc != 0) {
        P->conf_on = was;
        return rc;
    }
    /* this boot too (the release's second call takes them from the file) */
    if ((on != 0) == w.on_sets)
        P->ctx->flags |= w.flag;
    else
        P->ctx->flags &= ~w.flag;
    return 0;
}

/* every switch at its default */
int conf_reset(struct plat *P)
{
    struct sel_switch w;
    for (int i = 0; conf_switch(i, &w) == 0; i++)
        if ((int)((P->conf_on >> i) & 1) != w.def) {
            int rc = conf_set(P, i, w.def);
            if (rc != 0)
                return rc;
        }
    return conf_write(P);
}

int sel_switch(struct sel *S, int i, int want)
{
    struct plat *P = PLAT(S);
    if (i < 0) /* Reset to defaults */
        return conf_reset(P) == 0 ? 0 : -1;
    if (want >= 0 && conf_set(P, i, want) != 0)
        return -1;
    return (int)((P->conf_on >> i) & 1);
}
