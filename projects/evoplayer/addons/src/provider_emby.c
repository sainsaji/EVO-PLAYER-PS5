/*
 * provider_emby.c — Emby behind the evo_provider_t vtable (#90).
 *
 * addon_emby.c keeps its own API and its own behaviour; this file is the
 * adapter that lets the registry, the resolver chain and a provider-authored
 * UI bundle reach it without knowing its name. That is the whole point of
 * re-homing it rather than rewriting it: Emby's REST client is 550 lines that
 * work, and the seam has to be proven against a second provider that is not
 * the one it was designed around.
 *
 * Jellyfin then becomes this file with a different auth header and a different
 * id, which is why the mapping lives here and not inside addon_emby.c.
 *
 * WHAT IS NOT HERE
 *
 * No screens. The Emby setup and browse UI are separate issues under `addons`,
 * and when they arrive they arrive as an Emby-authored bundle, not as C. This
 * file still compiles and registers with EVO_ENABLE_EMBY off - the logic seam
 * is provable with the UI still dark, which is exactly what #90's "done when"
 * asks for.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "evo_provider.h"
#include "addon_emby.h"
#include "evo_net.h"
#include "cJSON.h"

/* ------------------------------------------------------------------------- */
/* evo_media_entry_t -> evo_provider_item_t                                  */
/* ------------------------------------------------------------------------- */
/*
 * The two structs overlap but do not match, and the differences are the ones
 * #90 cares about: an item carries art, a subtitle and now/next, and it does
 * NOT carry a stream_url. A catalog row with a URL in it is how a token ended
 * up in Recent and how the OSD ended up showing a URL instead of a title
 * (issue #9) - resolve() is the only thing that produces a URL now.
 */
static void map_entry(const evo_media_entry_t *e, evo_provider_item_t *it)
{
    evo_provider_item_clear(it);
    snprintf(it->id,       sizeof it->id,       "%s", e->id);
    snprintf(it->title,    sizeof it->title,    "%s", e->title);
    snprintf(it->subtitle, sizeof it->subtitle, "%s", e->detail);
    snprintf(it->overview, sizeof it->overview, "%s", e->overview);
    it->duration_sec   = e->duration_sec;
    it->resume_pos_sec = e->resume_pos_sec;
    it->kind           = e->kind;
    it->is_folder      = e->is_folder;

    /*
     * Emby serves a poster for any item from a predictable path, so art does
     * not need another round trip to discover - which matters, because a
     * poster wall asks for thirty of these at once.
     */
    if (!e->is_folder && e->id[0]) {
        emby_config_t *c = emby_get_config();
        snprintf(it->art_url, sizeof it->art_url,
                 "%s://%s:%d/emby/Items/%s/Images/Primary?maxHeight=480&api_key=%s",
                 c->use_https ? "https" : "http", c->host, c->port,
                 e->id, c->token);
        snprintf(it->backdrop_url, sizeof it->backdrop_url,
                 "%s://%s:%d/emby/Items/%s/Images/Backdrop?maxWidth=1280&api_key=%s",
                 c->use_https ? "https" : "http", c->host, c->port,
                 e->id, c->token);
    }
}

/* ------------------------------------------------------------------------- */
/* Catalog                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct {
    evo_provider_items_cb cb;
    void *ud;
} items_ctx_t;

static void on_items(int success, const evo_media_entry_t *entries, int count,
                     void *userdata)
{
    items_ctx_t *ctx = (items_ctx_t *)userdata;
    if (!ctx) return;

    if (!success || count <= 0) {
        if (ctx->cb) ctx->cb(success ? 1 : 0, NULL, 0, 0, ctx->ud);
        free(ctx);
        return;
    }

    if (count > EVO_PROVIDER_PAGE_MAX) count = EVO_PROVIDER_PAGE_MAX;
    evo_provider_item_t *items =
        (evo_provider_item_t *)calloc((size_t)count, sizeof *items);
    if (!items) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        free(ctx);
        return;
    }
    for (int i = 0; i < count; ++i) map_entry(&entries[i], &items[i]);

    /*
     * has_more is 0 because addon_emby.c asks for Limit=64 and does not report
     * TotalRecordCount. Saying "no more" when there might be is the safe lie
     * here - it shows 64 rows instead of hanging a paging spinner on a count
     * nobody returned. Paging is an Emby-screen issue, not a seam issue.
     */
    if (ctx->cb) ctx->cb(1, items, count, 0, ctx->ud);
    free(items);
    free(ctx);
}

static int emby_list_catalog(const char *parent_id, int page,
                             evo_provider_items_cb cb, void *ud)
{
    (void)page;
    items_ctx_t *ctx = (items_ctx_t *)calloc(1, sizeof *ctx);
    if (!ctx) return -2;
    ctx->cb = cb;
    ctx->ud = ud;

    int rc = (!parent_id || !*parent_id)
           ? emby_fetch_libraries_async(on_items, ctx)
           : emby_fetch_items_async(parent_id, on_items, ctx);
    if (rc != 0) { free(ctx); return rc; }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Search                                                                    */
/* ------------------------------------------------------------------------- */
/*
 * addon_emby.c has no search call, so this one is built here against the same
 * /Items endpoint its fetch uses. It reuses addon_emby's response parser by
 * going through evo_net directly and doing the small amount of JSON work
 * itself, rather than growing addon_emby.h for a single caller.
 */
typedef struct {
    evo_provider_items_cb cb;
    void *ud;
} search_ctx_t;

static void on_search(int success, int status, const char *body, size_t len,
                      void *ud)
{
    (void)len;
    search_ctx_t *ctx = (search_ctx_t *)ud;
    if (!ctx) return;

    cJSON *root = (success && status == 200 && body) ? cJSON_Parse(body) : NULL;
    cJSON *arr  = root ? cJSON_GetObjectItem(root, "Items") : NULL;

    if (!arr || !cJSON_IsArray(arr)) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        if (root) cJSON_Delete(root);
        free(ctx);
        return;
    }

    int n = cJSON_GetArraySize(arr);
    if (n > EVO_PROVIDER_PAGE_MAX) n = EVO_PROVIDER_PAGE_MAX;
    evo_provider_item_t *items = n > 0
        ? (evo_provider_item_t *)calloc((size_t)n, sizeof *items) : NULL;

    int k = 0;
    for (int i = 0; i < n && items; ++i) {
        cJSON *o = cJSON_GetArrayItem(arr, i);
        if (!o) continue;
        cJSON *id   = cJSON_GetObjectItem(o, "Id");
        cJSON *name = cJSON_GetObjectItem(o, "Name");
        cJSON *type = cJSON_GetObjectItem(o, "Type");
        cJSON *year = cJSON_GetObjectItem(o, "ProductionYear");
        cJSON *ticks= cJSON_GetObjectItem(o, "RunTimeTicks");
        if (!id || !cJSON_IsString(id) || !name || !cJSON_IsString(name))
            continue;

        evo_media_entry_t e;
        memset(&e, 0, sizeof e);
        snprintf(e.id,    sizeof e.id,    "%s", id->valuestring);
        snprintf(e.title, sizeof e.title, "%s", name->valuestring);
        if (year && cJSON_IsNumber(year))
            snprintf(e.detail, sizeof e.detail, "%d", (int)year->valuedouble);
        /* Emby counts in 100 ns ticks. */
        if (ticks && cJSON_IsNumber(ticks))
            e.duration_sec = (int64_t)(ticks->valuedouble / 10000000.0);
        int folder = type && cJSON_IsString(type) &&
                     (strcmp(type->valuestring, "Folder") == 0 ||
                      strcmp(type->valuestring, "Series") == 0 ||
                      strcmp(type->valuestring, "Season") == 0 ||
                      strcmp(type->valuestring, "CollectionFolder") == 0);
        e.is_folder = folder;
        e.kind = folder ? EVO_MEDIA_FOLDER : EVO_MEDIA_VIDEO;

        map_entry(&e, &items[k++]);
    }

    if (ctx->cb) ctx->cb(1, items, k, 0, ctx->ud);
    free(items);
    cJSON_Delete(root);
    free(ctx);
}

static int emby_provider_search(const char *query, int page,
                                evo_provider_items_cb cb, void *ud)
{
    (void)page;
    emby_config_t *c = emby_get_config();
    if (!c->is_connected || !c->user_id[0] || !query || !*query) return -1;

    char esc[256];
    if (evo_provider_url_escape(query, esc, sizeof esc) < 0) return -1;

    char url[EVO_PROVIDER_MAX_URL];
    snprintf(url, sizeof url,
             "%s://%s:%d/emby/Users/%s/Items"
             "?SearchTerm=%s&Recursive=true&IncludeItemTypes=Movie,Series,Episode"
             "&Fields=Overview,RunTimeTicks,ProductionYear&Limit=64",
             c->use_https ? "https" : "http", c->host, c->port,
             c->user_id, esc);

    char auth[192];
    snprintf(auth, sizeof auth, "X-Emby-Token: %s", c->token);
    const char *headers[1] = { auth };

    search_ctx_t *ctx = (search_ctx_t *)calloc(1, sizeof *ctx);
    if (!ctx) return -2;
    ctx->cb = cb;
    ctx->ud = ud;

    int rc = evo_net_request_async("GET", url, NULL, headers, 1, on_search, ctx);
    if (rc != 0) { free(ctx); return rc; }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Resolve                                                                   */
/* ------------------------------------------------------------------------- */

static int emby_provider_resolve(const char *item_id,
                                 evo_provider_resolve_cb cb, void *ud)
{
    if (!item_id || !*item_id) return -1;
    emby_config_t *c = emby_get_config();
    if (!c->is_connected) return -1;

    evo_stream_choice_t choice;
    evo_provider_stream_choice_clear(&choice);

    if (emby_build_stream_url(item_id, choice.url, sizeof choice.url) != 0)
        return -1;

    snprintf(choice.label, sizeof choice.label, "Direct");
    /* Static=true is a byte-for-byte remux-free stream, so the container is
     * whatever the file on the server is - unknown from here, and FFmpeg
     * probes it anyway. */
    if (cb) cb(1, &choice, 1, ud);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle, auth, progress                                                 */
/* ------------------------------------------------------------------------- */

static int emby_provider_init(void)   { return emby_init(); }
static void emby_provider_shutdown(void) { /* addon_emby holds no resources */ }

static int emby_provider_is_configured(void)
{
    emby_config_t *c = emby_get_config();
    /* A host plus either a live token or a username to authenticate with.
     * Host alone is not configured - that is the state the removed LAN-literal
     * default used to fake. */
    return c->host[0] && (c->token[0] || c->username[0]) ? 1 : 0;
}

typedef struct {
    evo_provider_auth_cb cb;
    void *ud;
} auth_ctx_t;

static void on_auth(int success, const char *msg, void *userdata)
{
    auth_ctx_t *ctx = (auth_ctx_t *)userdata;
    if (!ctx) return;
    if (ctx->cb) ctx->cb(success, msg, ctx->ud);
    free(ctx);
}

static int emby_provider_auth(evo_provider_auth_cb cb, void *ud)
{
    auth_ctx_t *ctx = (auth_ctx_t *)calloc(1, sizeof *ctx);
    if (!ctx) return -2;
    ctx->cb = cb;
    ctx->ud = ud;
    int rc = emby_connect_async(on_auth, ctx);
    if (rc != 0) { free(ctx); return rc; }
    return 0;
}

static void emby_provider_progress(const char *item_id, int64_t pos_sec,
                                   int64_t dur_sec,
                                   evo_provider_play_state_t state)
{
    switch (state) {
    case EVO_PROVIDER_PLAY_START:  emby_report_playback_start(item_id); break;
    case EVO_PROVIDER_PLAY_UPDATE: emby_report_playback_progress(item_id, pos_sec, dur_sec); break;
    case EVO_PROVIDER_PLAY_STOP:   emby_report_playback_stop(item_id, pos_sec); break;
    }
}

/*
 * Emby does not serve a UI bundle of its own, so this returns NULL and the
 * embedded fallback skin renders instead. When an Emby-authored bundle exists
 * it will be a URL in emby.conf, the same way the IPTV provider does it -
 * which is the reason this returns a function result rather than a constant.
 */
static const char *emby_provider_ui_bundle_url(void) { return NULL; }

const evo_provider_t evo_provider_emby = {
    .id            = "emby",
    .name          = "Emby",
    .icon          = "icon_emby.png",
    .caps          = EVO_PROVIDER_CAP_CATALOG | EVO_PROVIDER_CAP_SEARCH |
                     EVO_PROVIDER_CAP_RESOLVE | EVO_PROVIDER_CAP_PROGRESS |
                     EVO_PROVIDER_CAP_AUTH    | EVO_PROVIDER_CAP_UI,
    .api_version   = EVO_PROVIDER_API_VERSION,
    .init          = emby_provider_init,
    .shutdown      = emby_provider_shutdown,
    .is_configured = emby_provider_is_configured,
    .auth          = emby_provider_auth,
    .list_catalog  = emby_list_catalog,
    .search        = emby_provider_search,
    .resolve       = emby_provider_resolve,
    .report_progress = emby_provider_progress,
    .ui_bundle_url = emby_provider_ui_bundle_url,
};
