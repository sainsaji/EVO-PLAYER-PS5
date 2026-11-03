#include "evo_boot_trace.h"
#include "evo_boot_log.h"
/*
 * Plex PIN pairing — initial native provider integration.
 * This phase implements asynchronous account linking; catalog and playback
 * are intentionally not advertised until implemented.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "evo_provider.h"
#include "evo_net.h"
#include "cJSON.h"
#include "evo_data_path.h"

#define PLEX_CLIENT_ID "evo-player-ps5"
#define PLEX_HEADERS "X-Plex-Client-Identifier: " PLEX_CLIENT_ID
#define PLEX_PRODUCT "X-Plex-Product: EVO Player"
#define PLEX_ACCEPT "Accept: application/json"
static const char *headers[] = { PLEX_HEADERS, PLEX_PRODUCT, PLEX_ACCEPT,
                                "Content-Type: application/x-www-form-urlencoded" };
static char token[1024];
static int pin_id;
static int pending;
static evo_provider_qc_code_cb start_cb;
static evo_provider_qc_cb poll_cb;
static void *start_ud, *poll_ud;

static void save_token(void) {
    FILE *f = fopen(evo_data_path("plex.conf"), "w");
    if (!f) return;
    fprintf(f, "%s\n", token);
    fclose(f);
}
static int plex_init(void) {
    FILE *f = fopen(evo_data_path("plex.conf"), "r");
    token[0] = 0;
    if (f) {
        if (fgets(token, sizeof token, f))
            token[strcspn(token, "\r\n")] = 0;
        fclose(f);
    }
    return 0;
}
static void plex_shutdown(void) { pending = 0; }
static int plex_configured(void) { return token[0] != 0; }
static int plex_signed_in(void) { return token[0] != 0; }
static int plex_needs_sign_in(void) { return !plex_signed_in(); }
static void plex_sign_out(void) { token[0] = 0; save_token(); pending = 0; }

static void on_pin(int ok, int status, const char *body, size_t len, void *ud) {
    (void)ud;

    EVO_LOGI("plex", "PIN: ok=%d status=%d len=%zu", ok, status, len);
    cJSON *r = NULL;

    if (ok && status >= 200 && status < 300 && body && len > 0) {
        /* Parse only the bytes returned by the HTTP callback. */
        r = cJSON_ParseWithLength(body, len);
    }

    cJSON *id = r ? cJSON_GetObjectItemCaseSensitive(r, "id") : NULL;
    cJSON *code = r ? cJSON_GetObjectItemCaseSensitive(r, "code") : NULL;

    int valid = cJSON_IsNumber(id) &&
                id->valuedouble > 0 &&
                cJSON_IsString(code) &&
                code->valuestring &&
                code->valuestring[0];

    EVO_LOGI("plex", "PIN parse: json=%d id=%d code=%d valid=%d",
           r != NULL,
           cJSON_IsNumber(id),
           cJSON_IsString(code),
           valid);

    if (valid) {
        pin_id = id->valueint;
        pending = 1;
    }

    if (start_cb)
        start_cb(valid,
                 valid ? code->valuestring : "Plex PIN response invalid",
                 start_ud);

    cJSON_Delete(r);
    start_cb = NULL;
}
static int plex_qc_start(evo_provider_qc_code_cb cb, void *ud) {
    /* A stale PIN (cancelled or expired) is simply replaced. */
    if (!cb || start_cb || poll_cb) return -1;
    pending = 0; pin_id = 0;
    start_cb = cb; start_ud = ud;
    int rc = evo_net_request_async("POST",
        "https://plex.tv/api/v2/pins?format=json", "",
        headers, 4, on_pin, NULL);
    if (rc != 0) start_cb = NULL;
    return rc;
}
static void on_poll(int ok, int status, const char *body, size_t len, void *ud) {
    (void)len; (void)ud;
    cJSON *r = (ok && status >= 200 && status < 300 && body) ? cJSON_Parse(body) : NULL;
    cJSON *t = r ? cJSON_GetObjectItemCaseSensitive(r, "authToken") : NULL;
    int state = -1;
    const char *msg = "Plex PIN polling failed";
    if (r) {
        state = 0; msg = "Waiting for Plex authorization";
        if (cJSON_IsString(t) && t->valuestring && t->valuestring[0]) {
            snprintf(token, sizeof token, "%s", t->valuestring);
            save_token();
            evo_provider_set_enabled("plex", 1);
            pending = 0;
            state = 1; msg = "Plex account linked";
        }
    }
    if (poll_cb) poll_cb(state, msg, poll_ud);
    poll_cb = NULL;
    cJSON_Delete(r);
}
static int plex_qc_poll(evo_provider_qc_cb cb, void *ud) {
    if (!pending || pin_id <= 0 || !cb || poll_cb) return -1;
    char url[160];
    snprintf(url, sizeof url, "https://plex.tv/api/v2/pins/%d?format=json", pin_id);
    poll_cb = cb; poll_ud = ud;
    int rc = evo_net_request_async("GET", url, NULL, headers, 3, on_poll, NULL);
    if (rc != 0) poll_cb = NULL;
    return rc;
}
static void plex_qc_cancel(void) { pending = 0; pin_id = 0; }
static int plex_auth(evo_provider_auth_cb cb, void *ud) {
    if (!cb) return -1;
    cb(plex_signed_in(), plex_signed_in() ? "Plex account linked" : "Sign in using Plex link code", ud);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Plex catalog — phase 1: discover available media servers                 */
/* ------------------------------------------------------------------------- */


#define PLEX_SERVER_MAX 32

typedef struct {
    char id[160];
    char uri[512];
    char access_token[1024];
} plex_server_t;

static plex_server_t plex_servers[PLEX_SERVER_MAX];
static int plex_server_count;

typedef struct {
    evo_provider_items_cb cb;
    void *ud;
    int server_index;

    int page;
} plex_catalog_ctx_t;


static const char *plex_json_string(cJSON *obj, const char *key)
{
    cJSON *v = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}


static int plex_server_index(const char *id)
{
    if (!id || strncmp(id, "srv:", 4) != 0)
        return -1;

    for (int i = 0; i < plex_server_count; ++i)
        if (strcmp(plex_servers[i].id, id + 4) == 0)
            return i;

    return -1;
}


/*
 * Minimal parser for Plex /library/sections XML.
 * Handles Directory elements and XML-escaped attribute values.
 * Never logs XML bodies or authentication tokens.
 */
static int plex_xml_attribute(const char *start, const char *end,
                              const char *name, char *out, size_t cap)
{
    size_t name_len = strlen(name);
    const char *p = start;

    if (!out || cap == 0)
        return 0;

    out[0] = '\0';

    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' ||
                           *p == '\r' || *p == '\n'))
            ++p;

        if (p >= end || *p == '/' || *p == '>')
            break;

        const char *key = p;

        while (p < end &&
               ((*p >= 'a' && *p <= 'z') ||
                (*p >= 'A' && *p <= 'Z') ||
                (*p >= '0' && *p <= '9') ||
                *p == '_' || *p == '-' || *p == ':'))
            ++p;

        size_t key_len = (size_t)(p - key);

        if (!key_len) {
            ++p;
            continue;
        }

        while (p < end && (*p == ' ' || *p == '\t'))
            ++p;

        if (p >= end || *p != '=') {
            while (p < end && *p != ' ' && *p != '>')
                ++p;
            continue;
        }

        ++p;

        while (p < end && (*p == ' ' || *p == '\t'))
            ++p;

        if (p >= end || (*p != '"' && *p != '\'')) {
            while (p < end && *p != ' ' && *p != '>')
                ++p;
            continue;
        }

        char quote = *p++;
        const char *value = p;

        while (p < end && *p != quote)
            ++p;

        const char *value_end = p;

        if (p < end)
            ++p;

        if (key_len == name_len &&
            strncmp(key, name, name_len) == 0) {
            size_t n = 0;

            while (value < value_end && n + 1 < cap) {
                if (*value == '&') {
                    const char *semi = memchr(
                        value, ';', (size_t)(value_end - value));

                    if (semi) {
                        size_t entity_len = (size_t)(semi - value + 1);
                        char decoded = '\0';

                        if (entity_len == 5 &&
                            !memcmp(value, "&amp;", 5))
                            decoded = '&';
                        else if (entity_len == 4 &&
                                 !memcmp(value, "&lt;", 4))
                            decoded = '<';
                        else if (entity_len == 4 &&
                                 !memcmp(value, "&gt;", 4))
                            decoded = '>';
                        else if (entity_len == 6 &&
                                 !memcmp(value, "&quot;", 6))
                            decoded = '"';
                        else if (entity_len == 6 &&
                                 !memcmp(value, "&apos;", 6))
                            decoded = '\'';

                        if (decoded) {
                            out[n++] = decoded;
                            value = semi + 1;
                            continue;
                        }
                    }
                }

                out[n++] = *value++;
            }

            out[n] = '\0';
            return 1;
        }
    }

    return 0;
}

static int plex_parse_xml_libraries(const char *body, size_t len,
                                    int server_index,
                                    evo_provider_item_t *items,
                                    int capacity)
{
    if (!body || !len || !items || capacity <= 0)
        return -1;

    const char *p = body;
    const char *end = body + len;
    int count = 0;

    /* Ensure this resembles a Plex XML MediaContainer response. */
    if (!memchr(body, '<', len))
        return -1;

    while (p < end && count < capacity) {
        const char *tag = memchr(p, '<', (size_t)(end - p));

        if (!tag)
            break;

        if ((size_t)(end - tag) < 11 ||
            memcmp(tag, "<Directory", 10) != 0 ||
            !(tag[10] == ' ' || tag[10] == '\t' ||
              tag[10] == '\r' || tag[10] == '\n' ||
              tag[10] == '/' || tag[10] == '>')) {
            p = tag + 1;
            continue;
        }

        const char *tag_end = memchr(tag, '>', (size_t)(end - tag));

        if (!tag_end)
            break;

        char key[80];
        char title[EVO_PROVIDER_MAX_TITLE];
        char type[48];

        plex_xml_attribute(tag + 10, tag_end, "key",
                           key, sizeof key);
        plex_xml_attribute(tag + 10, tag_end, "title",
                           title, sizeof title);
        plex_xml_attribute(tag + 10, tag_end, "type",
                           type, sizeof type);

        if (key[0] && title[0]) {
            evo_provider_item_t *it = &items[count];
            evo_provider_item_clear(it);

            snprintf(it->id, sizeof it->id,
                     "lib:%d:%s", server_index, key);

            snprintf(it->title, sizeof it->title,
                     "%s", title);

            snprintf(it->subtitle, sizeof it->subtitle, "%s",
                     !strcmp(type, "movie") ? "Movies" :
                     !strcmp(type, "show") ? "TV Shows" :
                     !strcmp(type, "artist") ? "Music" :
                     "Plex Library");

            it->kind = EVO_MEDIA_FOLDER;
            it->is_folder = 1;
            ++count;
        }

        p = tag_end + 1;
    }

    return count;
}

static void plex_on_libraries(int ok, int status,
                              const char *body, size_t len, void *ud)
{
    plex_catalog_ctx_t *ctx = (plex_catalog_ctx_t *)ud;
    cJSON *root = NULL;

    if (ok && status == 200 && body && len)
        root = cJSON_ParseWithLength(body, len);

    cJSON *container = root ?
        cJSON_GetObjectItemCaseSensitive(root, "MediaContainer") : NULL;

    cJSON *arr = container ?
        cJSON_GetObjectItemCaseSensitive(container, "Directory") : NULL;

    if (!cJSON_IsArray(arr) &&
        ok && status == 200 && body && len) {
        evo_provider_item_t *xml_items =
            (evo_provider_item_t *)calloc(
                EVO_PROVIDER_PAGE_MAX, sizeof *xml_items);

        if (xml_items) {
            int xml_count = plex_parse_xml_libraries(
                body, len, ctx->server_index,
                xml_items, EVO_PROVIDER_PAGE_MAX);

            if (xml_count >= 0) {
                EVO_LOGI("plex", "libraries: XML parsed %d libraries",
                       xml_count);
                ctx->cb(1, xml_items, xml_count, 0, ctx->ud);
                free(xml_items);
                cJSON_Delete(root);
                free(ctx);
                return;
            }

            free(xml_items);
        }
    }

    if (!cJSON_IsArray(arr)) {
        unsigned char first = (body && len) ? (unsigned char)body[0] : 0;

        EVO_LOGI("plex", "libraries: HTTP=%d ok=%d bytes=%zu JSON=%d first_byte=0x%02X",
               status, ok, len, root != NULL, first);

        EVO_LOGE("plex", "libraries: request failed HTTP=%d JSON=%d",
               status, root != NULL);
        ctx->cb(0, NULL, 0, 0, ctx->ud);
        cJSON_Delete(root);
        free(ctx);
        return;
    }

    int total = cJSON_GetArraySize(arr);
    int n = total < EVO_PROVIDER_PAGE_MAX ?
        total : EVO_PROVIDER_PAGE_MAX;

    evo_provider_item_t *items =
        (evo_provider_item_t *)calloc((size_t)n + 1, sizeof *items);

    if (!items) {
        ctx->cb(0, NULL, 0, 0, ctx->ud);
        cJSON_Delete(root);
        free(ctx);
        return;
    }

    int count = 0;

    for (int i = 0; i < n; ++i) {
        cJSON *dir = cJSON_GetArrayItem(arr, i);

        const char *key = plex_json_string(dir, "key");
        const char *title = plex_json_string(dir, "title");
        const char *type = plex_json_string(dir, "type");

        if (!key[0] || !title[0])
            continue;

        evo_provider_item_t *it = &items[count];
        evo_provider_item_clear(it);

        snprintf(it->id, sizeof it->id,
                 "lib:%d:%s", ctx->server_index, key);

        snprintf(it->title, sizeof it->title, "%s", title);

        snprintf(it->subtitle, sizeof it->subtitle, "%s",
                 !strcmp(type, "movie") ? "Movies" :
                 !strcmp(type, "show") ? "TV Shows" :
                 !strcmp(type, "artist") ? "Music" :
                 "Plex Library");

        it->kind = EVO_MEDIA_FOLDER;
        it->is_folder = 1;
        ++count;
    }

    EVO_LOGI("plex", "libraries: found %d libraries", count);

    ctx->cb(1, items, count, 0, ctx->ud);

    free(items);
    cJSON_Delete(root);
    free(ctx);
}


/* Plex media browsing: library -> shows/movies -> seasons -> episodes. */

static int plex_parse_media_id(const char *id, const char *prefix,
                               int *server_index, char *key, size_t cap)
{
    if (!id || !prefix || !server_index || !key || cap == 0)
        return 0;

    size_t n = strlen(prefix);
    if (strncmp(id, prefix, n))
        return 0;

    const char *p = id + n;
    char *end = NULL;
    long index = strtol(p, &end, 10);

    if (end == p || *end != ':' ||
        index < 0 || index >= plex_server_count)
        return 0;

    ++end;

    size_t len = strlen(end);
    if (!len || len >= cap)
        return 0;

    for (const char *c = end; *c; ++c)
        if (*c < '0' || *c > '9')
            return 0;

    *server_index = (int)index;
    memcpy(key, end, len + 1);
    return 1;
}


static void plex_set_media_artwork(evo_provider_item_t *it,
                                   int server_index,
                                   const char *thumb,
                                   const char *art)
{
    if (!it || server_index < 0 ||
        server_index >= plex_server_count)
        return;

    const char *base = plex_servers[server_index].uri;

    if (!base[0] || strncmp(base, "https://", 8) != 0)
        return;

    if (thumb && thumb[0] == '/') {
        int n = snprintf(it->art_url, sizeof it->art_url,
                         "%s%s", base, thumb);

        if (n < 0 || (size_t)n >= sizeof it->art_url)
            it->art_url[0] = '\0';
    }

    if (art && art[0] == '/') {
        int n = snprintf(it->backdrop_url,
                         sizeof it->backdrop_url,
                         "%s%s", base, art);

        if (n < 0 || (size_t)n >= sizeof it->backdrop_url)
            it->backdrop_url[0] = '\0';
    }
}

static int plex_parse_xml_media(const char *body, size_t len,
                                int server_index,
                                evo_provider_item_t *items,
                                int capacity)
{
    if (!body || !len || !items)
        return -1;

    const char *p = body;
    const char *end = body + len;
    int count = 0;

    if (!memchr(body, '<', len))
        return -1;

    while (p < end && count < capacity) {
        const char *tag = memchr(p, '<', (size_t)(end - p));
        if (!tag)
            break;

        size_t prefix_len = 0;
        int directory = 0;

        if ((size_t)(end - tag) > 10 &&
            !memcmp(tag, "<Directory", 10)) {
            prefix_len = 10;
            directory = 1;
        } else if ((size_t)(end - tag) > 6 &&
                   !memcmp(tag, "<Video", 6)) {
            prefix_len = 6;
        } else {
            p = tag + 1;
            continue;
        }

        char boundary = tag[prefix_len];
        if (boundary != ' ' && boundary != '\t' &&
            boundary != '\r' && boundary != '\n' &&
            boundary != '/' && boundary != '>') {
            p = tag + 1;
            continue;
        }

        const char *tag_end =
            memchr(tag, '>', (size_t)(end - tag));

        if (!tag_end)
            break;

        char key[80] = {0};
        char title[EVO_PROVIDER_MAX_TITLE] = {0};
        char type[48] = {0};
        char episode_index[24] = {0};
        char year[24] = {0};
        char duration[32] = {0};
        char offset[32] = {0};
        char thumb[256] = {0};
        char art[256] = {0};

        plex_xml_attribute(tag + prefix_len, tag_end,
                           "ratingKey", key, sizeof key);
        plex_xml_attribute(tag + prefix_len, tag_end,
                           "title", title, sizeof title);
        plex_xml_attribute(tag + prefix_len, tag_end,
                           "type", type, sizeof type);
        plex_xml_attribute(tag + prefix_len, tag_end,
                           "index", episode_index,
                           sizeof episode_index);
        plex_xml_attribute(tag + prefix_len, tag_end,
                           "year", year, sizeof year);
        plex_xml_attribute(tag + prefix_len, tag_end,
                           "duration", duration, sizeof duration);
        plex_xml_attribute(tag + prefix_len, tag_end,
                           "viewOffset", offset, sizeof offset);
        plex_xml_attribute(tag + prefix_len, tag_end,
                           "thumb", thumb, sizeof thumb);
        plex_xml_attribute(tag + prefix_len, tag_end,
                           "art", art, sizeof art);

        int numeric_key = key[0] != '\0';
        for (const char *c = key; *c; ++c)
            if (*c < '0' || *c > '9')
                numeric_key = 0;

        if (!numeric_key || !title[0]) {
            p = tag_end + 1;
            continue;
        }

        evo_provider_item_t *it = &items[count];
        evo_provider_item_clear(it);

        /* PLEX_EPISODE_TITLE_PREFIX:
         * Show the episode number before the episode title.
         * Only Plex Video elements with type="episode" are affected.
         */
        if (!directory && !strcmp(type, "episode") &&
            episode_index[0]) {
            char *endptr = NULL;
            long episode = strtol(episode_index, &endptr, 10);

            if (endptr != episode_index && *endptr == '\0' &&
                episode >= 0 && episode <= 99999) {
                snprintf(it->title, sizeof it->title,
                         "(E%02ld) %s", episode, title);
            } else {
                snprintf(it->title, sizeof it->title, "%s", title);
            }
        } else {
            snprintf(it->title, sizeof it->title, "%s", title);
        }

        plex_set_media_artwork(it, server_index, thumb, art);

        if (count < 5) {
            EVO_LOGI("plex", "ART DEBUG: item=%d title='%s'",
                   count, it->title);
            EVO_LOGI("plex", "ART DEBUG: thumb='%s' art='%s'",
                   thumb, art);
            EVO_LOGI("plex", "ART DEBUG: poster_url='%s'",
                   it->art_url);
            EVO_LOGI("plex", "ART DEBUG: backdrop_url='%s'",
                   it->backdrop_url);
            EVO_LOGI("plex", "ART DEBUG: poster_len=%zu backdrop_len=%zu",
                   strlen(it->art_url),
                   strlen(it->backdrop_url));
        }
        snprintf(it->subtitle, sizeof it->subtitle, "%s",
                 year[0] ? year : type);

        if (directory) {
            const char *prefix =
                !strcmp(type, "show") ? "show" :
                !strcmp(type, "season") ? "season" : "folder";

            snprintf(it->id, sizeof it->id,
                     "%s:%d:%s", prefix, server_index, key);
            it->kind = EVO_MEDIA_FOLDER;
            it->is_folder = 1;
            it->detail = !strcmp(type, "show") ? 2 : 0;
        } else {
            it->detail = !strcmp(type, "movie") ? 1 : 0;
            snprintf(it->id, sizeof it->id,
                     "video:%d:%s", server_index, key);
            it->kind = EVO_MEDIA_VIDEO;
            it->is_folder = 0;

            it->duration_sec = strtoll(duration, NULL, 10) / 1000;
            it->resume_pos_sec = strtoll(offset, NULL, 10) / 1000;
        }

        ++count;
        p = tag_end + 1;
    }

    return count;
}

#define PLEX_CATALOG_PAGE_SIZE 128

static void plex_on_media_items(int ok, int status,
                                const char *body, size_t len,
                                void *ud)
{
    plex_catalog_ctx_t *ctx = (plex_catalog_ctx_t *)ud;

    evo_provider_item_t *items =
        (evo_provider_item_t *)calloc(
            PLEX_CATALOG_PAGE_SIZE, sizeof *items);

    if (!items) {
        ctx->cb(0, NULL, 0, 0, ctx->ud);
        free(ctx);
        return;
    }

    int count = -1;
    int has_more = 0;
    long long total = -1;

    if (ok && status == 200 && body && len) {
        count = plex_parse_xml_media(
            body, len, ctx->server_index,
            items, PLEX_CATALOG_PAGE_SIZE);

        /* Plex normally reports totalSize on MediaContainer. */
        const char *root = strstr(body, "<MediaContainer");

        if (root) {
            const char *tag_end = strchr(root, '>');

            if (tag_end && tag_end < body + len) {
                char value[40] = {0};

                if (plex_xml_attribute(
                        root, tag_end, "totalSize",
                        value, sizeof value) && value[0]) {
                    char *tail = NULL;
                    long long parsed = strtoll(value, &tail, 10);

                    if (tail != value && *tail == '\0' &&
                        parsed >= 0)
                        total = parsed;
                }
            }
        }

        if (count >= 0) {
            long long loaded =
                (long long)ctx->page * PLEX_CATALOG_PAGE_SIZE
                + count;

            has_more = total >= 0
                ? (count > 0 && loaded < total)
                : (count == PLEX_CATALOG_PAGE_SIZE);
        }
    }

    EVO_LOGI("plex", "media: page=%d HTTP=%d ok=%d bytes=%zu items=%d total=%lld has_more=%d",
           ctx->page, status, ok, len, count, total, has_more);

    if (count >= 0)
        ctx->cb(1, items, count, has_more, ctx->ud);
    else
        ctx->cb(0, NULL, 0, 0, ctx->ud);

    free(items);
    free(ctx);
}

static int plex_request_media(int server_index, const char *path,
                              int page,
                              evo_provider_items_cb cb, void *ud)
{
    if (server_index < 0 || server_index >= plex_server_count ||
        !path || !cb)
        return -1;

    plex_server_t *server = &plex_servers[server_index];

    plex_catalog_ctx_t *ctx =
        (plex_catalog_ctx_t *)calloc(1, sizeof *ctx);

    if (!ctx)
        return -1;

    ctx->cb = cb;
    ctx->ud = ud;
    ctx->server_index = server_index;
    ctx->page = page;

    char url[1200];
    snprintf(url, sizeof url, "%s%s", server->uri, path);

    char auth_header[1200];
    snprintf(auth_header, sizeof auth_header,
             "X-Plex-Token: %s", server->access_token);

    char start_header[80];
    char size_header[80];

    snprintf(start_header, sizeof start_header,
             "X-Plex-Container-Start: %lld",
             (long long)page * PLEX_CATALOG_PAGE_SIZE);

    /* A home shelf asks for only the posters it shows (evo_provider_row_limit). */
    const int hint = evo_provider_row_limit();
    snprintf(size_header, sizeof size_header,
             "X-Plex-Container-Size: %d",
             (page == 0 && hint > 0 && hint < PLEX_CATALOG_PAGE_SIZE) ? hint : PLEX_CATALOG_PAGE_SIZE);

    const char *req_headers[] = {
        PLEX_HEADERS,
        PLEX_PRODUCT,
        PLEX_ACCEPT,
        auth_header,
        start_header,
        size_header
    };

    int rc = evo_net_request_async(
        "GET", url, NULL, req_headers, 6,
        plex_on_media_items, ctx);

    EVO_LOGI("plex", "media: request queued rc=%d", rc);

    if (rc != 0)
        free(ctx);

    return rc;
}

static int plex_request_libraries(int server_index,
                                  evo_provider_items_cb cb, void *ud)
{
    if (server_index < 0 || server_index >= plex_server_count)
        return -1;

    plex_server_t *server = &plex_servers[server_index];

    if (!server->uri[0] || !server->access_token[0])
        return -1;

    plex_catalog_ctx_t *ctx =
        (plex_catalog_ctx_t *)calloc(1, sizeof *ctx);

    if (!ctx)
        return -1;

    ctx->cb = cb;
    ctx->ud = ud;
    ctx->server_index = server_index;

    char url[1024];
    snprintf(url, sizeof url, "%s/library/sections?format=json",
             server->uri);

    char auth_header[1200];
    snprintf(auth_header, sizeof auth_header,
             "X-Plex-Token: %s", server->access_token);

    const char *req_headers[] = {
        PLEX_HEADERS,
        PLEX_PRODUCT,
        PLEX_ACCEPT,
        "X-Plex-Accept: application/json",
        auth_header
    };

    EVO_LOGI("plex", "libraries: requesting server=%d HTTPS=%d",
           server_index,
           strncmp(server->uri, "https://", 8) == 0);

    int rc = evo_net_request_async(
        "GET", url, NULL,
        req_headers, 5,
        plex_on_libraries, ctx);

    EVO_LOGI("plex", "libraries: request queued rc=%d", rc);

    if (rc != 0)
        free(ctx);

    return rc;
}


static int plex_connection_score(const char *uri)
{
    if (!uri || strncmp(uri, "https://", 8) != 0)
        return -1;

    const char *host = uri + 8;

    /* plex.direct encodes the server IP in its hostname. */
    if (strncmp(host, "192-168-", 8) == 0)
        return 100;

    if (strncmp(host, "10-", 3) == 0)
        return 90;

    /* Docker's common bridge subnet: usually unreachable from PS5. */
    if (strncmp(host, "172-17-", 7) == 0)
        return 0;

    if (strncmp(host, "172-", 4) == 0)
        return 70;

    /* Other HTTPS connections remain possible fallbacks. */
    return 30;
}

static void plex_on_resources(int ok, int status,
                              const char *body, size_t len, void *ud)
{
    plex_catalog_ctx_t *ctx = (plex_catalog_ctx_t *)ud;
    cJSON *root = NULL;

    if (ok && status == 200 && body && len)
        root = cJSON_ParseWithLength(body, len);

    EVO_LOGI("plex", "resources: HTTP=%d ok=%d bytes=%zu JSON=%d array=%d object=%d",
           status, ok, len,
           root != NULL,
           root && cJSON_IsArray(root),
           root && cJSON_IsObject(root));

    if (!root || !cJSON_IsArray(root)) {
        EVO_LOGE("plex", "catalog: resource discovery failed, HTTP %d", status);
        ctx->cb(0, NULL, 0, 0, ctx->ud);
        cJSON_Delete(root);
        free(ctx);
        return;
    }

    int count = 0;
    int total = cJSON_GetArraySize(root);

    plex_server_count = 0;
    memset(plex_servers, 0, sizeof plex_servers);

    evo_provider_item_t *items =
        (evo_provider_item_t *)calloc(EVO_PROVIDER_PAGE_MAX, sizeof *items);

    if (!items) {
        ctx->cb(0, NULL, 0, 0, ctx->ud);
        cJSON_Delete(root);
        free(ctx);
        return;
    }

    for (int i = 0; i < total && count < EVO_PROVIDER_PAGE_MAX; ++i) {
        cJSON *resource = cJSON_GetArrayItem(root, i);
        const char *provides = plex_json_string(resource, "provides");
        const char *name = plex_json_string(resource, "name");
        const char *client_id = plex_json_string(resource, "clientIdentifier");

        /* Plex resources can also include players and other devices. */
        if (!strstr(provides, "server") || !client_id[0])
            continue;

        if (plex_server_count >= PLEX_SERVER_MAX)
            break;

        cJSON *connections =
            cJSON_GetObjectItemCaseSensitive(resource, "connections");

        const char *chosen_uri = NULL;
        int best_score = -1;

        if (cJSON_IsArray(connections)) {
            int n = cJSON_GetArraySize(connections);
            EVO_LOGI("plex", "discovery: advertised connections=%d", n);

            for (int j = 0; j < n; ++j) {
                cJSON *conn = cJSON_GetArrayItem(connections, j);
                const char *uri = plex_json_string(conn, "uri");
                cJSON *local = cJSON_GetObjectItemCaseSensitive(conn, "local");
                cJSON *relay = cJSON_GetObjectItemCaseSensitive(conn, "relay");

                int is_local = cJSON_IsTrue(local);
                int is_relay = cJSON_IsTrue(relay);
                int is_https = strncmp(uri, "https://", 8) == 0;
                int is_docker = strstr(uri, "172-17-") != NULL ||
                                strstr(uri, "172-22-") != NULL;

                EVO_LOGI("plex", "connection[%d]: https=%d local=%d relay=%d docker=%d",
                       j, is_https, is_local, is_relay, is_docker);
            }
        }

        if (cJSON_IsArray(connections)) {
            int nconn = cJSON_GetArraySize(connections);

            for (int j = 0; j < nconn; ++j) {
                cJSON *conn = cJSON_GetArrayItem(connections, j);
                const char *uri = plex_json_string(conn, "uri");
                int score = plex_connection_score(uri);

                cJSON *local_flag =
                    cJSON_GetObjectItemCaseSensitive(conn, "local");
                cJSON *relay_flag =
                    cJSON_GetObjectItemCaseSensitive(conn, "relay");

                /* Prefer remote HTTPS connections for remote Plex servers. */
                if (score >= 0 && cJSON_IsFalse(local_flag)) {
                    score = cJSON_IsTrue(relay_flag) ? 150 : 200;
                }

                if (score > best_score) {
                    best_score = score;
                    chosen_uri = uri;
                }
            }
        }

        EVO_LOGI("plex", "discovery: selected HTTPS connection score=%d",
               best_score);

        if (!chosen_uri) {
            EVO_LOGW("plex", "discovery: server has no HTTPS connection");
            continue;
        }

        const char *access_token =
            plex_json_string(resource, "accessToken");

        if (!access_token[0])
            access_token = token;

        plex_server_t *server = &plex_servers[plex_server_count];

        snprintf(server->id, sizeof server->id, "%s", client_id);
        snprintf(server->uri, sizeof server->uri, "%s", chosen_uri);
        snprintf(server->access_token,
                 sizeof server->access_token, "%s", access_token);

        ++plex_server_count;

        evo_provider_item_t *it = &items[count];
        evo_provider_item_clear(it);

        snprintf(it->id, sizeof it->id, "srv:%s", client_id);
        snprintf(it->title, sizeof it->title, "%s",
                 name[0] ? name : "Plex Media Server");
        snprintf(it->subtitle, sizeof it->subtitle, "Plex Media Server");

        it->kind = EVO_MEDIA_FOLDER;
        it->is_folder = 1;
        ++count;
    }

    EVO_LOGI("plex", "catalog: discovered %d media server(s)", count);

    ctx->cb(1, items, count, 0, ctx->ud);

    free(items);
    cJSON_Delete(root);
    free(ctx);
}

static int plex_list_catalog(const char *parent_id, int page,
                             evo_provider_items_cb cb, void *ud)
{
    if (!cb || !plex_signed_in() || page < 0)
        return -1;

    /*
     * Phase 1 only supports the catalog root.
     * Server/library navigation is added in the next phase.
     */
    if (parent_id && parent_id[0]) {
        int index = plex_server_index(parent_id);

        if (index >= 0) {
            if (page > 0) {
                cb(1, NULL, 0, 0, ud);
                return 0;
            }

            return plex_request_libraries(index, cb, ud);
        }

        int server_index = -1;
        char key[80];
        char path[256];

        if (plex_parse_media_id(parent_id, "lib:",
                                &server_index, key, sizeof key)) {
            snprintf(path, sizeof path,
                     "/library/sections/%s/all", key);

            return plex_request_media(server_index, path, page, cb, ud);
        }

        if (plex_parse_media_id(parent_id, "show:",
                                &server_index, key, sizeof key) ||
            plex_parse_media_id(parent_id, "season:",
                                &server_index, key, sizeof key) ||
            plex_parse_media_id(parent_id, "folder:",
                                &server_index, key, sizeof key)) {
            snprintf(path, sizeof path,
                     "/library/metadata/%s/children", key);

            return plex_request_media(server_index, path, page, cb, ud);
        }

        return -1;
    }

    if (page > 0) {
        cb(1, NULL, 0, 0, ud);
        return 0;
    }

    plex_catalog_ctx_t *ctx =
        (plex_catalog_ctx_t *)calloc(1, sizeof *ctx);

    if (!ctx)
        return -1;

    ctx->cb = cb;
    ctx->ud = ud;

    char auth_header[1200];
    snprintf(auth_header, sizeof auth_header,
             "X-Plex-Token: %s", token);

    const char *req_headers[] = {
        PLEX_HEADERS,
        PLEX_PRODUCT,
        PLEX_ACCEPT,
        auth_header
    };

    int rc = evo_net_request_async(
        "GET",
        "https://plex.tv/api/v2/resources?includeHttps=1&format=json",
        NULL,
        req_headers, 4,
        plex_on_resources, ctx);

    if (rc != 0)
        free(ctx);

    return rc;
}


/*
 * Only authorize artwork URLs under a discovered Plex server origin.
 * The returned header is copied into the caller's buffer.
 *
 * Do not log this header.
 */
int evo_plex_art_auth_header(const char *url, char *out, size_t cap)
{
    if (!url || !out || cap == 0)
        return 0;

    out[0] = '\0';

    for (int i = 0; i < plex_server_count; ++i) {
        const plex_server_t *server = &plex_servers[i];
        const char *base = server->uri;

        if (strncmp(base, "https://", 8) != 0)
            continue;

        size_t n = strlen(base);

        if (!n || strncmp(url, base, n) != 0)
            continue;

        /*
         * Prevent prefix confusion:
         * https://server.example must not match
         * https://server.example.attacker.example
         */
        const char *suffix = url + n;

        if (suffix[0] != '/')
            continue;

        if (strncmp(suffix, "/library/metadata/", 18) != 0)
            continue;

        int written = snprintf(out, cap,
                               "X-Plex-Token: %s",
                               server->access_token);

        if (written < 0 || (size_t)written >= cap) {
            out[0] = '\0';
            return 0;
        }

        return 1;
    }

    return 0;
}


/* ------------------------------------------------------------------------- */
/* Plex Direct Play                                                          */
/* ------------------------------------------------------------------------- */

typedef struct {
    evo_provider_resolve_cb cb;
    void *ud;
    int server_index;
    char rating_key[64];
} plex_resolve_ctx_t;

static int plex_resolve_xml(const char *body, size_t len,
                            int server_index,
                            evo_stream_choice_t *choice)
{
    if (!body || !choice ||
        server_index < 0 || server_index >= plex_server_count)
        return 0;

    const char *end = body + len;
    const char *media = body;

    /* Skip <MediaContainer> and find an actual <Media> element. */
    while ((media = strstr(media, "<Media")) != NULL) {
        char next = media[6];

        if (next == ' ' || next == '\t' ||
            next == '\r' || next == '\n' ||
            next == '>' || next == '/') {
            break;
        }

        media += 6;
    }

    if (!media || media >= end)
        return 0;

    const char *media_end = strchr(media, '>');

    if (!media_end || media_end >= end)
        return 0;

    const char *part = strstr(media_end + 1, "<Part");

    if (!part || part >= end)
        return 0;

    const char *part_end = strchr(part, '>');

    if (!part_end || part_end >= end)
        return 0;

    char key[512] = {0};
    char container[32] = {0};
    char video_codec[32] = {0};
    char audio_codec[32] = {0};
    char width[32] = {0};
    char height[32] = {0};
    char bitrate[32] = {0};
    char size[32] = {0};

    plex_xml_attribute(part, part_end, "key", key, sizeof key);
    plex_xml_attribute(media, media_end, "container",
                       container, sizeof container);
    plex_xml_attribute(media, media_end, "videoCodec",
                       video_codec, sizeof video_codec);
    plex_xml_attribute(media, media_end, "audioCodec",
                       audio_codec, sizeof audio_codec);
    plex_xml_attribute(media, media_end, "width",
                       width, sizeof width);
    plex_xml_attribute(media, media_end, "height",
                       height, sizeof height);
    plex_xml_attribute(media, media_end, "bitrate",
                       bitrate, sizeof bitrate);
    plex_xml_attribute(part, part_end, "size",
                       size, sizeof size);

    if (strncmp(key, "/library/parts/", 15) != 0)
        return 0;

    plex_server_t *server = &plex_servers[server_index];

    if (strncmp(server->uri, "https://", 8) != 0 ||
        !server->access_token[0])
        return 0;

    evo_provider_stream_choice_clear(choice);

    int n = snprintf(choice->url, sizeof choice->url,
                     "%s%s", server->uri, key);

    if (n < 0 || (size_t)n >= sizeof choice->url)
        return 0;

    n = snprintf(choice->headers, sizeof choice->headers,
                 "X-Plex-Token: %s\r\n",
                 server->access_token);

    if (n < 0 || (size_t)n >= sizeof choice->headers)
        return 0;

    snprintf(choice->label, sizeof choice->label,
             "Plex Direct Play");

    snprintf(choice->container, sizeof choice->container,
             "%s", container);
    snprintf(choice->video_codec, sizeof choice->video_codec,
             "%s", video_codec);
    snprintf(choice->audio_codec, sizeof choice->audio_codec,
             "%s", audio_codec);

    choice->width = atoi(width);
    choice->height = atoi(height);

    /* Plex metadata bitrate is in kbps. */
    choice->bitrate_bps = (int64_t)atoll(bitrate) * 1000;
    choice->size_bytes = (int64_t)atoll(size);

    EVO_LOGI("plex", "playback: Direct Play %s %dx%d video=%s audio=%s bitrate=%lld kbps",
           choice->container,
           choice->width,
           choice->height,
           choice->video_codec,
           choice->audio_codec,
           (long long)(choice->bitrate_bps / 1000));

    return 1;
}

static void plex_on_resolve(int ok, int status,
                            const char *body, size_t len, void *ud)
{
    plex_resolve_ctx_t *ctx = (plex_resolve_ctx_t *)ud;

    EVO_LOGI("plex", "playback: metadata HTTP=%d ok=%d bytes=%zu",
           status, ok, len);

    evo_stream_choice_t *choice =
        (evo_stream_choice_t *)calloc(1, sizeof *choice);

    int resolved = 0;

    if (choice && ok && status == 200 && body && len)
        resolved = plex_resolve_xml(body, len,
                                    ctx->server_index, choice);

    if (ctx->cb)
        ctx->cb(resolved, resolved ? choice : NULL,
                resolved ? 1 : 0, ctx->ud);

    if (!resolved)
        EVO_LOGE("plex", "playback: no usable Direct Play source");

    free(choice);
    free(ctx);
}

static int plex_resolve(const char *item_id,
                        evo_provider_resolve_cb cb, void *ud)
{
    int server_index = -1;
    char key[64];

    if (!cb ||
        !plex_parse_media_id(item_id, "video:",
                             &server_index, key, sizeof key))
        return -1;

    plex_server_t *server = &plex_servers[server_index];

    if (!server->uri[0] || !server->access_token[0])
        return -1;

    plex_resolve_ctx_t *ctx =
        (plex_resolve_ctx_t *)calloc(1, sizeof *ctx);

    if (!ctx)
        return -2;

    ctx->cb = cb;
    ctx->ud = ud;
    ctx->server_index = server_index;
    snprintf(ctx->rating_key, sizeof ctx->rating_key,
             "%s", key);

    char url[1200];

    int n = snprintf(url, sizeof url,
                     "%s/library/metadata/%s",
                     server->uri, key);

    if (n < 0 || (size_t)n >= sizeof url) {
        free(ctx);
        return -1;
    }

    char auth_header[1200];

    n = snprintf(auth_header, sizeof auth_header,
                 "X-Plex-Token: %s",
                 server->access_token);

    if (n < 0 || (size_t)n >= sizeof auth_header) {
        free(ctx);
        return -1;
    }

    const char *headers[] = {
        PLEX_HEADERS,
        PLEX_PRODUCT,
        PLEX_ACCEPT,
        auth_header
    };

    EVO_LOGI("plex", "playback: resolving server=%d ratingKey=%s",
           server_index, key);

    int rc = evo_net_request_async(
        "GET", url, NULL, headers, 4,
        plex_on_resolve, ctx);

    EVO_LOGI("plex", "playback: metadata request queued rc=%d", rc);

    if (rc != 0)
        free(ctx);

    return rc;
}


/* PLEX_TIMELINE_PROGRESS_V1
 * Synchronize playback position with the Plex server.
 * All times sent to Plex are milliseconds.
 */
static void plex_on_timeline(int ok, int status,
                             const char *body, size_t len, void *ud)
{
    (void)body;
    (void)len;
    (void)ud;

    if (!ok || status < 200 || status >= 300)
        EVO_LOGI("plex", "timeline: HTTP=%d ok=%d", status, ok);
}

static void plex_report_progress(const char *item_id,
                                 int64_t pos_sec,
                                 int64_t dur_sec,
                                 evo_provider_play_state_t state)
{
    int server_index = -1;
    char key[64];

    if (!plex_parse_media_id(item_id, "video:",
                             &server_index, key, sizeof key))
        return;

    plex_server_t *server = &plex_servers[server_index];

    if (!server->uri[0] || !server->access_token[0])
        return;

    /*
     * EVO's START callback currently supplies position zero, even
     * when resuming. Avoid resetting Plex's existing viewOffset.
     */
    if (state == EVO_PROVIDER_PLAY_START)
        return;

    if (pos_sec < 0)
        pos_sec = 0;

    if (dur_sec < 0)
        dur_sec = 0;

    const char *plex_state =
        state == EVO_PROVIDER_PLAY_STOP ? "stopped" : "playing";

    char url[1600];

    int n = snprintf(
        url, sizeof url,
        "%s/:/timeline?ratingKey=%s"
        "&key=/library/metadata/%s"
        "&state=%s"
        "&time=%lld"
        "&duration=%lld",
        server->uri,
        key,
        key,
        plex_state,
        (long long)(pos_sec * 1000),
        (long long)(dur_sec * 1000)
    );

    if (n < 0 || (size_t)n >= sizeof url)
        return;

    char auth_header[1200];

    n = snprintf(auth_header, sizeof auth_header,
                 "X-Plex-Token: %s",
                 server->access_token);

    if (n < 0 || (size_t)n >= sizeof auth_header)
        return;

    const char *req_headers[] = {
        PLEX_HEADERS,
        PLEX_PRODUCT,
        PLEX_ACCEPT,
        auth_header
    };

    int rc = evo_net_request_async(
        "GET", url, NULL,
        req_headers, 4,
        plex_on_timeline, NULL
    );

    EVO_LOGI("plex", "timeline: state=%s ratingKey=%s pos=%lld duration=%lld rc=%d",
           plex_state, key,
           (long long)pos_sec,
           (long long)dur_sec,
           rc);
}

/* ------------------------------------------------------------------------- */
/* Details and cast (the Triangle panel)                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    evo_provider_rows_cb cb;
    void *ud;
    int server_index;
} plex_panel_ctx_t;

static int plex_parse_any_media_id(const char *id, int *server_index, char *key, size_t cap)
{
    if (plex_parse_media_id(id, "video:", server_index, key, cap)) return 1;
    if (plex_parse_media_id(id, "show:", server_index, key, cap)) return 1;
    if (plex_parse_media_id(id, "season:", server_index, key, cap)) return 1;
    if (plex_parse_media_id(id, "folder:", server_index, key, cap)) return 1;
    if (plex_parse_media_id(id, "lib:", server_index, key, cap)) return 1;

    if (!id || !server_index || !key || cap == 0) return 0;
    const char *colon = strchr(id, ':');
    if (colon) {
        char *end = NULL;
        long idx = strtol(id, &end, 10);
        if (end == colon && idx >= 0 && idx < plex_server_count) {
            *server_index = (int)idx;
            snprintf(key, cap, "%s", colon + 1);
            return 1;
        }
        const char *colon2 = strchr(colon + 1, ':');
        if (colon2) {
            long idx2 = strtol(colon + 1, &end, 10);
            if (end == colon2 && idx2 >= 0 && idx2 < plex_server_count) {
                *server_index = (int)idx2;
                snprintf(key, cap, "%s", colon2 + 1);
                return 1;
            }
        }
    }
    return 0;
}

static void plex_build_thumb_url(int server_index, const char *thumb, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!thumb || !thumb[0] || server_index < 0 || server_index >= plex_server_count)
        return;

    const plex_server_t *server = &plex_servers[server_index];
    if (thumb[0] == '/') {
        snprintf(out, cap, "%s%s%sX-Plex-Token=%s",
                 server->uri, thumb, strchr(thumb, '?') ? "&" : "?", server->access_token);
    } else if (!strncmp(thumb, "http", 4)) {
        if (strstr(thumb, server->uri) && !strstr(thumb, "X-Plex-Token")) {
            snprintf(out, cap, "%s%sX-Plex-Token=%s",
                     thumb, strchr(thumb, '?') ? "&" : "?", server->access_token);
        } else {
            snprintf(out, cap, "%s", thumb);
        }
    }
}

static void plex_row_add(evo_provider_row_t *rows, int *n, int max,
                         const char *title, const char *detail,
                         const char *art_url, const char *group)
{
    if (*n >= max || !title || !title[0]) return;
    evo_provider_row_t *r = &rows[(*n)++];
    memset(r, 0, sizeof *r);
    r->id[0] = '\0';
    snprintf(r->title, sizeof r->title, "%s", title);
    if (detail && detail[0]) snprintf(r->detail, sizeof r->detail, "%s", detail);
    if (art_url && art_url[0]) snprintf(r->art_url, sizeof r->art_url, "%s", art_url);
    if (group && group[0]) snprintf(r->group, sizeof r->group, "%s", group);
}

static void plex_on_panel_details(int ok, int status, const char *body, size_t len, void *ud)
{
    plex_panel_ctx_t *ctx = (plex_panel_ctx_t *)ud;
    if (!ctx) return;

    if (!ok || status != 200 || !body || !len) {
        if (ctx->cb) ctx->cb(0, NULL, 0, ctx->ud);
        free(ctx);
        return;
    }

    evo_provider_row_t rows[20];
    int n = 0;

    cJSON *root = cJSON_ParseWithLength(body, len);
    cJSON *mc = root ? cJSON_GetObjectItemCaseSensitive(root, "MediaContainer") : NULL;
    cJSON *item = NULL;
    if (mc) {
        cJSON *arr = cJSON_GetObjectItemCaseSensitive(mc, "Metadata");
        if (!arr) arr = cJSON_GetObjectItemCaseSensitive(mc, "Video");
        if (!arr) arr = cJSON_GetObjectItemCaseSensitive(mc, "Directory");
        if (arr && cJSON_IsArray(arr)) item = cJSON_GetArrayItem(arr, 0);
        else if (arr && cJSON_IsObject(arr)) item = arr;
    }

    if (item) {
        /* Rating: audienceRating or rating, "x.x/10", plus contentRating */
        double ar_val = 0.0;
        cJSON *ar = cJSON_GetObjectItemCaseSensitive(item, "audienceRating");
        if (ar) {
            if (cJSON_IsNumber(ar)) ar_val = ar->valuedouble;
            else if (cJSON_IsString(ar) && ar->valuestring) ar_val = atof(ar->valuestring);
        }
        if (ar_val <= 0.0) {
            cJSON *rt = cJSON_GetObjectItemCaseSensitive(item, "rating");
            if (rt) {
                if (cJSON_IsNumber(rt)) ar_val = rt->valuedouble;
                else if (cJSON_IsString(rt) && rt->valuestring) ar_val = atof(rt->valuestring);
            }
        }
        const char *cr = plex_json_string(item, "contentRating");
        char rbuf[160];
        size_t rb = 0;
        rbuf[0] = '\0';
        if (ar_val > 0.0) {
            rb += (size_t)snprintf(rbuf + rb, sizeof rbuf - rb, "%.1f/10", ar_val);
        }
        if (cr && cr[0]) {
            rb += (size_t)snprintf(rbuf + rb, sizeof rbuf - rb, "%s%s", rb ? " · " : "", cr);
        }
        if (rb > 0) {
            plex_row_add(rows, &n, 20, "Rating", rbuf, NULL, NULL);
        }

        /* Genres: Genre[].tag */
        cJSON *genres = cJSON_GetObjectItemCaseSensitive(item, "Genre");
        char gbuf[160];
        size_t gb = 0;
        gbuf[0] = '\0';
        for (int i = 0; genres && cJSON_IsArray(genres) && i < cJSON_GetArraySize(genres) && gb + 2 < sizeof gbuf; ++i) {
            cJSON *g = cJSON_GetArrayItem(genres, i);
            const char *tag = plex_json_string(g, "tag");
            if (tag[0]) {
                gb += (size_t)snprintf(gbuf + gb, sizeof gbuf - gb, "%s%s", gb ? ", " : "", tag);
            }
        }
        if (gb > 0) {
            plex_row_add(rows, &n, 20, "Genres", gbuf, NULL, NULL);
        }

        /* Director: Director[].tag */
        cJSON *directors = cJSON_GetObjectItemCaseSensitive(item, "Director");
        char dbuf[160];
        size_t db = 0;
        dbuf[0] = '\0';
        for (int i = 0; directors && cJSON_IsArray(directors) && i < cJSON_GetArraySize(directors) && db + 2 < sizeof dbuf; ++i) {
            cJSON *d = cJSON_GetArrayItem(directors, i);
            const char *tag = plex_json_string(d, "tag");
            if (tag[0]) {
                db += (size_t)snprintf(dbuf + db, sizeof dbuf - db, "%s%s", db ? ", " : "", tag);
            }
        }
        if (db > 0) {
            plex_row_add(rows, &n, 20, "Director", dbuf, NULL, NULL);
        }

        /* Studio */
        const char *studio = plex_json_string(item, "studio");
        if (!studio[0]) studio = plex_json_string(item, "Studio");
        if (studio[0]) {
            plex_row_add(rows, &n, 20, "Studio", studio, NULL, NULL);
        }

        /* Cast: up to 12 from Role[] */
        cJSON *roles = cJSON_GetObjectItemCaseSensitive(item, "Role");
        int cast_count = 0;
        for (int i = 0; roles && cJSON_IsArray(roles) && i < cJSON_GetArraySize(roles) && cast_count < 12; ++i) {
            cJSON *r = cJSON_GetArrayItem(roles, i);
            const char *name = plex_json_string(r, "tag");
            if (!name[0]) continue;
            const char *role = plex_json_string(r, "role");
            const char *thumb = plex_json_string(r, "thumb");
            char art[EVO_PROVIDER_MAX_ART_URL] = {0};
            if (thumb[0]) {
                plex_build_thumb_url(ctx->server_index, thumb, art, sizeof art);
            }
            plex_row_add(rows, &n, 20, name, role, art, "cast");
            cast_count++;
        }
    } else {
        /* Fallback: XML parser using existing plex_xml_attribute */
        const char *tag = strstr(body, "<Video");
        if (!tag) tag = strstr(body, "<Directory");
        if (tag) {
            const char *tag_end = strchr(tag, '>');
            if (tag_end) {
                char ar_str[32] = {0}, rt_str[32] = {0}, cr_str[64] = {0}, st_str[96] = {0};
                plex_xml_attribute(tag, tag_end, "audienceRating", ar_str, sizeof ar_str);
                plex_xml_attribute(tag, tag_end, "rating", rt_str, sizeof rt_str);
                plex_xml_attribute(tag, tag_end, "contentRating", cr_str, sizeof cr_str);
                plex_xml_attribute(tag, tag_end, "studio", st_str, sizeof st_str);

                double ar_val = ar_str[0] ? atof(ar_str) : (rt_str[0] ? atof(rt_str) : 0.0);
                char rbuf[160];
                size_t rb = 0;
                rbuf[0] = '\0';
                if (ar_val > 0.0) {
                    rb += (size_t)snprintf(rbuf + rb, sizeof rbuf - rb, "%.1f/10", ar_val);
                }
                if (cr_str[0]) {
                    rb += (size_t)snprintf(rbuf + rb, sizeof rbuf - rb, "%s%s", rb ? " · " : "", cr_str);
                }
                if (rb > 0) {
                    plex_row_add(rows, &n, 20, "Rating", rbuf, NULL, NULL);
                }

                /* Genres from <Genre tag="..."/> */
                char gbuf[160];
                size_t gb = 0;
                gbuf[0] = '\0';
                const char *p = tag_end;
                while ((p = strstr(p, "<Genre")) != NULL) {
                    const char *ge = strchr(p, '>');
                    if (!ge) break;
                    char val[64] = {0};
                    if (plex_xml_attribute(p, ge, "tag", val, sizeof val) && val[0]) {
                        if (gb + 2 < sizeof gbuf) {
                            gb += (size_t)snprintf(gbuf + gb, sizeof gbuf - gb, "%s%s", gb ? ", " : "", val);
                        }
                    }
                    p = ge + 1;
                }
                if (gb > 0) {
                    plex_row_add(rows, &n, 20, "Genres", gbuf, NULL, NULL);
                }

                /* Director from <Director tag="..."/> */
                char dbuf[160];
                size_t db = 0;
                dbuf[0] = '\0';
                p = tag_end;
                while ((p = strstr(p, "<Director")) != NULL) {
                    const char *de = strchr(p, '>');
                    if (!de) break;
                    char val[64] = {0};
                    if (plex_xml_attribute(p, de, "tag", val, sizeof val) && val[0]) {
                        if (db + 2 < sizeof dbuf) {
                            db += (size_t)snprintf(dbuf + db, sizeof dbuf - db, "%s%s", db ? ", " : "", val);
                        }
                    }
                    p = de + 1;
                }
                if (db > 0) {
                    plex_row_add(rows, &n, 20, "Director", dbuf, NULL, NULL);
                }

                if (st_str[0]) {
                    plex_row_add(rows, &n, 20, "Studio", st_str, NULL, NULL);
                }

                /* Roles from <Role tag="..." role="..." thumb="..."/> */
                int cast_count = 0;
                p = tag_end;
                while ((p = strstr(p, "<Role")) != NULL && cast_count < 12) {
                    const char *re = strchr(p, '>');
                    if (!re) break;
                    char name[96] = {0}, role[96] = {0}, thumb[256] = {0};
                    plex_xml_attribute(p, re, "tag", name, sizeof name);
                    plex_xml_attribute(p, re, "role", role, sizeof role);
                    plex_xml_attribute(p, re, "thumb", thumb, sizeof thumb);
                    if (name[0]) {
                        char art[EVO_PROVIDER_MAX_ART_URL] = {0};
                        if (thumb[0]) {
                            plex_build_thumb_url(ctx->server_index, thumb, art, sizeof art);
                        }
                        plex_row_add(rows, &n, 20, name, role, art, "cast");
                        cast_count++;
                    }
                    p = re + 1;
                }
            }
        }
    }

    if (root) cJSON_Delete(root);

    if (ctx->cb) ctx->cb(n > 0, rows, n, ctx->ud);
    free(ctx);
}

static int plex_panel(const char *item_id, const char *kind, evo_provider_rows_cb cb, void *ud)
{
    if (!item_id || !kind || strcmp(kind, "details") != 0) return -1;
    int server_index = -1;
    char key[64];
    if (!plex_parse_any_media_id(item_id, &server_index, key, sizeof key))
        return -1;
    if (server_index < 0 || server_index >= plex_server_count)
        return -1;

    plex_server_t *server = &plex_servers[server_index];
    if (!server->uri[0] || !server->access_token[0])
        return -1;

    plex_panel_ctx_t *ctx = (plex_panel_ctx_t *)calloc(1, sizeof *ctx);
    if (!ctx) return -2;
    ctx->cb = cb;
    ctx->ud = ud;
    ctx->server_index = server_index;

    char url[1200];
    int n = snprintf(url, sizeof url, "%s/library/metadata/%s", server->uri, key);
    if (n < 0 || (size_t)n >= sizeof url) {
        free(ctx);
        return -1;
    }

    char auth_header[1200];
    n = snprintf(auth_header, sizeof auth_header, "X-Plex-Token: %s", server->access_token);
    if (n < 0 || (size_t)n >= sizeof auth_header) {
        free(ctx);
        return -1;
    }

    const char *headers[] = {
        PLEX_HEADERS,
        PLEX_PRODUCT,
        PLEX_ACCEPT,
        "X-Plex-Accept: application/json",
        auth_header
    };

    int rc = evo_net_request_async("GET", url, NULL, headers, 5, plex_on_panel_details, ctx);
    if (rc != 0) free(ctx);
    return rc;
}

const evo_provider_t evo_provider_plex = {
    .id = "plex", .name = "Plex", .icon = "brand_plex.png",
    .caps = EVO_PROVIDER_CAP_AUTH | EVO_PROVIDER_CAP_CATALOG |
            EVO_PROVIDER_CAP_RESOLVE | EVO_PROVIDER_CAP_PROGRESS,
    .api_version = EVO_PROVIDER_API_VERSION,
    .init = plex_init, .shutdown = plex_shutdown,
    .is_configured = plex_configured, .auth = plex_auth,
    .list_catalog = plex_list_catalog,
    .resolve = plex_resolve,
    .report_progress = plex_report_progress,
    .needs_sign_in = plex_needs_sign_in,
    .qc_start = plex_qc_start, .qc_poll = plex_qc_poll,
    .qc_cancel = plex_qc_cancel,
    .is_signed_in = plex_signed_in, .sign_out = plex_sign_out,
    .ui_embedded = "mediaserver",
    .panel = plex_panel
};
