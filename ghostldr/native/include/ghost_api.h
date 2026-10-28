/*
 * Interface between the ghostldr core and its runtime extension.
 *
 * The extension (ghostldr/native/ext, built to build/ghostldr/ext.bin) is a
 * second freestanding PIC image with the same compiler flags as the core.
 * run.py serves it over TCP at boot; the core reads it (ext.c), places its
 * code in executable memory, relocates it and calls ext_init(&api).  The
 * extension reaches the core only through struct ghost_api, and hands back
 * its HLE table in struct ext_exports.  The blob also carries the full
 * NID -> name table the core resolves host imports with.
 *
 * Compatibility: the core and the extension are built from one tree.  The
 * blob records GHOST_API_VERSION and GHOST_ABI (the Makefile's checksum of
 * ghost.h and this header); the core refuses an extension built against
 * other headers.  Add new ghost_api members at the end.
 */
#pragma once
#include "ghost.h"

#define GHOST_API_VERSION 1
#ifndef GHOST_ABI
#define GHOST_ABI 0u /* the Makefile passes the headers' checksum */
#endif

struct lib_title;

struct ghost_api {
    u32 version; /* GHOST_API_VERSION */
    u32 size;    /* sizeof(struct ghost_api) */

    /* core state: pointers to the core's globals (ext.h: g_ctx, K, ...) */
    struct boot_ctx **ctx;   /* g_ctx */
    struct kfuncs *k;        /* K, the libkernel functions */
    struct module **modules; /* g_modules, the title's loaded modules */
    struct module **main_mod; /* g_main_mod, the title executable */
    u64 *reported_dmem;      /* g_reported_dmem (budget.c) */

    /* logging (the extension has its own copy of rt.c: fmt, memcpy, ...) */
    void (*logf)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
    void (*log_hex)(const char *tag, const void *p, size_t n);

    /* host (SWRR process) functions */
    void *(*host_real)(const char *name);
    void *(*host_real_in)(const char *modname, const char *name);
    void *(*host_dlsym)(u32 handle, const char *name);
    int (*host_module_handle)(const char *modname, u32 *out);
    int (*host_find_module)(const char *name, u32 *handle_out);
    u64 (*call_gadget6)(u64 fn, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6);

    /* NIDs.  nid_to_name works only during boot (it returns NULL once the
     * title's imports are resolved and the table is freed). */
    const char *(*nid_to_name)(const char *nid);
    void (*compute_nid)(const char *name, char out[12]);

    /* memory */
    void *(*page_alloc)(size_t size);
    void (*page_free)(void *addr, size_t size);
    void *(*direct_alloc)(size_t size, int prot, u64 fixed);
    void (*direct_free)(void *addr);
    void *(*arena_alloc)(size_t size);
    char *(*arena_strdup)(const char *s);
    void (*spin_lock)(volatile int *l);
    void (*spin_unlock)(volatile int *l);

    /* files: /app0 remapping, ghostfs (the title's files in SWRR saves) */
    const char *(*remap_path)(const char *path, char *buf, size_t size);
    int (*vfs_open)(const char *path, int flags, int mode);
    s64 (*vfs_read)(int fd, void *buf, u64 n);
    s64 (*vfs_lseek)(int fd, s64 off, int whence);
    int (*vfs_close)(int fd);
    int (*vfs_stat)(const char *path, void *st);
    bool (*gfs_active)(void);
    bool (*gfs_owns)(int fd);
    int (*gfs_open)(const char *path, int flags, int *fd_out);
    int (*gfs_close)(int fd);
    int (*gfs_stat)(const char *path, void *st, int *rc);
    s64 (*gfs_read)(int fd, void *buf, u64 n);
    s64 (*gfs_pread)(int fd, void *buf, u64 n, s64 off);
    s64 (*gfs_lseek)(int fd, s64 off, int whence);
    int (*gfs_fstat)(int fd, void *st);
    int (*gfs_getdents)(int fd, void *buf, int size);
    int (*gfs_mount)(const char *dirname, int mode, u64 blocks, char mount_point[16]);
    int (*gfs_umount)(const char mount_point[16], u32 max_ms);
    const char *(*gfs_save_name)(void);

    /* the title's modules, threads and TLS */
    u64 (*module_export)(struct module *m, const char *nid);
    struct module *(*ghost_main_module)(void);
    int (*ghost_thread_create)(void *thr, const void *attr, void *(*fn)(void *), void *arg, const char *name);
    void (*tls_ensure_thread)(void);
    void *(*tls_get_addr)(void *ti);

    /* executable memory and the JIT compiler process */
    u64 (*jit_call)(u64 fn, u64 rax, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6);
    int (*jit_copy_to)(u64 dst_in_compiler, const void *src, size_t len);
    int (*arena_write)(u64 dst, const void *src, u64 size);
    u64 (*stub_alloc_code)(const u8 *code, u32 len); /* 32-byte RX slot, written at stubs_flush */

    /* the core's own overrides (an extension function may wrap one) */
    u64 (*hle_lookup)(const char *lib, const char *name);

    /* the title's identity and path namespace */
    const char *(*title_id)(void); /* "CUSA11456" from the ghostfs index, "" when unknown */
    /* remap_path asks fn about title paths starting with prefix; fn returns
     * the host path (in buf) or NULL.  Only during ext_init (a family's init). */
    int (*path_remap_add)(const char *prefix, const char *(*fn)(const char *path, char *buf, size_t size));
    /* save data family (ext/src/hle_savedata.c): real save containers for a
     * given user, real mount slots taken from ghostfs's data containers, the
     * installed title's ID, and path translations ahead of remap_path's own */
    int (*gfs_mount_user)(s32 user, const char *dirname, int mode, u64 blocks, char mount_point[16],
                          u64 *required_blocks);
    int (*gfs_slot_reserve)(void);
    void (*gfs_slot_release)(void);
    const char *(*gfs_title)(void);
    int (*remap_hook_add)(const char *(*fn)(const char *path, char *buf, size_t size));
    /* the title's modules after boot: load a prx (loader.c; *err an SCE
     * kernel error) and start one at most once (modules.c) */
    struct module *(*module_load_runtime)(const char *path, int *err);
    int (*module_start)(struct module *m, u64 argc, const void *argp);
    void (**thread_dtors)(void); /* the title libc's thread-exit hook (tls.c) */
    /* a title thread created through real (a host function with
     * scePthreadCreate's arguments), with its TLS block */
    int (*ghost_thread_start)(void *real, void *thr, const void *attr, void *(*fn)(void *), void *arg,
                              const char *name);
    u32 (*profile_has)(u32 mask); /* the title profile's PF_* flags (profile.c) */
    void (*tls_thread_exit)(void); /* a title thread ends: its TLS blocks go back (tls.c) */
    u64 (*tls_thunk)(u64 fn);      /* fn behind a stub that gives the calling thread its TLS block */
    /* flexible memory as the title gets it (budget.c): mapped from direct
     * memory within the title's flexible budget, and a whole mapping given
     * back (its budget too) */
    int (*flex_map)(void **addr, size_t len, int prot, int flags);
    int (*flex_unmap)(void *addr, size_t len);
    /* app roots other than ghostfs (ext/src/approot.c, docs/usb.md):
     * directory listings through ghostfs, another provider or the kernel
     * (the game selector lists a library's title folders with it); the
     * title's profile; a provider of /app0; saves in SWRR's containers */
    int (*vfs_getdents)(int fd, void *buf, int size);
    const char *(*profile_requested)(void);
    void (*profile_select)(const char *title);
    int (*appfs_register)(const struct appfs_ops *ops);
    void (*gfs_use_saves)(const char *title, const char *prefix);
    /* a descriptor of the JIT compiler process moved into this one
     * (jitrpc.c; < 0 on errors).  The compiler keeps its own copy.  Like
     * jit_call, only from the boot thread: the calls share one channel. */
    int (*jit_recv_fd)(int compiler_fd);
    /* the ELF loader (ext/src/elfldr_sys.c, docs/elfldr.md): Orbis modules
     * parsed and their imports resolved as a title's are, published while
     * they run; the RPC lock around every compiler call once payloads run
     * (their sockets come from the compiler too); payload TLS and threads
     * (tls.c); the emulator's JIT objects (arena.c, bigexec.c); and the
     * crash handler's hook for a payload thread that faults (crash.c) */
    int (*module_parse)(struct module *m, u8 *file, u64 size);
    u64 (*resolve_import)(struct module *m, const char *encoded, bool weak, int kind);
    void (*module_publish)(struct module *m);
    void (*module_unpublish)(struct module *m);
    u64 (*unresolved_stub)(const char *lib, const char *nid, const char *modname, s64 ret, bool denied);
    void (*stubs_flush)(void);
    void (*rpc_lock)(void);
    void (*rpc_unlock)(void);
    void (*tls_static_reserve)(u64 size, u64 align);
    struct module *(*tls_owner)(void);
    int (*payload_thread_start)(struct module *owner, void *thr, const void *attr, void *(*fn)(void *), void *arg,
                                const char *name);
    u32 (*arena_buffers)(u64 *start, u64 *end, u32 max);
    int (*jit_objects_find)(struct jit_obj *o, u32 n, u32 want);
    void (**payload_fault)(struct module *owner, int sig, u64 rip, u64 addr);
    /* the games an app root that is a folder of games holds (a USB drive:
     * ext/src/approot.c, docs/usb.md), as the game selector lists them:
     * their count, 0 for none */
    int (*library)(const struct lib_title **list);
    /* ---- save tools: a profile's "save_tool" feature (PF_SAVE_TOOL, Apollo
     * Save Tool; docs/selector.md "Save tools") ----
     * title_user is the core's g_title_user: the user whose saves the title
     * works on (its user service answers, its save lists and mounts); 0: the
     * user running SWRR.  The game selector sets it before the title loads.
     * ghostldr's own containers and the title's <prefix>SAVE stay the SWRR
     * user's. */
    u32 *title_user;
    /* The save tool's scan, before the title loads, while the game selector
     * owns the screen; the core forwards it to the extension (struct
     * ext_early.save_prescan; < 0 when it has none, or before title_pick).
     * For user: DirNameSearch over every candidate title not fresh in its
     * cache, progress(arg, done, total) once the candidates are known (done
     * 0, may be left out) and after each title searched (nonzero: stop,
     * keeping what was found), the cache written; returns the number of
     * titles with saves, < 0 on errors. */
    int (*save_prescan)(u32 user, int (*progress)(void *arg, u32 done, u32 total), void *arg);
    /* The game selector's choice before it returns, so that save_prescan
     * sees the title's files: the core mounts it as boot.c does after the
     * selector (ghostfs's gfs_pick, a library's approot_pick; nothing when
     * the core had the title already).  title and index as sel_main returns
     * them.  Once: later calls return the first one's result.  0, or < 0
     * (the boot then stops). */
    int (*title_pick)(const char *title, int index);
};

/* A game found under an app root that is a folder of games (a USB drive, or
 * a dev setup's folder), by its contents: a folder with eboot.bin, or one
 * with app0/eboot.bin (ext/src/approot.c).  The game selector shows them in
 * this order, and its choice comes back to approot_pick by index. */
#define LIB_TITLE_BAD 1u /* can't be booted: note says why */
#define LIB_TITLE_DUP 2u /* a second copy of a title: --title boots the first */
struct lib_title {
    char id[16];    /* param.sfo's TITLE_ID, meta.bin's, or the folder's name; "" when unknown */
    char path[256]; /* its app0: the folder with eboot.bin */
    u16 dir_len;    /* path's first dir_len bytes: its folder, with prefix and meta.bin (path's parent for .../app0) */
    u16 flags;      /* LIB_TITLE_* */
    u32 reserved;
    char note[96];  /* why it can't be booted, or about the copy; may be empty */
};

/* A provider of the title's files other than ghostfs: the extension's usbfs
 * (route B, docs/usb.md).  Registered with ghostfs.c (appfs_register), it
 * answers every gfs_* call with ghostfs's contracts: open and stat return 1
 * for paths that are not its own, fds are real placeholder fds, errors are
 * SCE kernel errors. */
struct appfs_ops {
    u32 size;              /* sizeof(struct appfs_ops) */
    const char *name;
    int (*open)(const char *path, int flags, int *fd_out);
    int (*stat)(const char *path, void *st, int *rc);
    bool (*owns)(int fd);
    int (*close)(int fd);
    s64 (*read)(int fd, void *buf, u64 n);
    s64 (*pread)(int fd, void *buf, u64 n, s64 off);
    s64 (*lseek)(int fd, s64 off, int whence);
    int (*fstat)(int fd, void *st);
    int (*getdents)(int fd, void *buf, int size);
    /* the title's app0 on the provider's volume ("/ghostldr/CUSA12345/app0"): 0, or an SCE error */
    int (*set_root)(const char *path);
};

/* The extension's entry before the title's modules load (ext.c
 * ext_approot_init): app roots on USB drives and folders of titles
 * (ext/src/approot.c).  ext_init still runs at its usual place. */
#define EXT_EARLY_MAGIC 0x594c5245u /* "ERLY" */
struct ext_early {
    u32 magic, size;
    int (*approot_init)(const char **root);
    /* the game the selector chose: its index in library()'s list and its ID
     * (index < 0 or title NULL: none chosen) */
    int (*approot_pick)(const char *title, int index, const char **root);
    int (*library)(const struct lib_title **list);
    /* A save tool's scan (ghost_api.save_prescan, same contract), once the
     * title's root is mounted and before ext_init: only what ghost_api and
     * the host's libraries give.  The core places the code for it when no
     * app root did (ext.c).  NULL: none. */
    int (*save_prescan)(u32 user, int (*progress)(void *arg, u32 done, u32 total), void *arg);
};
typedef const struct ext_early *(*ext_early_fn)(const struct ghost_api *api);

/* ---- what the extension registers and returns ---- */

/* One HLE function.  It matches an import by NID: the NID of name, or nid
 * when given (functions whose name is unknown), and by library: lib or any
 * alias a struct ext_lib declares for it.  name is also used in logs. */
struct ext_hle_ent {
    const char *lib;  /* import library, e.g. "libSceAudioOut" */
    const char *name; /* function name, e.g. "sceAudioOutOpen"; may be NULL with nid */
    void *fn;
    const char *nid;  /* optional 11-character NID; NULL: computed from name */
};

/* Per-library policy.  Aliases are the library's other import names
 * ("libSceNpManagerCompat libSceNpManagerForToolkit"): entries registered
 * under any of the names match imports from all of them.  With
 * EXT_LIB_NO_HOST an import the extension does not implement never falls
 * back to SWRR's own library (its trophies, saves, NP state): it gets a stub
 * that returns stub_ret.  stub_ret is also what the library's unresolved
 * stubs return instead of 0. */
#define EXT_LIB_NO_HOST 1u
/* The extension is the whole module: sceKernelLoadStartModule of the
 * library's .sprx gets a handle of the module family's (hle_module.c) and
 * sceKernelDlsym on it finds the extension's functions, with no host module
 * loaded (a library a PS4 app has on one console and not on another: Piglet) */
#define EXT_LIB_VIRTUAL 2u
struct ext_lib {
    const char *name;    /* import library */
    const char *aliases; /* other import library names, space separated, or NULL */
    u32 flags;           /* EXT_LIB_* */
    s32 stub_ret;        /* e.g. 0x80551622 (SCE_NP_TROPHY_ERROR_NOT_SUPPORTED) */
};

/* One per ext/src file (EXT_FAMILY / EXT_FAMILY_LIBS in ext.h). */
struct ext_family {
    const char *name;
    const struct ext_hle_ent *hle;
    u32 nhle, nlibs;
    const struct ext_lib *libs;
    int (*init)(void); /* optional; nonzero: the family's functions and policies are not used */
    /* optional (EXT_FAMILY_HOOKS): a sysmodule load the title asked for failed */
    void (*sysmodule_failed)(u32 id, s32 rc);
};

#define EXT_EXPORTS_MAGIC 0x32505845u /* "EXP2" */
struct ext_exports {
    u32 magic, size;
    /* The function for an import of nid from lib (NULL: any library), or 0. */
    u64 (*lookup)(const char *lib, const char *nid);
    /* The policy covering lib (by name or alias), or NULL. */
    const struct ext_lib *(*lib_policy)(const char *lib);
    /* everything registered, for logs and tools: nid filled in */
    const struct ext_hle_ent *hle;
    u32 nhle, nfamilies, nlibs;
    /* Tells the families that a sysmodule load the title asked for failed
     * (hle.c; the core checks size before calling it). */
    void (*sysmodule_failed)(u32 id, s32 rc);
    /* The title's main thread is about to be created: the boot can no longer
     * stop short (the FTP server starts here: ftp_sys.c).  The core checks
     * size before calling it. */
    void (*title_starting)(void);
    /* No title (ctx CTX_FLAG_ELFLDR): the ELF loader serves payloads on the
     * boot thread until a client tells it to quit (ext/src/elfldr.c).  The
     * core checks size before calling it. */
    int (*elfldr_main)(void);
    /* A module's text, about to be written to executable memory (loader.c):
     * the extension may patch it in place (raw system call sites, hle_rawsys.c).
     * The core checks size before calling it. */
    void (*patch_text)(struct module *m, u8 *text, u64 size);
    /* The title registered display buffers (sceVideoOutRegisterBuffers,
     * hle_sys.c; attr is its SceVideoOutBufferAttribute), and a pad state it
     * read (scePadReadState, scePadRead) before it sees it: the extension
     * draws dialogs over the first and takes the controller from the second
     * while one is open (dlg_overlay.c).  The core checks size before calling
     * them; NULL in an extension without drawn dialogs. */
    void (*video_buffers)(s32 handle, s32 start, const u64 *addrs, s32 num, const void *attr);
    void (*pad_state)(s32 handle, u8 *data);
};

typedef const struct ext_exports *(*ext_init_fn)(const struct ghost_api *api);

/* The core calls this after ext_init: it checks the extension's relocated
 * data, its bss and a call back into the core, and the lookups by name, by
 * NID only and through an alias.  fn(seed) returns
 * EXT_SELFTEST_OK ^ seed ^ (call number << 32). */
#define EXT_SELFTEST_LIB "libGhostldrExt"
#define EXT_SELFTEST_ALIAS "libGhostldrExtAlias"
#define EXT_SELFTEST_NAME "ghostldrExtSelfTest"
#define EXT_SELFTEST_NID "KGH3R7sRA84" /* "ghostldrExtSelfTestByNid", a name in no list */
#define EXT_SELFTEST_STUB_RET ((s32)0x8002002D) /* the library's stub value (EOPNOTSUPP) */
#define EXT_SELFTEST_OK 0x5e1f7e57ULL

/* ---- build/ghostldr/ext.bin (tools/mkext.py) ----
 * [struct ext_blob][image][relocations][NID keys][name offsets][names]
 * Offsets are from the start of the blob. */
#define EXT_BLOB_MAGIC 0x5458455244484c47ULL /* "GHLDREXT" */
#define EXT_BLOB_FORMAT 1

struct ext_blob {
    u64 magic;
    u32 format;      /* EXT_BLOB_FORMAT */
    u32 header_size; /* sizeof(struct ext_blob) */
    u32 api_version; /* GHOST_API_VERSION it was built against */
    u32 abi;         /* GHOST_ABI it was built against */
    u64 blob_size;
    /* code: a flat image, [text | data], linked at 0 (ext/ext.ld) */
    u32 image_off;
    u32 image_filesz; /* bytes stored; the rest of image_memsz is zero */
    u32 image_memsz;  /* with bss, 16 KiB aligned */
    u32 text_size;    /* executable part, 16 KiB aligned; RW data follows */
    u32 entry;        /* ext_init */
    u32 reloc_off, reloc_count;       /* struct ext_reloc[] */
    u32 families_off, families_count; /* image offset of the family pointers (for tools) */
    /* NID -> name, every known name: keys ascending */
    u32 nid_count;
    u32 nid_keys_off;  /* u64[nid_count]: a NID's 64-bit value (its 11 base64 digits) */
    u32 nid_names_off; /* u32[nid_count]: offset of the name in the string pool */
    u32 str_off, str_size;
    u32 early_entry; /* ext_early (struct ext_early), valid with EXT_BLOB_EARLY */
    u32 flags;       /* EXT_BLOB_* */
};
#define EXT_BLOB_EARLY 1u

/* R_X86_64_RELATIVE: *(u64 *)(base + offset) = base + addend */
struct ext_reloc {
    u32 offset, addend;
};
