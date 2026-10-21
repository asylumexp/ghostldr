/*
 * ghostldr runtime extension: include this (only) from every ext/src file.
 *
 * The extension is linked on its own: it cannot call the core directly.  The
 * macros below give the core's globals and services their core names, routed
 * through the API the core passed to ext_init (G), so code reads the same as
 * in the core and can move between the two.  A core function without a macro
 * here is not available: the link fails with an undefined symbol.  rt.c
 * (memcpy, strcmp, fmt, ...) is compiled into the extension itself.
 *
 * Registering functions, one family per file:
 *
 *     static s32 hle_sceFooBar(s32 a) { ... }
 *     static const struct ext_hle_ent k_hle[] = {
 *         {"libSceFoo", "sceFooBar", hle_sceFooBar},
 *         {"libSceFoo", NULL, hle_foo_unnamed, "AbCdEfGhIjK"},   (by NID: no known name)
 *     };
 *     EXT_FAMILY("foo", k_hle, NULL);
 *
 * Library policies (aliases, no host fallback, stub value) come with them:
 *
 *     static const struct ext_lib k_libs[] = {
 *         {"libSceFoo", "libSceFooCompat libSceFooForToolkit", EXT_LIB_NO_HOST, (s32)0x80AA0001},
 *     };
 *     EXT_FAMILY_LIBS("foo", k_hle, k_libs, foo_init);
 *
 * Resolution order: the core's own overrides (hle.c, hle_sys.c, by name) win,
 * then these (by library and NID), then the title's modules, then host
 * modules by name unless a policy says EXT_LIB_NO_HOST, then a stub.
 */
#pragma once
#include "ghost.h"
#include "ghost_api.h"

extern const struct ghost_api *G HIDDEN;

/* core state */
#define g_ctx (*G->ctx)
#define K (*G->k)
#define g_modules (*G->modules)
#define g_main_mod (*G->main_mod)
#define g_reported_dmem (*G->reported_dmem)

/* core services */
#define logf G->logf
#define log_hex G->log_hex
#define host_real G->host_real
#define host_real_in G->host_real_in
#define host_dlsym G->host_dlsym
#define host_module_handle G->host_module_handle
#define host_find_module G->host_find_module
#define call_gadget6 G->call_gadget6
#define nid_to_name G->nid_to_name
#define compute_nid G->compute_nid
#define page_alloc G->page_alloc
#define page_free G->page_free
#define direct_alloc G->direct_alloc
#define direct_free G->direct_free
#define arena_alloc G->arena_alloc
#define arena_strdup G->arena_strdup
#define spin_lock G->spin_lock
#define spin_unlock G->spin_unlock
#define remap_path G->remap_path
#define vfs_open G->vfs_open
#define vfs_read G->vfs_read
#define vfs_lseek G->vfs_lseek
#define vfs_close G->vfs_close
#define vfs_stat G->vfs_stat
#define gfs_active G->gfs_active
#define gfs_owns G->gfs_owns
#define gfs_open G->gfs_open
#define gfs_close G->gfs_close
#define gfs_stat G->gfs_stat
#define gfs_read G->gfs_read
#define gfs_pread G->gfs_pread
#define gfs_lseek G->gfs_lseek
#define gfs_fstat G->gfs_fstat
#define gfs_getdents G->gfs_getdents
#define gfs_mount G->gfs_mount
#define gfs_umount G->gfs_umount
#define gfs_save_name G->gfs_save_name
#define module_export G->module_export
#define ghost_main_module G->ghost_main_module
#define ghost_thread_create G->ghost_thread_create
#define tls_ensure_thread G->tls_ensure_thread
#define tls_get_addr G->tls_get_addr
#define jit_call G->jit_call
#define jit_copy_to G->jit_copy_to
#define arena_write G->arena_write
#define stub_alloc_code G->stub_alloc_code
#define hle_lookup G->hle_lookup
#define title_id G->title_id
#define path_remap_add G->path_remap_add
#define gfs_mount_user G->gfs_mount_user
#define gfs_slot_reserve G->gfs_slot_reserve
#define gfs_slot_release G->gfs_slot_release
#define gfs_title G->gfs_title
#define remap_hook_add G->remap_hook_add

/* One family per file: its HLE table, its library policies and an optional
 * int init(void), run by ext_init first (nonzero: the family is skipped). */
#define EXT_FAMILY_LIBS(family, table, lib_table, init_fn)                                       \
    static const struct ext_family ext_family_ = {family, table, ARRAY_SIZE(table),              \
                                                  ARRAY_SIZE(lib_table), lib_table, init_fn};   \
    static const struct ext_family *const ext_family_ref_                                       \
        __attribute__((used, section("ext_families"))) = &ext_family_

#define EXT_FAMILY(family, table, init_fn)                                                        \
    static const struct ext_family ext_family_ = {family, table, ARRAY_SIZE(table), 0, NULL, init_fn}; \
    static const struct ext_family *const ext_family_ref_                                       \
        __attribute__((used, section("ext_families"))) = &ext_family_
