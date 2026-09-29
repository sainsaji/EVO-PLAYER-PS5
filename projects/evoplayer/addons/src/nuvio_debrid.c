/*
 * nuvio_debrid.c — see nuvio_debrid.h.
 */
#include <stdio.h>
#include <string.h>

#include "nuvio_debrid.h"

static int lower_c(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int is_digit_c(int c) { return c >= '0' && c <= '9'; }
static int is_alnum_c(int c)
{
    c = lower_c(c);
    return is_digit_c(c) || (c >= 'a' && c <= 'z');
}

static int eq(const char *a, const char *b, int ci)
{
    if (!ci) return strcmp(a, b) == 0;
    for (; *a && *b; ++a, ++b)
        if (lower_c((unsigned char)*a) != lower_c((unsigned char)*b)) return 0;
    return *a == *b;
}

static int ends_with(const char *s, const char *suffix, int ci)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && eq(s + n - m, suffix, ci);
}

/* normalizedPath(): trim, '\' -> '/', no leading '/'. */
static void normalize(const char *in, char *out, size_t cap)
{
    while (*in == ' ' || *in == '\t') ++in;
    while (*in == '/' || *in == '\\') ++in;
    size_t o = 0;
    for (; *in && o + 1 < cap; ++in) out[o++] = (*in == '\\') ? '/' : *in;
    while (o && (out[o - 1] == ' ' || out[o - 1] == '\t')) --o;
    out[o] = '\0';
}

static const char *basename_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

int nuvio_debrid_is_video_name(const char *name)
{
    static const char *const EXT[] = {
        ".mp4", ".mkv", ".webm", ".avi", ".mov", ".m4v", ".ts", ".m2ts", ".wmv", ".flv"
    };
    for (size_t i = 0; i < sizeof EXT / sizeof EXT[0]; ++i)
        if (ends_with(name, EXT[i], 1)) return 1;
    return 0;
}

/* Read a run of digits at *p as a number, allowing leading zeros; advances. */
static int read_num(const char **p, int *out)
{
    const char *s = *p;
    if (!is_digit_c((unsigned char)*s)) return 0;
    int v = 0, n = 0;
    while (is_digit_c((unsigned char)*s) && n < 6) { v = v * 10 + (*s - '0'); ++s; ++n; }
    *p = s;
    *out = v;
    return 1;
}

int nuvio_debrid_episode_match(const char *name, int season, int episode)
{
    if (season <= 0 && episode <= 0) return 0;
    const char *base = basename_of(name);
    for (const char *p = base; *p; ++p) {
        /* (?<![a-z0-9]) */
        if (p > base && is_alnum_c((unsigned char)p[-1])) continue;

        const char *q = p;
        int s = 0, e = 0;
        if (lower_c((unsigned char)*q) == 's') {                 /* s0*Se0*E */
            ++q;
            if (!read_num(&q, &s) || lower_c((unsigned char)*q) != 'e') continue;
            ++q;
            if (!read_num(&q, &e)) continue;
        } else if (is_digit_c((unsigned char)*q)) {                /* 0*Sx0*E */
            if (!read_num(&q, &s) || lower_c((unsigned char)*q) != 'x') continue;
            ++q;
            if (!read_num(&q, &e)) continue;
        } else {
            continue;
        }
        /* (?![0-9]) - read_num consumed every digit, so this holds unless it
         * stopped at its length cap. */
        if (is_digit_c((unsigned char)*q)) continue;
        if (s == season && e == episode) return 1;
    }
    return 0;
}

/* Every playable file matching `name`, per NuvioTV's matchingFiles(). */
static int matching(const nuvio_debrid_file_t *f, int count, const char *name,
                    int *out, int cap)
{
    char want[512];
    normalize(name, want, sizeof want);
    if (!want[0]) return 0;
    int has_slash = strchr(want, '/') != NULL;

    for (int ci = 0; ci <= 1; ++ci) {
        int n = 0;
        for (int i = 0; i < count; ++i) {
            if (!f[i].is_video) continue;
            char path[512];
            normalize(f[i].path, path, sizeof path);
            int hit = eq(path, want, ci);
            if (!hit && has_slash) {
                char suffix[520];
                snprintf(suffix, sizeof suffix, "/%s", want);
                hit = ends_with(path, suffix, ci);
            }
            if (hit && n < cap) out[n++] = i;
        }
        if (n) return n;
    }
    const char *wb = basename_of(want);
    for (int ci = 0; ci <= 1; ++ci) {
        int n = 0;
        for (int i = 0; i < count; ++i) {
            if (!f[i].is_video) continue;
            char path[512];
            normalize(f[i].path, path, sizeof path);
            if (eq(basename_of(path), wb, ci) && n < cap) out[n++] = i;
        }
        if (n) return n;
    }
    return 0;
}

int nuvio_debrid_select(const nuvio_debrid_file_t *files, int count,
                        const nuvio_magnet_hints_t *h)
{
    int playable = 0;
    for (int i = 0; i < count; ++i) if (files[i].is_video) ++playable;
    if (!playable) return -1;

    int m[4];
    if (h && h->filename[0]) {
        int n = matching(files, count, h->filename, m, 4);
        if (n) return n == 1 ? m[0] : -1;
    }

    int want_episode = h && h->season > 0 && h->episode > 0;
    if (want_episode) {
        int n = 0, only = -1;
        for (int i = 0; i < count; ++i) {
            if (!files[i].is_video) continue;
            char path[512];
            normalize(files[i].path, path, sizeof path);
            if (nuvio_debrid_episode_match(path, h->season, h->episode)) { ++n; only = i; }
        }
        if (n) return n == 1 ? only : -1;
    }

    if ((h && h->filename[0]) || want_episode) return -1;

    if (h && h->file_idx >= 0 && h->file_idx < count && files[h->file_idx].is_video)
        return h->file_idx;
    if (h && h->file_idx >= 0) {
        /* fileIdx names a file that is not a video (or not there): NuvioTV
         * returns null here rather than guessing. */
        return -1;
    }

    int best = -1;
    for (int i = 0; i < count; ++i)
        if (files[i].is_video && (best < 0 || files[i].size > files[best].size)) best = i;
    return best;
}
