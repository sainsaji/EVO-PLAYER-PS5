/*
 * provider_xtream.c — Xtream Codes IPTV provider for EVO Player (#93).
 *
 * Implements the Xtream Codes player API (player_api.php) behind the
 * evo_provider_t vtable. Supports Live TV, VOD (Movies), and Series with
 * full season/episode drilling, posters/covers, and multi-stream format
 * selection (HLS .m3u8 vs MPEG-TS .ts).
 *
 * Credentials are read from and persisted to xtream.conf (with USB fallback
 * /mnt/usb0/.evo_xtream.conf), and are NEVER printed to evo.log or PROV_LOG.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#include "addon_xtream.h"
#include "evo_provider.h"
#include "evo_net.h"
#include "cJSON.h"
#include "evo_data_path.h"
#include "evo_provider_log.h"

#define XTREAM_CONF "xtream.conf"
#define XTREAM_CONF_USB "/mnt/usb0/.evo_xtream.conf"

static xtream_config_t g_cfg;
static char g_source[256];

/* ------------------------------------------------------------------------- */
/* Credential masking for safe logging                                       */
/* ------------------------------------------------------------------------- */

static void mask_url(const char *url, char *out, size_t out_sz)
{
    if (!url || !out || out_sz == 0) return;
    snprintf(out, out_sz, "%s", url);

    /* Mask username=... */
    char *u = strstr(out, "username=");
    if (u) {
        u += 9;
        char *amp = strchr(u, '&');
        char tail[256] = {0};
        if (amp) snprintf(tail, sizeof tail, "%s", amp);
        snprintf(u, out_sz - (size_t)(u - out), "***%s", tail);
    }

    /* Mask password=... */
    char *p = strstr(out, "password=");
    if (p) {
        p += 9;
        char *amp = strchr(p, '&');
        char tail[256] = {0};
        if (amp) snprintf(tail, sizeof tail, "%s", amp);
        snprintf(p, out_sz - (size_t)(p - out), "***%s", tail);
    }

    /* Mask /live/user/pass/ or /movie/user/pass/ or /series/user/pass/ */
    static const char *prefixes[] = { "/live/", "/movie/", "/series/", NULL };
    for (int i = 0; prefixes[i]; ++i) {
        char *loc = strstr(out, prefixes[i]);
        if (loc) {
            char *u_start = loc + strlen(prefixes[i]);
            char *slash1 = strchr(u_start, '/');
            if (slash1) {
                char *slash2 = strchr(slash1 + 1, '/');
                if (slash2) {
                    char tail[256] = {0};
                    snprintf(tail, sizeof tail, "%s", slash2);
                    snprintf(u_start, out_sz - (size_t)(u_start - out), "***/***%s", tail);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Config persistence                                                        */
/* ------------------------------------------------------------------------- */

static void trim(char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
    size_t lead = 0;
    while (s[lead] == ' ' || s[lead] == '\t') lead++;
    if (lead) memmove(s, s + lead, n - lead + 1);
}

int xtream_init(void)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.port = 80;
    snprintf(g_cfg.stream_format, sizeof g_cfg.stream_format, "m3u8");

    const char *path = evo_data_path(XTREAM_CONF);
    FILE *f = fopen(path, "r");
    if (!f) f = fopen(XTREAM_CONF_USB, "r");
    if (!f) return 0;

    char line[512];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line, *v = eq + 1;
        trim(k); trim(v);

        if (strcmp(k, "host") == 0)
            snprintf(g_cfg.host, sizeof g_cfg.host, "%s", v);
        else if (strcmp(k, "port") == 0)
            g_cfg.port = atoi(v);
        else if (strcmp(k, "username") == 0)
            snprintf(g_cfg.username, sizeof g_cfg.username, "%s", v);
        else if (strcmp(k, "password") == 0)
            snprintf(g_cfg.password, sizeof g_cfg.password, "%s", v);
        else if (strcmp(k, "use_https") == 0)
            g_cfg.use_https = (atoi(v) != 0);
        else if (strcmp(k, "stream_format") == 0)
            snprintf(g_cfg.stream_format, sizeof g_cfg.stream_format, "%s", v);
        else if (strcmp(k, "status") == 0)
            snprintf(g_cfg.status, sizeof g_cfg.status, "%s", v);
        else if (strcmp(k, "exp_date") == 0)
            snprintf(g_cfg.exp_date, sizeof g_cfg.exp_date, "%s", v);
    }
    fclose(f);

    PROV_LOG("xtream: loaded config host='%s' port=%d https=%d format='%s'",
             g_cfg.host, g_cfg.port, g_cfg.use_https ? 1 : 0, g_cfg.stream_format);
    return 0;
}

int xtream_save_config(void)
{
    const char *path = evo_data_path(XTREAM_CONF);
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "# EVO Player - Xtream Codes Configuration\n");
    fprintf(f, "host=%s\n", g_cfg.host);
    fprintf(f, "port=%d\n", g_cfg.port > 0 ? g_cfg.port : (g_cfg.use_https ? 443 : 80));
    fprintf(f, "username=%s\n", g_cfg.username);
    fprintf(f, "password=%s\n", g_cfg.password);
    fprintf(f, "use_https=%d\n", g_cfg.use_https ? 1 : 0);
    fprintf(f, "stream_format=%s\n", g_cfg.stream_format[0] ? g_cfg.stream_format : "m3u8");
    if (g_cfg.status[0])   fprintf(f, "status=%s\n", g_cfg.status);
    if (g_cfg.exp_date[0]) fprintf(f, "exp_date=%s\n", g_cfg.exp_date);
    fclose(f);
    return 0;
}

xtream_config_t *xtream_get_config(void)
{
    return &g_cfg;
}

void xtream_set_server(const char *host, int port, const char *username, const char *password, bool use_https)
{
    if (host)     snprintf(g_cfg.host, sizeof g_cfg.host, "%s", host);
    if (port > 0) g_cfg.port = port;
    if (username) snprintf(g_cfg.username, sizeof g_cfg.username, "%s", username);
    if (password) snprintf(g_cfg.password, sizeof g_cfg.password, "%s", password);
    g_cfg.use_https = use_https;
    if (!g_cfg.stream_format[0])
        snprintf(g_cfg.stream_format, sizeof g_cfg.stream_format, "m3u8");
}

void xtream_set_stream_format(const char *fmt)
{
    if (fmt && (strcmp(fmt, "ts") == 0 || strcmp(fmt, "m3u8") == 0)) {
        snprintf(g_cfg.stream_format, sizeof g_cfg.stream_format, "%s", fmt);
        xtream_save_config();
    }
}

/* ------------------------------------------------------------------------- */
/* URL Construction (Internal)                                               */
/* ------------------------------------------------------------------------- */

static void build_api_url(char *buf, size_t sz, const char *action_query)
{
    const char *scheme = g_cfg.use_https ? "https" : "http";
    int default_port = g_cfg.use_https ? 443 : 80;
    if (g_cfg.port > 0 && g_cfg.port != default_port) {
        if (action_query && *action_query) {
            snprintf(buf, sz, "%s://%s:%d/player_api.php?username=%s&password=%s&%s",
                     scheme, g_cfg.host, g_cfg.port, g_cfg.username, g_cfg.password, action_query);
        } else {
            snprintf(buf, sz, "%s://%s:%d/player_api.php?username=%s&password=%s",
                     scheme, g_cfg.host, g_cfg.port, g_cfg.username, g_cfg.password);
        }
    } else {
        if (action_query && *action_query) {
            snprintf(buf, sz, "%s://%s/player_api.php?username=%s&password=%s&%s",
                     scheme, g_cfg.host, g_cfg.username, g_cfg.password, action_query);
        } else {
            snprintf(buf, sz, "%s://%s/player_api.php?username=%s&password=%s",
                     scheme, g_cfg.host, g_cfg.username, g_cfg.password);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Authentication                                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    xtream_auth_cb cb;
    void *ud;
} auth_ctx_t;

static void on_auth_resp(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    auth_ctx_t *ctx = (auth_ctx_t *)ud;
    if (!ctx) return;

    if (!success || status < 200 || status >= 300 || !body) {
        g_cfg.is_connected = false;
        if (ctx->cb) ctx->cb(0, "Connection to Xtream server failed", ctx->ud);
        free(ctx);
        return;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        g_cfg.is_connected = false;
        if (ctx->cb) ctx->cb(0, "Invalid JSON from Xtream server", ctx->ud);
        free(ctx);
        return;
    }

    cJSON *uinfo = cJSON_GetObjectItem(root, "user_info");
    int auth = 0;
    const char *stat = "";
    const char *exp = "";

    if (uinfo) {
        cJSON *a = cJSON_GetObjectItem(uinfo, "auth");
        if (a) {
            if (cJSON_IsNumber(a)) auth = (a->valueint != 0);
            else if (cJSON_IsString(a) && a->valuestring) auth = (strcmp(a->valuestring, "1") == 0);
        }
        cJSON *st = cJSON_GetObjectItem(uinfo, "status");
        if (st && cJSON_IsString(st) && st->valuestring) stat = st->valuestring;
        cJSON *ex = cJSON_GetObjectItem(uinfo, "exp_date");
        if (ex && cJSON_IsString(ex) && ex->valuestring) exp = ex->valuestring;
    }

    if (auth != 0 && (stat[0] == '\0' || strcasecmp(stat, "Active") == 0)) {
        g_cfg.is_connected = true;
        if (stat[0]) snprintf(g_cfg.status, sizeof g_cfg.status, "%s", stat);
        if (exp[0])  snprintf(g_cfg.exp_date, sizeof g_cfg.exp_date, "%s", exp);
        xtream_save_config();
        if (ctx->cb) ctx->cb(1, "Account active", ctx->ud);
    } else if (auth == 0) {
        g_cfg.is_connected = false;
        if (ctx->cb) ctx->cb(0, "Invalid username or password", ctx->ud);
    } else {
        g_cfg.is_connected = false;
        char msg[96];
        snprintf(msg, sizeof msg, "Account status: %s", stat[0] ? stat : "Inactive");
        if (ctx->cb) ctx->cb(0, msg, ctx->ud);
    }

    cJSON_Delete(root);
    free(ctx);
}

int xtream_connect_async(xtream_auth_cb callback, void *userdata)
{
    if (!g_cfg.host[0] || !g_cfg.username[0] || !g_cfg.password[0]) {
        if (callback) callback(0, "Missing Xtream server credentials", userdata);
        return -1;
    }

    auth_ctx_t *ctx = (auth_ctx_t *)calloc(1, sizeof *ctx);
    if (!ctx) return -2;
    ctx->cb = callback;
    ctx->ud = userdata;

    char url[1024];
    build_api_url(url, sizeof url, NULL);

    char masked[1024];
    mask_url(url, masked, sizeof masked);
    PROV_LOG("xtream: connecting to %s", masked);

    return evo_net_request_async("GET", url, NULL, NULL, 0, on_auth_resp, ctx);
}

/* ------------------------------------------------------------------------- */
/* Catalog & Items                                                           */
/* ------------------------------------------------------------------------- */

typedef struct {
    evo_provider_items_cb cb;
    void *ud;
    char parent_id[EVO_PROVIDER_MAX_ITEM_ID];
} catalog_ctx_t;

static void on_categories_resp(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    catalog_ctx_t *ctx = (catalog_ctx_t *)ud;
    if (!ctx) return;

    if (!success || status < 200 || status >= 300 || !body) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        free(ctx);
        return;
    }

    cJSON *arr = cJSON_Parse(body);
    if (!arr || !cJSON_IsArray(arr)) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        if (arr) cJSON_Delete(arr);
        free(ctx);
        return;
    }

    int count = cJSON_GetArraySize(arr);
    if (count <= 0) {
        if (ctx->cb) ctx->cb(1, NULL, 0, 0, ctx->ud);
        cJSON_Delete(arr);
        free(ctx);
        return;
    }

    if (count > EVO_PROVIDER_PAGE_MAX) count = EVO_PROVIDER_PAGE_MAX;
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)count, sizeof *items);
    if (!items) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        cJSON_Delete(arr);
        free(ctx);
        return;
    }

    const char *prefix = (strcmp(ctx->parent_id, "live") == 0) ? "live_cat:" :
                         (strcmp(ctx->parent_id, "vod") == 0)  ? "vod_cat:" : "series_cat:";

    for (int i = 0; i < count; ++i) {
        cJSON *obj = cJSON_GetArrayItem(arr, i);
        if (!obj) continue;
        evo_provider_item_clear(&items[i]);

        cJSON *cid = cJSON_GetObjectItem(obj, "category_id");
        cJSON *cname = cJSON_GetObjectItem(obj, "category_name");

        const char *id_str = cid ? (cJSON_IsString(cid) ? cid->valuestring : "") : "";
        char id_buf[32] = {0};
        if (!id_str[0] && cid && cJSON_IsNumber(cid)) {
            snprintf(id_buf, sizeof id_buf, "%d", cid->valueint);
            id_str = id_buf;
        }

        snprintf(items[i].id, sizeof items[i].id, "%s%s", prefix, id_str);
        snprintf(items[i].parent_id, sizeof items[i].parent_id, "%s", ctx->parent_id);
        snprintf(items[i].title, sizeof items[i].title, "%s",
                 (cname && cname->valuestring) ? cname->valuestring : "Category");
        items[i].kind = EVO_MEDIA_FOLDER;
        items[i].is_folder = 1;
    }

    if (ctx->cb) ctx->cb(1, items, count, 0, ctx->ud);
    free(items);
    cJSON_Delete(arr);
    free(ctx);
}

static void on_live_streams_resp(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    catalog_ctx_t *ctx = (catalog_ctx_t *)ud;
    if (!ctx) return;

    if (!success || status < 200 || status >= 300 || !body) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        free(ctx);
        return;
    }

    cJSON *arr = cJSON_Parse(body);
    if (!arr || !cJSON_IsArray(arr)) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        if (arr) cJSON_Delete(arr);
        free(ctx);
        return;
    }

    int count = cJSON_GetArraySize(arr);
    if (count <= 0) {
        if (ctx->cb) ctx->cb(1, NULL, 0, 0, ctx->ud);
        cJSON_Delete(arr);
        free(ctx);
        return;
    }

    if (count > EVO_PROVIDER_PAGE_MAX) count = EVO_PROVIDER_PAGE_MAX;
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)count, sizeof *items);
    if (!items) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        cJSON_Delete(arr);
        free(ctx);
        return;
    }

    for (int i = 0; i < count; ++i) {
        cJSON *obj = cJSON_GetArrayItem(arr, i);
        if (!obj) continue;
        evo_provider_item_clear(&items[i]);

        cJSON *sid = cJSON_GetObjectItem(obj, "stream_id");
        cJSON *name = cJSON_GetObjectItem(obj, "name");
        cJSON *icon = cJSON_GetObjectItem(obj, "stream_icon");

        char id_buf[32] = {0};
        const char *id_str = sid ? (cJSON_IsString(sid) ? sid->valuestring : "") : "";
        if (!id_str[0] && sid && cJSON_IsNumber(sid)) {
            snprintf(id_buf, sizeof id_buf, "%d", sid->valueint);
            id_str = id_buf;
        }

        snprintf(items[i].id, sizeof items[i].id, "live_stream:%s", id_str);
        snprintf(items[i].parent_id, sizeof items[i].parent_id, "%s", ctx->parent_id);
        snprintf(items[i].title, sizeof items[i].title, "%s",
                 (name && name->valuestring) ? name->valuestring : "Channel");
        if (icon && icon->valuestring && icon->valuestring[0]) {
            snprintf(items[i].art_url, sizeof items[i].art_url, "%s", icon->valuestring);
        }
        cJSON *now_obj = cJSON_GetObjectItem(obj, "now_title");
        if (!now_obj) now_obj = cJSON_GetObjectItem(obj, "now");
        if (!now_obj) now_obj = cJSON_GetObjectItem(obj, "current_show");
        if (!now_obj) now_obj = cJSON_GetObjectItem(obj, "prog");

        cJSON *next_obj = cJSON_GetObjectItem(obj, "next_title");
        if (!next_obj) next_obj = cJSON_GetObjectItem(obj, "next");
        if (!next_obj) next_obj = cJSON_GetObjectItem(obj, "next_show");

        cJSON *epg_id = cJSON_GetObjectItem(obj, "epg_channel_id");

        if (now_obj && now_obj->valuestring && now_obj->valuestring[0]) {
            snprintf(items[i].now_title, sizeof items[i].now_title, "%s", now_obj->valuestring);
        }
        if (next_obj && next_obj->valuestring && next_obj->valuestring[0]) {
            snprintf(items[i].next_title, sizeof items[i].next_title, "%s", next_obj->valuestring);
        }

        if (items[i].now_title[0]) {
            if (items[i].next_title[0])
                snprintf(items[i].subtitle, sizeof items[i].subtitle, "%s | Next: %s",
                         items[i].now_title, items[i].next_title);
            else
                snprintf(items[i].subtitle, sizeof items[i].subtitle, "%s", items[i].now_title);
        } else if (epg_id && epg_id->valuestring && epg_id->valuestring[0]) {
            snprintf(items[i].subtitle, sizeof items[i].subtitle, "EPG: %s", epg_id->valuestring);
        } else {
            snprintf(items[i].subtitle, sizeof items[i].subtitle, "Live Broadcast");
        }

        items[i].kind = EVO_MEDIA_STREAM;
        items[i].is_live = 1;
        items[i].is_folder = 0;
    }

    if (ctx->cb) ctx->cb(1, items, count, 0, ctx->ud);
    free(items);
    cJSON_Delete(arr);
    free(ctx);
}

static void on_vod_streams_resp(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    catalog_ctx_t *ctx = (catalog_ctx_t *)ud;
    if (!ctx) return;

    if (!success || status < 200 || status >= 300 || !body) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        free(ctx);
        return;
    }

    cJSON *arr = cJSON_Parse(body);
    if (!arr || !cJSON_IsArray(arr)) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        if (arr) cJSON_Delete(arr);
        free(ctx);
        return;
    }

    int count = cJSON_GetArraySize(arr);
    if (count <= 0) {
        if (ctx->cb) ctx->cb(1, NULL, 0, 0, ctx->ud);
        cJSON_Delete(arr);
        free(ctx);
        return;
    }

    if (count > EVO_PROVIDER_PAGE_MAX) count = EVO_PROVIDER_PAGE_MAX;
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)count, sizeof *items);
    if (!items) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        cJSON_Delete(arr);
        free(ctx);
        return;
    }

    for (int i = 0; i < count; ++i) {
        cJSON *obj = cJSON_GetArrayItem(arr, i);
        if (!obj) continue;
        evo_provider_item_clear(&items[i]);

        cJSON *sid = cJSON_GetObjectItem(obj, "stream_id");
        cJSON *name = cJSON_GetObjectItem(obj, "name");
        cJSON *icon = cJSON_GetObjectItem(obj, "stream_icon");
        cJSON *ext = cJSON_GetObjectItem(obj, "container_extension");
        cJSON *rating = cJSON_GetObjectItem(obj, "rating");

        char id_buf[32] = {0};
        const char *id_str = sid ? (cJSON_IsString(sid) ? sid->valuestring : "") : "";
        if (!id_str[0] && sid && cJSON_IsNumber(sid)) {
            snprintf(id_buf, sizeof id_buf, "%d", sid->valueint);
            id_str = id_buf;
        }

        const char *ext_str = (ext && ext->valuestring && ext->valuestring[0]) ? ext->valuestring : "mp4";

        snprintf(items[i].id, sizeof items[i].id, "vod_stream:%s:%s", id_str, ext_str);
        snprintf(items[i].parent_id, sizeof items[i].parent_id, "%s", ctx->parent_id);
        snprintf(items[i].title, sizeof items[i].title, "%s",
                 (name && name->valuestring) ? name->valuestring : "Movie");
        if (rating && rating->valuestring && rating->valuestring[0]) {
            snprintf(items[i].subtitle, sizeof items[i].subtitle, "★ %s", rating->valuestring);
        }
        if (icon && icon->valuestring && icon->valuestring[0]) {
            snprintf(items[i].art_url, sizeof items[i].art_url, "%s", icon->valuestring);
        }
        items[i].kind = EVO_MEDIA_VIDEO;
        items[i].is_live = 0;
        items[i].is_folder = 0;
    }

    if (ctx->cb) ctx->cb(1, items, count, 0, ctx->ud);
    free(items);
    cJSON_Delete(arr);
    free(ctx);
}

static void on_series_resp(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    catalog_ctx_t *ctx = (catalog_ctx_t *)ud;
    if (!ctx) return;

    if (!success || status < 200 || status >= 300 || !body) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        free(ctx);
        return;
    }

    cJSON *arr = cJSON_Parse(body);
    if (!arr || !cJSON_IsArray(arr)) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        if (arr) cJSON_Delete(arr);
        free(ctx);
        return;
    }

    int count = cJSON_GetArraySize(arr);
    if (count <= 0) {
        if (ctx->cb) ctx->cb(1, NULL, 0, 0, ctx->ud);
        cJSON_Delete(arr);
        free(ctx);
        return;
    }

    if (count > EVO_PROVIDER_PAGE_MAX) count = EVO_PROVIDER_PAGE_MAX;
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)count, sizeof *items);
    if (!items) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        cJSON_Delete(arr);
        free(ctx);
        return;
    }

    for (int i = 0; i < count; ++i) {
        cJSON *obj = cJSON_GetArrayItem(arr, i);
        if (!obj) continue;
        evo_provider_item_clear(&items[i]);

        cJSON *sid = cJSON_GetObjectItem(obj, "series_id");
        cJSON *name = cJSON_GetObjectItem(obj, "name");
        cJSON *cover = cJSON_GetObjectItem(obj, "cover");
        cJSON *plot = cJSON_GetObjectItem(obj, "plot");
        cJSON *genre = cJSON_GetObjectItem(obj, "genre");

        char id_buf[32] = {0};
        const char *id_str = sid ? (cJSON_IsString(sid) ? sid->valuestring : "") : "";
        if (!id_str[0] && sid && cJSON_IsNumber(sid)) {
            snprintf(id_buf, sizeof id_buf, "%d", sid->valueint);
            id_str = id_buf;
        }

        snprintf(items[i].id, sizeof items[i].id, "series_show:%s", id_str);
        snprintf(items[i].parent_id, sizeof items[i].parent_id, "%s", ctx->parent_id);
        snprintf(items[i].title, sizeof items[i].title, "%s",
                 (name && name->valuestring) ? name->valuestring : "Series");
        if (genre && genre->valuestring && genre->valuestring[0]) {
            snprintf(items[i].subtitle, sizeof items[i].subtitle, "%s", genre->valuestring);
        }
        if (plot && plot->valuestring && plot->valuestring[0]) {
            snprintf(items[i].overview, sizeof items[i].overview, "%s", plot->valuestring);
        }
        if (cover && cover->valuestring && cover->valuestring[0]) {
            snprintf(items[i].art_url, sizeof items[i].art_url, "%s", cover->valuestring);
        }
        items[i].kind = EVO_MEDIA_FOLDER;
        items[i].is_folder = 1;
    }

    if (ctx->cb) ctx->cb(1, items, count, 0, ctx->ud);
    free(items);
    cJSON_Delete(arr);
    free(ctx);
}

static void on_series_info_resp(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    catalog_ctx_t *ctx = (catalog_ctx_t *)ud;
    if (!ctx) return;

    if (!success || status < 200 || status >= 300 || !body) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        free(ctx);
        return;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        free(ctx);
        return;
    }

    const char *series_id = "";
    if (strncmp(ctx->parent_id, "series_show:", 12) == 0)
        series_id = ctx->parent_id + 12;

    cJSON *seasons = cJSON_GetObjectItem(root, "seasons");
    cJSON *episodes = cJSON_GetObjectItem(root, "episodes");

    /* If we are at the show level and there are seasons, emit Season folders */
    if (strncmp(ctx->parent_id, "series_show:", 12) == 0 && seasons && cJSON_IsArray(seasons) && cJSON_GetArraySize(seasons) > 1) {
        int count = cJSON_GetArraySize(seasons);
        if (count > EVO_PROVIDER_PAGE_MAX) count = EVO_PROVIDER_PAGE_MAX;
        evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)count, sizeof *items);
        if (!items) {
            if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
            cJSON_Delete(root);
            free(ctx);
            return;
        }

        for (int i = 0; i < count; ++i) {
            cJSON *s = cJSON_GetArrayItem(seasons, i);
            if (!s) continue;
            evo_provider_item_clear(&items[i]);

            cJSON *snum = cJSON_GetObjectItem(s, "season_number");
            cJSON *sname = cJSON_GetObjectItem(s, "name");
            cJSON *ep_count = cJSON_GetObjectItem(s, "episode_count");

            int num = snum ? (cJSON_IsNumber(snum) ? snum->valueint : atoi(snum->valuestring)) : (i + 1);
            snprintf(items[i].id, sizeof items[i].id, "series_season:%s:%d", series_id, num);
            snprintf(items[i].parent_id, sizeof items[i].parent_id, "%s", ctx->parent_id);
            snprintf(items[i].title, sizeof items[i].title, "%s",
                     (sname && sname->valuestring) ? sname->valuestring : "Season");
            if (ep_count) {
                int epc = cJSON_IsNumber(ep_count) ? ep_count->valueint : atoi(ep_count->valuestring);
                snprintf(items[i].subtitle, sizeof items[i].subtitle, "%d episode%s", epc, epc == 1 ? "" : "s");
            }
            items[i].kind = EVO_MEDIA_FOLDER;
            items[i].is_folder = 1;
        }

        if (ctx->cb) ctx->cb(1, items, count, 0, ctx->ud);
        free(items);
        cJSON_Delete(root);
        free(ctx);
        return;
    }

    /* Otherwise, emit episodes */
    int target_season = 1;
    if (strncmp(ctx->parent_id, "series_season:", 14) == 0) {
        const char *p = ctx->parent_id + 14;
        const char *colon = strchr(p, ':');
        if (colon) target_season = atoi(colon + 1);
    }

    char snum_key[16];
    snprintf(snum_key, sizeof snum_key, "%d", target_season);
    cJSON *ep_arr = episodes ? cJSON_GetObjectItem(episodes, snum_key) : NULL;
    if (!ep_arr || !cJSON_IsArray(ep_arr)) {
        /* If not found by key, try the first object inside episodes */
        if (episodes && episodes->child && cJSON_IsArray(episodes->child))
            ep_arr = episodes->child;
    }

    if (!ep_arr || !cJSON_IsArray(ep_arr)) {
        if (ctx->cb) ctx->cb(1, NULL, 0, 0, ctx->ud);
        cJSON_Delete(root);
        free(ctx);
        return;
    }

    int count = cJSON_GetArraySize(ep_arr);
    if (count > EVO_PROVIDER_PAGE_MAX) count = EVO_PROVIDER_PAGE_MAX;
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)count, sizeof *items);
    if (!items) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        cJSON_Delete(root);
        free(ctx);
        return;
    }

    for (int i = 0; i < count; ++i) {
        cJSON *ep = cJSON_GetArrayItem(ep_arr, i);
        if (!ep) continue;
        evo_provider_item_clear(&items[i]);

        cJSON *eid = cJSON_GetObjectItem(ep, "id");
        cJSON *title = cJSON_GetObjectItem(ep, "title");
        cJSON *ep_num = cJSON_GetObjectItem(ep, "episode_num");
        cJSON *ext = cJSON_GetObjectItem(ep, "container_extension");
        cJSON *info = cJSON_GetObjectItem(ep, "info");

        char id_buf[32] = {0};
        const char *id_str = eid ? (cJSON_IsString(eid) ? eid->valuestring : "") : "";
        if (!id_str[0] && eid && cJSON_IsNumber(eid)) {
            snprintf(id_buf, sizeof id_buf, "%d", eid->valueint);
            id_str = id_buf;
        }

        const char *ext_str = (ext && ext->valuestring && ext->valuestring[0]) ? ext->valuestring : "mp4";
        int num = ep_num ? (cJSON_IsNumber(ep_num) ? ep_num->valueint : atoi(ep_num->valuestring)) : (i + 1);

        snprintf(items[i].id, sizeof items[i].id, "series_ep:%s:%s", id_str, ext_str);
        snprintf(items[i].parent_id, sizeof items[i].parent_id, "%s", ctx->parent_id);
        snprintf(items[i].title, sizeof items[i].title, "S%02dE%02d - %s",
                 target_season, num, (title && title->valuestring) ? title->valuestring : "Episode");

        if (info) {
            cJSON *plot = cJSON_GetObjectItem(info, "plot");
            if (plot && plot->valuestring)
                snprintf(items[i].overview, sizeof items[i].overview, "%s", plot->valuestring);
            cJSON *dur = cJSON_GetObjectItem(info, "duration_secs");
            if (dur && cJSON_IsNumber(dur))
                items[i].duration_sec = dur->valueint;
        }

        items[i].kind = EVO_MEDIA_VIDEO;
        items[i].is_live = 0;
        items[i].is_folder = 0;
    }

    if (ctx->cb) ctx->cb(1, items, count, 0, ctx->ud);
    free(items);
    cJSON_Delete(root);
    free(ctx);
}

static int xtream_list_catalog(const char *parent_id, int page,
                               evo_provider_items_cb cb, void *ud)
{
    (void)page;
    if (!g_cfg.host[0] || !g_cfg.username[0] || !g_cfg.password[0]) {
        if (cb) cb(0, NULL, 0, 0, ud);
        return -1;
    }

    /* 1. Root level: 3 distinct catalogs */
    if (!parent_id || !*parent_id) {
        evo_provider_item_t root_items[3];
        memset(root_items, 0, sizeof root_items);

        snprintf(root_items[0].id, sizeof root_items[0].id, "live");
        snprintf(root_items[0].title, sizeof root_items[0].title, "Live Channels");
        snprintf(root_items[0].subtitle, sizeof root_items[0].subtitle, "Live broadcast TV streams");
        root_items[0].kind = EVO_MEDIA_FOLDER;
        root_items[0].is_folder = 1;

        snprintf(root_items[1].id, sizeof root_items[1].id, "vod");
        snprintf(root_items[1].title, sizeof root_items[1].title, "Movies (VOD)");
        snprintf(root_items[1].subtitle, sizeof root_items[1].subtitle, "Feature films on demand");
        root_items[1].kind = EVO_MEDIA_FOLDER;
        root_items[1].is_folder = 1;

        snprintf(root_items[2].id, sizeof root_items[2].id, "series");
        snprintf(root_items[2].title, sizeof root_items[2].title, "TV Series");
        snprintf(root_items[2].subtitle, sizeof root_items[2].subtitle, "Television series & episodes");
        root_items[2].kind = EVO_MEDIA_FOLDER;
        root_items[2].is_folder = 1;

        if (cb) cb(1, root_items, 3, 0, ud);
        return 0;
    }

    catalog_ctx_t *ctx = (catalog_ctx_t *)calloc(1, sizeof *ctx);
    if (!ctx) return -2;
    ctx->cb = cb;
    ctx->ud = ud;
    snprintf(ctx->parent_id, sizeof ctx->parent_id, "%s", parent_id);

    char url[1024];

    /* 2. Live categories */
    if (strcmp(parent_id, "live") == 0) {
        build_api_url(url, sizeof url, "action=get_live_categories");
        return evo_net_request_async("GET", url, NULL, NULL, 0, on_categories_resp, ctx);
    }

    /* 3. Live streams in category */
    if (strncmp(parent_id, "live_cat:", 9) == 0) {
        char q[64];
        snprintf(q, sizeof q, "action=get_live_streams&category_id=%s", parent_id + 9);
        build_api_url(url, sizeof url, q);
        return evo_net_request_async("GET", url, NULL, NULL, 0, on_live_streams_resp, ctx);
    }

    /* 4. VOD categories */
    if (strcmp(parent_id, "vod") == 0) {
        build_api_url(url, sizeof url, "action=get_vod_categories");
        return evo_net_request_async("GET", url, NULL, NULL, 0, on_categories_resp, ctx);
    }

    /* 5. VOD streams in category */
    if (strncmp(parent_id, "vod_cat:", 8) == 0) {
        char q[64];
        snprintf(q, sizeof q, "action=get_vod_streams&category_id=%s", parent_id + 8);
        build_api_url(url, sizeof url, q);
        return evo_net_request_async("GET", url, NULL, NULL, 0, on_vod_streams_resp, ctx);
    }

    /* 6. Series categories */
    if (strcmp(parent_id, "series") == 0) {
        build_api_url(url, sizeof url, "action=get_series_categories");
        return evo_net_request_async("GET", url, NULL, NULL, 0, on_categories_resp, ctx);
    }

    /* 7. Series list in category */
    if (strncmp(parent_id, "series_cat:", 11) == 0) {
        char q[64];
        snprintf(q, sizeof q, "action=get_series&category_id=%s", parent_id + 11);
        build_api_url(url, sizeof url, q);
        return evo_net_request_async("GET", url, NULL, NULL, 0, on_series_resp, ctx);
    }

    /* 8. Series info (seasons & episodes) */
    if (strncmp(parent_id, "series_show:", 12) == 0) {
        char q[64];
        snprintf(q, sizeof q, "action=get_series_info&series_id=%s", parent_id + 12);
        build_api_url(url, sizeof url, q);
        return evo_net_request_async("GET", url, NULL, NULL, 0, on_series_info_resp, ctx);
    }

    if (strncmp(parent_id, "series_season:", 14) == 0) {
        const char *p = parent_id + 14;
        const char *colon = strchr(p, ':');
        char series_id[32] = {0};
        if (colon) {
            size_t n = (size_t)(colon - p);
            if (n >= sizeof series_id) n = sizeof series_id - 1;
            memcpy(series_id, p, n);
        } else {
            snprintf(series_id, sizeof series_id, "%s", p);
        }
        char q[64];
        snprintf(q, sizeof q, "action=get_series_info&series_id=%s", series_id);
        build_api_url(url, sizeof url, q);
        return evo_net_request_async("GET", url, NULL, NULL, 0, on_series_info_resp, ctx);
    }

    free(ctx);
    if (cb) cb(0, NULL, 0, 0, ud);
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Stream Resolution & Multi-Format Selection                                */
/* ------------------------------------------------------------------------- */

static int xtream_resolve(const char *item_id, evo_provider_resolve_cb cb, void *ud)
{
    if (!item_id || !g_cfg.host[0] || !g_cfg.username[0] || !g_cfg.password[0])
        return -1;

    const char *scheme = g_cfg.use_https ? "https" : "http";
    int default_port = g_cfg.use_https ? 443 : 80;
    char host_port[160];
    if (g_cfg.port > 0 && g_cfg.port != default_port)
        snprintf(host_port, sizeof host_port, "%s:%d", g_cfg.host, g_cfg.port);
    else
        snprintf(host_port, sizeof host_port, "%s", g_cfg.host);

    /* 1. Live stream resolution -> Offers both HLS (.m3u8) and MPEG-TS (.ts) */
    if (strncmp(item_id, "live_stream:", 12) == 0) {
        const char *stream_id = item_id + 12;
        evo_stream_choice_t choices[2];
        memset(choices, 0, sizeof choices);

        bool prefer_ts = (strcmp(g_cfg.stream_format, "ts") == 0);

        const char *ext0 = prefer_ts ? "ts" : "m3u8";
        const char *lbl0 = prefer_ts ? "MPEG-TS (Raw Stream)" : "HLS (Adaptive Stream)";
        const char *ext1 = prefer_ts ? "m3u8" : "ts";
        const char *lbl1 = prefer_ts ? "HLS (Adaptive Stream)" : "MPEG-TS (Raw Stream)";

        snprintf(choices[0].url, sizeof choices[0].url,
                 "%s://%s/live/%s/%s/%s.%s",
                 scheme, host_port, g_cfg.username, g_cfg.password, stream_id, ext0);
        snprintf(choices[0].label, sizeof choices[0].label, "%s", lbl0);
        snprintf(choices[0].container, sizeof choices[0].container, "%s", ext0);
        choices[0].is_live = 1;

        snprintf(choices[1].url, sizeof choices[1].url,
                 "%s://%s/live/%s/%s/%s.%s",
                 scheme, host_port, g_cfg.username, g_cfg.password, stream_id, ext1);
        snprintf(choices[1].label, sizeof choices[1].label, "%s", lbl1);
        snprintf(choices[1].container, sizeof choices[1].container, "%s", ext1);
        choices[1].is_live = 1;

        char masked[512];
        mask_url(choices[0].url, masked, sizeof masked);
        PROV_LOG("xtream: resolved live stream to %s (format: %s)", masked, ext0);

        if (cb) cb(1, choices, 2, ud);
        return 0;
    }

    /* 2. VOD movie resolution -> /movie/u/p/id.ext */
    if (strncmp(item_id, "vod_stream:", 11) == 0) {
        const char *p = item_id + 11;
        const char *colon = strchr(p, ':');
        char stream_id[32] = {0};
        const char *ext = "mp4";
        if (colon) {
            size_t n = (size_t)(colon - p);
            if (n >= sizeof stream_id) n = sizeof stream_id - 1;
            memcpy(stream_id, p, n);
            ext = colon + 1;
        } else {
            snprintf(stream_id, sizeof stream_id, "%s", p);
        }

        evo_stream_choice_t choice;
        memset(&choice, 0, sizeof choice);
        snprintf(choice.url, sizeof choice.url,
                 "%s://%s/movie/%s/%s/%s.%s",
                 scheme, host_port, g_cfg.username, g_cfg.password, stream_id, ext);
        snprintf(choice.label, sizeof choice.label, "Direct (Movie)");
        snprintf(choice.container, sizeof choice.container, "%s", ext);
        choice.is_live = 0;

        char masked[512];
        mask_url(choice.url, masked, sizeof masked);
        PROV_LOG("xtream: resolved VOD to %s", masked);

        if (cb) cb(1, &choice, 1, ud);
        return 0;
    }

    /* 3. Series episode resolution -> /series/u/p/id.ext */
    if (strncmp(item_id, "series_ep:", 10) == 0) {
        const char *p = item_id + 10;
        const char *colon = strchr(p, ':');
        char episode_id[32] = {0};
        const char *ext = "mp4";
        if (colon) {
            size_t n = (size_t)(colon - p);
            if (n >= sizeof episode_id) n = sizeof episode_id - 1;
            memcpy(episode_id, p, n);
            ext = colon + 1;
        } else {
            snprintf(episode_id, sizeof episode_id, "%s", p);
        }

        evo_stream_choice_t choice;
        memset(&choice, 0, sizeof choice);
        snprintf(choice.url, sizeof choice.url,
                 "%s://%s/series/%s/%s/%s.%s",
                 scheme, host_port, g_cfg.username, g_cfg.password, episode_id, ext);
        snprintf(choice.label, sizeof choice.label, "Direct (Series)");
        snprintf(choice.container, sizeof choice.container, "%s", ext);
        choice.is_live = 0;

        char masked[512];
        mask_url(choice.url, masked, sizeof masked);
        PROV_LOG("xtream: resolved episode to %s", masked);

        if (cb) cb(1, &choice, 1, ud);
        return 0;
    }

    return -1;
}

/* ------------------------------------------------------------------------- */
/* Search                                                                    */
/* ------------------------------------------------------------------------- */

typedef struct {
    evo_provider_items_cb cb;
    void *ud;
    char query[64];
} search_ctx_t;

static void on_search_resp(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    search_ctx_t *ctx = (search_ctx_t *)ud;
    if (!ctx) return;

    if (!success || status < 200 || status >= 300 || !body) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        free(ctx);
        return;
    }

    cJSON *arr = cJSON_Parse(body);
    if (!arr || !cJSON_IsArray(arr)) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        if (arr) cJSON_Delete(arr);
        free(ctx);
        return;
    }

    int total = cJSON_GetArraySize(arr);
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)EVO_PROVIDER_PAGE_MAX, sizeof *items);
    if (!items) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        cJSON_Delete(arr);
        free(ctx);
        return;
    }

    int match_count = 0;
    for (int i = 0; i < total && match_count < EVO_PROVIDER_PAGE_MAX; ++i) {
        cJSON *obj = cJSON_GetArrayItem(arr, i);
        if (!obj) continue;
        cJSON *name = cJSON_GetObjectItem(obj, "name");
        if (!name || !name->valuestring) continue;

        /* Case-insensitive search match */
        if (!strcasestr(name->valuestring, ctx->query)) continue;

        cJSON *sid = cJSON_GetObjectItem(obj, "stream_id");
        cJSON *icon = cJSON_GetObjectItem(obj, "stream_icon");

        char id_buf[32] = {0};
        const char *id_str = sid ? (cJSON_IsString(sid) ? sid->valuestring : "") : "";
        if (!id_str[0] && sid && cJSON_IsNumber(sid)) {
            snprintf(id_buf, sizeof id_buf, "%d", sid->valueint);
            id_str = id_buf;
        }

        evo_provider_item_clear(&items[match_count]);
        snprintf(items[match_count].id, sizeof items[match_count].id, "live_stream:%s", id_str);
        snprintf(items[match_count].title, sizeof items[match_count].title, "%s", name->valuestring);
        if (icon && icon->valuestring && icon->valuestring[0]) {
            snprintf(items[match_count].art_url, sizeof items[match_count].art_url, "%s", icon->valuestring);
        }
        items[match_count].kind = EVO_MEDIA_STREAM;
        items[match_count].is_live = 1;
        match_count++;
    }

    if (ctx->cb) ctx->cb(1, items, match_count, 0, ctx->ud);
    free(items);
    cJSON_Delete(arr);
    free(ctx);
}

static int xtream_search(const char *query, int page, evo_provider_items_cb cb, void *ud)
{
    (void)page;
    if (!query || !*query || !g_cfg.host[0] || !g_cfg.username[0] || !g_cfg.password[0]) {
        if (cb) cb(0, NULL, 0, 0, ud);
        return -1;
    }

    search_ctx_t *ctx = (search_ctx_t *)calloc(1, sizeof *ctx);
    if (!ctx) return -2;
    ctx->cb = cb;
    ctx->ud = ud;
    snprintf(ctx->query, sizeof ctx->query, "%s", query);

    char url[1024];
    build_api_url(url, sizeof url, "action=get_live_streams");
    return evo_net_request_async("GET", url, NULL, NULL, 0, on_search_resp, ctx);
}

/* ------------------------------------------------------------------------- */
/* Lifecycle & vtable                                                        */
/* ------------------------------------------------------------------------- */

static int xtream_provider_init(void)
{
    return xtream_init();
}

static void xtream_provider_shutdown(void)
{
}

static int xtream_provider_is_configured(void)
{
    return (g_cfg.host[0] && g_cfg.username[0] && g_cfg.password[0]) ? 1 : 0;
}

static int xtream_provider_auth(evo_provider_auth_cb cb, void *ud)
{
    return xtream_connect_async((xtream_auth_cb)cb, ud);
}

static const char *xtream_provider_get_source(void)
{
    if (!g_cfg.host[0]) return "";
    const char *scheme = g_cfg.use_https ? "https" : "http";
    int default_port = g_cfg.use_https ? 443 : 80;
    if (g_cfg.port > 0 && g_cfg.port != default_port) {
        snprintf(g_source, sizeof g_source, "%s://%s:%d/get.php?username=%s&password=%s",
                 scheme, g_cfg.host, g_cfg.port, g_cfg.username, g_cfg.password);
    } else {
        snprintf(g_source, sizeof g_source, "%s://%s/get.php?username=%s&password=%s",
                 scheme, g_cfg.host, g_cfg.username, g_cfg.password);
    }
    return g_source;
}

/*
 * An M3U exported from an Xtream panel carries the account in every stream
 * link: http://host:port/live/USER/PASS/123.ts, the short form
 * http://host/USER/PASS/123, or a get.php?username=..&password=.. link.
 *
 * "Every" is the test: a public list can hold the odd Xtream-shaped link from
 * some stray server (iptv.m3u had one in 738), and that is not the user's
 * account. So the account has to be behind at least half of the file's links.
 * Fills `out` with the get.php form set_source understands; -1 = a plain
 * playlist, which belongs to the IPTV provider, not this one.
 */
static int xtream_link_account(const char *line, char *out, size_t out_sz)
{
    int https = strncmp(line, "https://", 8) == 0;
    if (!https && strncmp(line, "http://", 7) != 0) return -1;

    const char *u = strstr(line, "username=");
    const char *pw = strstr(line, "password=");
    if (u && pw) {
        /* Keep only scheme://host/get.php?username=..&password=.. */
        const char *hp = line + (https ? 8 : 7);
        const char *sl = strchr(hp, '/');
        size_t hl = sl ? (size_t)(sl - hp) : strlen(hp);
        u += 9; pw += 9;
        size_t ul = strcspn(u, "&"), pl = strcspn(pw, "&");
        snprintf(out, out_sz, "%s://%.*s/get.php?username=%.*s&password=%.*s",
                 https ? "https" : "http", (int)hl, hp, (int)ul, u, (int)pl, pw);
        return 0;
    }

    const char *hp = line + (https ? 8 : 7);
    const char *path_start = strchr(hp, '/');
    if (!path_start) return -1;
    size_t hl = (size_t)(path_start - hp);
    if (hl == 0 || hl >= 160) return -1;

    char segbuf[512];
    snprintf(segbuf, sizeof segbuf, "%s", path_start + 1);
    char *q = strchr(segbuf, '?');
    if (q) *q = 0;
    char *seg[6];
    int ns = 0;
    for (char *p = segbuf; *p && ns < 6; ) {
        seg[ns++] = p;
        char *sl = strchr(p, '/');
        if (!sl) break;
        *sl = 0;
        p = sl + 1;
    }

    const char *user = NULL, *pass = NULL;
    if (ns >= 4 && (strcmp(seg[0], "live") == 0 || strcmp(seg[0], "movie") == 0 ||
                    strcmp(seg[0], "series") == 0)) {
        user = seg[1]; pass = seg[2];
    } else if (ns == 3 && seg[2][0] >= '0' && seg[2][0] <= '9') {
        user = seg[0]; pass = seg[1];
    }
    if (!user || !pass || !*user || !*pass) return -1;
    snprintf(out, out_sz, "%s://%.*s/get.php?username=%s&password=%s",
             https ? "https" : "http", (int)hl, hp, user, pass);
    return 0;
}

static int xtream_url_from_m3u(const char *path, char *out, size_t out_sz)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    enum { MAXA = 8 };
    static char acct[MAXA][512];
    int votes[MAXA] = {0};
    int na = 0, links = 0, lines = 0;
    char line[1024], cand[512];

    while (lines < 20000 && fgets(line, sizeof line, f)) {
        lines++;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
        if (strncmp(line, "http", 4) != 0) continue;
        links++;
        if (xtream_link_account(line, cand, sizeof cand) != 0) continue;
        int i = 0;
        while (i < na && strcmp(acct[i], cand) != 0) i++;
        if (i == na) {
            if (na == MAXA) continue;
            snprintf(acct[na++], sizeof acct[0], "%s", cand);
        }
        votes[i]++;
    }
    fclose(f);

    int best = -1;
    for (int i = 0; i < na; ++i)
        if (best < 0 || votes[i] > votes[best]) best = i;
    if (best < 0 || votes[best] * 2 < links) {
        PROV_LOG("xtream: playlist has %d links, best account behind %d - not an Xtream export",
                 links, best < 0 ? 0 : votes[best]);
        return -1;
    }
    snprintf(out, out_sz, "%s", acct[best]);
    return 0;
}

static int xtream_provider_set_source(const char *value);

static void xtream_provider_sign_out(void)
{
    xtream_provider_set_source("");      /* "" clears the account and saves */
}

static int xtream_provider_set_source(const char *value)
{
    if (!value || !*value) {
        memset(&g_cfg, 0, sizeof g_cfg);
        xtream_save_config();
        return 0;
    }

    /* A playlist file from the USB picker: Xtream needs the account inside it. */
    char from_file[1024];
    if (value[0] == '/') {
        if (xtream_url_from_m3u(value, from_file, sizeof from_file) != 0) {
            PROV_LOG("xtream: %s has no Xtream account in its links", value);
            return -1;
        }
        PROV_LOG("xtream: account read from %s", value);
        value = from_file;
    }

    char host[128] = {0};
    int port = 80;
    char user[64] = {0};
    char pass[64] = {0};
    bool https = false;

    /* Check format: http(s)://... */
    if (strncmp(value, "http://", 7) == 0 || strncmp(value, "https://", 8) == 0) {
        https = (strncmp(value, "https://", 8) == 0);
        const char *p = value + (https ? 8 : 7);
        port = https ? 443 : 80;

        /* Check for get.php?username=...&password=... */
        const char *u_param = strstr(p, "username=");
        const char *p_param = strstr(p, "password=");
        if (u_param && p_param) {
            u_param += 9;
            const char *u_end = strchr(u_param, '&');
            if (!u_end) u_end = u_param + strlen(u_param);
            size_t u_len = (size_t)(u_end - u_param);
            if (u_len >= sizeof user) u_len = sizeof user - 1;
            memcpy(user, u_param, u_len);

            p_param += 9;
            const char *p_end = strchr(p_param, '&');
            if (!p_end) p_end = p_param + strlen(p_param);
            size_t p_len = (size_t)(p_end - p_param);
            if (p_len >= sizeof pass) p_len = sizeof pass - 1;
            memcpy(pass, p_param, p_len);
        }

        /* Check for user:pass@host */
        const char *at = strchr(p, '@');
        const char *slash = strchr(p, '/');
        if (at && (!slash || at < slash)) {
            const char *colon = strchr(p, ':');
            if (colon && colon < at) {
                size_t ul = (size_t)(colon - p);
                if (ul >= sizeof user) ul = sizeof user - 1;
                memcpy(user, p, ul);

                size_t pl = (size_t)(at - (colon + 1));
                if (pl >= sizeof pass) pl = sizeof pass - 1;
                memcpy(pass, colon + 1, pl);
            }
            p = at + 1;
        }

        /* Extract host[:port] */
        const char *h_end = strchr(p, '/');
        if (!h_end) h_end = strchr(p, '?');
        if (!h_end) h_end = p + strlen(p);

        const char *colon = strchr(p, ':');
        if (colon && colon < h_end) {
            size_t hl = (size_t)(colon - p);
            if (hl >= sizeof host) hl = sizeof host - 1;
            memcpy(host, p, hl);
            port = atoi(colon + 1);
        } else {
            size_t hl = (size_t)(h_end - p);
            if (hl >= sizeof host) hl = sizeof host - 1;
            memcpy(host, p, hl);
        }
    } else {
        /* Format: host:port:user:pass */
        const char *c1 = strchr(value, ':');
        if (c1) {
            size_t hl = (size_t)(c1 - value);
            if (hl >= sizeof host) hl = sizeof host - 1;
            memcpy(host, value, hl);

            const char *c2 = strchr(c1 + 1, ':');
            if (c2) {
                port = atoi(c1 + 1);
                const char *c3 = strchr(c2 + 1, ':');
                if (c3) {
                    size_t ul = (size_t)(c3 - (c2 + 1));
                    if (ul >= sizeof user) ul = sizeof user - 1;
                    memcpy(user, c2 + 1, ul);
                    snprintf(pass, sizeof pass, "%s", c3 + 1);
                }
            }
        }
    }

    if (!host[0]) return -1;
    xtream_set_server(host, port, user[0] ? user : NULL, pass[0] ? pass : NULL, https);
    return xtream_save_config();
}

static const char *xtream_provider_ui_bundle_url(void)
{
    return NULL;
}

const evo_provider_t evo_provider_xtream = {
    .id              = "xtream",
    .name            = "Xtream Codes",
    .icon            = "icon_tv.png",
    .caps            = EVO_PROVIDER_CAP_CATALOG | EVO_PROVIDER_CAP_SEARCH |
                       EVO_PROVIDER_CAP_RESOLVE |
                       EVO_PROVIDER_CAP_AUTH    | EVO_PROVIDER_CAP_UI |
                       EVO_PROVIDER_CAP_CONFIG  | EVO_PROVIDER_CAP_LIVE,
    .api_version     = EVO_PROVIDER_API_VERSION,
    .init            = xtream_provider_init,
    .shutdown        = xtream_provider_shutdown,
    .is_configured   = xtream_provider_is_configured,
    .auth            = xtream_provider_auth,
    .list_catalog    = xtream_list_catalog,
    .search          = xtream_search,
    .resolve         = xtream_resolve,
    .report_progress = NULL,
    .ui_bundle_url   = xtream_provider_ui_bundle_url,
    .get_source      = xtream_provider_get_source,
    .set_source      = xtream_provider_set_source,
    .web_ui_url      = NULL,
    .web_ui_path     = NULL,
    .web_ui_hook     = NULL,
    .is_signed_in    = xtream_provider_is_configured,
    .sign_out        = xtream_provider_sign_out,
};
