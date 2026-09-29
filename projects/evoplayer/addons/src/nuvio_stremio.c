/*
 * nuvio_stremio.c — see nuvio_stremio.h.
 *
 * libc use is deliberately narrow: only calls the app module's libc already
 * resolves elsewhere in EVO (docs/research/web-browser-dialog.md, "Lessons":
 * a libc name the native-app surface lacks is a call through a null import).
 * Hence the hand-rolled case-insensitive search and character classes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nuvio_stremio.h"
#include "cJSON.h"

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static int lower_c(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int is_digit_c(int c) { return c >= '0' && c <= '9'; }
static int is_alnum_c(int c)
{
    return is_digit_c(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static const char *ci_find(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle) return NULL;
    size_t n = strlen(needle);
    for (; *hay; ++hay) {
        size_t i = 0;
        while (i < n && hay[i] && lower_c((unsigned char)hay[i]) == lower_c((unsigned char)needle[i]))
            ++i;
        if (i == n) return hay;
    }
    return NULL;
}

static int ends_with_ci(const char *s, size_t len, const char *suffix)
{
    size_t n = strlen(suffix);
    if (len < n) return 0;
    for (size_t i = 0; i < n; ++i)
        if (lower_c((unsigned char)s[len - n + i]) != lower_c((unsigned char)suffix[i]))
            return 0;
    return 1;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!cap) return;
    if (!src) { dst[0] = '\0'; return; }
    snprintf(dst, cap, "%s", src);
}

/* A JSON value as text: strings as-is, numbers without a trailing ".0". */
static void json_text(const cJSON *v, char *out, size_t cap)
{
    if (!cap) return;
    out[0] = '\0';
    if (!v) return;
    if (cJSON_IsString(v) && v->valuestring) {
        copy_str(out, cap, v->valuestring);
    } else if (cJSON_IsNumber(v)) {
        double d = v->valuedouble;
        if (d == (double)(long long)d) snprintf(out, cap, "%lld", (long long)d);
        else                           snprintf(out, cap, "%.1f", d);
    }
}

static void obj_text(const cJSON *obj, const char *key, char *out, size_t cap)
{
    json_text(cJSON_GetObjectItemCaseSensitive(obj, key), out, cap);
}

static const char *obj_str(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;
}

static int obj_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v)) return (int)v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring && is_digit_c((unsigned char)v->valuestring[0]))
        return atoi(v->valuestring);
    return def;
}

static int64_t obj_i64(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v)) return (int64_t)v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring) return (int64_t)atoll(v->valuestring);
    return 0;
}

/* Split a canonical base at its query: path is base[0..*path_len), query is
 * the rest ("" or "?..."). */
static void split_base(const char *base, size_t *path_len, const char **query)
{
    const char *q = strchr(base, '?');
    *path_len = q ? (size_t)(q - base) : strlen(base);
    *query = q ? q : "";
}

/* ------------------------------------------------------------------------- */
/* URLs                                                                      */
/* ------------------------------------------------------------------------- */

int nuvio_canonical_base(const char *manifest_url, char *out, size_t cap)
{
    if (!manifest_url || !out || cap == 0) return -1;
    while (*manifest_url == ' ' || *manifest_url == '\t') manifest_url++;

    char buf[NUVIO_MAX_BASE_URL + 32];
    if (!strncmp(manifest_url, "stremio://", 10))
        snprintf(buf, sizeof buf, "https://%s", manifest_url + 10);
    else
        snprintf(buf, sizeof buf, "%s", manifest_url);

    /* Trailing whitespace and CR from a text file line. */
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == ' ' || buf[n - 1] == '\r' || buf[n - 1] == '\n' ||
                 buf[n - 1] == '\t'))
        buf[--n] = '\0';

    if (strncmp(buf, "http://", 7) != 0 && strncmp(buf, "https://", 8) != 0)
        return -1;

    char *q = strchr(buf, '?');
    char query[NUVIO_MAX_BASE_URL];
    query[0] = '\0';
    if (q) {
        snprintf(query, sizeof query, "%s", q);
        *q = '\0';
    }

    n = strlen(buf);
    while (n && buf[n - 1] == '/') buf[--n] = '\0';
    if (ends_with_ci(buf, n, "/manifest.json")) {
        n -= strlen("/manifest.json");
        buf[n] = '\0';
    }
    while (n && buf[n - 1] == '/') buf[--n] = '\0';

    /* Nothing left past the scheme is not an addon. */
    const char *host = strstr(buf, "://");
    if (!host || !host[3]) return -1;

    int w = snprintf(out, cap, "%s%s", buf, query);
    return (w < 0 || (size_t)w >= cap) ? -1 : 0;
}

int nuvio_manifest_url(const char *base, char *out, size_t cap)
{
    size_t pl; const char *query;
    split_base(base, &pl, &query);
    int w = snprintf(out, cap, "%.*s/manifest.json%s", (int)pl, base, query);
    return (w < 0 || (size_t)w >= cap) ? -1 : 0;
}

int nuvio_resource_url(const char *base, const char *resource, const char *type,
                       const char *id, char *out, size_t cap)
{
    char et[96], ei[EVO_PROVIDER_MAX_ITEM_ID * 3];
    if (evo_provider_url_escape(type, et, sizeof et) < 0) return -1;
    if (evo_provider_url_escape(id, ei, sizeof ei) < 0) return -1;
    size_t pl; const char *query;
    split_base(base, &pl, &query);
    int w = snprintf(out, cap, "%.*s/%s/%s/%s.json%s", (int)pl, base, resource,
                     et, ei, query);
    return (w < 0 || (size_t)w >= cap) ? -1 : 0;
}

int nuvio_catalog_url(const char *base, const char *type, const char *catalog_id,
                      const char *search, int skip, char *out, size_t cap)
{
    char et[96], ec[300];
    if (evo_provider_url_escape(type, et, sizeof et) < 0) return -1;
    if (evo_provider_url_escape(catalog_id, ec, sizeof ec) < 0) return -1;

    size_t pl; const char *query;
    split_base(base, &pl, &query);

    char extra[600];
    extra[0] = '\0';
    if (search && *search) {
        char es[400];
        if (evo_provider_url_escape(search, es, sizeof es) < 0) return -1;
        snprintf(extra, sizeof extra, "search=%s", es);
        if (skip > 0) {
            size_t el = strlen(extra);
            snprintf(extra + el, sizeof extra - el, "&skip=%d", skip);
        }
    } else if (skip > 0) {
        snprintf(extra, sizeof extra, "skip=%d", skip);
    }

    int w;
    if (extra[0])
        w = snprintf(out, cap, "%.*s/catalog/%s/%s/%s.json%s", (int)pl, base, et, ec,
                     extra, query);
    else
        w = snprintf(out, cap, "%.*s/catalog/%s/%s.json%s", (int)pl, base, et, ec, query);
    return (w < 0 || (size_t)w >= cap) ? -1 : 0;
}

/* ------------------------------------------------------------------------- */
/* Manifest                                                                  */
/* ------------------------------------------------------------------------- */

static int string_list(const cJSON *arr, char (*out)[24], int cap)
{
    int n = 0;
    const cJSON *it;
    if (!cJSON_IsArray(arr)) return 0;
    for (it = arr->child; it && n < cap; it = it->next) {
        if (!cJSON_IsString(it) || !it->valuestring || !it->valuestring[0]) continue;
        copy_str(out[n++], 24, it->valuestring);
    }
    return n;
}

static int list_has(const cJSON *arr, const char *name)
{
    const cJSON *it;
    if (!cJSON_IsArray(arr)) return 0;
    for (it = arr->child; it; it = it->next)
        if (cJSON_IsString(it) && it->valuestring && !strcmp(it->valuestring, name))
            return 1;
    return 0;
}

static void parse_catalog_desc(const cJSON *c, nuvio_catalog_t *out)
{
    memset(out, 0, sizeof *out);
    obj_text(c, "type", out->type, sizeof out->type);
    obj_text(c, "id",   out->id,   sizeof out->id);
    obj_text(c, "name", out->name, sizeof out->name);
    if (!out->name[0]) copy_str(out->name, sizeof out->name, out->id);

    const cJSON *sh = cJSON_GetObjectItemCaseSensitive(c, "showInHome");
    out->show_in_home = cJSON_IsFalse(sh) ? 0 : 1;

    int required_search = 0, required_other = 0;

    /* The current form: extra[] of {name, isRequired, options}. */
    const cJSON *extra = cJSON_GetObjectItemCaseSensitive(c, "extra");
    if (cJSON_IsArray(extra)) {
        for (const cJSON *e = extra->child; e; e = e->next) {
            const char *name = cJSON_IsString(e) ? e->valuestring : obj_str(e, "name");
            if (!name) continue;
            int req = cJSON_IsObject(e) && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "isRequired"));
            if (!strcmp(name, "search")) { out->supports_search = 1; if (req) required_search = 1; }
            else if (!strcmp(name, "skip")) out->supports_skip = 1;
            else if (req) required_other = 1;
        }
    }
    /* The legacy form: extraSupported[] / extraRequired[] of names. */
    const cJSON *sup = cJSON_GetObjectItemCaseSensitive(c, "extraSupported");
    const cJSON *req = cJSON_GetObjectItemCaseSensitive(c, "extraRequired");
    if (list_has(sup, "search")) out->supports_search = 1;
    if (list_has(sup, "skip"))   out->supports_skip = 1;
    if (cJSON_IsArray(req)) {
        for (const cJSON *e = req->child; e; e = e->next) {
            if (!cJSON_IsString(e) || !e->valuestring) continue;
            if (!strcmp(e->valuestring, "search")) { out->supports_search = 1; required_search = 1; }
            else required_other = 1;
        }
    }

    out->needs_other_extra = required_other;
    out->search_only = required_search && !required_other;
}

int nuvio_parse_manifest(const char *json, size_t len, const char *base,
                         nuvio_addon_t *out)
{
    if (!json || !out) return -1;
    memset(out, 0, sizeof *out);
    copy_str(out->base_url, sizeof out->base_url, base);

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return -1; }

    obj_text(root, "id",   out->id,   sizeof out->id);
    obj_text(root, "name", out->name, sizeof out->name);
    if (!out->id[0]) { cJSON_Delete(root); return -1; }
    if (!out->name[0]) copy_str(out->name, sizeof out->name, out->id);

    out->type_count   = string_list(cJSON_GetObjectItemCaseSensitive(root, "types"),
                                    out->types, NUVIO_MAX_TYPES);
    out->prefix_count = string_list(cJSON_GetObjectItemCaseSensitive(root, "idPrefixes"),
                                    out->prefixes, NUVIO_MAX_PREFIXES);

    const cJSON *cats = cJSON_GetObjectItemCaseSensitive(root, "catalogs");
    if (cJSON_IsArray(cats)) {
        for (const cJSON *c = cats->child; c && out->catalog_count < NUVIO_MAX_CATALOGS; c = c->next) {
            if (!cJSON_IsObject(c)) continue;
            nuvio_catalog_t *d = &out->catalogs[out->catalog_count];
            parse_catalog_desc(c, d);
            if (d->type[0] && d->id[0]) out->catalog_count++;
        }
    }

    const cJSON *res = cJSON_GetObjectItemCaseSensitive(root, "resources");
    if (cJSON_IsArray(res)) {
        for (const cJSON *r = res->child; r && out->resource_count < NUVIO_MAX_RESOURCES; r = r->next) {
            nuvio_resource_t *d = &out->resources[out->resource_count];
            memset(d, 0, sizeof *d);
            if (cJSON_IsString(r) && r->valuestring) {
                copy_str(d->name, sizeof d->name, r->valuestring);
            } else if (cJSON_IsObject(r)) {
                obj_text(r, "name", d->name, sizeof d->name);
                d->type_count   = string_list(cJSON_GetObjectItemCaseSensitive(r, "types"),
                                              d->types, NUVIO_MAX_TYPES);
                d->prefix_count = string_list(cJSON_GetObjectItemCaseSensitive(r, "idPrefixes"),
                                              d->prefixes, NUVIO_MAX_PREFIXES);
            }
            if (d->name[0]) out->resource_count++;
        }
    }

    cJSON_Delete(root);
    return 0;
}

int nuvio_addon_serves(const nuvio_addon_t *a, const char *resource,
                       const char *type, const char *id)
{
    if (!a || !resource) return 0;
    for (int i = 0; i < a->resource_count; ++i) {
        const nuvio_resource_t *r = &a->resources[i];
        if (strcmp(r->name, resource) != 0) continue;

        /* A bare-string resource inherits the manifest's types; so does an
         * object that lists none. */
        const char (*types)[24] = r->type_count ? r->types : a->types;
        int type_count          = r->type_count ? r->type_count : a->type_count;
        if (type && type_count) {
            int ok = 0;
            for (int t = 0; t < type_count; ++t)
                if (!strcmp(types[t], type)) { ok = 1; break; }
            if (!ok) continue;
        }

        const char (*pre)[24] = r->prefix_count ? r->prefixes : a->prefixes;
        int pre_count         = r->prefix_count ? r->prefix_count : a->prefix_count;
        if (id && pre_count) {
            int ok = 0;
            for (int p = 0; p < pre_count; ++p)
                if (!strncmp(id, pre[p], strlen(pre[p]))) { ok = 1; break; }
            if (!ok) continue;
        }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Catalog                                                                   */
/* ------------------------------------------------------------------------- */

int nuvio_parse_catalog(const char *json, size_t len, const char *fallback_type,
                        evo_provider_item_t *out, int cap, int *raw_count)
{
    if (raw_count) *raw_count = 0;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return -1; }

    const cJSON *metas = cJSON_GetObjectItemCaseSensitive(root, "metas");
    int n = 0;
    if (cJSON_IsArray(metas)) {
        if (raw_count) *raw_count = cJSON_GetArraySize(metas);
        for (const cJSON *m = metas->child; m && n < cap; m = m->next) {
            if (!cJSON_IsObject(m)) continue;
            const char *id = obj_str(m, "id");
            if (!id || !*id) continue;
            char type[24];
            obj_text(m, "type", type, sizeof type);
            if (!type[0]) copy_str(type, sizeof type, fallback_type);
            if (!type[0] || strchr(type, ':')) continue;

            evo_provider_item_t *it = &out[n];
            evo_provider_item_clear(it);
            int w = snprintf(it->id, sizeof it->id, "m:%s:%s", type, id);
            if (w < 0 || (size_t)w >= sizeof it->id) continue;   /* never truncate */

            int dup = 0;
            for (int j = 0; j < n; ++j)
                if (!strcmp(out[j].id, it->id)) { dup = 1; break; }
            if (dup) continue;

            obj_text(m, "name", it->title, sizeof it->title);
            if (!it->title[0]) copy_str(it->title, sizeof it->title, id);

            char rel[32], rating[16];
            obj_text(m, "releaseInfo", rel, sizeof rel);
            if (!rel[0]) obj_text(m, "year", rel, sizeof rel);
            obj_text(m, "imdbRating", rating, sizeof rating);
            if (rel[0] && rating[0])
                snprintf(it->subtitle, sizeof it->subtitle, "%s  -  IMDb %s", rel, rating);
            else if (rel[0])
                copy_str(it->subtitle, sizeof it->subtitle, rel);
            else if (rating[0])
                snprintf(it->subtitle, sizeof it->subtitle, "IMDb %s", rating);

            obj_text(m, "description", it->overview, sizeof it->overview);
            obj_text(m, "poster", it->art_url, sizeof it->art_url);
            obj_text(m, "background", it->backdrop_url, sizeof it->backdrop_url);
            copy_str(it->parent_id, sizeof it->parent_id, "");
            it->is_folder = 1;
            ++n;
        }
    }
    cJSON_Delete(root);
    return n;
}

/* ------------------------------------------------------------------------- */
/* Meta                                                                      */
/* ------------------------------------------------------------------------- */

void nuvio_meta_free(nuvio_meta_t *m)
{
    if (!m) return;
    free(m->videos);
    m->videos = NULL;
    m->video_count = 0;
}

int nuvio_parse_meta(const char *json, size_t len, nuvio_meta_t *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return -1; }

    const cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "meta");
    if (!cJSON_IsObject(m)) { cJSON_Delete(root); return -1; }

    obj_text(m, "id",   out->id,   sizeof out->id);
    obj_text(m, "type", out->type, sizeof out->type);
    obj_text(m, "name", out->name, sizeof out->name);
    obj_text(m, "releaseInfo", out->release_info, sizeof out->release_info);
    obj_text(m, "description", out->description, sizeof out->description);
    obj_text(m, "poster",      out->poster,      sizeof out->poster);
    obj_text(m, "background",  out->background,  sizeof out->background);
    {
        char rt[32];
        obj_text(m, "runtime", rt, sizeof rt);   /* "58 min" */
        int mins = atoi(rt);
        out->runtime_sec = mins > 0 ? (int64_t)mins * 60 : 0;
    }

    const cJSON *videos = cJSON_GetObjectItemCaseSensitive(m, "videos");
    int total = cJSON_IsArray(videos) ? cJSON_GetArraySize(videos) : 0;
    if (total > 0) {
        out->videos = (nuvio_video_t *)calloc((size_t)total, sizeof *out->videos);
        if (!out->videos) { cJSON_Delete(root); return -1; }
        for (const cJSON *v = videos->child; v; v = v->next) {
            if (!cJSON_IsObject(v)) continue;
            nuvio_video_t *d = &out->videos[out->video_count];
            obj_text(v, "id", d->id, sizeof d->id);
            if (!d->id[0]) continue;
            obj_text(v, "title", d->title, sizeof d->title);
            if (!d->title[0]) obj_text(v, "name", d->title, sizeof d->title);
            obj_text(v, "overview", d->overview, sizeof d->overview);
            if (!d->overview[0]) obj_text(v, "description", d->overview, sizeof d->overview);
            obj_text(v, "thumbnail", d->thumbnail, sizeof d->thumbnail);
            obj_text(v, "released", d->released, sizeof d->released);
            d->season  = obj_int(v, "season", 0);
            d->episode = obj_int(v, "episode", obj_int(v, "number", 0));
            out->video_count++;
        }
    }

    cJSON_Delete(root);
    return out->id[0] ? 0 : -1;
}

int nuvio_meta_seasons(const nuvio_meta_t *m, int *out, int cap)
{
    int n = 0, has_zero = 0;
    for (int i = 0; i < m->video_count; ++i) {
        int s = m->videos[i].season;
        if (s <= 0) { has_zero = 1; continue; }
        int seen = 0;
        for (int j = 0; j < n; ++j) if (out[j] == s) { seen = 1; break; }
        if (!seen && n < cap) out[n++] = s;
    }
    /* insertion sort; a series has tens of seasons at most */
    for (int i = 1; i < n; ++i) {
        int v = out[i], j = i - 1;
        while (j >= 0 && out[j] > v) { out[j + 1] = out[j]; --j; }
        out[j + 1] = v;
    }
    if (has_zero && n < cap) out[n++] = 0;
    return n;
}

/* ------------------------------------------------------------------------- */
/* Magnets                                                                   */
/* ------------------------------------------------------------------------- */

void nuvio_url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; ++r) {
        if (r[0] == '%' && r[1] && r[2]) {
            int hi = r[1], lo = r[2], v = 0, ok = 1;
            for (int k = 0; k < 2; ++k) {
                int c = k ? lo : hi, d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else { ok = 0; break; }
                v = v * 16 + d;
            }
            if (ok) { *w++ = (char)v; r += 2; continue; }
        }
        *w++ = *r;
    }
    *w = '\0';
}

int nuvio_build_magnet(const char *info_hash, const char *const *sources,
                       int source_count, int file_idx, const char *filename,
                       int season, int episode, char *out, size_t cap)
{
    if (!info_hash || !*info_hash || !out || cap == 0) return -1;
    for (const char *h = info_hash; *h; ++h)
        if (!is_alnum_c((unsigned char)*h)) return -1;

    /* The hints are what make a season pack playable, so they are placed
     * first in the budget; trackers fill whatever room is left. */
    char hints[700];
    size_t hl = 0;
    hints[0] = '\0';
    if (file_idx >= 0)
        hl += (size_t)snprintf(hints + hl, sizeof hints - hl, "&evo_fidx=%d", file_idx);
    if (filename && *filename) {
        char ef[600];
        if (evo_provider_url_escape(filename, ef, sizeof ef) >= 0 &&
            hl + strlen(ef) + 9 < sizeof hints)
            hl += (size_t)snprintf(hints + hl, sizeof hints - hl, "&evo_fn=%s", ef);
    }
    if (season > 0 && episode > 0 && hl + 32 < sizeof hints)
        hl += (size_t)snprintf(hints + hl, sizeof hints - hl, "&evo_s=%d&evo_e=%d",
                               season, episode);

    int w = snprintf(out, cap, "magnet:?xt=urn:btih:%s", info_hash);
    if (w < 0 || (size_t)w + hl >= cap) return -1;
    size_t ol = (size_t)w;

    int trackers = 0;
    for (int i = 0; i < source_count && trackers < 8; ++i) {
        const char *s = sources[i];
        if (!s) continue;
        if (!strncmp(s, "tracker:", 8)) s += 8;
        else if (!strncmp(s, "dht:", 4)) continue;
        if (strncmp(s, "udp://", 6) && strncmp(s, "http://", 7) && strncmp(s, "https://", 8))
            continue;
        char es[400];
        if (evo_provider_url_escape(s, es, sizeof es) < 0) continue;
        size_t need = 4 + strlen(es);
        if (ol + need + hl >= cap) break;
        ol += (size_t)snprintf(out + ol, cap - ol, "&tr=%s", es);
        ++trackers;
    }
    snprintf(out + ol, cap - ol, "%s", hints);
    return 0;
}

int nuvio_magnet_parse(const char *url, nuvio_magnet_hints_t *h)
{
    if (!url || !h || strncmp(url, "magnet:?", 8) != 0) return -1;
    memset(h, 0, sizeof *h);
    h->file_idx = -1;

    const char *p = url + 8;
    while (*p) {
        const char *end = strchr(p, '&');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        char kv[800];
        if (n < sizeof kv) {
            memcpy(kv, p, n);
            kv[n] = '\0';
            char *eq = strchr(kv, '=');
            if (eq) {
                *eq = '\0';
                char *val = eq + 1;
                if (!strcmp(kv, "xt")) {
                    const char *b = ci_find(val, "urn:btih:");
                    if (b) copy_str(h->info_hash, sizeof h->info_hash, b + 9);
                } else if (!strcmp(kv, "evo_fidx")) {
                    h->file_idx = atoi(val);
                } else if (!strcmp(kv, "evo_fn")) {
                    nuvio_url_decode(val);
                    copy_str(h->filename, sizeof h->filename, val);
                } else if (!strcmp(kv, "evo_s")) {
                    h->season = atoi(val);
                } else if (!strcmp(kv, "evo_e")) {
                    h->episode = atoi(val);
                }
            }
        }
        if (!end) break;
        p = end + 1;
    }
    return h->info_hash[0] ? 0 : -1;
}

int nuvio_magnet_strip_hints(const char *url, char *out, size_t cap)
{
    if (!url || !out || cap == 0 || strncmp(url, "magnet:?", 8) != 0) return -1;
    size_t ol = (size_t)snprintf(out, cap, "magnet:?");
    int first = 1;
    const char *p = url + 8;
    while (*p) {
        const char *end = strchr(p, '&');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n && strncmp(p, "evo_", 4) != 0) {
            if (ol + n + 2 >= cap) return -1;
            if (!first) out[ol++] = '&';
            memcpy(out + ol, p, n);
            ol += n;
            out[ol] = '\0';
            first = 0;
        }
        if (!end) break;
        p = end + 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Streams                                                                   */
/* ------------------------------------------------------------------------- */

int nuvio_guess_quality(const char *text)
{
    if (!text) return 0;
    if (ci_find(text, "2160p") || ci_find(text, "4k") || ci_find(text, "uhd")) return 2160;
    if (ci_find(text, "1440p")) return 1440;
    if (ci_find(text, "1080p") || ci_find(text, "1080i")) return 1080;
    if (ci_find(text, "720p")) return 720;
    if (ci_find(text, "576p")) return 576;
    if (ci_find(text, "480p")) return 480;
    if (ci_find(text, "360p")) return 360;
    return 0;
}

int nuvio_parse_streams(const char *json, size_t len, const char *addon_name,
                        int season, int episode,
                        nuvio_stream_t *out, int cap, int *count)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return -1; }

    int added = 0;
    const cJSON *streams = cJSON_GetObjectItemCaseSensitive(root, "streams");
    if (cJSON_IsArray(streams)) {
        for (const cJSON *s = streams->child; s && *count < cap; s = s->next) {
            if (!cJSON_IsObject(s)) continue;
            nuvio_stream_t *d = &out[*count];
            memset(d, 0, sizeof *d);

            const cJSON *hints = cJSON_GetObjectItemCaseSensitive(s, "behaviorHints");
            if (cJSON_IsObject(hints)) {
                obj_text(hints, "filename", d->filename, sizeof d->filename);
                d->size_bytes = obj_i64(hints, "videoSize");
                const cJSON *ph = cJSON_GetObjectItemCaseSensitive(hints, "proxyHeaders");
                const cJSON *rq = cJSON_IsObject(ph) ? cJSON_GetObjectItemCaseSensitive(ph, "request") : NULL;
                if (cJSON_IsObject(rq) && rq->child) d->needs_headers = 1;
            }

            const char *url = obj_str(s, "url");
            const char *hash = obj_str(s, "infoHash");
            if (url && (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8))) {
                if (strlen(url) >= sizeof d->url) continue;
                copy_str(d->url, sizeof d->url, url);
            } else if (url && !strncmp(url, "magnet:?", 8)) {
                if (strlen(url) >= sizeof d->url) continue;
                copy_str(d->url, sizeof d->url, url);
                d->needs_resolver = 1;
            } else if (hash && *hash) {
                const char *srcs[16];
                int ns = 0;
                const cJSON *sa = cJSON_GetObjectItemCaseSensitive(s, "sources");
                if (cJSON_IsArray(sa))
                    for (const cJSON *e = sa->child; e && ns < 16; e = e->next)
                        if (cJSON_IsString(e) && e->valuestring) srcs[ns++] = e->valuestring;
                int fidx = obj_int(s, "fileIdx", -1);
                if (nuvio_build_magnet(hash, srcs, ns, fidx, d->filename, season, episode,
                                       d->url, sizeof d->url) != 0)
                    continue;
                d->needs_resolver = 1;
            } else {
                continue;       /* ytId / externalUrl / nothing: not playable here */
            }

            copy_str(d->addon_name, sizeof d->addon_name, addon_name);
            obj_text(s, "name", d->name, sizeof d->name);
            obj_text(s, "description", d->description, sizeof d->description);
            if (!d->description[0]) obj_text(s, "title", d->description, sizeof d->description);

            char probe[1024];
            snprintf(probe, sizeof probe, "%s %s %s", d->name, d->description, d->filename);
            d->quality = nuvio_guess_quality(probe);

            ++*count;
            ++added;
        }
    }
    cJSON_Delete(root);
    return added;
}

static int stream_before(const nuvio_stream_t *a, const nuvio_stream_t *b)
{
    if (a->needs_headers != b->needs_headers) return a->needs_headers < b->needs_headers;
    if (a->quality != b->quality) return a->quality > b->quality;
    return 0;   /* equal: keep input order */
}

void nuvio_sort_streams(nuvio_stream_t *s, int count)
{
    if (count < 2) return;
    int *idx = (int *)malloc((size_t)count * sizeof *idx);
    nuvio_stream_t *tmp = (nuvio_stream_t *)malloc((size_t)count * sizeof *tmp);
    if (!idx || !tmp) { free(idx); free(tmp); return; }

    /* Stable insertion sort over indices - qsort is not stable, and moving
     * the 3 KB structs themselves n^2 times would be wasteful. */
    for (int i = 0; i < count; ++i) idx[i] = i;
    for (int i = 1; i < count; ++i) {
        int v = idx[i], j = i - 1;
        while (j >= 0 && stream_before(&s[v], &s[idx[j]])) { idx[j + 1] = idx[j]; --j; }
        idx[j + 1] = v;
    }
    for (int i = 0; i < count; ++i) tmp[i] = s[idx[i]];
    memcpy(s, tmp, (size_t)count * sizeof *tmp);
    free(idx);
    free(tmp);
}
