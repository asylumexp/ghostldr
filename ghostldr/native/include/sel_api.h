/*
 * The game selector module and the core (src/sel.c).
 *
 * The selector (ghostldr/native/selector, built to build/ghostldr/selector.bin)
 * is a second runtime module next to the extension.  It runs before the
 * title is loaded and is gone afterwards:
 *
 *  - run.py serves it over TCP at boot like ext.bin (selector socket in the
 *    boot context); the core reads it into the compiler-visible staging
 *    buffer, so receiving it allocates nothing;
 *  - its image has no writable sections (sel.ld, tools/mksel.py); its
 *    relocations (pointers in read-only data: BearSSL's tables, plansync/)
 *    are applied in the staging buffer, then placing it is one copy through
 *    the compiler into the emulator's JIT code buffer, where the title's
 *    modules go afterwards;
 *  - sel_main(&g_ghost_api, title) runs on the boot thread with the same core
 *    services as the extension.  The module allocates its own memory (whole
 *    direct-memory blocks), closes what it opens and frees all of it before
 *    it returns;
 *  - the core then overwrites the image with zeros.
 *
 * The blob records GHOST_ABI: the core refuses a selector built against
 * other headers, as it does an extension.
 */
#pragma once
#include "ghost_api.h"

#define SEL_BLOB_MAGIC 0x4C45535244484C47ULL /* "GHLDRSEL" */
#define SEL_FORMAT 2

/* build/ghostldr/selector.bin: this header, the image (linked at 0), then
 * reloc_count u32 pairs {offset, addend}: the u64 at offset becomes the
 * image's address plus addend (R_X86_64_RELATIVE) */
struct sel_blob {
    u64 magic;
    u32 format;       /* SEL_FORMAT */
    u32 header_size;  /* sizeof(struct sel_blob) */
    u32 api_version;  /* GHOST_API_VERSION it was built against */
    u32 abi;          /* GHOST_ABI it was built against */
    u32 image_size;   /* bytes after the header */
    u32 entry;        /* offset of sel_main in the image */
    u32 font_off, font_size; /* the built-in font inside the image (for tools) */
    u32 reloc_count, reserved;
};

/* ctx flags: CTX_FLAG_SELECT and CTX_FLAG_SEL_FONT_BUILTIN (ghost.h) */

/* Returns the chosen title's index in the catalogue and writes its ID
 * (NUL-padded, as in GFSCATALOG) to title; < 0: nothing chosen, boot the
 * default title. */
typedef int (*sel_main_fn)(const struct ghost_api *api, char title[16]);
