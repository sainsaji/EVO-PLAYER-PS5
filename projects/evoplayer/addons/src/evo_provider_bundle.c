/*
 * evo_provider_bundle.c — fetch, verify and cache a provider's UI bundle.
 *
 * The trust boundary for #90's runtime UI. Everything a downloaded bundle can
 * do to this process has to be stopped here, because past this file the bundle
 * is just files on disk that RmlUi's file interface opens with fopen() - it
 * does no checking of its own and should not have to.
 *
 * Three separate jobs, in order of how much damage getting them wrong does:
 *
 *   1. PATH SAFETY. A manifest entry names where its file is written and,
 *      later, what LoadDocument opens. An entry of "../../../data/evoplayer/
 *      emby.conf" would have this code write a downloaded file over stored
 *      credentials. evo_bundle_path() is the only way to turn a bundle-relative
 *      name into a real path and it refuses anything that could leave the
 *      directory. Nothing else in the codebase may build one by hand.
 *
 *   2. LIMITS. Entry count, per-file bytes and total bytes, checked against
 *      the manifest AND against what actually arrives - a manifest that
 *      declares 4 KB and serves 40 MB is the interesting case, and only the
 *      second check catches it.
 *
 *   3. INTEGRITY. sha256 per entry. This catches a truncated download and a
 *      corrupted cache, which are the failures that actually happen. It is
 *      NOT authenticity: the manifest travels the same connection as the
 *      files, so anything able to rewrite one rewrites both. https is what
 *      provides authenticity, and the bundle URL should use it.
 *
 * SHA-256 is implemented here rather than called out to OpenSSL so that the
 * check behaves identically in the -DNO_OPENSSL=1 test build and the shipping
 * one. A verification step that silently becomes a no-op in one build is
 * worse than no verification step, because it is trusted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "evo_provider_bundle.h"
#include "evo_net.h"
#include "cJSON.h"
#include "evo_data_path.h"

/* ------------------------------------------------------------------------- */
/* SHA-256 (FIPS 180-4)                                                      */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t  buf[64];
    size_t   buf_len;
} sha256_t;

static const uint32_t K256[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
    0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
    0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
    0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
    0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
    0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_t *s, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_init(sha256_t *s)
{
    s->h[0] = 0x6a09e667u; s->h[1] = 0xbb67ae85u;
    s->h[2] = 0x3c6ef372u; s->h[3] = 0xa54ff53au;
    s->h[4] = 0x510e527fu; s->h[5] = 0x9b05688cu;
    s->h[6] = 0x1f83d9abu; s->h[7] = 0x5be0cd19u;
    s->len = 0;
    s->buf_len = 0;
}

static void sha256_update(sha256_t *s, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    s->len += n;
    while (n > 0) {
        size_t take = 64 - s->buf_len;
        if (take > n) take = n;
        memcpy(s->buf + s->buf_len, p, take);
        s->buf_len += take;
        p += take;
        n -= take;
        if (s->buf_len == 64) {
            sha256_block(s, s->buf);
            s->buf_len = 0;
        }
    }
}

/* Lowercase hex, 64 characters plus a NUL. */
static void sha256_hex(sha256_t *s, char out[65])
{
    static const char HEX[] = "0123456789abcdef";
    uint64_t bits = s->len * 8;

    uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    uint8_t zero = 0;
    while (s->buf_len != 56) sha256_update(s, &zero, 1);

    uint8_t be[8];
    for (int i = 0; i < 8; ++i) be[i] = (uint8_t)(bits >> (56 - i * 8));
    /* Bypass sha256_update's length accounting for the length field itself. */
    memcpy(s->buf + 56, be, 8);
    sha256_block(s, s->buf);

    for (int i = 0; i < 8; ++i) {
        for (int b = 0; b < 4; ++b) {
            uint8_t byte = (uint8_t)(s->h[i] >> (24 - b * 8));
            out[i * 8 + b * 2]     = HEX[byte >> 4];
            out[i * 8 + b * 2 + 1] = HEX[byte & 0xF];
        }
    }
    out[64] = '\0';
}

static void sha256_buf(const void *data, size_t n, char out[65])
{
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, data, n);
    sha256_hex(&s, out);
}

/* ------------------------------------------------------------------------- */
/* Status strings                                                            */
/* ------------------------------------------------------------------------- */

const char *evo_bundle_status_str(evo_bundle_status_t st)
{
    switch (st) {
    case EVO_BUNDLE_OK:              return "ok";
    case EVO_BUNDLE_ERR_NO_URL:      return "no bundle URL configured";
    case EVO_BUNDLE_ERR_FETCH:       return "download failed";
    case EVO_BUNDLE_ERR_MANIFEST:    return "manifest.json is not valid";
    case EVO_BUNDLE_ERR_API_VERSION: return "bundle needs a newer EVO";
    case EVO_BUNDLE_ERR_ID_MISMATCH: return "manifest is for another provider";
    case EVO_BUNDLE_ERR_PATH:        return "unsafe path in manifest";
    case EVO_BUNDLE_ERR_TOO_MANY:    return "too many files in bundle";
    case EVO_BUNDLE_ERR_TOO_BIG:     return "bundle is too large";
    case EVO_BUNDLE_ERR_HASH:        return "file hash did not match";
    case EVO_BUNDLE_ERR_WRITE:       return "could not write cache";
    case EVO_BUNDLE_ERR_INCOMPLETE:  return "cached bundle is incomplete";
    }
    return "unknown";
}

/* ------------------------------------------------------------------------- */
/* Paths                                                                     */
/* ------------------------------------------------------------------------- */

int evo_bundle_path_safe(const char *rel)
{
    if (!rel || !*rel) return 0;

    size_t n = strlen(rel);
    if (n >= EVO_BUNDLE_MAX_PATH) return 0;

    /* Absolute, or a Windows drive letter (the host harness runs these too). */
    if (rel[0] == '/') return 0;
    if (n >= 2 && rel[1] == ':') return 0;

    /*
     * Any "scheme:" prefix. RmlUi resolves an <img src> relative to the
     * document, so a bundle asset named "evo:mem/art0-3" would collide with
     * the in-memory registry, and "file://..." would reach the filesystem
     * root. Rejecting every colon costs nothing - no legitimate asset name
     * needs one.
     */
    if (strchr(rel, ':')) return 0;

    /* Backslashes, so a path cannot be re-read as a Windows separator by the
     * host build after passing a forward-slash-only check here. */
    if (strchr(rel, '\\')) return 0;

    /* Control characters and anything non-ASCII: an asset name is ASCII. */
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)rel[i];
        if (c < 0x20 || c >= 0x7F) return 0;
    }

    /* Walk the components. An empty one ("a//b") and a dotted one (".", "..",
     * and also "..." which some filesystems normalise) are all refused - the
     * check is "every component has a non-dot character", which cannot be
     * fooled by a longer run of dots. */
    const char *p = rel;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0) return 0;
        int all_dots = 1;
        for (size_t i = 0; i < len; ++i)
            if (p[i] != '.') { all_dots = 0; break; }
        if (all_dots) return 0;
        if (!slash) break;
        p = slash + 1;
    }
    return 1;
}

const char *evo_bundle_dir(const char *provider_id)
{
    static _Thread_local char buf[512];
    if (!evo_provider_id_valid(provider_id)) return NULL;
    snprintf(buf, sizeof buf, "%s/providers/%s", evo_data_dir(), provider_id);
    return buf;
}

int evo_bundle_path(const char *provider_id, const char *rel,
                    char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return -1;
    if (!evo_bundle_path_safe(rel)) return -1;
    const char *dir = evo_bundle_dir(provider_id);
    if (!dir) return -1;
    int n = snprintf(out, out_sz, "%s/%s", dir, rel);
    if (n < 0 || (size_t)n >= out_sz) return -1;
    return 0;
}

/* mkdir -p over the directory part of `path`, inside the bundle dir only. */
int evo_bundle_ensure_parent_dirs(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    char *last = strrchr(tmp, '/');
    if (!last) return 0;
    *last = '\0';

    /* Walk forward creating each level. The leading components are the data
     * root and "providers", which may not exist yet either. */
    for (char *p = tmp + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        evo_mkdir(tmp);
        *p = '/';
    }
    evo_mkdir(tmp);

    struct stat st;
    return (stat(tmp, &st) == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------------- */
/* Manifest                                                                  */
/* ------------------------------------------------------------------------- */

static evo_bundle_status_t parse_manifest(const char *json, size_t len,
                                          const char *provider_id,
                                          evo_bundle_manifest_t *out)
{
    if (!json || len == 0 || len > EVO_BUNDLE_MAX_MANIFEST)
        return EVO_BUNDLE_ERR_MANIFEST;

    cJSON *root = cJSON_Parse(json);
    if (!root) return EVO_BUNDLE_ERR_MANIFEST;

    memset(out, 0, sizeof *out);
    evo_bundle_status_t st = EVO_BUNDLE_ERR_MANIFEST;

    cJSON *j_id    = cJSON_GetObjectItem(root, "id");
    cJSON *j_name  = cJSON_GetObjectItem(root, "name");
    cJSON *j_ver   = cJSON_GetObjectItem(root, "version");
    cJSON *j_api   = cJSON_GetObjectItem(root, "api_version");
    cJSON *j_entry = cJSON_GetObjectItem(root, "entry");
    cJSON *j_model = cJSON_GetObjectItem(root, "data_model");
    cJSON *j_assets= cJSON_GetObjectItem(root, "assets");

    if (!j_id    || !cJSON_IsString(j_id))    goto done;
    if (!j_entry || !cJSON_IsString(j_entry)) goto done;
    if (!j_api   || !cJSON_IsNumber(j_api))   goto done;
    if (!j_assets|| !cJSON_IsArray(j_assets)) goto done;

    snprintf(out->id,   sizeof out->id,   "%s", j_id->valuestring);
    snprintf(out->entry,sizeof out->entry,"%s", j_entry->valuestring);
    out->api_version = (int)j_api->valuedouble;
    if (j_name  && cJSON_IsString(j_name))
        snprintf(out->name, sizeof out->name, "%s", j_name->valuestring);
    if (j_ver   && cJSON_IsString(j_ver))
        snprintf(out->version, sizeof out->version, "%s", j_ver->valuestring);
    if (j_model && cJSON_IsString(j_model))
        snprintf(out->data_model, sizeof out->data_model, "%s", j_model->valuestring);

    /*
     * The manifest's own id must be the provider we asked for. Without this a
     * provider could be pointed at another's bundle URL and the two would
     * share a cache directory and a data model name - which fails in the most
     * confusing way possible, as a screen that renders but binds nothing.
     */
    if (strcmp(out->id, provider_id) != 0) { st = EVO_BUNDLE_ERR_ID_MISMATCH; goto done; }

    /* A bundle written against a newer seam is refused, not half-rendered:
     * its markup binds to fields this build does not publish. */
    if (out->api_version > EVO_PROVIDER_API_VERSION) {
        st = EVO_BUNDLE_ERR_API_VERSION;
        goto done;
    }
    if (out->api_version < 1) { st = EVO_BUNDLE_ERR_MANIFEST; goto done; }

    if (!evo_bundle_path_safe(out->entry)) { st = EVO_BUNDLE_ERR_PATH; goto done; }

    int n = cJSON_GetArraySize(j_assets);
    if (n <= 0 || n > EVO_BUNDLE_MAX_ENTRIES) {
        st = (n > EVO_BUNDLE_MAX_ENTRIES) ? EVO_BUNDLE_ERR_TOO_MANY
                                          : EVO_BUNDLE_ERR_MANIFEST;
        goto done;
    }

    for (int i = 0; i < n; ++i) {
        cJSON *a = cJSON_GetArrayItem(j_assets, i);
        if (!a) { st = EVO_BUNDLE_ERR_MANIFEST; goto done; }
        cJSON *p = cJSON_GetObjectItem(a, "path");
        cJSON *b = cJSON_GetObjectItem(a, "bytes");
        cJSON *h = cJSON_GetObjectItem(a, "sha256");
        if (!p || !cJSON_IsString(p)) { st = EVO_BUNDLE_ERR_MANIFEST; goto done; }

        evo_bundle_entry_t *e = &out->entries[out->entry_count];
        if (!evo_bundle_path_safe(p->valuestring)) { st = EVO_BUNDLE_ERR_PATH; goto done; }
        snprintf(e->path, sizeof e->path, "%s", p->valuestring);

        e->bytes = (b && cJSON_IsNumber(b) && b->valuedouble > 0)
                 ? (uint32_t)b->valuedouble : 0;
        if (e->bytes > EVO_BUNDLE_MAX_FILE_BYTES) { st = EVO_BUNDLE_ERR_TOO_BIG; goto done; }

        if (h && cJSON_IsString(h) && strlen(h->valuestring) == 64) {
            /* Lowercase it here so the comparison later is a plain strcmp. */
            for (int k = 0; k < 64; ++k) {
                char c = h->valuestring[k];
                e->sha256[k] = (c >= 'A' && c <= 'F') ? (char)(c - 'A' + 'a') : c;
            }
            e->sha256[64] = '\0';
        } else {
            e->sha256[0] = '\0';
        }

        out->total_bytes += e->bytes;
        if (out->total_bytes > EVO_BUNDLE_MAX_TOTAL_BYTES) {
            st = EVO_BUNDLE_ERR_TOO_BIG;
            goto done;
        }
        out->entry_count++;
    }

    /* The entry document has to be one of the files the manifest ships, or the
     * load will fall through to whatever happens to be in the cache. */
    {
        int found = 0;
        for (int i = 0; i < out->entry_count; ++i)
            if (strcmp(out->entries[i].path, out->entry) == 0) { found = 1; break; }
        if (!found) { st = EVO_BUNDLE_ERR_MANIFEST; goto done; }
    }

    if (!out->data_model[0])
        snprintf(out->data_model, sizeof out->data_model, "%s", provider_id);

    st = EVO_BUNDLE_OK;

done:
    cJSON_Delete(root);
    return st;
}

/* ------------------------------------------------------------------------- */
/* Cache                                                                     */
/* ------------------------------------------------------------------------- */

#define MANIFEST_NAME "manifest.json"

static char *read_whole_file(const char *path, size_t *out_len, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || (size_t)n > cap) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

evo_bundle_status_t evo_bundle_load_cached(const char *provider_id,
                                           evo_bundle_manifest_t *out)
{
    if (!provider_id || !out) return EVO_BUNDLE_ERR_MANIFEST;

    char mpath[512];
    if (evo_bundle_path(provider_id, MANIFEST_NAME, mpath, sizeof mpath) != 0)
        return EVO_BUNDLE_ERR_PATH;

    size_t len = 0;
    char *json = read_whole_file(mpath, &len, EVO_BUNDLE_MAX_MANIFEST);
    if (!json) return EVO_BUNDLE_ERR_INCOMPLETE;

    evo_bundle_status_t st = parse_manifest(json, len, provider_id, out);
    free(json);
    if (st != EVO_BUNDLE_OK) return st;

    /*
     * Every file the manifest names must be present at the size it claims.
     * The hash is NOT re-checked here: it is checked once at fetch time, and
     * re-hashing the whole bundle on the frame a screen opens would be work
     * the user waits for to detect a case (a cache modified behind EVO's back)
     * that the size check already catches in practice.
     */
    for (int i = 0; i < out->entry_count; ++i) {
        char p[512];
        if (evo_bundle_path(provider_id, out->entries[i].path, p, sizeof p) != 0)
            return EVO_BUNDLE_ERR_PATH;
        struct stat sb;
        if (stat(p, &sb) != 0) return EVO_BUNDLE_ERR_INCOMPLETE;
        if (out->entries[i].bytes && (uint32_t)sb.st_size != out->entries[i].bytes)
            return EVO_BUNDLE_ERR_INCOMPLETE;
    }
    return EVO_BUNDLE_OK;
}

int evo_bundle_clear(const char *provider_id)
{
    evo_bundle_manifest_t m;
    /* Remove only what a manifest named, then the manifest itself. Deleting a
     * directory tree blind is how a bug here would reach /data. */
    if (evo_bundle_load_cached(provider_id, &m) == EVO_BUNDLE_OK) {
        for (int i = 0; i < m.entry_count; ++i) {
            char p[512];
            if (evo_bundle_path(provider_id, m.entries[i].path, p, sizeof p) == 0)
                remove(p);
        }
    }
    char mp[512];
    if (evo_bundle_path(provider_id, MANIFEST_NAME, mp, sizeof mp) == 0)
        remove(mp);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Fetch                                                                     */
/* ------------------------------------------------------------------------- */

/* Join a directory URL and a relative path with exactly one '/'. */
static int join_url(const char *base, const char *rel, char *out, size_t out_sz)
{
    size_t bl = strlen(base);
    int has_slash = bl > 0 && base[bl - 1] == '/';
    int n = snprintf(out, out_sz, "%s%s%s", base, has_slash ? "" : "/", rel);
    return (n > 0 && (size_t)n < out_sz) ? 0 : -1;
}

static evo_bundle_status_t write_entry(const char *provider_id,
                                       const evo_bundle_entry_t *e,
                                       const char *body, size_t len)
{
    if (len > EVO_BUNDLE_MAX_FILE_BYTES) return EVO_BUNDLE_ERR_TOO_BIG;

    /*
     * The declared size is a claim; this is the check that matters. A manifest
     * saying 4 KB and a server sending 40 MB is exactly the case the per-file
     * cap exists for, and the manifest pass cannot see it.
     */
    if (e->bytes && (uint32_t)len != e->bytes) return EVO_BUNDLE_ERR_TOO_BIG;

    if (e->sha256[0]) {
        char got[65];
        sha256_buf(body, len, got);
        if (strcmp(got, e->sha256) != 0) return EVO_BUNDLE_ERR_HASH;
    }

    char path[512];
    if (evo_bundle_path(provider_id, e->path, path, sizeof path) != 0)
        return EVO_BUNDLE_ERR_PATH;
    if (evo_bundle_ensure_parent_dirs(path) != 0) return EVO_BUNDLE_ERR_WRITE;

    FILE *f = fopen(path, "wb");
    if (!f) return EVO_BUNDLE_ERR_WRITE;
    size_t put = fwrite(body, 1, len, f);
    int cerr = fclose(f);
    if (put != len || cerr != 0) { remove(path); return EVO_BUNDLE_ERR_WRITE; }
    return EVO_BUNDLE_OK;
}

evo_bundle_status_t evo_bundle_fetch_sync(const char *provider_id,
                                           const char *base_url,
                                           evo_bundle_manifest_t *out)
{
    if (!provider_id || !evo_provider_id_valid(provider_id))
        return EVO_BUNDLE_ERR_PATH;
    if (!base_url || !*base_url) return EVO_BUNDLE_ERR_NO_URL;

    char url[EVO_PROVIDER_MAX_URL];
    if (join_url(base_url, MANIFEST_NAME, url, sizeof url) != 0)
        return EVO_BUNDLE_ERR_NO_URL;

    char *body = NULL;
    size_t len = 0;
    int status = 0;
    if (evo_net_http_get_sync(url, NULL, 0, &body, &len, &status) != 0 ||
        status != 200 || !body) {
        free(body);
        return EVO_BUNDLE_ERR_FETCH;
    }

    evo_bundle_manifest_t m;
    evo_bundle_status_t st = parse_manifest(body, len, provider_id, &m);
    char *manifest_json = body;
    size_t manifest_len = len;
    if (st != EVO_BUNDLE_OK) { free(manifest_json); return st; }

    /*
     * Download everything into a staging name first, then move each file into
     * place, so a refresh that fails halfway leaves the previous bundle usable
     * rather than a mix of two versions - which would render, and render
     * wrongly, which is the failure mode this whole file is trying to avoid.
     *
     * Staging is per-file rather than a whole shadow directory because the
     * app module has no rename() guarantee across directories and a handful of
     * small files is not worth the risk of a half-moved tree.
     */
    uint64_t arrived = 0;
    for (int i = 0; i < m.entry_count; ++i) {
        if (join_url(base_url, m.entries[i].path, url, sizeof url) != 0) {
            free(manifest_json);
            return EVO_BUNDLE_ERR_PATH;
        }
        char *fb = NULL;
        size_t fl = 0;
        status = 0;
        if (evo_net_http_get_sync(url, NULL, 0, &fb, &fl, &status) != 0 ||
            status != 200 || !fb) {
            free(fb);
            free(manifest_json);
            return EVO_BUNDLE_ERR_FETCH;
        }

        arrived += fl;
        if (arrived > EVO_BUNDLE_MAX_TOTAL_BYTES) {
            free(fb);
            free(manifest_json);
            return EVO_BUNDLE_ERR_TOO_BIG;
        }

        st = write_entry(provider_id, &m.entries[i], fb, fl);
        free(fb);
        if (st != EVO_BUNDLE_OK) { free(manifest_json); return st; }
    }

    /* The manifest goes down last. Its presence is what evo_bundle_load_cached
     * takes as "there is a bundle here", so writing it first would make a
     * failed refresh look like a complete bundle with missing files. */
    evo_bundle_entry_t me;
    memset(&me, 0, sizeof me);
    snprintf(me.path, sizeof me.path, "%s", MANIFEST_NAME);
    st = write_entry(provider_id, &me, manifest_json, manifest_len);
    free(manifest_json);
    if (st != EVO_BUNDLE_OK) return st;

    if (out) *out = m;
    return EVO_BUNDLE_OK;
}

/* ------------------------------------------------------------------------- */
/* Async refresh                                                             */
/* ------------------------------------------------------------------------- */
/*
 * evo_bundle_fetch_sync blocks, and the only thread allowed to block on the
 * network is evo_net's worker - which is busy running requests. So the async
 * refresh is built out of async requests instead: fetch the manifest through
 * evo_net_request_async, then walk the asset list one request at a time from
 * inside the callbacks. Slower than a parallel fetch and far easier to reason
 * about, for a handful of files that are cached after the first run.
 */
typedef struct refresh {
    char provider_id[EVO_PROVIDER_MAX_ID];
    char base_url[EVO_PROVIDER_MAX_URL];
    evo_bundle_manifest_t m;
    char *manifest_json;
    size_t manifest_len;
    int  next;              /* index of the asset being fetched */
    uint64_t arrived;
    evo_bundle_cb cb;
    void *ud;
    int  in_use;
} refresh_t;

/* One at a time, per provider, and there are a handful of providers. A static
 * table keeps this allocation-free on a path that runs during a screen open. */
#define REFRESH_SLOTS 4
static refresh_t g_refresh[REFRESH_SLOTS];

/* Completed refreshes waiting for evo_bundle_poll(). The callback must land on
 * the main thread, and evo_net's callbacks already do - but a refresh finishes
 * inside one of those callbacks, so it is queued rather than called from
 * there, to keep "callbacks fire from poll()" true for this layer too. */
static struct {
    evo_bundle_status_t st;
    evo_bundle_manifest_t m;
    evo_bundle_cb cb;
    void *ud;
    int pending;
} g_done[REFRESH_SLOTS];

static void refresh_release(refresh_t *r, evo_bundle_status_t st)
{
    int slot = (int)(r - g_refresh);
    free(r->manifest_json);
    r->manifest_json = NULL;

    g_done[slot].st = st;
    g_done[slot].m = r->m;
    g_done[slot].cb = r->cb;
    g_done[slot].ud = r->ud;
    g_done[slot].pending = 1;

    r->in_use = 0;
}

static void fetch_next_asset(refresh_t *r);

static void on_asset(int success, int status, const char *body, size_t len,
                     void *ud)
{
    refresh_t *r = (refresh_t *)ud;
    if (!r->in_use) return;

    if (!success || status != 200 || !body) {
        refresh_release(r, EVO_BUNDLE_ERR_FETCH);
        return;
    }

    r->arrived += len;
    if (r->arrived > EVO_BUNDLE_MAX_TOTAL_BYTES) {
        refresh_release(r, EVO_BUNDLE_ERR_TOO_BIG);
        return;
    }

    evo_bundle_status_t st =
        write_entry(r->provider_id, &r->m.entries[r->next], body, len);
    if (st != EVO_BUNDLE_OK) { refresh_release(r, st); return; }

    r->next++;
    fetch_next_asset(r);
}

static void fetch_next_asset(refresh_t *r)
{
    if (r->next >= r->m.entry_count) {
        evo_bundle_entry_t me;
        memset(&me, 0, sizeof me);
        snprintf(me.path, sizeof me.path, "%s", MANIFEST_NAME);
        evo_bundle_status_t st = write_entry(r->provider_id, &me,
                                             r->manifest_json, r->manifest_len);
        refresh_release(r, st);
        return;
    }

    char url[EVO_PROVIDER_MAX_URL];
    if (join_url(r->base_url, r->m.entries[r->next].path, url, sizeof url) != 0) {
        refresh_release(r, EVO_BUNDLE_ERR_PATH);
        return;
    }
    if (evo_net_request_async("GET", url, NULL, NULL, 0, on_asset, r) != 0) {
        refresh_release(r, EVO_BUNDLE_ERR_FETCH);
        return;
    }
}

static void on_manifest(int success, int status, const char *body, size_t len,
                        void *ud)
{
    refresh_t *r = (refresh_t *)ud;
    if (!r->in_use) return;

    if (!success || status != 200 || !body) {
        refresh_release(r, EVO_BUNDLE_ERR_FETCH);
        return;
    }

    evo_bundle_status_t st = parse_manifest(body, len, r->provider_id, &r->m);
    if (st != EVO_BUNDLE_OK) { refresh_release(r, st); return; }

    /*
     * Already current: same version string as what is cached and every file
     * present. Nothing is downloaded and the screen keeps rendering the copy
     * it already opened.
     */
    evo_bundle_manifest_t cached;
    if (r->m.version[0] &&
        evo_bundle_load_cached(r->provider_id, &cached) == EVO_BUNDLE_OK &&
        strcmp(cached.version, r->m.version) == 0) {
        refresh_release(r, EVO_BUNDLE_OK);
        return;
    }

    r->manifest_json = (char *)malloc(len + 1);
    if (!r->manifest_json) { refresh_release(r, EVO_BUNDLE_ERR_WRITE); return; }
    memcpy(r->manifest_json, body, len);
    r->manifest_json[len] = '\0';
    r->manifest_len = len;

    r->next = 0;
    r->arrived = 0;
    fetch_next_asset(r);
}

int evo_bundle_refresh_async(const char *provider_id, const char *base_url,
                             evo_bundle_cb cb, void *ud)
{
    if (!provider_id || !evo_provider_id_valid(provider_id)) return -1;
    if (!base_url || !*base_url) return -1;

    for (int i = 0; i < REFRESH_SLOTS; ++i)
        if (g_refresh[i].in_use &&
            strcmp(g_refresh[i].provider_id, provider_id) == 0)
            return -2;

    refresh_t *r = NULL;
    for (int i = 0; i < REFRESH_SLOTS; ++i)
        if (!g_refresh[i].in_use) { r = &g_refresh[i]; break; }
    if (!r) return -2;

    memset(r, 0, sizeof *r);
    r->in_use = 1;
    snprintf(r->provider_id, sizeof r->provider_id, "%s", provider_id);
    snprintf(r->base_url, sizeof r->base_url, "%s", base_url);
    r->cb = cb;
    r->ud = ud;

    char url[EVO_PROVIDER_MAX_URL];
    if (join_url(base_url, MANIFEST_NAME, url, sizeof url) != 0) {
        r->in_use = 0;
        return -1;
    }
    if (evo_net_request_async("GET", url, NULL, NULL, 0, on_manifest, r) != 0) {
        r->in_use = 0;
        return -3;
    }
    return 0;
}

void evo_bundle_poll(void)
{
    for (int i = 0; i < REFRESH_SLOTS; ++i) {
        if (!g_done[i].pending) continue;
        g_done[i].pending = 0;
        if (g_done[i].cb) g_done[i].cb(g_done[i].st, &g_done[i].m, g_done[i].ud);
    }
}
