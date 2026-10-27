/*
 * The ELF loader's service (ext/src/elfldr.c) built for this machine, over
 * the FTP harness's POSIX environment (ftp_host.c) and the fake loader in
 * elfldr_fake.c.  test_elfldr.py drives it:
 *
 *     elfldr_host serve [countdown=N] [boot] [bundles] [cancel=N]
 *                                  prints "listening PORT", serves until it
 *                                  returns ("served RC"): N seconds of
 *                                  countdown, the full extension ("boot"
 *                                  works), bundles taken, the user cancels
 *                                  the countdown when it shows N
 *     elfldr_host size FILE N      el_file_size of FILE's first N bytes
 */
#define FTP_HOST_NO_MAIN
#include "ftp_host.c"
#include "elfldr.h"

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "size") == 0) {
        FILE *f = fopen(argv[2], "rb");
        if (!f)
            return 2;
        static unsigned char buf[64 << 20];
        size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        size_t want = (size_t)atol(argv[3]);
        printf("%ld\n", el_file_size(buf, want < n ? want : n));
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "serve") == 0) {
        extern int g_cancel_at;
        struct el_serve o = {0, 0, 0, 0};
        for (int i = 2; i < argc; i++) {
            if (strncmp(argv[i], "countdown=", 10) == 0)
                o.countdown = (unsigned)atoi(argv[i] + 10);
            else if (strcmp(argv[i], "boot") == 0)
                o.can_boot = 1;
            else if (strcmp(argv[i], "bundles") == 0)
                o.bundles = 1;
            else if (strncmp(argv[i], "cancel=", 7) == 0)
                g_cancel_at = atoi(argv[i] + 7);
        }
        int s = fv_listen(0x7f000001, 0, 1); /* a free port, as the system picks one */
        unsigned a;
        unsigned short port;
        fv_sockname(s, &a, &port);
        close(s);
        o.port = port;
        printf("listening %u\n", port);
        fflush(stdout);
        int rc = elfldr_serve(&o);
        printf("served %d\n", rc);
        fflush(stdout);
        return rc < 0 ? 1 : 0;
    }
    fprintf(stderr, "usage: elfldr_host serve | size FILE N\n");
    return 2;
}
