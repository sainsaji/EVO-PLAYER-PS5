/*
 * evo_hls_variants.c - see evo_hls_variants.h.
 */
#include "evo_hls_variants.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "evo_net.h"

/* ------------------------------------------------------------------------- */
/* URL join                                                                   */
/* ------------------------------------------------------------------------- */

/* Length of a leading "scheme:", or 0 if `u` does not start with one. */
static size_t scheme_len(const char *u)
{
    if (!u || !isalpha((unsigned char)u[0])) return 0;
    size_t i = 1;
    while (isalnum((unsigned char)u[i]) || u[i] == '+' || u[i] == '-' || u[i] == '.') i++;
    return u[i] == ':' ? i + 1 : 0;
}

/*
 * Removes "." and ".." segments from the path part of `url` in place. The path
 * starts after "scheme://authority" and ends at the query or fragment.
 */
static void remove_dot_segments(char *url)
{
    size_t bs = scheme_len(url);
    char *p = url + bs;
    if (p[0] == '/' && p[1] == '/') p += 2 + strcspn(p + 2, "/?#");
    char *path = p;
    const size_t total = strlen(path);                       /* path + query + fragment */
    size_t path_len = strcspn(path, "?#");
    if (path_len == 0 || path[0] != '/') return;

    char tail[1024];
    snprintf(tail, sizeof tail, "%s", path + path_len);     /* query + fragment */

    char res[1024];
    size_t o = 0;
    const char *i = path;
    const char *end = path + path_len;
    while (i < end && o + 2 < sizeof res) {
        if (i[0] == '/' && i[1] == '.' && (i + 2 >= end || i[2] == '/')) {
            i += 2;                                          /* "/." */
            if (i >= end) res[o++] = '/';
            continue;
        }
        if (i[0] == '/' && i[1] == '.' && i[2] == '.' && (i + 3 >= end || i[3] == '/')) {
            while (o > 0 && res[o - 1] != '/') o--;          /* "/.." : drop the last segment */
            if (o > 0) o--;
            i += 3;
            if (i >= end) res[o++] = '/';
            continue;
        }
        res[o++] = *i++;                                     /* the '/' */
        while (i < end && *i != '/' && o + 1 < sizeof res) res[o++] = *i++;
    }
    res[o] = '\0';
    /* Never longer than what it replaces, so `total + 1` is room enough. */
    snprintf(path, total + 1, "%s%s", res, tail);
}

int evo_hls_join_url(const char *base, const char *ref, char *out, size_t out_size)
{
    if (!ref || !out || out_size == 0) return -1;
    out[0] = '\0';

    if (scheme_len(ref)) {                                   /* already absolute */
        int n = snprintf(out, out_size, "%s", ref);
        return (n < 0 || (size_t)n >= out_size) ? -1 : 0;
    }

    size_t bs = scheme_len(base);
    if (!bs) return -1;

    /* base = scheme:[//authority]path[?query][#fragment] */
    const char *p = base + bs;
    size_t auth_end = 0;                                     /* offset past "//authority" */
    if (p[0] == '/' && p[1] == '/') auth_end = 2 + strcspn(p + 2, "/?#");
    const char *path = p + auth_end;
    size_t path_len = strcspn(path, "?#");

    char tmp[2048];
    int n;
    if (ref[0] == '/' && ref[1] == '/') {                    /* //host/x: scheme only */
        n = snprintf(tmp, sizeof tmp, "%.*s%s", (int)bs, base, ref);
    } else if (ref[0] == '/') {                              /* /x: scheme + authority */
        n = snprintf(tmp, sizeof tmp, "%.*s%s", (int)(bs + auth_end), base, ref);
    } else if (ref[0] == '?') {                              /* ?q: same path */
        n = snprintf(tmp, sizeof tmp, "%.*s%s", (int)(bs + auth_end + path_len), base, ref);
    } else if (ref[0] == '\0') {
        n = snprintf(tmp, sizeof tmp, "%.*s", (int)(bs + auth_end + path_len), base);
    } else {                                                 /* relative to base's directory */
        size_t dir = path_len;
        while (dir > 0 && path[dir - 1] != '/') dir--;
        if (dir == 0 && auth_end) {                          /* "https://host" + "x" */
            n = snprintf(tmp, sizeof tmp, "%.*s/%s", (int)(bs + auth_end), base, ref);
        } else {
            n = snprintf(tmp, sizeof tmp, "%.*s%s", (int)(bs + auth_end + dir), base, ref);
        }
    }
    if (n < 0 || (size_t)n >= sizeof tmp) return -1;

    remove_dot_segments(tmp);
    n = snprintf(out, out_size, "%s", tmp);
    return (n < 0 || (size_t)n >= out_size) ? -1 : 0;
}

/* ------------------------------------------------------------------------- */
/* Master playlist                                                            */
/* ------------------------------------------------------------------------- */

/*
 * Reads attribute `key` out of an attribute list ("BANDWIDTH=1,CODECS=\"a,b\"").
 * A quoted value may contain commas. Returns 1 and fills `val`, or 0.
 */
static int attr_get(const char *list, const char *key, char *val, size_t val_size)
{
    const size_t klen = strlen(key);
    const char *p = list;
    while (*p) {
        while (*p == ',' || *p == ' ') p++;
        const char *name = p;
        while (*p && *p != '=' && *p != ',') p++;
        size_t nlen = (size_t)(p - name);
        if (*p != '=') continue;
        p++;

        const char *v;
        size_t vlen;
        if (*p == '"') {
            v = ++p;
            while (*p && *p != '"') p++;
            vlen = (size_t)(p - v);
            if (*p == '"') p++;
        } else {
            v = p;
            while (*p && *p != ',') p++;
            vlen = (size_t)(p - v);
        }
        if (nlen == klen && strncmp(name, key, klen) == 0) {
            snprintf(val, val_size, "%.*s", (int)vlen, v);
            return 1;
        }
    }
    return 0;
}

static void copy_codecs(const char *codecs, evo_stream_choice_t *c, int *has_video, int *has_audio)
{
    *has_video = *has_audio = 0;
    const char *p = codecs;
    while (*p) {
        const char *tok = p;
        while (*p && *p != ',') p++;
        size_t n = (size_t)(p - tok);
        if (*p == ',') p++;

        if (n >= 4 && (!strncmp(tok, "avc1", 4) || !strncmp(tok, "avc3", 4))) {
            *has_video = 1; snprintf(c->video_codec, sizeof c->video_codec, "h264");
        } else if (n >= 4 && (!strncmp(tok, "hvc1", 4) || !strncmp(tok, "hev1", 4))) {
            *has_video = 1; snprintf(c->video_codec, sizeof c->video_codec, "hevc");
        } else if (n >= 3 && !strncmp(tok, "vp9", 3)) {
            *has_video = 1; snprintf(c->video_codec, sizeof c->video_codec, "vp9");
        } else if (n >= 4 && !strncmp(tok, "vp09", 4)) {
            *has_video = 1; snprintf(c->video_codec, sizeof c->video_codec, "vp9");
        } else if (n >= 4 && !strncmp(tok, "av01", 4)) {
            *has_video = 1; snprintf(c->video_codec, sizeof c->video_codec, "av1");
        } else if (n >= 4 && !strncmp(tok, "mp4a", 4)) {
            *has_audio = 1; snprintf(c->audio_codec, sizeof c->audio_codec, "aac");
        } else if (n >= 4 && !strncmp(tok, "ac-3", 4)) {
            *has_audio = 1; snprintf(c->audio_codec, sizeof c->audio_codec, "ac3");
        } else if (n >= 4 && !strncmp(tok, "ec-3", 4)) {
            *has_audio = 1; snprintf(c->audio_codec, sizeof c->audio_codec, "eac3");
        } else if (n >= 4 && !strncmp(tok, "opus", 4)) {
            *has_audio = 1; snprintf(c->audio_codec, sizeof c->audio_codec, "opus");
        }
    }
}

/* Best first: taller, then faster; audio-only (height 0) falls to the end. */
static int variant_better(const evo_stream_choice_t *a, const evo_stream_choice_t *b)
{
    if (a->height != b->height) return a->height > b->height;
    return a->bitrate_bps > b->bitrate_bps;
}

int evo_hls_parse_master(const char *body, size_t len, const char *base_url,
                         evo_stream_choice_t *out, int max)
{
    if (!body || !len || !out || max <= 0) return 0;

    int n = 0;
    char attrs[512];
    int have_inf = 0;

    const char *p = body;
    const char *end = body + len;
    while (p < end) {
        const char *eol = p;
        while (eol < end && *eol != '\n' && *eol != '\r') eol++;
        size_t ll = (size_t)(eol - p);

        if (ll > 0) {
            if (ll >= 18 && !strncmp(p, "#EXT-X-STREAM-INF:", 18)) {
                snprintf(attrs, sizeof attrs, "%.*s", (int)(ll - 18), p + 18);
                have_inf = 1;
            } else if (p[0] == '#') {
                /* Any other tag - #EXT-X-MEDIA, #EXT-X-I-FRAME-STREAM-INF, a
                 * comment - is not a variant. Keep waiting for the URI a
                 * pending STREAM-INF is owed. */
            } else if (have_inf) {
                have_inf = 0;

                char ref[1024];
                snprintf(ref, sizeof ref, "%.*s", (int)ll, p);

                evo_stream_choice_t c;
                memset(&c, 0, sizeof c);
                if (evo_hls_join_url(base_url, ref, c.url, sizeof c.url) == 0) {
                    int dup = 0;
                    for (int i = 0; i < n; i++)
                        if (!strcmp(out[i].url, c.url)) dup = 1;

                    if (!dup) {
                        char v[256];
                        snprintf(c.container, sizeof c.container, "hls");
                        c.is_live = 1;
                        if (attr_get(attrs, "BANDWIDTH", v, sizeof v))
                            c.bitrate_bps = (int64_t)strtoll(v, NULL, 10);
                        if (attr_get(attrs, "AVERAGE-BANDWIDTH", v, sizeof v) && !c.bitrate_bps)
                            c.bitrate_bps = (int64_t)strtoll(v, NULL, 10);
                        if (attr_get(attrs, "RESOLUTION", v, sizeof v)) {
                            int w = 0, h = 0;
                            if (sscanf(v, "%dx%d", &w, &h) == 2) { c.width = w; c.height = h; }
                        }
                        int has_video = 0, has_audio = 0;
                        if (attr_get(attrs, "CODECS", v, sizeof v))
                            copy_codecs(v, &c, &has_video, &has_audio);

                        if (c.height > 0) {
                            snprintf(c.label, sizeof c.label, "%dp", c.height);
                        } else if (has_audio && !has_video) {
                            snprintf(c.label, sizeof c.label, "Audio only");
                        } else if (c.bitrate_bps >= 1000000) {
                            snprintf(c.label, sizeof c.label, "%.1f Mbps", (double)c.bitrate_bps / 1e6);
                        } else if (c.bitrate_bps > 0) {
                            snprintf(c.label, sizeof c.label, "%d kbps", (int)(c.bitrate_bps / 1000));
                        } else {
                            snprintf(c.label, sizeof c.label, "Stream %d", n + 1);
                        }

                        if (n < max) {
                            /* insertion, best first */
                            int at = n;
                            while (at > 0 && variant_better(&c, &out[at - 1])) {
                                out[at] = out[at - 1];
                                at--;
                            }
                            out[at] = c;
                            n++;
                        } else if (variant_better(&c, &out[max - 1])) {
                            int at = max - 1;
                            while (at > 0 && variant_better(&c, &out[at - 1])) {
                                out[at] = out[at - 1];
                                at--;
                            }
                            out[at] = c;
                        }
                    }
                }
            }
        }

        p = eol;
        while (p < end && (*p == '\n' || *p == '\r')) p++;
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* Fetch                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct {
    evo_hls_variants_cb cb;
    void *ud;
    char url[EVO_PROVIDER_MAX_URL];
} fetch_ctx_t;

static void on_master(int success, int status, const char *body, size_t body_len, void *ud)
{
    fetch_ctx_t *ctx = (fetch_ctx_t *)ud;
    evo_stream_choice_t variants[EVO_HLS_MAX_VARIANTS];
    int count = 0;

    if (success && status == 200 && body && body_len > 0)
        count = evo_hls_parse_master(body, body_len, ctx->url, variants, EVO_HLS_MAX_VARIANTS);

    if (ctx->cb)
        ctx->cb(count, count > 0 ? variants : NULL, ctx->ud);
    free(ctx);
}

int evo_hls_variants_fetch(const char *master_url, evo_hls_variants_cb cb, void *ud)
{
    if (!master_url || !master_url[0] || !cb) return -1;

    fetch_ctx_t *ctx = (fetch_ctx_t *)calloc(1, sizeof *ctx);
    if (!ctx) return -1;
    ctx->cb = cb;
    ctx->ud = ud;
    snprintf(ctx->url, sizeof ctx->url, "%s", master_url);

    int rc = evo_net_request_async("GET", master_url, NULL, NULL, 0, on_master, ctx);
    if (rc != 0) {
        free(ctx);
        return rc < 0 ? rc : -1;
    }
    return 0;
}
