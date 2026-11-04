/*
 * evo_web_url.c - typed text to a URL or a search (#237). See evo_web_url.h.
 * Uses only what the app module's libc offers (no strcasestr, no regex).
 */
#include "evo_web_url.h"

#include <stdio.h>
#include <string.h>

#define SEARCH_PREFIX "https://duckduckgo.com/?q="

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int starts_with_ci(const char *s, size_t n, const char *prefix)
{
    const size_t p = strlen(prefix);
    if (n < p) return 0;
    for (size_t i = 0; i < p; ++i)
        if (lower((unsigned char)s[i]) != prefix[i]) return 0;
    return 1;
}

/* Host part of s[0..n): up to the first '/', '?' or '#'. */
static size_t host_len(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n && s[i] != '/' && s[i] != '?' && s[i] != '#') ++i;
    return i;
}

/* a.b.c.d with an optional :port */
static int is_ipv4(const char *s, size_t n)
{
    int dots = 0, digits = 0;
    size_t i = 0;
    for (; i < n && s[i] != ':'; ++i) {
        if (s[i] == '.') {
            if (!digits) return 0;
            ++dots;
            digits = 0;
        } else if (s[i] >= '0' && s[i] <= '9') {
            if (++digits > 3) return 0;
        } else {
            return 0;
        }
    }
    return dots == 3 && digits > 0;
}

static void copy_cut(char *out, size_t cap, const char *a, size_t na, const char *b, size_t nb)
{
    if (!cap) return;
    size_t k = 0;
    for (size_t i = 0; i < na && k + 1 < cap; ++i) out[k++] = a[i];
    for (size_t i = 0; i < nb && k + 1 < cap; ++i) out[k++] = b[i];
    out[k] = 0;
}

int evo_web_normalize_input(const char *in, char *out, size_t cap)
{
    if (cap) out[0] = 0;
    if (!in) return EVO_WEB_INPUT_EMPTY;
    while (is_space(*in)) ++in;
    size_t n = strlen(in);
    while (n && is_space(in[n - 1])) --n;
    if (!n) return EVO_WEB_INPUT_EMPTY;

    if (starts_with_ci(in, n, "http://") || starts_with_ci(in, n, "https://") ||
        starts_with_ci(in, n, "about:")) {
        copy_cut(out, cap, in, n, "", 0);
        return EVO_WEB_INPUT_URL;
    }

    int has_space = 0;
    for (size_t i = 0; i < n; ++i)
        if (is_space(in[i])) has_space = 1;
    const size_t h = host_len(in, n);
    int has_dot = 0;
    for (size_t i = 0; i < h; ++i)
        if (in[i] == '.') has_dot = 1;
    const int local = starts_with_ci(in, h, "localhost") &&
                      (h == 9 || in[9] == ':');
    const int ip = is_ipv4(in, h);
    /* a host needs a dot inside it, not at either end */
    const int dotted = has_dot && in[0] != '.' && in[h - 1] != '.';

    if (!has_space && (dotted || local)) {
        const char *scheme = (ip || local) ? "http://" : "https://";
        copy_cut(out, cap, scheme, strlen(scheme), in, n);
        return EVO_WEB_INPUT_URL;
    }

    /* a search: form-encode the words */
    size_t k = 0;
    const size_t pre = strlen(SEARCH_PREFIX);
    for (size_t i = 0; i < pre && k + 1 < cap; ++i) out[k++] = SEARCH_PREFIX[i];
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char)in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            if (k + 1 >= cap) break;
            out[k++] = (char)c;
        } else if (c == ' ') {
            if (k + 1 >= cap) break;
            out[k++] = '+';
        } else {
            if (k + 3 >= cap) break;
            out[k++] = '%';
            out[k++] = hex[c >> 4];
            out[k++] = hex[c & 15];
        }
    }
    if (cap) out[k < cap ? k : cap - 1] = 0;
    return EVO_WEB_INPUT_SEARCH;
}

void evo_web_url_host(const char *url, char *out, size_t cap)
{
    if (!cap) return;
    out[0] = 0;
    if (!url) return;
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *at = NULL;
    size_t n = host_len(p, strlen(p));
    for (size_t i = 0; i < n; ++i)
        if (p[i] == '@') at = p + i;
    if (at) {   /* drop user:pass@ */
        n -= (size_t)(at + 1 - p);
        p = at + 1;
    }
    copy_cut(out, cap, p, n, "", 0);
}
