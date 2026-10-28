/*
 * sqlite_min.c built for this machine (test_sqlite_min.py): writes the
 * database file named on the command line with N rows of savedata.db's
 * columns, for Python's sqlite3 to read back.
 *
 *     sqlite_min_host OUT.db N
 */
#include "../../native/ext/src/sqlite_min.h"

long write(int fd, const void *buf, unsigned long n);
int open(const char *path, int flags, ...);
int close(int fd);
void *calloc(unsigned long n, unsigned long size);
int snprintf(char *buf, unsigned long size, const char *fmt, ...);
long atol(const char *s);

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    long n = atol(argv[2]);
    u64 cap = 4096UL * 600;
    u8 *buf = calloc(1, cap);
    struct sqlm db;
    sqlm_begin(&db, buf, cap, "savedata",
               "CREATE TABLE savedata(title_id TEXT, dir_name TEXT, main_title TEXT, sub_title TEXT, blocks INTEGER, "
               "account_id INTEGER)",
               6);
    for (long i = 0; i < n; i++) {
        char t[16], d[40], m[200];
        snprintf(t, sizeof(t), "CUSA%05ld", i % 100000);
        snprintf(d, sizeof(d), "SAVE%04ld", i);
        snprintf(m, sizeof(m), "Game number %ld with a fairly long name to fill pages up quickly", i);
        struct sqlm_val v[6] = {{t, 0}, {d, 0}, {m, 0}, {i % 3 ? "sub" : "", 0}, {NULL, 96 + i},
                                {NULL, i % 2 ? 0x1122334455667788L : -5}};
        if (!sqlm_row(&db, v))
            return 3;
    }
    u64 size = sqlm_end(&db);
    if (!size)
        return 4;
    int fd = open(argv[1], 0x601 /* O_WRONLY|O_CREAT|O_TRUNC on macOS */, 0644);
    if (fd < 0)
        return 5;
    write(fd, buf, size);
    close(fd);
    return 0;
}
