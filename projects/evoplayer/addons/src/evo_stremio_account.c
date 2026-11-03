/*
 * evo_stremio_account - a Stremio account inside EVO. See evo_stremio_account.h.
 */
#include "evo_stremio_account.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "evo_net.h"
#include "evo_data_path.h"
#include "evo_provider_log.h"

#define SA_API   "https://api.strem.io/api/"
#define SA_LINK  "https://link.stremio.com/api/v2/"

#define SA_FILE_ACCOUNT "stremio_account.json"
#define SA_FILE_ADDONS  "stremio_addons.json"
#define SA_FILE_LIBRARY "stremio_library.json"

static char   g_key[160];
static char   g_email[128];
static cJSON *g_addons;        /* array, owned */
static cJSON *g_library;       /* array, owned */
static int    g_pulls_out;

/* link-code sign-in */
static char   g_code[16];
static evo_provider_qc_code_cb g_start_cb;
static void  *g_start_ud;
static evo_provider_qc_cb      g_poll_cb;
static void  *g_poll_ud;

/* ------------------------------------------------------------------------- */
/* Files                                                                     */
/* ------------------------------------------------------------------------- */

static cJSON *read_json(const char *leaf)
{
    FILE *f = fopen(evo_data_path(leaf), "rb");
    if (!f)
        return NULL;
    size_t cap = 1 << 16, n = 0;
    char *buf = (char *)malloc(cap + 1);
    while (buf) {
        size_t r = fread(buf + n, 1, cap - n, f);
        n += r;
        if (n < cap)
            break;
        if (cap >= ((size_t)16 << 20)) {            /* 16 MB is not a library */
            free(buf);
            buf = NULL;
            break;
        }
        char *g = (char *)realloc(buf, cap * 2 + 1);
        if (!g) { free(buf); buf = NULL; break; }
        buf = g;
        cap *= 2;
    }
    fclose(f);
    if (!buf)
        return NULL;
    buf[n] = 0;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

static int write_json(const char *leaf, const cJSON *j)
{
    char *txt = j ? cJSON_PrintUnformatted(j) : NULL;
    if (!txt)
        return -1;
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%s.tmp", leaf);
    char tmp_path[512];
    snprintf(tmp_path, sizeof tmp_path, "%s", evo_data_path(tmp));
    FILE *f = fopen(tmp_path, "wb");
    int ok = 0;
    if (f) {
        ok = fputs(txt, f) >= 0;
        fclose(f);
    }
    free(txt);
    if (!ok)
        return -1;
    char final_path[512];
    snprintf(final_path, sizeof final_path, "%s", evo_data_path(leaf));
    remove(final_path);
    return rename(tmp_path, final_path) == 0 ? 0 : -1;
}

static void save_account(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "authKey", g_key);
    cJSON_AddStringToObject(j, "email", g_email);
    write_json(SA_FILE_ACCOUNT, j);
    cJSON_Delete(j);
}

void evo_sa_init(void)
{
    g_key[0] = g_email[0] = 0;
    cJSON *a = read_json(SA_FILE_ACCOUNT);
    const char *k = a ? cJSON_GetStringValue(cJSON_GetObjectItem(a, "authKey")) : NULL;
    const char *e = a ? cJSON_GetStringValue(cJSON_GetObjectItem(a, "email")) : NULL;
    if (k && k[0]) {
        snprintf(g_key, sizeof g_key, "%s", k);
        snprintf(g_email, sizeof g_email, "%s", e ? e : "");
    }
    if (a)
        cJSON_Delete(a);
    if (g_addons) { cJSON_Delete(g_addons); g_addons = NULL; }
    if (g_library) { cJSON_Delete(g_library); g_library = NULL; }
    if (!g_key[0])
        return;
    g_addons = read_json(SA_FILE_ADDONS);
    if (g_addons && !cJSON_IsArray(g_addons)) { cJSON_Delete(g_addons); g_addons = NULL; }
    g_library = read_json(SA_FILE_LIBRARY);
    if (g_library && !cJSON_IsArray(g_library)) { cJSON_Delete(g_library); g_library = NULL; }
    PROV_LOG("stremio account: signed in (%s), %d addon(s) and %d library item(s) cached",
             g_email[0] ? g_email : "no email yet",
             g_addons ? cJSON_GetArraySize(g_addons) : 0,
             g_library ? cJSON_GetArraySize(g_library) : 0);
}

int evo_sa_signed_in(void) { return g_key[0] != 0; }
const char *evo_sa_email(void) { return g_email; }
int evo_sa_busy(void) { return g_pulls_out > 0; }
const cJSON *evo_sa_addons_cached(void) { return g_addons; }
const cJSON *evo_sa_library_cached(void) { return g_library; }

void evo_sa_sign_out(void)
{
    if (g_key[0]) {
        /* Tell the server too, fire and forget: the key stops working there. */
        char body[256];
        snprintf(body, sizeof body, "{\"type\":\"Logout\",\"authKey\":\"%s\"}", g_key);
        const char *h[1] = { "Content-Type: application/json" };
        evo_net_request_async("POST", SA_API "logout", body, h, 1, NULL, NULL);
    }
    g_key[0] = g_email[0] = 0;
    if (g_addons) { cJSON_Delete(g_addons); g_addons = NULL; }
    if (g_library) { cJSON_Delete(g_library); g_library = NULL; }
    remove(evo_data_path(SA_FILE_ACCOUNT));
    remove(evo_data_path(SA_FILE_ADDONS));
    remove(evo_data_path(SA_FILE_LIBRARY));
    PROV_LOG("stremio account: signed out");
}

/* ------------------------------------------------------------------------- */
/* Requests                                                                  */
/* ------------------------------------------------------------------------- */

/* {"result": X} -> X, {"error": ...} -> NULL. The link API answers the same way;
 * a bare object without "result" is taken as the result itself. */
static cJSON *result_of(cJSON *root)
{
    if (!root)
        return NULL;
    if (cJSON_GetObjectItem(root, "error"))
        return NULL;
    cJSON *r = cJSON_GetObjectItem(root, "result");
    return r ? r : root;
}

/* An authKey is [A-Za-z0-9] and a few symbols; anything that would need JSON
 * escaping is not one, and is refused rather than spliced into a body. */
static int key_is_safe(const char *k)
{
    if (!k || !k[0] || strlen(k) >= sizeof g_key)
        return 0;
    for (const char *p = k; *p; ++p)
        if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20)
            return 0;
    return 1;
}

static int api_post(const char *method, const char *body, evo_net_cb cb, void *ud)
{
    char url[128];
    snprintf(url, sizeof url, SA_API "%s", method);
    const char *h[2] = { "Content-Type: application/json", "Accept: application/json" };
    return evo_net_request_async("POST", url, body, h, 2, cb, ud);
}

static void on_user(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len; (void)ud;
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    cJSON *r = result_of(root);
    const char *e = r ? cJSON_GetStringValue(cJSON_GetObjectItem(r, "email")) : NULL;
    if (e && e[0] && g_key[0]) {
        snprintf(g_email, sizeof g_email, "%s", e);
        save_account();
    }
    if (root)
        cJSON_Delete(root);
}

static void fetch_user(void)
{
    char body[256];
    snprintf(body, sizeof body, "{\"type\":\"GetUser\",\"authKey\":\"%s\"}", g_key);
    api_post("getUser", body, on_user, NULL);
}

/* ---- link code ----------------------------------------------------------- */

static void on_link_create(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len; (void)ud;
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    cJSON *r = result_of(root);
    const char *code = r ? cJSON_GetStringValue(cJSON_GetObjectItem(r, "code")) : NULL;
    int good = code && code[0] && strlen(code) < sizeof g_code;
    if (good)
        snprintf(g_code, sizeof g_code, "%s", code);
    else
        g_code[0] = 0;
    PROV_LOG("stremio account: link code %s (http %d)", good ? "issued" : "refused", status);
    evo_provider_qc_code_cb cb = g_start_cb;
    void *cud = g_start_ud;
    g_start_cb = NULL;
    if (cb)
        cb(good, good ? g_code : "Could not reach link.stremio.com", cud);
    if (root)
        cJSON_Delete(root);
}

int evo_sa_link_start(evo_provider_qc_code_cb cb, void *ud)
{
    if (!cb || g_start_cb)
        return -1;
    g_code[0] = 0;
    g_start_cb = cb;
    g_start_ud = ud;
    const char *h[1] = { "Accept: application/json" };
    int rc = evo_net_request_async("GET", SA_LINK "create?type=Create", NULL, h, 1,
                                   on_link_create, NULL);
    if (rc != 0)
        g_start_cb = NULL;
    return rc;
}

static void on_link_read(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len; (void)ud;
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    /* 0 until linked: "not entered yet", an expired code (the screen renews
     * it) and a network blip all mean keep waiting - Circle gives up. */
    int state = 0;
    const char *msg = "Waiting for the code at link.stremio.com";
    cJSON *r = root ? cJSON_GetObjectItem(root, "result") : NULL;
    const char *k = r ? cJSON_GetStringValue(cJSON_GetObjectItem(r, "authKey")) : NULL;
    if (k && key_is_safe(k)) {
        snprintf(g_key, sizeof g_key, "%s", k);
        g_email[0] = 0;
        save_account();
        g_code[0] = 0;
        state = 1;
        msg = "Stremio account linked";
        PROV_LOG("stremio account: linked");
        fetch_user();
    }
    evo_provider_qc_cb cb = g_poll_cb;
    void *cud = g_poll_ud;
    g_poll_cb = NULL;
    if (cb)
        cb(state, msg, cud);
    if (root)
        cJSON_Delete(root);
}

int evo_sa_link_poll(evo_provider_qc_cb cb, void *ud)
{
    if (!cb || g_poll_cb || !g_code[0])
        return -1;
    char url[128];
    snprintf(url, sizeof url, SA_LINK "read?type=Read&code=%s", g_code);
    g_poll_cb = cb;
    g_poll_ud = ud;
    const char *h[1] = { "Accept: application/json" };
    int rc = evo_net_request_async("GET", url, NULL, h, 1, on_link_read, NULL);
    if (rc != 0)
        g_poll_cb = NULL;
    return rc;
}

void evo_sa_link_cancel(void)
{
    g_code[0] = 0;
}

/* ---- addons -------------------------------------------------------------- */

typedef struct {
    void (*cb)(int ok, int changed, void *ud);
    void *ud;
    char key[160];
} pull_ctx_t;

static int same_urls(const cJSON *a, const cJSON *b)
{
    const int na = a ? cJSON_GetArraySize(a) : 0, nb = b ? cJSON_GetArraySize(b) : 0;
    if (na != nb)
        return 0;
    for (int i = 0; i < na; ++i) {
        const char *ua = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(a, i), "transportUrl"));
        const char *ub = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(b, i), "transportUrl"));
        if (!ua || !ub || strcmp(ua, ub))
            return 0;
    }
    return 1;
}

static void on_addons(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    pull_ctx_t *x = (pull_ctx_t *)ud;
    g_pulls_out--;
    int good = 0, changed = 0;
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    cJSON *r = result_of(root);
    cJSON *arr = r ? cJSON_GetObjectItem(r, "addons") : NULL;
    /* Signed out (or in again as someone else) while this was out: drop it. */
    if (arr && cJSON_IsArray(arr) && !strcmp(x->key, g_key)) {
        good = 1;
        changed = !same_urls(arr, g_addons);
        /* Keep the array: detach it from the reply by re-parsing its text. */
        char *txt = cJSON_PrintUnformatted(arr);
        cJSON *copy = txt ? cJSON_Parse(txt) : NULL;
        free(txt);
        if (copy) {
            if (g_addons) cJSON_Delete(g_addons);
            g_addons = copy;
            write_json(SA_FILE_ADDONS, g_addons);
        } else {
            good = 0;
        }
    } else if (root && cJSON_GetObjectItem(root, "error")) {
        PROV_LOG("stremio account: addonCollectionGet refused (http %d)", status);
    }
    PROV_LOG("stremio account: addons %s (%d, %s)", good ? "pulled" : "not pulled",
             g_addons ? cJSON_GetArraySize(g_addons) : 0, changed ? "changed" : "same");
    if (x->cb)
        x->cb(good, changed, x->ud);
    if (root)
        cJSON_Delete(root);
    free(x);
}

int evo_sa_addons_pull(void (*cb)(int ok, int changed, void *ud), void *ud)
{
    if (!g_key[0])
        return -1;
    pull_ctx_t *x = (pull_ctx_t *)calloc(1, sizeof *x);
    if (!x)
        return -2;
    x->cb = cb;
    x->ud = ud;
    snprintf(x->key, sizeof x->key, "%s", g_key);
    char body[256];
    snprintf(body, sizeof body, "{\"type\":\"AddonCollectionGet\",\"authKey\":\"%s\",\"update\":true}", g_key);
    int rc = api_post("addonCollectionGet", body, on_addons, x);
    if (rc != 0) {
        free(x);
        return rc;
    }
    g_pulls_out++;
    if (!g_email[0])
        fetch_user();
    return 0;
}

/* ---- library ------------------------------------------------------------- */

typedef struct {
    void (*cb)(int ok, void *ud);
    void *ud;
    char key[160];
} lib_ctx_t;

static void on_library(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    lib_ctx_t *x = (lib_ctx_t *)ud;
    g_pulls_out--;
    int good = 0;
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    cJSON *r = result_of(root);
    if (r && cJSON_IsArray(r) && !strcmp(x->key, g_key)) {
        char *txt = cJSON_PrintUnformatted(r);
        cJSON *copy = txt ? cJSON_Parse(txt) : NULL;
        free(txt);
        if (copy) {
            if (g_library) cJSON_Delete(g_library);
            g_library = copy;
            write_json(SA_FILE_LIBRARY, g_library);
            good = 1;
        }
    }
    PROV_LOG("stremio account: library %s (%d item(s), http %d)", good ? "pulled" : "not pulled",
             g_library ? cJSON_GetArraySize(g_library) : 0, status);
    if (x->cb)
        x->cb(good, x->ud);
    if (root)
        cJSON_Delete(root);
    free(x);
}

int evo_sa_library_pull(void (*cb)(int ok, void *ud), void *ud)
{
    if (!g_key[0])
        return -1;
    lib_ctx_t *x = (lib_ctx_t *)calloc(1, sizeof *x);
    if (!x)
        return -2;
    x->cb = cb;
    x->ud = ud;
    snprintf(x->key, sizeof x->key, "%s", g_key);
    char body[320];
    snprintf(body, sizeof body,
             "{\"authKey\":\"%s\",\"collection\":\"libraryItem\",\"ids\":[],\"all\":true}", g_key);
    int rc = api_post("datastoreGet", body, on_library, x);
    if (rc != 0) {
        free(x);
        return rc;
    }
    g_pulls_out++;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

void evo_sa_iso8601(int64_t t, char out[32])
{
    if (t < 0)
        t = 0;
    int64_t days = t / 86400;
    int sec = (int)(t % 86400);
    /* civil_from_days (H. Hinnant), proleptic Gregorian */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2)
        y++;
    snprintf(out, 32, "%04d-%02u-%02uT%02d:%02d:%02d.000Z", (int)y, m, d,
             sec / 3600, (sec / 60) % 60, sec % 60);
}

void evo_sa_lib_id_of(const char *video_id, char *out, int cap)
{
    if (!out || cap <= 0)
        return;
    out[0] = 0;
    if (!video_id)
        return;
    /* IMDb: everything before the season. Other schemes ("kitsu:7:3",
     * "tmdb:1399:1:2") keep their prefix and the first number. */
    const char *c = strchr(video_id, ':');
    if (!strncmp(video_id, "tt", 2) || !c) {
        snprintf(out, (size_t)cap, "%.*s", c ? (int)(c - video_id) : (int)strlen(video_id), video_id);
        return;
    }
    const char *c2 = strchr(c + 1, ':');
    snprintf(out, (size_t)cap, "%.*s", c2 ? (int)(c2 - video_id) : (int)strlen(video_id), video_id);
}

static double num(const cJSON *o, const char *k)
{
    const cJSON *v = o ? cJSON_GetObjectItem(o, k) : NULL;
    return (v && cJSON_IsNumber(v)) ? v->valuedouble : 0.0;
}

int evo_sa_in_continue_watching(const cJSON *item)
{
    if (!item)
        return 0;
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(item, "type"));
    if (!type || !strcmp(type, "other"))
        return 0;
    const int removed = cJSON_IsTrue(cJSON_GetObjectItem(item, "removed"));
    const int temp = cJSON_IsTrue(cJSON_GetObjectItem(item, "temp"));
    const cJSON *st = cJSON_GetObjectItem(item, "state");
    return (!removed || temp) && num(st, "timeOffset") > 0.0;
}

/* ------------------------------------------------------------------------- */
/* Progress                                                                  */
/* ------------------------------------------------------------------------- */

static cJSON *library_find(const char *id)
{
    for (int i = 0; g_library && i < cJSON_GetArraySize(g_library); ++i) {
        cJSON *it = cJSON_GetArrayItem(g_library, i);
        const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(it, "_id"));
        if (v && !strcmp(v, id))
            return it;
    }
    return NULL;
}

static void set_num(cJSON *o, const char *k, double v)
{
    cJSON_DeleteItemFromObject(o, k);
    cJSON_AddNumberToObject(o, k, v);
}

static void set_str(cJSON *o, const char *k, const char *v)
{
    cJSON_DeleteItemFromObject(o, k);
    if (v)
        cJSON_AddStringToObject(o, k, v);
    else
        cJSON_AddItemToObject(o, k, cJSON_CreateNull());
}

static cJSON *new_item(const char *id, const char *type, const char *name,
                       const char *poster, const char *now)
{
    cJSON *it = cJSON_CreateObject();
    cJSON_AddStringToObject(it, "_id", id);
    cJSON_AddStringToObject(it, "name", name && name[0] ? name : id);
    cJSON_AddStringToObject(it, "type", type && type[0] ? type : "movie");
    if (poster && !strncmp(poster, "http", 4))
        cJSON_AddStringToObject(it, "poster", poster);
    else
        cJSON_AddItemToObject(it, "poster", cJSON_CreateNull());
    cJSON_AddStringToObject(it, "posterShape", "poster");
    cJSON_AddItemToObject(it, "removed", cJSON_CreateFalse());
    cJSON_AddItemToObject(it, "temp", cJSON_CreateTrue());          /* watched, not added: CW only */
    cJSON_AddStringToObject(it, "_ctime", now);
    cJSON_AddStringToObject(it, "_mtime", now);
    cJSON *st = cJSON_CreateObject();
    cJSON_AddStringToObject(st, "lastWatched", now);
    cJSON_AddNumberToObject(st, "timeWatched", 0);
    cJSON_AddNumberToObject(st, "timeOffset", 0);
    cJSON_AddNumberToObject(st, "overallTimeWatched", 0);
    cJSON_AddNumberToObject(st, "timesWatched", 0);
    cJSON_AddNumberToObject(st, "flaggedWatched", 0);
    cJSON_AddNumberToObject(st, "duration", 0);
    cJSON_AddItemToObject(st, "video_id", cJSON_CreateNull());
    cJSON_AddItemToObject(st, "watched", cJSON_CreateNull());
    cJSON_AddItemToObject(st, "noNotif", cJSON_CreateFalse());
    cJSON_AddItemToObject(it, "state", st);
    cJSON *bh = cJSON_CreateObject();
    cJSON_AddItemToObject(bh, "defaultVideoId", cJSON_CreateNull());
    cJSON_AddItemToObject(bh, "featuredVideoId", cJSON_CreateNull());
    cJSON_AddItemToObject(bh, "hasScheduledVideos", cJSON_CreateFalse());
    cJSON_AddItemToObject(it, "behaviorHints", bh);
    return it;
}

static char    g_prog_id[160];
static int64_t g_prog_last_push;      /* unix seconds of the last push */
static int64_t g_prog_last_pos;
static int     g_prog_finished;       /* this session already counted as watched */

static void on_put(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)body; (void)len; (void)ud;
    if (!ok || status != 200)
        PROV_LOG("stremio account: progress not saved (ok=%d http=%d)", ok, status);
}

void evo_sa_progress(const char *lib_id, const char *type, const char *video_id,
                     const char *name, const char *poster,
                     int64_t pos_sec, int64_t dur_sec, int stopped)
{
    if (!g_key[0] || !lib_id || !lib_id[0] || strlen(lib_id) >= sizeof g_prog_id)
        return;
    if (!g_library)
        g_library = cJSON_CreateArray();
    if (!g_library)
        return;

    const int64_t now = (int64_t)time(NULL);
    const int same = !strcmp(g_prog_id, lib_id);
    if (!same) {
        snprintf(g_prog_id, sizeof g_prog_id, "%s", lib_id);
        g_prog_last_push = 0;
        g_prog_last_pos = pos_sec;
        g_prog_finished = 0;
    }
    /* At most once a minute while playing; at once on stop. */
    if (!stopped && same && now - g_prog_last_push < 60)
        return;

    char iso[32];
    evo_sa_iso8601(now, iso);

    cJSON *it = library_find(lib_id);
    if (!it) {
        it = new_item(lib_id, type, name, poster, iso);
        cJSON_AddItemToArray(g_library, it);
    }
    cJSON *st = cJSON_GetObjectItem(it, "state");
    if (!st) {
        st = cJSON_CreateObject();
        cJSON_AddItemToObject(it, "state", st);
    }

    int64_t watched_delta = pos_sec - g_prog_last_pos;
    if (watched_delta < 0 || watched_delta > 120)
        watched_delta = 0;                       /* a seek, not watching */
    g_prog_last_pos = pos_sec;

    const int finished = dur_sec > 0 && pos_sec >= dur_sec * 9 / 10;
    set_num(st, "timeWatched", num(st, "timeWatched") + (double)watched_delta * 1000.0);
    set_num(st, "overallTimeWatched", num(st, "overallTimeWatched") + (double)watched_delta * 1000.0);
    set_num(st, "duration", dur_sec > 0 ? (double)dur_sec * 1000.0 : num(st, "duration"));
    set_str(st, "lastWatched", iso);
    if (video_id && video_id[0])
        set_str(st, "video_id", video_id);
    if (finished) {
        /* Done: out of Continue Watching, counted once. */
        set_num(st, "timeOffset", 0);
        if (!g_prog_finished) {
            set_num(st, "timesWatched", num(st, "timesWatched") + 1);
            if (type && !strcmp(type, "movie"))
                set_num(st, "flaggedWatched", 1);
            g_prog_finished = 1;
        }
    } else {
        set_num(st, "timeOffset", (double)(pos_sec > 0 ? pos_sec : 1) * 1000.0);
    }
    set_str(it, "_mtime", iso);

    /* {"authKey","collection":"libraryItem","changes":[item]} */
    char *item_txt = cJSON_PrintUnformatted(it);
    if (!item_txt)
        return;
    size_t blen = strlen(item_txt) + 256;
    char *body = (char *)malloc(blen);
    if (body) {
        snprintf(body, blen, "{\"authKey\":\"%s\",\"collection\":\"libraryItem\",\"changes\":[%s]}",
                 g_key, item_txt);
        if (api_post("datastorePut", body, on_put, NULL) == 0)
            g_prog_last_push = now;
        free(body);
    }
    free(item_txt);
    if (stopped)
        write_json(SA_FILE_LIBRARY, g_library);
}
