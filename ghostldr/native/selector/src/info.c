/*
 * What Settings shows (setui.c), read on the console when it opens, and its
 * calls into the core:
 *
 *  - ghostldr: the version this module was built from (GHOSTLDR_VERSION, git
 *    describe), and the bundle: GLDRCACHE/bundle.bin's header (mkbundle.py),
 *    which is this boot's when its selector CRC is the one the core checked;
 *  - the console and its firmware (ctx fw, from Luac0re's FW_VERSION), the
 *    host game from its own /app0/sce_sys/param.sfo (read only);
 *  - the console's address (ghostldr.lua's, in listen mode; else
 *    sceNetCtlGetInfo) and the ports ghostldr's features use;
 *  - the USB drives: libSceUsbStorage's device list and each drive's
 *    GetDeviceInfo (size, free space, maker and model, file system), read
 *    only; the games on each from the extension's list (a drive's games are
 *    under /ghostldr_usb, /ghostldr_usb2, ... in the device list's order);
 *  - the log on the USB drive (ghost_api.log_usb) and the network installer
 *    (native/netinst, linked into this module when it is in the tree: a web
 *    server that runs while the selector does, in the emulator's JIT buffer
 *    with the rest of this module, so it costs no new JIT memory).
 */
#include "plat.h"
#if __has_include("netinst.h")
#include "netinst.h"
#define HAVE_NETINST 1
#endif

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
size_t strlcpy(char *d, const char *s, size_t n);
size_t strlen(const char *s);

#ifndef GHOSTLDR_VERSION
#define GHOSTLDR_VERSION "unknown"
#endif

#define USBS_NOT_INIT ((s32)0x80F40009)
#define USB_MOUNT "/ghostldr_usb"

/* ---- the bundle and its cache ---- */
#define BNDL_HEADER 128
static void bundle(struct plat *P, struct sel_info *in)
{
    u8 h[BNDL_HEADER];
    char mp[16], path[48];
    s64 got = -1;
    memset(h, 0, sizeof(h));
    if (P->api->gfs_mount("GLDRCACHE", 1, 0, mp) == 0) {
        P->mounts++;
        fmt(path, sizeof(path), "%s/bundle.bin", mp);
        int fd = P->k->open(path, O_RDONLY, 0);
        if (fd >= 0) {
            got = P->k->read(fd, h, sizeof(h));
            P->k->close(fd);
        }
        P->api->gfs_umount(mp, 2000);
        P->umounts++;
    }
    u32 kind = *(u32 *)(h + 16), sel_crc = *(u32 *)(h + 44);
    u64 ext_size = *(u64 *)(h + 24), sel_size = *(u64 *)(h + 32);
    char build[20];
    for (int i = 0; i < 8; i++)
        fmt(build + 2 * i, 3, "%02x", h[112 + i]);
    int ok = got == BNDL_HEADER && !memcmp(h, "GLDRBNDL", 8) && (kind == 1 || kind == 2);
    int ours = ok && kind == 1 && sel_crc == (u32)P->ctx->sel_crc;
    char size[24];
    ui_size(size, sizeof(size), BNDL_HEADER + ext_size + sel_size);
    if (!ok)
        str_copy(in->cache, got < 0 ? "Empty: the bundle sent after ghostldr.lua is kept here, so that ghostldr.lua "
                                      "alone is enough the next time"
                                    : "Damaged: the next bundle sent replaces it",
                 sizeof(in->cache));
    else if (ours)
        fmt(in->cache, sizeof(in->cache), "ghostldr-ext.bin of this build (%s): send ghostldr.lua alone the next time",
            size);
    else
        fmt(in->cache, sizeof(in->cache), "%s of build %s (%s), not this boot's: the next bundle sent replaces it",
            kind == 1 ? "ghostldr-ext.bin" : "ghostldr-elfldr.bin", build, size);
    if (ours)
        fmt(in->build, sizeof(in->build), "%s (bundle)", build);
    else if (P->ctx->flags & CTX_FLAG_BUNDLE)
        fmt(in->build, sizeof(in->build), "A bundle (selector CRC-32 %08x)", (u32)P->ctx->sel_crc);
    else
        fmt(in->build, sizeof(in->build), "Served by run.py (selector CRC-32 %08x)", (u32)P->ctx->sel_crc);
    LOG(P, "settings: bundle cache: %s, kind %u, build %s, selector CRC %x (this boot's %x)",
        got < 0 ? "none" : ok ? "read" : "damaged", kind, build, sel_crc, (u32)P->ctx->sel_crc);
}

/* ---- the host game: its own param.sfo ---- */
static void host(struct sel *S, struct plat *P, struct sel_info *in)
{
    u8 sfo[4096];
    char id[16], ver[16], name[96];
    id[0] = ver[0] = name[0] = 0;
    int fd = P->k->open("/app0/sce_sys/param.sfo", O_RDONLY, 0);
    s64 n = fd >= 0 ? P->k->read(fd, sfo, sizeof(sfo)) : -1;
    if (fd >= 0)
        P->k->close(fd);
    if (n > 20) {
        sfo_get(sfo, (u64)n, "TITLE_ID", id, sizeof(id), NULL);
        sfo_get(sfo, (u64)n, "APP_VER", ver, sizeof(ver), NULL);
        sfo_get(sfo, (u64)n, "TITLE", name, sizeof(name), NULL);
    }
    const u64 *slots = (const u64 *)P->ctx;
    if (!id[0] && P->ctx->size > CTX_CONSOLE_IP_SLOT * 8 && slots[CTX_CONSOLE_IP_SLOT]) {
        /* ghostldr.lua leaves Luac0re's TITLE_ID 48 bytes into the address's buffer */
        const char *t = (const char *)slots[CTX_CONSOLE_IP_SLOT] + 48;
        int ok = 1;
        for (int i = 0; i < 9 && ok; i++)
            ok = i < 4 ? t[i] >= 'A' && t[i] <= 'Z' : t[i] >= '0' && t[i] <= '9';
        if (ok && !t[9])
            str_copy(id, t, sizeof(id));
    }
    int jakx = (P->ctx->flags & CTX_FLAG_JAKX) != 0;
    const char *region = !strcmp(id, "CUSA03474") ? "US" : !strcmp(id, "CUSA03492") ? "EU" : !strcmp(id, "CUSA07992") ? "EU" : "";
    if (!name[0] || !text_fits_font(S, name))
        str_copy(name, jakx ? "Jak X: Combat Racing" : "Star Wars Racer Revenge", sizeof(name));
    if (id[0] && ver[0])
        fmt(in->host, sizeof(in->host), "%s  \xC2\xB7  %s%s%s%s  \xC2\xB7  version %s  \xC2\xB7  %s", name, region,
            region[0] ? " (" : "", id, region[0] ? ")" : "", ver, jakx ? "UltraC0re" : "Luac0re");
    else if (id[0])
        fmt(in->host, sizeof(in->host), "%s  \xC2\xB7  %s%s%s%s  \xC2\xB7  %s", name, region, region[0] ? " (" : "", id,
            region[0] ? ")" : "", jakx ? "UltraC0re" : "Luac0re");
    else
        fmt(in->host, sizeof(in->host), "%s  \xC2\xB7  %s", name, jakx ? "UltraC0re" : "Luac0re");
    LOG(P, "settings: host %s %s '%s' (param.sfo %ld bytes)", id, ver, name, n);
}

/* ---- the network ---- */
struct netctl_info {
    char ip[16];
    u8 rest[496];
};
void info_network_ip(struct plat *P, char ip[16])
{
    ip[0] = 0;
    const u64 *slots = (const u64 *)P->ctx;
    if (P->ctx->size > CTX_CONSOLE_IP_SLOT * 8 && slots[CTX_CONSOLE_IP_SLOT]) { /* listen mode: ghostldr.lua's */
        const char *known = (const char *)slots[CTX_CONSOLE_IP_SLOT];
        if (known[0] >= '0' && known[0] <= '9')
            str_copy(ip, known, 16);
    }
    if (!ip[0]) {
        u32 h = sys_module(P, "libSceNetCtl.sprx", 0, NULL);
        s32 (*get)(s32, void *) = h ? (s32(*)(s32, void *))sys_sym(P, h, "sceNetCtlGetInfo") : NULL;
        struct netctl_info ni;
        memset(&ni, 0, sizeof(ni));
        s32 rc = get ? get(14 /* SCE_NET_CTL_INFO_IP_ADDRESS */, &ni) : -1;
        if (rc == 0 && ni.ip[0] >= '0' && ni.ip[0] <= '9')
            str_copy(ip, ni.ip, 16);
        LOG(P, "sceNetCtlGetInfo: %x '%s'", (u32)rc, ip);
    }
}

static void network(struct plat *P, struct sel_info *in)
{
    info_network_ip(P, in->ip);
    u64 f = P->ctx->flags;
    int n = 0;
    struct sel_port *pt = in->ports;
    int jakx = (f & CTX_FLAG_JAKX) != 0;
    pt[n].port = jakx ? 8888 : 9026;
    fmt(pt[n++].what, sizeof(pt[0].what), "%s's loader: send ghostldr.lua here", jakx ? "UltraC0re" : "Luac0re");
    if (f & CTX_FLAG_BUNDLE) {
        u32 el = (f & CTX_FLAG_ELFLDR) ? CTX_ELFLDR_PORT(f) : 0;
        pt[n].port = (u16)(el ? el : 9021);
        str_copy(pt[n++].what, el ? "ghostldr's bundle port and ELF loader (it listens now)"
                                  : "ghostldr's bundle port and ELF loader (9022 when 9021 was taken)",
                 sizeof(pt[0].what));
    }
    u32 ftp = CTX_SELECTOR_FTP_PORT(f);
    if (ftp) {
        pt[n].port = (u16)ftp;
        str_copy(pt[n++].what, "FTP, while a game runs: the USB drive, and the game's own files", sizeof(pt[0].what));
    }
    if (in->netinst) {
        pt[n].port = 8090;
        str_copy(pt[n++].what, "The network installer, while it runs (its page and its uploads)", sizeof(pt[0].what));
    }
    in->nports = n;
}

/* ---- USB drives (read only) ---- */
static int drive_of(const char *path)
{
    int m = str_len(USB_MOUNT);
    if (strncmp(path, USB_MOUNT, (size_t)m) != 0)
        return -1;
    if (path[m] == '/')
        return 0;
    if (path[m] >= '2' && path[m] <= '9' && path[m + 1] == '/')
        return path[m] - '1';
    return -1;
}

/* a game's folder as the drive shows it: without the mount point; "USB drive
 * 2: ..." when more than one drive has games (shown as is: see ui.c) */
void info_title_where(struct plat *P, const char *path, u32 len, char *out, int cap)
{
    int d = drive_of(path), many = 0, m = str_len(USB_MOUNT);
    for (int i = 0; i < P->ncat; i++)
        if (drive_of(P->cat[i].path) > 0)
            many = 1;
    const char *w = path;
    if (d >= 0) {
        int skip = m + (d > 0) + 1;
        if ((int)len > skip)
            w += skip, len -= (u32)skip;
    }
    int n = 0;
    if (d >= 0 && many)
        n = fmt(out, (size_t)cap, "USB drive %d: ", d + 1);
    str_copy(out + n, w, (int)len + 1 < cap - n ? (int)len + 1 : cap - n);
}

static void drives(struct plat *P, struct sel_info *in)
{
    u64 f = P->ctx->flags;
    in->ndrives = 0;
    if (f & CTX_FLAG_PS4) {
        in->usb = USB_PS4;
        return;
    }
    u32 h = sys_module(P, "libSceUsbStorage.sprx", 0, NULL);
    if (!h) /* the extension didn't look (an app root given): loaded as it would, and kept */
        h = sys_module(P, "libSceUsbStorage.sprx", 1, &P->mod_usb);
    s32 (*init)(void *) = h ? (s32(*)(void *))sys_sym(P, h, "sceUsbStorageInit") : NULL;
    s32 (*list)(u32 *, s32 *) = h ? (s32(*)(u32 *, s32 *))sys_sym(P, h, "sceUsbStorageGetDeviceList") : NULL;
    s32 (*info)(u32, u8 *) = h ? (s32(*)(u32, u8 *))sys_sym(P, h, "sceUsbStorageGetDeviceInfo") : NULL;
    u32 ids[16];
    s32 n = 0;
    memset(ids, 0, sizeof(ids));
    s32 rc = list ? list(ids, &n) : -1;
    if (rc == USBS_NOT_INIT && init) { /* as approot.c does: the session stays for the title */
        s32 r = init(NULL);
        LOG(P, "settings: sceUsbStorageInit -> %x", (u32)r);
        if (r == 0) {
            P->ctx->flags |= CTX_FLAG_USB_SESSION;
            rc = list(ids, &n);
        }
    }
    LOG(P, "settings: sceUsbStorageGetDeviceList -> %x, %d drive(s)", (u32)rc, n);
    if (rc != 0 || !info) {
        in->usb = USB_NONE;
        return;
    }
    in->usb = USB_READ;
    for (s32 i = 0; i < n && in->ndrives < SEL_MAX_DRIVES; i++) {
        u8 buf[0x400];
        memset(buf, 0, sizeof(buf));
        s32 r = info(ids[i], buf);
        struct sel_drive *d = &in->drives[in->ndrives++];
        memset(d, 0, sizeof(*d));
        d->id = ids[i];
        u32 features = 0;
        if (r == 0) {
            char maker[64], model[64];
            str_copy(maker, (const char *)buf + 0x16, sizeof(maker));
            str_copy(model, (const char *)buf + 0x120, sizeof(model));
            for (char *c = maker; *c; c++) /* uninitialised server bytes past the strings: printable only */
                if ((u8)*c < 0x20 || (u8)*c > 0x7E)
                    *c = 0;
            for (char *c = model; *c; c++)
                if ((u8)*c < 0x20 || (u8)*c > 0x7E)
                    *c = 0;
            fmt(d->name, sizeof(d->name), "%s%s%s", maker, maker[0] && model[0] ? " " : "", model);
            d->total = *(u64 *)buf, d->free = *(u64 *)(buf + 8);
            features = *(u32 *)(buf + 0x330);
            d->fs = features & 0x80 ? FS_EXFAT : features & 0x40 ? FS_FAT32 : FS_UNKNOWN; /* as measured (docs/usb.md) */
        }
        /* its games: in the extension's list under its mount point; else none
         * when the extension looked at the drives (the session is open) */
        d->games = d->bad = (P->ctx->flags & CTX_FLAG_USB_SESSION) ? 0 : -1;
        for (int k = 0; k < P->ncat; k++) /* a library's games; or the one game the extension mounted */
            if (!P->ghostfs && drive_of(P->library ? P->cat[k].path : P->root) == in->ndrives - 1) {
                if (d->games < 0)
                    d->games = d->bad = 0;
                if (P->cat[k].flags & CAT_BAD)
                    d->bad++;
                else
                    d->games++;
            }
        d->listed = d->games > 0;
        LOG(P, "settings: drive %d: id %x, info %x, '%s', %lu of %lu MiB free, features %x, %d game(s), %d bad", i,
            ids[i], (u32)r, d->name, d->free >> 20, d->total >> 20, features, d->games, d->bad);
    }
}

void sel_info_load(struct sel *S, struct sel_info *in)
{
    struct plat *P = PLAT(S);
    memset(in, 0, sizeof(*in));
    str_copy(in->version, GHOSTLDR_VERSION, sizeof(in->version));
    u32 fw = (u32)P->ctx->fw;
    if (fw)
        fmt(in->console, sizeof(in->console), "%s  \xC2\xB7  firmware %x.%02x", P->ps4 ? "PS4" : "PS5", fw >> 8, fw & 0xFF);
    else
        str_copy(in->console, P->ps4 ? "PS4  \xC2\xB7  firmware unknown" : "PS5  \xC2\xB7  firmware unknown", sizeof(in->console));
    in->netinst = info_has_netinst();
    in->log_usb = (int)((P->conf_on >> CONF_LOG_USB) & 1);
#ifdef GHOST_DIAG /* rel-diag's log: where it goes, once the extension has decided */
    const struct ghost_diag *dg = GHOST_DIAG(P->api);
    if (dg && dg->where[0])
        str_copy(in->log_where, dg->where, sizeof(in->log_where));
#endif
    if (!in->log_where[0])
        str_copy(in->log_where, in->log_usb ? "The drive your game starts from, else the first drive that takes a write, "
                                              "in the folder ghostldr/logs" : "Not to a drive", sizeof(in->log_where));
    in->ninst = P->ghostfs ? -1 : cat_installed(S, P, in->inst, SEL_MAX_TITLES);
    in->ps5 = P->library ? (int)P->ps5 : 0;
    for (int k = 0; k < NKEYS; k++) { /* a key as shown: its ends only */
        const char *v = P->art_key[k];
        int n = str_len(v);
        if (!n)
            in->key_shown[k][0] = 0;
        else if (n <= 10)
            fmt(in->key_shown[k], sizeof(in->key_shown[k]), "%d characters", n);
        else
            fmt(in->key_shown[k], sizeof(in->key_shown[k]), "%.4s\xE2\x80\xA6%s", v, v + n - 4);
    }
    in->art_fetched = P->art_fetched;
    for (in->nsw = 0; in->nsw < SEL_MAX_SWITCHES && conf_switch(in->nsw, &in->sw[in->nsw]) == 0; in->nsw++)
        ;
    in->sw_on = P->conf_on, in->sw_safe = P->conf_safe, in->sw_live = P->conf_live;
    bundle(P, in);
    host(S, P, in);
    network(P, in);
    drives(P, in);
}

int sel_netinst(struct sel *S, int op, struct sel_net *st)
{
#ifdef HAVE_NETINST
    struct plat *P = PLAT(S);
    int rc = 0;
    if (op == NETOP_STOP) {
        if (P->net)
            netinst_stop(&P->net);
        P->net = NULL;
        return 0;
    }
    if (op == NETOP_START && !P->net) {
        rc = netinst_start(P->api, &P->net);
        LOG(P, "settings: netinst_start -> %d", rc);
    }
    if (!st)
        return rc;
    if (!P->net)
        return rc ? rc : -1;
    struct netinst_status ns;
    memset(&ns, 0, sizeof(ns));
    ns.size = sizeof(ns);
    if (netinst_status(P->net, &ns) < 0)
        return rc ? rc : -1;
    st->state = ns.state, st->done = ns.done, st->total = ns.total, st->rate_kib = ns.rate_kib;
    st->installed = ns.installed;
    str_copy(st->url, ns.url, (int)sizeof(ns.url) < (int)sizeof(st->url) ? (int)sizeof(ns.url) : (int)sizeof(st->url));
    str_copy(st->token, ns.token, (int)sizeof(ns.token));
    str_copy(st->title, ns.title, (int)sizeof(ns.title));
    str_copy(st->name, ns.name, (int)sizeof(st->name));
    str_copy(st->message, ns.message, (int)sizeof(st->message));
    st->changes = ns.changes;
    return rc;
#else
    (void)S, (void)op, (void)st;
    return -1;
#endif
}

/* the network installer, if it ran: stopped (sel_main, before it returns) */
void info_netinst_stop(struct plat *P)
{
#ifdef HAVE_NETINST
    if (P->net) {
        netinst_stop(&P->net);
        LOG(P, "network installer stopped");
    }
    P->net = NULL;
#else
    (void)P;
#endif
}

int info_has_netinst(void)
{
#ifdef HAVE_NETINST
    return 1;
#else
    return 0;
#endif
}
