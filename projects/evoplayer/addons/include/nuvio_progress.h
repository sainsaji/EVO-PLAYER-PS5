/*
 * nuvio_progress.h — watch progress for the Nuvio provider.
 *
 * One entry per movie or episode, keyed and shaped exactly like NuvioTV's
 * (WatchProgressRepositoryImpl.progressKey, SupabaseWatchProgress), so the
 * same records push to and pull from a Nuvio account without translation:
 *
 *   progress_key   "<content_id>" for a movie, "<content_id>_s<S>e<E>" for an
 *                  episode
 *   position/duration/last_watched   milliseconds, as NuvioTV stores them
 *
 * Persisted as one JSON file, loaded and saved whole. Pure: the caller owns
 * the path and the timing of saves.
 */
#ifndef NUVIO_PROGRESS_H
#define NUVIO_PROGRESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NUVIO_PROGRESS_MAX   256

/* NuvioTV's WatchProgress thresholds. */
#define NUVIO_PROGRESS_STARTED    0.02
#define NUVIO_PROGRESS_COMPLETED  0.90

typedef struct nuvio_progress_entry {
    char    progress_key[200];
    char    content_id[128];     /* the meta id - IMDb for Cinemeta         */
    char    content_type[24];    /* "movie" / "series"                      */
    char    video_id[160];       /* what the stream request was made for    */
    int     season;              /* 0 = movie                               */
    int     episode;
    int64_t position_ms;
    int64_t duration_ms;
    int64_t last_watched_ms;     /* unix ms                                 */

    /* Display only - never sent. Filled when played here, or later from
     * the meta for entries that arrived from an account. */
    char    name[192];
    char    poster[384];

    int     dirty;               /* changed since the last successful push   */
} nuvio_progress_entry_t;

typedef struct nuvio_progress {
    nuvio_progress_entry_t *entries;   /* capacity NUVIO_PROGRESS_MAX */
    int                     count;
    int64_t                 last_pull_ms;  /* account sync cursor (phase 3) */
} nuvio_progress_t;

int  nuvio_progress_init(nuvio_progress_t *p);
void nuvio_progress_free(nuvio_progress_t *p);

/* Missing file is an empty store, not an error. */
int  nuvio_progress_load(nuvio_progress_t *p, const char *path);
int  nuvio_progress_save(const nuvio_progress_t *p, const char *path);

void nuvio_progress_key(const char *content_id, int season, int episode,
                        char *out, size_t cap);

/*
 * Insert or replace by progress_key. With the store full, the entry watched
 * longest ago is evicted. `mark_dirty` is 1 for local playback and 0 for
 * entries pulled from an account, which must not be pushed straight back.
 * An incoming entry older than the one held is ignored (last write wins by
 * last_watched, which is how NuvioTV merges too).
 */
nuvio_progress_entry_t *nuvio_progress_upsert(nuvio_progress_t *p,
                                              const nuvio_progress_entry_t *e,
                                              int mark_dirty);

nuvio_progress_entry_t *nuvio_progress_find_key(nuvio_progress_t *p, const char *key);
nuvio_progress_entry_t *nuvio_progress_find_video(nuvio_progress_t *p, const char *video_id);

/* 1 when started and not yet completed. */
int  nuvio_progress_in_progress(const nuvio_progress_entry_t *e);

/*
 * Continue Watching: in-progress entries, most recent first, at most one per
 * series (its latest episode). Writes indices into p->entries; returns count.
 */
int  nuvio_progress_continue(const nuvio_progress_t *p, int *out_idx, int cap);

#ifdef __cplusplus
}
#endif

#endif /* NUVIO_PROGRESS_H */
