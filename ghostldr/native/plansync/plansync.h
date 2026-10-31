/*
 * Community memory plans on the console (docs/community-plans.md): the game
 * selector downloads the plans other players shared from GitHub, and sends
 * the ones this console learned to the collection server.
 *
 *   plansync.c      DNS, HTTPS (BearSSL, third_party/bearssl), GitHub's
 *                   plans/index.json and plans, the collection server.
 *                   Freestanding C with no globals (it links into the
 *                   selector module, whose image is read-only), every
 *                   outside call through struct ps_env.
 *   plansync_sel.c  the console's environment, in the selector module:
 *                   sockets made by the JIT compiler process, SWRR's save
 *                   calls (ghostldr's GLDRCONF container), libkernel.
 *   plansync_ta.c   the certificate authorities it trusts
 *                   (tools/gen_trust_anchors.py).
 *   artfetch.c      game art from the services the player gave keys for
 *                   (SteamGridDB, IGDB), over plansync.c's HTTPS: the
 *                   selector's art job (plansync_sel.c artsync_*).
 *
 * The selector starts ps_run on a thread of its own while it shows the
 * games, and joins it before it returns.  Every failure is silent: a game
 * whose plan could not be checked this time just isn't offered one.  The
 * host tests (ghostldr/tests/test_plansync.py) build plansync.c natively
 * against tests/selector/plansync_host.c.  Plain C types only.
 */
#pragma once

#define PS_PLAN_SIZE 1080 /* struct am_plan (automem.c, AM_VERSION 1) */
#define PS_MAX_TITLES 64
#define PS_MAX_PLANS 32
#define PS_GITHUB_REPO "asylumexp/ghostldr"
#define PS_GITHUB_BRANCH "main"

struct ps_env {
    void *ctx; /* the environment's own */
    /* memory: zeroed; NULL when there is none */
    void *(*alloc)(struct ps_env *e, unsigned long size);
    void (*free)(struct ps_env *e, void *p, unsigned long size);
    void (*log)(struct ps_env *e, const char *line);
    long (*now)(struct ps_env *e);      /* seconds since 1970 (certificates' dates) */
    long (*clock_ms)(struct ps_env *e); /* monotonic milliseconds */
    void (*random)(struct ps_env *e, void *buf, unsigned long n);
    /* sockets: a fresh TCP or UDP socket (the console makes them ahead, on
     * the boot thread), or -errno; connect waits up to timeout_ms; recv
     * gives 0 at the end and waits up to timeouts' ms; send sends all of it */
    int (*tcp)(struct ps_env *e);
    int (*udp)(struct ps_env *e);
    int (*connect)(struct ps_env *e, int fd, unsigned addr, unsigned short port, int timeout_ms); /* addr host order */
    void (*timeouts)(struct ps_env *e, int fd, int ms);
    long (*send)(struct ps_env *e, int fd, const void *buf, unsigned long n);
    long (*recv)(struct ps_env *e, int fd, void *buf, unsigned long n);
    void (*sclose)(struct ps_env *e, int fd);
    int (*dns)(struct ps_env *e, unsigned servers[4]); /* DNS servers to ask in turn (host order): how many */
    /* the store, ghostldr's own files (GLDRCONF): whole files by name */
    long (*load)(struct ps_env *e, const char *name, void *buf, unsigned long cap); /* bytes, or -errno */
    int (*save)(struct ps_env *e, const char *name, const void *buf, unsigned long n);
    int (*unlink)(struct ps_env *e, const char *name);
    int (*list)(struct ps_env *e, const char *prefix, char *names, unsigned long cap); /* NUL-separated: how many */
    /* the collection server: "host" or "host:port", and the key; "" shares nothing */
    char share_host[64];
    char share_key[72];
    int share_tls;      /* 1: https (the public hostname), 0: http (a LAN address, for testing) */
    char version[32];   /* ghostldr's, sent with a plan */
    /* GitHub's two hosts as "host[:port]", and whether over TLS: "" means
     * api.github.com and raw.githubusercontent.com over TLS (the host tests
     * point them at plain HTTP servers of their own) */
    char github_api[64], github_raw[64];
    int github_plain;
    int stop;           /* set by the selector: give up at the next step */
    struct { char host[64]; unsigned addr; } dns_cache[4]; /* names this job resolved */
};

/* one community plan for a game in the catalogue */
struct ps_plan {
    char title[16];
    unsigned char fingerprint[20];
    unsigned platform;     /* 0 PS5, 1 PS4 */
    unsigned contributors;
    unsigned char plan[PS_PLAN_SIZE];
};

enum { PS_IDLE, PS_RUNNING, PS_DONE, PS_FAILED };

struct ps_job {
    /* in: the catalogue's titles, and whether to share and to fetch */
    char titles[PS_MAX_TITLES][16];
    unsigned ntitles;
    int share, fetch;
    /* out, once state is PS_DONE or PS_FAILED (written by the thread, read
     * by the selector after it sees the state change) */
    volatile int state;
    int github;            /* the index request's HTTP status; 0 no answer */
    unsigned nplans;
    struct ps_plan plans[PS_MAX_PLANS];
    unsigned shared, unshared; /* outbox plans sent, and those still waiting */
};

/* the whole job (the selector's thread): share the outbox, then fetch */
void ps_run(struct ps_env *e, struct ps_job *job);
/* is this a struct am_plan this ghostldr reads, for that build? */
int ps_plan_ok(const unsigned char *plan, unsigned long n, const unsigned char fingerprint[20]);

/* one HTTP/1.1 request (plansync.c): the status, or -1 with no complete
 * answer; the body in out (at most cap bytes: *truncated when there was more) */
int ps_http(struct ps_env *e, int tls, const char *host, unsigned short port, const char *method, const char *path,
            const char *headers, const void *body, unsigned long blen, unsigned char *out, unsigned long cap,
            unsigned long *len, int *truncated);

/* ---- game art (artfetch.c, docs/selector.md "Game art") ----
 * A game with no tile picture or backdrop of its own gets them from the
 * services the player gave keys for in Settings > Game art, looked up by
 * its name: SteamGridDB (an official icon, a hero), else IGDB (its cover, an
 * artwork), PNG only (the selector decodes nothing else). */
#define PS_ART_IMAGE_CAP (8ul << 20)
#define PS_ART_SGDB 1 /* where a picture came from (sel.h ART_SGDB, ART_IGDB) */
#define PS_ART_IGDB 2
struct ps_art_keys {
    char sgdb[72];               /* SteamGridDB's API key */
    char igdb_id[48], igdb_secret[72]; /* a Twitch application's Client ID and secret, for IGDB */
    /* the services' hosts as "host[:port]", and whether over TLS: "" means
     * www.steamgriddb.com, id.twitch.tv and api.igdb.com over TLS, and only
     * https images (the host tests point them at a plain server of their own) */
    char sgdb_host[64], twitch_host[64], igdb_host[64];
    int plain;
};
struct ps_art_state {            /* what the job keeps between games */
    char igdb_token[128];        /* Twitch's app access token, once asked for */
    int igdb_refused;            /* the keys were refused: IGDB isn't asked again */
    int sgdb_refused;
};
struct ps_art_got {              /* PNG files, from e->alloc (PS_ART_IMAGE_CAP each): the caller frees them */
    unsigned char *icon, *backdrop;
    unsigned long icon_len, backdrop_len;
    int icon_from, backdrop_from; /* PS_ART_* */
};
/* name's art (want_icon, want_backdrop: what the game lacks): 0 when
 * something was found, -1 when nothing */
int ps_art_fetch(struct ps_env *e, const struct ps_art_keys *k, struct ps_art_state *st, const char *name,
                 int want_icon, int want_backdrop, struct ps_art_got *got);
