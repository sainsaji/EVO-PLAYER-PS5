/*
 * nuvio_progress.c — see nuvio_progress.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nuvio_progress.h"
#include "nuvio_json.h"
#include "cJSON.h"

int nuvio_progress_init(nuvio_progress_t *p)
{
    memset(p, 0, sizeof *p);
    p->entries = (nuvio_progress_entry_t *)calloc(NUVIO_PROGRESS_MAX, sizeof *p->entries);
    return p->entries ? 0 : -1;
}

void nuvio_progress_free(nuvio_progress_t *p)
{
    if (!p) return;
    free(p->entries);
    memset(p, 0, sizeof *p);
}

void nuvio_progress_key(const char *content_id, int season, int episode,
                        char *out, size_t cap)
{
    if (season > 0 || episode > 0)
        snprintf(out, cap, "%s_s%de%d", content_id, season, episode);
    else
        snprintf(out, cap, "%s", content_id);
}

nuvio_progress_entry_t *nuvio_progress_find_key(nuvio_progress_t *p, const char *key)
{
    for (int i = 0; i < p->count; ++i)
        if (!strcmp(p->entries[i].progress_key, key)) return &p->entries[i];
    return NULL;
}

nuvio_progress_entry_t *nuvio_progress_find_video(nuvio_progress_t *p, const char *video_id)
{
    nuvio_progress_entry_t *best = NULL;
    for (int i = 0; i < p->count; ++i)
        if (!strcmp(p->entries[i].video_id, video_id) &&
            (!best || p->entries[i].last_watched_ms > best->last_watched_ms))
            best = &p->entries[i];
    return best;
}

nuvio_progress_entry_t *nuvio_progress_upsert(nuvio_progress_t *p,
                                              const nuvio_progress_entry_t *e,
                                              int mark_dirty)
{
    if (!p->entries || !e->progress_key[0]) return NULL;

    nuvio_progress_entry_t *slot = nuvio_progress_find_key(p, e->progress_key);
    if (slot) {
        if (e->last_watched_ms < slot->last_watched_ms) return slot;
        /* Keep display fields a remote record does not carry. */
        char name[sizeof slot->name], poster[sizeof slot->poster];
        memcpy(name, slot->name, sizeof name);
        memcpy(poster, slot->poster, sizeof poster);
        *slot = *e;
        if (!slot->name[0])   memcpy(slot->name, name, sizeof name);
        if (!slot->poster[0]) memcpy(slot->poster, poster, sizeof poster);
    } else {
        if (p->count < NUVIO_PROGRESS_MAX) {
            slot = &p->entries[p->count++];
        } else {
            slot = &p->entries[0];
            for (int i = 1; i < p->count; ++i)
                if (p->entries[i].last_watched_ms < slot->last_watched_ms)
                    slot = &p->entries[i];
        }
        *slot = *e;
    }
    slot->dirty = mark_dirty ? 1 : 0;
    return slot;
}

int nuvio_progress_in_progress(const nuvio_progress_entry_t *e)
{
    if (e->duration_ms <= 0) return 0;
    double f = (double)e->position_ms / (double)e->duration_ms;
    return f >= NUVIO_PROGRESS_STARTED && f < NUVIO_PROGRESS_COMPLETED;
}

int nuvio_progress_continue(const nuvio_progress_t *p, int *out, int cap)
{
    int n = 0;
    for (int i = 0; i < p->count; ++i) {
        const nuvio_progress_entry_t *e = &p->entries[i];
        if (!nuvio_progress_in_progress(e)) continue;

        /* One row per series: keep the most recently watched episode. */
        int replaced = 0, skip = 0;
        for (int j = 0; j < n; ++j) {
            const nuvio_progress_entry_t *o = &p->entries[out[j]];
            if (strcmp(o->content_id, e->content_id) != 0) continue;
            if (e->last_watched_ms > o->last_watched_ms) { out[j] = i; replaced = 1; }
            else skip = 1;
            break;
        }
        if (replaced || skip) continue;
        if (n < cap) out[n++] = i;
    }
    /* most recent first */
    for (int i = 1; i < n; ++i) {
        int v = out[i], j = i - 1;
        while (j >= 0 && p->entries[out[j]].last_watched_ms < p->entries[v].last_watched_ms) {
            out[j + 1] = out[j];
            --j;
        }
        out[j + 1] = v;
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* Persistence                                                               */
/* ------------------------------------------------------------------------- */

static void get_s(const cJSON *o, const char *k, char *out, size_t cap)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && v->valuestring) snprintf(out, cap, "%s", v->valuestring);
    else out[0] = '\0';
}

static int64_t get_n(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (int64_t)v->valuedouble : 0;
}

int nuvio_progress_load(nuvio_progress_t *p, const char *path)
{
    p->count = 0;
    p->last_pull_ms = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) { fclose(f); return 0; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';

    cJSON *root = cJSON_ParseWithLength(buf, got);
    free(buf);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return 0; }

    p->last_pull_ms = get_n(root, "last_pull");
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "entries");
    if (cJSON_IsArray(arr)) {
        for (const cJSON *o = arr->child; o && p->count < NUVIO_PROGRESS_MAX; o = o->next) {
            nuvio_progress_entry_t *e = &p->entries[p->count];
            memset(e, 0, sizeof *e);
            get_s(o, "progress_key", e->progress_key, sizeof e->progress_key);
            if (!e->progress_key[0]) continue;
            get_s(o, "content_id",   e->content_id,   sizeof e->content_id);
            get_s(o, "content_type", e->content_type, sizeof e->content_type);
            get_s(o, "video_id",     e->video_id,     sizeof e->video_id);
            get_s(o, "name",         e->name,         sizeof e->name);
            get_s(o, "poster",       e->poster,       sizeof e->poster);
            e->season          = (int)get_n(o, "season");
            e->episode         = (int)get_n(o, "episode");
            e->position_ms     = get_n(o, "position");
            e->duration_ms     = get_n(o, "duration");
            e->last_watched_ms = get_n(o, "last_watched");
            e->dirty           = (int)get_n(o, "dirty");
            p->count++;
        }
    }
    cJSON_Delete(root);
    return 0;
}

int nuvio_progress_save(const nuvio_progress_t *p, const char *path)
{
    nuvio_jw_t w;
    nuvio_jw_init(&w);
    nuvio_jw_obj(&w, NULL);
    nuvio_jw_i64(&w, "version", 1);
    nuvio_jw_i64(&w, "last_pull", p->last_pull_ms);
    nuvio_jw_arr(&w, "entries");
    for (int i = 0; i < p->count; ++i) {
        const nuvio_progress_entry_t *e = &p->entries[i];
        nuvio_jw_obj(&w, NULL);
        nuvio_jw_str(&w, "progress_key", e->progress_key);
        nuvio_jw_str(&w, "content_id",   e->content_id);
        nuvio_jw_str(&w, "content_type", e->content_type);
        nuvio_jw_str(&w, "video_id",     e->video_id);
        if (e->name[0])   nuvio_jw_str(&w, "name",   e->name);
        if (e->poster[0]) nuvio_jw_str(&w, "poster", e->poster);
        nuvio_jw_i64(&w, "season",       e->season);
        nuvio_jw_i64(&w, "episode",      e->episode);
        nuvio_jw_i64(&w, "position",     e->position_ms);
        nuvio_jw_i64(&w, "duration",     e->duration_ms);
        nuvio_jw_i64(&w, "last_watched", e->last_watched_ms);
        nuvio_jw_i64(&w, "dirty",        e->dirty);
        nuvio_jw_end_obj(&w);
    }
    nuvio_jw_end_arr(&w);
    nuvio_jw_end_obj(&w);

    char *text = nuvio_jw_finish(&w);
    if (!text) return -1;

    /* Written in place: the app module has no reliable rename()
     * (evo_provider_bundle.c), so there is no write-then-swap. A torn write
     * loses progress, which load() treats as an empty store. */
    FILE *f = fopen(path, "wb");
    if (!f) { free(text); return -1; }
    size_t len = strlen(text);
    size_t wrote = fwrite(text, 1, len, f);
    fclose(f);
    free(text);
    return wrote == len ? 0 : -1;
}
