/* The save data harness's fake core and console (savedata_fake.c), for
 * savedata_host.c. */
#pragma once
#include "ghost.h"
#include "ghost_api.h"

#define FAKE_USER0 0x10000001  /* the user who started SWRR */
#define FAKE_USER1 0x10000002  /* a second signed-in user */

extern const struct ghost_api g_fake_api;

/* a new console: containers, mounts and counters under root (a fresh
 * directory); gfs: ghostfs is active (12 data containers hold real slots,
 * SWRR's own save holds /savedata0); prefix: the title's (<prefix>SAVE) */
void fake_init(const char *root, bool gfs, const char *prefix, u32 sdk_version);
void fake_set_sdk(u32 v);
const char *fake_root(void);

/* SWRR dies: uncommitted container writes are lost, every mount and fd goes */
void fake_crash(void);

/* real containers */
bool fake_cont_exists(s32 user, const char *name);
void fake_cont_dir(s32 user, const char *name, char *out, size_t size); /* its committed files */
u64 fake_cont_blocks(s32 user, const char *name);
void fake_cont_create(s32 user, const char *name, u64 blocks);         /* made outside the title */
void fake_cont_delete(s32 user, const char *name);                     /* deleted in the PS5's settings */
void fake_set_broken(s32 user, const char *name, bool broken);
bool fake_cont_mounted(s32 user, const char *name);
void fake_set_storage_cap(u64 blocks);
void fake_refuse_destruct_off(bool refuse);
int fake_last_mode(void);

/* invariants: names outside the title's, SWRR's /savedata0 reached, paths
 * outside the test's directories */
u32 fake_violations(void);
u32 fake_swrr_touched(void);
u32 fake_bad_paths(void);
u32 fake_real_mounts(void);     /* real slots in use, SWRR's and ghostfs's included */
u32 fake_max_real_mounts(void); /* the most at once */
u32 fake_data_mounts(void);     /* ghostfs data containers mounted */
u32 fake_reserved(void);        /* gfs_slot_reserve holds */
u32 fake_open_fds(void);
u32 fake_real_calls(void);
void fake_pick_user(s32 user); /* the game selector picked user for the title (title_user) */   /* searches and mounts sent to the real service (other titles) */
const char *fake_mounted_names(void); /* every name the service was asked for, space separated */

/* log lines since fake_log_clear */
bool fake_log_has(const char *needle);
void fake_log_clear(void);

/* host files */
long fake_read_host(const char *path, void *buf, u64 max);
int fake_write_host(const char *path, const void *buf, u64 n);
bool fake_host_exists(const char *path);
void fake_mkdir_host(const char *path);
