/*
 * Read access to the title's virtual save data, for the save data dialog
 * (libSceSaveDataDialog is virtual too: the real one would list SWRR's own
 * saves).  Implemented by the save data family (ext/src/hle_savedata.c).
 *
 * The dialog answers from what these report: a LIST dialog shows the
 * title's dirNames that sd_exists, focuses one by sd_mtime, and a chosen
 * save's result carries its dirName and SceSaveDataParam from sd_get_param.
 *
 * All of them read the catalogue of the title's saves (<prefix>SAVE/catalog.bin
 * on a stock console, <save root>/<user>/catalog.bin on a dev install).  The
 * first call for a user may mount <prefix>SAVE (~100 ms); later ones are
 * answered from memory.  They serialise with the title's own save calls and
 * work whether or not the title initialised libSceSaveData.  user < 0 means
 * the user who started SWRR (the initial user), which is also every title's
 * normal user.  Save data memory (sceSaveDataSetupSaveDataMemory2) is not a
 * save directory and is never listed, like on a PS4.
 */
#pragma once
#include "ghost.h"

/* SceSaveDataParam, 0x530 bytes */
struct sd_param {
    char title[128];
    char sub_title[128];
    char detail[1024];
    u32 user_param;
    u32 reserved0;
    s64 mtime; /* seconds since 1970, set at the last commit or SetParam */
    u8 reserved[32];
};

/* one save directory of the title */
struct sd_save_info {
    char dir[32];     /* SceSaveDataDirName: NUL-padded */
    u64 blocks;       /* the title's size in 32 KiB blocks */
    u64 free_blocks;  /* at its last read/write unmount */
    s64 mtime;
    u32 user_param;
    u32 has_icon;
};

/* Up to max saves of user, in the order they were created; returns how many
 * the user has (may exceed max), or a negative SCE_SAVE_DATA_ERROR_*. */
s32 sd_list(s32 user, struct sd_save_info *out, u32 max);

/* The title's save directory dir (char[32], NUL-terminated or full) of the
 * initial user: whether it exists, its param (0 or 0x809F0008 NOT_FOUND),
 * its mtime (-1 if missing).  The names dialogs.md proposed. */
bool sd_exists(const char *dir);
s32 sd_get_param(const char *dir, struct sd_param *out);
s64 sd_mtime(const char *dir);

/* Everything about one save (savedata.md §6.11): any of param
 * (SceSaveDataParam *), icon (SceSaveDataIcon *: buf and bufSize in,
 * dataSize out; 0 when there is no icon) and info (SceSaveDataSearchInfo *:
 * blocks, freeBlocks) may be NULL.  Returns 0, 0x809F0008 (NOT_FOUND) or
 * another SCE_SAVE_DATA_ERROR_*. */
s32 savedata_query(s32 user, const char *dir, struct sd_param *param, void *icon, void *info);
