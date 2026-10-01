/*
 * provider_stremio.c — Stremio addons, browsed natively
 * (docs/addons/native-providers.md, Phase B).
 *
 * A Stremio addon is a small web service answering four JSON requests:
 *
 *   <base>/manifest.json                 what it serves (catalogs, resources)
 *   <base>/catalog/<type>/<id>.json      a list of metas (posters)
 *   <base>/meta/<type>/<id>.json         one title; a series lists its videos
 *   <base>/stream/<type>/<id>.json       playable links for one title/episode
 *
 * Nuvio is a web front end over exactly this; here EVO is the client, so the
 * screens are EVO's own (rml/mediaserver.rml, the same poster grid as Emby and
 * Jellyfin) and nothing runs in the PS5 browser.
 *
 * The user's addons are a list of manifest URLs in /data/evoplayer/addons.json.
 * Only streams with a direct http(s) `url` are offered; torrent-only streams
 * (infoHash) need a debrid resolver and are skipped.
 *
 * CATALOG IDS (only this file builds or reads them; <a> is the addon index)
 *
 *   c:<a>:<type>:<catalog>        an addon catalog         (folder)
 *   s:<a>:<type>:<id>             a series -> its seasons  (folder)
 *   ss:<a>:<type>:<id>:<season>   a season -> episodes     (folder)
 *   m:<a>:<type>:<id>             a playable title
 *   e:<a>:<type>:<videoId>        a playable episode
 *   x:<a>                         an addon that failed to load (empty folder)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "evo_provider.h"
#include "evo_net.h"
#include "cJSON.h"
#include "evo_data_path.h"
#include "evo_provider_log.h"

#define AD_CONF      "addons.json"
#define AD_MAX       12
#define AD_MAX_CATS  24
#define AD_URL       512
#define AD_CHOICES   16

typedef struct {
    char type[24];
    char id[96];
    char name[96];
    int  searchable;
    int  needs_extra;            /* a required extra (genre, search): not browsable */
} ad_cat_t;

typedef struct {
    char url[AD_URL];            /* the manifest URL as added */
    char base[AD_URL];           /* without /manifest.json */
    char name[96];
    int  loaded;                 /* 0 not fetched, 1 ok, -1 failed */
    ad_cat_t cats[AD_MAX_CATS];
    int  ncats;
    int  has_meta, has_stream;
    char meta_types[160], stream_types[160];   /* ",movie,series," - "" = any */
    char prefixes[384];                          /* ",tt,kitsu:," - "" = any */
} addon_t;

static addon_t g_ad[AD_MAX];
static int     g_nad;

/* ------------------------------------------------------------------------- */
/* Config                                                                    */
/* ------------------------------------------------------------------------- */

static void ad_set_base(addon_t *a)
{
    snprintf(a->base, sizeof a->base, "%s", a->url);
    size_t n = strlen(a->base);
    const char *tail = "/manifest.json";
    size_t t = strlen(tail);
    if (n >= t && !strcmp(a->base + n - t, tail)) a->base[n - t] = '\0';
    n = strlen(a->base);
    while (n > 0 && a->base[n - 1] == '/') a->base[--n] = '\0';
}

static int ad_save(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "addons", arr);
    for (int i = 0; i < g_nad; ++i) cJSON_AddItemToArray(arr, cJSON_CreateString(g_ad[i].url));
    char *txt = cJSON_Print(root);
    cJSON_Delete(root);
    if (!txt) return -1;
    FILE *f = fopen(evo_data_path(AD_CONF), "w");
    if (!f) { free(txt); return -1; }
    fputs(txt, f);
    fclose(f);
    free(txt);
    return 0;
}

static int ad_init(void)
{
    g_nad = 0;
    memset(g_ad, 0, sizeof g_ad);
    FILE *f = fopen(evo_data_path(AD_CONF), "r");
    if (!f) return 0;                    /* none added yet - not an error */
    static char buf[16384];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    cJSON *arr = root ? cJSON_GetObjectItem(root, "addons") : NULL;
    for (int i = 0; arr && i < cJSON_GetArraySize(arr) && g_nad < AD_MAX; ++i) {
        cJSON *u = cJSON_GetArrayItem(arr, i);
        if (!u || !cJSON_IsString(u) || !u->valuestring[0]) continue;
        snprintf(g_ad[g_nad].url, AD_URL, "%s", u->valuestring);
        ad_set_base(&g_ad[g_nad]);
        g_nad++;
    }
    if (root) cJSON_Delete(root);
    return 0;
}

static void ad_shutdown(void) { }
static int  ad_is_configured(void) { return g_nad > 0; }

/* The editor starts empty: the value typed is an addon to ADD, not a source
 * to replace (see ad_set_source). */
static const char *ad_get_source(void) { return ""; }

/*
 * Add an addon. Accepts the manifest URL, the addon's base URL, or the
 * stremio:// form addon sites link to. Empty clears the list. An address
 * already in the list is not added twice.
 */
static int ad_set_source(const char *value)
{
    char v[AD_URL];
    snprintf(v, sizeof v, "%s", value ? value : "");
    char *s = v;
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = '\0';

    if (!*s) {
        g_nad = 0;
        memset(g_ad, 0, sizeof g_ad);
        return ad_save();
    }
    char url[AD_URL];
    if (!strncmp(s, "stremio://", 10))      snprintf(url, sizeof url, "https://%s", s + 10);
    else if (!strncmp(s, "http://", 7) || !strncmp(s, "https://", 8)) snprintf(url, sizeof url, "%s", s);
    else return -1;
    n = strlen(url);
    if (n < 14 || strcmp(url + n - 14, "/manifest.json")) {
        while (n > 0 && url[n - 1] == '/') url[--n] = '\0';
        strncat(url, "/manifest.json", sizeof url - strlen(url) - 1);
    }
    for (int i = 0; i < g_nad; ++i)
        if (!strcmp(g_ad[i].url, url)) return 0;
    if (g_nad >= AD_MAX) return -1;
    memset(&g_ad[g_nad], 0, sizeof g_ad[g_nad]);
    snprintf(g_ad[g_nad].url, AD_URL, "%s", url);
    ad_set_base(&g_ad[g_nad]);
    g_nad++;
    return ad_save();
}

/* ------------------------------------------------------------------------- */
/* Manifests                                                                 */
/* ------------------------------------------------------------------------- */

static void list_append(char *list, size_t cap, cJSON *arr)
{
    for (int i = 0; arr && i < cJSON_GetArraySize(arr); ++i) {
        cJSON *t = cJSON_GetArrayItem(arr, i);
        if (!t || !cJSON_IsString(t)) continue;
        if (!list[0]) snprintf(list, cap, ",");
        strncat(list, t->valuestring, cap - strlen(list) - 1);
        strncat(list, ",", cap - strlen(list) - 1);
    }
}

static int list_has(const char *list, const char *v)
{
    if (!list[0]) return 1;
    char key[128];
    snprintf(key, sizeof key, ",%s,", v);
    return strstr(list, key) != NULL;
}

static int prefix_ok(const addon_t *a, const char *id)
{
    if (!a->prefixes[0]) return 1;
    const char *p = a->prefixes + 1;
    while (*p) {
        const char *e = strchr(p, ',');
        if (!e) break;
        if (e > p && !strncmp(id, p, (size_t)(e - p))) return 1;
        p = e + 1;
    }
    return 0;
}

static void parse_manifest(addon_t *a, cJSON *m)
{
    cJSON *name = cJSON_GetObjectItem(m, "name");
    snprintf(a->name, sizeof a->name, "%s",
             (name && cJSON_IsString(name)) ? name->valuestring : "Addon");
    cJSON *types = cJSON_GetObjectItem(m, "types");
    a->prefixes[0] = '\0';
    list_append(a->prefixes, sizeof a->prefixes, cJSON_GetObjectItem(m, "idPrefixes"));

    cJSON *res = cJSON_GetObjectItem(m, "resources");
    for (int i = 0; res && i < cJSON_GetArraySize(res); ++i) {
        cJSON *r = cJSON_GetArrayItem(res, i);
        const char *rn = NULL;
        cJSON *rtypes = types;
        if (cJSON_IsString(r)) {
            rn = r->valuestring;
        } else if (cJSON_IsObject(r)) {
            cJSON *n = cJSON_GetObjectItem(r, "name");
            rn = (n && cJSON_IsString(n)) ? n->valuestring : NULL;
            if (cJSON_GetObjectItem(r, "types")) rtypes = cJSON_GetObjectItem(r, "types");
            list_append(a->prefixes, sizeof a->prefixes, cJSON_GetObjectItem(r, "idPrefixes"));
        }
        if (!rn) continue;
        if (!strcmp(rn, "meta")) {
            a->has_meta = 1;
            list_append(a->meta_types, sizeof a->meta_types, rtypes);
        } else if (!strcmp(rn, "stream")) {
            a->has_stream = 1;
            list_append(a->stream_types, sizeof a->stream_types, rtypes);
        }
    }

    a->ncats = 0;
    cJSON *cats = cJSON_GetObjectItem(m, "catalogs");
    for (int i = 0; cats && i < cJSON_GetArraySize(cats) && a->ncats < AD_MAX_CATS; ++i) {
        cJSON *c = cJSON_GetArrayItem(cats, i);
        cJSON *t = cJSON_GetObjectItem(c, "type"), *id = cJSON_GetObjectItem(c, "id");
        cJSON *nm = cJSON_GetObjectItem(c, "name");
        if (!t || !cJSON_IsString(t) || !id || !cJSON_IsString(id)) continue;
        ad_cat_t *k = &a->cats[a->ncats++];
        memset(k, 0, sizeof *k);
        snprintf(k->type, sizeof k->type, "%s", t->valuestring);
        snprintf(k->id, sizeof k->id, "%s", id->valuestring);
        snprintf(k->name, sizeof k->name, "%s",
                 (nm && cJSON_IsString(nm)) ? nm->valuestring : id->valuestring);
        /* extra: [{name, isRequired}] (current) or extraRequired / extraSupported (old). */
        cJSON *ex = cJSON_GetObjectItem(c, "extra");
        for (int j = 0; ex && j < cJSON_GetArraySize(ex); ++j) {
            cJSON *e = cJSON_GetArrayItem(ex, j);
            cJSON *en = cJSON_GetObjectItem(e, "name");
            int req = cJSON_IsTrue(cJSON_GetObjectItem(e, "isRequired"));
            if (en && cJSON_IsString(en) && !strcmp(en->valuestring, "search")) k->searchable = 1;
            if (req) k->needs_extra = 1;
        }
        cJSON *er = cJSON_GetObjectItem(c, "extraRequired");
        if (er && cJSON_GetArraySize(er) > 0) k->needs_extra = 1;
        cJSON *es = cJSON_GetObjectItem(c, "extraSupported");
        for (int j = 0; es && j < cJSON_GetArraySize(es); ++j) {
            cJSON *e = cJSON_GetArrayItem(es, j);
            if (e && cJSON_IsString(e) && !strcmp(e->valuestring, "search")) k->searchable = 1;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Rows                                                                      */
/* ------------------------------------------------------------------------- */

static const char *type_label(const char *t)
{
    return !strcmp(t, "movie") ? "Movies" : !strcmp(t, "series") ? "Series" :
           !strcmp(t, "tv") ? "TV" : !strcmp(t, "channel") ? "Channels" : t;
}

static const char *jstr(cJSON *o, const char *k)
{
    cJSON *v = o ? cJSON_GetObjectItem(o, k) : NULL;
    return (v && cJSON_IsString(v)) ? v->valuestring : "";
}

static void emit_root(evo_provider_items_cb cb, void *ud)
{
    int max = 1;
    for (int i = 0; i < g_nad; ++i) max += g_ad[i].ncats + 1;
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)max, sizeof *items);
    if (!items) { if (cb) cb(0, NULL, 0, 0, ud); return; }
    int k = 0;
    for (int i = 0; i < g_nad; ++i) {
        addon_t *a = &g_ad[i];
        if (a->loaded < 0) {
            evo_provider_item_clear(&items[k]);
            snprintf(items[k].id, sizeof items[k].id, "x:%d", i);
            snprintf(items[k].title, sizeof items[k].title, "%s", a->base);
            snprintf(items[k].subtitle, sizeof items[k].subtitle, "Could not load this addon");
            items[k].is_folder = 1;
            k++;
            continue;
        }
        for (int c = 0; c < a->ncats; ++c) {
            ad_cat_t *ct = &a->cats[c];
            if (ct->needs_extra) continue;
            evo_provider_item_clear(&items[k]);
            snprintf(items[k].id, sizeof items[k].id, "c:%d:%s:%s", i, ct->type, ct->id);
            snprintf(items[k].title, sizeof items[k].title, "%s", ct->name);
            snprintf(items[k].subtitle, sizeof items[k].subtitle, "%s · %s", a->name, type_label(ct->type));
            items[k].is_folder = 1;
            items[k].kind = EVO_MEDIA_FOLDER;
            k++;
        }
    }
    if (cb) cb(1, items, k, 0, ud);
    free(items);
}

/* A meta from a catalog or search answer -> a row. */
static void map_meta(int a, cJSON *m, evo_provider_item_t *it)
{
    evo_provider_item_clear(it);
    const char *type = jstr(m, "type");
    const char *id = jstr(m, "id");
    int series = !strcmp(type, "series") || !strcmp(type, "anime");
    snprintf(it->id, sizeof it->id, "%s:%d:%s:%s", series ? "s" : "m", a, type, id);
    snprintf(it->title, sizeof it->title, "%s", jstr(m, "name"));
    const char *year = jstr(m, "releaseInfo");
    const char *rating = jstr(m, "imdbRating");
    if (year[0] && rating[0]) snprintf(it->subtitle, sizeof it->subtitle, "%s · %s", year, rating);
    else snprintf(it->subtitle, sizeof it->subtitle, "%s", year[0] ? year : type_label(type));
    snprintf(it->overview, sizeof it->overview, "%s", jstr(m, "description"));
    const char *poster = jstr(m, "poster");
    if (!strncmp(poster, "http", 4)) snprintf(it->art_url, sizeof it->art_url, "%s", poster);
    it->is_folder = series;
    it->kind = series ? EVO_MEDIA_FOLDER : EVO_MEDIA_VIDEO;
}

/* ------------------------------------------------------------------------- */
/* Fan-out requests (manifests, search, streams)                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    int  remaining;
    evo_provider_items_cb cb;
    void *ud;
    /* search */
    evo_provider_item_t *items;
    int  nitems, cap;
    /* streams */
    evo_provider_resolve_cb rcb;
    evo_stream_choice_t choices[AD_CHOICES];
    int  nchoices;
} fan_t;

typedef struct { fan_t *fan; int addon; } fan_req_t;

static void fan_done_root(fan_t *f)
{
    if (--f->remaining > 0) return;
    emit_root(f->cb, f->ud);
    free(f);
}

static void on_manifest(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    fan_req_t *r = (fan_req_t *)ud;
    addon_t *a = &g_ad[r->addon];
    cJSON *m = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    if (m && cJSON_IsObject(m)) {
        parse_manifest(a, m);
        a->loaded = 1;
    } else {
        a->loaded = -1;
        PROV_LOG("addons: manifest %s failed ok=%d http=%d", a->url, ok, status);
    }
    if (m) cJSON_Delete(m);
    fan_done_root(r->fan);
    free(r);
}

static int ad_get(const char *url, evo_net_cb cb, void *ud)
{
    const char *h[1] = { "Accept: application/json" };
    return evo_net_request_async("GET", url, NULL, h, 1, cb, ud);
}

static int list_root(evo_provider_items_cb cb, void *ud)
{
    fan_t *f = (fan_t *)calloc(1, sizeof *f);
    if (!f) return -2;
    f->cb = cb; f->ud = ud;
    f->remaining = 1;                    /* held until every request is out */
    for (int i = 0; i < g_nad; ++i) {
        if (g_ad[i].loaded != 0) continue;
        fan_req_t *r = (fan_req_t *)calloc(1, sizeof *r);
        if (!r) continue;
        r->fan = f; r->addon = i;
        f->remaining++;
        if (ad_get(g_ad[i].url, on_manifest, r) != 0) {
            g_ad[i].loaded = -1;
            f->remaining--;
            free(r);
        }
    }
    fan_done_root(f);                    /* drops the hold; emits if nothing went out */
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Catalog pages                                                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    int a;
    int skip;
    evo_provider_items_cb cb;
    void *ud;
} cat_ctx_t;

/* How many rows the open catalog already has, for Stremio's skip= paging. */
static char g_cat_key[256];
static int  g_cat_loaded;

static void on_catalog(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    cat_ctx_t *x = (cat_ctx_t *)ud;
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    cJSON *arr = root ? cJSON_GetObjectItem(root, "metas") : NULL;
    if (!arr || !cJSON_IsArray(arr)) {
        /* A page past the end is often a 404: that is "no more", not an error. */
        if (x->cb) x->cb(x->skip > 0 ? 1 : 0, NULL, 0, 0, x->ud);
        if (root) cJSON_Delete(root);
        free(x);
        return;
    }
    int n = cJSON_GetArraySize(arr);
    if (n > EVO_PROVIDER_PAGE_MAX) n = EVO_PROVIDER_PAGE_MAX;
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)n + 1, sizeof *items);
    int k = 0;
    for (int i = 0; items && i < n; ++i) {
        cJSON *m = cJSON_GetArrayItem(arr, i);
        if (!m || !jstr(m, "id")[0]) continue;
        map_meta(x->a, m, &items[k++]);
    }
    g_cat_loaded = x->skip + n;
    /* Addons page in fixed sizes they do not announce (Cinemeta: 100); a page
     * of 20+ may have a next one, and an empty next page ends it. */
    if (x->cb) x->cb(1, items, k, n >= 20, x->ud);
    free(items);
    cJSON_Delete(root);
    free(x);
}

static int list_catalog_page(int a, const char *type, const char *cat, const char *key,
                             int page, evo_provider_items_cb cb, void *ud)
{
    if (page == 0 || strcmp(g_cat_key, key)) {
        snprintf(g_cat_key, sizeof g_cat_key, "%s", key);
        g_cat_loaded = 0;
    }
    char url[EVO_PROVIDER_MAX_URL];
    if (page > 0 && g_cat_loaded > 0)
        snprintf(url, sizeof url, "%s/catalog/%s/%s/skip=%d.json", g_ad[a].base, type, cat, g_cat_loaded);
    else
        snprintf(url, sizeof url, "%s/catalog/%s/%s.json", g_ad[a].base, type, cat);
    cat_ctx_t *x = (cat_ctx_t *)calloc(1, sizeof *x);
    if (!x) return -2;
    x->a = a; x->skip = page > 0 ? g_cat_loaded : 0; x->cb = cb; x->ud = ud;
    int rc = ad_get(url, on_catalog, x);
    if (rc != 0) free(x);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Series: meta -> seasons -> episodes                                       */
/* ------------------------------------------------------------------------- */

/* The last series meta fetched: the seasons level and every episodes level
 * under it read the same document. */
static char   g_meta_key[256];
static cJSON *g_meta;

static int find_meta_addon(int pref, const char *type, const char *id)
{
    if (pref >= 0 && pref < g_nad && g_ad[pref].has_meta &&
        list_has(g_ad[pref].meta_types, type) && prefix_ok(&g_ad[pref], id))
        return pref;
    for (int i = 0; i < g_nad; ++i)
        if (g_ad[i].loaded == 1 && g_ad[i].has_meta && list_has(g_ad[i].meta_types, type) &&
            prefix_ok(&g_ad[i], id))
            return i;
    return -1;
}

static void emit_seasons(int a, const char *type, const char *id,
                         evo_provider_items_cb cb, void *ud)
{
    cJSON *meta = cJSON_GetObjectItem(g_meta, "meta");
    cJSON *videos = meta ? cJSON_GetObjectItem(meta, "videos") : NULL;
    int seasons[128], counts[128], ns = 0;
    for (int i = 0; videos && i < cJSON_GetArraySize(videos); ++i) {
        cJSON *v = cJSON_GetArrayItem(videos, i);
        cJSON *s = cJSON_GetObjectItem(v, "season");
        int sn = (s && cJSON_IsNumber(s)) ? s->valueint : 1;
        int j = 0;
        while (j < ns && seasons[j] != sn) j++;
        if (j == ns && ns < 128) { seasons[ns] = sn; counts[ns] = 0; ns++; }
        if (j < 128) counts[j]++;
    }
    /* Ascending, specials (season 0) last. */
    for (int i = 0; i < ns; ++i)
        for (int j = i + 1; j < ns; ++j) {
            int ki = seasons[i] == 0 ? 1 << 20 : seasons[i];
            int kj = seasons[j] == 0 ? 1 << 20 : seasons[j];
            if (kj < ki) {
                int t = seasons[i]; seasons[i] = seasons[j]; seasons[j] = t;
                t = counts[i]; counts[i] = counts[j]; counts[j] = t;
            }
        }
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)ns + 1, sizeof *items);
    if (!items) { if (cb) cb(0, NULL, 0, 0, ud); return; }
    const char *poster = meta ? jstr(meta, "poster") : "";
    for (int i = 0; i < ns; ++i) {
        evo_provider_item_t *it = &items[i];
        evo_provider_item_clear(it);
        snprintf(it->id, sizeof it->id, "ss:%d:%s:%s:%d", a, type, id, seasons[i]);
        if (seasons[i] == 0) snprintf(it->title, sizeof it->title, "Specials");
        else snprintf(it->title, sizeof it->title, "Season %d", seasons[i]);
        snprintf(it->subtitle, sizeof it->subtitle, "%d episode%s", counts[i], counts[i] == 1 ? "" : "s");
        if (!strncmp(poster, "http", 4)) snprintf(it->art_url, sizeof it->art_url, "%s", poster);
        it->is_folder = 1;
        it->kind = EVO_MEDIA_FOLDER;
    }
    if (cb) cb(1, items, ns, 0, ud);
    free(items);
}

static void emit_episodes(int a, const char *type, int season,
                          evo_provider_items_cb cb, void *ud)
{
    cJSON *meta = cJSON_GetObjectItem(g_meta, "meta");
    cJSON *videos = meta ? cJSON_GetObjectItem(meta, "videos") : NULL;
    int n = videos ? cJSON_GetArraySize(videos) : 0;
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)n + 1, sizeof *items);
    if (!items) { if (cb) cb(0, NULL, 0, 0, ud); return; }
    const char *poster = jstr(meta, "poster");
    int k = 0;
    for (int i = 0; i < n; ++i) {
        cJSON *v = cJSON_GetArrayItem(videos, i);
        cJSON *s = cJSON_GetObjectItem(v, "season"), *e = cJSON_GetObjectItem(v, "episode");
        int sn = (s && cJSON_IsNumber(s)) ? s->valueint : 1;
        int en = (e && cJSON_IsNumber(e)) ? e->valueint : i + 1;
        if (sn != season || !jstr(v, "id")[0]) continue;
        evo_provider_item_t *it = &items[k++];
        evo_provider_item_clear(it);
        snprintf(it->id, sizeof it->id, "e:%d:%s:%s", a, type, jstr(v, "id"));
        const char *t = jstr(v, "title")[0] ? jstr(v, "title") : jstr(v, "name");
        if (t[0]) snprintf(it->title, sizeof it->title, "%s", t);
        else snprintf(it->title, sizeof it->title, "Episode %d", en);
        const char *rel = jstr(v, "released");
        if (rel[0]) snprintf(it->subtitle, sizeof it->subtitle, "Episode %d · %.10s", en, rel);
        else snprintf(it->subtitle, sizeof it->subtitle, "Episode %d", en);
        const char *ov = jstr(v, "overview")[0] ? jstr(v, "overview") : jstr(v, "description");
        snprintf(it->overview, sizeof it->overview, "%s", ov);
        /* The poster card wants the series poster, not a 16:9 still. */
        if (!strncmp(poster, "http", 4)) snprintf(it->art_url, sizeof it->art_url, "%s", poster);
        it->kind = EVO_MEDIA_VIDEO;
    }
    /* Episode order, whatever order the addon listed them in. */
    for (int i = 0; i < k; ++i)
        for (int j = i + 1; j < k; ++j) {
            int ei = atoi(items[i].subtitle + 8), ej = atoi(items[j].subtitle + 8);
            if (ej < ei) { evo_provider_item_t t = items[i]; items[i] = items[j]; items[j] = t; }
        }
    if (cb) cb(1, items, k, 0, ud);
    free(items);
}

typedef struct {
    int a;
    int season;                  /* -1: list seasons */
    char type[24];
    char id[160];
    evo_provider_items_cb cb;
    void *ud;
} meta_ctx_t;

static void on_meta(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    meta_ctx_t *x = (meta_ctx_t *)ud;
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    if (!root || !cJSON_GetObjectItem(root, "meta")) {
        PROV_LOG("addons: meta %s/%s failed ok=%d http=%d", x->type, x->id, ok, status);
        if (x->cb) x->cb(0, NULL, 0, 0, x->ud);
        if (root) cJSON_Delete(root);
        free(x);
        return;
    }
    if (g_meta) cJSON_Delete(g_meta);
    g_meta = root;
    snprintf(g_meta_key, sizeof g_meta_key, "%s:%s", x->type, x->id);
    if (x->season < 0) emit_seasons(x->a, x->type, x->id, x->cb, x->ud);
    else               emit_episodes(x->a, x->type, x->season, x->cb, x->ud);
    free(x);
}

static int list_series(int a, const char *type, const char *id, int season,
                       evo_provider_items_cb cb, void *ud)
{
    char key[256];
    snprintf(key, sizeof key, "%s:%s", type, id);
    if (g_meta && !strcmp(g_meta_key, key)) {
        if (season < 0) emit_seasons(a, type, id, cb, ud);
        else            emit_episodes(a, type, season, cb, ud);
        return 0;
    }
    int m = find_meta_addon(a, type, id);
    if (m < 0) return -1;
    meta_ctx_t *x = (meta_ctx_t *)calloc(1, sizeof *x);
    if (!x) return -2;
    x->a = a; x->season = season; x->cb = cb; x->ud = ud;
    snprintf(x->type, sizeof x->type, "%s", type);
    snprintf(x->id, sizeof x->id, "%s", id);
    char url[EVO_PROVIDER_MAX_URL];
    snprintf(url, sizeof url, "%s/meta/%s/%s.json", g_ad[m].base, type, id);
    int rc = ad_get(url, on_meta, x);
    if (rc != 0) free(x);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* list_catalog                                                              */
/* ------------------------------------------------------------------------- */

/* Split "<a>:<type>:<rest>" (after the kind prefix). rest may contain ':'. */
static int split3(const char *s, int *a, char *type, size_t tcap, const char **rest)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != ':' || v < 0 || v >= g_nad) return -1;
    *a = (int)v;
    const char *t = end + 1, *c = strchr(t, ':');
    if (!c || (size_t)(c - t) >= tcap) return -1;
    snprintf(type, tcap, "%.*s", (int)(c - t), t);
    *rest = c + 1;
    return 0;
}

static int ad_list_catalog(const char *parent_id, int page,
                           evo_provider_items_cb cb, void *ud)
{
    if (!parent_id || !*parent_id) {
        if (page > 0) return -1;
        return list_root(cb, ud);
    }
    int a;
    char type[24];
    const char *rest;
    if (!strncmp(parent_id, "c:", 2)) {
        if (split3(parent_id + 2, &a, type, sizeof type, &rest) != 0) return -1;
        return list_catalog_page(a, type, rest, parent_id, page, cb, ud);
    }
    if (page > 0) return -1;             /* seasons and episodes come in one page */
    if (!strncmp(parent_id, "s:", 2)) {
        if (split3(parent_id + 2, &a, type, sizeof type, &rest) != 0) return -1;
        return list_series(a, type, rest, -1, cb, ud);
    }
    if (!strncmp(parent_id, "ss:", 3)) {
        if (split3(parent_id + 3, &a, type, sizeof type, &rest) != 0) return -1;
        const char *last = strrchr(rest, ':');
        if (!last) return -1;
        char id[160];
        snprintf(id, sizeof id, "%.*s", (int)(last - rest), rest);
        return list_series(a, type, id, atoi(last + 1), cb, ud);
    }
    if (!strncmp(parent_id, "x:", 2)) {
        if (cb) cb(1, NULL, 0, 0, ud);
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Search                                                                    */
/* ------------------------------------------------------------------------- */

static void fan_done_search(fan_t *f)
{
    if (--f->remaining > 0) return;
    if (f->cb) f->cb(1, f->items, f->nitems, 0, f->ud);
    free(f->items);
    free(f);
}

static void on_search(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    fan_req_t *r = (fan_req_t *)ud;
    fan_t *f = r->fan;
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    cJSON *arr = root ? cJSON_GetObjectItem(root, "metas") : NULL;
    for (int i = 0; arr && i < cJSON_GetArraySize(arr) && f->nitems < f->cap; ++i) {
        cJSON *m = cJSON_GetArrayItem(arr, i);
        if (!m || !jstr(m, "id")[0]) continue;
        evo_provider_item_t it;
        map_meta(r->addon, m, &it);
        int dup = 0;                      /* the same title from two catalogs */
        for (int j = 0; j < f->nitems && !dup; ++j)
            dup = !strcmp(strrchr(f->items[j].id, ':'), strrchr(it.id, ':')) &&
                  !strcmp(f->items[j].title, it.title);
        if (!dup) f->items[f->nitems++] = it;
    }
    if (root) cJSON_Delete(root);
    fan_done_search(f);
    free(r);
}

static int ad_search(const char *query, int page, evo_provider_items_cb cb, void *ud)
{
    if (!query || !*query || page > 0) return -1;
    char esc[256];
    if (evo_provider_url_escape(query, esc, sizeof esc) < 0) return -1;
    fan_t *f = (fan_t *)calloc(1, sizeof *f);
    if (!f) return -2;
    f->cb = cb; f->ud = ud;
    f->cap = EVO_PROVIDER_PAGE_MAX;
    f->items = (evo_provider_item_t *)calloc((size_t)f->cap, sizeof *f->items);
    if (!f->items) { free(f); return -2; }
    f->remaining = 1;
    for (int i = 0; i < g_nad; ++i) {
        for (int c = 0; g_ad[i].loaded == 1 && c < g_ad[i].ncats; ++c) {
            ad_cat_t *ct = &g_ad[i].cats[c];
            if (!ct->searchable) continue;
            char url[EVO_PROVIDER_MAX_URL];
            snprintf(url, sizeof url, "%s/catalog/%s/%s/search=%s.json",
                     g_ad[i].base, ct->type, ct->id, esc);
            fan_req_t *r = (fan_req_t *)calloc(1, sizeof *r);
            if (!r) continue;
            r->fan = f; r->addon = i;
            f->remaining++;
            if (ad_get(url, on_search, r) != 0) { f->remaining--; free(r); }
        }
    }
    fan_done_search(f);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Resolve: every addon that streams this type                               */
/* ------------------------------------------------------------------------- */

static void fan_done_streams(fan_t *f)
{
    if (--f->remaining > 0) return;
    if (f->rcb) f->rcb(f->nchoices > 0, f->choices, f->nchoices, f->ud);
    free(f);
}

static void first_line(const char *s, char *out, size_t cap)
{
    size_t n = strcspn(s, "\n");
    snprintf(out, cap, "%.*s", (int)n, s);
}

static void on_streams(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    fan_req_t *r = (fan_req_t *)ud;
    fan_t *f = r->fan;
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    cJSON *arr = root ? cJSON_GetObjectItem(root, "streams") : NULL;
    for (int i = 0; arr && i < cJSON_GetArraySize(arr) && f->nchoices < AD_CHOICES; ++i) {
        cJSON *s = cJSON_GetArrayItem(arr, i);
        const char *url = jstr(s, "url");
        if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) continue;
        evo_stream_choice_t *c = &f->choices[f->nchoices++];
        evo_provider_stream_choice_clear(c);
        snprintf(c->url, sizeof c->url, "%s", url);
        char name[48], title[64];
        first_line(jstr(s, "name")[0] ? jstr(s, "name") : g_ad[r->addon].name, name, sizeof name);
        first_line(jstr(s, "title")[0] ? jstr(s, "title") : jstr(s, "description"), title, sizeof title);
        if (title[0]) snprintf(c->label, sizeof c->label, "%s - %s", name, title);
        else          snprintf(c->label, sizeof c->label, "%s", name);
        if (strstr(url, ".m3u8")) snprintf(c->container, sizeof c->container, "hls");
    }
    if (!ok || status != 200)
        PROV_LOG("addons: streams from %s failed ok=%d http=%d", g_ad[r->addon].name, ok, status);
    if (root) cJSON_Delete(root);
    fan_done_streams(f);
    free(r);
}

static int ad_resolve(const char *item_id, evo_provider_resolve_cb cb, void *ud)
{
    if (!item_id || (strncmp(item_id, "m:", 2) && strncmp(item_id, "e:", 2))) return -1;
    int a;
    char type[24];
    const char *id;
    if (split3(item_id + 2, &a, type, sizeof type, &id) != 0) return -1;
    fan_t *f = (fan_t *)calloc(1, sizeof *f);
    if (!f) return -2;
    f->rcb = cb; f->ud = ud;
    f->remaining = 1;
    for (int i = 0; i < g_nad; ++i) {
        addon_t *ad = &g_ad[i];
        if (ad->loaded != 1 || !ad->has_stream || !list_has(ad->stream_types, type) || !prefix_ok(ad, id))
            continue;
        char url[EVO_PROVIDER_MAX_URL];
        snprintf(url, sizeof url, "%s/stream/%s/%s.json", ad->base, type, id);
        fan_req_t *r = (fan_req_t *)calloc(1, sizeof *r);
        if (!r) continue;
        r->fan = f; r->addon = i;
        f->remaining++;
        if (ad_get(url, on_streams, r) != 0) { f->remaining--; free(r); }
    }
    fan_done_streams(f);
    return 0;
}

const evo_provider_t evo_provider_stremio = {
    .id            = "addons",
    .name          = "Addons",
    .icon          = "icon_emby.png",
    .caps          = EVO_PROVIDER_CAP_CATALOG | EVO_PROVIDER_CAP_SEARCH |
                     EVO_PROVIDER_CAP_RESOLVE | EVO_PROVIDER_CAP_CONFIG |
                     EVO_PROVIDER_CAP_PICK,
    .api_version   = EVO_PROVIDER_API_VERSION,
    .init          = ad_init,
    .shutdown      = ad_shutdown,
    .is_configured = ad_is_configured,
    .list_catalog  = ad_list_catalog,
    .search        = ad_search,
    .resolve       = ad_resolve,
    .get_source    = ad_get_source,
    .set_source    = ad_set_source,
    .ui_embedded   = "mediaserver",
};
