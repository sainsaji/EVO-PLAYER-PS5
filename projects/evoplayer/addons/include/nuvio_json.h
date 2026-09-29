/*
 * nuvio_json.h — a small streaming JSON writer.
 *
 * The bundled cJSON's printer is a flat-object helper for Emby's login body:
 * no nesting, no string escaping, integers only, and a fixed 1 KB buffer. The
 * progress store and the account RPCs need nested arrays, 13-digit
 * millisecond timestamps and titles with quotes in them, so they write JSON
 * through this instead.
 *
 * Every value call takes the key it is written under, or NULL inside an array.
 * Errors (allocation, nesting too deep) are sticky: nuvio_jw_finish() returns
 * NULL and the caller has nothing to free.
 */
#ifndef NUVIO_JSON_H
#define NUVIO_JSON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NUVIO_JW_MAX_DEPTH 16

typedef struct nuvio_jw {
    char  *buf;
    size_t len;
    size_t cap;
    int    err;
    int    depth;
    int    count[NUVIO_JW_MAX_DEPTH];   /* values written at each depth */
} nuvio_jw_t;

void  nuvio_jw_init(nuvio_jw_t *w);
void  nuvio_jw_obj(nuvio_jw_t *w, const char *key);     /* begin object */
void  nuvio_jw_arr(nuvio_jw_t *w, const char *key);     /* begin array  */
void  nuvio_jw_end_obj(nuvio_jw_t *w);
void  nuvio_jw_end_arr(nuvio_jw_t *w);
void  nuvio_jw_str(nuvio_jw_t *w, const char *key, const char *value);
void  nuvio_jw_i64(nuvio_jw_t *w, const char *key, int64_t value);
void  nuvio_jw_bool(nuvio_jw_t *w, const char *key, int value);
void  nuvio_jw_null(nuvio_jw_t *w, const char *key);

/* The document, malloc'd, or NULL on any earlier error. Resets `w`. */
char *nuvio_jw_finish(nuvio_jw_t *w);

#ifdef __cplusplus
}
#endif

#endif /* NUVIO_JSON_H */
