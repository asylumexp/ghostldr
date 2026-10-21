/*
 * The title store: small named records of the running title (trophy state,
 * HLE bookkeeping), kept beside its saves and never in SWRR's.  Implemented
 * by the save data family (ext/src/hle_savedata.c); any family may call it.
 *
 *   stock console (ghostfs): <prefix>SAVE/store/<name> in the title's own
 *       progress container (GHOSTSAVE for Ghost of Tsushima), mounted for the
 *       length of the call;
 *   dev install (/data reachable): <app root>/../savedata/store/<name>.
 *
 * Records belong to the user who started SWRR (the initial user).  A write
 * replaces the whole record and is durable when it returns (the container is
 * committed).  Each call mounts and unmounts a container on the stock
 * console, roughly 100 ms or more: read once, write rarely, never per frame.
 * The calls serialise with the title's save data calls; they work whether or
 * not the title initialised libSceSaveData.
 *
 * Names: 1-31 of A-Z a-z 0-9 . _ -, not starting with '.'.  Size: at most
 * TITLE_STORE_MAX bytes per record.
 */
#pragma once
#include "ghost.h"

#define TITLE_STORE_NAME_MAX 31
#define TITLE_STORE_MAX (1u << 20)

/* errors (negative s32, the kernel's and save data's codes) */
#define TITLE_STORE_E_NOT_FOUND ((s32)0x80020002) /* ENOENT: no such record */
#define TITLE_STORE_E_INVALID ((s32)0x80020016)   /* EINVAL: bad name, NULL buffer or too large */
#define TITLE_STORE_E_NO_SPACE ((s32)0x8002001C)  /* ENOSPC: the container is full */
#define TITLE_STORE_E_INTERNAL ((s32)0x809F000B)  /* the save container could not be mounted or committed */

/* Copies up to len bytes of the record into buf.  Returns the bytes copied
 * (0..len), or a negative TITLE_STORE_E_* code. */
s64 title_store_read(const char *name, void *buf, u64 len);

/* Replaces the record with len bytes from buf (len 0: an empty record).
 * Returns 0 or a negative TITLE_STORE_E_* code. */
s32 title_store_write(const char *name, const void *buf, u64 len);

/* The record's size in bytes, or a negative TITLE_STORE_E_* code. */
s64 title_store_size(const char *name);

/* Deletes the record.  Returns 0 (also when there was none) or a negative
 * TITLE_STORE_E_* code. */
s32 title_store_remove(const char *name);
