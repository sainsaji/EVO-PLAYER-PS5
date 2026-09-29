/*
 * provider_debrid.c — Torbox and Real-Debrid as resolvers.
 *
 * The roadmap's two "resolver only" stories: CAP_RESOLVE and nothing else. A
 * catalog provider (Nuvio) hands evo_provider_resolve_chain() a stream whose
 * url is a magnet with needs_resolver set; the chain offers that magnet to
 * each enabled resolver in turn, and the first to return an http(s) URL wins.
 * Neither side knows the other exists - a service that does not have this
 * torrent cached answers ok = 0 and the chain moves on.
 *
 * The request sequences are NuvioTV's (core/debrid/TorboxDirectDebridResolver
 * and RealDebridDirectDebridResolver), including "cached only": nothing here
 * starts a download and waits for it. An uncached torrent is a failure the
 * user sees at once, not a spinner that ends in a timeout.
 *
 * Each resolve is a heap job driven by evo_net callbacks on the main thread;
 * it fires the caller's callback exactly once, from finish(), and frees
 * itself there.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "evo_provider.h"
#include "evo_provider_log.h"
#include "evo_net.h"
#include "evo_data_path.h"
#include "nuvio_stremio.h"
#include "nuvio_debrid.h"
#include "cJSON.h"

#define DEBRID_MAX_FILES   256
#define RD_INFO_RETRIES    3

/* ------------------------------------------------------------------------- */
/* Keys                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *conf;       /* file under the data root */
    char key[128];
    char masked[32];
} debrid_key_t;

static debrid_key_t g_tb = { "torbox.conf", "", "" };
static debrid_key_t g_rd = { "realdebrid.conf", "", "" };

static void key_load(debrid_key_t *k)
{
    k->key[0] = '\0';
    FILE *f = fopen(evo_data_path(k->conf), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "key=", 4))
            snprintf(k->key, sizeof k->key, "%s", line + 4);
    }
    fclose(f);
}

/* An API key is a bearer credential: accept only the characters one can
 * contain, so a stray space or newline from the keyboard is refused rather
 * than sent in an Authorization header. */
static int key_set(debrid_key_t *k, const char *value)
{
    if (!value) return -1;
    size_t n = strlen(value);
    if (n >= sizeof k->key) return -1;
    for (size_t i = 0; i < n; ++i) {
        char c = value[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) return -1;
    }
    FILE *f = fopen(evo_data_path(k->conf), "w");
    if (!f) return -1;
    if (n) fprintf(f, "key=%s\n", value);
    fclose(f);
    snprintf(k->key, sizeof k->key, "%s", value);
    return 0;
}

/* Never pre-fill the keyboard with the key itself: it would sit on screen. */
static const char *key_masked(debrid_key_t *k)
{
    k->masked[0] = '\0';
    return k->masked;
}

/* ------------------------------------------------------------------------- */
/* The job                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct job {
    int  rd;                                /* 0 Torbox, 1 Real-Debrid       */
    char magnet[EVO_PROVIDER_MAX_URL];      /* hints stripped                */
    nuvio_magnet_hints_t hints;
    char auth[176];                         /* "Authorization: Bearer …"     */
    char torrent_id[64];
    int64_t file_id;
    char file_name[256];
    int64_t file_size;
    int  retries;
    evo_provider_resolve_cb cb;
    void *ud;
} job_t;

static void finish(job_t *j, const char *url)
{
    evo_stream_choice_t c;
    int ok = 0;
    if (url && (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8)) &&
        strlen(url) < sizeof c.url) {
        evo_provider_stream_choice_clear(&c);
        snprintf(c.url, sizeof c.url, "%s", url);
        snprintf(c.label, sizeof c.label, "%s", j->rd ? "Real-Debrid" : "Torbox");
        c.size_bytes = j->file_size;
        const char *dot = strrchr(j->file_name, '.');
        if (dot && strlen(dot + 1) < sizeof c.container)
            snprintf(c.container, sizeof c.container, "%s", dot + 1);
        ok = 1;
    }
    PROV_LOG("%s: resolve %s (%s)", j->rd ? "realdebrid" : "torbox",
             ok ? "ok" : "FAILED", j->file_name[0] ? j->file_name : "-");
    evo_provider_resolve_cb cb = j->cb;
    void *ud = j->ud;
    free(j);
    if (cb) cb(ok, ok ? &c : NULL, ok ? 1 : 0, ud);
}

static int send_req(job_t *j, const char *method, const char *url, const char *body,
                    const char *content_type, evo_net_cb cb)
{
    const char *h[2];
    int n = 0;
    char ct[128];
    h[n++] = j->auth;
    if (content_type) {
        snprintf(ct, sizeof ct, "Content-Type: %s", content_type);
        h[n++] = ct;
    }
    return evo_net_request_async(method, url, body, h, n, cb, j);
}

static cJSON *parse_body(const char *body, size_t len)
{
    return (body && len) ? cJSON_ParseWithLength(body, len) : NULL;
}

static void fill_file_from_selection(job_t *j, const nuvio_debrid_file_t *f)
{
    j->file_id = f->id;
    j->file_size = f->size;
    const char *slash = strrchr(f->path, '/');
    snprintf(j->file_name, sizeof j->file_name, "%s", slash ? slash + 1 : f->path);
}

/* ------------------------------------------------------------------------- */
/* Torbox                                                                    */
/* ------------------------------------------------------------------------- */

/* The API roots. A host build may point them at a local fake
 * (tests/test_runner.c); the app module cannot be talked out of the real ones. */
static const char *api_root(const char *env, const char *real)
{
#ifndef EVO_APP_MODULE
    const char *o = getenv(env);
    if (o && *o) return o;
#else
    (void)env;
#endif
    return real;
}
#define TB_API_ROOT api_root("EVO_TORBOX_API", "https://api.torbox.app/v1/api")
#define RD_API_ROOT api_root("EVO_REALDEBRID_API", "https://api.real-debrid.com/rest/1.0")
#define TB_BOUNDARY "----EVOPlayerTorbox7d31a2"

static void tb_on_link(int ok, int status, const char *body, size_t len, void *ud)
{
    job_t *j = (job_t *)ud;
    (void)status;
    char url[EVO_PROVIDER_MAX_URL] = "";
    cJSON *r = ok ? parse_body(body, len) : NULL;
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(r, "data");
    if (cJSON_IsString(d) && d->valuestring) snprintf(url, sizeof url, "%s", d->valuestring);
    cJSON_Delete(r);
    finish(j, url[0] ? url : NULL);
}

static void tb_on_torrent(int ok, int status, const char *body, size_t len, void *ud)
{
    job_t *j = (job_t *)ud;
    (void)status;
    cJSON *r = ok ? parse_body(body, len) : NULL;
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(r, "data");
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(data, "files");
    if (!cJSON_IsArray(files)) { cJSON_Delete(r); finish(j, NULL); return; }

    static nuvio_debrid_file_t fl[DEBRID_MAX_FILES];
    int n = 0;
    for (const cJSON *f = files->child; f && n < DEBRID_MAX_FILES; f = f->next) {
        nuvio_debrid_file_t *d = &fl[n];
        memset(d, 0, sizeof *d);
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(f, "id");
        if (!cJSON_IsNumber(id)) continue;
        d->id = (int64_t)id->valuedouble;
        const char *keys[] = { "name", "absolute_path", "short_name" };
        for (int k = 0; k < 3 && !d->path[0]; ++k) {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(f, keys[k]);
            if (cJSON_IsString(v) && v->valuestring && v->valuestring[0])
                snprintf(d->path, sizeof d->path, "%s", v->valuestring);
        }
        const cJSON *sz = cJSON_GetObjectItemCaseSensitive(f, "size");
        d->size = cJSON_IsNumber(sz) ? (int64_t)sz->valuedouble : 0;
        const cJSON *mt = cJSON_GetObjectItemCaseSensitive(f, "mimetype");
        d->is_video = (cJSON_IsString(mt) && mt->valuestring &&
                       !strncmp(mt->valuestring, "video/", 6)) ||
                      nuvio_debrid_is_video_name(d->path);
        ++n;
    }
    cJSON_Delete(r);

    int pick = nuvio_debrid_select(fl, n, &j->hints);
    if (pick < 0) { finish(j, NULL); return; }
    fill_file_from_selection(j, &fl[pick]);

    char url[768];
    snprintf(url, sizeof url,
             "%s/torrents/requestdl?token=%s&torrent_id=%s&file_id=%lld"
             "&zip_link=false&redirect=false&append_name=false",
             TB_API_ROOT, g_tb.key, j->torrent_id, (long long)j->file_id);
    if (send_req(j, "GET", url, NULL, NULL, tb_on_link) != 0) finish(j, NULL);
}

static void tb_on_create(int ok, int status, const char *body, size_t len, void *ud)
{
    job_t *j = (job_t *)ud;
    if (!ok) {
        /* 409 is Torbox's "not cached" with add_only_if_cached. */
        PROV_LOG("torbox: createtorrent status=%d%s", status,
                 status == 409 ? " (not cached)" : "");
        finish(j, NULL);
        return;
    }
    cJSON *r = parse_body(body, len);
    const cJSON *succ = cJSON_GetObjectItemCaseSensitive(r, "success");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(r, "data");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(data, "torrent_id");
    if (!cJSON_IsNumber(id)) id = cJSON_GetObjectItemCaseSensitive(data, "id");
    if (cJSON_IsFalse(succ) || !cJSON_IsNumber(id)) {
        cJSON_Delete(r);
        finish(j, NULL);
        return;
    }
    snprintf(j->torrent_id, sizeof j->torrent_id, "%lld", (long long)id->valuedouble);
    cJSON_Delete(r);

    char url[256];
    snprintf(url, sizeof url, "%s/torrents/mylist?id=%s&bypass_cache=true", TB_API_ROOT, j->torrent_id);
    if (send_req(j, "GET", url, NULL, NULL, tb_on_torrent) != 0) finish(j, NULL);
}

/* A text-only multipart body: the three fields NuvioTV sends. */
static char *tb_multipart(const char *magnet)
{
    size_t cap = strlen(magnet) + 512;
    char *b = (char *)malloc(cap);
    if (!b) return NULL;
    snprintf(b, cap,
             "--" TB_BOUNDARY "\r\nContent-Disposition: form-data; name=\"magnet\"\r\n\r\n%s\r\n"
             "--" TB_BOUNDARY "\r\nContent-Disposition: form-data; name=\"add_only_if_cached\"\r\n\r\ntrue\r\n"
             "--" TB_BOUNDARY "\r\nContent-Disposition: form-data; name=\"allow_zip\"\r\n\r\nfalse\r\n"
             "--" TB_BOUNDARY "--\r\n",
             magnet);
    return b;
}

/* ------------------------------------------------------------------------- */
/* Real-Debrid                                                               */
/* ------------------------------------------------------------------------- */


static void rd_forget(job_t *j)
{
    /* Clean up a torrent this resolve added but could not use, as NuvioTV's
     * finally block does. Fire and forget. */
    if (!j->torrent_id[0]) return;
    char url[256];
    snprintf(url, sizeof url, "%s/torrents/delete/%s", RD_API_ROOT, j->torrent_id);
    const char *h[1] = { j->auth };
    evo_net_request_async("DELETE", url, NULL, h, 1, NULL, NULL);
}

static void rd_fail(job_t *j)
{
    rd_forget(j);
    finish(j, NULL);
}

static void rd_on_unrestrict(int ok, int status, const char *body, size_t len, void *ud)
{
    job_t *j = (job_t *)ud;
    (void)status;
    char url[EVO_PROVIDER_MAX_URL] = "";
    cJSON *r = ok ? parse_body(body, len) : NULL;
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(r, "download");
    if (cJSON_IsString(d) && d->valuestring) snprintf(url, sizeof url, "%s", d->valuestring);
    const cJSON *fs = cJSON_GetObjectItemCaseSensitive(r, "filesize");
    if (cJSON_IsNumber(fs)) j->file_size = (int64_t)fs->valuedouble;
    cJSON_Delete(r);
    if (!url[0]) { rd_fail(j); return; }
    finish(j, url);
}

static void rd_request_info(job_t *j);

static void rd_on_info_after(int ok, int status, const char *body, size_t len, void *ud)
{
    job_t *j = (job_t *)ud;
    (void)status;
    cJSON *r = ok ? parse_body(body, len) : NULL;
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(r, "status");
    const cJSON *links = cJSON_GetObjectItemCaseSensitive(r, "links");
    int downloaded = cJSON_IsString(st) && st->valuestring &&
                     !strcmp(st->valuestring, "downloaded");
    char link[1024] = "";
    if (downloaded && cJSON_IsArray(links) && links->child &&
        cJSON_IsString(links->child) && links->child->valuestring)
        snprintf(link, sizeof link, "%s", links->child->valuestring);
    int transient = cJSON_IsString(st) && st->valuestring &&
                    (!strcmp(st->valuestring, "queued") ||
                     !strcmp(st->valuestring, "magnet_conversion") ||
                     !strcmp(st->valuestring, "waiting_files_selection"));
    cJSON_Delete(r);

    if (!link[0]) {
        /* A cached torrent reports "downloaded" within a moment of the file
         * selection; give it a couple of round trips before calling it
         * uncached. */
        if (transient && ++j->retries <= RD_INFO_RETRIES) { rd_request_info(j); return; }
        rd_fail(j);
        return;
    }

    char esc[1024 * 3];
    if (evo_provider_url_escape(link, esc, sizeof esc) < 0) { rd_fail(j); return; }
    char form[sizeof esc + 8];
    snprintf(form, sizeof form, "link=%s", esc);
    char uurl[256];
    snprintf(uurl, sizeof uurl, "%s/unrestrict/link", RD_API_ROOT);
    if (send_req(j, "POST", uurl, form,
                 "application/x-www-form-urlencoded", rd_on_unrestrict) != 0)
        rd_fail(j);
}

static void rd_request_info(job_t *j)
{
    char url[256];
    snprintf(url, sizeof url, "%s/torrents/info/%s", RD_API_ROOT, j->torrent_id);
    if (send_req(j, "GET", url, NULL, NULL, rd_on_info_after) != 0) rd_fail(j);
}

static void rd_on_select(int ok, int status, const char *body, size_t len, void *ud)
{
    job_t *j = (job_t *)ud;
    (void)body; (void)len;
    if (!ok && status != 202) { rd_fail(j); return; }
    rd_request_info(j);
}

static void rd_on_info_before(int ok, int status, const char *body, size_t len, void *ud)
{
    job_t *j = (job_t *)ud;
    (void)status;
    cJSON *r = ok ? parse_body(body, len) : NULL;
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(r, "files");
    if (!cJSON_IsArray(files)) { cJSON_Delete(r); rd_fail(j); return; }

    static nuvio_debrid_file_t fl[DEBRID_MAX_FILES];
    int n = 0;
    for (const cJSON *f = files->child; f && n < DEBRID_MAX_FILES; f = f->next) {
        nuvio_debrid_file_t *d = &fl[n];
        memset(d, 0, sizeof *d);
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(f, "id");
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(f, "path");
        const cJSON *b = cJSON_GetObjectItemCaseSensitive(f, "bytes");
        if (!cJSON_IsNumber(id) || !cJSON_IsString(p) || !p->valuestring) continue;
        d->id = (int64_t)id->valuedouble;
        snprintf(d->path, sizeof d->path, "%s", p->valuestring);
        d->size = cJSON_IsNumber(b) ? (int64_t)b->valuedouble : 0;
        d->is_video = nuvio_debrid_is_video_name(d->path);
        ++n;
    }
    cJSON_Delete(r);

    int pick = nuvio_debrid_select(fl, n, &j->hints);
    if (pick < 0) { rd_fail(j); return; }
    fill_file_from_selection(j, &fl[pick]);

    char url[256], form[64];
    snprintf(url, sizeof url, "%s/torrents/selectFiles/%s", RD_API_ROOT, j->torrent_id);
    snprintf(form, sizeof form, "files=%lld", (long long)j->file_id);
    if (send_req(j, "POST", url, form, "application/x-www-form-urlencoded", rd_on_select) != 0)
        rd_fail(j);
}

static void rd_on_add(int ok, int status, const char *body, size_t len, void *ud)
{
    job_t *j = (job_t *)ud;
    cJSON *r = ok ? parse_body(body, len) : NULL;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(r, "id");
    if (cJSON_IsString(id) && id->valuestring)
        snprintf(j->torrent_id, sizeof j->torrent_id, "%s", id->valuestring);
    cJSON_Delete(r);
    if (!j->torrent_id[0]) {
        PROV_LOG("realdebrid: addMagnet status=%d", status);
        finish(j, NULL);
        return;
    }
    char url[256];
    snprintf(url, sizeof url, "%s/torrents/info/%s", RD_API_ROOT, j->torrent_id);
    if (send_req(j, "GET", url, NULL, NULL, rd_on_info_before) != 0) rd_fail(j);
}

/* ------------------------------------------------------------------------- */
/* Shared entry                                                              */
/* ------------------------------------------------------------------------- */

static int start(int rd, const char *item_id, evo_provider_resolve_cb cb, void *ud)
{
    debrid_key_t *k = rd ? &g_rd : &g_tb;
    if (!k->key[0] || !item_id) return -1;

    job_t *j = (job_t *)calloc(1, sizeof *j);
    if (!j) return -2;
    if (nuvio_magnet_parse(item_id, &j->hints) != 0 ||
        nuvio_magnet_strip_hints(item_id, j->magnet, sizeof j->magnet) != 0) {
        free(j);
        return -1;      /* not a magnet: not ours, the chain moves on */
    }
    j->rd = rd;
    j->cb = cb;
    j->ud = ud;
    snprintf(j->auth, sizeof j->auth, "Authorization: Bearer %s", k->key);

    int rc;
    if (rd) {
        char esc[EVO_PROVIDER_MAX_URL * 3];
        if (evo_provider_url_escape(j->magnet, esc, sizeof esc) < 0) { free(j); return -1; }
        size_t fl = strlen(esc) + 8;
        char *form = (char *)malloc(fl);
        if (!form) { free(j); return -2; }
        snprintf(form, fl, "magnet=%s", esc);
        char aurl[256];
        snprintf(aurl, sizeof aurl, "%s/torrents/addMagnet", RD_API_ROOT);
        rc = send_req(j, "POST", aurl, form,
                      "application/x-www-form-urlencoded", rd_on_add);
        free(form);
    } else {
        char *mp = tb_multipart(j->magnet);
        if (!mp) { free(j); return -2; }
        char curl[256];
        snprintf(curl, sizeof curl, "%s/torrents/createtorrent", TB_API_ROOT);
        rc = send_req(j, "POST", curl, mp,
                      "multipart/form-data; boundary=" TB_BOUNDARY, tb_on_create);
        free(mp);
    }
    if (rc != 0) { free(j); return -3; }
    PROV_LOG("%s: resolving btih %s", rd ? "realdebrid" : "torbox", j->hints.info_hash);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Vtables                                                                   */
/* ------------------------------------------------------------------------- */

static int  tb_init(void) { key_load(&g_tb); return 0; }
static int  rd_init(void) { key_load(&g_rd); return 0; }
static void debrid_shutdown(void) { }
static int  tb_is_configured(void) { return g_tb.key[0] != 0; }
static int  rd_is_configured(void) { return g_rd.key[0] != 0; }
static int  tb_resolve(const char *id, evo_provider_resolve_cb cb, void *ud) { return start(0, id, cb, ud); }
static int  rd_resolve(const char *id, evo_provider_resolve_cb cb, void *ud) { return start(1, id, cb, ud); }
static const char *tb_get_source(void) { return key_masked(&g_tb); }
static const char *rd_get_source(void) { return key_masked(&g_rd); }
static int  tb_set_source(const char *v) { return key_set(&g_tb, v); }
static int  rd_set_source(const char *v) { return key_set(&g_rd, v); }

const evo_provider_t evo_provider_torbox = {
    .id            = "torbox",
    .name          = "Torbox",
    .icon          = NULL,
    .caps          = EVO_PROVIDER_CAP_RESOLVE | EVO_PROVIDER_CAP_CONFIG,
    .api_version   = EVO_PROVIDER_API_VERSION,
    .init          = tb_init,
    .shutdown      = debrid_shutdown,
    .is_configured = tb_is_configured,
    .resolve       = tb_resolve,
    .get_source    = tb_get_source,
    .set_source    = tb_set_source,
    .source_prompt = "Torbox API key (torbox.app/settings) - clear to remove",
};

const evo_provider_t evo_provider_realdebrid = {
    .id            = "realdebrid",
    .name          = "Real-Debrid",
    .icon          = NULL,
    .caps          = EVO_PROVIDER_CAP_RESOLVE | EVO_PROVIDER_CAP_CONFIG,
    .api_version   = EVO_PROVIDER_API_VERSION,
    .init          = rd_init,
    .shutdown      = debrid_shutdown,
    .is_configured = rd_is_configured,
    .resolve       = rd_resolve,
    .get_source    = rd_get_source,
    .set_source    = rd_set_source,
    .source_prompt = "Real-Debrid API token (real-debrid.com/apitoken) - clear to remove",
};
