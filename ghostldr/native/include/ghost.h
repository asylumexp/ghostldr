/*
 * ghostldr: in-process PS4 title loader that runs inside SWRR (US CUSA03474 or EU CUSA03492)
 * after Luac0re 2.x gives Lua + JIT-compiler code execution.
 *
 * The core is a freestanding, position-independent blob.  Lua copies it into
 * executable JIT memory, fills a struct boot_ctx, and calls ghost_entry(ctx).
 */
#pragma once

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
typedef signed char s8;
typedef short s16;
typedef int s32;
typedef long s64;
typedef unsigned long size_t;
typedef long ssize_t;
typedef long off_t;
typedef unsigned long uintptr_t;
typedef _Bool bool;
#define true 1
#define false 0
#define NULL ((void *)0)

#define HIDDEN __attribute__((visibility("hidden")))
#define NOINLINE __attribute__((noinline))
/* for boot-time code where small beats fast: minsize is clang-only, so
 * other compilers keep just the cold half */
#if defined(__clang__)
#define GHOST_COLD __attribute__((minsize, cold))
#else
#define GHOST_COLD __attribute__((cold))
#endif
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((u64)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((u64)(a) - 1))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define BOOT_MAGIC 0x3130305244484c47ULL /* "GHLDR001" */

/* Filled by Lua (ghostldr/lua/ghostldr.lua).  Every field is a u64 so the Lua
 * side can write it with write64(ctx + 8 * index, value). */
struct boot_ctx {
    u64 magic;
    u64 size;              /* sizeof(struct boot_ctx) */
    u64 eboot_base;        /* host game's eboot base (SWRR, or Jak X: CTX_FLAG_JAKX) */
    u64 dlsym;             /* sceKernelDlsym in main process */
    u64 call_gadget;       /* eboot "call rbx; ret" */
    u64 libkernel_handle;
    /* PS4: the compiler's libkernel syscall stubs, (number, address) u64
     * pairs ended by a zero number (ghostldr.lua; jit_call); 0 on a PS5 */
    u64 jit_sysw;
    u64 log_fd;            /* UDP socket from the loader's create_socket() */
    u64 log_addr;          /* sockaddr_in (16 bytes) of the log server */
    /* compiler-process RPC (jitrpc.c): the compiler's base, then the chain
     * parts of the loader that runs us */
    u64 jit_base;
    union {
        struct { /* Luac0re (SWRR): its jit_rop gadgets and the registers its chain restores */
            u64 rop_ret, rop_mov_rax_rdi, rop_pop_rdi, rop_pop_rsi, rop_mov_rdx_rdi;
            u64 rop_pop_rcx, rop_pop_rbx, rop_mov_deref_rbx_rax, rop_pop_rsp;
            u64 rop_pop_r12_r15_rbp, rop_mov_r8_rbx, rop_mov_r9_rbx;
            u64 leak_rbx, leak_rbp, leak_r12, leak_r13, leak_r14, leak_r15;
            u64 fake_rsp;
        };
        struct { /* UltraC0re (Jak X): door1.lua's parked compiler loop.  Gadgets
                  * for rdi/rsi/rdx/rcx only: four arguments at most */
            u64 uc_pop_rax, uc_pop_rdi_rbp, uc_pop_rsi_rbp, uc_pop_rcx, uc_mov_rdx_rsi;
            u64 uc_store_rdi;      /* mov [rdi], rax */
            u64 uc_pop_rsp_deref;  /* the loop's "pop rsp" (touches [rax]) */
            u64 uc_spin, uc_call, uc_res, uc_cnt; /* J_SPIN, J_CALL, J_RES, J_CNT */
        };
    };
    u64 oob_scratch, chain_start, chain_end; /* UltraC0re: oob_scratch only */
    u64 jit_memcpy, jit_calloc, jit_syscall, jit_errno_fn;
    u64 jit_shm_create, jit_shm_alias, jit_shm_map;
    u64 new_jit_sock, new_main_sock;
    u64 staging, staging_size; /* shared bridge memory both processes see */
    u64 app_root;          /* C string: where the title's app0 lives */
    u64 flags;
    /* reserved[0] = compiler sceKernelVirtualQuery, [1] = BRIDGE_BASE,
     * [2] = IPU_PROG_BASE (start of the emulator's JIT program buffers),
     * [3] = first free byte of the IPU buffer after the core image */
    u64 reserved[8];
    /* runtime extension (ext.c): a TCP socket connected to run.py's extension
     * server (-1: none), and the blob's size and CRC-32 */
    u64 ext_fd, ext_size, ext_crc;
    /* game selector (sel.c): the same for its module */
    u64 sel_fd, sel_size, sel_crc;
    /* the console's firmware from Luac0re's FW_VERSION, as 0xMMmm (12.02 ->
     * 0x1202); 0: unknown.  The platform is ctx flag CTX_FLAG_PS4. */
    u64 fw;
    /* the first 24 bytes of the extension blob's SHA-256, pinned by
     * run.py through ghostldr.lua (GHOSTLDR_EXT_SHA256): ext.c verifies
     * the served blob against it after the CRC-32 screen, when the boot
     * context is large enough to carry it and any byte is nonzero.
     * 192 bits: a collision or a second preimage is not the threat;
     * a wrong or swapped blob is.  The bytes end exactly at the
     * profile slot (PROFILE_SLOT * 8 == 504). */
    u8 ext_sha256[24];
};

/* ---- runtime (rt.c) ---- */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
char *strstr(const char *h, const char *n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
size_t strlcpy(char *d, const char *s, size_t n);
size_t strlcat(char *d, const char *s, size_t n);
const char *strrchr_c(const char *s, int c);
int vfmt(char *buf, size_t size, const char *fmt, __builtin_va_list ap);
int fmt(char *buf, size_t size, const char *fmt, ...);

/* ---- logging (log.c) ---- */
void log_init(struct boot_ctx *ctx);
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_hex(const char *tag, const void *p, size_t n);

/* ---- host functions (host.c, hle.c, hle_sys.c) ---- */
u64 call_gadget6(u64 fn, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6);
int host_init(struct boot_ctx *ctx);
void *host_dlsym(u32 handle, const char *name);
int host_find_module(const char *name, u32 *handle_out);
void *host_real(const char *name);                         /* libkernel / libSceSysmodule, cached */
void *host_real_in(const char *modname, const char *name); /* any host module, loaded on demand */

struct sce_module_info {
    u64 size;
    char name[256];
    struct { u64 base; u32 size; u32 prot; } segs[4];
    u32 nsegs;
    u8 fingerprint[20];
};

struct kfuncs {
    int (*usleep)(u32);
    int (*getpid)(void);
    int (*pthread_create)(void *thr, const void *attr, void *(*fn)(void *), void *arg, const char *name);
    int (*pthread_attr_init)(void *attr);
    int (*pthread_attr_setstacksize)(void *attr, size_t size);
    int (*pthread_attr_destroy)(void *attr);
    int (*open)(const char *path, int flags, int mode);
    ssize_t (*read)(int fd, void *buf, size_t n);
    ssize_t (*pread)(int fd, void *buf, size_t n, off_t off);
    ssize_t (*write)(int fd, const void *buf, size_t n);
    int (*close)(int fd);
    off_t (*lseek)(int fd, off_t off, int whence);
    int (*fstat)(int fd, void *st);
    int (*stat)(const char *path, void *st);
    int (*mmap)(void *addr, size_t len, int prot, int flags, int fd, off_t off, void **out);
    int (*munmap)(void *addr, size_t len);
    int (*mprotect)(const void *addr, size_t len, int prot);
    int (*reserve_va)(void **addr, size_t len, int flags, size_t align);
    int (*map_flex)(void **addr, size_t len, int prot, int flags);
    int (*alloc_direct)(off_t start, off_t end, size_t len, size_t align, int type, off_t *out);
    int (*map_direct)(void **addr, size_t len, int prot, int flags, off_t phys, size_t align);
    size_t (*direct_size)(void);
    int (*avail_direct)(off_t start, off_t end, size_t align, off_t *start_out, size_t *size_out);
    int (*avail_flex)(size_t *out);
    int (*module_list)(u32 *handles, size_t max, size_t *count);
    int (*module_info)(u32 handle, struct sce_module_info *info);
    int (*module_info_from_addr)(u64 addr, int flags, void *info);
    int (*load_start_module)(const char *path, size_t argc, const void *argv, u32 flags, void *opt, int *res);
    int (*virtual_query)(const void *addr, int flags, void *info, size_t size);
    ssize_t (*sendto)(int fd, const void *buf, size_t n, int flags, const void *addr, u32 alen);
    ssize_t (*recvmsg)(int fd, void *msg, int flags);
    int (*jit_map)(int fd, int prot, void **addr);
    int *(*errno_fn)(void);
    void *(*get_proc_param)(void);
    int (*debug_out)(int ch, const char *text);
};

extern struct kfuncs K;
extern struct boot_ctx *g_ctx;

/* ---- compiler-process RPC (jitrpc.c) ---- */
u64 jit_call(u64 fn, u64 rax, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6);
u64 jit_sys_stub(u64 num); /* the compiler's stub for system call num: jit_syscall on a PS5, 0 when none */
u64 jit_read64(u64 addr);
int jit_copy_to(u64 dst_in_compiler, const void *src, size_t len);
int jit_recv_fd(int compiler_fd);

/* A block of JIT shared memory: written through the compiler's RW alias and
 * executed through a main-process RX mapping. */
struct jit_block {
    u64 size;
    u64 rw;      /* address of RW mapping inside the compiler process */
    u64 rx;      /* address of RX mapping inside this process */
    int base_fd, rw_fd, rx_fd_compiler, rx_fd;
};
int jit_block_create(struct jit_block *b, u64 size, u64 want_rx_addr);
int jit_block_write(struct jit_block *b, u64 offset, const void *src, size_t len);
void jit_block_destroy(struct jit_block *b); /* unmap and close both sides (needs the compiler's syscall gadget) */

/* One sceKernelVirtualQuery answer (SceKernelVirtualQueryInfo, 0x48 bytes). */
struct vmq {
    u64 start, end, offset;
    s32 prot, type;
    u8 flags;              /* 1 flexible, 2 direct, 4 stack, 8 pooled, 16 committed */
    char name[32];
} __attribute__((packed));
/* The compiler process's mappings that meet [lo, hi), ascending (at most max).
 * It lists none of the JIT mappings: only for the diagnostic's log. */
u32 jit_vmap(u64 lo, u64 hi, struct vmq *out, u32 max);
int jit_peek(u64 addr, void *out, u32 n); /* compiler reads at addr, here: 0, or -1 (EFAULT, no crash) */
int jit_stamp(u64 addr);                  /* compiler writes 16 bytes (the time) at addr: 0, or -1 (EFAULT) */
int jit_ps4_check(void); /* PS4: the stubs, clock_gettime, the socket pair, EFAULT on known memory: 0 when all work */

/* mmap constants (FreeBSD / Orbis) */
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 0x1
#define MAP_PRIVATE 0x2
#define MAP_FIXED 0x10
#define MAP_ANON 0x1000
#define MAP_NO_OVERWRITE 0x80
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0x200
#define O_TRUNC 0x400

/* ---- memory (mem.c) ---- */
void *page_alloc(size_t size);
void *direct_alloc(size_t size, int prot, u64 fixed);
void direct_free(void *addr);
void page_free(void *addr, size_t size);
void *arena_alloc(size_t size);
char *arena_strdup(const char *s);
void spin_lock(volatile int *l);
void spin_unlock(volatile int *l);

/* ---- Orbis modules (elf.c, loader.c) ---- */
struct elf_phdr {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
};
struct elf_sym {
    u32 name;
    u8 info, other;
    u16 shndx;
    u64 value, size;
};
struct elf_rela {
    u64 offset, info;
    s64 addend;
};

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_TLS 7
#define PT_SCE_DYNLIBDATA 0x61000000
#define PT_SCE_PROCPARAM 0x61000001
#define PT_SCE_MODULE_PARAM 0x61000002
#define PT_SCE_RELRO 0x61000010
#define PT_GNU_EH_FRAME 0x6474E550

#define MAX_LIBS 64
struct libref {
    u16 id;
    const char *name;
};

struct module {
    const char *name;          /* file name, e.g. "libc.prx" */
    const char *modname;       /* SCE module name from DT_SCE_MODULE_INFO */
    bool is_main;
    u32 fake_handle;           /* handle we hand out for dlsym on our modules */
    /* file */
    u8 *file;
    u64 file_size;
    struct elf_phdr *phdrs;
    u32 phnum;
    u64 entry;
    /* per-segment file data located through the SELF entry table */
    const u8 *seg_data[32];
    /* dynamic */
    const u8 *dynlib;          /* PT_SCE_DYNLIBDATA contents */
    const char *strtab;
    u64 strsz;
    const struct elf_sym *symtab;
    u64 nsyms;
    const struct elf_rela *rela, *jmprel;
    u64 nrela, njmprel;
    u64 init, fini;
    struct libref import_libs[MAX_LIBS], export_libs[MAX_LIBS], needed_mods[MAX_LIBS];
    u32 n_import_libs, n_export_libs, n_needed_mods;
    const char *needed_files[MAX_LIBS]; /* DT_NEEDED, e.g. "libSceAppContent.prx" */
    u32 n_needed_files;
    /* runtime */
    u64 base, image_size;
    u64 text_size;             /* bytes at base held in the RX arena */
    u64 relro_lo, relro_size;  /* RELRO range (written through the compiler) */
    u8 *relro_shadow;          /* local copy relocated before upload */
    u64 proc_param;            /* runtime address or 0 */
    /* TLS */
    u64 tls_vaddr, tls_filesz, tls_memsz, tls_align;
    u32 tls_id;                /* dynamic TLS module id (ours) */
    /* exports sorted by NID for lookup */
    struct export_ent *exports;
    u32 nexports;
    struct module *next;
    /* start (modules.c): DT_INIT when the module has one, else e_entry */
    bool has_init;
    u8 start_state;            /* MOD_* */
    s32 start_rc;              /* what its init entry returned */
    /* what the kernel's module queries report (module_parse; the extension
     * answers sceKernelGetModuleInfo* for title modules from these) */
    u64 eh_frame_hdr, eh_frame_hdr_size;  /* PT_GNU_EH_FRAME (vaddr, memsz), 0 if none */
    struct {
        u64 vaddr;
        u32 size;
        s32 prot;                         /* ELF p_flags */
    } segs[4];                            /* PT_LOAD and PT_SCE_RELRO in header order */
    u32 nsegs;
    u8 fingerprint[20];                   /* DT_SCE_FINGERPRINT */
    u64 tls_offset;            /* static TLS: image at thread pointer - tls_offset (tls.c); 0: dynamic */
    struct bx_state *bx;       /* code in whole emulator JIT objects (bigexec.c); NULL: in the arena */
    /* a payload of the ELF loader (ext/src/elfldr_sys.c, docs/elfldr.md):
     * its threads' TLS blocks hold its own image only, and it is finished
     * once none of them runs (tls.c) */
    bool payload;
    volatile s32 live_threads;
};

/* struct module.start_state */
#define MOD_BOOT 0     /* started by the title's main thread before the entry */
#define MOD_DEFERRED 1 /* an IsGame module: started by its sceSysmoduleLoadModule (extension) */
#define MOD_STARTING 2
#define MOD_STARTED 3

struct export_ent {
    char nid[12];
    const char *lib;
    u64 addr;
};

extern struct module *g_modules;
extern struct module *g_main_mod;
extern u64 g_reported_dmem;
int budget_patch_text(struct module *m, u8 *text); /* -1: the title cannot fit, stop the boot */
void hooks_patch_text(struct module *m, u8 *text);
bool vdm_is(u64 off);
u32 ghost_thread_list(u64 *tids, const char **names, u32 max);
struct dm_usage {
    u64 phys, len, mapped, from;
    int type;
};
extern struct dm_usage g_dm_usage[32];
extern u32 g_dm_nusage;
void crash_start_watchdog(void);
int vdm_alloc(u64 start, u64 end, u64 len, u64 align, int type, off_t *out);
int vdm_map(void **addr, u64 len, int prot, int flags, u64 off, u64 align);
int vdm_release(u64 off, u64 len);
int vdm_expand_entry(const void *entry, void *out, int cap);
int vdm_alloc_lazy(u64 len, int type, bool zero, off_t *out); /* zero: each chunk zeroed when committed */
u64 vdm_lazy_bytes(u64 off);                                 /* committed behind a lazy allocation */
bool vdm_lazy(u64 off);                                      /* off is a lazy allocation's */
int vdm_query(u64 off, bool next, u64 *start, u64 *end, int *type); /* 0, or -1: none */
u32 vdm_lazy_committed_mib(void);

/* ---- ghostfs.c: the title's /app0 in SWRR save containers ---- */
int gfs_init(void);
bool gfs_active(void);
bool gfs_owns(int fd);
int gfs_open(const char *path, int flags, int *fd_out);   /* 1: not ghostfs's */
int gfs_close(int fd);                                     /* 1: not ghostfs's */
int gfs_stat(const char *path, void *st, int *rc);         /* 1: not ghostfs's */
s64 gfs_read(int fd, void *buf, u64 n);
s64 gfs_pread(int fd, void *buf, u64 n, s64 off);
s64 gfs_lseek(int fd, s64 off, int whence);
int gfs_fstat(int fd, void *st);
int gfs_getdents(int fd, void *buf, int size);
int gfs_find_file(const char *name, char *dir, size_t size); /* a title file outside sce_module */
int gfs_mount(const char *dirname, int mode, u64 blocks, char mount_point[16]);
int gfs_umount(const char mount_point[16], u32 max_ms);
const char *gfs_save_name(void);                        /* <prefix>SAVE */
const char *title_id(void);                             /* the index's title ID, "" if unknown */
/* the save data family (ext/src/hle_savedata.c) */
int gfs_mount_user(s32 user, const char *dirname, int mode, u64 blocks, char mount_point[16], u64 *required_blocks);
int gfs_slot_reserve(void);                             /* 0, or -1: no real mount slot to spare */
void gfs_slot_release(void);
const char *gfs_title(void);                            /* the index's title ID, or NULL */
int vfs_open(const char *path, int flags, int mode);
s64 vfs_read(int fd, void *buf, u64 n);
s64 vfs_lseek(int fd, s64 off, int whence);
int vfs_close(int fd);
int vfs_stat(const char *path, void *st);
int vfs_getdents(int fd, void *buf, int size);
void vdm_log_lazy(void);
bool budget_lazy_site(u64 eboot_off);
bool budget_planned(void); /* a profile's manual memory plan applies (not run.py --auto-memory) */
/* automatic memory plans for titles without a manual one (automem.c) */
bool automem_on(void);
bool automem_lazy(u64 len);
u64 automem_plan(struct module *m, u8 *text, u64 phys); /* plan time: what the title is told */
void automem_note_alloc(u64 site, u64 len, int type, bool lazy, u64 off);
void automem_note_release(u64 off, u64 len);
bool automem_admit(u64 total, u64 len, u64 told); /* reserve past what the title was told, lazily */
#define AM_OOM_TOLD 1 /* an allocation refused past what the title was told */
#define AM_OOM_REAL 2 /* a lazy commit found no direct memory */
void automem_note_out_of_memory(int kind, u64 short_bytes);
void automem_start(void); /* the title starts: peaks and the learned plan saved as it runs */
u64 budget_allocatable(int type); /* PS4: direct memory of a type the kernel hands out now (taken and released) */
int hle_map_flex(void **addr, size_t len, int prot, int flags); /* flexible memory, backed by direct (hle.c) */
int budget_flex_unmap(void *addr, size_t len); /* a whole flex-as-direct block back, budget included */

/* ---- ghostfs catalogue: the installed titles (docs/ghostfs-catalogue.md) ---- */
struct gfs_cat_entry {
    char title[16], prefix[16], index[32], name[64], version[16];
    u64 layout_id, total;
    u32 containers, flags; /* GFS_CAT_* */
    u64 updated;
    u8 reserved[16];
};
#define GFS_CAT_INCOMPLETE 1
#define GFS_CAT_LEGACY 2
u32 gfs_titles(void);                             /* read the catalogue: number of entries (0: none) */
const struct gfs_cat_entry *gfs_title_at(u32 i);
int gfs_mount_title(const char *title);           /* its index: its containers become /app0 */
int gfs_pick(const char *title); /* mount title; NULL: the requested one, else the only complete one */
/* another provider of /app0 (the extension's usbfs: struct appfs_ops in ghost_api.h) */
struct appfs_ops;
int appfs_register(const struct appfs_ops *ops);
bool gfs_app0_virtual(void);                      /* /app0 is ghostfs's or a provider's, not a directory */
void gfs_use_saves(const char *title, const char *prefix); /* a title from a USB drive: progress in <prefix>SAVE */

/* ---- title profiles (profile.c; host/profiles.py writes them) ---- */
struct prof_hdr {
    char magic[4];             /* "GLPF" */
    u16 version, header_size;  /* 1, 96 */
    u32 total;                 /* header and records, a multiple of 8 */
    u32 flags;                 /* PF_* */
    char title[16];            /* "": the generic default */
    u8 fingerprint[20];        /* eboot DT_SCE_FINGERPRINT the profile is for; zero: any */
    u32 save_blocks;           /* <prefix>SAVE size when created (32 KiB blocks) */
    s32 app_param[5];          /* sceAppContentAppParamGetInt ids 0-4 */
    u32 runtime_reserve_mib;   /* direct memory kept for ghostldr (plan: the slack) */
    s32 video_reserve_mib;     /* for the video decoder; -1: 100 if the title imports one */
    u32 flex_mib;              /* flexible memory budget; 0: the title's own (plan-free) or none (plan) */
    u32 min_mib;               /* plan-free: refuse to boot below this direct size */
    u32 reserved;
};
#define PF_REQUESTED 1   /* boot this title (run.py --title) */
#define PF_PLAN 2        /* memory plan records (needs the fingerprint) */
#define PF_HOOKS 4       /* hooks.c diagnostics (needs the fingerprint) */
#define PF_DIAG 8        /* watchdog reads of the title's globals (needs the fingerprint) */
#define PF_TRACE_OPENS 16 /* log every file the title opens (capture_order.py) */
#define PF_OWN 32        /* the title has its own profile */
struct prof_rec {
    u16 kind, size; /* size includes this header, a multiple of 8 */
    u32 reserved;
};
#define REC_PLAN 1
#define REC_TABLE 2
#define REC_RULE 3
#define REC_TLS 4
#define REC_NAME 5      /* the title's display name, UTF-8 + NUL (the game selector reads it; the core ignores it) */
#define REC_EXEC 6      /* a module's code ranges (bigexec.c) */
#define REC_NEO 7       /* the plan for a Pro-mode host (Jak X): the title's own Pro memory mode */
struct prof_tls {       /* tools/tls_sites.py: one module's thread-pointer loads, checked before patching */
    struct prof_rec r;
    char module[32];    /* its file name, e.g. "eboot.bin" */
    u32 count, off[];   /* text offsets of each "mov/add r64, fs:[0]"'s fs prefix */
};
struct prof_exec {      /* a module's code ranges, instead of the evidence bxplan.c reads */
    struct prof_rec r;
    char module[32];    /* its file name, e.g. "eboot.bin" */
    u32 count, range[]; /* start, end text offsets of each range */
};
struct prof_plan {
    struct prof_rec r;
    u32 title_fixed_mib, overcommit_mib, nsites, sites[]; /* lazily backed allocation sites (return addresses) */
};
struct prof_neo {       /* with a plan: when the host runs in Pro mode (sceKernelIsNeoMode 1) the
                         * title is told so, and the plan applies to this memory mode instead of 0 */
    struct prof_rec r;
    u32 mode, title_fixed_mib, overcommit_mib; /* the plan's overcommit when 0 */
    u32 knob_mib[8];                           /* set tables' sizes in that mode, by knob; 0: the table's own */
};
struct prof_table {     /* a u64 table in the executable's text, indexed by memory mode */
    struct prof_rec r;
    u32 offset;
    u8 role, knob;      /* role 0 set, 1 absorb; knob: boot_ctx reserved[] slot, 0xff none */
    u16 nexpect;
    u32 mib, min_mib;
    u64 expect[];       /* its contents, checked before patching; then the name */
};
struct prof_fill {
    u8 arg, size;       /* argument 0-5; size 1/2/4/8, 0: clear `value` bytes */
    u16 offset;
    u32 reserved;
    u64 value;
};
struct prof_rule {
    struct prof_rec r;
    u8 action, nfill;   /* action 1: return value */
    u16 log;            /* calls to log */
    char nid[12];
    u64 value;
    struct prof_fill fill[]; /* then lib and function name, NUL-terminated */
};
const char *profile_requested(void);
void profile_select(const char *title);
void profile_bind(const u8 *fingerprint);
u32 profile_has(u32 mask);
const struct prof_hdr *profile_hdr(void);
const char *profile_title(void);
const char *profile_thread_name(void);
s32 profile_app_param(u32 id);
const void *profile_record(u32 kind, u32 n);
u64 profile_resolve(const char *lib, const char *nid);

int module_parse(struct module *m, u8 *file, u64 size);
int module_place(struct module *m, u64 base);
int module_relocate(struct module *m);
int module_finish(struct module *m);
struct module *module_open(const char *path, bool is_main); /* read and parse; not in g_modules yet */
void module_publish(struct module *m);                       /* handle, TLS id, append to g_modules */
void module_unpublish(struct module *m);                     /* out of g_modules again (its struct stays valid) */
struct module *module_load_runtime(const char *path, int *err); /* a title prx after boot (SCE error in *err) */
int module_write_text(struct module *m);
void stubs_flush(void);
/* modules.c: which sce_module files load at boot, and starting them */
int modules_load_boot(const char *root, struct module **main_out);
void modules_start_boot(void);
int module_start(struct module *m, u64 argc, const void *argp);
u64 module_export(struct module *m, const char *nid);
const char *module_sym_name(struct module *m, const struct elf_sym *s);

/* ---- resolution (resolve.c, nid.c) ---- */
const char *nid_to_name(const char *nid); /* ext.c: the extension's table, until ext_boot_done */
void nid_keep(const char *nid);           /* keep this NID's name after ext_boot_done (run-time loads) */
void compute_nid(const char *name, char out[12]);
/* kind: what the import is used as.  An unresolved variable binds to 0 (a
 * code stub would be read as data or faulted on when written). */
#define IMPORT_DATA 0   /* a function's address (GLOB_DAT, R_X86_64_64) */
#define IMPORT_PLT 1    /* a PLT slot of a function */
#define IMPORT_OBJECT 2 /* a variable (STT_OBJECT, or STT_NOTYPE outside the PLT) */
u64 resolve_import(struct module *m, const char *encoded, bool weak, int kind);
u64 stub_alloc_code(const u8 *code, u32 len);
int host_module_handle(const char *modname, u32 *out);
int host_module_handle_for(struct module *m, const char *modname, u32 *out);
void crash_install(void);
u64 unresolved_stub(const char *lib, const char *nid, const char *modname, s64 ret, bool denied);

/* ---- HLE + TLS (hle.c, tls.c) ---- */
u64 hle_lookup(const char *lib, const char *name);
struct module *ghost_main_module(void);
void tls_setup(struct module *main_mod);
void *tls_get_addr(void *ti);
int ghost_thread_create(void *thr, const void *attr, void *(*fn)(void *), void *arg, const char *name);
int ghost_thread_start(void *real, void *thr, const void *attr, void *(*fn)(void *), void *arg, const char *name);
bool module_has_tpoff(const struct module *m);
void tls_thread_exit(void);   /* a title thread ends: its TLS blocks go back */
u64 tls_thunk(u64 fn);        /* fn behind a stub that gives the calling thread its TLS block */
/* ELF loader payloads (docs/elfldr.md): every thread's static TLS block is
 * size bytes, whoever's thread it is; a thread started for owner (and the
 * threads it starts) gets owner's image and counts in its live_threads */
void tls_static_reserve(u64 size, u64 align);
struct module *tls_owner(void); /* the payload the calling thread belongs to, or NULL */
int payload_thread_start(struct module *owner, void *thr, const void *attr, void *(*fn)(void *), void *arg,
                         const char *name);
extern void (*g_payload_fault)(struct module *owner, int sig, u64 rip, u64 addr); /* crash.c: a payload thread faulted */
void rpc_lock(void);          /* one user of the compiler-process RPC at a time (loader.c) */
void rpc_unlock(void);
void tls_ensure_thread(void);
extern void (*g_thread_dtors)(void);
void tls_set_thread_dtors(void (*fn)(void));
const char *remap_path(const char *path, char *buf, size_t size);
/* more title path namespaces (registered by the extension's families during
 * ext_init): remap_path asks fn about paths that start with prefix */
int path_remap_add(const char *prefix, const char *(*fn)(const char *path, char *buf, size_t size));
int remap_hook_add(const char *(*fn)(const char *path, char *buf, size_t size)); /* 0, or -1: table full */

/* ---- executable arena (arena.c) ---- */
int arena_init(void);
int arena_code_region(u64 *start, u64 *end);
bool arena_is_rx(u64 addr, u64 size);
u64 arena_small_alloc(u64 size);
u64 arena_small_alloc_top(u64 size, u64 keep);
u64 arena_code_alloc(u64 size); /* next 64 KiB-aligned module base in the code region */
u64 arena_wr(u64 dst, u64 size); /* where the compiler writes [dst, dst+size); 0: it cannot (PS4 only) */
extern u64 g_punched;           /* bytes arena_punch_rw has mapped */
int arena_write(u64 dst, const void *src, u64 size);
int arena_punch_rw(u64 addr, u64 size);
u32 arena_buffers(u64 *start, u64 *end, u32 max); /* whole JIT buffers, not the core's; by address */
void arena_drop(u64 start);                       /* a buffer bigexec.c maps elsewhere: place nothing in it */

/* ---- large executables (bigexec.c, bxplan.c) ---- */
int bx_prepare(void);          /* after arena_init: views for a module the arena cannot hold; -1: no room */
int bx_place(struct module *m);
int bx_write_text(struct module *m, const u8 *text);
struct bx_range {
    s64 lo, hi;                /* module offsets, [lo, hi) */
};
u32 bx_code(const struct module *m, const struct bx_range **runs); /* its code runs (TLS patching) */
bool bx_in_view(const struct module *m, u64 addr);                 /* executable: in one of its views */
/* One of the emulator's JIT objects (home, size): the compiler's base
 * descriptor of it (fd), an RX alias of it here (rx; mapped whole with
 * sceKernelMmap MAP_FIXED, it shows the object anywhere) and where the
 * compiler writes it (wr: its home).  jit_objects_find fills those of the
 * objects in want (bit i: o[i]); 0, or -1 when some are missing. */
struct jit_obj {
    u64 home, size;
    u64 wr;
    int fd, rx;
};
int jit_objects_find(struct jit_obj *o, u32 n, u32 want);

/* ---- runtime extension (ext.c; interface in ghost_api.h) ---- */
#define CTX_FLAG_EXT_JIT 64 /* place the extension in a new JIT block (run.py --ext-jit) */
/* ctx flag bits: 1 probe, 2 release host memory, 4 snap, 8 count imports, 16 profile,
 * 32 force title ready, 64 ext in a JIT block, 128 dialog cancel, 256 IME fixed name,
 * 512 keep Remote Play, 1024 start game modules at boot, 2048 show the game selector,
 * 4096 selector: built-in font only (sel_api.h), 8192 the console is a PS4,
 * 16384 PS4 diagnostic (diag.c), 32768 the host is Jak X under UltraC0re,
 * 262144 the app root is a library, 65536 no automatic memory plan, 131072
 * the automatic plan even over a manual one, 524288 forget the learned
 * memory plan (automem.c); bits 48-63 hold the FTP server's port
 * (CTX_FTP_PORT) */
#define CTX_FLAG_NO_AUTO_MEM 65536      /* plan-free titles keep the plain budget (run.py --no-auto-memory) */
#define CTX_FLAG_AUTO_MEM 131072        /* the automatic plan instead of a profile's manual one (run.py --auto-memory) */
#define CTX_FLAG_AUTO_MEM_FORGET 524288 /* start the learned memory plan over (run.py --forget-memory) */
#define CTX_FLAG_KEEP_REMOTE_PLAY 512 /* the title's sceRemoteplayProhibit is a no-op (run.py --keep-remote-play) */
#define CTX_FLAG_BOOT_START_ALL 1024 /* start IsGame modules at boot too (run.py --boot-start-modules) */
#define CTX_FLAG_SELECT 2048           /* game selector: show it even when the title is known (run.py --select) */
#define CTX_FLAG_SEL_FONT_BUILTIN 4096 /* game selector: never try libSceFont (run.py --select-font builtin) */
#define CTX_FLAG_PS4 8192              /* Luac0re's PLATFORM is "PS4" (ghostldr.lua; docs/ps4.md) */
#define CTX_FLAG_DIAG 16384            /* run the PS4 diagnostic instead of booting (run.py probe --diag) */
#define CTX_FLAG_JAKX 32768            /* UltraC0re in Jak X, not Luac0re in SWRR (ghostldr.lua) */
#define CTX_FLAG_LIBRARY 262144        /* set by the core: app_root is a folder of title folders (the selector reads it) */
#define CTX_FTP_PORT(flags) ((u32)((flags) >> 48)) /* the FTP server's TCP port (run.py --ftp-port; 0: none) */
#define CTX_FLAG_ELFLDR 1048576 /* no title: serve the ELF loader (run.py elfldr, docs/elfldr.md) */
#define CTX_ELFLDR_PORT(flags) ((u32)((flags) >> 32) & 0xFFFF) /* its TCP port (run.py elfldr --port) */
int ghost_elfldr(void);   /* boot.c */
int ext_elfldr(void);     /* ext.c: the extension's ELF loader; returns when told to quit */
static inline bool is_ps4(void) { return (g_ctx->flags & CTX_FLAG_PS4) != 0; }
static inline bool is_jakx(void) { return (g_ctx->flags & CTX_FLAG_JAKX) != 0; }

/* The PS2 classic ghostldr runs inside (host.c): where its executable keeps
 * what the core takes over.  Offsets are from the eboot base. */
struct host_game {
    const char *name;
    u32 video_handle;   /* its main sceVideoOutOpen handle (s32) */
    u32 save_mount;     /* import slot of its sceSaveDataMount, or sceSaveDataMount2 */
    bool save_mount2;   /* the slot is sceSaveDataMount2 (no title ID, other offsets) */
    u32 pthread_self;   /* import slot of scePthreadSelf (diag.c) */
    /* emulator threads the loader left running that use the buffers
     * hostmem.c releases: cancelled first (Luac0re stops SWRR's itself) */
    struct host_thread {
        u32 handle;     /* where its pthread_t is kept */
        const char *name;
    } stop[2], threads[13]; /* threads: all of them, for diag.c */
};
extern const struct host_game *g_hostgame;
int ghost_diag(void);                  /* diag.c */

/* ---- PS4 (ps4.c: decisions without console calls; arena.c, hostmem.c and
 * diag.c make the calls).  docs/ps4.md.
 * The first 16 KiB of each emulator JIT program buffer cannot be written
 * through the compiler: Luac0re puts code at +16 KiB, the core sits at the
 * IPU buffer's +16 KiB, and a write at the EE buffer's start killed the
 * compiler on PS5 (the selector) and on PS4 (module text). */
#define JIT_BUF_HEADER 0x4000
u64 jv_code_need(const struct module *mods);
bool ps4_keep_heap(u64 start, u64 end, u64 heap, bool heap_idle);
int ext_fetch(void);     /* receive and verify ext.bin (the NID table lives there) */
int ext_start(void);     /* place, relocate and initialize its code */
/* app roots other than ghostfs and a title's own directory: USB drives and
 * folders of titles, decided by the extension before the title's modules
 * load (ext/src/approot.c, docs/usb.md).  init: 0 when *root is the title's
 * app0, 1 when the game selector picks (then pick), < 0 on errors */
int ext_approot_init(const char **root);
int ext_approot_pick(const char *title, const char **root);
void ext_abandon(void);  /* a boot stopped before the title loaded: free the code's JIT block and the blob */
void ext_boot_done(void); /* imports resolved: free the blob */
u64 ext_hle_lookup(const char *lib, const char *nid, const char *name);
const struct ext_lib *ext_lib_policy(const char *lib);
void ext_note_sysmodule(u32 id, s32 rc); /* a failed sysmodule load, for the extension's families */
void ext_title_starting(void); /* the title's main thread is next: the extension's last boot step (its FTP server) */

/* ---- game selector (sel.c; module interface in sel_api.h) ---- */
int sel_run(bool show, char title[16]); /* the chosen title's catalogue index and ID, or -1 */
