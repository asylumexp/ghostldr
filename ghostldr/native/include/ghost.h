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
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((u64)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((u64)(a) - 1))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define BOOT_MAGIC 0x3130305244484c47ULL /* "GHLDR001" */

/* Filled by Lua (ghostldr/lua/ghostldr.lua).  Every field is a u64 so the Lua
 * side can write it with write64(ctx + 8 * index, value). */
struct boot_ctx {
    u64 magic;
    u64 size;              /* sizeof(struct boot_ctx) */
    u64 eboot_base;        /* SWRR eboot base */
    u64 dlsym;             /* sceKernelDlsym in main process */
    u64 call_gadget;       /* eboot "call rbx; ret" */
    u64 libkernel_handle;
    u64 libc_handle;
    u64 log_fd;            /* UDP socket from Luac0re create_socket() */
    u64 log_addr;          /* sockaddr_in (16 bytes) of the log server */
    /* compiler-process RPC (port of Luac0re jit_rop) */
    u64 jit_base;
    u64 rop_ret, rop_mov_rax_rdi, rop_pop_rdi, rop_pop_rsi, rop_mov_rdx_rdi;
    u64 rop_pop_rcx, rop_pop_rbx, rop_mov_deref_rbx_rax, rop_pop_rsp;
    u64 rop_pop_r12_r15_rbp, rop_mov_r8_rbx, rop_mov_r9_rbx;
    u64 leak_rbx, leak_rbp, leak_r12, leak_r13, leak_r14, leak_r15;
    u64 fake_rsp, oob_scratch, chain_start, chain_end;
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
};

struct export_ent {
    char nid[12];
    const char *lib;
    u64 addr;
};

extern struct module *g_modules;
extern struct module *g_main_mod;
extern u64 g_reported_dmem;
void budget_patch_text(struct module *m, u8 *text);
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
int vdm_alloc_lazy(u64 len, int type, off_t *out);
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
int gfs_mount(const char *dirname, int mode, u64 blocks, char mount_point[16]);
int gfs_umount(const char mount_point[16], u32 max_ms);
const char *gfs_save_name(void);                        /* <prefix>SAVE */
const char *title_id(void);                             /* the index's title ID, "" if unknown */
int vfs_open(const char *path, int flags, int mode);
s64 vfs_read(int fd, void *buf, u64 n);
s64 vfs_lseek(int fd, s64 off, int whence);
int vfs_close(int fd);
int vfs_stat(const char *path, void *st);
void vdm_log_lazy(void);
bool budget_lazy_site(u64 eboot_off);

int module_parse(struct module *m, u8 *file, u64 size);
int module_place(struct module *m, u64 base);
int module_relocate(struct module *m);
int module_finish(struct module *m);
struct module *module_load_file(const char *path, bool is_main);
u64 module_export(struct module *m, const char *nid);
const char *module_sym_name(struct module *m, const struct elf_sym *s);

/* ---- resolution (resolve.c, nid.c) ---- */
const char *nid_to_name(const char *nid); /* ext.c: the extension's table, until ext_boot_done */
void compute_nid(const char *name, char out[12]);
u64 resolve_import(struct module *m, const char *encoded, bool weak, bool plt_func);
u64 stub_alloc_code(const u8 *code, u32 len);
int host_module_handle(const char *modname, u32 *out);
int host_module_handle_for(struct module *m, const char *modname, u32 *out);
void crash_install(void);
u64 unresolved_stub(const char *lib, const char *nid, s64 ret, bool denied);

/* ---- HLE + TLS (hle.c, tls.c) ---- */
u64 hle_lookup(const char *lib, const char *name);
struct module *ghost_main_module(void);
void tls_setup(struct module *main_mod);
void *tls_get_addr(void *ti);
int ghost_thread_create(void *thr, const void *attr, void *(*fn)(void *), void *arg, const char *name);
void tls_ensure_thread(void);
const char *remap_path(const char *path, char *buf, size_t size);
/* more title path namespaces (registered by the extension's families during
 * ext_init): remap_path asks fn about paths that start with prefix */
int path_remap_add(const char *prefix, const char *(*fn)(const char *path, char *buf, size_t size));

/* ---- executable arena (arena.c) ---- */
int arena_init(void);
int arena_code_region(u64 *start, u64 *end);
bool arena_is_rx(u64 addr, u64 size);
u64 arena_small_alloc(u64 size);
u64 arena_small_alloc_top(u64 size, u64 keep);
int arena_write(u64 dst, const void *src, u64 size);
int arena_punch_rw(u64 addr, u64 size);

/* ---- runtime extension (ext.c; interface in ghost_api.h) ---- */
#define CTX_FLAG_EXT_JIT 64 /* place the extension in a new JIT block (run.py --ext-jit) */
int ext_fetch(void);     /* receive and verify ext.bin (the NID table lives there) */
int ext_start(void);     /* place, relocate and initialize its code */
void ext_boot_done(void); /* imports resolved: free the blob */
u64 ext_hle_lookup(const char *lib, const char *nid, const char *name);
const struct ext_lib *ext_lib_policy(const char *lib);
int ext_dlsym(u32 handle, const char *name, void **out);
