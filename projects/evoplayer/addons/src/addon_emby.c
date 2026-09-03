/*
 * addon_emby.c — Emby & Jellyfin media server client implementation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "addon_emby.h"
#include "evo_net.h"
#include "cJSON.h"
#include "evo_data_path.h"

#ifndef EMBY_CONF_PATH
#define EMBY_CONF_PATH evo_data_path("emby.conf")
#endif

#ifndef EMBY_CONF_USB
#define EMBY_CONF_USB  "/mnt/usb0/.evo_emby.conf"
#endif

static emby_config_t g_emby_config = {
    .host = "192.168.0.11",
    .port = 8096,
    .username = "bin",
    .password = "",
    .token = "",
    .user_id = "",
    .server_name = "Emby Server",
    .server_version = "",
    .is_connected = false
};

int emby_init(void)
{
    FILE *f = fopen(EMBY_CONF_PATH, "r");
    if (!f) f = fopen(EMBY_CONF_USB, "r");

    if (f) {
        char line[256];
        int line_idx = 0;
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;

            if (strncmp(line, "host=", 5) == 0) {
                strncpy(g_emby_config.host, line + 5, sizeof(g_emby_config.host) - 1);
            } else if (strncmp(line, "port=", 5) == 0) {
                g_emby_config.port = atoi(line + 5);
                if (g_emby_config.port <= 0) g_emby_config.port = 8096;
            } else if (strncmp(line, "username=", 9) == 0) {
                strncpy(g_emby_config.username, line + 9, sizeof(g_emby_config.username) - 1);
            } else if (strncmp(line, "password=", 9) == 0) {
                strncpy(g_emby_config.password, line + 9, sizeof(g_emby_config.password) - 1);
            } else if (strncmp(line, "token=", 6) == 0) {
                strncpy(g_emby_config.token, line + 6, sizeof(g_emby_config.token) - 1);
            } else if (strncmp(line, "user_id=", 8) == 0) {
                strncpy(g_emby_config.user_id, line + 8, sizeof(g_emby_config.user_id) - 1);
            } else if (strncmp(line, "server_name=", 12) == 0) {
                strncpy(g_emby_config.server_name, line + 12, sizeof(g_emby_config.server_name) - 1);
            } else {
                /* Positional fallback */
                if (line_idx == 0 && line[0]) {
                    strncpy(g_emby_config.host, line, sizeof(g_emby_config.host) - 1);
                } else if (line_idx == 1 && line[0]) {
                    g_emby_config.port = atoi(line);
                    if (g_emby_config.port <= 0) g_emby_config.port = 8096;
                } else if (line_idx == 2 && line[0]) {
                    strncpy(g_emby_config.username, line, sizeof(g_emby_config.username) - 1);
                } else if (line_idx == 3) {
                    strncpy(g_emby_config.password, line, sizeof(g_emby_config.password) - 1);
                } else if (line_idx == 4 && line[0]) {
                    strncpy(g_emby_config.token, line, sizeof(g_emby_config.token) - 1);
                } else if (line_idx == 5 && line[0]) {
                    strncpy(g_emby_config.user_id, line, sizeof(g_emby_config.user_id) - 1);
                } else if (line_idx == 6 && line[0]) {
                    strncpy(g_emby_config.server_name, line, sizeof(g_emby_config.server_name) - 1);
                }
            }
            line_idx++;
        }

        if (g_emby_config.token[0] && g_emby_config.user_id[0]) {
            g_emby_config.is_connected = true;
        }

        fclose(f);
    }

    return 0;
}

int emby_save_config(void)
{
    FILE *f = fopen(EMBY_CONF_PATH, "w");
    if (!f) f = fopen(EMBY_CONF_USB, "w");
    if (!f) return -1;

    fprintf(f, "host=%s\nport=%d\nusername=%s\npassword=%s\ntoken=%s\nuser_id=%s\nserver_name=%s\n",
            g_emby_config.host,
            g_emby_config.port,
            g_emby_config.username,
            g_emby_config.password,
            g_emby_config.token,
            g_emby_config.user_id,
            g_emby_config.server_name);

    fclose(f);
    return 0;
}

emby_config_t *emby_get_config(void)
{
    return &g_emby_config;
}

void emby_set_server(const char *host, int port, const char *username, const char *password)
{
    if (host) strncpy(g_emby_config.host, host, sizeof(g_emby_config.host) - 1);
    if (port > 0) g_emby_config.port = port;
    if (username) strncpy(g_emby_config.username, username, sizeof(g_emby_config.username) - 1);
    if (password) strncpy(g_emby_config.password, password, sizeof(g_emby_config.password) - 1);
}

void emby_disconnect(void)
{
    g_emby_config.token[0] = '\0';
    g_emby_config.user_id[0] = '\0';
    g_emby_config.is_connected = false;
    emby_save_config();
}

/* Authentication Context */
typedef struct {
    emby_auth_cb callback;
    void        *userdata;
    int          retried;
} auth_ctx_t;

static void on_auth_response(int success, int status_code, const char *body, size_t body_len, void *userdata);

static void on_public_users_response(int success, int status_code, const char *body, size_t body_len, void *userdata)
{
    (void)body_len;
    auth_ctx_t *ctx = (auth_ctx_t *)userdata;

    if (success && status_code == 200 && body) {
        cJSON *root = cJSON_Parse(body);
        if (root && cJSON_IsArray(root) && cJSON_GetArraySize(root) > 0) {
            cJSON *first_user = cJSON_GetArrayItem(root, 0);
            if (first_user) {
                cJSON *name_item = cJSON_GetObjectItem(first_user, "Name");
                if (name_item && cJSON_IsString(name_item)) {
                    strncpy(g_emby_config.username, name_item->valuestring, sizeof(g_emby_config.username) - 1);
                    emby_save_config();
                    cJSON_Delete(root);

                    /* Retry auth with discovered username */
                    char url[256];
                    snprintf(url, sizeof(url), "http://%s:%d/emby/Users/AuthenticateByName",
                             g_emby_config.host, g_emby_config.port);

                    cJSON *auth_req = cJSON_CreateObject();
                    cJSON_AddStringToObject(auth_req, "Username", g_emby_config.username);
                    cJSON_AddStringToObject(auth_req, "Pw", g_emby_config.password);
                    char *post_json = cJSON_PrintUnformatted(auth_req);

                    const char *headers[2];
                    headers[0] = "X-Emby-Authorization: MediaBrowser Client=\"EVOPlayer\", Device=\"PlayStation 5\", DeviceId=\"EVO-PS5-050\", Version=\"0.5.0\"";
                    headers[1] = "Content-Type: application/json";

                    ctx->retried = 1;
                    evo_net_request_async("POST", url, post_json ? post_json : "{}", headers, 2, on_auth_response, ctx);
                    if (post_json) free(post_json);
                    cJSON_Delete(auth_req);
                    return;
                }
            }
        }
        if (root) cJSON_Delete(root);
    }

    if (ctx && ctx->callback) {
        ctx->callback(0, "Connection or authentication failed", ctx->userdata);
    }
    free(ctx);
}

static void on_auth_response(int success, int status_code, const char *body, size_t body_len, void *userdata)
{
    (void)body_len;
    auth_ctx_t *ctx = (auth_ctx_t *)userdata;

    if (!success || status_code != 200 || !body) {
        if (ctx && !ctx->retried) {
            /* Try auto-discovering public users on the server */
            char url[256];
            snprintf(url, sizeof(url), "http://%s:%d/emby/Users/Public",
                     g_emby_config.host, g_emby_config.port);
            evo_net_request_async("GET", url, NULL, NULL, 0, on_public_users_response, ctx);
            return;
        }

        if (ctx && ctx->callback) {
            ctx->callback(0, "Connection or authentication failed", ctx->userdata);
        }
        free(ctx);
        return;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        if (ctx && ctx->callback) {
            ctx->callback(0, "Invalid JSON from server", ctx->userdata);
        }
        free(ctx);
        return;
    }

    cJSON *token_item = cJSON_GetObjectItem(root, "AccessToken");
    cJSON *user_item  = cJSON_GetObjectItem(root, "User");
    cJSON *uid_item   = user_item ? cJSON_GetObjectItem(user_item, "Id") : NULL;
    cJSON *srv_item   = cJSON_GetObjectItem(root, "ServerId");

    if (token_item && cJSON_IsString(token_item) && uid_item && cJSON_IsString(uid_item)) {
        strncpy(g_emby_config.token, token_item->valuestring, sizeof(g_emby_config.token) - 1);
        strncpy(g_emby_config.user_id, uid_item->valuestring, sizeof(g_emby_config.user_id) - 1);
        if (srv_item && cJSON_IsString(srv_item)) {
            snprintf(g_emby_config.server_name, sizeof(g_emby_config.server_name), "Emby (%.16s)", srv_item->valuestring);
        }
        g_emby_config.is_connected = true;
        emby_save_config();

        if (ctx && ctx->callback) {
            ctx->callback(1, "Connected successfully", ctx->userdata);
        }
    } else {
        if (ctx && ctx->callback) {
            ctx->callback(0, "Missing AccessToken or UserId in response", ctx->userdata);
        }
    }

    cJSON_Delete(root);
    free(ctx);
}

int emby_connect_async(emby_auth_cb callback, void *userdata)
{
    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d/emby/Users/AuthenticateByName",
             g_emby_config.host, g_emby_config.port);

    cJSON *auth_req = cJSON_CreateObject();
    cJSON_AddStringToObject(auth_req, "Username", g_emby_config.username);
    cJSON_AddStringToObject(auth_req, "Pw", g_emby_config.password);
    char *post_json = cJSON_PrintUnformatted(auth_req);

    const char *headers[2];
    headers[0] = "X-Emby-Authorization: MediaBrowser Client=\"EVOPlayer\", Device=\"PlayStation 5\", DeviceId=\"EVO-PS5-050\", Version=\"0.5.0\"";
    headers[1] = "Content-Type: application/json";

    auth_ctx_t *ctx = (auth_ctx_t *)malloc(sizeof(auth_ctx_t));
    if (!ctx) {
        if (post_json) free(post_json);
        cJSON_Delete(auth_req);
        return -1;
    }
    ctx->callback = callback;
    ctx->userdata = userdata;
    ctx->retried  = 0;

    int ret = evo_net_request_async("POST", url, post_json ? post_json : "{}", headers, 2, on_auth_response, ctx);
    if (post_json) free(post_json);
    cJSON_Delete(auth_req);
    return ret;
}

/* Items Query Context */
typedef struct {
    emby_items_cb callback;
    void         *userdata;
} items_ctx_t;

static void on_views_response(int success, int status_code, const char *body, size_t body_len, void *userdata)
{
    (void)body_len;
    items_ctx_t *ctx = (items_ctx_t *)userdata;
    evo_media_entry_t items[EMBY_MAX_ITEMS];
    int count = 0;

    if (!success || status_code != 200 || !body) {
        if (ctx && ctx->callback) ctx->callback(0, NULL, 0, ctx->userdata);
        free(ctx);
        return;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        if (ctx && ctx->callback) ctx->callback(0, NULL, 0, ctx->userdata);
        free(ctx);
        return;
    }

    cJSON *items_arr = cJSON_GetObjectItem(root, "Items");
    if (items_arr && cJSON_IsArray(items_arr)) {
        int n = cJSON_GetArraySize(items_arr);
        if (n > EMBY_MAX_ITEMS) n = EMBY_MAX_ITEMS;

        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(items_arr, i);
            if (!item) continue;

            cJSON *id_val   = cJSON_GetObjectItem(item, "Id");
            cJSON *name_val = cJSON_GetObjectItem(item, "Name");
            cJSON *type_val = cJSON_GetObjectItem(item, "CollectionType");

            memset(&items[count], 0, sizeof(items[count]));
            if (id_val && cJSON_IsString(id_val))
                strncpy(items[count].id, id_val->valuestring, sizeof(items[count].id) - 1);
            if (name_val && cJSON_IsString(name_val))
                strncpy(items[count].title, name_val->valuestring, sizeof(items[count].title) - 1);

            const char *type_str = (type_val && cJSON_IsString(type_val)) ? type_val->valuestring : "folder";
            snprintf(items[count].detail, sizeof(items[count].detail), "LIBRARY - %s", type_str);
            items[count].is_folder = 1;
            items[count].kind = EVO_MEDIA_FOLDER;
            count++;
        }
    }

    cJSON_Delete(root);

    if (ctx && ctx->callback) {
        ctx->callback(1, items, count, ctx->userdata);
    }
    free(ctx);
}

int emby_fetch_libraries_async(emby_items_cb callback, void *userdata)
{
    if (!g_emby_config.is_connected || !g_emby_config.user_id[0])
        return -1;

    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d/emby/Users/%s/Views",
             g_emby_config.host, g_emby_config.port, g_emby_config.user_id);

    char auth_hdr[256];
    snprintf(auth_hdr, sizeof(auth_hdr), "X-Emby-Token: %s", g_emby_config.token);

    const char *headers[1];
    headers[0] = auth_hdr;

    items_ctx_t *ctx = (items_ctx_t *)malloc(sizeof(items_ctx_t));
    if (!ctx) return -2;
    ctx->callback = callback;
    ctx->userdata = userdata;

    return evo_net_request_async("GET", url, NULL, headers, 1, on_views_response, ctx);
}

static void on_items_response(int success, int status_code, const char *body, size_t body_len, void *userdata)
{
    (void)body_len;
    items_ctx_t *ctx = (items_ctx_t *)userdata;
    evo_media_entry_t items[EMBY_MAX_ITEMS];
    int count = 0;

    if (!success || status_code != 200 || !body) {
        if (ctx && ctx->callback) ctx->callback(0, NULL, 0, ctx->userdata);
        free(ctx);
        return;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        if (ctx && ctx->callback) ctx->callback(0, NULL, 0, ctx->userdata);
        free(ctx);
        return;
    }

    cJSON *items_arr = cJSON_GetObjectItem(root, "Items");
    if (items_arr && cJSON_IsArray(items_arr)) {
        int n = cJSON_GetArraySize(items_arr);
        if (n > EMBY_MAX_ITEMS) n = EMBY_MAX_ITEMS;

        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(items_arr, i);
            if (!item) continue;

            cJSON *id_val   = cJSON_GetObjectItem(item, "Id");
            cJSON *name_val = cJSON_GetObjectItem(item, "Name");
            cJSON *type_val = cJSON_GetObjectItem(item, "Type");
            cJSON *desc_val = cJSON_GetObjectItem(item, "Overview");
            cJSON *year_val = cJSON_GetObjectItem(item, "ProductionYear");
            cJSON *dur_val  = cJSON_GetObjectItem(item, "RunTimeTicks");
            cJSON *fold_val = cJSON_GetObjectItem(item, "IsFolder");

            memset(&items[count], 0, sizeof(items[count]));
            if (id_val && cJSON_IsString(id_val))
                strncpy(items[count].id, id_val->valuestring, sizeof(items[count].id) - 1);
            if (name_val && cJSON_IsString(name_val))
                strncpy(items[count].title, name_val->valuestring, sizeof(items[count].title) - 1);
            if (desc_val && cJSON_IsString(desc_val))
                strncpy(items[count].overview, desc_val->valuestring, sizeof(items[count].overview) - 1);

            int is_folder = (fold_val && cJSON_IsTrue(fold_val));
            const char *type_str = (type_val && cJSON_IsString(type_val)) ? type_val->valuestring : "Media";

            int year = year_val ? year_val->valueint : 0;
            if (dur_val && cJSON_IsNumber(dur_val)) {
                items[count].duration_sec = (int64_t)(dur_val->valuedouble / 10000000.0);
            }

            if (is_folder) {
                snprintf(items[count].detail, sizeof(items[count].detail), "%s", type_str);
                items[count].is_folder = 1;
                items[count].kind = EVO_MEDIA_FOLDER;
            } else {
                int mins = (int)(items[count].duration_sec / 60);
                if (year > 0 && mins > 0) {
                    snprintf(items[count].detail, sizeof(items[count].detail), "%d  -  %d MIN  -  %s", year, mins, type_str);
                } else if (year > 0) {
                    snprintf(items[count].detail, sizeof(items[count].detail), "%d  -  %s", year, type_str);
                } else {
                    snprintf(items[count].detail, sizeof(items[count].detail), "%s", type_str);
                }
                items[count].is_folder = 0;
                items[count].kind = EVO_MEDIA_VIDEO;
                emby_build_stream_url(items[count].id, items[count].stream_url, sizeof(items[count].stream_url));
            }

            count++;
        }
    }

    cJSON_Delete(root);

    if (ctx && ctx->callback) {
        ctx->callback(1, items, count, ctx->userdata);
    }
    free(ctx);
}

int emby_fetch_items_async(const char *parent_id, emby_items_cb callback, void *userdata)
{
    if (!g_emby_config.is_connected || !g_emby_config.user_id[0])
        return -1;

    char url[512];
    if (parent_id && *parent_id) {
        snprintf(url, sizeof(url),
                 "http://%s:%d/emby/Users/%s/Items?ParentId=%s&Fields=Overview,RunTimeTicks,ProductionYear,MediaSources&Limit=64",
                 g_emby_config.host, g_emby_config.port, g_emby_config.user_id, parent_id);
    } else {
        snprintf(url, sizeof(url),
                 "http://%s:%d/emby/Users/%s/Items?Fields=Overview,RunTimeTicks,ProductionYear,MediaSources&Limit=64",
                 g_emby_config.host, g_emby_config.port, g_emby_config.user_id);
    }

    char auth_hdr[256];
    snprintf(auth_hdr, sizeof(auth_hdr), "X-Emby-Token: %s", g_emby_config.token);

    const char *headers[1];
    headers[0] = auth_hdr;

    items_ctx_t *ctx = (items_ctx_t *)malloc(sizeof(items_ctx_t));
    if (!ctx) return -2;
    ctx->callback = callback;
    ctx->userdata = userdata;

    return evo_net_request_async("GET", url, NULL, headers, 1, on_items_response, ctx);
}

int emby_build_stream_url(const char *item_id, char *out_url, size_t max_len)
{
    if (!item_id || !out_url || max_len == 0) return -1;

    snprintf(out_url, max_len,
             "http://%s:%d/emby/Videos/%s/stream?Static=true&api_key=%s",
             g_emby_config.host, g_emby_config.port, item_id, g_emby_config.token);

    return 0;
}

void emby_report_playback_start(const char *item_id)
{
    if (!g_emby_config.is_connected || !item_id) return;

    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d/emby/Sessions/Playing",
             g_emby_config.host, g_emby_config.port);

    char auth_hdr[256];
    snprintf(auth_hdr, sizeof(auth_hdr), "X-Emby-Token: %s", g_emby_config.token);

    char post_json[256];
    snprintf(post_json, sizeof(post_json), "{\"ItemId\":\"%s\",\"CanSeek\":true}", item_id);

    const char *headers[2] = { auth_hdr, "Content-Type: application/json" };
    evo_net_request_async("POST", url, post_json, headers, 2, NULL, NULL);
}

void emby_report_playback_progress(const char *item_id, int64_t pos_sec, int64_t dur_sec)
{
    if (!g_emby_config.is_connected || !item_id) return;

    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d/emby/Sessions/Playing/Progress",
             g_emby_config.host, g_emby_config.port);

    char auth_hdr[256];
    snprintf(auth_hdr, sizeof(auth_hdr), "X-Emby-Token: %s", g_emby_config.token);

    int64_t pos_ticks = pos_sec * 10000000LL;
    int64_t dur_ticks = dur_sec * 10000000LL;

    char post_json[256];
    snprintf(post_json, sizeof(post_json),
             "{\"ItemId\":\"%s\",\"PositionTicks\":%lld,\"RunTimeTicks\":%lld,\"CanSeek\":true}",
             item_id, (long long)pos_ticks, (long long)dur_ticks);

    const char *headers[2] = { auth_hdr, "Content-Type: application/json" };
    evo_net_request_async("POST", url, post_json, headers, 2, NULL, NULL);
}

void emby_report_playback_stop(const char *item_id, int64_t pos_sec)
{
    if (!g_emby_config.is_connected || !item_id) return;

    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d/emby/Sessions/Playing/Stopped",
             g_emby_config.host, g_emby_config.port);

    char auth_hdr[256];
    snprintf(auth_hdr, sizeof(auth_hdr), "X-Emby-Token: %s", g_emby_config.token);

    int64_t pos_ticks = pos_sec * 10000000LL;

    char post_json[256];
    snprintf(post_json, sizeof(post_json),
             "{\"ItemId\":\"%s\",\"PositionTicks\":%lld}",
             item_id, (long long)pos_ticks);

    const char *headers[2] = { auth_hdr, "Content-Type: application/json" };
    evo_net_request_async("POST", url, post_json, headers, 2, NULL, NULL);
}
