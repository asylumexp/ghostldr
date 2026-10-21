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
};

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
    u32 reserved[2];
};

/* R_X86_64_RELATIVE: *(u64 *)(base + offset) = base + addend */
struct ext_reloc {
    u32 offset, addend;
};
