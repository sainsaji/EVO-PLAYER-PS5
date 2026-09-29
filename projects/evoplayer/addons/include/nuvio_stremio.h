/*
 * nuvio_stremio.h — the Stremio addon protocol, as NuvioTV speaks it.
 *
 * Pure parsing and URL building: no network, no files, no provider state.
 * provider_nuvio_native.c owns all of that and calls in here, which is what
 * lets every rule below be tested on the host against captured response
 * shapes (tests/test_runner.c, test_nuvio_*).
 *
 * The rules are ported from NuvioTV rather than from the Stremio docs, because
 * NuvioTV is what the addons people actually install are tested against:
 *
 *   base URL      AddonRepositoryImpl.canonicalizeUrl - strip /manifest.json,
 *                 keep a configured addon's ?query and re-append it LAST
 *   catalog URL   CatalogRepositoryImpl.buildCatalogUrl
 *   meta/stream   MetaRepositoryImpl / StreamRepositoryImpl - type and id are
 *                 percent-encoded path segments
 *   resources     AddonMapper.parseResources - a bare string inherits the
 *                 manifest's types; an object carries its own
 */
#ifndef NUVIO_STREMIO_H
#define NUVIO_STREMIO_H

#include <stddef.h>
#include <stdint.h>

#include "evo_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A configured addon's URL carries its whole config (Torrentio, AIOStreams),
 * which is routinely over 500 bytes. Leaves room under evo_net's 2048 for the
 * /stream/<type>/<id>.json that goes after it. */
#define NUVIO_MAX_BASE_URL   1600

#define NUVIO_MAX_CATALOGS   48    /* per addon */
#define NUVIO_MAX_RESOURCES  8
#define NUVIO_MAX_TYPES      8
#define NUVIO_MAX_PREFIXES   8

typedef struct nuvio_catalog {
    char type[24];
    char id[96];
    char name[96];
    int  supports_search;       /* "search" in extra / extraSupported        */
    int  supports_skip;         /* "skip" in extra / extraSupported          */
    /* extraRequired names something other than search: the catalog cannot be
     * listed without an argument EVO has no UI for (a genre it must pick).  */
    int  needs_other_extra;
    int  search_only;           /* extraRequired is exactly "search"          */
    int  show_in_home;          /* manifest's showInHome, default 1           */
} nuvio_catalog_t;

typedef struct nuvio_resource {
    char name[24];                              /* "stream", "meta", ...   */
    char types[NUVIO_MAX_TYPES][24];
    int  type_count;
    char prefixes[NUVIO_MAX_PREFIXES][24];
    int  prefix_count;                          /* 0 = inherit the addon's */
} nuvio_resource_t;

typedef struct nuvio_addon {
    char base_url[NUVIO_MAX_BASE_URL];          /* canonical, see above    */
    char id[96];
    char name[96];

    nuvio_catalog_t  catalogs[NUVIO_MAX_CATALOGS];
    int              catalog_count;

    nuvio_resource_t resources[NUVIO_MAX_RESOURCES];
    int              resource_count;

    char types[NUVIO_MAX_TYPES][24];
    int  type_count;
    char prefixes[NUVIO_MAX_PREFIXES][24];
    int  prefix_count;
} nuvio_addon_t;

/* One entry of a meta's videos[]: an episode, or for a movie nothing. */
typedef struct nuvio_video {
    char id[EVO_PROVIDER_MAX_ITEM_ID];
    char title[EVO_PROVIDER_MAX_TITLE];
    char overview[EVO_PROVIDER_MAX_OVERVIEW];
    char thumbnail[EVO_PROVIDER_MAX_ART_URL];
    char released[32];
    int  season;
    int  episode;
} nuvio_video_t;

typedef struct nuvio_meta {
    char id[EVO_PROVIDER_MAX_ITEM_ID];
    char type[24];
    char name[EVO_PROVIDER_MAX_TITLE];
    char release_info[32];
    char description[EVO_PROVIDER_MAX_OVERVIEW];
    char poster[EVO_PROVIDER_MAX_ART_URL];
    char background[EVO_PROVIDER_MAX_ART_URL];
    int64_t runtime_sec;

    nuvio_video_t *videos;      /* malloc'd; nuvio_meta_free() */
    int            video_count;
} nuvio_meta_t;

/* One stream an addon returned. */
typedef struct nuvio_stream {
    char addon_name[96];
    char name[128];             /* the addon's label, often "Torrentio\n4k"   */
    char description[512];      /* title/description: size, seeders, codec    */
    char url[EVO_PROVIDER_MAX_URL];   /* direct http(s), or a built magnet   */
    char filename[256];         /* behaviorHints.filename                    */
    int  needs_resolver;        /* url is a magnet - a debrid provider's job  */
    int  needs_headers;         /* behaviorHints.proxyHeaders.request: EVO's
                                 * player cannot send them yet (doc §5)      */
    int64_t size_bytes;         /* behaviorHints.videoSize, 0 if unknown      */
    int  quality;               /* 2160/1080/720/480 guessed from the text, 0 */
} nuvio_stream_t;

/* ---- URLs ---------------------------------------------------------------- */

/*
 * A manifest URL as a user pastes it (https://…/manifest.json, stremio://…,
 * with or without a trailing slash or a query) into the canonical base every
 * resource URL is built from. Returns 0, or -1 if it is not an addon URL.
 */
int nuvio_canonical_base(const char *manifest_url, char *out, size_t cap);

/* {base}/manifest.json{query}. */
int nuvio_manifest_url(const char *base, char *out, size_t cap);

/*
 * {base}/catalog/{type}/{id}[/{extra}].json{query}, where extra is
 * "search=<q>" and/or "skip=<n>", percent-encoded, in that order - and with no
 * extra at all a bare skip keeps NuvioTV's "/skip=<n>.json" form. `search` may
 * be NULL; `skip` 0 means none.
 */
int nuvio_catalog_url(const char *base, const char *type, const char *catalog_id,
                      const char *search, int skip, char *out, size_t cap);

/* {base}/{resource}/{type}/{id}.json{query}, type and id percent-encoded. */
int nuvio_resource_url(const char *base, const char *resource, const char *type,
                       const char *id, char *out, size_t cap);

/* ---- Manifest ------------------------------------------------------------ */

int nuvio_parse_manifest(const char *json, size_t len, const char *base,
                         nuvio_addon_t *out);

/* 1 when `addon` answers {resource}/{type}/{id}. */
int nuvio_addon_serves(const nuvio_addon_t *addon, const char *resource,
                       const char *type, const char *id);

/* ---- Catalog ------------------------------------------------------------- */

/*
 * A catalog response's metas[] as provider items, each a folder whose id is
 * "m:<type>:<id>". Ids that would not fit in EVO_PROVIDER_MAX_ITEM_ID are
 * dropped, never truncated - a truncated id is a different item. Duplicate ids
 * are dropped too (NuvioTV's distinctBy). `fallback_type` fills a meta with no
 * type of its own. Returns the number written, or -1 on unparseable JSON;
 * *raw_count gets the number of metas in the response before any was dropped,
 * which is what paging decides on.
 */
int nuvio_parse_catalog(const char *json, size_t len, const char *fallback_type,
                        evo_provider_item_t *out, int cap, int *raw_count);

/* ---- Meta ---------------------------------------------------------------- */

int  nuvio_parse_meta(const char *json, size_t len, nuvio_meta_t *out);
void nuvio_meta_free(nuvio_meta_t *m);

/* Seasons present in m->videos, ascending, season 0 (specials) last. */
int  nuvio_meta_seasons(const nuvio_meta_t *m, int *out, int cap);

/* ---- Streams ------------------------------------------------------------- */

/*
 * Append a stream response's streams[] to `out` (capacity `cap`, currently
 * *count used). Streams EVO cannot do anything with - YouTube ids, external
 * web pages, empty entries - are skipped. `season`/`episode` (0 = none) ride
 * along inside a built magnet so a debrid resolver can pick the right file
 * from a season pack. Returns the number appended, or -1 on bad JSON.
 */
int nuvio_parse_streams(const char *json, size_t len, const char *addon_name,
                        int season, int episode,
                        nuvio_stream_t *out, int cap, int *count);

/*
 * Best first: playable-without-headers before not, then resolution. Stable
 * for equal keys, so each addon's own order survives inside a tier -
 * Torrentio sorts by seeders and that is worth keeping over raw size.
 */
void nuvio_sort_streams(nuvio_stream_t *s, int count);

/* ---- Magnets (shared with the debrid resolvers) -------------------------- */

/*
 * The magnet EVO hands to the resolver chain: btih, trackers, and EVO's own
 * hints as evo_* parameters (fidx, fn, s, e). A resolver strips those before
 * sending the magnet anywhere (nuvio_magnet_strip_hints).
 */
int nuvio_build_magnet(const char *info_hash, const char *const *sources,
                       int source_count, int file_idx, const char *filename,
                       int season, int episode, char *out, size_t cap);

typedef struct nuvio_magnet_hints {
    char info_hash[64];
    int  file_idx;              /* -1 = none */
    char filename[256];
    int  season;                /* 0 = none  */
    int  episode;
} nuvio_magnet_hints_t;

/* 0 if `url` is a magnet with a btih; fills `h`. */
int nuvio_magnet_parse(const char *url, nuvio_magnet_hints_t *h);

/* `url` without its evo_* parameters, for sending to a debrid service. */
int nuvio_magnet_strip_hints(const char *url, char *out, size_t cap);

/* ---- Small helpers ------------------------------------------------------- */

/* Percent-decode in place. */
void nuvio_url_decode(char *s);

/* Highest resolution named in `text` ("2160p", "4K", "1080p", ...), or 0. */
int  nuvio_guess_quality(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* NUVIO_STREMIO_H */
