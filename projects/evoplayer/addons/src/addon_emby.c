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

#define MS_DEVICE_ID   "evo-ps5"
#define MS_PAGE        96          /* rows per catalog request: twelve 8-card pages */
#define MS_FIELDS      "Fields=Overview,ProductionYear,ChildCount&EnableImageTypes=Primary&ImageTypeLimit=1"

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
    char qc_secret[128];           /* Quick Connect in progress */
};

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
    if (!f) return 0;                    /* not set up yet - not an error */

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
    }
    fclose(f);
    g->is_connected = g->token[0] && g->user_id[0];
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
    fprintf(f, "host=%s\nport=%d\nusername=%s\ntoken=%s\nuser_id=%s\nserver_name=%s\nhttps=%d\npath=%s\n",
            g->host, g->port, g->username, g->token, g->user_id, g->server_name,
            g->use_https ? 1 : 0, g->path);
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
                     MS_DEVICE_ID, EVO_PLAYER_VERSION);
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
    (void)len;
    signin_ctx_t *x = (signin_ctx_t *)ud;
    ms_client_t *c = x->c;
    const char *msg = "Could not reach the server";
    int good = 0;

    if (ok && (status == 401 || status == 403)) {
        msg = "Wrong user name or password";
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
        }
        if (root) cJSON_Delete(root);
    }
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
        PROV_LOG("quick connect: initiate -> ok=%d http=%d", ok, status);
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

    if (!strcmp(type, "Series")) {
        snprintf(it->id, sizeof it->id, "ser:%s", id);
        it->is_folder = 1;
        it->kind = EVO_MEDIA_FOLDER;
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
        if (has_primary && !strcmp(type, "BoxSet")) art_url(c, id, it->art_url, sizeof it->art_url);
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
            if (year && rt[0]) snprintf(it->subtitle, sizeof it->subtitle, "%d · %s", year, rt);
            else if (year)     snprintf(it->subtitle, sizeof it->subtitle, "%d", year);
            else               snprintf(it->subtitle, sizeof it->subtitle, "%s", rt);
            if (has_primary) art_url(c, id, it->art_url, sizeof it->art_url);
        }
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
        PROV_LOG("media server: session rejected (http %d) - sign-in needed", status);
        ms_drop_session(c);
    }
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    /* Seasons and episodes come back as {"Items": [...]}, like everything
     * else; a bare array is tolerated in case a server version differs. */
    cJSON *arr = root ? (cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "Items")) : NULL;
    if (!arr || !cJSON_IsArray(arr)) {
        if (x->cb) x->cb(0, NULL, 0, 0, x->ud);
        if (root) cJSON_Delete(root);
        free(x);
        return;
    }

    int n = cJSON_GetArraySize(arr);
    int extra = x->root ? 2 : 0;
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
    }
    for (int i = 0; i < n; ++i) {
        cJSON *o = cJSON_GetArrayItem(arr, i);
        if (!o || !jstr(o, "Id")[0]) continue;
        if (x->root) {
            /* A view: lib:<CollectionType>:<id>. */
            const char *ct = jstr(o, "CollectionType");
            evo_provider_item_clear(&items[k]);
            snprintf(items[k].id, sizeof items[k].id, "lib:%s:%s", ct[0] ? ct : "folder", jstr(o, "Id"));
            snprintf(items[k].title, sizeof items[k].title, "%s", jstr(o, "Name"));
            snprintf(items[k].subtitle, sizeof items[k].subtitle, "%s",
                     !strcmp(ct, "movies")  ? "Movies" :
                     !strcmp(ct, "tvshows") ? "TV Shows" :
                     !strcmp(ct, "music")   ? "Music" : "Library");
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
    char path[1024];

    if (!parent_id || !*parent_id) {
        if (page > 0) return -1;
        snprintf(path, sizeof path, "/Users/%s/Views", uid);
        return items_request(c, path, CTX_LIST, 0, 1, cb, ud);
    }
    if (!strcmp(parent_id, "v:resume")) {
        snprintf(path, sizeof path,
                 "/Users/%s/Items/Resume?MediaTypes=Video&StartIndex=%d&Limit=%d&%s",
                 uid, start, MS_PAGE, MS_FIELDS);
        return items_request(c, path, CTX_MIXED, start, 0, cb, ud);
    }
    if (!strcmp(parent_id, "v:nextup")) {
        snprintf(path, sizeof path, "/Shows/NextUp?UserId=%s&StartIndex=%d&Limit=%d&%s",
                 uid, start, MS_PAGE, MS_FIELDS);
        return items_request(c, path, CTX_MIXED, start, 0, cb, ud);
    }
    if (!strncmp(parent_id, "lib:", 4)) {
        char type[32] = "", id[96] = "";
        const char *colon = strchr(parent_id + 4, ':');
        if (!colon) return -1;
        snprintf(type, sizeof type, "%.*s", (int)(colon - (parent_id + 4)), parent_id + 4);
        snprintf(id, sizeof id, "%s", colon + 1);
        const char *include = !strcmp(type, "movies")  ? "&Recursive=true&IncludeItemTypes=Movie" :
                              !strcmp(type, "tvshows") ? "&Recursive=true&IncludeItemTypes=Series" : "";
        snprintf(path, sizeof path,
                 "/Users/%s/Items?ParentId=%s%s&SortBy=SortName&SortOrder=Ascending"
                 "&StartIndex=%d&Limit=%d&%s",
                 uid, id, include, start, MS_PAGE, MS_FIELDS);
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
                 "/Users/%s/Items?ParentId=%s&SortBy=SortName&SortOrder=Ascending"
                 "&StartIndex=%d&Limit=%d&%s",
                 uid, parent_id + 4, start, MS_PAGE, MS_FIELDS);
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
/* Resolve + progress                                                        */
/* ------------------------------------------------------------------------- */

static const char *raw_id(const char *item_id)
{
    return (item_id && !strncmp(item_id, "i:", 2)) ? item_id + 2 : item_id;
}

int ms_build_stream_url(ms_client_t *c, const char *item_id, char *out, size_t cap)
{
    if (!item_id || !out || cap == 0) return -1;
    char base[200];
    ms_base(c, base, sizeof base);
    snprintf(out, cap, "%s/Videos/%s/stream?Static=true&api_key=%s",
             base, raw_id(item_id), c->cfg.token);
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

/* A source name on one line: AIOStreams writes multi-line release blurbs. */
static void one_line(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 4 < cap; ++in) {
        unsigned char ch = (unsigned char)*in;
        if (ch == '\n') {
            while (o > 0 && out[o - 1] == ' ') o--;
            if (o > 0) { memcpy(out + o, " | ", 3); o += 3; }
            while (in[1] == ' ' || in[1] == '\n' || in[1] == '\r') in++;
        } else if (ch < 0x20) {
            if (o > 0 && out[o - 1] != ' ') out[o++] = ' ';
        } else {
            out[o++] = (char)ch;
        }
    }
    out[o] = '\0';
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

        char name[256];
        one_line(jstr(ms, "Name"), name, sizeof name);
        if (name[0]) snprintf(ch->label, sizeof ch->label, "%.63s", name);
        else         snprintf(ch->label, sizeof ch->label, "Version %d", n + 1);
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

static void on_playback_info(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    resolve_ctx_t *x = (resolve_ctx_t *)ud;
    evo_stream_choice_t *ch = (evo_stream_choice_t *)calloc(MS_MAX_SOURCES, sizeof *ch);
    int n = (ch && ok && status == 200) ? ms_parse_sources(x->c, x->item, body, ch, MS_MAX_SOURCES) : 0;
    if (n <= 0) {
        PROV_LOG("media server: PlaybackInfo for %s -> ok=%d http=%d, no sources - direct stream",
                 x->item, ok, status);
        resolve_direct(x->c, x->item, x->cb, x->ud);
    } else {
        PROV_LOG("media server: %d version(s) for %s", n, x->item);
        for (int i = 0; i < n; ++i)
            PROV_LOG("  [%d] %s | %dx%d %s %s | %s %dch | %lld kbps %lld MB", i, ch[i].label,
                     ch[i].width, ch[i].height, ch[i].video_codec, ch[i].video_range,
                     ch[i].audio_codec, ch[i].audio_channels,
                     (long long)(ch[i].bitrate_bps / 1000), (long long)(ch[i].size_bytes >> 20));
        if (x->cb) x->cb(1, ch, n, x->ud);
    }
    free(ch);
    free(x);
}

int ms_resolve(ms_client_t *c, const char *item_id, evo_provider_resolve_cb cb, void *ud)
{
    if (!item_id || strncmp(item_id, "i:", 2) || !c->cfg.is_connected) return -1;
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
    if (!item_id || !choice) return;
    url_param(choice->url, "MediaSourceId", c->chosen_source, sizeof c->chosen_source);
    if (!c->chosen_source[0]) return;                  /* the direct fallback */
    url_param(choice->url, "PlaySessionId", c->chosen_session, sizeof c->chosen_session);
    snprintf(c->chosen_item, sizeof c->chosen_item, "%s", raw_id(item_id));
    PROV_LOG("media server: playing %s source %s (%s)", c->chosen_item, c->chosen_source, choice->label);
}

/* Logged, so a server that rejects the reports shows in evo.log rather than
 * silently never saving a resume point. */
static void on_report(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    if (!ok || status >= 300)
        PROV_LOG("media server report %s -> ok=%d http=%d %.120s",
                 (const char *)ud, ok, status, body ? body : "");
}

void ms_report(ms_client_t *c, const char *item_id, int64_t pos_sec, int64_t dur_sec,
               evo_provider_play_state_t state)
{
    if (!c->cfg.is_connected || !item_id || strncmp(item_id, "i:", 2)) return;
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
                 "\"PositionTicks\":%lld,\"CanSeek\":true,\"PlayMethod\":\"DirectStream\"}",
                 item, source, c->play_session, pos);
    } else if (state == EVO_PROVIDER_PLAY_UPDATE) {
        path = "/Sessions/Playing/Progress";
        snprintf(body, sizeof body,
                 "{\"ItemId\":\"%s\",\"MediaSourceId\":\"%s\",\"PlaySessionId\":\"%s\","
                 "\"PositionTicks\":%lld,\"CanSeek\":true,\"IsPaused\":false,"
                 "\"PlayMethod\":\"DirectStream\",\"EventName\":\"TimeUpdate\"}",
                 item, source, c->play_session, pos);
    } else {
        path = "/Sessions/Playing/Stopped";
        snprintf(body, sizeof body,
                 "{\"ItemId\":\"%s\",\"MediaSourceId\":\"%s\",\"PlaySessionId\":\"%s\","
                 "\"PositionTicks\":%lld}",
                 item, source, c->play_session, pos);
        /* A later play of the same item from Recent goes through no picker. */
        if (chosen) c->chosen_item[0] = c->chosen_source[0] = c->chosen_session[0] = '\0';
    }
    (void)dur_sec;
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
