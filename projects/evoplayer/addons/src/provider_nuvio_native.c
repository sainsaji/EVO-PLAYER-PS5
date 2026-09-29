/*
 * provider_nuvio_native.c — Nuvio, natively: Stremio addons behind the seam.
 *
 * What NuvioTV does for catalogs, metadata, streams and watch progress,
 * reimplemented against evo_provider_t (docs/addons/nuvio-native-provider.md).
 * The web-UI provider (provider_nuvio.c, "Nuvio (Web)") is untouched and still
 * there; this one is id "nuvio-native".
 *
 * THE TREE
 *
 * The seam's model is one list per level, so NuvioTV's home rows become
 * folders. Every id below is opaque to EVO and parsed only here:
 *
 *   ""                              root: Continue Watching + one per catalog
 *   "cw"                            Continue Watching
 *   "c:<addon>:<type>:<catalog>"    a catalog, paged by skip
 *   "m:<type>:<meta>"               a title: seasons, or straight to streams
 *   "s:<season>:<type>:<meta>"      a season's episodes
 *   "v:<type>:<meta>|<video>"       an episode: its streams
 *   "x:<gen>:<index>"               a stream - the only playable rows
 *
 * A stream row's id indexes the provider's own table for the last streams
 * level, because a stream URL does not fit an item id. The generation makes a
 * stale id fail cleanly instead of playing the wrong stream.
 *
 * FILES, under /data/evoplayer/providers/nuvio-native/
 *
 *   addons.txt       one manifest URL per line, in order (FTP it, or add one
 *                    from the provider's setup prompt). Absent: Cinemeta.
 *   m_<hash>.json    each addon's manifest, so the root lists without network
 *   progress.json    watch progress (nuvio_progress.h)
 *   account.conf     optional Nuvio account (nuvio_account.h)
 *   bundle.txt       optional UI bundle URL; absent: the fallback skin
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "evo_provider.h"
#include "evo_provider_log.h"
#include "evo_net.h"
#include "evo_data_path.h"
#include "nuvio_stremio.h"
#include "nuvio_progress.h"
#include "nuvio_account.h"
#include "nuvio_json.h"
#include "cJSON.h"

#define NV_ID              "nuvio-native"
#define NV_DIR             "providers/" NV_ID
#define NV_MAX_ADDONS      24
#define NV_MAX_STREAMS     200
#define NV_MAX_SEARCH_REQ  12
#define NV_REFRESH_MS      (12LL * 60 * 60 * 1000)
#define NV_SAVE_EVERY_MS   (60LL * 1000)
#define NV_DEFAULT_ADDON   "https://v3-cinemeta.strem.io/manifest.json"
/* Socket idle timeouts. Scraping stream addons (Torrentio on a cold title,
 * AIOStreams fanning out to its own sources) think well past evo_net's 6 s
 * default before sending a byte; catalogs and metadata come from caches. */
#define NV_STREAM_TIMEOUT_S 20
#define NV_SEARCH_TIMEOUT_S 15

/* The streams level's subject: what is being chosen a stream for. */
typedef struct {
    char type[24];
    char meta_id[128];
    char video_id[160];
    int  season;
    int  episode;
    char name[192];         /* the show or film   */
    char ep_title[192];     /* the episode, if any */
    char poster[384];
} nv_ctx_t;

static struct {
    char            bases[NV_MAX_ADDONS][NUVIO_MAX_BASE_URL];
    int             count;
    nuvio_addon_t  *addons;                 /* [NV_MAX_ADDONS], heap */
    int             loaded[NV_MAX_ADDONS];
    int64_t         manifests_ms;
    int             refreshing;

    nuvio_meta_t    meta;                   /* last title opened */
    int             meta_valid;
    char            meta_type[24];

    evo_provider_item_t *recent;            /* last catalog/search page */
    int             recent_count;

    nuvio_stream_t *streams;                /* the last streams level */
    int             stream_count;
    unsigned        gen;
    nv_ctx_t        sctx;

    nv_ctx_t        now;                    /* what is playing */
    int             now_stream;
    int             now_active;
    int64_t         now_saved_ms;

    char            cat_key[256];
    int             cat_next_skip;

    nuvio_progress_t progress;
    char            bundle[512];

    int             account_synced;         /* this session */
} G;

static char g_title[400];

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static int64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static const char *nv_path(const char *leaf, char *out, size_t cap)
{
    char rel[256];
    snprintf(rel, sizeof rel, NV_DIR "/%s", leaf);
    snprintf(out, cap, "%s", evo_data_path(rel));
    return out;
}

static void nv_mkdirs(void)
{
    char p[512];
    snprintf(p, sizeof p, "%s", evo_data_path("providers"));
    evo_mkdir(p);
    snprintf(p, sizeof p, "%s", evo_data_path(NV_DIR));
    evo_mkdir(p);
}

static uint64_t fnv1a(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; ++s) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
    return h;
}

static void manifest_cache_path(const char *base, char *out, size_t cap)
{
    char leaf[40];
    snprintf(leaf, sizeof leaf, "m_%016llx.json", (unsigned long long)fnv1a(base));
    nv_path(leaf, out, cap);
}

static char *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 2 * 1024 * 1024) { fclose(f); return NULL; }
    char *b = (char *)malloc((size_t)sz + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)sz, f);
    fclose(f);
    b[got] = '\0';
    if (len_out) *len_out = got;
    return b;
}

static void write_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(data, 1, len, f);
    fclose(f);
}

/*
 * Text for the UI font: newlines to a separator, and 4-byte UTF-8 (emoji -
 * addons decorate stream text with them) and variation selectors dropped. The
 * rest of UTF-8 is kept; accented titles are real text.
 */
static void ui_text(const char *in, char *out, size_t cap, const char *nl)
{
    size_t o = 0, nll = strlen(nl);
    const unsigned char *p = (const unsigned char *)in;
    while (*p && o + 1 < cap) {
        if (*p == '\n' || *p == '\r') {
            while (*p == '\n' || *p == '\r') ++p;
            if (o && *p && o + nll + 1 < cap) { memcpy(out + o, nl, nll); o += nll; }
            continue;
        }
        if (*p >= 0xF0) {                                   /* 4-byte sequence */
            p++;
            while ((*p & 0xC0) == 0x80) p++;
            continue;
        }
        if (p[0] == 0xEF && p[1] == 0xB8 && p[2] == 0x8F) { p += 3; continue; }  /* VS16 */
        if (p[0] == 0xE2 && p[1] == 0x80 && p[2] == 0x8D) { p += 3; continue; }  /* ZWJ  */
        out[o++] = (char)*p++;
    }
    /* collapse runs of spaces left behind */
    out[o] = '\0';
    char *w = out;
    for (char *r = out; *r; ++r) {
        if (*r == ' ' && (w == out || w[-1] == ' ')) continue;
        *w++ = *r;
    }
    while (w > out && w[-1] == ' ') --w;
    *w = '\0';
}

static const char *type_label(const char *type)
{
    if (!strcmp(type, "movie"))   return "Movies";
    if (!strcmp(type, "series"))  return "Series";
    if (!strcmp(type, "anime"))   return "Anime";
    if (!strcmp(type, "tv"))      return "TV";
    if (!strcmp(type, "channel")) return "Channels";
    return type;
}

static int resolver_enabled(void)
{
    return evo_provider_is_enabled("torbox") || evo_provider_is_enabled("realdebrid");
}

/* ------------------------------------------------------------------------- */
/* Addon list and manifests                                                  */
/* ------------------------------------------------------------------------- */

static int add_base(const char *manifest_url)
{
    char base[NUVIO_MAX_BASE_URL];
    if (nuvio_canonical_base(manifest_url, base, sizeof base) != 0) return -1;
    for (int i = 0; i < G.count; ++i)
        if (!strcmp(G.bases[i], base)) return 0;
    if (G.count >= NV_MAX_ADDONS) return -1;
    snprintf(G.bases[G.count], sizeof G.bases[0], "%s", base);
    G.loaded[G.count] = 0;
    G.count++;
    return 1;
}

static void save_addon_list(void)
{
    char p[512];
    FILE *f = fopen(nv_path("addons.txt", p, sizeof p), "w");
    if (!f) return;
    fprintf(f, "# Stremio addon manifest URLs, one per line, in order.\n");
    for (int i = 0; i < G.count; ++i) {
        char m[NUVIO_MAX_BASE_URL + 32];
        if (nuvio_manifest_url(G.bases[i], m, sizeof m) == 0) fprintf(f, "%s\n", m);
    }
    fclose(f);
}

static void load_cached_manifest(int i)
{
    char p[512];
    manifest_cache_path(G.bases[i], p, sizeof p);
    size_t len = 0;
    char *body = read_file(p, &len);
    if (!body) return;
    G.loaded[i] = nuvio_parse_manifest(body, len, G.bases[i], &G.addons[i]) == 0;
    free(body);
}

static void load_addon_list(void)
{
    G.count = 0;
    char p[512];
    FILE *f = fopen(nv_path("addons.txt", p, sizeof p), "r");
    if (f) {
        char line[NUVIO_MAX_BASE_URL + 64];
        while (fgets(line, sizeof line, f)) {
            char *s = line;
            while (*s == ' ' || *s == '\t') ++s;
            if (*s == '#' || *s == '\r' || *s == '\n' || !*s) continue;
            if (add_base(s) < 0) PROV_LOG("nuvio: ignoring addon line '%.60s'", s);
        }
        fclose(f);
    }
    if (G.count == 0) add_base(NV_DEFAULT_ADDON);
    for (int i = 0; i < G.count; ++i) load_cached_manifest(i);
}

/* A manifest fetch fan-out. `cb` NULL = a background refresh. */
typedef struct mf_job {
    int  pending;
    evo_provider_items_cb cb;
    void *ud;
} mf_job_t;

typedef struct mf_req {
    mf_job_t *job;
    char base[NUVIO_MAX_BASE_URL];
} mf_req_t;

static void emit_root(evo_provider_items_cb cb, void *ud);

static void mf_job_done(mf_job_t *job)
{
    G.manifests_ms = now_ms();
    G.refreshing = 0;
    if (job->cb) emit_root(job->cb, job->ud);
    free(job);
}

static void on_manifest(int ok, int status, const char *body, size_t len, void *ud)
{
    mf_req_t *r = (mf_req_t *)ud;
    mf_job_t *job = r->job;
    for (int i = 0; i < G.count; ++i) {
        if (strcmp(G.bases[i], r->base) != 0) continue;
        if (ok && body && nuvio_parse_manifest(body, len, G.bases[i], &G.addons[i]) == 0) {
            G.loaded[i] = 1;
            char p[512];
            manifest_cache_path(G.bases[i], p, sizeof p);
            write_file(p, body, len);
        } else {
            PROV_LOG("nuvio: manifest failed for addon %d (HTTP %d)", i, status);
        }
        break;
    }
    free(r);
    if (--job->pending == 0) mf_job_done(job);
}

/* Fetch manifests: every addon (`all`), or only those not yet loaded. */
static int fetch_manifests(int all, evo_provider_items_cb cb, void *ud)
{
    mf_job_t *job = (mf_job_t *)calloc(1, sizeof *job);
    if (!job) return -2;
    job->cb = cb;
    job->ud = ud;
    job->pending = 1;                 /* held until every request is queued */
    G.refreshing = 1;
    for (int i = 0; i < G.count; ++i) {
        if (!all && G.loaded[i]) continue;
        mf_req_t *r = (mf_req_t *)calloc(1, sizeof *r);
        char url[NUVIO_MAX_BASE_URL + 32];
        if (!r || nuvio_manifest_url(G.bases[i], url, sizeof url) != 0) { free(r); continue; }
        r->job = job;
        snprintf(r->base, sizeof r->base, "%s", G.bases[i]);
        job->pending++;
        if (evo_net_request_async("GET", url, NULL, NULL, 0, on_manifest, r) != 0) {
            job->pending--;
            free(r);
        }
    }
    if (--job->pending == 0) mf_job_done(job);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Account sync (phase 3)                                                    */
/* ------------------------------------------------------------------------- */

static void progress_save(void)
{
    char p[512];
    nuvio_progress_save(&G.progress, nv_path("progress.json", p, sizeof p));
}

static void on_progress_pushed(int ok, const char *body, size_t len, void *ud)
{
    (void)body; (void)len;
    char **keys = (char **)ud;
    if (ok) {
        for (int i = 0; keys && keys[i]; ++i) {
            nuvio_progress_entry_t *e = nuvio_progress_find_key(&G.progress, keys[i]);
            if (e) e->dirty = 0;
        }
        progress_save();
    }
    for (int i = 0; keys && keys[i]; ++i) free(keys[i]);
    free(keys);
}

/* WatchProgressSyncService.pushToRemote: dirty entries, NuvioTV's shape. */
static void push_progress(void)
{
    if (!nuvio_account_is_configured()) return;
    int n = 0;
    for (int i = 0; i < G.progress.count; ++i) if (G.progress.entries[i].dirty) ++n;
    if (!n) return;

    char **keys = (char **)calloc((size_t)n + 1, sizeof *keys);
    if (!keys) return;
    nuvio_jw_t w;
    nuvio_jw_init(&w);
    nuvio_jw_obj(&w, NULL);
    nuvio_jw_arr(&w, "p_entries");
    int k = 0;
    for (int i = 0; i < G.progress.count; ++i) {
        const nuvio_progress_entry_t *e = &G.progress.entries[i];
        if (!e->dirty) continue;
        nuvio_jw_obj(&w, NULL);
        nuvio_jw_str(&w, "content_id", e->content_id);
        nuvio_jw_str(&w, "content_type", e->content_type);
        nuvio_jw_str(&w, "video_id", e->video_id);
        if (e->season > 0 || e->episode > 0) {
            nuvio_jw_i64(&w, "season", e->season);
            nuvio_jw_i64(&w, "episode", e->episode);
        }
        nuvio_jw_i64(&w, "position", e->position_ms);
        nuvio_jw_i64(&w, "duration", e->duration_ms);
        nuvio_jw_i64(&w, "last_watched", e->last_watched_ms);
        nuvio_jw_str(&w, "progress_key", e->progress_key);
        nuvio_jw_end_obj(&w);
        size_t kl = strlen(e->progress_key) + 1;
        keys[k] = (char *)malloc(kl);
        if (keys[k]) { memcpy(keys[k], e->progress_key, kl); ++k; }
    }
    nuvio_jw_end_arr(&w);
    nuvio_jw_i64(&w, "p_profile_id", nuvio_account_profile());
    nuvio_jw_str(&w, "p_origin_client_id", nuvio_account_client_id());
    nuvio_jw_end_obj(&w);
    char *body = nuvio_jw_finish(&w);
    if (!body || nuvio_account_rpc("sync_push_watch_progress", body, on_progress_pushed, keys) != 0) {
        for (int i = 0; i < k; ++i) free(keys[i]);
        free(keys);
    }
    free(body);
}

static void on_progress_pulled(int ok, const char *body, size_t len, void *ud)
{
    (void)ud;
    if (!ok || !body) return;
    cJSON *arr = cJSON_ParseWithLength(body, len);
    int merged = 0;
    if (cJSON_IsArray(arr)) {
        for (const cJSON *o = arr->child; o; o = o->next) {
            nuvio_progress_entry_t e;
            memset(&e, 0, sizeof e);
            const cJSON *v;
#define S(k, f) v = cJSON_GetObjectItemCaseSensitive(o, k); \
                if (cJSON_IsString(v) && v->valuestring) snprintf(e.f, sizeof e.f, "%s", v->valuestring)
#define N(k)    ((v = cJSON_GetObjectItemCaseSensitive(o, k)), cJSON_IsNumber(v) ? (int64_t)v->valuedouble : 0)
            S("progress_key", progress_key);
            S("content_id", content_id);
            S("content_type", content_type);
            S("video_id", video_id);
            e.season          = (int)N("season");
            e.episode         = (int)N("episode");
            e.position_ms     = N("position");
            e.duration_ms     = N("duration");
            e.last_watched_ms = N("last_watched");
#undef S
#undef N
            if (!e.progress_key[0] || !e.content_id[0]) continue;
            if (!e.video_id[0]) snprintf(e.video_id, sizeof e.video_id, "%s", e.content_id);
            nuvio_progress_upsert(&G.progress, &e, 0);
            if (e.last_watched_ms > G.progress.last_pull_ms) G.progress.last_pull_ms = e.last_watched_ms;
            ++merged;
        }
    }
    cJSON_Delete(arr);
    PROV_LOG("nuvio: pulled %d progress entries from the account", merged);
    progress_save();
}

static void pull_progress(void)
{
    char body[128];
    snprintf(body, sizeof body, "{\"p_profile_id\":%d}", nuvio_account_profile());
    nuvio_account_rpc("sync_pull_watch_progress", body, on_progress_pulled, NULL);
}

/* The root waits for the account's addon list; everything else is async. */
typedef struct acct_job {
    evo_provider_items_cb cb;
    void *ud;
    char owner[64];
} acct_job_t;

static void root_continue(acct_job_t *j)
{
    evo_provider_items_cb cb = j->cb;
    void *ud = j->ud;
    free(j);
    G.account_synced = 1;
    pull_progress();
    push_progress();
    int missing = 0;
    for (int i = 0; i < G.count; ++i) if (!G.loaded[i]) missing = 1;
    if (missing) {
        if (fetch_manifests(0, cb, ud) != 0) emit_root(cb, ud);
    } else {
        emit_root(cb, ud);
    }
}

static void on_account_addons(int ok, const char *body, size_t len, void *ud)
{
    acct_job_t *j = (acct_job_t *)ud;
    cJSON *arr = (ok && body) ? cJSON_ParseWithLength(body, len) : NULL;
    if (cJSON_IsArray(arr) && arr->child) {
        /* AddonSyncService.getRemoteAddonUrls: the account is the source of
         * truth for the list and its order. Disabled ones are left out. */
        char keep[NV_MAX_ADDONS][NUVIO_MAX_BASE_URL];
        int kn = 0;
        for (const cJSON *o = arr->child; o && kn < NV_MAX_ADDONS; o = o->next) {
            const cJSON *u = cJSON_GetObjectItemCaseSensitive(o, "url");
            const cJSON *en = cJSON_GetObjectItemCaseSensitive(o, "enabled");
            if (!cJSON_IsString(u) || !u->valuestring || cJSON_IsFalse(en)) continue;
            if (nuvio_canonical_base(u->valuestring, keep[kn], sizeof keep[0]) == 0) ++kn;
        }
        if (kn) {
            /* Reuse loaded manifests for addons that stay. */
            static nuvio_addon_t old[NV_MAX_ADDONS];
            static char old_bases[NV_MAX_ADDONS][NUVIO_MAX_BASE_URL];
            int old_loaded[NV_MAX_ADDONS], old_n = G.count;
            memcpy(old, G.addons, sizeof(nuvio_addon_t) * NV_MAX_ADDONS);
            memcpy(old_bases, G.bases, sizeof old_bases);
            memcpy(old_loaded, G.loaded, sizeof old_loaded);
            G.count = 0;
            for (int i = 0; i < kn; ++i) {
                snprintf(G.bases[i], sizeof G.bases[0], "%s", keep[i]);
                G.loaded[i] = 0;
                for (int k = 0; k < old_n; ++k)
                    if (old_loaded[k] && !strcmp(old_bases[k], keep[i])) {
                        G.addons[i] = old[k];
                        G.loaded[i] = 1;
                        break;
                    }
                if (!G.loaded[i]) load_cached_manifest(i);
                G.count++;
            }
            save_addon_list();
            PROV_LOG("nuvio: %d addons from the account", kn);
        }
    }
    cJSON_Delete(arr);
    root_continue(j);
}

static void on_owner(int ok, const char *body, size_t len, void *ud)
{
    acct_job_t *j = (acct_job_t *)ud;
    /* get_sync_owner returns a JSON string: a linked device reads the owner's
     * rows, not its own. */
    cJSON *v = (ok && body) ? cJSON_ParseWithLength(body, len) : NULL;
    if (cJSON_IsString(v) && v->valuestring)
        snprintf(j->owner, sizeof j->owner, "%s", v->valuestring);
    cJSON_Delete(v);
    if (!j->owner[0]) { root_continue(j); return; }

    char q[400];
    snprintf(q, sizeof q,
             "addons?select=url,enabled,sort_order&user_id=eq.%s&profile_id=eq.%d"
             "&order=sort_order.asc",
             j->owner, nuvio_account_profile());
    if (nuvio_account_select(q, on_account_addons, j) != 0) root_continue(j);
}

static int root_with_account(evo_provider_items_cb cb, void *ud)
{
    acct_job_t *j = (acct_job_t *)calloc(1, sizeof *j);
    if (!j) return -2;
    j->cb = cb;
    j->ud = ud;
    if (nuvio_account_rpc("get_sync_owner", "{}", on_owner, j) != 0) {
        free(j);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Emitting levels                                                           */
/* ------------------------------------------------------------------------- */

static void remember_items(const evo_provider_item_t *items, int n)
{
    free(G.recent);
    G.recent = NULL;
    G.recent_count = 0;
    if (n <= 0) return;
    G.recent = (evo_provider_item_t *)malloc((size_t)n * sizeof *items);
    if (!G.recent) return;
    memcpy(G.recent, items, (size_t)n * sizeof *items);
    G.recent_count = n;
}

static const evo_provider_item_t *recent_find(const char *id)
{
    for (int i = 0; i < G.recent_count; ++i)
        if (!strcmp(G.recent[i].id, id)) return &G.recent[i];
    return NULL;
}

static void emit_root(evo_provider_items_cb cb, void *ud)
{
    int cap = 1;
    for (int i = 0; i < G.count; ++i) if (G.loaded[i]) cap += G.addons[i].catalog_count;
    evo_provider_item_t *it = (evo_provider_item_t *)calloc((size_t)cap, sizeof *it);
    if (!it) { cb(0, NULL, 0, 0, ud); return; }
    int n = 0;

    int cw[NUVIO_PROGRESS_MAX];
    int ncw = nuvio_progress_continue(&G.progress, cw, NUVIO_PROGRESS_MAX);
    if (ncw > 0) {
        evo_provider_item_clear(&it[n]);
        snprintf(it[n].id, sizeof it[n].id, "cw");
        snprintf(it[n].title, sizeof it[n].title, "Continue Watching");
        snprintf(it[n].subtitle, sizeof it[n].subtitle, "%d in progress", ncw);
        const nuvio_progress_entry_t *e = &G.progress.entries[cw[0]];
        snprintf(it[n].art_url, sizeof it[n].art_url, "%s", e->poster);
        it[n].is_folder = 1;
        ++n;
    }

    /* showInHome catalogs first, then the rest - NuvioTV's home, then its
     * discover. Catalogs that need an argument EVO has no UI for are left
     * out; search-only ones are reached through Search. */
    for (int pass = 0; pass < 2; ++pass) {
        for (int a = 0; a < G.count; ++a) {
            if (!G.loaded[a]) continue;
            const nuvio_addon_t *ad = &G.addons[a];
            for (int c = 0; c < ad->catalog_count && n < cap; ++c) {
                const nuvio_catalog_t *cat = &ad->catalogs[c];
                if (cat->needs_other_extra || cat->search_only) continue;
                if ((pass == 0) != (cat->show_in_home != 0)) continue;
                evo_provider_item_t *r = &it[n];
                evo_provider_item_clear(r);
                int w = snprintf(r->id, sizeof r->id, "c:%d:%s:%s", a, cat->type, cat->id);
                if (w < 0 || (size_t)w >= sizeof r->id) continue;
                snprintf(r->title, sizeof r->title, "%s - %s", cat->name, type_label(cat->type));
                snprintf(r->subtitle, sizeof r->subtitle, "%s", ad->name);
                r->is_folder = 1;
                ++n;
            }
        }
    }
    PROV_LOG("nuvio: root -> %d rows (%d addons)", n, G.count);
    cb(1, it, n, 0, ud);
    free(it);
}

static void emit_continue(evo_provider_items_cb cb, void *ud)
{
    int idx[NUVIO_PROGRESS_MAX];
    int n = nuvio_progress_continue(&G.progress, idx, NUVIO_PROGRESS_MAX);
    evo_provider_item_t *it = (evo_provider_item_t *)calloc((size_t)(n ? n : 1), sizeof *it);
    if (!it) { cb(0, NULL, 0, 0, ud); return; }
    int out = 0;
    for (int i = 0; i < n; ++i) {
        const nuvio_progress_entry_t *e = &G.progress.entries[idx[i]];
        evo_provider_item_t *r = &it[out];
        evo_provider_item_clear(r);
        int w = snprintf(r->id, sizeof r->id, "v:%s:%s|%s",
                         e->content_type[0] ? e->content_type : "movie",
                         e->content_id, e->video_id);
        if (w < 0 || (size_t)w >= sizeof r->id) continue;
        snprintf(r->title, sizeof r->title, "%s", e->name[0] ? e->name : e->content_id);
        int pct = e->duration_ms > 0 ? (int)(e->position_ms * 100 / e->duration_ms) : 0;
        if (e->season > 0 || e->episode > 0)
            snprintf(r->subtitle, sizeof r->subtitle, "S%02dE%02d  -  %d%% watched",
                     e->season, e->episode, pct);
        else
            snprintf(r->subtitle, sizeof r->subtitle, "%d%% watched", pct);
        snprintf(r->art_url, sizeof r->art_url, "%s", e->poster);
        r->duration_sec = e->duration_ms / 1000;
        r->resume_pos_sec = e->position_ms / 1000;
        r->is_folder = 1;
        ++out;
    }
    cb(1, it, out, 0, ud);
    free(it);
}

static void emit_seasons(evo_provider_items_cb cb, void *ud)
{
    int seasons[128];
    int ns = nuvio_meta_seasons(&G.meta, seasons, 128);
    evo_provider_item_t *it = (evo_provider_item_t *)calloc((size_t)(ns ? ns : 1), sizeof *it);
    if (!it) { cb(0, NULL, 0, 0, ud); return; }
    int n = 0;
    for (int i = 0; i < ns; ++i) {
        evo_provider_item_t *r = &it[n];
        evo_provider_item_clear(r);
        int w = snprintf(r->id, sizeof r->id, "s:%d:%s:%s", seasons[i], G.meta_type, G.meta.id);
        if (w < 0 || (size_t)w >= sizeof r->id) continue;
        if (seasons[i] > 0) snprintf(r->title, sizeof r->title, "Season %d", seasons[i]);
        else                snprintf(r->title, sizeof r->title, "Specials");
        int eps = 0;
        for (int v = 0; v < G.meta.video_count; ++v)
            if (G.meta.videos[v].season == seasons[i] ||
                (seasons[i] == 0 && G.meta.videos[v].season < 0)) ++eps;
        snprintf(r->subtitle, sizeof r->subtitle, "%s  -  %d episodes", G.meta.name, eps);
        snprintf(r->overview, sizeof r->overview, "%s", G.meta.description);
        snprintf(r->art_url, sizeof r->art_url, "%s", G.meta.poster);
        snprintf(r->backdrop_url, sizeof r->backdrop_url, "%s", G.meta.background);
        r->is_folder = 1;
        ++n;
    }
    cb(1, it, n, 0, ud);
    free(it);
}

static void emit_episodes(int season, evo_provider_items_cb cb, void *ud)
{
    int total = G.meta.video_count;
    evo_provider_item_t *it = (evo_provider_item_t *)calloc((size_t)(total ? total : 1), sizeof *it);
    if (!it) { cb(0, NULL, 0, 0, ud); return; }

    /* In episode order - addons do not all list videos[] sorted. */
    int *order = (int *)malloc((size_t)(total ? total : 1) * sizeof *order);
    int no = 0;
    if (!order) { free(it); cb(0, NULL, 0, 0, ud); return; }
    for (int v = 0; v < total; ++v) {
        int s = G.meta.videos[v].season;
        if (s == season || (season == 0 && s < 0)) order[no++] = v;
    }
    for (int i = 1; i < no; ++i) {
        int x = order[i], j = i - 1;
        while (j >= 0 && G.meta.videos[order[j]].episode > G.meta.videos[x].episode) {
            order[j + 1] = order[j];
            --j;
        }
        order[j + 1] = x;
    }

    int n = 0;
    for (int i = 0; i < no; ++i) {
        const nuvio_video_t *v = &G.meta.videos[order[i]];
        evo_provider_item_t *r = &it[n];
        evo_provider_item_clear(r);
        int w = snprintf(r->id, sizeof r->id, "v:%s:%s|%s", G.meta_type, G.meta.id, v->id);
        if (w < 0 || (size_t)w >= sizeof r->id) continue;
        char ep[192];
        ui_text(v->title[0] ? v->title : "Episode", ep, sizeof ep, " ");
        snprintf(r->title, sizeof r->title, "%d. %s", v->episode, ep);

        char date[16] = "";
        if (v->released[0]) snprintf(date, sizeof date, "%.10s", v->released);
        const nuvio_progress_entry_t *pe = nuvio_progress_find_video(&G.progress, v->id);
        if (pe && pe->duration_ms > 0) {
            int pct = (int)(pe->position_ms * 100 / pe->duration_ms);
            snprintf(r->subtitle, sizeof r->subtitle, "S%02dE%02d  %s  -  %s",
                     v->season, v->episode, date,
                     pct >= 90 ? "watched" : "in progress");
            r->resume_pos_sec = nuvio_progress_in_progress(pe) ? pe->position_ms / 1000 : 0;
        } else {
            snprintf(r->subtitle, sizeof r->subtitle, "S%02dE%02d  %s", v->season, v->episode, date);
        }
        ui_text(v->overview, r->overview, sizeof r->overview, " ");
        /* The show's poster, not the 16:9 episode still: a bundle lays art
         * out at one aspect, and a still squeezed into a poster frame reads
         * as broken. The still rides along as the backdrop. */
        snprintf(r->art_url, sizeof r->art_url, "%s", G.meta.poster[0] ? G.meta.poster : v->thumbnail);
        snprintf(r->backdrop_url, sizeof r->backdrop_url, "%s", v->thumbnail);
        r->duration_sec = G.meta.runtime_sec;
        r->is_folder = 1;
        ++n;
    }
    free(order);
    cb(1, it, n, 0, ud);
    free(it);
}

static void format_size(int64_t b, char *out, size_t cap)
{
    if (b <= 0) { out[0] = '\0'; return; }
    double g = (double)b / (1024.0 * 1024.0 * 1024.0);
    if (g >= 1.0) snprintf(out, cap, "%.1f GB", g);
    else          snprintf(out, cap, "%d MB", (int)((double)b / (1024.0 * 1024.0)));
}

static void emit_streams(evo_provider_items_cb cb, void *ud)
{
    int n = G.stream_count;
    evo_provider_item_t *it = (evo_provider_item_t *)calloc((size_t)(n ? n : 1), sizeof *it);
    if (!it) { cb(0, NULL, 0, 0, ud); return; }
    int have_resolver = resolver_enabled();

    for (int i = 0; i < n; ++i) {
        const nuvio_stream_t *s = &G.streams[i];
        evo_provider_item_t *r = &it[i];
        evo_provider_item_clear(r);
        snprintf(r->id, sizeof r->id, "x:%u:%d", G.gen, i);

        char name[160];
        ui_text(s->name[0] ? s->name : s->addon_name, name, sizeof name, " ");
        snprintf(r->title, sizeof r->title, "%s", name);

        char size[32], q[16] = "";
        format_size(s->size_bytes, size, sizeof size);
        if (s->quality) snprintf(q, sizeof q, "%s", s->quality >= 2160 ? "4K" : "");
        if (s->quality && s->quality < 2160) snprintf(q, sizeof q, "%dp", s->quality);
        const char *how = "";
        if (s->needs_headers)                        how = "  -  may not play (needs headers)";
        else if (s->needs_resolver && have_resolver) how = "  -  via debrid";
        else if (s->needs_resolver)                  how = "  -  torrent: set up Torbox or Real-Debrid";
        snprintf(r->subtitle, sizeof r->subtitle, "%s%s%s%s%s", s->addon_name,
                 q[0] ? "  -  " : "", q, size[0] ? "  -  " : "", size);
        size_t sl = strlen(r->subtitle);
        snprintf(r->subtitle + sl, sizeof r->subtitle - sl, "%s", how);

        ui_text(s->description, r->overview, sizeof r->overview, "  |  ");
        snprintf(r->art_url, sizeof r->art_url, "%s", G.sctx.poster);
        r->is_folder = 0;
    }
    PROV_LOG("nuvio: streams for %s -> %d", G.sctx.video_id, n);
    cb(1, it, n, 0, ud);
    free(it);
}

/* ------------------------------------------------------------------------- */
/* Streams: the fan-out                                                      */
/* ------------------------------------------------------------------------- */

typedef struct st_job {
    int  pending;
    nuvio_stream_t *acc;
    int  count;
    nv_ctx_t ctx;
    evo_provider_items_cb cb;
    void *ud;
} st_job_t;

typedef struct st_req {
    st_job_t *job;
    char addon_name[96];
} st_req_t;

static void st_job_done(st_job_t *j)
{
    nuvio_sort_streams(j->acc, j->count);
    free(G.streams);
    G.streams = j->acc;
    G.stream_count = j->count;
    G.gen++;
    G.sctx = j->ctx;
    emit_streams(j->cb, j->ud);
    free(j);
}

static void on_streams(int ok, int status, const char *body, size_t len, void *ud)
{
    st_req_t *r = (st_req_t *)ud;
    st_job_t *j = r->job;
    if (ok && body) {
        int added = nuvio_parse_streams(body, len, r->addon_name, j->ctx.season, j->ctx.episode,
                                        j->acc, NV_MAX_STREAMS, &j->count);
        PROV_LOG("nuvio: %s -> %d streams", r->addon_name, added < 0 ? 0 : added);
    } else {
        PROV_LOG("nuvio: %s streams failed (HTTP %d)", r->addon_name, status);
    }
    free(r);
    if (--j->pending == 0) st_job_done(j);
}

static int fetch_streams(const nv_ctx_t *ctx, evo_provider_items_cb cb, void *ud)
{
    st_job_t *j = (st_job_t *)calloc(1, sizeof *j);
    if (!j) return -2;
    j->acc = (nuvio_stream_t *)calloc(NV_MAX_STREAMS, sizeof *j->acc);
    if (!j->acc) { free(j); return -2; }
    j->ctx = *ctx;
    j->cb = cb;
    j->ud = ud;
    j->pending = 1;
    int asked = 0;
    for (int a = 0; a < G.count; ++a) {
        if (!G.loaded[a]) continue;
        if (!nuvio_addon_serves(&G.addons[a], "stream", ctx->type, ctx->video_id)) continue;
        char url[2048];
        if (nuvio_resource_url(G.bases[a], "stream", ctx->type, ctx->video_id, url, sizeof url) != 0)
            continue;
        st_req_t *r = (st_req_t *)calloc(1, sizeof *r);
        if (!r) continue;
        r->job = j;
        snprintf(r->addon_name, sizeof r->addon_name, "%s", G.addons[a].name);
        j->pending++;
        if (evo_net_request_async_timeout("GET", url, NULL, NULL, 0, NV_STREAM_TIMEOUT_S,
                                          on_streams, r) != 0) {
            j->pending--;
            free(r);
            continue;
        }
        ++asked;
    }
    PROV_LOG("nuvio: asking %d addons for streams of %s %s", asked, ctx->type, ctx->video_id);
    if (--j->pending == 0) st_job_done(j);
    return 0;
}

/* The context for an episode or a film, from whatever is known locally. */
static void build_ctx(const char *type, const char *meta_id, const char *video_id, nv_ctx_t *c)
{
    memset(c, 0, sizeof *c);
    snprintf(c->type, sizeof c->type, "%s", type);
    snprintf(c->meta_id, sizeof c->meta_id, "%s", meta_id);
    snprintf(c->video_id, sizeof c->video_id, "%s", video_id);

    if (G.meta_valid && !strcmp(G.meta.id, meta_id)) {
        snprintf(c->name, sizeof c->name, "%s", G.meta.name);
        snprintf(c->poster, sizeof c->poster, "%s", G.meta.poster);
        for (int i = 0; i < G.meta.video_count; ++i) {
            const nuvio_video_t *v = &G.meta.videos[i];
            if (strcmp(v->id, video_id) != 0) continue;
            c->season = v->season > 0 ? v->season : 0;
            c->episode = v->episode;
            ui_text(v->title, c->ep_title, sizeof c->ep_title, " ");
            break;
        }
    }
    if (!c->name[0]) {
        const nuvio_progress_entry_t *e = nuvio_progress_find_video(&G.progress, video_id);
        if (e) {
            snprintf(c->name, sizeof c->name, "%s", e->name);
            snprintf(c->poster, sizeof c->poster, "%s", e->poster);
            c->season = e->season;
            c->episode = e->episode;
        }
    }
    if (!c->name[0]) {
        char mid[200];
        snprintf(mid, sizeof mid, "m:%s:%s", type, meta_id);
        const evo_provider_item_t *it = recent_find(mid);
        if (it) {
            snprintf(c->name, sizeof c->name, "%s", it->title);
            snprintf(c->poster, sizeof c->poster, "%s", it->art_url);
        }
    }
    /* Cinemeta-style episode ids carry season and episode: "tt…:S:E". */
    if (!c->season && strcmp(meta_id, video_id) != 0) {
        const char *p = strrchr(video_id, ':');
        if (p && p > video_id) {
            const char *q = p - 1;
            while (q > video_id && *q != ':') --q;
            if (*q == ':') {
                c->season = atoi(q + 1);
                c->episode = atoi(p + 1);
            }
        }
    }
    if (!c->name[0]) snprintf(c->name, sizeof c->name, "%s", meta_id);
}

/* ------------------------------------------------------------------------- */
/* Meta                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct meta_req {
    char type[24];
    char id[160];
    int  season;         /* -1: the title level; >= 0: that season's episodes */
    evo_provider_items_cb cb;
    void *ud;
} meta_req_t;

static void meta_level(meta_req_t *q)
{
    if (!G.meta_valid) {
        q->cb(0, NULL, 0, 0, q->ud);
        return;
    }
    if (q->season >= 0) { emit_episodes(q->season, q->cb, q->ud); return; }

    /* A film, or a "series" with nothing to choose between: straight to its
     * streams, with the title as both meta and video id. */
    int single = G.meta.video_count == 0 ||
                 (G.meta.video_count == 1 && !strcmp(G.meta.videos[0].id, G.meta.id));
    if (single) {
        nv_ctx_t c;
        build_ctx(q->type, G.meta.id, G.meta.id, &c);
        if (fetch_streams(&c, q->cb, q->ud) != 0) q->cb(0, NULL, 0, 0, q->ud);
        return;
    }
    emit_seasons(q->cb, q->ud);
}

static void on_meta(int ok, int status, const char *body, size_t len, void *ud)
{
    meta_req_t *q = (meta_req_t *)ud;
    nuvio_meta_t m;
    if (ok && body && nuvio_parse_meta(body, len, &m) == 0) {
        nuvio_meta_free(&G.meta);
        G.meta = m;
        G.meta_valid = 1;
        snprintf(G.meta_type, sizeof G.meta_type, "%s", q->type);
        /* The request id is the one the tree uses; an addon that echoes a
         * different one would otherwise break the next level's lookup. */
        snprintf(G.meta.id, sizeof G.meta.id, "%s", q->id);
        /* Fill Continue Watching's names from what we now know. */
        for (int i = 0; i < G.progress.count; ++i) {
            nuvio_progress_entry_t *e = &G.progress.entries[i];
            if (!strcmp(e->content_id, q->id) && !e->name[0]) {
                snprintf(e->name, sizeof e->name, "%s", G.meta.name);
                snprintf(e->poster, sizeof e->poster, "%s", G.meta.poster);
            }
        }
    } else {
        PROV_LOG("nuvio: meta %s %s failed (HTTP %d)", q->type, q->id, status);
        if (q->season < 0 && !strcmp(q->type, "movie")) {
            /* No metadata is not no film: go to its streams anyway. */
            nv_ctx_t c;
            build_ctx(q->type, q->id, q->id, &c);
            if (fetch_streams(&c, q->cb, q->ud) != 0) q->cb(0, NULL, 0, 0, q->ud);
            free(q);
            return;
        }
    }
    meta_level(q);
    free(q);
}

static int open_meta(const char *type, const char *id, int season,
                     evo_provider_items_cb cb, void *ud)
{
    meta_req_t *q = (meta_req_t *)calloc(1, sizeof *q);
    if (!q) return -2;
    snprintf(q->type, sizeof q->type, "%s", type);
    snprintf(q->id, sizeof q->id, "%s", id);
    q->season = season;
    q->cb = cb;
    q->ud = ud;

    if (G.meta_valid && !strcmp(G.meta.id, id) && !strcmp(G.meta_type, type)) {
        meta_level(q);
        free(q);
        return 0;
    }

    /* First addon in order that serves meta for it (NuvioTV's rule). */
    for (int a = 0; a < G.count; ++a) {
        if (!G.loaded[a] || !nuvio_addon_serves(&G.addons[a], "meta", type, id)) continue;
        char url[2048];
        if (nuvio_resource_url(G.bases[a], "meta", type, id, url, sizeof url) != 0) continue;
        if (evo_net_request_async("GET", url, NULL, NULL, 0, on_meta, q) == 0) return 0;
    }

    /* Nobody has metadata. A film still has streams. */
    if (season < 0) {
        nv_ctx_t c;
        build_ctx(type, id, id, &c);
        free(q);
        return fetch_streams(&c, cb, ud);
    }
    free(q);
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Catalogs and search                                                       */
/* ------------------------------------------------------------------------- */

typedef struct cat_req {
    char key[256];
    char type[24];
    int  skip;
    int  supports_skip;
    evo_provider_items_cb cb;
    void *ud;
} cat_req_t;

static void on_catalog(int ok, int status, const char *body, size_t len, void *ud)
{
    cat_req_t *q = (cat_req_t *)ud;
    if (!ok || !body) {
        PROV_LOG("nuvio: catalog %s failed (HTTP %d)", q->key, status);
        q->cb(0, NULL, 0, 0, q->ud);
        free(q);
        return;
    }
    evo_provider_item_t *it = (evo_provider_item_t *)calloc(EVO_PROVIDER_PAGE_MAX, sizeof *it);
    int raw = 0;
    int n = it ? nuvio_parse_catalog(body, len, q->type, it, EVO_PROVIDER_PAGE_MAX, &raw) : -1;
    if (n < 0) {
        q->cb(0, NULL, 0, 0, q->ud);
    } else {
        for (int i = 0; i < n; ++i) ui_text(it[i].overview, it[i].overview, sizeof it[i].overview, " ");
        int has_more = q->supports_skip && raw > 0;
        snprintf(G.cat_key, sizeof G.cat_key, "%s", q->key);
        G.cat_next_skip = q->skip + raw;
        remember_items(it, n);
        q->cb(1, it, n, has_more, q->ud);
    }
    free(it);
    free(q);
}

static int open_catalog(const char *id, int page, evo_provider_items_cb cb, void *ud)
{
    /* c:<addon>:<type>:<catalog> */
    const char *p = id + 2;
    int a = atoi(p);
    const char *t = strchr(p, ':');
    if (!t) return -1;
    ++t;
    const char *c = strchr(t, ':');
    if (!c) return -1;
    char type[24];
    snprintf(type, sizeof type, "%.*s", (int)(c - t), t);
    const char *cat_id = c + 1;
    if (a < 0 || a >= G.count || !G.loaded[a]) return -1;

    const nuvio_catalog_t *cat = NULL;
    for (int i = 0; i < G.addons[a].catalog_count; ++i)
        if (!strcmp(G.addons[a].catalogs[i].id, cat_id) && !strcmp(G.addons[a].catalogs[i].type, type)) {
            cat = &G.addons[a].catalogs[i];
            break;
        }
    if (!cat) return -1;

    int skip = 0;
    if (page > 0) {
        if (strcmp(G.cat_key, id) != 0) return -1;
        skip = G.cat_next_skip;
    }
    char url[2048];
    if (nuvio_catalog_url(G.bases[a], type, cat_id, NULL, skip, url, sizeof url) != 0) return -1;

    cat_req_t *q = (cat_req_t *)calloc(1, sizeof *q);
    if (!q) return -2;
    snprintf(q->key, sizeof q->key, "%s", id);
    snprintf(q->type, sizeof q->type, "%s", type);
    q->skip = skip;
    q->supports_skip = cat->supports_skip;
    q->cb = cb;
    q->ud = ud;
    if (evo_net_request_async("GET", url, NULL, NULL, 0, on_catalog, q) != 0) { free(q); return -3; }
    return 0;
}

typedef struct se_job {
    int pending;
    evo_provider_item_t *acc;
    int count;
    evo_provider_items_cb cb;
    void *ud;
} se_job_t;

typedef struct se_req {
    se_job_t *job;
    char type[24];
} se_req_t;

static void se_done(se_job_t *j)
{
    remember_items(j->acc, j->count);
    j->cb(1, j->acc, j->count, 0, j->ud);
    free(j->acc);
    free(j);
}

static void on_search(int ok, int status, const char *body, size_t len, void *ud)
{
    se_req_t *r = (se_req_t *)ud;
    se_job_t *j = r->job;
    (void)status;
    if (ok && body && j->count < EVO_PROVIDER_PAGE_MAX) {
        evo_provider_item_t *tmp = (evo_provider_item_t *)calloc(EVO_PROVIDER_PAGE_MAX, sizeof *tmp);
        int raw = 0;
        int n = tmp ? nuvio_parse_catalog(body, len, r->type, tmp, EVO_PROVIDER_PAGE_MAX, &raw) : -1;
        for (int i = 0; i < n && j->count < EVO_PROVIDER_PAGE_MAX; ++i) {
            int dup = 0;
            for (int k = 0; k < j->count; ++k)
                if (!strcmp(j->acc[k].id, tmp[i].id)) { dup = 1; break; }
            if (dup) continue;
            ui_text(tmp[i].overview, tmp[i].overview, sizeof tmp[i].overview, " ");
            j->acc[j->count++] = tmp[i];
        }
        free(tmp);
    }
    free(r);
    if (--j->pending == 0) se_done(j);
}

static int nv_search(const char *query, int page, evo_provider_items_cb cb, void *ud)
{
    if (!query || !*query) return -1;
    if (page > 0) { cb(1, NULL, 0, 0, ud); return 0; }
    se_job_t *j = (se_job_t *)calloc(1, sizeof *j);
    if (!j) return -2;
    j->acc = (evo_provider_item_t *)calloc(EVO_PROVIDER_PAGE_MAX, sizeof *j->acc);
    if (!j->acc) { free(j); return -2; }
    j->cb = cb;
    j->ud = ud;
    j->pending = 1;
    int asked = 0;
    for (int a = 0; a < G.count && asked < NV_MAX_SEARCH_REQ; ++a) {
        if (!G.loaded[a]) continue;
        for (int c = 0; c < G.addons[a].catalog_count && asked < NV_MAX_SEARCH_REQ; ++c) {
            const nuvio_catalog_t *cat = &G.addons[a].catalogs[c];
            if (!cat->supports_search || cat->needs_other_extra) continue;
            char url[2048];
            if (nuvio_catalog_url(G.bases[a], cat->type, cat->id, query, 0, url, sizeof url) != 0)
                continue;
            se_req_t *r = (se_req_t *)calloc(1, sizeof *r);
            if (!r) continue;
            r->job = j;
            snprintf(r->type, sizeof r->type, "%s", cat->type);
            j->pending++;
            if (evo_net_request_async_timeout("GET", url, NULL, NULL, 0, NV_SEARCH_TIMEOUT_S,
                                              on_search, r) != 0) {
                j->pending--;
                free(r);
                continue;
            }
            ++asked;
        }
    }
    PROV_LOG("nuvio: search '%s' across %d catalogs", query, asked);
    if (--j->pending == 0) se_done(j);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* The vtable                                                                */
/* ------------------------------------------------------------------------- */

static int nv_list_catalog(const char *parent_id, int page, evo_provider_items_cb cb, void *ud)
{
    const char *id = parent_id ? parent_id : "";

    if (!id[0]) {
        if (page > 0) { cb(1, NULL, 0, 0, ud); return 0; }
        if (nuvio_account_is_configured() && !G.account_synced &&
            root_with_account(cb, ud) == 0)
            return 0;
        int missing = 0;
        for (int i = 0; i < G.count; ++i) if (!G.loaded[i]) missing = 1;
        if (missing) return fetch_manifests(0, cb, ud);
        /* Cached manifests list at once; a stale set refreshes behind it. */
        if (!G.refreshing && now_ms() - G.manifests_ms > NV_REFRESH_MS)
            fetch_manifests(1, NULL, NULL);
        emit_root(cb, ud);
        return 0;
    }

    if (!strcmp(id, "cw")) { emit_continue(cb, ud); return 0; }
    if (page > 0 && id[0] != 'c') { cb(1, NULL, 0, 0, ud); return 0; }

    if (!strncmp(id, "c:", 2)) return open_catalog(id, page, cb, ud);

    if (!strncmp(id, "m:", 2)) {                        /* m:<type>:<meta> */
        const char *t = id + 2, *c = strchr(t, ':');
        if (!c) return -1;
        char type[24];
        snprintf(type, sizeof type, "%.*s", (int)(c - t), t);
        return open_meta(type, c + 1, -1, cb, ud);
    }

    if (!strncmp(id, "s:", 2)) {                        /* s:<season>:<type>:<meta> */
        const char *p = id + 2;
        int season = atoi(p);
        const char *t = strchr(p, ':');
        if (!t) return -1;
        ++t;
        const char *c = strchr(t, ':');
        if (!c) return -1;
        char type[24];
        snprintf(type, sizeof type, "%.*s", (int)(c - t), t);
        return open_meta(type, c + 1, season, cb, ud);
    }

    if (!strncmp(id, "v:", 2)) {                        /* v:<type>:<meta>|<video> */
        const char *t = id + 2, *c = strchr(t, ':');
        const char *bar = c ? strchr(c + 1, '|') : NULL;
        if (!c || !bar) return -1;
        char type[24], meta_id[160];
        snprintf(type, sizeof type, "%.*s", (int)(c - t), t);
        snprintf(meta_id, sizeof meta_id, "%.*s", (int)(bar - c - 1), c + 1);
        nv_ctx_t ctx;
        build_ctx(type, meta_id, bar + 1, &ctx);
        return fetch_streams(&ctx, cb, ud);
    }
    return -1;
}

/* x:<gen>:<index> -> the stream, or NULL if the table has moved on. */
static const nuvio_stream_t *stream_for(const char *item_id, int *index)
{
    if (!item_id || strncmp(item_id, "x:", 2) != 0) return NULL;
    char *end = NULL;
    long gen = strtol(item_id + 2, &end, 10);
    if (!end || *end != ':' || gen < 0 || (unsigned long)gen != G.gen) return NULL;
    int i = atoi(end + 1);
    if (i < 0 || i >= G.stream_count) return NULL;
    if (index) *index = i;
    return &G.streams[i];
}

static int nv_resolve(const char *item_id, evo_provider_resolve_cb cb, void *ud)
{
    int idx = 0;
    const nuvio_stream_t *s = stream_for(item_id, &idx);
    if (!s) return -1;

    evo_stream_choice_t c;
    evo_provider_stream_choice_clear(&c);
    snprintf(c.url, sizeof c.url, "%s", s->url);
    snprintf(c.label, sizeof c.label, "%s", s->addon_name);
    c.size_bytes = s->size_bytes;
    c.height = s->quality;
    c.needs_resolver = s->needs_resolver;

    G.now = G.sctx;
    G.now_stream = idx;
    G.now_active = 1;
    G.now_saved_ms = 0;
    PROV_LOG("nuvio: resolve %s -> %s (%s)", item_id,
             s->needs_resolver ? "magnet, via resolver chain" : "direct",
             s->addon_name);
    cb(1, &c, 1, ud);
    return 0;
}

static const char *nv_play_title(const char *item_id)
{
    if (!stream_for(item_id, NULL)) return NULL;
    const nv_ctx_t *c = &G.sctx;
    if (c->season > 0 || c->episode > 0) {
        if (c->ep_title[0])
            snprintf(g_title, sizeof g_title, "%s - S%02dE%02d - %s",
                     c->name, c->season, c->episode, c->ep_title);
        else
            snprintf(g_title, sizeof g_title, "%s - S%02dE%02d", c->name, c->season, c->episode);
    } else {
        snprintf(g_title, sizeof g_title, "%s", c->name);
    }
    return g_title;
}

static int64_t nv_resume_sec(const char *item_id)
{
    if (!stream_for(item_id, NULL)) return 0;
    const nuvio_progress_entry_t *e = nuvio_progress_find_video(&G.progress, G.sctx.video_id);
    if (!e || !nuvio_progress_in_progress(e)) return 0;
    return e->position_ms / 1000;
}

static void nv_report_progress(const char *item_id, int64_t pos_sec, int64_t dur_sec,
                               evo_provider_play_state_t state)
{
    (void)item_id;
    if (!G.now_active) return;
    const nv_ctx_t *c = &G.now;

    nuvio_progress_entry_t e;
    memset(&e, 0, sizeof e);
    nuvio_progress_key(c->meta_id, c->season, c->episode, e.progress_key, sizeof e.progress_key);
    snprintf(e.content_id, sizeof e.content_id, "%s", c->meta_id);
    snprintf(e.content_type, sizeof e.content_type, "%s", c->type);
    snprintf(e.video_id, sizeof e.video_id, "%s", c->video_id);
    snprintf(e.name, sizeof e.name, "%s", c->name);
    snprintf(e.poster, sizeof e.poster, "%s", c->poster);
    e.season = c->season;
    e.episode = c->episode;
    e.position_ms = pos_sec * 1000;
    e.duration_ms = dur_sec * 1000;
    e.last_watched_ms = now_ms();

    /* A START or an early UPDATE carries no duration yet; keep the one we
     * already have rather than recording 0% of an unknown length. */
    nuvio_progress_entry_t *old = nuvio_progress_find_key(&G.progress, e.progress_key);
    if (e.duration_ms <= 0 && old) e.duration_ms = old->duration_ms;
    if (state == EVO_PROVIDER_PLAY_START && old && e.position_ms == 0)
        e.position_ms = old->position_ms;
    if (e.duration_ms <= 0 && state != EVO_PROVIDER_PLAY_STOP) return;

    nuvio_progress_upsert(&G.progress, &e, 1);

    int64_t t = now_ms();
    if (state != EVO_PROVIDER_PLAY_UPDATE || t - G.now_saved_ms > NV_SAVE_EVERY_MS) {
        progress_save();
        G.now_saved_ms = t;
    }
    if (state == EVO_PROVIDER_PLAY_STOP) {
        G.now_active = 0;
        push_progress();
    }
}

static const char *nv_get_source(void) { return ""; }

/*
 * The setup prompt adds an addon: a manifest URL (https:// or stremio://).
 * "reset" puts the list back to Cinemeta alone. The host reopens the provider
 * afterwards, and the root fetches the new manifest.
 *
 * Empty is a no-op, NOT a reset: the host calls set_source("") every time the
 * setup page opens (EvoRmlProviderHost::ShowSetupScreen - for IPTV that is
 * how a typo is undone), and for Nuvio that would throw away the user's whole
 * addon list on a press of OPTIONS.
 */
static int nv_set_source(const char *value)
{
    if (!value) return -1;
    if (!*value) return 0;
    if (!strcmp(value, "reset")) {
        G.count = 0;
        add_base(NV_DEFAULT_ADDON);
        load_cached_manifest(0);
        save_addon_list();
        PROV_LOG("nuvio: addon list reset to Cinemeta");
        return 0;
    }
    int r = add_base(value);
    if (r < 0) return -1;
    save_addon_list();
    PROV_LOG("nuvio: addon %s (%d total)", r ? "added" : "already present", G.count);
    return 0;
}

static const char *nv_ui_bundle_url(void)
{
    return G.bundle[0] ? G.bundle : NULL;
}

static int nv_init(void)
{
    if (!G.addons) {
        G.addons = (nuvio_addon_t *)calloc(NV_MAX_ADDONS, sizeof *G.addons);
        if (!G.addons) return -1;
    }
    if (!G.progress.entries && nuvio_progress_init(&G.progress) != 0) return -1;

    /* init() runs again after the self-unjail moves the data root (rebind):
     * everything here is a reload from files, nothing touches the network. */
    nuvio_meta_free(&G.meta);
    G.meta_valid = 0;
    G.account_synced = 0;
    G.manifests_ms = 0;
    memset(G.loaded, 0, sizeof G.loaded);

    nv_mkdirs();
    load_addon_list();

    char p[512];
    nuvio_progress_load(&G.progress, nv_path("progress.json", p, sizeof p));
    nuvio_account_init(nv_path("account.conf", p, sizeof p));

    G.bundle[0] = '\0';
    size_t len = 0;
    char *b = read_file(nv_path("bundle.txt", p, sizeof p), &len);
    if (b) {
        b[strcspn(b, "\r\n")] = '\0';
        if (!strncmp(b, "http://", 7) || !strncmp(b, "https://", 8))
            snprintf(G.bundle, sizeof G.bundle, "%s", b);
        free(b);
    }

    int loaded = 0;
    for (int i = 0; i < G.count; ++i) loaded += G.loaded[i];
    PROV_LOG("nuvio: init %d addons (%d cached), %d progress entries, account: %s",
             G.count, loaded, G.progress.count, nuvio_account_status());
    return 0;
}

static void nv_shutdown(void)
{
    if (G.now_active) progress_save();
    nuvio_meta_free(&G.meta);
    free(G.streams);
    G.streams = NULL;
    G.stream_count = 0;
    free(G.recent);
    G.recent = NULL;
    G.recent_count = 0;
}

/* Cinemeta needs nothing, so there is always something to browse. */
static int nv_is_configured(void) { return 1; }

const evo_provider_t evo_provider_nuvio_native = {
    .id              = NV_ID,
    .name            = "Nuvio",
    .icon            = NULL,
    .caps            = EVO_PROVIDER_CAP_CATALOG | EVO_PROVIDER_CAP_SEARCH |
                       EVO_PROVIDER_CAP_RESOLVE | EVO_PROVIDER_CAP_PROGRESS |
                       EVO_PROVIDER_CAP_CONFIG  | EVO_PROVIDER_CAP_UI,
    .api_version     = EVO_PROVIDER_API_VERSION,
    .init            = nv_init,
    .shutdown        = nv_shutdown,
    .is_configured   = nv_is_configured,
    .list_catalog    = nv_list_catalog,
    .search          = nv_search,
    .resolve         = nv_resolve,
    .report_progress = nv_report_progress,
    .ui_bundle_url   = nv_ui_bundle_url,
    .get_source      = nv_get_source,
    .set_source      = nv_set_source,
    .source_prompt   = "Add a Stremio addon (manifest URL) - or type reset for Cinemeta only",
    .play_title      = nv_play_title,
    .resume_sec      = nv_resume_sec,
};
