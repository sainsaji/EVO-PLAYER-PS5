/*
 * addon_emby.c — the Emby & Jellyfin media-server client (see addon_emby.h).
 *
 * Everything here runs on the main thread: requests go out through
 * evo_net_request_async and every reply lands from evo_net_poll().
 *
 * CATALOG IDS
 *
 * The provider host treats an item id as opaque, so the catalog encodes where
 * a row leads in the id itself:
 *
 *   v:resume            Continue Watching          (folder)
 *   v:nextup            Next Up                    (folder)
 *   lib:<type>:<id>     a library; <type> is its CollectionType
 *   ser:<id>            a series  -> its seasons
 *   sea:<series>:<id>   a season  -> its episodes
 *   fol:<id>            any other folder (box set, plain folder)
 *   i:<id>              something playable
 *
 * Only this file builds or reads them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "addon_emby.h"
#include "evo_net.h"
#include "cJSON.h"
#include "evo_data_path.h"
#include "evo_provider_log.h"

#include <time.h>

#ifndef EVO_PLAYER_VERSION
#define EVO_PLAYER_VERSION "dev"
#endif

#define MS_DEVICE_ID   "evo-ps5"      /* only until a device_id is made */
#define MS_TAG(c)      ((c)->kind == MS_JELLYFIN ? "jellyfin" : "emby")   /* log lines */
#define MS_PAGE        96          /* rows per catalog request: twelve 8-card pages */
#define MS_FIELDS      "Fields=Overview,ProductionYear,ChildCount,Genres,CommunityRating,OfficialRating,Taglines,ParentBackdropImageTags,ParentBackdropItemId&EnableImageTypes=Primary,Backdrop,Thumb&ImageTypeLimit=1"

struct ms_client {
    ms_kind_t   kind;
    const char *conf;              /* leaf under the data root */
    const char *usb_conf;          /* pre-#46 fallback, Emby only */
    const char *prefix;            /* "/emby" or "" */
    emby_config_t cfg;
    /* One per playback, sent with every report. Emby 4.9 answers a
     * /Sessions/Playing without one with 400 "Value cannot be null (key)". */
    char play_session[64];
    unsigned play_count;
    /* The version about to play (#116), from stream_chosen: reports carry its
     * MediaSourceId, and the server's PlaySessionId when PlaybackInfo gave one.
     * Only used while chosen_item matches the reported item. */
    char chosen_item[96];
    char chosen_source[96];
    char chosen_session[64];
    int  sort, filter;             /* library browsing: see sort_names / ms_filter_names */
    int  years, rating;            /* #220: index into ms_year_* / ms_rating_*, 0 = any */
    char genre[64];                /* "" = any */
    int  chosen_transcode;         /* the chosen stream is a server transcode */
    char qc_secret[128];           /* Quick Connect in progress */
};

/* #220: library filters. Each panel row cycles through its list. */
#define MS_FILTER_COUNT 4
static const char *const ms_filter_names[MS_FILTER_COUNT] = {
    "Everything", "Unwatched only", "Watched only", "Favourites only" };
#define MS_YEAR_COUNT 7
static const char *const ms_year_names[MS_YEAR_COUNT] = {
    "Any", "2020s", "2010s", "2000s", "1990s", "1980s", "1970s" };
static const int ms_year_from[MS_YEAR_COUNT] = { 0, 2020, 2010, 2000, 1990, 1980, 1970 };
static const int ms_year_to[MS_YEAR_COUNT]   = { 0, 2029, 2019, 2009, 1999, 1989, 1979 };
#define MS_RATING_COUNT 4
static const char *const ms_rating_names[MS_RATING_COUNT] = { "Any", "6+", "7+", "8+" };
static const int ms_rating_min[MS_RATING_COUNT] = { 0, 6, 7, 8 };

static ms_client_t g_clients[2] = {
    { MS_EMBY,     "emby.conf",     "/mnt/usb0/.evo_emby.conf", "/emby",
      { .port = 8096, .server_name = "Emby Server" } },
    { MS_JELLYFIN, "jellyfin.conf", NULL,                       "",
      { .port = 8096, .server_name = "Jellyfin" } },
};

ms_client_t *ms_client(ms_kind_t kind)
{
    return &g_clients[kind == MS_JELLYFIN ? 1 : 0];
}

emby_config_t *ms_config(ms_client_t *c) { return &c->cfg; }

/* ------------------------------------------------------------------------- */
/* Config                                                                    */
/* ------------------------------------------------------------------------- */

/* A fresh id for this console, made once. Not a hardware serial: nothing here
 * needs one, only that two consoles differ and one console stays the same. */
static void ms_make_device_id(emby_config_t *g)
{
    if (g->device_id[0]) return;
    unsigned long long a = (unsigned long long)time(NULL);
    unsigned long long b = (unsigned long long)clock() * 2654435761ULL;
    unsigned long long x = (a * 6364136223846793005ULL) ^ (b + (unsigned long long)(size_t)g);
    snprintf(g->device_id, sizeof g->device_id, "evo-ps5-%08llx%08llx",
             (x >> 32) & 0xffffffffULL, x & 0xffffffffULL);
}

int ms_load(ms_client_t *c)
{
    emby_config_t *g = &c->cfg;
    char name[sizeof g->server_name];
    snprintf(name, sizeof name, "%s", c->kind == MS_EMBY ? "Emby Server" : "Jellyfin");
    memset(g, 0, sizeof *g);
    g->port = 8096;
    snprintf(g->server_name, sizeof g->server_name, "%s", name);

    FILE *f = fopen(evo_data_path(c->conf), "r");
    if (!f && c->usb_conf) f = fopen(c->usb_conf, "r");
    if (!f) { ms_make_device_id(g); return 0; }   /* not set up yet - not an error */

    char line[1200];                     /* token= can be a long JWT */
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        const char *v = strchr(line, '=');
        if (!v) continue;
        v++;
        if      (!strncmp(line, "host=", 5))        snprintf(g->host, sizeof g->host, "%s", v);
        else if (!strncmp(line, "port=", 5))        { g->port = atoi(v); if (g->port <= 0 || g->port > 65535) g->port = 8096; }
        else if (!strncmp(line, "username=", 9))    snprintf(g->username, sizeof g->username, "%s", v);
        else if (!strncmp(line, "token=", 6))       snprintf(g->token, sizeof g->token, "%s", v);
        else if (!strncmp(line, "user_id=", 8))     snprintf(g->user_id, sizeof g->user_id, "%s", v);
        else if (!strncmp(line, "server_name=", 12)) snprintf(g->server_name, sizeof g->server_name, "%s", v);
        else if (!strncmp(line, "https=", 6))       g->use_https = atoi(v) ? true : false;
        else if (!strncmp(line, "path=", 5))        snprintf(g->path, sizeof g->path, "%s", v);
        else if (!strncmp(line, "transcode=", 10))  g->transcode = atoi(v) ? 1 : 0;
        else if (!strncmp(line, "device_id=", 10))  snprintf(g->device_id, sizeof g->device_id, "%s", v);
    }
    fclose(f);
    const bool had_id = g->device_id[0] != 0;
    ms_make_device_id(g);
    g->is_connected = g->token[0] && g->user_id[0];
    /* An id made now is lost on the next start unless it is written: a server
     * would see a new device every launch. Only for a server that is set up. */
    if (!had_id && g->host[0]) ms_save(c);
    return 0;
}

int ms_save(ms_client_t *c)
{
    emby_config_t *g = &c->cfg;
    FILE *f = fopen(evo_data_path(c->conf), "w");
    if (!f && c->usb_conf) f = fopen(c->usb_conf, "w");
    if (!f) return -1;
    /* No password: the token is the session, and a password in a plain file
     * on the console is a password anyone with FTP can read. */
    fprintf(f, "host=%s\nport=%d\nusername=%s\ntoken=%s\nuser_id=%s\nserver_name=%s\nhttps=%d\npath=%s\ndevice_id=%s\ntranscode=%d\n",
            g->host, g->port, g->username, g->token, g->user_id, g->server_name,
            g->use_https ? 1 : 0, g->path, g->device_id, g->transcode);
    fclose(f);
    return 0;
}

int ms_is_configured(ms_client_t *c) { return c->cfg.host[0] ? 1 : 0; }

int ms_needs_sign_in(ms_client_t *c)
{
    return c->cfg.host[0] && !c->cfg.is_connected;
}

static void ms_drop_session(ms_client_t *c)
{
    c->cfg.token[0] = '\0';
    c->cfg.user_id[0] = '\0';
    c->cfg.is_connected = false;
    ms_save(c);
}

int ms_is_signed_in(ms_client_t *c)
{
    return c->cfg.host[0] && c->cfg.token[0];
}

void ms_sign_out(ms_client_t *c)
{
    ms_drop_session(c);
}

const char *ms_get_source(ms_client_t *c)
{
    static char src[2][176];
    char *out = src[c->kind == MS_JELLYFIN ? 1 : 0];
    if (!c->cfg.host[0]) return "";
    int port = c->cfg.port > 0 ? c->cfg.port : 8096;
    if ((c->cfg.use_https && port == 443) || (!c->cfg.use_https && port == 80))
        snprintf(out, sizeof src[0], "%s://%s%s", c->cfg.use_https ? "https" : "http",
                 c->cfg.host, c->cfg.path);
    else
        snprintf(out, sizeof src[0], "%s://%s:%d%s", c->cfg.use_https ? "https" : "http",
                 c->cfg.host, port, c->cfg.path);
    return out;
}

int ms_set_source(ms_client_t *c, const char *value)
{
    if (!value || !value[0]) {
        memset(&c->cfg, 0, sizeof c->cfg);
        unlink(evo_data_path(c->conf));
        if (c->usb_conf) unlink(c->usb_conf);
        return 0;
    }
    char host[128];
    int port = 8096, tls = 0;
    if (evo_provider_parse_web_source(value, host, sizeof host, &port, &tls, 8096) != 0)
        return -1;
    snprintf(c->cfg.host, sizeof c->cfg.host, "%s", host);
    c->cfg.port = port;
    c->cfg.use_https = tls ? true : false;
    /* The path after host[:port] - a reverse proxy's "/jellyfin" - without
     * a query, a fragment or a trailing '/'. The parser above ignores it. */
    c->cfg.path[0] = '\0';
    {
        const char *v = value;
        while (*v == ' ') v++;
        const char *s = strstr(v, "://");
        s = s ? s + 3 : v;
        const char *slash = strchr(s, '/');
        if (slash) {
            size_t n = strcspn(slash, "?# ");
            while (n > 0 && slash[n - 1] == '/') n--;
            if (n > 0 && n < sizeof c->cfg.path)
                snprintf(c->cfg.path, sizeof c->cfg.path, "%.*s", (int)n, slash);
        }
    }
    c->cfg.token[0] = '\0';
    c->cfg.user_id[0] = '\0';
    c->cfg.is_connected = false;
    return ms_save(c);
}

/* ------------------------------------------------------------------------- */
/* Requests                                                                  */
/* ------------------------------------------------------------------------- */

static void ms_base(ms_client_t *c, char *out, size_t cap)
{
    /* Emby behind a proxy at /emby already is the /emby prefix. */
    size_t pl = strlen(c->cfg.path);
    const char *prefix = (c->prefix[0] && pl >= 5 && !strcmp(c->cfg.path + pl - 5, "/emby"))
                       ? "" : c->prefix;
    snprintf(out, cap, "%s://%s:%d%s%s", c->cfg.use_https ? "https" : "http",
             c->cfg.host, c->cfg.port > 0 ? c->cfg.port : 8096, c->cfg.path, prefix);
}

static void ms_auth_header(ms_client_t *c, int with_token, char *out, size_t cap)
{
    if (c->kind == MS_EMBY && with_token && c->cfg.token[0]) {
        snprintf(out, cap, "X-Emby-Token: %s", c->cfg.token);
        return;
    }
    int n = snprintf(out, cap,
                     "%s: MediaBrowser Client=\"EVO Player\", Device=\"PlayStation 5\", "
                     "DeviceId=\"%s\", Version=\"%s\"",
                     c->kind == MS_EMBY ? "X-Emby-Authorization" : "Authorization",
                     c->cfg.device_id[0] ? c->cfg.device_id : MS_DEVICE_ID, EVO_PLAYER_VERSION);
    if (with_token && c->cfg.token[0] && n > 0 && (size_t)n < cap)
        snprintf(out + n, cap - (size_t)n, ", Token=\"%s\"", c->cfg.token);
}

static int ms_request(ms_client_t *c, const char *method, const char *path,
                      const char *body, int with_token, evo_net_cb cb, void *ud)
{
    char base[200], url[EVO_PROVIDER_MAX_URL], auth[1400];   /* header + a JWT token */
    ms_base(c, base, sizeof base);
    if (snprintf(url, sizeof url, "%s%s", base, path) >= (int)sizeof url) return -1;
    ms_auth_header(c, with_token, auth, sizeof auth);
    if (c->kind == MS_JELLYFIN) {
        /* As Jelly5's Jellyfin client does (github.com/02dnot/Jelly5): FFmpeg's
         * HTTP client, an explicit JSON Accept, and a body on every POST - an
         * empty one is "{}", so the request always carries a Content-Length. */
        const char *headers[2] = { auth, "Accept: application/json" };
        const bool post = method && !strcmp(method, "POST");
        const char *b = (body && body[0]) ? body : (post ? "{}" : NULL);
        return evo_net_request_async_avio(method, url, b, headers, 2, cb, ud);
    }
    const char *headers[1] = { auth };
    return evo_net_request_async(method, url, body, headers, 1, cb, ud);
}

/* ------------------------------------------------------------------------- */
/* Sign-in                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct {
    ms_client_t *c;
    void (*cb)(const char *name, void *ud);
    void *ud;
} user_ctx_t;

static void on_public_users(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    user_ctx_t *x = (user_ctx_t *)ud;
    const char *name = "";
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    if (root && cJSON_IsArray(root) && cJSON_GetArraySize(root) > 0) {
        cJSON *n = cJSON_GetObjectItem(cJSON_GetArrayItem(root, 0), "Name");
        if (n && cJSON_IsString(n)) name = n->valuestring;
    }
    if (x->cb) x->cb(name, x->ud);
    if (root) cJSON_Delete(root);
    free(x);
}

int ms_suggest_user(ms_client_t *c, void (*cb)(const char *name, void *ud), void *ud)
{
    if (!c->cfg.host[0]) return -1;
    user_ctx_t *x = (user_ctx_t *)calloc(1, sizeof *x);
    if (!x) return -2;
    x->c = c; x->cb = cb; x->ud = ud;
    int rc = ms_request(c, "GET", "/Users/Public", NULL, 0, on_public_users, x);
    if (rc != 0) free(x);
    return rc;
}

typedef struct {
    ms_client_t *c;
    evo_provider_auth_cb cb;
    void *ud;
    char user[64];
} signin_ctx_t;

static void on_sign_in(int ok, int status, const char *body, size_t len, void *ud)
{
    signin_ctx_t *x = (signin_ctx_t *)ud;
    ms_client_t *c = x->c;
    const char *msg = "Could not reach the server";
    int good = 0;

    if (ok && (status == 401 || status == 403)) {
        msg = "Wrong user name or password";
        PROV_ERR("%s: sign-in refused (http %d)", MS_TAG(c), status);
    } else if (ok && status == 200 && body) {
        cJSON *root = cJSON_Parse(body);
        cJSON *tok  = root ? cJSON_GetObjectItem(root, "AccessToken") : NULL;
        cJSON *user = root ? cJSON_GetObjectItem(root, "User") : NULL;
        cJSON *uid  = user ? cJSON_GetObjectItem(user, "Id") : NULL;
        if (tok && cJSON_IsString(tok) && uid && cJSON_IsString(uid)) {
            snprintf(c->cfg.token, sizeof c->cfg.token, "%s", tok->valuestring);
            snprintf(c->cfg.user_id, sizeof c->cfg.user_id, "%s", uid->valuestring);
            snprintf(c->cfg.username, sizeof c->cfg.username, "%s", x->user);
            c->cfg.is_connected = true;
            ms_save(c);
            good = 1;
            msg = "Signed in";
        } else {
            msg = "The server sent an answer EVO does not understand";
            PROV_ERR("%s: sign-in reply has no AccessToken/User.Id (%zu bytes, json=%d)",
                     MS_TAG(c), len, root != NULL);
        }
        if (root) cJSON_Delete(root);
    } else {
        PROV_ERR("%s: sign-in failed ok=%d http=%d", MS_TAG(c), ok, status);
    }
    if (good) PROV_LOG("%s: signed in", MS_TAG(c));
    if (x->cb) x->cb(good, msg, x->ud);
    free(x);
}

int ms_sign_in(ms_client_t *c, const char *user, const char *password,
               evo_provider_auth_cb cb, void *ud)
{
    if (!c->cfg.host[0] || !user || !*user) return -1;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "Username", user);
    cJSON_AddStringToObject(req, "Pw", password ? password : "");
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) return -2;

    signin_ctx_t *x = (signin_ctx_t *)calloc(1, sizeof *x);
    if (!x) { free(body); return -2; }
    x->c = c; x->cb = cb; x->ud = ud;
    snprintf(x->user, sizeof x->user, "%s", user);

    int rc = ms_request(c, "POST", "/Users/AuthenticateByName", body, 0, on_sign_in, x);
    free(body);
    if (rc != 0) free(x);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Quick Connect (Jellyfin)                                                  */
/* ------------------------------------------------------------------------- */

typedef struct {
    ms_client_t *c;
    evo_provider_qc_code_cb code_cb;
    evo_provider_qc_cb poll_cb;
    void *ud;
} qc_ctx_t;

static void on_qc_initiate(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    qc_ctx_t *x = (qc_ctx_t *)ud;
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    cJSON *sec  = root ? cJSON_GetObjectItem(root, "Secret") : NULL;
    cJSON *code = root ? cJSON_GetObjectItem(root, "Code") : NULL;
    if (sec && cJSON_IsString(sec) && code && cJSON_IsString(code)) {
        snprintf(x->c->qc_secret, sizeof x->c->qc_secret, "%s", sec->valuestring);
        if (x->code_cb) x->code_cb(1, code->valuestring, x->ud);
    } else {
        /* 401 = Quick Connect switched off on the server: password instead. */
        PROV_LOG("mediasrv: quick connect: initiate -> ok=%d http=%d err=%d", ok, status, evo_net_callback_error());
        x->c->qc_secret[0] = '\0';
        if (x->code_cb) x->code_cb(0, "", x->ud);
    }
    if (root) cJSON_Delete(root);
    free(x);
}

int ms_qc_start(ms_client_t *c, evo_provider_qc_code_cb cb, void *ud)
{
    if (!c->cfg.host[0] || c->kind != MS_JELLYFIN) return -1;
    qc_ctx_t *x = (qc_ctx_t *)calloc(1, sizeof *x);
    if (!x) return -2;
    x->c = c; x->code_cb = cb; x->ud = ud;
    int rc = ms_request(c, "POST", "/QuickConnect/Initiate", "", 0, on_qc_initiate, x);
    if (rc != 0) free(x);
    return rc;
}

static void on_qc_session(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    qc_ctx_t *x = (qc_ctx_t *)ud;
    ms_client_t *c = x->c;
    int good = 0;
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    cJSON *tok  = root ? cJSON_GetObjectItem(root, "AccessToken") : NULL;
    cJSON *user = root ? cJSON_GetObjectItem(root, "User") : NULL;
    cJSON *uid  = user ? cJSON_GetObjectItem(user, "Id") : NULL;
    cJSON *name = user ? cJSON_GetObjectItem(user, "Name") : NULL;
    if (tok && cJSON_IsString(tok) && uid && cJSON_IsString(uid)) {
        snprintf(c->cfg.token, sizeof c->cfg.token, "%s", tok->valuestring);
        snprintf(c->cfg.user_id, sizeof c->cfg.user_id, "%s", uid->valuestring);
        if (name && cJSON_IsString(name))
            snprintf(c->cfg.username, sizeof c->cfg.username, "%s", name->valuestring);
        c->cfg.is_connected = true;
        c->qc_secret[0] = '\0';
        ms_save(c);
        good = 1;
    } else {
        PROV_ERR("%s: quick connect sign-in refused ok=%d http=%d", MS_TAG(c), ok, status);
    }
    if (root) cJSON_Delete(root);
    if (x->poll_cb) x->poll_cb(good ? 1 : -1, good ? "Signed in" : "The server refused the approved code", x->ud);
    free(x);
}

static void on_qc_connect(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    qc_ctx_t *x = (qc_ctx_t *)ud;
    ms_client_t *c = x->c;
    if (!ok || !body) {                 /* a dropped poll: try again next time */
        if (x->poll_cb) x->poll_cb(0, "", x->ud);
        free(x);
        return;
    }
    if (status != 200) {                /* the secret expired or was rejected */
        PROV_WARN("%s: quick connect code expired or rejected (http %d)", MS_TAG(c), status);
        c->qc_secret[0] = '\0';
        if (x->poll_cb) x->poll_cb(-1, "The code expired", x->ud);
        free(x);
        return;
    }
    cJSON *root = cJSON_Parse(body);
    int approved = root && cJSON_IsTrue(cJSON_GetObjectItem(root, "Authenticated"));
    if (root) cJSON_Delete(root);
    if (!approved) {
        if (x->poll_cb) x->poll_cb(0, "", x->ud);
        free(x);
        return;
    }
    char req[200];
    snprintf(req, sizeof req, "{\"Secret\":\"%s\"}", c->qc_secret);
    if (ms_request(c, "POST", "/Users/AuthenticateWithQuickConnect", req, 0, on_qc_session, x) != 0) {
        if (x->poll_cb) x->poll_cb(-1, "Could not reach the server", x->ud);
        free(x);
    }
}

int ms_qc_poll(ms_client_t *c, evo_provider_qc_cb cb, void *ud)
{
    if (!c->qc_secret[0]) return -1;
    qc_ctx_t *x = (qc_ctx_t *)calloc(1, sizeof *x);
    if (!x) return -2;
    x->c = c; x->poll_cb = cb; x->ud = ud;
    char path[200];
    snprintf(path, sizeof path, "/QuickConnect/Connect?Secret=%s", c->qc_secret);
    int rc = ms_request(c, "GET", path, NULL, 0, on_qc_connect, x);
    if (rc != 0) free(x);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Items                                                                     */
/* ------------------------------------------------------------------------- */

static const char *jstr(cJSON *o, const char *k)
{
    cJSON *v = o ? cJSON_GetObjectItem(o, k) : NULL;
    return (v && cJSON_IsString(v)) ? v->valuestring : "";
}

static double jnum(cJSON *o, const char *k)
{
    cJSON *v = o ? cJSON_GetObjectItem(o, k) : NULL;
    return (v && cJSON_IsNumber(v)) ? v->valuedouble : 0.0;
}

static int jbool(cJSON *o, const char *k)
{
    cJSON *v = o ? cJSON_GetObjectItem(o, k) : NULL;
    return v && cJSON_IsTrue(v);
}

static void fmt_runtime(int64_t secs, char *out, size_t cap)
{
    out[0] = '\0';
    if (secs <= 0) return;
    int h = (int)(secs / 3600), m = (int)((secs % 3600) / 60);
    if (h > 0) snprintf(out, cap, "%dh %dm", h, m);
    else       snprintf(out, cap, "%dm", m > 0 ? m : 1);
}

/* A poster for `id`, or "" for no art. 2:3 posters only: a library's 16:9
 * thumb stretched into a poster card looks broken, so folders that only have
 * one get the folder icon instead. */
static void art_url(ms_client_t *c, const char *id, char *out, size_t cap)
{
    char base[200];
    ms_base(c, base, sizeof base);
    snprintf(out, cap, "%s/Items/%s/Images/Primary?maxHeight=480&quality=85&format=jpg",
             base, id);
}

/* Context flags for the subtitle wording. */
enum { CTX_LIST = 0, CTX_SEASON = 1, CTX_MIXED = 2 };

static void append_meta(cJSON *o, evo_provider_item_t *it);

static void map_item(ms_client_t *c, cJSON *o, int ctx, evo_provider_item_t *it)
{
    evo_provider_item_clear(it);
    const char *id   = jstr(o, "Id");
    const char *type = jstr(o, "Type");
    const char *name = jstr(o, "Name");
    int year         = (int)jnum(o, "ProductionYear");
    int64_t runtime  = (int64_t)(jnum(o, "RunTimeTicks") / 10000000.0);
    cJSON *ud        = cJSON_GetObjectItem(o, "UserData");
    cJSON *tags      = cJSON_GetObjectItem(o, "ImageTags");
    int has_primary  = tags && cJSON_GetObjectItem(tags, "Primary") != NULL;
    char rt[24];
    fmt_runtime(runtime, rt, sizeof rt);

    snprintf(it->title, sizeof it->title, "%s", name);
    snprintf(it->overview, sizeof it->overview, "%s", jstr(o, "Overview"));
    it->duration_sec = runtime;
    it->resume_pos_sec = (int64_t)(jnum(ud, "PlaybackPositionTicks") / 10000000.0);
    it->played = jbool(ud, "Played");
    it->favorite = jbool(ud, "IsFavorite");

    if (!strcmp(type, "Series")) {
        snprintf(it->id, sizeof it->id, "ser:%s", id);
        it->is_folder = 1;
        it->kind = EVO_MEDIA_FOLDER;
        it->detail = 2;
        int n = (int)jnum(o, "ChildCount");
        if (year && n > 0) snprintf(it->subtitle, sizeof it->subtitle, "%d · %d season%s", year, n, n == 1 ? "" : "s");
        else if (year)     snprintf(it->subtitle, sizeof it->subtitle, "%d", year);
        if (has_primary) art_url(c, id, it->art_url, sizeof it->art_url);
    } else if (!strcmp(type, "Season")) {
        snprintf(it->id, sizeof it->id, "sea:%s:%s", jstr(o, "SeriesId"), id);
        it->is_folder = 1;
        it->kind = EVO_MEDIA_FOLDER;
        int n = (int)jnum(o, "ChildCount");
        if (n > 0) snprintf(it->subtitle, sizeof it->subtitle, "%d episode%s", n, n == 1 ? "" : "s");
        if (has_primary)                 art_url(c, id, it->art_url, sizeof it->art_url);
        else if (jstr(o, "SeriesId")[0]) art_url(c, jstr(o, "SeriesId"), it->art_url, sizeof it->art_url);
    } else if (jbool(o, "IsFolder")) {
        snprintf(it->id, sizeof it->id, "fol:%s", id);
        it->is_folder = 1;
        it->kind = EVO_MEDIA_FOLDER;
        int n = (int)jnum(o, "ChildCount");
        if (n > 0) snprintf(it->subtitle, sizeof it->subtitle, "%d item%s", n, n == 1 ? "" : "s");
        if (has_primary) art_url(c, id, it->art_url, sizeof it->art_url);
    } else if (!strcmp(type, "Audio")) {
        snprintf(it->id, sizeof it->id, "a:%s", id);
        it->kind = EVO_MEDIA_AUDIO;
        cJSON *artists = cJSON_GetObjectItem(o, "Artists");
        cJSON *a0 = artists ? cJSON_GetArrayItem(artists, 0) : NULL;
        const char *artist = (a0 && cJSON_IsString(a0)) ? a0->valuestring : jstr(o, "AlbumArtist");
        snprintf(it->subtitle, sizeof it->subtitle, "%s%s%s", artist, artist[0] && rt[0] ? " · " : "", rt);
        if (jstr(o, "AlbumId")[0]) art_url(c, jstr(o, "AlbumId"), it->art_url, sizeof it->art_url);
        else if (has_primary)      art_url(c, id, it->art_url, sizeof it->art_url);
    } else if (!strcmp(type, "TvChannel")) {
        snprintf(it->id, sizeof it->id, "tv:%s", id);
        it->kind = EVO_MEDIA_VIDEO;
        it->is_live = 1;
        cJSON *cp = cJSON_GetObjectItem(o, "CurrentProgram");
        const char *num = jstr(o, "Number");
        snprintf(it->subtitle, sizeof it->subtitle, "%s%s%s", num, num[0] && cp ? " · " : "", cp ? jstr(cp, "Name") : "");
        snprintf(it->now_title, sizeof it->now_title, "%s", cp ? jstr(cp, "Name") : "");
        if (has_primary) art_url(c, id, it->art_url, sizeof it->art_url);
    } else {
        snprintf(it->id, sizeof it->id, "i:%s", id);
        it->kind = EVO_MEDIA_VIDEO;
        if (!strcmp(type, "Episode")) {
            int s = (int)jnum(o, "ParentIndexNumber"), e = (int)jnum(o, "IndexNumber");
            if (ctx == CTX_SEASON)
                snprintf(it->subtitle, sizeof it->subtitle, "Episode %d%s%s", e, rt[0] ? " · " : "", rt);
            else
                snprintf(it->subtitle, sizeof it->subtitle, "%s · S%d E%d", jstr(o, "SeriesName"), s, e);
            /* An episode's own Primary is a 16:9 still; the poster card wants
             * the series poster. */
            if (jstr(o, "SeriesId")[0]) art_url(c, jstr(o, "SeriesId"), it->art_url, sizeof it->art_url);
        } else {
            it->detail = 1;
            if (year && rt[0]) snprintf(it->subtitle, sizeof it->subtitle, "%d · %s", year, rt);
            else if (year)     snprintf(it->subtitle, sizeof it->subtitle, "%d", year);
            else               snprintf(it->subtitle, sizeof it->subtitle, "%s", rt);
            if (has_primary) art_url(c, id, it->art_url, sizeof it->art_url);
        }
    }
    if (!strcmp(type, "Movie") || !strcmp(type, "Series") || !strcmp(type, "Episode"))
        append_meta(o, it);

    /* The wide image and the title logo, for the shelves' background. An episode
     * or season borrows its series' backdrop. */
    {
        cJSON *bd = cJSON_GetObjectItem(o, "BackdropImageTags");
        cJSON *pbd = cJSON_GetObjectItem(o, "ParentBackdropImageTags");
        const char *bid = NULL;
        if (bd && cJSON_GetArraySize(bd) > 0) bid = id;
        else if (pbd && cJSON_GetArraySize(pbd) > 0 && jstr(o, "ParentBackdropItemId")[0])
            bid = jstr(o, "ParentBackdropItemId");
        char base[200];
        ms_base(c, base, sizeof base);
        if (bid)
            snprintf(it->backdrop_url, sizeof it->backdrop_url,
                     "%s/Items/%s/Images/Backdrop?maxWidth=1280&quality=70&format=jpg", base, bid);
        /* If the poster is not there: an episode's own still, else the backdrop, else the thumb. */
        const int is_episode = !strcmp(type, "Episode");
        if (is_episode && jstr(o, "SeriesId")[0] && tags && cJSON_GetObjectItem(tags, "Primary"))
            snprintf(it->art_alt_url, sizeof it->art_alt_url,
                     "%s/Items/%s/Images/Primary?maxWidth=640&quality=70&format=jpg", base, id);
        else if (bid)
            snprintf(it->art_alt_url, sizeof it->art_alt_url,
                     "%s/Items/%s/Images/Backdrop?maxWidth=640&quality=70&format=jpg", base, bid);
        else if (tags && cJSON_GetObjectItem(tags, "Thumb"))
            snprintf(it->art_alt_url, sizeof it->art_alt_url,
                     "%s/Items/%s/Images/Thumb?maxWidth=640&quality=70&format=jpg", base, id);
        if (is_episode && ctx == CTX_SEASON) {
            char *tj = tags ? cJSON_PrintUnformatted(tags) : NULL;
            PROV_LOG("mediasrv: episode art: '%s' ImageTags=%s SeriesPrimary=%s SeasonId=%s parentBackdrop=%d art=%s alt=%s",
                     name, tj ? tj : "none", jstr(o, "SeriesPrimaryImageTag")[0] ? "yes" : "no",
                     jstr(o, "SeasonId")[0] ? "yes" : "no", pbd ? cJSON_GetArraySize(pbd) : -1,
                     it->art_url[0] ? "set" : "none", it->art_alt_url[0] ? "set" : "none");
            free(tj);
        }
        if (tags && cJSON_GetObjectItem(tags, "Logo"))
            snprintf(it->logo_url, sizeof it->logo_url,
                     "%s/Items/%s/Images/Logo?maxWidth=480", base, id);
        else if (jstr(o, "ParentLogoItemId")[0] && jstr(o, "ParentLogoImageTag")[0])
            snprintf(it->logo_url, sizeof it->logo_url,
                     "%s/Items/%s/Images/Logo?maxWidth=480", base, jstr(o, "ParentLogoItemId"));
    }
}

/* " · ★ 7.8 · PG-13 · Action, Drama" on the end of the subtitle line, and the
 * tagline in front of the overview, when the server has them. */
static void append_meta(cJSON *o, evo_provider_item_t *it)
{
    char meta[96] = "";
    size_t n = 0;
    double rating = jnum(o, "CommunityRating");
    const char *off = jstr(o, "OfficialRating");
    if (rating > 0) n += (size_t)snprintf(meta + n, sizeof meta - n, " · %.1f/10", rating);
    if (off[0] && n < sizeof meta) n += (size_t)snprintf(meta + n, sizeof meta - n, " · %s", off);
    cJSON *g = cJSON_GetObjectItem(o, "Genres");
    for (int i = 0; g && i < 2 && i < cJSON_GetArraySize(g) && n < sizeof meta; ++i) {
        cJSON *v = cJSON_GetArrayItem(g, i);
        if (cJSON_IsString(v))
            n += (size_t)snprintf(meta + n, sizeof meta - n, "%s%s", i ? ", " : " · ", v->valuestring);
    }
    /* the Details block on the shelves: the rating and the age rating as pills, the rest as one line */
    if (rating > 0) snprintf(it->rating, sizeof it->rating, "%.1f", rating);
    snprintf(it->cert, sizeof it->cert, "%s", off);
    {
        size_t b = 0;
        it->facts[0] = '\0';
        const int year = (int)jnum(o, "ProductionYear");
        if (year > 0) b += (size_t)snprintf(it->facts + b, sizeof it->facts - b, "%d", year);
        const int mins = (int)(jnum(o, "RunTimeTicks") / 600000000.0);
        if (mins >= 60 && mins < 1000)
            b += (size_t)snprintf(it->facts + b, sizeof it->facts - b, "%s%dh %02dm", b ? "  \xC2\xB7  " : "", mins / 60, mins % 60);
        else if (mins > 0 && mins < 60)
            b += (size_t)snprintf(it->facts + b, sizeof it->facts - b, "%s%dm", b ? "  \xC2\xB7  " : "", mins);
        for (int i = 0; g && i < 3 && i < cJSON_GetArraySize(g); ++i) {
            cJSON *v = cJSON_GetArrayItem(g, i);
            if (cJSON_IsString(v))
                b += (size_t)snprintf(it->facts + b, sizeof it->facts - b, "%s%s", i ? ", " : (b ? "  \xC2\xB7  " : ""), v->valuestring);
        }
    }
    size_t have = strlen(it->subtitle);
    if (meta[0] && have + strlen(meta) < sizeof it->subtitle)
        snprintf(it->subtitle + have, sizeof it->subtitle - have, "%s", meta);

    cJSON *tl = cJSON_GetObjectItem(o, "Taglines");
    cJSON *t0 = tl ? cJSON_GetArrayItem(tl, 0) : NULL;
    if (t0 && cJSON_IsString(t0) && t0->valuestring[0] && jstr(o, "Overview")[0]) {
        char buf[sizeof it->overview];
        snprintf(buf, sizeof buf, "%s - %s", t0->valuestring, it->overview);
        snprintf(it->overview, sizeof it->overview, "%s", buf);
    }
}

typedef struct {
    ms_client_t *c;
    evo_provider_items_cb cb;
    void *ud;
    int  ctx;
    int  start;                 /* StartIndex of this request */
    int  root;                  /* a Views reply: prepend the virtual rows */
} items_ctx_t;

static void add_virtual(evo_provider_item_t *it, const char *id, const char *title,
                        const char *sub)
{
    evo_provider_item_clear(it);
    snprintf(it->id, sizeof it->id, "%s", id);
    snprintf(it->title, sizeof it->title, "%s", title);
    snprintf(it->subtitle, sizeof it->subtitle, "%s", sub);
    it->is_folder = 1;
    it->kind = EVO_MEDIA_FOLDER;
}

static void on_items(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    items_ctx_t *x = (items_ctx_t *)ud;
    ms_client_t *c = x->c;

    if (status == 401 || status == 403) {
        /* The session is gone (token revoked, server reset): sign in again.
         * Whatever evo_net calls `ok`, the status is the server's verdict. */
        PROV_WARN("mediasrv: session rejected (http %d) - sign-in needed", status);
        ms_drop_session(c);
    }
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    /* Seasons and episodes come back as {"Items": [...]}, like everything
     * else; a bare array is tolerated in case a server version differs. */
    cJSON *arr = root ? (cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "Items")) : NULL;
    if (!arr || !cJSON_IsArray(arr)) {
        PROV_ERR("%s: item list failed ok=%d http=%d json=%d", MS_TAG(c), ok, status, root != NULL);
        if (x->cb) x->cb(0, NULL, 0, 0, x->ud);
        if (root) cJSON_Delete(root);
        free(x);
        return;
    }

    int n = cJSON_GetArraySize(arr);
    int extra = x->root ? 4 : 0;
    if (n + extra > EVO_PROVIDER_PAGE_MAX) n = EVO_PROVIDER_PAGE_MAX - extra;
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)(n + extra) + 1, sizeof *items);
    if (!items) {
        if (x->cb) x->cb(0, NULL, 0, 0, x->ud);
        cJSON_Delete(root);
        free(x);
        return;
    }

    int k = 0;
    if (x->root) {
        add_virtual(&items[k++], "v:resume", "Continue Watching", "Pick up where you left off");
        add_virtual(&items[k++], "v:nextup", "Next Up", "The next episode of your shows");
        add_virtual(&items[k++], "v:latest", "Recently Added", "New in your libraries");
        add_virtual(&items[k++], "v:favs", "Favourites", "Items you starred");
    }
    for (int i = 0; i < n; ++i) {
        cJSON *o = cJSON_GetArrayItem(arr, i);
        if (!o || !jstr(o, "Id")[0]) continue;
        if (!x->root && (!strcmp(jstr(o, "Type"), "Photo") || !strcmp(jstr(o, "Type"), "Book"))) continue;
        if (x->root) {
            /* A view: lib:<CollectionType>:<id>. */
            const char *ct = jstr(o, "CollectionType");
            evo_provider_item_clear(&items[k]);
            snprintf(items[k].id, sizeof items[k].id, "lib:%s:%s", ct[0] ? ct : "folder", jstr(o, "Id"));
            snprintf(items[k].title, sizeof items[k].title, "%s", jstr(o, "Name"));
            snprintf(items[k].subtitle, sizeof items[k].subtitle, "%s",
                     !strcmp(ct, "movies")  ? "Movies" :
                     !strcmp(ct, "tvshows") ? "TV Shows" :
                     !strcmp(ct, "music")   ? "Music" :
                     !strcmp(ct, "livetv")  ? "Live TV" :
                     !strcmp(ct, "photos") || !strcmp(ct, "homevideos") ? "Photos (not playable yet)" : "Library");
            items[k].is_folder = 1;
            items[k].kind = EVO_MEDIA_FOLDER;
            k++;
        } else {
            map_item(c, o, x->ctx, &items[k++]);
        }
    }

    int total = (int)jnum(root, "TotalRecordCount");
    int has_more = !x->root && total > 0 && x->start + n < total;
    if (x->cb) x->cb(1, items, k, has_more, x->ud);
    free(items);
    cJSON_Delete(root);
    free(x);
}

static int items_request(ms_client_t *c, const char *path, int ctx, int start, int root,
                         evo_provider_items_cb cb, void *ud)
{
    items_ctx_t *x = (items_ctx_t *)calloc(1, sizeof *x);
    if (!x) return -2;
    x->c = c; x->cb = cb; x->ud = ud; x->ctx = ctx; x->start = start; x->root = root;
    int rc = ms_request(c, "GET", path, NULL, 1, on_items, x);
    if (rc != 0) free(x);
    return rc;
}

int ms_list_catalog(ms_client_t *c, const char *parent_id, int page,
                    evo_provider_items_cb cb, void *ud)
{
    if (!c->cfg.is_connected) return -1;
    const char *uid = c->cfg.user_id;
    const int start = page > 0 ? page * MS_PAGE : 0;
    /* A home shelf shows a couple of dozen posters: ask for that many, not a
     * whole page (a page of 96 with overviews was half a megabyte). */
    const int hint = evo_provider_row_limit();
    const int limit = (page == 0 && hint > 0 && hint < MS_PAGE) ? hint : MS_PAGE;
    const int few = limit < 48 ? limit : 48;
    char path[1024];

    if (!parent_id || !*parent_id) {
        if (page > 0) return -1;
        snprintf(path, sizeof path, "/Users/%s/Views", uid);
        return items_request(c, path, CTX_LIST, 0, 1, cb, ud);
    }
    if (!strcmp(parent_id, "v:resume")) {
        snprintf(path, sizeof path,
                 "/Users/%s/Items/Resume?MediaTypes=Video&StartIndex=%d&Limit=%d&%s",
                 uid, start, limit, MS_FIELDS);
        return items_request(c, path, CTX_MIXED, start, 0, cb, ud);
    }
    if (!strcmp(parent_id, "v:nextup")) {
        snprintf(path, sizeof path, "/Shows/NextUp?UserId=%s&StartIndex=%d&Limit=%d&%s",
                 uid, start, limit, MS_FIELDS);
        return items_request(c, path, CTX_MIXED, start, 0, cb, ud);
    }
    if (!strncmp(parent_id, "sim:", 4)) {
        const char *sid = strrchr(parent_id + 4, ':');
        sid = sid ? sid + 1 : parent_id + 4;
        snprintf(path, sizeof path, "/Items/%s/Similar?UserId=%s&Limit=%d&%s", sid, uid, few, MS_FIELDS);
        return items_request(c, path, CTX_MIXED, 0, 0, cb, ud);
    }
    if (!strcmp(parent_id, "v:latest")) {
        if (page > 0) return -1;
        snprintf(path, sizeof path, "/Users/%s/Items/Latest?Limit=%d&GroupItems=true&%s",
                 uid, few, MS_FIELDS);
        return items_request(c, path, CTX_MIXED, 0, 0, cb, ud);
    }
    if (!strcmp(parent_id, "v:favs")) {
        snprintf(path, sizeof path,
                 "/Users/%s/Items?Filters=IsFavorite&Recursive=true"
                 "&IncludeItemTypes=Movie,Series,Episode&SortBy=SortName&SortOrder=Ascending"
                 "&StartIndex=%d&Limit=%d&%s",
                 uid, start, limit, MS_FIELDS);
        return items_request(c, path, CTX_MIXED, start, 0, cb, ud);
    }
    if (!strncmp(parent_id, "lib:", 4)) {
        char type[32] = "", id[96] = "";
        const char *colon = strchr(parent_id + 4, ':');
        if (!colon) return -1;
        snprintf(type, sizeof type, "%.*s", (int)(colon - (parent_id + 4)), parent_id + 4);
        snprintf(id, sizeof id, "%s", colon + 1);
        if (!strcmp(type, "livetv")) {
            snprintf(path, sizeof path,
                     "/LiveTv/Channels?UserId=%s&AddCurrentProgram=true&StartIndex=%d&Limit=%d&%s",
                     uid, start, limit, MS_FIELDS);
            return items_request(c, path, CTX_LIST, start, 0, cb, ud);
        }
        const char *include = !strcmp(type, "movies")  ? "&Recursive=true&IncludeItemTypes=Movie" :
                              !strcmp(type, "tvshows") ? "&Recursive=true&IncludeItemTypes=Series" :
                              !strcmp(type, "music")   ? "&Recursive=true&IncludeItemTypes=MusicAlbum" : "";
        static const char *const sorts[] = { "SortBy=SortName&SortOrder=Ascending",
                                             "SortBy=DateCreated,SortName&SortOrder=Descending",
                                             "SortBy=ProductionYear,SortName&SortOrder=Descending",
                                             "SortBy=CommunityRating,SortName&SortOrder=Descending" };
        static const char *const filters[] = { "", "&Filters=IsUnplayed", "&Filters=IsPlayed",
                                               "&Filters=IsFavorite" };
        char genre[160] = "";
        char years[96] = "";
        const int yi = c->years % MS_YEAR_COUNT;
        if (yi > 0) {   /* Years= takes a comma list on both Emby and Jellyfin */
            int p = snprintf(years, sizeof years, "&Years=");
            for (int y = ms_year_from[yi]; y <= ms_year_to[yi] && p < (int)sizeof years - 6; y++)
                p += snprintf(years + p, sizeof years - p, y == ms_year_from[yi] ? "%d" : ",%d", y);
        }
        char rating[40] = "";
        if (c->rating % MS_RATING_COUNT)
            snprintf(rating, sizeof rating, "&MinCommunityRating=%d", ms_rating_min[c->rating % MS_RATING_COUNT]);
        if (c->genre[0]) {
            char esc[120];
            if (evo_provider_url_escape(c->genre, esc, sizeof esc) >= 0)
                snprintf(genre, sizeof genre, "&Genres=%s", esc);
        }
        snprintf(path, sizeof path,
                 "/Users/%s/Items?ParentId=%s%s&%s%s%s%s%s&StartIndex=%d&Limit=%d&%s",
                 uid, id, include, sorts[c->sort % 4], filters[c->filter % MS_FILTER_COUNT], genre,
                 years, rating,
                 start, limit, MS_FIELDS);
        return items_request(c, path, CTX_LIST, start, 0, cb, ud);
    }
    if (!strncmp(parent_id, "ser:", 4)) {
        snprintf(path, sizeof path, "/Shows/%s/Seasons?UserId=%s&%s",
                 parent_id + 4, uid, MS_FIELDS);
        return items_request(c, path, CTX_LIST, 0, 0, cb, ud);
    }
    if (!strncmp(parent_id, "sea:", 4)) {
        char series[96];
        const char *colon = strchr(parent_id + 4, ':');
        if (!colon) return -1;
        snprintf(series, sizeof series, "%.*s", (int)(colon - (parent_id + 4)), parent_id + 4);
        snprintf(path, sizeof path, "/Shows/%s/Episodes?SeasonId=%s&UserId=%s&%s",
                 series, colon + 1, uid, MS_FIELDS);
        return items_request(c, path, CTX_SEASON, 0, 0, cb, ud);
    }
    if (!strncmp(parent_id, "fol:", 4)) {
        snprintf(path, sizeof path,
                 "/Users/%s/Items?ParentId=%s&SortBy=ParentIndexNumber,IndexNumber,SortName&SortOrder=Ascending"
                 "&StartIndex=%d&Limit=%d&%s",
                 uid, parent_id + 4, start, limit, MS_FIELDS);
        return items_request(c, path, CTX_LIST, start, 0, cb, ud);
    }
    return -1;
}

int ms_search(ms_client_t *c, const char *query, int page,
              evo_provider_items_cb cb, void *ud)
{
    if (!c->cfg.is_connected || !query || !*query) return -1;
    char esc[256], path[1024];
    if (evo_provider_url_escape(query, esc, sizeof esc) < 0) return -1;
    const int start = page > 0 ? page * MS_PAGE : 0;
    snprintf(path, sizeof path,
             "/Users/%s/Items?SearchTerm=%s&Recursive=true&IncludeItemTypes=Movie,Series,Episode"
             "&StartIndex=%d&Limit=%d&%s",
             c->cfg.user_id, esc, start, MS_PAGE, MS_FIELDS);
    return items_request(c, path, CTX_MIXED, start, 0, cb, ud);
}

/* ------------------------------------------------------------------------- */
/* Item actions                                                              */
/* ------------------------------------------------------------------------- */

typedef struct { evo_provider_done_cb cb; void *ud; } act_ctx_t;

static void on_action(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    act_ctx_t *x = (act_ctx_t *)ud;
    const int good = ok && status >= 200 && status < 300;
    if (!good) PROV_ERR("mediasrv: action -> ok=%d http=%d %.120s", ok, status, body ? body : "");
    if (x->cb) x->cb(good, NULL, x->ud);
    free(x);
}

/* The server's own id from a catalog id: i:<id>, ser:<id>, fol:<id>, sea:<series>:<season>. */
static const char *item_server_id(const char *id)
{
    const char *p = strrchr(id, ':');
    return p ? p + 1 : id;
}

int ms_item_action(ms_client_t *c, const char *item_id, const char *action,
                   evo_provider_done_cb cb, void *ud)
{
    if (!c->cfg.is_connected || !item_id || !action) return -1;
    if (!strcmp(action, "transcode")) {
        c->cfg.transcode = !c->cfg.transcode;
        ms_save(c);
        if (cb) cb(1, c->cfg.transcode ? "Server transcoding: always"
                                       : "Server transcoding: only when a file will not play", ud);
        return 0;
    }
    {
        static const char *const sort_names[] = { "Name", "Date added", "Release year", "Rating" };
        static char msg[96];
        if (!strcmp(action, "sort")) {
            c->sort = (c->sort + 1) % 4;
            snprintf(msg, sizeof msg, "Sorted by %s", sort_names[c->sort]);
            if (cb) cb(1, msg, ud);
            return 0;
        }
        if (!strcmp(action, "filter")) {
            c->filter = (c->filter + 1) % MS_FILTER_COUNT;
            snprintf(msg, sizeof msg, "Showing %s", ms_filter_names[c->filter]);
            if (cb) cb(1, msg, ud);
            return 0;
        }
        if (!strcmp(action, "years")) {
            c->years = (c->years + 1) % MS_YEAR_COUNT;
            snprintf(msg, sizeof msg, c->years ? "Years: %s" : "Any year", ms_year_names[c->years]);
            if (cb) cb(1, msg, ud);
            return 0;
        }
        if (!strcmp(action, "rating")) {
            c->rating = (c->rating + 1) % MS_RATING_COUNT;
            snprintf(msg, sizeof msg, c->rating ? "Rating: %s" : "Any rating", ms_rating_names[c->rating]);
            if (cb) cb(1, msg, ud);
            return 0;
        }
        if (!strncmp(action, "genre:", 6)) {
            snprintf(c->genre, sizeof c->genre, "%s", action + 6);
            snprintf(msg, sizeof msg, c->genre[0] ? "Genre: %s" : "Any genre", c->genre);
            if (cb) cb(1, msg, ud);
            return 0;
        }
    }
    const char *id = item_server_id(item_id);
    if (!*id || !strcmp(item_id, "v:") || !strncmp(item_id, "v:", 2) || !strncmp(item_id, "lib:", 4))
        return -1;
    const int played = !strcmp(action, "played") || !strcmp(action, "unplayed");
    const int on     = !strcmp(action, "played") || !strcmp(action, "fav");
    if (!played && strcmp(action, "fav") && strcmp(action, "unfav")) return -1;

    char path[512];
    if (c->kind == MS_JELLYFIN)
        snprintf(path, sizeof path, "/%s/%s?userId=%s", played ? "UserPlayedItems" : "UserFavoriteItems",
                 id, c->cfg.user_id);
    else
        snprintf(path, sizeof path, "/Users/%s/%s/%s", c->cfg.user_id,
                 played ? "PlayedItems" : "FavoriteItems", id);

    act_ctx_t *x = (act_ctx_t *)calloc(1, sizeof *x);
    if (!x) return -2;
    x->cb = cb; x->ud = ud;
    int rc = ms_request(c, on ? "POST" : "DELETE", path, NULL, 1, on_action, x);
    if (rc != 0) free(x);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Side-panel rows: details and cast, browse options, genres                  */
/* ------------------------------------------------------------------------- */

typedef void (*ms_request_fn_t)(int ok, int status, const char *body, size_t len, void *ud);
typedef struct { evo_provider_rows_cb cb; void *ud; char base[200]; } rows_ctx_t;

static void row_add(evo_provider_row_t *rows, int *n, int max, const char *id,
                    const char *title, const char *detail)
{
    if (*n >= max) return;
    evo_provider_row_t *r = &rows[(*n)++];
    memset(r, 0, sizeof *r);
    snprintf(r->id, sizeof r->id, "%s", id);
    snprintf(r->title, sizeof r->title, "%s", title);
    snprintf(r->detail, sizeof r->detail, "%s", detail);
}

static void on_details(int ok, int status, const char *body, size_t len, void *ud)
{
    rows_ctx_t *x = (rows_ctx_t *)ud;
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    /* /Items?Ids= answers {"Items":[item]}; an older server's /Users/../Items/id the item itself. */
    cJSON *its = root ? cJSON_GetObjectItem(root, "Items") : NULL;
    cJSON *o = (its && cJSON_IsArray(its)) ? cJSON_GetArrayItem(its, 0) : root;
    evo_provider_row_t rows[20];
    int n = 0;
    PROV_LOG("mediasrv: details -> ok=%d http=%d len=%zu parsed=%d", ok, status, len, o != NULL);
    if (o) {
        char buf[160];
        cJSON *tl = cJSON_GetObjectItem(o, "Taglines");
        cJSON *t0 = tl ? cJSON_GetArrayItem(tl, 0) : NULL;
        if (t0 && cJSON_IsString(t0) && t0->valuestring[0]) row_add(rows, &n, 20, "", "Tagline", t0->valuestring);

        size_t b = 0;
        buf[0] = '\0';
        double cr = jnum(o, "CommunityRating"), crit = jnum(o, "CriticRating");
        if (cr > 0)   b += (size_t)snprintf(buf + b, sizeof buf - b, "%.1f/10 viewers", cr);
        if (crit > 0) b += (size_t)snprintf(buf + b, sizeof buf - b, "%s%.0f%% critics", b ? " · " : "", crit);
        if (jstr(o, "OfficialRating")[0])
            b += (size_t)snprintf(buf + b, sizeof buf - b, "%s%s", b ? " · " : "", jstr(o, "OfficialRating"));
        if (b) row_add(rows, &n, 20, "", "Rating", buf);

        cJSON *g = cJSON_GetObjectItem(o, "Genres");
        b = 0; buf[0] = '\0';
        for (int i = 0; g && i < cJSON_GetArraySize(g) && b + 2 < sizeof buf; ++i) {
            cJSON *v = cJSON_GetArrayItem(g, i);
            if (cJSON_IsString(v)) b += (size_t)snprintf(buf + b, sizeof buf - b, "%s%s", b ? ", " : "", v->valuestring);
        }
        if (b) row_add(rows, &n, 20, "", "Genres", buf);

        cJSON *st = cJSON_GetObjectItem(o, "Studios");
        b = 0; buf[0] = '\0';
        for (int i = 0; st && i < 2 && i < cJSON_GetArraySize(st) && b + 2 < sizeof buf; ++i)
            b += (size_t)snprintf(buf + b, sizeof buf - b, "%s%s", b ? ", " : "", jstr(cJSON_GetArrayItem(st, i), "Name"));
        if (b) row_add(rows, &n, 20, "", "Studio", buf);

        cJSON *ppl = cJSON_GetObjectItem(o, "People");
        b = 0; buf[0] = '\0';
        for (int i = 0; ppl && i < cJSON_GetArraySize(ppl) && b + 2 < sizeof buf; ++i) {
            cJSON *p = cJSON_GetArrayItem(ppl, i);
            if (!strcmp(jstr(p, "Type"), "Director"))
                b += (size_t)snprintf(buf + b, sizeof buf - b, "%s%s", b ? ", " : "", jstr(p, "Name"));
        }
        if (b) row_add(rows, &n, 20, "", "Director", buf);

        int cast = 0;
        for (int i = 0; ppl && i < cJSON_GetArraySize(ppl) && cast < 12; ++i) {
            cJSON *p = cJSON_GetArrayItem(ppl, i);
            if (strcmp(jstr(p, "Type"), "Actor")) continue;
            int prev = n;
            row_add(rows, &n, 20, "", jstr(p, "Name"), jstr(p, "Role"));
            if (n > prev) {
                snprintf(rows[n - 1].group, sizeof rows[n - 1].group, "cast");
                if (jstr(p, "PrimaryImageTag")[0] && jstr(p, "Id")[0])
                    snprintf(rows[n - 1].art_url, sizeof rows[n - 1].art_url,
                             "%s/Items/%s/Images/Primary?maxHeight=120&quality=80&format=jpg", x->base, jstr(p, "Id"));
                ++cast;
            }
        }
    }
    PROV_LOG("mediasrv: details: %d row(s)", n);
    if (x->cb) x->cb(o != NULL, rows, n, x->ud);
    if (root) cJSON_Delete(root);
    free(x);
}

static void on_genres(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    rows_ctx_t *x = (rows_ctx_t *)ud;
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    cJSON *arr = root ? cJSON_GetObjectItem(root, "Items") : NULL;
    evo_provider_row_t rows[16];
    int n = 0;
    row_add(rows, &n, 16, "genre:", "Any genre", "Show everything");
    for (int i = 0; arr && i < cJSON_GetArraySize(arr) && n < 16; ++i) {
        const char *name = jstr(cJSON_GetArrayItem(arr, i), "Name");
        char id[64];
        snprintf(id, sizeof id, "genre:%s", name);
        if (name[0]) row_add(rows, &n, 16, id, name, "");
    }
    if (x->cb) x->cb(root != NULL, rows, n, x->ud);
    if (root) cJSON_Delete(root);
    free(x);
}

int ms_panel(ms_client_t *c, const char *item_id, const char *kind, evo_provider_rows_cb cb, void *ud)
{
    if (!c->cfg.is_connected || !item_id || !kind) return -1;
    if (!strcmp(kind, "browse")) {
        static const char *const sort_names[] = { "Name", "Date added", "Release year", "Rating" };
        evo_provider_row_t rows[5];
        int n = 0;
        row_add(rows, &n, 5, "sort", "Sort by", sort_names[c->sort % 4]);
        row_add(rows, &n, 5, "filter", "Show", ms_filter_names[c->filter % MS_FILTER_COUNT]);
        row_add(rows, &n, 5, "panel:genres", "Genre", c->genre[0] ? c->genre : "Any");
        row_add(rows, &n, 5, "years", "Years", ms_year_names[c->years % MS_YEAR_COUNT]);
        row_add(rows, &n, 5, "rating", "Rating", ms_rating_names[c->rating % MS_RATING_COUNT]);
        if (cb) cb(1, rows, n, ud);
        return 0;
    }
    rows_ctx_t *x = (rows_ctx_t *)calloc(1, sizeof *x);
    if (!x) return -2;
    x->cb = cb; x->ud = ud;
    ms_base(c, x->base, sizeof x->base);
    char path[400];
    ms_request_fn_t fn = NULL;
    if (!strcmp(kind, "details")) {
        snprintf(path, sizeof path, "/Items?Ids=%s&UserId=%s&Fields=People,Genres,Studios,Taglines,CommunityRating,OfficialRating",
                 item_server_id(item_id), c->cfg.user_id);
        fn = on_details;
    } else if (!strcmp(kind, "genres")) {
        const char *lib = strrchr(item_id, ':');
        snprintf(path, sizeof path, "/Genres?UserId=%s&ParentId=%s&SortBy=SortName&Recursive=true",
                 c->cfg.user_id, lib ? lib + 1 : item_id);
        fn = on_genres;
    } else {
        free(x);
        return -1;
    }
    int rc = ms_request(c, "GET", path, NULL, 1, fn, x);
    if (rc != 0) free(x);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Segments and the next episode                                             */
/* ------------------------------------------------------------------------- */

typedef struct { evo_provider_segments_cb cb; void *ud; int jellyfin; } seg_ctx_t;

static void on_segments(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    seg_ctx_t *x = (seg_ctx_t *)ud;
    evo_provider_segment_t segs[8];
    int n = 0;
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    if (root && x->jellyfin) {
        cJSON *arr = cJSON_GetObjectItem(root, "Items");
        for (int i = 0; arr && i < cJSON_GetArraySize(arr) && n < 8; ++i) {
            cJSON *o = cJSON_GetArrayItem(arr, i);
            const char *t = jstr(o, "Type");
            int kind = (!strcmp(t, "Intro") || !strcmp(t, "Recap")) ? EVO_SEGMENT_INTRO :
                       !strcmp(t, "Outro") ? EVO_SEGMENT_OUTRO : 0;
            if (!kind) continue;
            segs[n].kind = kind;
            segs[n].start_sec = jnum(o, "StartTicks") / 10000000.0;
            segs[n].end_sec = jnum(o, "EndTicks") / 10000000.0;
            if (segs[n].end_sec > segs[n].start_sec) ++n;
        }
    } else if (root) {
        /* Emby: chapter markers. IntroStart..IntroEnd, CreditsStart..end. */
        cJSON *ch = cJSON_GetObjectItem(root, "Chapters");
        double intro_start = -1;
        for (int i = 0; ch && i < cJSON_GetArraySize(ch) && n < 8; ++i) {
            cJSON *o = cJSON_GetArrayItem(ch, i);
            const char *m = jstr(o, "MarkerType");
            double t = jnum(o, "StartPositionTicks") / 10000000.0;
            if (!strcmp(m, "IntroStart")) intro_start = t;
            else if (!strcmp(m, "IntroEnd") && intro_start >= 0 && t > intro_start) {
                segs[n].kind = EVO_SEGMENT_INTRO; segs[n].start_sec = intro_start; segs[n].end_sec = t; ++n;
                intro_start = -1;
            } else if (!strcmp(m, "CreditsStart")) {
                segs[n].kind = EVO_SEGMENT_OUTRO; segs[n].start_sec = t; segs[n].end_sec = 1e9; ++n;
            }
        }
    }
    if (x->cb) x->cb(root != NULL, segs, n, x->ud);
    if (root) cJSON_Delete(root);
    free(x);
}

int ms_segments(ms_client_t *c, const char *item_id, evo_provider_segments_cb cb, void *ud)
{
    if (!c->cfg.is_connected || !item_id || strncmp(item_id, "i:", 2)) return -1;
    seg_ctx_t *x = (seg_ctx_t *)calloc(1, sizeof *x);
    if (!x) return -2;
    x->cb = cb; x->ud = ud; x->jellyfin = c->kind == MS_JELLYFIN;
    char path[400];
    if (x->jellyfin)
        snprintf(path, sizeof path,
                 "/MediaSegments/%s?includeSegmentTypes=Intro&includeSegmentTypes=Outro&includeSegmentTypes=Recap",
                 item_id + 2);
    else
        snprintf(path, sizeof path, "/Users/%s/Items/%s?Fields=Chapters", c->cfg.user_id, item_id + 2);
    int rc = ms_request(c, "GET", path, NULL, 1, on_segments, x);
    if (rc != 0) free(x);
    return rc;
}

typedef struct { ms_client_t *c; evo_provider_next_cb cb; void *ud; char item[96]; } next_ctx_t;

static void on_next_list(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    next_ctx_t *x = (next_ctx_t *)ud;
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    cJSON *arr = root ? cJSON_GetObjectItem(root, "Items") : NULL;
    int done = 0;
    /* StartItemId lists the item itself first; the one after it is next. */
    for (int i = 0; arr && i < cJSON_GetArraySize(arr); ++i) {
        cJSON *o = cJSON_GetArrayItem(arr, i);
        if (!strcmp(jstr(o, "Id"), x->item)) {
            cJSON *nx = cJSON_GetArrayItem(arr, i + 1);
            if (nx && jstr(nx, "Id")[0]) {
                char id[EVO_PROVIDER_MAX_ITEM_ID], title[160];
                snprintf(id, sizeof id, "i:%s", jstr(nx, "Id"));
                snprintf(title, sizeof title, "S%d E%d · %s", (int)jnum(nx, "ParentIndexNumber"),
                         (int)jnum(nx, "IndexNumber"), jstr(nx, "Name"));
                if (x->cb) x->cb(1, id, title, x->ud);
                done = 1;
            }
            break;
        }
    }
    if (!done && x->cb) x->cb(0, NULL, NULL, x->ud);
    if (root) cJSON_Delete(root);
    free(x);
}

static void on_next_item(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    next_ctx_t *x = (next_ctx_t *)ud;
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    const char *series = root && !strcmp(jstr(root, "Type"), "Episode") ? jstr(root, "SeriesId") : "";
    if (series[0]) {
        char path[512];
        snprintf(path, sizeof path, "/Shows/%s/Episodes?UserId=%s&StartItemId=%s&Limit=2",
                 series, x->c->cfg.user_id, x->item);
        cJSON_Delete(root);
        if (ms_request(x->c, "GET", path, NULL, 1, on_next_list, x) == 0) return;
        if (x->cb) x->cb(0, NULL, NULL, x->ud);
        free(x);
        return;
    }
    if (root) cJSON_Delete(root);
    if (x->cb) x->cb(0, NULL, NULL, x->ud);
    free(x);
}

int ms_next_item(ms_client_t *c, const char *item_id, evo_provider_next_cb cb, void *ud)
{
    if (!c->cfg.is_connected || !item_id || strncmp(item_id, "i:", 2)) return -1;
    next_ctx_t *x = (next_ctx_t *)calloc(1, sizeof *x);
    if (!x) return -2;
    x->c = c; x->cb = cb; x->ud = ud;
    snprintf(x->item, sizeof x->item, "%s", item_id + 2);
    char path[320];
    snprintf(path, sizeof path, "/Users/%s/Items/%s", c->cfg.user_id, x->item);
    int rc = ms_request(c, "GET", path, NULL, 1, on_next_item, x);
    if (rc != 0) free(x);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Resolve + progress                                                        */
/* ------------------------------------------------------------------------- */

static void url_param(const char *url, const char *key, char *out, size_t cap);

static const char *raw_id(const char *item_id)
{
    if (!item_id) return item_id;
    if (!strncmp(item_id, "i:", 2) || !strncmp(item_id, "a:", 2)) return item_id + 2;
    if (!strncmp(item_id, "tv:", 3)) return item_id + 3;
    return item_id;
}

int ms_build_stream_url(ms_client_t *c, const char *item_id, char *out, size_t cap)
{
    if (!item_id || !out || cap == 0) return -1;
    char base[200];
    ms_base(c, base, sizeof base);
    snprintf(out, cap, "%s/%s/%s/stream?Static=true&api_key=%s", base,
             !strncmp(item_id, "a:", 2) ? "Audio" : "Videos", raw_id(item_id), c->cfg.token);
    return 0;
}

/*
 * VERSIONS (#116)
 *
 * One item can have several MediaSources: a 4K and a 1080p file of the same
 * movie, or - behind AIOStreams - a dozen releases. /Items/{id}/PlaybackInfo
 * lists them; each becomes one choice whose stream URL names its
 * MediaSourceId. Without that parameter the server plays the first source.
 * Anything that goes wrong on the way falls back to the plain direct stream,
 * which is what EVO always played.
 */
#define MS_MAX_SOURCES 12

static void video_range(cJSON *vs, char *out, size_t cap)
{
    const char *t   = jstr(vs, "VideoRangeType");     /* Jellyfin 10.9+   */
    const char *ext = jstr(vs, "ExtendedVideoType");  /* Emby 4.8+        */
    const char *r   = jstr(vs, "VideoRange");         /* both: SDR / HDR  */
    const char *s   = "";
    if (!strncmp(t, "DOVI", 4) || !strcmp(ext, "DolbyVision") || jstr(vs, "VideoDoViTitle")[0])
        s = "Dolby Vision";
    else if (!strcmp(t, "HDR10Plus") || !strcmp(ext, "Hdr10Plus")) s = "HDR10+";
    else if (!strcmp(t, "HDR10")     || !strcmp(ext, "Hdr10"))     s = "HDR10";
    else if (!strcmp(t, "HLG")       || !strcmp(ext, "HyperLogGamma")) s = "HLG";
    else if (!strcmp(r, "HDR"))                                     s = "HDR";
    else if (!strcmp(t, "SDR") || !strcmp(r, "SDR"))                s = "SDR";
    snprintf(out, cap, "%s", s);
}

/* One UTF-8 sequence at `s`: its code point and length; -1 for a bad byte. */
static long utf8_next(const unsigned char *s, int *len)
{
    if (s[0] < 0x80) { *len = 1; return s[0]; }
    int n = (s[0] & 0xE0) == 0xC0 ? 2 : (s[0] & 0xF0) == 0xE0 ? 3 : (s[0] & 0xF8) == 0xF0 ? 4 : 0;
    if (!n) { *len = 1; return -1; }
    long cp = s[0] & (0x7F >> n);
    for (int i = 1; i < n; ++i) {
        if ((s[i] & 0xC0) != 0x80) { *len = i; return -1; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *len = n;
    return cp;
}

/*
 * A source name as a picker label (#116). AIOStreams names are multi-line
 * blurbs padded with zero-width joiners and emoji ("4K ‍⚡ | 〈Web-dl〉"):
 * the font has none of those glyphs, and a byte cut could split a character.
 * Lines become " | " segments, invisible / symbol / emoji code points go,
 * angle brackets become [ ], spaces collapse, and only whole characters fit.
 */
/* Styled letters and digits that carry real information ("s₀₁·ᴇ₀₈", "15.4 ᴹᵇᵖˢ")
 * and have a plain ASCII twin the font does have: subscript / superscript digits,
 * small capitals and modifier letters. 0 = no twin. */
static char label_fold(long cp)
{
    static const struct { long from; const char *to; } runs[] = {
        { 0x2080, "0123456789" },                 /* subscript digits      */
        { 0x2074, "456789" },                     /* superscript 4..9      */
        { 0x1D2C, "A" }, { 0x1D2E, "B" }, { 0x1D30, "DEFG" }, { 0x1D34, "HIJKLMN" },
        { 0x1D3C, "O" }, { 0x1D3E, "PR" }, { 0x1D40, "TU" }, { 0x1D42, "W" },
        { 0x1D43, "a" }, { 0x1D47, "bde" }, { 0x1D4D, "g" }, { 0x1D4F, "km" }, { 0x1D52, "o" },
        { 0x1D56, "p" }, { 0x1D57, "tu" }, { 0x1D5B, "v" },
        { 0x1D00, "a" }, { 0x1D04, "cd" }, { 0x1D07, "e" }, { 0x1D0A, "jklm" },
        { 0x1D0F, "o" }, { 0x1D18, "p" }, { 0x1D1B, "tu" }, { 0x1D20, "vw" }, { 0x1D22, "z" },
        { 0x02B0, "h" }, { 0x02B2, "j" }, { 0x02B3, "r" }, { 0x02B7, "w" }, { 0x02B8, "y" },
        { 0x02E1, "l" }, { 0x02E2, "sx" }, { 0x2071, "i" }, { 0x207F, "n" },
        { 0x1D9C, "c" }, { 0x1DA0, "f" }, { 0x1DBB, "z" },
    };
    if (cp == 0x2070) return '0';
    if (cp == 0xB9) return '1';
    if (cp == 0xB2) return '2';
    if (cp == 0xB3) return '3';
    for (size_t i = 0; i < sizeof runs / sizeof runs[0]; ++i) {
        long d = cp - runs[i].from;
        if (d >= 0 && d < (long)strlen(runs[i].to)) return runs[i].to[d];
    }
    switch (cp) {                                  /* small capitals outside 1D00 */
    case 0x0262: return 'g'; case 0x029C: return 'h'; case 0x026A: return 'i';
    case 0x029F: return 'l'; case 0x0274: return 'n'; case 0x0280: return 'r';
    case 0x028F: return 'y'; case 0x0299: return 'b'; case 0xA730: return 'f';
    case 0xA731: return 's';
    case 0x2605: case 0x2B50: return '*';         /* star ratings           */
    default: return 0;
    }
}

static void fact_add(char *facts, size_t cap, const char *name, const char *value)
{
    size_t n = strlen(facts);
    if (!value[0]) return;
    snprintf(facts + n, cap - n, "%s\t%s\n", name, value);
}

/*
 * An AIOStreams source name, cleaned, reads "4K | [Web-dl] | ***** | Show s01e08 |
 * HEVC SDR DD+ 5.1 | 6.28 / 45 GB · 15.4 Mbps · 1d | [TB] Indexer · Group | en · sub (en)".
 * Split it into labelled facts for the picker's detail card and leave the title
 * as the row label. A name that does not look like that is left alone.
 */
static void label_facts(evo_stream_choice_t *ch)
{
    char buf[256];
    char *seg[12];
    int n = 0;
    snprintf(buf, sizeof buf, "%s", ch->label);
    for (char *p = buf; p && n < 12;) {
        seg[n++] = p;
        char *bar = strstr(p, " | ");
        if (bar) { *bar = '\0'; p = bar + 3; } else p = NULL;
    }
    size_t q = strlen(seg[0]);
    if (n < 4 || q > 6 || !(strchr(seg[0], 'K') || strchr(seg[0], 'p') || !strcmp(seg[0], "SD")))
        return;

    char *other[12];
    int no = 0;
    ch->facts[0] = '\0';
    fact_add(ch->facts, sizeof ch->facts, "Quality", seg[0]);
    for (int i = 1; i < n; ++i) {
        char *s = seg[i];
        size_t len = strlen(s);
        if (len > 2 && s[0] == '[' && s[len - 1] == ']' && !strchr(s, ' ')) {
            s[len - 1] = '\0';
            fact_add(ch->facts, sizeof ch->facts, "Source", s + 1);
        } else if (s[0] == '*' && s[strspn(s, "*")] == '\0') {
            char r[16];
            snprintf(r, sizeof r, "%d of 5", (int)strlen(s));
            fact_add(ch->facts, sizeof ch->facts, "Rating", r);
        } else if (strstr(s, " GB") || strstr(s, " MB")) {
            for (char *part = s; part;) {
                char *dot = strstr(part, " \xC2\xB7 ");       /* " · " */
                if (dot) *dot = '\0';
                fact_add(ch->facts, sizeof ch->facts,
                         (strstr(part, "GB") || strstr(part, "MB")) ? "Size"
                         : strstr(part, "bps") ? "Bitrate" : "Added", part);
                part = dot ? dot + 3 : NULL;
            }
        } else if (strstr(s, "HEVC") || strstr(s, "AVC") || strstr(s, "AV1") || strstr(s, "VP9") ||
                   strstr(s, "H264") || strstr(s, "x265") || strstr(s, "x264")) {
            fact_add(ch->facts, sizeof ch->facts, "Video and audio", s);
        } else if (no < 12) {
            other[no++] = s;
        }
    }
    /* What is left: the title first, then the release, then languages and the rest. */
    static const char *const names[] = { NULL, "Release", "Languages", "Details" };
    for (int i = 0; i < no; ++i) {
        if (i == 0) {
            snprintf(ch->label, sizeof ch->label, "%s", other[0]);
        } else if (i == 2 && strstr(other[i], " \xC2\xBB ")) {      /* "en · sub (en) » tags" */
            char *tags = strstr(other[i], " \xC2\xBB ");
            *tags = '\0';
            fact_add(ch->facts, sizeof ch->facts, "Languages", other[i]);
            fact_add(ch->facts, sizeof ch->facts, "Tags", tags + 4);
        } else {
            fact_add(ch->facts, sizeof ch->facts, names[i < 3 ? i : 3], other[i]);
        }
    }
}

static void label_clean(const char *in, char *out, size_t cap)
{
    char tmp[512];
    size_t t = 0;
    const unsigned char *s = (const unsigned char *)in;
    while (*s && t + 5 < sizeof tmp) {
        int len;
        long cp = utf8_next(s, &len);
        if (cp == '\n') tmp[t++] = '|';
        else if (cp == '\r' || cp == '\t' || cp == 0xA0 || (cp >= 0x2000 && cp <= 0x200A) ||
                 cp == 0x202F || cp == 0x205F || cp == 0x3000)
            tmp[t++] = ' ';                 /* AIOStreams pads with NBSP */
        else if (label_fold(cp)) tmp[t++] = label_fold(cp);
        else if (cp == 0x3008 || cp == 0x2329 || cp == 0x27E8) tmp[t++] = '[';
        else if (cp == 0x3009 || cp == 0x232A || cp == 0x27E9) tmp[t++] = ']';
        else if (cp < 0x20 || cp < 0 || (cp >= 0x2000 && cp < 0x2C00) ||
                 (cp >= 0x0250 && cp < 0x0300) ||       /* IPA, modifier letters  */
                 (cp >= 0x1D00 && cp < 0x1DC0) ||       /* small capitals         */
                 (cp >= 0xFE00 && cp < 0xFE10) || cp == 0xFEFF || cp >= 0x1F000) {
            /* control, invisible, symbol or emoji: dropped */
        } else {
            memcpy(tmp + t, s, (size_t)len);
            t += (size_t)len;
        }
        s += len;
    }
    tmp[t] = '\0';

    size_t o = 0;
    out[0] = '\0';
    for (char *seg = tmp; seg; ) {
        char *bar = strchr(seg, '|');
        if (bar) *bar = '\0';
        int pending_space = 0, wrote = 0;
        for (const unsigned char *p = (const unsigned char *)seg; *p; ) {
            if (*p == ' ') { pending_space = wrote; ++p; continue; }
            int len;
            (void)utf8_next(p, &len);
            const char *sep = (!wrote && o > 0) ? " | " : pending_space ? " " : "";
            size_t need = strlen(sep) + (size_t)len;
            if (o + need >= cap) { out[o] = '\0'; return; }
            memcpy(out + o, sep, strlen(sep));
            o += strlen(sep);
            memcpy(out + o, p, (size_t)len);
            o += (size_t)len;
            p += len;
            wrote = 1;
            pending_space = 0;
        }
        out[o] = '\0';
        seg = bar ? bar + 1 : NULL;
    }
}

static void source_url(ms_client_t *c, const char *item, const char *source,
                       const char *session, char *out, size_t cap)
{
    char base[200], src[200], ses[128];
    ms_base(c, base, sizeof base);
    if (evo_provider_url_escape(source, src, sizeof src) < 0) src[0] = '\0';
    if (!session || evo_provider_url_escape(session, ses, sizeof ses) < 0) ses[0] = '\0';
    snprintf(out, cap, "%s/Videos/%s/stream?Static=true&MediaSourceId=%s%s%s&api_key=%s",
             base, item, src, ses[0] ? "&PlaySessionId=" : "", ses, c->cfg.token);
}

int ms_parse_sources(ms_client_t *c, const char *item_id, const char *body,
                     evo_stream_choice_t *out, int max)
{
    if (!body || !out || max <= 0) return 0;
    cJSON *root = cJSON_Parse(body);
    cJSON *arr  = root ? cJSON_GetObjectItem(root, "MediaSources") : NULL;
    const char *session = jstr(root, "PlaySessionId");
    int n = 0;
    for (int i = 0; arr && cJSON_IsArray(arr) && i < cJSON_GetArraySize(arr) && n < max; ++i) {
        cJSON *ms = cJSON_GetArrayItem(arr, i);
        const char *id = jstr(ms, "Id");
        if (!id[0]) continue;
        evo_stream_choice_t *ch = &out[n];
        evo_provider_stream_choice_clear(ch);
        source_url(c, raw_id(item_id), id, session, ch->url, sizeof ch->url);
        snprintf(ch->container, sizeof ch->container, "%s", jstr(ms, "Container"));
        ch->size_bytes  = (int64_t)jnum(ms, "Size");
        ch->bitrate_bps = (int64_t)jnum(ms, "Bitrate");

        cJSON *streams = cJSON_GetObjectItem(ms, "MediaStreams");
        cJSON *vs = NULL, *as = NULL, *as_default = NULL, *as_first = NULL;
        cJSON *dai = cJSON_GetObjectItem(ms, "DefaultAudioStreamIndex");
        for (int k = 0; streams && k < cJSON_GetArraySize(streams); ++k) {
            cJSON *s = cJSON_GetArrayItem(streams, k);
            const char *type = jstr(s, "Type");
            if (!strcmp(type, "Video") && !vs) vs = s;
            else if (!strcmp(type, "Audio")) {
                if (!as_first) as_first = s;
                if (!as_default && jbool(s, "IsDefault")) as_default = s;
                if (dai && cJSON_IsNumber(dai) && (int)jnum(s, "Index") == dai->valueint) as = s;
            }
        }
        if (!as) as = as_default ? as_default : as_first;

        /* A sidecar text subtitle (.srt next to the file): the default one, else
         * English, else the first. The server converts it to SubRip. */
        {
            cJSON *best = NULL;
            int best_rank = 0;
            for (int k = 0; streams && k < cJSON_GetArraySize(streams); ++k) {
                cJSON *s = cJSON_GetArrayItem(streams, k);
                if (strcmp(jstr(s, "Type"), "Subtitle") || !jbool(s, "IsExternal")) continue;
                cJSON *txt = cJSON_GetObjectItem(s, "IsTextSubtitleStream");
                if (txt && cJSON_IsFalse(txt)) continue;
                const char *lang = jstr(s, "Language");
                int rank = jbool(s, "IsDefault") ? 3 : (!strcmp(lang, "eng") || !strcmp(lang, "en")) ? 2 : 1;
                if (rank > best_rank) { best = s; best_rank = rank; }
            }
            if (best) {
                char base[200];
                ms_base(c, base, sizeof base);
                if (c->kind == MS_JELLYFIN)
                    snprintf(ch->sub_url, sizeof ch->sub_url, "%s/Videos/%s/%s/Subtitles/%d/0/Stream.srt?api_key=%s",
                             base, raw_id(item_id), id, (int)jnum(best, "Index"), c->cfg.token);
                else
                    snprintf(ch->sub_url, sizeof ch->sub_url, "%s/Videos/%s/%s/Subtitles/%d/Stream.srt?api_key=%s",
                             base, raw_id(item_id), id, (int)jnum(best, "Index"), c->cfg.token);
            }
        }
        if (vs) {
            ch->width  = (int)jnum(vs, "Width");
            ch->height = (int)jnum(vs, "Height");
            snprintf(ch->video_codec, sizeof ch->video_codec, "%s", jstr(vs, "Codec"));
            video_range(vs, ch->video_range, sizeof ch->video_range);
        }
        if (as) {
            snprintf(ch->audio_codec, sizeof ch->audio_codec, "%s", jstr(as, "Codec"));
            ch->audio_channels = (int)jnum(as, "Channels");
        }

        PROV_LOG("mediasrv:   source %d name (raw): %s", n + 1, jstr(ms, "Name"));
        label_clean(jstr(ms, "Name"), ch->label, sizeof ch->label);
        label_facts(ch);
        if (!ch->label[0]) snprintf(ch->label, sizeof ch->label, "Version %d", n + 1);
        ++n;
    }
    if (root) cJSON_Delete(root);
    return n;
}

typedef struct {
    ms_client_t *c;
    evo_provider_resolve_cb cb;
    void *ud;
    char item[EVO_PROVIDER_MAX_ITEM_ID];
} resolve_ctx_t;

static void resolve_direct(ms_client_t *c, const char *item_id,
                           evo_provider_resolve_cb cb, void *ud)
{
    evo_stream_choice_t choice;
    evo_provider_stream_choice_clear(&choice);
    ms_build_stream_url(c, item_id, choice.url, sizeof choice.url);
    /* Static=true is the file byte for byte, no transcode: the container is
     * whatever it is on the server, and FFmpeg probes it anyway. */
    snprintf(choice.label, sizeof choice.label, "Direct");
    if (cb) cb(1, &choice, 1, ud);
}

/* The same version as a server-made HLS stream: H.264 + AAC/AC-3 at up to 1080p,
 * for a file EVO cannot decode or a link too slow for the original. */
static void add_transcode(ms_client_t *c, const char *item, const evo_stream_choice_t *src,
                          evo_stream_choice_t *out)
{
    char base[200], msid[200], ses[128], srcesc[200], dev[96];
    ms_base(c, base, sizeof base);
    url_param(src->url, "MediaSourceId", msid, sizeof msid);
    url_param(src->url, "PlaySessionId", ses, sizeof ses);
    if (evo_provider_url_escape(msid, srcesc, sizeof srcesc) < 0) srcesc[0] = '\0';
    if (evo_provider_url_escape(c->cfg.device_id[0] ? c->cfg.device_id : MS_DEVICE_ID, dev, sizeof dev) < 0)
        dev[0] = '\0';
    evo_provider_stream_choice_clear(out);
    snprintf(out->url, sizeof out->url,
             "%s/Videos/%s/master.m3u8?MediaSourceId=%s&DeviceId=%s%s%s&VideoCodec=h264"
             "&AudioCodec=aac,ac3&MaxStreamingBitrate=15000000&MaxWidth=1920"
             "&TranscodingMaxAudioChannels=6&SegmentContainer=ts&MinSegments=1"
             "&BreakOnNonKeyFrames=true&api_key=%s",
             base, item, srcesc, dev, ses[0] ? "&PlaySessionId=" : "", ses, c->cfg.token);
    snprintf(out->label, sizeof out->label, "Server transcode (1080p H.264)");
    snprintf(out->container, sizeof out->container, "hls");
    snprintf(out->video_codec, sizeof out->video_codec, "h264");
    snprintf(out->audio_codec, sizeof out->audio_codec, "aac");
    snprintf(out->sub_url, sizeof out->sub_url, "%s", src->sub_url);
    out->width = 1920; out->height = 1080;
    out->fallback = 1;
}

static void on_playback_info(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    resolve_ctx_t *x = (resolve_ctx_t *)ud;
    evo_stream_choice_t *ch = (evo_stream_choice_t *)calloc(MS_MAX_SOURCES + 1, sizeof *ch);
    int n = (ch && ok && status == 200) ? ms_parse_sources(x->c, x->item, body, ch, MS_MAX_SOURCES) : 0;
    if (n > 0) {
        evo_stream_choice_t tc;
        add_transcode(x->c, raw_id(x->item), &ch[0], &tc);
        if (x->c->cfg.transcode) {           /* always: first, and a real pick */
            memmove(&ch[1], &ch[0], (size_t)n * sizeof *ch);
            ch[0] = tc;
            ch[0].fallback = 0;
        } else {
            ch[n] = tc;
        }
        ++n;
    }
    if (n <= 0) {
        PROV_WARN("mediasrv: PlaybackInfo for %s -> ok=%d http=%d, no sources - direct stream",
                 x->item, ok, status);
        resolve_direct(x->c, x->item, x->cb, x->ud);
    } else {
        PROV_LOG("mediasrv: %d version(s) for %s", n, x->item);
        for (int i = 0; i < n; ++i)
            PROV_LOG("mediasrv:   [%d] %s | %dx%d %s %s | %s %dch | %lld kbps %lld MB (as listed)", i, ch[i].label,
                     ch[i].width, ch[i].height, ch[i].video_codec, ch[i].video_range,
                     ch[i].audio_codec, ch[i].audio_channels,
                     (long long)(ch[i].bitrate_bps / 1000), (long long)(ch[i].size_bytes >> 20));
        if (x->cb) x->cb(1, ch, n, x->ud);
    }
    free(ch);
    free(x);
}

/* A live channel: ask the server to open it and give back an HLS stream. */
static void on_live_info(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    resolve_ctx_t *x = (resolve_ctx_t *)ud;
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    cJSON *srcs = root ? cJSON_GetObjectItem(root, "MediaSources") : NULL;
    cJSON *ms = srcs ? cJSON_GetArrayItem(srcs, 0) : NULL;
    const char *turl = jstr(ms, "TranscodingUrl");
    evo_stream_choice_t ch;
    evo_provider_stream_choice_clear(&ch);
    if (turl[0]) {
        char base[200];
        ms_base(x->c, base, sizeof base);
        if (!strncmp(turl, "/emby/", 6)) turl += 5;          /* base carries the prefix */
        snprintf(ch.url, sizeof ch.url, "%s%s%s%s", base, turl,
                 (strstr(turl, "api_key=") || strstr(turl, "ApiKey=")) ? "" : (strchr(turl, '?') ? "&api_key=" : "?api_key="),
                 (strstr(turl, "api_key=") || strstr(turl, "ApiKey=")) ? "" : x->c->cfg.token);
        snprintf(ch.container, sizeof ch.container, "hls");
        snprintf(ch.label, sizeof ch.label, "Live");
        ch.is_live = 1;
        if (x->cb) x->cb(1, &ch, 1, x->ud);
    } else {
        PROV_ERR("mediasrv: live channel %s -> ok=%d http=%d, no stream", x->item, ok, status);
        if (x->cb) x->cb(0, NULL, 0, x->ud);
    }
    if (root) cJSON_Delete(root);
    free(x);
}

static int ms_resolve_live(ms_client_t *c, const char *item_id, evo_provider_resolve_cb cb, void *ud)
{
    resolve_ctx_t *x = (resolve_ctx_t *)calloc(1, sizeof *x);
    if (!x) return -2;
    x->c = c; x->cb = cb; x->ud = ud;
    snprintf(x->item, sizeof x->item, "%s", item_id);
    char path[320], body[640];
    snprintf(path, sizeof path, "/Items/%s/PlaybackInfo?UserId=%s&AutoOpenLiveStream=true&MaxStreamingBitrate=20000000",
             raw_id(item_id), c->cfg.user_id);
    snprintf(body, sizeof body, "{\"UserId\":\"%s\",\"DeviceProfile\":{\"MaxStreamingBitrate\":20000000,"
             "\"TranscodingProfiles\":[{\"Type\":\"Video\",\"Container\":\"ts\",\"Protocol\":\"hls\","
             "\"VideoCodec\":\"h264\",\"AudioCodec\":\"aac,ac3\",\"Context\":\"Streaming\"}]}}", c->cfg.user_id);
    int rc = ms_request(c, "POST", path, body, 1, on_live_info, x);
    if (rc != 0) free(x);
    return rc;
}

int ms_resolve(ms_client_t *c, const char *item_id, evo_provider_resolve_cb cb, void *ud)
{
    if (!item_id || !c->cfg.is_connected) return -1;
    if (!strncmp(item_id, "a:", 2)) {            /* a track: the file, no versions */
        resolve_direct(c, item_id, cb, ud);
        return 0;
    }
    if (!strncmp(item_id, "tv:", 3)) return ms_resolve_live(c, item_id, cb, ud);
    if (strncmp(item_id, "i:", 2)) return -1;
    resolve_ctx_t *x = (resolve_ctx_t *)calloc(1, sizeof *x);
    if (!x) return -2;
    x->c = c; x->cb = cb; x->ud = ud;
    snprintf(x->item, sizeof x->item, "%s", item_id);

    char path[256], body[128];
    snprintf(path, sizeof path, "/Items/%s/PlaybackInfo?UserId=%s", raw_id(item_id), c->cfg.user_id);
    snprintf(body, sizeof body, "{\"UserId\":\"%s\"}", c->cfg.user_id);
    if (ms_request(c, "POST", path, body, 1, on_playback_info, x) != 0) {
        free(x);
        resolve_direct(c, item_id, cb, ud);
    }
    return 0;
}

/* Copy query parameter `key` out of `url`, %-decoded. "" when absent. */
static void url_param(const char *url, const char *key, char *out, size_t cap)
{
    out[0] = '\0';
    size_t kl = strlen(key);
    const char *q = strchr(url, '?');
    for (const char *p = q; p; p = strchr(p + 1, '&')) {
        if (strncmp(p + 1, key, kl) || p[1 + kl] != '=') continue;
        const char *v = p + 2 + kl;
        size_t o = 0;
        while (*v && *v != '&' && o + 1 < cap) {
            if (*v == '%' && v[1] && v[2]) {
                char hex[3] = { v[1], v[2], 0 };
                out[o++] = (char)strtol(hex, NULL, 16);
                v += 3;
            } else {
                out[o++] = *v++;
            }
        }
        out[o] = '\0';
        return;
    }
}

void ms_stream_chosen(ms_client_t *c, const char *item_id, const evo_stream_choice_t *choice)
{
    c->chosen_item[0] = c->chosen_source[0] = c->chosen_session[0] = '\0';
    c->chosen_transcode = choice && strstr(choice->url, "/master.m3u8?") != NULL;
    if (!item_id || !choice) return;
    url_param(choice->url, "MediaSourceId", c->chosen_source, sizeof c->chosen_source);
    if (!c->chosen_source[0]) return;                  /* the direct fallback */
    url_param(choice->url, "PlaySessionId", c->chosen_session, sizeof c->chosen_session);
    snprintf(c->chosen_item, sizeof c->chosen_item, "%s", raw_id(item_id));
    PROV_LOG("mediasrv: playing %s source %s (%s)", c->chosen_item, c->chosen_source, choice->label);
}

/* Logged, so a server that rejects the reports shows in evo.log rather than
 * silently never saving a resume point. */
static void on_report(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    if (!ok || status >= 300)
        PROV_LOG("mediasrv: report %s -> ok=%d http=%d %.120s",
                 (const char *)ud, ok, status, body ? body : "");
}

void ms_report(ms_client_t *c, const char *item_id, int64_t pos_sec, int64_t dur_sec,
               evo_provider_play_state_t state)
{
    if (!c->cfg.is_connected || !item_id || (strncmp(item_id, "i:", 2) && strncmp(item_id, "a:", 2))) return;
    char body[640];
    long long pos = (long long)pos_sec * 10000000LL;
    const char *path = "/Sessions/Playing";
    /* The version that plays when the user picked one (#116); otherwise the
     * item's own id, which is what a single-source item's source is called. */
    const char *item = raw_id(item_id);
    const int chosen = c->chosen_source[0] && !strcmp(c->chosen_item, item);
    const char *source = chosen ? c->chosen_source : item;
    if (state == EVO_PROVIDER_PLAY_START || !c->play_session[0]) {
        if (chosen && c->chosen_session[0])
            snprintf(c->play_session, sizeof c->play_session, "%s", c->chosen_session);
        else
            snprintf(c->play_session, sizeof c->play_session, "evo-%s-%u-%lld",
                     c->kind == MS_EMBY ? "e" : "j", ++c->play_count, (long long)time(NULL));
    }
    if (state == EVO_PROVIDER_PLAY_START) {
        snprintf(body, sizeof body,
                 "{\"ItemId\":\"%s\",\"MediaSourceId\":\"%s\",\"PlaySessionId\":\"%s\","
                 "\"PositionTicks\":%lld,\"CanSeek\":true,\"PlayMethod\":\"%s\"}",
                 item, source, c->play_session, pos, c->chosen_transcode && chosen ? "Transcode" : "DirectPlay");
    } else if (state == EVO_PROVIDER_PLAY_UPDATE) {
        path = "/Sessions/Playing/Progress";
        snprintf(body, sizeof body,
                 "{\"ItemId\":\"%s\",\"MediaSourceId\":\"%s\",\"PlaySessionId\":\"%s\","
                 "\"PositionTicks\":%lld,\"CanSeek\":true,\"IsPaused\":%s,"
                 "\"PlayMethod\":\"%s\",\"EventName\":\"%s\"}",
                 item, source, c->play_session, pos,
                 c->chosen_transcode && chosen ? "Transcode" : "DirectPlay",
                 evo_provider_is_paused() ? "true" : "false",
                 evo_provider_is_paused() ? "Pause" : "TimeUpdate");
    } else {
        path = "/Sessions/Playing/Stopped";
        snprintf(body, sizeof body,
                 "{\"ItemId\":\"%s\",\"MediaSourceId\":\"%s\",\"PlaySessionId\":\"%s\","
                 "\"PositionTicks\":%lld}",
                 item, source, c->play_session, pos);
        /* A later play of the same item from Recent goes through no picker. */
        if (chosen) c->chosen_item[0] = c->chosen_source[0] = c->chosen_session[0] = '\0';
    }
    (void)dur_sec;   /* the server knows the runtime; the report has no field for it */
    ms_request(c, "POST", path, body, 1, on_report, (void *)(path + 10));
}

/* ------------------------------------------------------------------------- */
/* The old single-instance API                                               */
/* ------------------------------------------------------------------------- */

int emby_init(void)                 { return ms_load(ms_client(MS_EMBY)); }
int emby_save_config(void)          { return ms_save(ms_client(MS_EMBY)); }
emby_config_t *emby_get_config(void) { return &ms_client(MS_EMBY)->cfg; }
void emby_disconnect(void)          { ms_drop_session(ms_client(MS_EMBY)); }

void emby_set_server(const char *host, int port, const char *username, const char *password)
{
    emby_config_t *g = &ms_client(MS_EMBY)->cfg;
    if (host) snprintf(g->host, sizeof g->host, "%s", host);
    if (port > 0) g->port = port;
    if (username) snprintf(g->username, sizeof g->username, "%s", username);
    if (password) snprintf(g->password, sizeof g->password, "%s", password);
}

int emby_build_stream_url(const char *item_id, char *out_url, size_t max_len)
{
    return ms_build_stream_url(ms_client(MS_EMBY), item_id, out_url, max_len);
}
