/* The selector module's console side (main.c, cat.c, png.c, sysfont.c). */
#pragma once
#include "sel_api.h"
#include "sel.h"

/* rt.c (the module's own copy) */
int vfmt(char *buf, size_t size, const char *fmt, __builtin_va_list ap);
int fmt(char *buf, size_t size, const char *fmt, ...);

#define LOG(P, ...) (P)->api->logf("selector: " __VA_ARGS__)
#define PLAT(S) ((struct plat *)(S)->plat)

/* One installed title: its GFSCATALOG entry (docs/ghostfs-catalogue.md), and
 * where its art is once its index has been read. */
#define CAT_INCOMPLETE 1u
#define CAT_LEGACY 2u
enum { FILE_ICON, FILE_PIC, FILE_SFO, NFILES };
struct cat_title {
    char id[16], prefix[16], index[32], name[128], version[16];
    u64 total, cap;
    u32 containers, flags;
    struct {
        u64 off, size;
    } file[NFILES];
    u8 indexed; /* 0 not yet, 1 read, 2 failed */
};

struct sysfont;

struct plat {
    const struct ghost_api *api;
    struct boot_ctx *ctx;
    struct kfuncs *k;
    /* memory: two whole direct-memory blocks, freed before sel_main returns */
    u8 *heap;
    u64 heap_size, heap_used, heap_peak, heap_phys;
    u8 *video;
    u64 video_size, video_phys;
    /* the catalogue (cat.c) */
    struct cat_title *cat;
    int ncat;
    int ghostfs;          /* titles in save containers; else one title at root */
    int catalogue;        /* GFSCATALOG present; else the legacy index */
    char legacy[16];      /* GFSINDEX or GHOSTIDX */
    char root[256];
    u32 mounts, umounts;
    /* VideoOut */
    s32 vo;
    int vo_open, fb_shown;
    u32 *fb[2];
    u64 flips;
    s32 (*vo_close)(s32);
    s32 (*vo_submit)(s32, s32, s32, s64);
    s32 (*vo_pending)(s32);
    /* pad */
    s32 pad;
    s32 (*pad_read)(s32, u8 *);
    /* system modules this module loaded (0: none or not ours) */
    u32 mod_png, mod_font, mod_fontft;
    /* libScePngDec */
    s32 (*png_parse)(const void *param, void *info);
    s32 (*png_query)(const void *param);
    s32 (*png_create)(const void *param, void *mem, u32 size, void **handle);
    s32 (*png_decode)(void *handle, const void *param, void *info);
    s32 (*png_delete)(void *handle);
    /* the system font (sysfont.c), when it opened */
    struct sysfont *sf;
    /* time and system settings */
    u64 (*proc_time)(void);
    s32 (*gettimeofday)(void *tv, void *tz);
    s32 (*utc2local)(s64 utc, s64 *local, void *ts, u64 *dst);
    s32 (*param_int)(s32 id, s32 *val);
};

/* main.c */
u32 sys_module(struct plat *P, const char *file, int load, u32 *loaded_handle);
void sys_unload(struct plat *P, u32 *handle, const char *what);
void *sys_sym(struct plat *P, u32 handle, const char *name);

/* cat.c */
int cat_count(struct plat *P);
int cat_load(struct sel *S, struct plat *P, int n);
s64 cat_file(struct sel *S, struct plat *P, int title, int which, void *buf, u64 cap);

/* png.c */
int png_init(struct plat *P);
int png_decode_to(struct sel *S, struct plat *P, const u8 *data, u64 n, surface *out);

/* sysfont.c */
int sysfont_open(struct sel *S, struct plat *P, u64 mem_bytes);
void sysfont_close(struct plat *P);
