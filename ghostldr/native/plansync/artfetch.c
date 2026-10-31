/*
 * Game art from the services the player gave keys for (plansync.h
 * ps_art_fetch, docs/selector.md "Game art"), over plansync.c's HTTPS:
 *
 *  - SteamGridDB (one API key, steamgriddb.com > Preferences > API): the
 *    game by name (/api/v2/search/autocomplete/NAME), then an official icon
 *    (or another static icon when there is no official one) for the tile and
 *    a static hero (1920x620 or 1600x650) for the backdrop,
 *    PNG only (mimes=image/png).
 *  - IGDB (a Twitch application's Client ID and secret, dev.twitch.tv >
 *    Console): an app access token from id.twitch.tv (client credentials),
 *    then the game by name among PS4 games (platform 48) with its cover
 *    and artworks; images.igdb.com serves t_cover_big_2x and t_1080p as PNG.
 *
 * SteamGridDB first for each picture, then IGDB.  A service that refuses
 * its key (401, 403) isn't asked again in this job.  Images only over
 * https from the services' own hosts' answers.  Freestanding C, no
 * globals, like plansync.c.  The keys are never logged.
 */
#include <stddef.h>
#include <string.h>
#include "plansync.h"

#define JSON_CAP (64ul << 10)

static unsigned long alen(const char *s)
{
    unsigned long n = 0;
    while (s[n])
        n++;
    return n;
}

/* 0 when s starts with p (strncmp's answer, for a prefix) */
static int strncmp_(const char *s, const char *p)
{
    while (*p && *s == *p)
        s++, p++;
    return *p != 0;
}

static const char *strstr_(const char *h, const char *n)
{
    for (; *h; h++) {
        unsigned long i = 0;
        while (n[i] && h[i] == n[i])
            i++;
        if (!n[i])
            return h;
    }
    return NULL;
}

static void acat(char *b, unsigned long cap, const char *s)
{
    unsigned long n = alen(b);
    while (*s && n + 1 < cap)
        b[n++] = *s++;
    b[n] = 0;
}

static void acat_num(char *b, unsigned long cap, unsigned long v)
{
    char t[24];
    int n = 0;
    do
        t[n++] = (char)('0' + v % 10), v /= 10;
    while (v && n < 23);
    char r[24];
    for (int i = 0; i < n; i++)
        r[i] = t[n - 1 - i];
    r[n] = 0;
    acat(b, cap, r);
}

static void say(struct ps_env *e, const char *a, const char *b, long v)
{
    char line[240] = "art: ";
    acat(line, sizeof(line), a);
    if (b)
        acat(line, sizeof(line), b);
    if (v >= 0)
        acat_num(line, sizeof(line), (unsigned long)v);
    e->log(e, line);
}

/* s percent-encoded for a URL path or a form (letters, digits, -._~ kept) */
static void acat_pct(char *b, unsigned long cap, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    unsigned long n = alen(b);
    for (; *s && n + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~') {
            b[n++] = (char)c;
        } else {
            b[n++] = '%', b[n++] = hex[c >> 4], b[n++] = hex[c & 15];
        }
    }
    b[n] = 0;
}

/* ---- JSON, as far as these answers need it ---- */

/* the value of the first "key" at or after p (before end): a string
 * (escapes undone: \/ \" \\, \uXXXX as '?') or a number; 0 when absent */
static int jget(const char *p, const char *end, const char *key, char *out, unsigned long cap, const char **after)
{
    unsigned long kl = alen(key);
    for (; p + kl + 2 < end; p++) {
        if (*p != '"' || memcmp(p + 1, key, kl) || p[kl + 1] != '"')
            continue;
        const char *s = p + kl + 2;
        while (s < end && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
            s++;
        if (s >= end || *s != ':')
            continue;
        s++;
        while (s < end && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
            s++;
        unsigned long n = 0;
        if (s < end && *s == '"') {
            for (s++; s < end && *s != '"'; s++) {
                char c = *s;
                if (c == '\\' && s + 1 < end) {
                    c = *++s;
                    if (c == 'u') {
                        s += s + 4 < end ? 4 : 0;
                        c = '?';
                    } else if (c == 'n' || c == 't' || c == 'r') {
                        c = ' ';
                    }
                }
                if (n + 1 < cap)
                    out[n++] = c;
            }
        } else {
            for (; s < end && ((*s >= '0' && *s <= '9') || *s == '-') && n + 1 < cap; s++)
                out[n++] = *s;
        }
        out[n] = 0;
        if (after)
            *after = s;
        return n > 0;
    }
    return 0;
}

/* ---- requests ---- */

/* "host[:port]" given (the host tests'), else the default over TLS */
static int host_of(const struct ps_art_keys *k, const char *given, const char *def, char host[64], unsigned short *port)
{
    int tls = !k->plain;
    const char *spec = given[0] ? given : def;
    unsigned long i = 0;
    *port = tls ? 443 : 80;
    while (spec[i] && spec[i] != ':' && i + 1 < 64)
        host[i] = spec[i], i++;
    host[i] = 0;
    if (spec[i] == ':') {
        unsigned p = 0;
        for (const char *s = spec + i + 1; *s >= '0' && *s <= '9'; s++)
            p = p * 10 + (unsigned)(*s - '0');
        if (p && p < 65536)
            *port = (unsigned short)p;
    }
    return tls;
}

/* a JSON answer into buf (JSON_CAP): its status, or -1 */
static int ask(struct ps_env *e, int tls, const char *host, unsigned short port, const char *method, const char *path,
               const char *headers, const char *body, char *buf, unsigned long *len)
{
    int trunc = 0;
    int st = ps_http(e, tls, host, port, method, path, headers, body, body ? alen(body) : 0, (unsigned char *)buf,
                     JSON_CAP - 1, len, &trunc);
    if (st == 200 && trunc)
        st = -1;
    buf[st == 200 ? *len : 0] = 0;
    return st;
}

/* An image from url (https://host/path; http:// too for the host tests):
 * a PNG file in a fresh e->alloc(PS_ART_IMAGE_CAP), or NULL */
static unsigned char *image(struct ps_env *e, const struct ps_art_keys *k, const char *url, unsigned long *len)
{
    int tls = !strncmp_(url, "https://");
    if (!tls && !(k->plain && !strncmp_(url, "http://"))) {
        say(e, "not an https image: left", NULL, -1);
        return NULL;
    }
    const char *h = url + (tls ? 8 : 7), *slash = h;
    while (*slash && *slash != '/')
        slash++;
    char host[64], spec[64];
    unsigned long hl = (unsigned long)(slash - h);
    if (!hl || hl >= sizeof(spec))
        return NULL;
    memcpy(spec, h, hl), spec[hl] = 0;
    struct ps_art_keys plain = *k;
    plain.plain = !tls;
    unsigned short port;
    host_of(&plain, spec, "", host, &port);
    unsigned char *buf = e->alloc(e, PS_ART_IMAGE_CAP);
    if (!buf)
        return NULL;
    int trunc = 0;
    int st = ps_http(e, tls, host, port, "GET", *slash ? slash : "/", "Accept: image/png\r\n", NULL, 0, buf,
                     PS_ART_IMAGE_CAP, len, &trunc);
    static const unsigned char png[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (st != 200 || trunc || *len < 64 || memcmp(buf, png, 8)) {
        say(e, st == 200 ? (trunc ? "image too large from " : "not a PNG from ") : "no image from ", host,
            st == 200 ? -1 : st);
        e->free(e, buf, PS_ART_IMAGE_CAP);
        return NULL;
    }
    return buf;
}

/* ---- SteamGridDB ---- */

static int sgdb_get(struct ps_env *e, const struct ps_art_keys *k, struct ps_art_state *st, const char *path, char *buf)
{
    char host[64], headers[160] = "Authorization: Bearer ";
    unsigned short port;
    int tls = host_of(k, k->sgdb_host, "www.steamgriddb.com", host, &port);
    acat(headers, sizeof(headers), k->sgdb), acat(headers, sizeof(headers), "\r\nAccept: application/json\r\n");
    unsigned long len;
    int s = ask(e, tls, host, port, "GET", path, headers, NULL, buf, &len);
    if (s == 401 || s == 403) {
        st->sgdb_refused = 1;
        say(e, "SteamGridDB refused the API key (Settings > Game art): HTTP ", NULL, s);
    }
    return s;
}

/* the first picture's url in a list answer ({"success":true,"data":[{"url": ...}]}) */
static int sgdb_url(const char *buf, char *url, unsigned long cap)
{
    const char *d = strstr_(buf, "\"data\"");
    return d && jget(d, buf + alen(buf), "url", url, cap, NULL);
}

static int name_mark(const unsigned char *s)
{
    return (s[0] == 0xE2 && s[1] == 0x84 && s[2] == 0xA2) || /* trademark */
           (s[0] == 0xC2 && (s[1] == 0xA9 || s[1] == 0xAE)); /* copyright, registered */
}

static int name_skip(const unsigned char **s)
{
    unsigned char c = **s;
    if (name_mark(*s)) {
        *s += c == 0xE2 ? 3 : 2;
        return 1;
    }
    if (c < 0x80 && !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
        (*s)++;
        return 1;
    }
    return 0;
}

/* SteamGridDB's autocomplete is relevance ordered, and its first result can
 * be a spin-off (for example Ghost of Tsushima: Legends before Ghost of
 * Tsushima).  Pick the entry whose name is the title's name.  ASCII
 * punctuation, case and common title marks do not make names different;
 * extra words and non-ASCII letters still do. */
static int sgdb_same_name(const char *a, const char *b)
{
    const unsigned char *aa = (const unsigned char *)a, *bb = (const unsigned char *)b;
    for (;;) {
        while (*aa && name_skip(&aa))
            ;
        while (*bb && name_skip(&bb))
            ;
        if (!*aa || !*bb)
            return !*aa && !*bb;
        unsigned char ca = *aa++, cb = *bb++;
        if (ca >= 'A' && ca <= 'Z')
            ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z')
            cb += 'a' - 'A';
        if (ca != cb)
            return 0;
    }
}

/* id of name's exact match in an autocomplete answer, rather than the first
 * result's id.  Objects are bounded so a field in a later result cannot be
 * paired with this one's name. */
static int sgdb_game_id(const char *buf, const char *name, char *id, unsigned long cap)
{
    const char *p = strstr_(buf, "\"data\"");
    const char *end = buf + alen(buf);
    if (!p)
        return 0;
    while (p < end && *p != '[')
        p++;
    if (p == end)
        return 0;
    for (p++; p < end && *p != ']'; p++) {
        if (*p != '{')
            continue;
        const char *obj = p, *q = p + 1;
        int depth = 1, string = 0, escape = 0;
        for (; q < end && depth; q++) {
            char c = *q;
            if (string) {
                if (escape)
                    escape = 0;
                else if (c == '\\')
                    escape = 1;
                else if (c == '"')
                    string = 0;
            } else if (c == '"') {
                string = 1;
            } else if (c == '{') {
                depth++;
            } else if (c == '}') {
                depth--;
            }
        }
        if (depth)
            return 0;
        char found[160];
        if (jget(obj, q, "name", found, sizeof(found), NULL) && sgdb_same_name(found, name) &&
            jget(obj, q, "id", id, cap, NULL))
            return 1;
        p = q - 1;
    }
    return 0;
}

static void sgdb(struct ps_env *e, const struct ps_art_keys *k, struct ps_art_state *st, const char *name, int icon,
                 int backdrop, struct ps_art_got *got, char *buf)
{
    char path[512] = "/api/v2/search/autocomplete/", id[24], url[512];
    acat_pct(path, sizeof(path), name);
    if (sgdb_get(e, k, st, path, buf) != 200 || !sgdb_game_id(buf, name, id, sizeof(id))) {
        if (!st->sgdb_refused)
            say(e, "SteamGridDB has no exact game named ", name, -1);
        return;
    }
    if (icon) {
        path[0] = 0;
        /* A grid can be artwork for a mode of the game rather than the game
         * itself (Ghost of Tsushima's first square grid is its Legends
         * mode), so only use SteamGridDB's icon collection for this tile.
         * Do not constrain dimensions: an icon can advertise several sizes
         * instead of one dimension value in the API. */
        acat(path, sizeof(path), "/api/v2/icons/game/"), acat(path, sizeof(path), id);
        acat(path, sizeof(path),
             "?mimes=image/png&types=static&styles=official&nsfw=false&limit=1");
        if (sgdb_get(e, k, st, path, buf) == 200 && sgdb_url(buf, url, sizeof(url)) &&
            (got->icon = image(e, k, url, &got->icon_len)))
            got->icon_from = PS_ART_SGDB;
        if (!got->icon && !st->sgdb_refused) {
            path[0] = 0;
            acat(path, sizeof(path), "/api/v2/icons/game/"), acat(path, sizeof(path), id);
            acat(path, sizeof(path),
                 "?mimes=image/png&types=static&nsfw=false&limit=1");
            if (sgdb_get(e, k, st, path, buf) == 200 && sgdb_url(buf, url, sizeof(url)) &&
                (got->icon = image(e, k, url, &got->icon_len)))
                got->icon_from = PS_ART_SGDB;
        }
    }
    if (backdrop && !st->sgdb_refused) {
        path[0] = 0;
        acat(path, sizeof(path), "/api/v2/heroes/game/"), acat(path, sizeof(path), id);
        acat(path, sizeof(path), "?dimensions=1920x620,1600x650&mimes=image/png&types=static&nsfw=false&limit=1");
        if (sgdb_get(e, k, st, path, buf) == 200 && sgdb_url(buf, url, sizeof(url)) &&
            (got->backdrop = image(e, k, url, &got->backdrop_len)))
            got->backdrop_from = PS_ART_SGDB;
    }
    say(e, "SteamGridDB for ", name, -1);
}

/* ---- IGDB ---- */

static int igdb_token(struct ps_env *e, const struct ps_art_keys *k, struct ps_art_state *st, char *buf)
{
    if (st->igdb_token[0])
        return 0;
    char host[64], body[256] = "client_id=";
    unsigned short port;
    int tls = host_of(k, k->twitch_host, "id.twitch.tv", host, &port);
    acat_pct(body, sizeof(body), k->igdb_id), acat(body, sizeof(body), "&client_secret=");
    acat_pct(body, sizeof(body), k->igdb_secret), acat(body, sizeof(body), "&grant_type=client_credentials");
    unsigned long len;
    int s = ask(e, tls, host, port, "POST", "/oauth2/token", "Content-Type: application/x-www-form-urlencoded\r\n",
                body, buf, &len);
    if (s == 200 && jget(buf, buf + alen(buf), "access_token", st->igdb_token, sizeof(st->igdb_token), NULL))
        return 0;
    if (s == 400 || s == 401 || s == 403) {
        st->igdb_refused = 1;
        say(e, "Twitch refused the IGDB Client ID or secret (Settings > Game art): HTTP ", NULL, s);
    } else {
        say(e, "no IGDB token: HTTP ", NULL, s);
    }
    st->igdb_token[0] = 0;
    return -1;
}

static void igdb(struct ps_env *e, const struct ps_art_keys *k, struct ps_art_state *st, const char *name, int icon,
                 int backdrop, struct ps_art_got *got, char *buf)
{
    if (igdb_token(e, k, st, buf) != 0)
        return;
    char host[64], headers[320] = "Client-ID: ", body[400] = "search \"";
    unsigned short port;
    int tls = host_of(k, k->igdb_host, "api.igdb.com", host, &port);
    acat(headers, sizeof(headers), k->igdb_id), acat(headers, sizeof(headers), "\r\nAuthorization: Bearer ");
    acat(headers, sizeof(headers), st->igdb_token);
    acat(headers, sizeof(headers), "\r\nAccept: application/json\r\nContent-Type: text/plain\r\n");
    unsigned long n = alen(body);
    for (const char *s = name; *s && n + 40 < sizeof(body); s++) /* the name inside "...": no quotes or backslashes */
        if (*s != '"' && *s != '\\')
            body[n++] = *s;
    body[n] = 0;
    acat(body, sizeof(body), "\"; fields name,cover.image_id,artworks.image_id; where platforms = (48); limit 1;");
    unsigned long len;
    int s = ask(e, tls, host, port, "POST", "/v4/games", headers, body, buf, &len);
    if (s == 401 || s == 403) {
        st->igdb_refused = 1;
        say(e, "IGDB refused the token: HTTP ", NULL, s);
        return;
    }
    const char *end = buf + alen(buf), *at;
    char cover[48] = "", art[48] = "", url[160];
    const char *c = strstr_(buf, "\"cover\""), *a = strstr_(buf, "\"artworks\"");
    if (s != 200 || (!c && !a)) {
        say(e, "IGDB has no PS4 game named ", name, -1);
        return;
    }
    if (c)
        jget(c, end, "image_id", cover, sizeof(cover), &at);
    if (a)
        jget(a, end, "image_id", art, sizeof(art), &at);
    if (icon && cover[0]) {
        url[0] = 0;
        acat(url, sizeof(url), k->plain ? "http://" : "https://");
        acat(url, sizeof(url), k->plain ? k->igdb_host : "images.igdb.com");
        acat(url, sizeof(url), "/igdb/image/upload/t_cover_big_2x/"), acat(url, sizeof(url), cover);
        acat(url, sizeof(url), ".png");
        if ((got->icon = image(e, k, url, &got->icon_len)))
            got->icon_from = PS_ART_IGDB;
    }
    if (backdrop && art[0]) {
        url[0] = 0;
        acat(url, sizeof(url), k->plain ? "http://" : "https://");
        acat(url, sizeof(url), k->plain ? k->igdb_host : "images.igdb.com");
        acat(url, sizeof(url), "/igdb/image/upload/t_1080p/"), acat(url, sizeof(url), art), acat(url, sizeof(url), ".png");
        if ((got->backdrop = image(e, k, url, &got->backdrop_len)))
            got->backdrop_from = PS_ART_IGDB;
    }
    say(e, "IGDB for ", name, -1);
}

int ps_art_fetch(struct ps_env *e, const struct ps_art_keys *k, struct ps_art_state *st, const char *name,
                 int want_icon, int want_backdrop, struct ps_art_got *got)
{
    memset(got, 0, sizeof(*got));
    if (!name[0] || (!want_icon && !want_backdrop))
        return -1;
    char *buf = e->alloc(e, JSON_CAP);
    if (!buf)
        return -1;
    if (k->sgdb[0] && !st->sgdb_refused && !e->stop)
        sgdb(e, k, st, name, want_icon, want_backdrop, got, buf);
    int icon = want_icon && !got->icon, backdrop = want_backdrop && !got->backdrop;
    if ((icon || backdrop) && k->igdb_id[0] && k->igdb_secret[0] && !st->igdb_refused && !e->stop)
        igdb(e, k, st, name, icon, backdrop, got, buf);
    e->free(e, buf, JSON_CAP);
    return got->icon || got->backdrop ? 0 : -1;
}
