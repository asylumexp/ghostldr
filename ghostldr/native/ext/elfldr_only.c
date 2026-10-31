/*
 * The ELF loader's own extension, build/ghostldr/ext-elfldr.bin (in the
 * release's ghostldr-elfldr.bin; docs/elfldr.md): the families a payload
 * binds to (ELFLDR_EXT_SRCS in the Makefile) and none of a game's, no FTP
 * server, no app roots and no NID table.  It runs no title, so nothing here
 * is reached: ext_exports.title_starting (the FTP server, ftp_sys.c) and
 * ext_early (app roots on USB drives) are a title's.  These stand in for
 * what ftp.c, approot.c and hle_sysmodule.c would link.
 */
#include "kmod.h"
#include "ftp.h"

int ftp_start(unsigned short port)
{
    logf("ftp: this extension has no FTP server (port %u)", port);
    return -1;
}

HIDDEN void usb_ftp_prepare(void) {}
HIDDEN const char *usb_ftp_home(void) { return "/"; }
int ftp_stop(unsigned max_ms) { return 0; }
int ftp_running(void) { return 0; }

/* ext_main.c's ext_early: no app root here (ext.c refuses a zero magic) */
const struct ext_early g_approot_early HIDDEN = {0, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

/* hle_module.c: whether a title module is a sysmodule game module; a
 * payload never is */
bool sysmod_is_game(const char *file) { return false; }

/* hle_appcontent.c's title identity: a payload runs with no title */
const char *title_ident(void) { return ""; }

/* hle_net.c's pool of sockets for a title: a payload has its own (elfldr_sys.c) */
HIDDEN void net_pool_fill(void) {}
