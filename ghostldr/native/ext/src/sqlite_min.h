/*
 * A minimal SQLite 3 database writer: one table of text and integer columns,
 * as a file image in memory.  For read-only stand-ins of system databases
 * that homebrew opens with its own SQLite (hle_fsvsh.c: savedata.db).  Built
 * for this machine too (tests/test_sqlite_min.py reads the result with
 * Python's sqlite3).
 */
#pragma once
#include "ext.h"

#define SQLM_PAGE 4096u

/* one value: text (s non-NULL) or integer */
struct sqlm_val {
    const char *s;
    s64 i;
};

struct sqlm {
    u8 *buf;      /* the file image; pages of SQLM_PAGE bytes */
    u64 cap;      /* bytes at buf */
    u32 pages;    /* used */
    u32 ncols;
    u32 leaf;     /* the leaf page being filled (1-based), 0 none */
    u32 nleaves;
    u32 leaves[512];
    s64 last_rowid[512];
    s64 rowid;
    bool overflow; /* buf too small or too many rows: the image is incomplete */
};

/* Start a database in buf (cap bytes, a multiple of SQLM_PAGE, at least 3
 * pages) with one table, created by sql (its CREATE TABLE statement) and
 * holding ncols columns. */
void sqlm_begin(struct sqlm *db, u8 *buf, u64 cap, const char *table, const char *sql, u32 ncols);
/* Append a row of db->ncols values.  false when it no longer fits. */
bool sqlm_row(struct sqlm *db, const struct sqlm_val *v);
/* Finish: the file's size in bytes, or 0 if it overflowed. */
u64 sqlm_end(struct sqlm *db);
