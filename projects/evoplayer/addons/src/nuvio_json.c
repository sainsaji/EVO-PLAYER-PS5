/*
 * nuvio_json.c — see nuvio_json.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nuvio_json.h"

void nuvio_jw_init(nuvio_jw_t *w)
{
    memset(w, 0, sizeof *w);
}

static void put(nuvio_jw_t *w, const char *s, size_t n)
{
    if (w->err) return;
    if (w->len + n + 1 > w->cap) {
        size_t cap = w->cap ? w->cap : 256;
        while (cap < w->len + n + 1) cap *= 2;
        char *nb = (char *)realloc(w->buf, cap);
        if (!nb) { w->err = 1; return; }
        w->buf = nb;
        w->cap = cap;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

static void put_s(nuvio_jw_t *w, const char *s) { put(w, s, strlen(s)); }

static void put_escaped(nuvio_jw_t *w, const char *s)
{
    put(w, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; p && *p; ++p) {
        switch (*p) {
            case '"':  put(w, "\\\"", 2); break;
            case '\\': put(w, "\\\\", 2); break;
            case '\n': put(w, "\\n", 2);  break;
            case '\r': put(w, "\\r", 2);  break;
            case '\t': put(w, "\\t", 2);  break;
            default:
                if (*p < 0x20) {
                    char u[8];
                    snprintf(u, sizeof u, "\\u%04x", *p);
                    put_s(w, u);
                } else {
                    put(w, (const char *)p, 1);   /* UTF-8 passes through */
                }
        }
    }
    put(w, "\"", 1);
}

/* Comma and key before a value at the current depth. */
static void prefix(nuvio_jw_t *w, const char *key)
{
    if (w->err) return;
    if (w->depth > 0 && w->count[w->depth - 1]++ > 0) put(w, ",", 1);
    if (key) {
        put_escaped(w, key);
        put(w, ":", 1);
    }
}

static void jw_open(nuvio_jw_t *w, const char *key, const char *brace)
{
    prefix(w, key);
    if (w->depth >= NUVIO_JW_MAX_DEPTH) { w->err = 1; return; }
    put_s(w, brace);
    w->count[w->depth++] = 0;
}

static void jw_close(nuvio_jw_t *w, const char *brace)
{
    if (w->depth <= 0) { w->err = 1; return; }
    w->depth--;
    put_s(w, brace);
}

void nuvio_jw_obj(nuvio_jw_t *w, const char *key) { jw_open(w, key, "{"); }
void nuvio_jw_arr(nuvio_jw_t *w, const char *key) { jw_open(w, key, "["); }
void nuvio_jw_end_obj(nuvio_jw_t *w) { jw_close(w, "}"); }
void nuvio_jw_end_arr(nuvio_jw_t *w) { jw_close(w, "]"); }

void nuvio_jw_str(nuvio_jw_t *w, const char *key, const char *value)
{
    prefix(w, key);
    put_escaped(w, value ? value : "");
}

void nuvio_jw_i64(nuvio_jw_t *w, const char *key, int64_t value)
{
    char n[32];
    snprintf(n, sizeof n, "%lld", (long long)value);
    prefix(w, key);
    put_s(w, n);
}

void nuvio_jw_bool(nuvio_jw_t *w, const char *key, int value)
{
    prefix(w, key);
    put_s(w, value ? "true" : "false");
}

void nuvio_jw_null(nuvio_jw_t *w, const char *key)
{
    prefix(w, key);
    put_s(w, "null");
}

char *nuvio_jw_finish(nuvio_jw_t *w)
{
    char *out = NULL;
    if (!w->err && w->depth == 0 && w->buf) {
        out = w->buf;
    } else {
        free(w->buf);
    }
    memset(w, 0, sizeof *w);
    return out;
}
