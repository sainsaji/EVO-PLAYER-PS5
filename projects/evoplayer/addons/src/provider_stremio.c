/*
 * provider_stremio.c — Stremio addons, browsed natively
 * (docs/providers/media-servers.md, Phase B).
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
#include "provider_stremio.h"
#include "evo_net.h"
#include "cJSON.h"
#include "evo_data_path.h"
#include "evo_provider_log.h"
#include "evo_error.h"

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
    int  builtin;                /* EVO's own Cinemeta, see builtin_sync() */
} addon_t;

/*
 * One addon engine, two providers: "addons" (EVO's own list, addons.json) and
 * "nuvio" (the list the Nuvio web app keeps in its page storage). S is the
 * instance in use: every vtable entry sets it, and every request carries its
 * instance so its callback can set it again.
 */
typedef struct ad_set {
    const char *tag;             /* log prefix */
    int  avio;                   /* requests on FFmpeg's HTTP client */
    addon_t ad[AD_MAX];
    int  nad;
    /* How many rows the open catalog already has, for Stremio's skip= paging. */
    char cat_key[256];
    int  cat_loaded;
    /* The last series meta fetched: the seasons level and every episodes
     * level under it read the same document. */
    char meta_key[256];
    cJSON *meta;
} ad_set_t;

static ad_set_t g_addons = { .tag = "addons", .avio = 0 };
static ad_set_t g_nuvio  = { .tag = "nuvio",  .avio = 1 };
static ad_set_t *S = &g_addons;

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

/*
 * Cinemeta, built in, as the Stremio app has it. Most addons people add are
 * either stream-only (Torrentio: no catalogs at all, so nothing to browse) or
 * catalog-only (Streaming Catalogs: lists of IMDb ids with no meta, so a series
 * has no seasons to open). Stremio fills both gaps with Cinemeta, and so does
 * EVO: it rides at the end of the list, is never saved or offered for removal,
 * lends its catalogs only when no added addon has any, and is always there for
 * series details and search. Skipped when the user added Cinemeta themselves.
 */
#define CINEMETA_BASE "https://v3-cinemeta.strem.io"

static int user_count(void)
{
    return (S->nad > 0 && S->ad[S->nad - 1].builtin) ? S->nad - 1 : S->nad;
}

static void builtin_drop(void)
{
    if (S->nad > 0 && S->ad[S->nad - 1].builtin) memset(&S->ad[--S->nad], 0, sizeof S->ad[0]);
}

static void builtin_sync(void)
{
    int want = user_count() > 0;
    for (int i = 0; want && i < user_count(); ++i)
        if (!strcmp(S->ad[i].base, CINEMETA_BASE)) want = 0;
    const int have = S->nad > 0 && S->ad[S->nad - 1].builtin;
    if (!want) { builtin_drop(); return; }
    if (have || S->nad >= AD_MAX) return;
    addon_t *a = &S->ad[S->nad++];
    memset(a, 0, sizeof *a);
    snprintf(a->url, sizeof a->url, "%s/manifest.json", CINEMETA_BASE);
    ad_set_base(a);
    a->builtin = 1;
}

static int ad_save(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "addons", arr);
    for (int i = 0; i < user_count(); ++i) cJSON_AddItemToArray(arr, cJSON_CreateString(S->ad[i].url));
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
    S = &g_addons;
    S->nad = 0;
    memset(S->ad, 0, sizeof S->ad);
    FILE *f = fopen(evo_data_path(AD_CONF), "r");
    if (!f) return 0;                    /* none added yet - not an error */
    static char buf[16384];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    cJSON *arr = root ? cJSON_GetObjectItem(root, "addons") : NULL;
    for (int i = 0; arr && i < cJSON_GetArraySize(arr) && S->nad < AD_MAX; ++i) {
        cJSON *u = cJSON_GetArrayItem(arr, i);
        if (!u || !cJSON_IsString(u) || !u->valuestring[0]) continue;
        snprintf(S->ad[S->nad].url, AD_URL, "%s", u->valuestring);
        ad_set_base(&S->ad[S->nad]);
        S->nad++;
    }
    if (root) cJSON_Delete(root);
    builtin_sync();
    return 0;
}

static void ad_shutdown(void) { }
static int  ad_is_configured(void) { S = &g_addons; return user_count() > 0; }

/* The editor starts empty: the value typed is an addon to ADD, not a source
 * to replace (see ad_set_source). */
static const char *ad_get_source(void) { return ""; }

/*
 * What the user typed -> a manifest URL. Accepts the manifest URL, the addon's
 * base URL, or the stremio:// form addon sites link to. 0 ok, -1 not an address.
 */
static int ad_parse_address(const char *s, char *url, size_t cap)
{
    if (!strncmp(s, "stremio://", 10))      snprintf(url, cap, "https://%s", s + 10);
    else if (!strncmp(s, "http://", 7) || !strncmp(s, "https://", 8)) snprintf(url, cap, "%s", s);
    else if (strchr(s, '.') && !strchr(s, ' ')) {
        /* Typed without a scheme: a home-network address is plain http (a
         * server on the LAN has no certificate), anything else is https. */
        int a = 0, b = 0;
        int lan = !strncmp(s, "10.", 3) || !strncmp(s, "127.", 4) ||
                  !strncmp(s, "192.168.", 8) ||
                  (sscanf(s, "172.%d.%d", &a, &b) == 2 && a >= 16 && a <= 31);
        snprintf(url, cap, "%s://%s", lan ? "http" : "https", s);
    }
    else return -1;
    size_t n = strlen(url);
    if (n < 14 || strcmp(url + n - 14, "/manifest.json")) {
        while (n > 0 && url[n - 1] == '/') url[--n] = '\0';
        strncat(url, "/manifest.json", cap - strlen(url) - 1);
    }
    return 0;
}

static char *ad_trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = '\0';
    return s;
}

/*
 * Add an addon. Empty clears the list. An address already in the list is not
 * added twice.
 */
static int ad_set_source(const char *value)
{
    char v[AD_URL];
    snprintf(v, sizeof v, "%s", value ? value : "");
    char *s = ad_trim(v);

    S = &g_addons;
    if (!*s) {
        S->nad = 0;
        memset(S->ad, 0, sizeof S->ad);
        return ad_save();
    }
    char url[AD_URL];
    if (ad_parse_address(s, url, sizeof url) != 0) return -1;
    for (int i = 0; i < user_count(); ++i)
        if (!strcmp(S->ad[i].url, url)) return 0;
    builtin_drop();                      /* added ones go before it */
    if (S->nad >= AD_MAX) { builtin_sync(); return -1; }
    memset(&S->ad[S->nad], 0, sizeof S->ad[S->nad]);
    snprintf(S->ad[S->nad].url, AD_URL, "%s", url);
    ad_set_base(&S->ad[S->nad]);
    S->nad++;
    builtin_sync();
    return ad_save();
}

int evo_stremio_addon_count(void) { S = &g_addons; return user_count(); }

const char *evo_stremio_addon_url(int i)
{
    S = &g_addons;
    return (i >= 0 && i < user_count()) ? S->ad[i].url : "";
}

static const char *addon_label(int i)
{
    if (i < 0 || i >= user_count()) return "";
    if (S->ad[i].name[0]) return S->ad[i].name;
    static char label[AD_URL];
    const char *u = S->ad[i].url;
    const char *scheme = strstr(u, "://");
    snprintf(label, sizeof label, "%s", scheme ? scheme + 3 : u);
    size_t n = strlen(label), t = strlen("/manifest.json");
    if (n >= t && !strcmp(label + n - t, "/manifest.json")) label[n - t] = '\0';
    return label;
}

const char *evo_stremio_addon_name(int i)
{
    S = &g_addons;
    return addon_label(i);
}

int evo_stremio_addon_remove(int i)
{
    S = &g_addons;
    if (i < 0 || i >= user_count()) return -1;
    for (int k = i; k + 1 < S->nad; ++k) S->ad[k] = S->ad[k + 1];
    memset(&S->ad[--S->nad], 0, sizeof S->ad[0]);
    builtin_sync();
    return ad_save();
}

/* ------------------------------------------------------------------------- */
/* Manifests                                                              */
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
    for (int i = 0; i < S->nad; ++i) max += S->ad[i].ncats + 1;
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)max, sizeof *items);
    if (!items) { if (cb) cb(0, NULL, 0, 0, ud); return; }
    int k = 0;
    /* The built-in Cinemeta lends its catalogs only when nothing added has any. */
    int others = 0;
    for (int i = 0; i < S->nad; ++i)
        for (int c = 0; !S->ad[i].builtin && S->ad[i].loaded == 1 && c < S->ad[i].ncats; ++c)
            if (!S->ad[i].cats[c].needs_extra) others = 1;
    for (int i = 0; i < S->nad; ++i) {
        addon_t *a = &S->ad[i];
        if (a->builtin && (others || a->loaded < 0)) continue;
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
static void join_field(cJSON *v, char *out, size_t cap, int limit);

static void map_meta(int a, cJSON *m, evo_provider_item_t *it)
{
    evo_provider_item_clear(it);
    const char *type = jstr(m, "type");
    const char *id = jstr(m, "id");
    /* A catalog keyed on its own ids (tmdb:123) that also gives the IMDb id:
     * use that, which is what stream addons such as Torrentio look up. */
    if (strncmp(id, "tt", 2) && !strncmp(jstr(m, "imdb_id"), "tt", 2)) id = jstr(m, "imdb_id");
    int series = !strcmp(type, "series") || !strcmp(type, "anime");
    snprintf(it->id, sizeof it->id, "%s:%d:%s:%s", series ? "s" : "m", a, type, id);
    snprintf(it->title, sizeof it->title, "%s", jstr(m, "name"));
    const char *year = jstr(m, "releaseInfo");
    const char *rating = jstr(m, "imdbRating");
    if (year[0] && rating[0]) snprintf(it->subtitle, sizeof it->subtitle, "%s · %s", year, rating);
    else snprintf(it->subtitle, sizeof it->subtitle, "%s", year[0] ? year : type_label(type));
    snprintf(it->overview, sizeof it->overview, "%s", jstr(m, "description"));
    snprintf(it->rating, sizeof it->rating, "%s", rating);
    {
        char genres[64];
        join_field(cJSON_GetObjectItem(m, "genres"), genres, sizeof genres, 3);
        size_t b = 0;
        it->facts[0] = '\0';
        if (year[0]) b += (size_t)snprintf(it->facts + b, sizeof it->facts - b, "%s", year);
        if (jstr(m, "runtime")[0])
            b += (size_t)snprintf(it->facts + b, sizeof it->facts - b, "%s%s", b ? "  \xC2\xB7  " : "", jstr(m, "runtime"));
        if (genres[0])
            snprintf(it->facts + b, sizeof it->facts - b, "%s%s", b ? "  \xC2\xB7  " : "", genres);
    }
    const char *poster = jstr(m, "poster");
    if (!strncmp(poster, "http", 4)) snprintf(it->art_url, sizeof it->art_url, "%s", poster);
    if (!strncmp(jstr(m, "background"), "http", 4)) {
        snprintf(it->backdrop_url, sizeof it->backdrop_url, "%s", jstr(m, "background"));
        if (it->art_url[0]) snprintf(it->art_alt_url, sizeof it->art_alt_url, "%s", jstr(m, "background"));
    }
    if (!strncmp(jstr(m, "logo"), "http", 4))
        snprintf(it->logo_url, sizeof it->logo_url, "%s", jstr(m, "logo"));
    it->is_folder = series;
    it->kind = series ? EVO_MEDIA_FOLDER : EVO_MEDIA_VIDEO;
    it->detail = series ? 2 : 1;
}

/* ------------------------------------------------------------------------- */
/* Fan-out requests (manifests, search, streams)                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    ad_set_t *set;
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
    int  asked;                  /* stream requests sent */
    int  torrents;               /* infoHash-only streams skipped */
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
    S = r->fan->set;
    addon_t *a = &S->ad[r->addon];
    cJSON *m = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    if (m && cJSON_IsObject(m)) {
        parse_manifest(a, m);
        a->loaded = 1;
    } else {
        a->loaded = -1;
        PROV_LOG("%s: manifest %s failed ok=%d http=%d", S->tag, a->url, ok, status);
    }
    if (m) cJSON_Delete(m);
    fan_done_root(r->fan);
    free(r);
}

static int ad_get(const char *url, evo_net_cb cb, void *ud)
{
    const char *h[1] = { "Accept: application/json" };
    if (S->avio)    /* FFmpeg's HTTP client, as the Jellyfin requests use */
        return evo_net_request_async_avio("GET", url, NULL, h, 1, cb, ud);
    return evo_net_request_async("GET", url, NULL, h, 1, cb, ud);
}

static void nv_load_addons(void);

static int list_root(evo_provider_items_cb cb, void *ud)
{
    if (S == &g_nuvio) nv_load_addons();     /* changed in the web version since? */
    fan_t *f = (fan_t *)calloc(1, sizeof *f);
    if (!f) return -2;
    f->set = S;
    f->cb = cb; f->ud = ud;
    f->remaining = 1;                    /* held until every request is out */
    for (int i = 0; i < S->nad; ++i) {
        if (S->ad[i].loaded != 0) continue;
        fan_req_t *r = (fan_req_t *)calloc(1, sizeof *r);
        if (!r) continue;
        r->fan = f; r->addon = i;
        f->remaining++;
        if (ad_get(S->ad[i].url, on_manifest, r) != 0) {
            S->ad[i].loaded = -1;
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
    ad_set_t *set;
    int a;
    int skip;
    evo_provider_items_cb cb;
    void *ud;
} cat_ctx_t;


static void on_catalog(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    cat_ctx_t *x = (cat_ctx_t *)ud;
    S = x->set;
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
    S->cat_loaded = x->skip + n;
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
    if (page == 0 || strcmp(S->cat_key, key)) {
        snprintf(S->cat_key, sizeof S->cat_key, "%s", key);
        S->cat_loaded = 0;
    }
    char url[EVO_PROVIDER_MAX_URL];
    if (page > 0 && S->cat_loaded > 0)
        snprintf(url, sizeof url, "%s/catalog/%s/%s/skip=%d.json", S->ad[a].base, type, cat, S->cat_loaded);
    else
        snprintf(url, sizeof url, "%s/catalog/%s/%s.json", S->ad[a].base, type, cat);
    cat_ctx_t *x = (cat_ctx_t *)calloc(1, sizeof *x);
    if (!x) return -2;
    x->set = S;
    x->a = a; x->skip = page > 0 ? S->cat_loaded : 0; x->cb = cb; x->ud = ud;
    int rc = ad_get(url, on_catalog, x);
    if (rc != 0) free(x);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Series: meta -> seasons -> episodes                                       */
/* ------------------------------------------------------------------------- */


static int find_meta_addon(int pref, const char *type, const char *id)
{
    if (pref >= 0 && pref < S->nad && S->ad[pref].has_meta &&
        list_has(S->ad[pref].meta_types, type) && prefix_ok(&S->ad[pref], id))
        return pref;
    for (int i = 0; i < S->nad; ++i)
        if (S->ad[i].loaded == 1 && S->ad[i].has_meta && list_has(S->ad[i].meta_types, type) &&
            prefix_ok(&S->ad[i], id))
            return i;
    return -1;
}

/* Art for a season or episode: the series poster, else its backdrop, else `own`
 * (an episode's still). The backdrop also backs up a poster that 404s. */
static void level_art(evo_provider_item_t *it, cJSON *meta, const char *own)
{
    const char *poster = meta ? jstr(meta, "poster") : "";
    const char *bg = meta ? jstr(meta, "background") : "";
    int hp = !strncmp(poster, "http", 4), hb = !strncmp(bg, "http", 4);
    int ho = own && !strncmp(own, "http", 4);
    if (hp)      snprintf(it->art_url, sizeof it->art_url, "%s", poster);
    else if (hb) snprintf(it->art_url, sizeof it->art_url, "%s", bg);
    else if (ho) snprintf(it->art_url, sizeof it->art_url, "%s", own);
    if (hp && hb)      snprintf(it->art_alt_url, sizeof it->art_alt_url, "%s", bg);
    else if (it->art_url[0] && ho && strcmp(own, it->art_url))
                       snprintf(it->art_alt_url, sizeof it->art_alt_url, "%s", own);
    if (hb) snprintf(it->backdrop_url, sizeof it->backdrop_url, "%s", bg);
}

static void emit_seasons(int a, const char *type, const char *id,
                         evo_provider_items_cb cb, void *ud)
{
    cJSON *meta = cJSON_GetObjectItem(S->meta, "meta");
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
    for (int i = 0; i < ns; ++i) {
        evo_provider_item_t *it = &items[i];
        evo_provider_item_clear(it);
        snprintf(it->id, sizeof it->id, "ss:%d:%s:%s:%d", a, type, id, seasons[i]);
        if (seasons[i] == 0) snprintf(it->title, sizeof it->title, "Specials");
        else snprintf(it->title, sizeof it->title, "Season %d", seasons[i]);
        snprintf(it->subtitle, sizeof it->subtitle, "%d episode%s", counts[i], counts[i] == 1 ? "" : "s");
        level_art(it, meta, NULL);
        it->is_folder = 1;
        it->kind = EVO_MEDIA_FOLDER;
    }
    if (cb) cb(1, items, ns, 0, ud);
    free(items);
}

static void emit_episodes(int a, const char *type, int season,
                          evo_provider_items_cb cb, void *ud)
{
    cJSON *meta = cJSON_GetObjectItem(S->meta, "meta");
    cJSON *videos = meta ? cJSON_GetObjectItem(meta, "videos") : NULL;
    int n = videos ? cJSON_GetArraySize(videos) : 0;
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)n + 1, sizeof *items);
    if (!items) { if (cb) cb(0, NULL, 0, 0, ud); return; }
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
        level_art(it, meta, jstr(v, "thumbnail"));
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
    ad_set_t *set;
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
    S = x->set;
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    if (!root || !cJSON_GetObjectItem(root, "meta")) {
        PROV_LOG("%s: meta %s/%s failed ok=%d http=%d", S->tag, x->type, x->id, ok, status);
        if (x->cb) x->cb(0, NULL, 0, 0, x->ud);
        if (root) cJSON_Delete(root);
        free(x);
        return;
    }
    if (S->meta) cJSON_Delete(S->meta);
    S->meta = root;
    snprintf(S->meta_key, sizeof S->meta_key, "%s:%s", x->type, x->id);
    if (x->season < 0) emit_seasons(x->a, x->type, x->id, x->cb, x->ud);
    else               emit_episodes(x->a, x->type, x->season, x->cb, x->ud);
    free(x);
}

static int list_series(int a, const char *type, const char *id, int season,
                       evo_provider_items_cb cb, void *ud)
{
    char key[256];
    snprintf(key, sizeof key, "%s:%s", type, id);
    if (S->meta && !strcmp(S->meta_key, key)) {
        if (season < 0) emit_seasons(a, type, id, cb, ud);
        else            emit_episodes(a, type, season, cb, ud);
        return 0;
    }
    int m = find_meta_addon(a, type, id);
    if (m < 0) {
        PROV_LOG("%s: no addon gives details for %s/%s", S->tag, type, id);
        evo_error_set("No addon gives the episode list for this title. Add one that "
                      "serves details for it, or use a catalog with IMDb ids");
        return -1;
    }
    meta_ctx_t *x = (meta_ctx_t *)calloc(1, sizeof *x);
    if (!x) return -2;
    x->set = S;
    x->a = a; x->season = season; x->cb = cb; x->ud = ud;
    snprintf(x->type, sizeof x->type, "%s", type);
    snprintf(x->id, sizeof x->id, "%s", id);
    char url[EVO_PROVIDER_MAX_URL];
    snprintf(url, sizeof url, "%s/meta/%s/%s.json", S->ad[m].base, type, id);
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
    if (end == s || *end != ':' || v < 0 || v >= S->nad) return -1;
    *a = (int)v;
    const char *t = end + 1, *c = strchr(t, ':');
    if (!c || (size_t)(c - t) >= tcap) return -1;
    snprintf(type, tcap, "%.*s", (int)(c - t), t);
    *rest = c + 1;
    return 0;
}

static int st_list_catalog(const char *parent_id, int page,
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
/* Details and cast (the Triangle panel)                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    ad_set_t *set;
    char type[24];
    char id[160];
    evo_provider_rows_cb cb;
    void *ud;
} panel_ctx_t;

static void prow(evo_provider_row_t *rows, int *n, int max, const char *title, const char *detail)
{
    if (*n >= max || !detail || !detail[0]) return;
    evo_provider_row_t *r = &rows[(*n)++];
    memset(r, 0, sizeof *r);
    r->id[0] = '\0';
    snprintf(r->title, sizeof r->title, "%s", title);
    snprintf(r->detail, sizeof r->detail, "%s", detail);
}

static evo_provider_row_t *cast_row(evo_provider_row_t *rows, int *n, int max, const char *title, const char *detail)
{
    if (*n >= max || !title || !title[0]) return NULL;
    evo_provider_row_t *r = &rows[(*n)++];
    memset(r, 0, sizeof *r);
    r->id[0] = '\0';
    snprintf(r->title, sizeof r->title, "%s", title);
    if (detail && detail[0]) snprintf(r->detail, sizeof r->detail, "%s", detail);
    snprintf(r->group, sizeof r->group, "cast");
    return r;
}

/* A field that is a string, or an array of strings: "a, b, c". */
static void join_field(cJSON *v, char *out, size_t cap, int limit)
{
    size_t b = 0;
    out[0] = '\0';
    if (!v) return;
    if (cJSON_IsString(v)) { snprintf(out, cap, "%s", v->valuestring); return; }
    for (int i = 0; cJSON_IsArray(v) && i < cJSON_GetArraySize(v) && i < limit && b + 2 < cap; ++i) {
        cJSON *e = cJSON_GetArrayItem(v, i);
        if (cJSON_IsString(e) && e->valuestring[0])
            b += (size_t)snprintf(out + b, cap - b, "%s%s", b ? ", " : "", e->valuestring);
    }
}

/* Only the rows the addon sent: fields differ between Cinemeta, TMDB and AIOMetadata. */
static void build_details(cJSON *meta, evo_provider_row_t *rows, int *n, int max)
{
    char buf[160];
    cJSON *v;
    if (!meta) return;

    size_t b = 0;
    buf[0] = '\0';
    v = cJSON_GetObjectItem(meta, "imdbRating");
    const char *rating = v && cJSON_IsString(v) ? v->valuestring : "";
    double rnum = v && cJSON_IsNumber(v) ? v->valuedouble : 0;
    if (rating[0]) b += (size_t)snprintf(buf + b, sizeof buf - b, "%s/10 IMDb", rating);
    else if (rnum > 0) b += (size_t)snprintf(buf + b, sizeof buf - b, "%.1f/10 IMDb", rnum);
    const char *rel = jstr(meta, "releaseInfo")[0] ? jstr(meta, "releaseInfo") : jstr(meta, "year");
    if (rel[0]) b += (size_t)snprintf(buf + b, sizeof buf - b, "%s%s", b ? " · " : "", rel);
    if (jstr(meta, "runtime")[0]) b += (size_t)snprintf(buf + b, sizeof buf - b, "%s%s", b ? " · " : "", jstr(meta, "runtime"));
    if (b) prow(rows, n, max, "Rating", buf);

    join_field(cJSON_GetObjectItem(meta, "genres"), buf, sizeof buf, 8);
    if (!buf[0]) join_field(cJSON_GetObjectItem(meta, "genre"), buf, sizeof buf, 8);
    prow(rows, n, max, "Genres", buf);

    join_field(cJSON_GetObjectItem(meta, "director"), buf, sizeof buf, 4);
    prow(rows, n, max, "Director", buf);

    /* One row per actor, as the media-server panel does. The addons that give no
     * "cast" list name people under links[] with the category "Cast". */
    int cast = 0;
    cJSON *c = cJSON_GetObjectItem(meta, "cast");
    for (int i = 0; c && cJSON_IsArray(c) && i < cJSON_GetArraySize(c) && cast < 12; ++i) {
        cJSON *e = cJSON_GetArrayItem(c, i);
        const char *name = cJSON_IsString(e) ? e->valuestring : jstr(e, "name");
        if (!name[0]) continue;
        const char *role = cJSON_IsObject(e) ? (jstr(e, "character")[0] ? jstr(e, "character") : jstr(e, "role")) : "";
        evo_provider_row_t *r = cast_row(rows, n, max, name, role);
        if (r) {
            if (cJSON_IsObject(e)) {          /* TMDB-style addons: a full photo URL */
                const char *ph = jstr(e, "photo")[0] ? jstr(e, "photo") : (jstr(e, "image")[0] ? jstr(e, "image") : jstr(e, "profile"));
                if (!strncmp(ph, "http", 4)) snprintf(r->art_url, sizeof r->art_url, "%s", ph);
            }
            ++cast;
        }
    }
    cJSON *links = cJSON_GetObjectItem(meta, "links");
    for (int i = 0; !cast && links && cJSON_IsArray(links) && i < cJSON_GetArraySize(links) && cast < 12; ++i) {
        cJSON *e = cJSON_GetArrayItem(links, i);
        if (strcmp(jstr(e, "category"), "Cast") || !jstr(e, "name")[0]) continue;
        const char *role = cJSON_IsObject(e) ? (jstr(e, "character")[0] ? jstr(e, "character") : jstr(e, "role")) : "";
        if (cast_row(rows, n, max, jstr(e, "name"), role)) {
            ++cast;
        }
    }
}

static void deliver_panel(panel_ctx_t *x)
{
    evo_provider_row_t rows[20];
    int n = 0;
    cJSON *meta = S->meta ? cJSON_GetObjectItem(S->meta, "meta") : NULL;
    build_details(meta, rows, &n, 20);
    if (x->cb) x->cb(n > 0, rows, n, x->ud);
    free(x);
}

static void on_panel_meta(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    panel_ctx_t *x = (panel_ctx_t *)ud;
    S = x->set;
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    if (!root || !cJSON_GetObjectItem(root, "meta")) {
        if (root) cJSON_Delete(root);
        if (x->cb) x->cb(0, NULL, 0, x->ud);
        free(x);
        return;
    }
    if (S->meta) cJSON_Delete(S->meta);
    S->meta = root;
    snprintf(S->meta_key, sizeof S->meta_key, "%s:%s", x->type, x->id);
    deliver_panel(x);
}

static int st_panel(const char *item_id, const char *kind, evo_provider_rows_cb cb, void *ud)
{
    if (!item_id || !kind || strcmp(kind, "details")) return -1;
    if (strncmp(item_id, "m:", 2) && strncmp(item_id, "s:", 2)) return -1;
    int a;
    char type[24];
    const char *id;
    if (split3(item_id + 2, &a, type, sizeof type, &id) != 0) return -1;

    panel_ctx_t *x = (panel_ctx_t *)calloc(1, sizeof *x);
    if (!x) return -2;
    x->set = S;
    x->cb = cb; x->ud = ud;
    snprintf(x->type, sizeof x->type, "%s", type);
    snprintf(x->id, sizeof x->id, "%s", id);

    char key[256];
    snprintf(key, sizeof key, "%s:%s", type, id);
    if (S->meta && !strcmp(S->meta_key, key)) {        /* already fetched for the episode list */
        deliver_panel(x);
        return 0;
    }
    int m = find_meta_addon(a, type, id);
    if (m < 0) { free(x); return -1; }
    char url[EVO_PROVIDER_MAX_URL];
    snprintf(url, sizeof url, "%s/meta/%s/%s.json", S->ad[m].base, type, id);
    int rc = ad_get(url, on_panel_meta, x);
    if (rc != 0) free(x);
    return rc;
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
    S = r->fan->set;
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

static int st_search(const char *query, int page, evo_provider_items_cb cb, void *ud)
{
    if (!query || !*query || page > 0) return -1;
    char esc[256];
    if (evo_provider_url_escape(query, esc, sizeof esc) < 0) return -1;
    fan_t *f = (fan_t *)calloc(1, sizeof *f);
    if (!f) return -2;
    f->set = S;
    f->cb = cb; f->ud = ud;
    f->cap = EVO_PROVIDER_PAGE_MAX;
    f->items = (evo_provider_item_t *)calloc((size_t)f->cap, sizeof *f->items);
    if (!f->items) { free(f); return -2; }
    f->remaining = 1;
    for (int i = 0; i < S->nad; ++i) {
        for (int c = 0; S->ad[i].loaded == 1 && c < S->ad[i].ncats; ++c) {
            ad_cat_t *ct = &S->ad[i].cats[c];
            if (!ct->searchable) continue;
            char url[EVO_PROVIDER_MAX_URL];
            snprintf(url, sizeof url, "%s/catalog/%s/%s/search=%s.json",
                     S->ad[i].base, ct->type, ct->id, esc);
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
    /* Nothing to play: say why, for the screen in place of its generic text.
     * Catalog addons (Cinemeta) and subtitle addons serve no streams. */
    const char *where = f->set == &g_nuvio ? "in Nuvio's web version" : "in OPTIONS";
    if (f->nchoices == 0 && f->asked == 0)
        evo_error_set("None of your addons gives streams. Add a stream addon "
                      "(Torrentio, AIOStreams...) %s", where);
    else if (f->nchoices == 0 && f->torrents > 0)
        evo_error_set("Only torrent links were found. Set up a debrid service "
                      "(TorBox, Real-Debrid) in the stream addon");
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
    S = r->fan->set;
    fan_t *f = r->fan;
    cJSON *root = (ok && status == 200 && body) ? cJSON_Parse(body) : NULL;
    cJSON *arr = root ? cJSON_GetObjectItem(root, "streams") : NULL;
    for (int i = 0; arr && i < cJSON_GetArraySize(arr) && f->nchoices < AD_CHOICES; ++i) {
        cJSON *s = cJSON_GetArrayItem(arr, i);
        const char *url = jstr(s, "url");
        if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
            if (jstr(s, "infoHash")[0]) f->torrents++;
            continue;
        }
        evo_stream_choice_t *c = &f->choices[f->nchoices++];
        evo_provider_stream_choice_clear(c);
        snprintf(c->url, sizeof c->url, "%s", url);
        char name[48], title[64];
        first_line(jstr(s, "name")[0] ? jstr(s, "name") : S->ad[r->addon].name, name, sizeof name);
        first_line(jstr(s, "title")[0] ? jstr(s, "title") : jstr(s, "description"), title, sizeof title);
        if (title[0]) snprintf(c->label, sizeof c->label, "%s - %s", name, title);
        else          snprintf(c->label, sizeof c->label, "%s", name);
        if (strstr(url, ".m3u8")) snprintf(c->container, sizeof c->container, "hls");
        /* behaviorHints.proxyHeaders.request: headers the host wants on the stream. */
        cJSON *hdr = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(s, "behaviorHints"),
                                                             "proxyHeaders"), "request");
        size_t hl = 0;
        for (cJSON *h = hdr ? hdr->child : NULL; h; h = h->next) {
            if (!cJSON_IsString(h) || !h->string || !h->valuestring) continue;
            if (strpbrk(h->string, "\r\n:") || strpbrk(h->valuestring, "\r\n")) continue;
            int w = snprintf(c->headers + hl, sizeof c->headers - hl, "%s: %s\r\n", h->string, h->valuestring);
            if (w < 0 || (size_t)w >= sizeof c->headers - hl) { c->headers[hl] = 0; break; }
            hl += (size_t)w;
        }
    }
    if (!ok || status != 200)
        PROV_LOG("%s: streams from %s failed ok=%d http=%d", S->tag, S->ad[r->addon].name, ok, status);
    if (root) cJSON_Delete(root);
    fan_done_streams(f);
    free(r);
}

static int st_resolve_now(const char *item_id, evo_provider_resolve_cb cb, void *ud);

/* Resume from the home screen right after a start: the addons' manifests have
 * not been fetched yet (only the catalog screen does that), so every addon
 * looked like it served no streams. Fetch them, then resolve. */
typedef struct {
    ad_set_t *set;
    char item_id[EVO_PROVIDER_MAX_ITEM_ID];
    evo_provider_resolve_cb rcb;
    void *ud;
} late_resolve_t;

static void late_resolve_go(int ok, const evo_provider_item_t *items, int count, int has_more, void *ud)
{
    (void)ok; (void)items; (void)count; (void)has_more;
    late_resolve_t *l = (late_resolve_t *)ud;
    S = l->set;
    st_resolve_now(l->item_id, l->rcb, l->ud);
    free(l);
}

static int st_resolve(const char *item_id, evo_provider_resolve_cb cb, void *ud)
{
    int unloaded = 0;
    for (int i = 0; i < S->nad; ++i)
        if (S->ad[i].loaded == 0) unloaded = 1;
    if (unloaded && item_id && strlen(item_id) < EVO_PROVIDER_MAX_ITEM_ID) {
        late_resolve_t *l = (late_resolve_t *)calloc(1, sizeof *l);
        if (l) {
            l->set = S;
            snprintf(l->item_id, sizeof l->item_id, "%s", item_id);
            l->rcb = cb; l->ud = ud;
            if (list_root(late_resolve_go, l) == 0) return 0;
            free(l);
        }
    }
    return st_resolve_now(item_id, cb, ud);
}

static int st_resolve_now(const char *item_id, evo_provider_resolve_cb cb, void *ud)
{
    if (!item_id || (strncmp(item_id, "m:", 2) && strncmp(item_id, "e:", 2))) return -1;
    int a;
    char type[24];
    const char *id;
    if (split3(item_id + 2, &a, type, sizeof type, &id) != 0) return -1;
    fan_t *f = (fan_t *)calloc(1, sizeof *f);
    if (!f) return -2;
    f->set = S;
    f->rcb = cb; f->ud = ud;
    f->remaining = 1;
    for (int i = 0; i < S->nad; ++i) {
        addon_t *ad = &S->ad[i];
        if (ad->loaded != 1 || !ad->has_stream || !list_has(ad->stream_types, type) || !prefix_ok(ad, id))
            continue;
        char url[EVO_PROVIDER_MAX_URL];
        snprintf(url, sizeof url, "%s/stream/%s/%s.json", ad->base, type, id);
        fan_req_t *r = (fan_req_t *)calloc(1, sizeof *r);
        if (!r) continue;
        r->fan = f; r->addon = i;
        f->remaining++;
        f->asked++;
        if (ad_get(url, on_streams, r) != 0) { f->remaining--; free(r); }
    }
    fan_done_streams(f);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Nuvio: the addons the Nuvio web app has installed                         */
/* ------------------------------------------------------------------------- */

/*
 * Nuvio keeps its addon list in the page's localStorage, which EVO saves for
 * it (evo_webui.c, one JSON object of key -> string per site). The value is
 * Nuvio's per-profile envelope:
 *
 *   installedAddonUrls          {"profiles":{"<pid>":["https://addon", ...]}}
 *   installedAddonEnabledStates {"profiles":{"<pid>":{"https://addon":false}}}
 *   activeProfileId             "<pid>"
 *
 * The URLs are addon bases (no /manifest.json). With nothing saved yet, Nuvio
 * starts on Cinemeta, and so does this.
 */
#define NV_DEFAULT_ADDON "https://v3-cinemeta.strem.io"

static char *nv_read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 65536, used = 0;
    char *buf = (char *)malloc(cap);
    while (buf) {
        if (used + 1 >= cap) {
            if (cap >= (8u << 20)) break;
            char *g = (char *)realloc(buf, cap * 2);
            if (!g) { free(buf); buf = NULL; break; }
            buf = g;
            cap *= 2;
        }
        size_t n = fread(buf + used, 1, cap - used - 1, f);
        if (n == 0) break;
        used += n;
    }
    fclose(f);
    if (buf) buf[used] = '\0';
    return buf;
}

/* A profile-scoped value: the active profile's, else the primary ("1"), else
 * the first. Older saves held the bare value with no envelope. */
static cJSON *nv_profile_value(cJSON *env, const char *pid)
{
    cJSON *profiles = cJSON_GetObjectItem(env, "profiles");
    if (!profiles) return env;
    cJSON *v = cJSON_GetObjectItem(profiles, pid);
    if (!v) v = cJSON_GetObjectItem(profiles, "1");
    if (!v) v = profiles->child;
    return v;
}

/* The active profile id: saved as a JSON string ("\"2\"") or bare, else "1". */
static void nv_active_pid(cJSON *store, char *pid, size_t cap)
{
    snprintf(pid, cap, "%s", jstr(store, "activeProfileId"));
    char *p = pid;
    while (*p == '"') p++;
    memmove(pid, p, strlen(p) + 1);
    size_t l = strlen(pid);
    while (l > 0 && pid[l - 1] == '"') pid[--l] = '\0';
    if (!pid[0]) snprintf(pid, cap, "1");
}

/* Whether the last load found an addon list that was saved (not the Cinemeta
 * default Nuvio starts on). Only a saved list has entries to remove. */
static int nv_saved = 0;

static void nv_load_addons(void)
{
    static char urls[AD_MAX][AD_URL];
    int n = 0;

    char *txt = nv_read_file(evo_data_path(evo_nuvio_store_file()));
    cJSON *store = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    char pid[32];
    nv_active_pid(store, pid, sizeof pid);
    const char *raw = jstr(store, "installedAddonUrls");
    cJSON *env = raw[0] ? cJSON_Parse(raw) : NULL;
    raw = jstr(store, "installedAddonEnabledStates");
    cJSON *en_env = raw[0] ? cJSON_Parse(raw) : NULL;
    cJSON *list = env ? nv_profile_value(env, pid) : NULL;
    cJSON *enabled = en_env ? nv_profile_value(en_env, pid) : NULL;
    nv_saved = list && cJSON_IsArray(list);

    for (int i = 0; list && cJSON_IsArray(list) && i < cJSON_GetArraySize(list) && n < AD_MAX; ++i) {
        cJSON *u = cJSON_GetArrayItem(list, i);
        if (!u || !cJSON_IsString(u) || strncmp(u->valuestring, "http", 4)) continue;
        if (cJSON_IsFalse(cJSON_GetObjectItem(enabled, u->valuestring))) continue;
        char *out = urls[n];
        snprintf(out, AD_URL, "%s", u->valuestring);
        size_t l = strlen(out);
        while (l > 0 && out[l - 1] == '/') out[--l] = '\0';
        if (l < 14 || strcmp(out + l - 14, "/manifest.json"))
            strncat(out, "/manifest.json", AD_URL - l - 1);
        n++;
    }
    if (!list || !cJSON_IsArray(list))
        snprintf(urls[n++], AD_URL, "%s/manifest.json", NV_DEFAULT_ADDON);
    if (env) cJSON_Delete(env);
    if (en_env) cJSON_Delete(en_env);
    if (store) cJSON_Delete(store);

    /* Unchanged: keep the loaded manifests. */
    int same = n == user_count();
    for (int i = 0; same && i < n; ++i) same = !strcmp(S->ad[i].url, urls[i]);
    if (same) return;

    static addon_t prev[AD_MAX];
    const int nprev = S->nad;
    memcpy(prev, S->ad, sizeof prev);
    memset(S->ad, 0, sizeof S->ad);
    S->nad = 0;
    for (int i = 0; i < n; ++i) {
        addon_t *a = &S->ad[S->nad++];
        int j = 0;
        while (j < nprev && strcmp(prev[j].url, urls[i])) j++;
        if (j < nprev) { *a = prev[j]; a->builtin = 0; continue; }
        snprintf(a->url, AD_URL, "%s", urls[i]);
        ad_set_base(a);
    }
    builtin_sync();
    /* Row ids carry addon indexes, which may have moved. */
    S->cat_key[0] = '\0';
    S->meta_key[0] = '\0';
    if (S->meta) { cJSON_Delete(S->meta); S->meta = NULL; }
    PROV_LOG("nuvio: %d addon%s from %s", S->nad, S->nad == 1 ? "" : "s", evo_nuvio_store_file());
}

/*
 * Write back. Adding and removing edit the same two page-storage keys the web
 * app reads, for the active profile, so the web version shows the same list.
 * Nuvio keeps addon bases (no /manifest.json) in the list; a base is matched
 * ignoring a trailing slash and /manifest.json.
 */
static void nv_base_of(const char *in, char *out, size_t cap)
{
    snprintf(out, cap, "%s", in);
    size_t l = strlen(out);
    while (l > 0 && out[l - 1] == '/') out[--l] = '\0';
    if (l >= 14 && !strcmp(out + l - 14, "/manifest.json")) { l -= 14; out[l] = '\0'; }
    while (l > 0 && out[l - 1] == '/') out[--l] = '\0';
}

static int nv_same_addon(const char *a, const char *b)
{
    char x[AD_URL], y[AD_URL];
    nv_base_of(a, x, sizeof x);
    nv_base_of(b, y, sizeof y);
    return !strcmp(x, y);
}

/* The active profile's slot in a per-profile envelope, made if it is missing.
 * Where the envelope has no such profile, the slot is the one nv_load_addons
 * showed (the primary, else the first), so what is edited is what was listed. */
static cJSON *nv_slot(cJSON **env, const char *pid, int want_array)
{
    if (!*env || (!cJSON_IsObject(*env) && !cJSON_IsArray(*env))) {
        if (*env) cJSON_Delete(*env);
        *env = cJSON_CreateObject();
        if (!*env) return NULL;
    }
    cJSON *v;
    if (cJSON_IsArray(*env)) {
        v = *env;                           /* older save: the bare value */
    } else {
        cJSON *profiles = cJSON_GetObjectItem(*env, "profiles");
        if (!profiles) {
            profiles = cJSON_CreateObject();
            if (!profiles || !cJSON_AddItemToObject(*env, "profiles", profiles)) return NULL;
        }
        v = nv_profile_value(*env, pid);
        if (!v) {
            v = want_array ? cJSON_CreateArray() : cJSON_CreateObject();
            if (!v) return NULL;
            cJSON_AddItemToObject(profiles, pid, v);
        }
    }
    return (want_array ? cJSON_IsArray(v) : cJSON_IsObject(v)) ? v : NULL;
}

static int nv_put_key(cJSON *store, const char *key, cJSON *env)
{
    char *txt = cJSON_PrintUnformatted(env);
    if (!txt) return -1;
    cJSON_DeleteItemFromObject(store, key);
    int ok = cJSON_AddStringToObject(store, key, txt);
    free(txt);
    return ok ? 0 : -1;
}

/* Add (add != 0) or drop one addon base in the saved storage. 0 ok. */
static int nv_edit(const char *base, int add)
{
    char path[512];     /* evo_data_path() reuses one buffer: keep our own copy */
    snprintf(path, sizeof path, "%s", evo_data_path(evo_nuvio_store_file()));
    char *txt = nv_read_file(path);
    cJSON *store = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    if (store && !cJSON_IsObject(store)) { cJSON_Delete(store); store = NULL; }
    if (!store) store = cJSON_CreateObject();
    if (!store) return -1;

    char pid[32];
    nv_active_pid(store, pid, sizeof pid);
    const char *raw = jstr(store, "installedAddonUrls");
    cJSON *env = raw[0] ? cJSON_Parse(raw) : NULL;
    const int had_list = env && nv_profile_value(env, pid) && cJSON_IsArray(nv_profile_value(env, pid));
    raw = jstr(store, "installedAddonEnabledStates");
    cJSON *en_env = raw[0] ? cJSON_Parse(raw) : NULL;

    int rc = -1, present = 0;
    cJSON *states = NULL;
    char *outtxt = NULL;
    FILE *f = NULL;
    cJSON *list = nv_slot(&env, pid, 1);
    if (!list) goto out;

    for (int i = cJSON_GetArraySize(list) - 1; i >= 0; --i) {
        cJSON *u = cJSON_GetArrayItem(list, i);
        if (!u || !cJSON_IsString(u) || !nv_same_addon(u->valuestring, base)) continue;
        present = 1;
        if (!add) cJSON_DeleteItemFromArray(list, i);
    }
    if (add && !present) {
        /* Nothing saved yet: Nuvio was running on Cinemeta, so keep it. */
        if (!had_list && !nv_same_addon(NV_DEFAULT_ADDON, base))
            cJSON_AddItemToArray(list, cJSON_CreateString(NV_DEFAULT_ADDON));
        cJSON_AddItemToArray(list, cJSON_CreateString(base));
    }
    if (nv_put_key(store, "installedAddonUrls", env) != 0) goto out;

    /* Keep the enabled states in step: a removed addon's flag goes, an added
     * one is switched on if it had been switched off earlier. */
    states = en_env ? nv_profile_value(en_env, pid) : NULL;
    if (states && cJSON_IsObject(states)) {
        for (cJSON *c = states->child, *next; c; c = next) {
            next = c->next;
            if (c->string && nv_same_addon(c->string, base))
                cJSON_DeleteItemFromObject(states, c->string);
        }
        if (nv_put_key(store, "installedAddonEnabledStates", en_env) != 0) goto out;
    }

    outtxt = cJSON_PrintUnformatted(store);
    if (!outtxt) goto out;
    evo_mkdir(evo_data_path("webui"));
    f = fopen(path, "wb");
    if (f) {
        rc = fwrite(outtxt, 1, strlen(outtxt), f) == strlen(outtxt) ? 0 : -1;
        fclose(f);
    }
out:
    free(outtxt);
    if (env) cJSON_Delete(env);
    if (en_env) cJSON_Delete(en_env);
    cJSON_Delete(store);
    return rc;
}

/* The addons Nuvio has saved, which EVO can remove. With nothing saved, the
 * Cinemeta it runs on is a default, not an entry, and is not offered. */
int evo_nuvio_addon_count(void)
{
    S = &g_nuvio;
    nv_load_addons();
    return nv_saved ? user_count() : 0;
}

const char *evo_nuvio_addon_name(int i)
{
    S = &g_nuvio;
    return addon_label(i);
}

int evo_nuvio_addon_remove(int i)
{
    S = &g_nuvio;
    if (i < 0 || i >= user_count() || !nv_saved) return -1;
    char base[AD_URL];
    snprintf(base, sizeof base, "%s", S->ad[i].base);
    if (nv_edit(base, 0) != 0) return -1;
    nv_load_addons();
    return 0;
}

/* Add an addon: a manifest URL, a base URL or a stremio:// link. 0 ok (also
 * when it was already there), -1 not an address, -2 could not be saved. */
int evo_nuvio_addon_add(const char *value)
{
    char v[AD_URL], url[AD_URL], base[AD_URL];
    snprintf(v, sizeof v, "%s", value ? value : "");
    char *s = ad_trim(v);
    if (!*s || ad_parse_address(s, url, sizeof url) != 0) {
        PROV_LOG("nuvio: add addon: not an address (%d chars)", (int)strlen(s));
        return -1;
    }
    nv_base_of(url, base, sizeof base);
    if (nv_edit(base, 1) != 0) {
        PROV_LOG("nuvio: add addon: could not save %s", evo_nuvio_store_file());
        return -2;
    }
    PROV_LOG("nuvio: added addon %.120s", base);
    S = &g_nuvio;
    nv_load_addons();
    return 0;
}

int evo_nuvio_list_catalog(const char *parent_id, int page, evo_provider_items_cb cb, void *ud)
{
    S = &g_nuvio;
    return st_list_catalog(parent_id, page, cb, ud);
}

int evo_nuvio_search(const char *query, int page, evo_provider_items_cb cb, void *ud)
{
    S = &g_nuvio;
    if (S->nad == 0) nv_load_addons();
    return st_search(query, page, cb, ud);
}

int evo_nuvio_panel(const char *item_id, const char *kind, evo_provider_rows_cb cb, void *ud)
{
    S = &g_nuvio;
    return st_panel(item_id, kind, cb, ud);
}

int evo_nuvio_resolve(const char *item_id, evo_provider_resolve_cb cb, void *ud)
{
    S = &g_nuvio;
    return st_resolve(item_id, cb, ud);
}

/* ------------------------------------------------------------------------- */
/* The Stremio Addons provider                                               */
/* ------------------------------------------------------------------------- */

static int ad_list_catalog(const char *parent_id, int page, evo_provider_items_cb cb, void *ud)
{
    S = &g_addons;
    return st_list_catalog(parent_id, page, cb, ud);
}

static int ad_search(const char *query, int page, evo_provider_items_cb cb, void *ud)
{
    S = &g_addons;
    return st_search(query, page, cb, ud);
}

static int ad_resolve(const char *item_id, evo_provider_resolve_cb cb, void *ud)
{
    S = &g_addons;
    return st_resolve(item_id, cb, ud);
}

static int ad_panel(const char *item_id, const char *kind, evo_provider_rows_cb cb, void *ud)
{
    S = &g_addons;
    return st_panel(item_id, kind, cb, ud);
}

const evo_provider_t evo_provider_stremio = {
    .id            = "addons",
    .name          = "Stremio",
    .icon          = "brand_stremio.png",
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
    .panel         = ad_panel,
};
