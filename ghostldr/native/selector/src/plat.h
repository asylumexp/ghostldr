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
#define CAT_BAD 4u /* a library's game that can't be booted (note: why) */
#define CAT_DUP 8u /* a library's second copy of a game (note: which one --title boots) */
enum { FILE_ICON, FILE_PIC, FILE_SFO, FILE_PIC0, NFILES };
#define NAME_SFO 1      /* where a title's display name came from (docs/selector.md, "Names") */
#define NAME_PROFILE 2
#define NAME_GIVEN 3
#define NAME_STORE 4
#define NAME_CATALOGUE 5
struct cat_title {
    char id[16], prefix[16], index[32], version[16];
    char name[128];      /* the one shown (cat_pick_name): the first of the ones below that is set */
    char sfo_name[128];  /* sce_sys/param.sfo: TITLE_xx in the system language, else TITLE */
    char prof_name[128]; /* the title's profile (run.py's bundle, REC_NAME) */
    char meta_name[128]; /* <prefix>META's, written at install time (--name, the store's ...) */
    char cat_name[128];  /* GFSCATALOG's, written at install time */
    char sfo_id[16];     /* param.sfo's TITLE_ID and CONTENT_ID, once read: game dump or homebrew */
    char content_id[48];
    u8 name_src;         /* NAME_* of name; 0: none (the ID is shown) */
    u64 total, cap;
    u32 containers, flags;
    u32 prof_flags; /* its profile's PF_* (PF_SAVE_TOOL: a save tool asks whose saves) */
    struct {
        u64 off, size;
    } file[NFILES];
    u8 indexed; /* 0 not yet, 1 read, 2 failed */
    u8 meta_state; /* <prefix>META: 0 not read yet, 1 read (meta), 2 none */
    struct meta_info meta;
    /* a library's game (struct lib_title): its app0, its folder's length, a note */
    char path[256];
    u32 dir_len;
    char note[96];
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
    int library;          /* root is a folder of games: lib lists them (docs/usb.md) */
    const struct lib_title *lib;
    int catalogue;        /* GFSCATALOG present; else the legacy index */
    int ps4;              /* ctx flag CTX_FLAG_PS4: game dumps are marked (ui_set_platform) */
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
    u64 prev, next_clock; /* the last step; the next clock reading */
    /* the console's users (a save tool asks whose saves to open) */
    s32 user;             /* the user running SWRR */
    s32 (*us_registered)(s32 *list16);
    s32 (*us_login)(s32 *list4);
    s32 (*us_name)(s32, char *, size_t);
    u64 next_users, scan_frame;
    int pick_rc;          /* ghost_api.title_pick's answer; 1: not asked yet */
    /* system modules this module loaded (0: none or not ours) */
    u32 mod_png, mod_font, mod_fontft;
    /* libScePngDec */
    s32 (*png_parse)(const void *param, void *info);
    s32 (*png_query)(const void *param);
    s32 (*png_create)(const void *param, void *mem, u32 size, void **handle);
    s32 (*png_decode)(void *handle, const void *param, void *info);
    s32 (*png_delete)(void *handle);
    /* frames: what drawing and presenting cost (logged at the end) */
    u64 draw_us, draw_max_us, present_us, present_max_us, present_rows;
    u32 frames, partial_frames;
    int prev_damage_y0, prev_damage_y1;
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
void cat_pick_name(struct cat_title *c);
int cat_load(struct sel *S, struct plat *P, int n);
s64 cat_file(struct sel *S, struct plat *P, int title, int which, void *buf, u64 cap);
s64 cat_meta_read(struct plat *P, int title, u64 off, void *buf, u64 cap);
void cat_meta_names(struct plat *P, int title, const u8 *head, u64 n);

/* png.c */
int png_init(struct plat *P);
int png_decode_to(struct sel *S, struct plat *P, const u8 *data, u64 n, surface *out);

/* sysfont.c */
int sysfont_open(struct sel *S, struct plat *P, u64 mem_bytes);
void sysfont_close(struct plat *P);
