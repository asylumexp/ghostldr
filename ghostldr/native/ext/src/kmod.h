/* Shared by the kernel families: hle_kernel.c, hle_module.c, hle_sysmodule.c
 * and hle_file.c (research/hle-specs/kernel-modules.md). */
#pragma once
#include "ext.h"

/* SCE kernel errors: 0x80020000 + errno */
#define E_PERM ((s32)0x80020001)
#define E_NOENT ((s32)0x80020002)
#define E_SRCH ((s32)0x80020003)
#define E_BADF ((s32)0x80020009)
#define E_NOMEM ((s32)0x8002000C)
#define E_ACCES ((s32)0x8002000D)
#define E_FAULT ((s32)0x8002000E)
#define E_INVAL ((s32)0x80020016)
#define E_ROFS ((s32)0x8002001E)

/* the title's loaded modules (hle_module.c) */
struct module *mod_by_handle(s32 handle) HIDDEN;
struct module *mod_by_name(const char *file) HIDDEN;
struct module *mod_by_addr(u64 addr) HIDDEN;
/* "/app0/x.prx", or a bare "x.prx" found in /app0 or /app0/sce_module: the
 * path the loader reads it from (ghostfs or the app root), if it exists */
bool title_vpath(const char *path, char *out, size_t size) HIDDEN;
/* an IsGame module file ("libSceNpToolkit2.prx"): libSceSysmodule manages it (hle_sysmodule.c) */
bool sysmod_is_game(const char *file) HIDDEN;
