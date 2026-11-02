/*
 * evo_online_subs - subtitles a Stremio addon offers for the title being played.
 *
 * Stremio's own app shows subtitles that are not in the video file: addons
 * with a "subtitles" resource (OpenSubtitles v3 is one, and Stremio ships it)
 * answer <base>/subtitles/<type>/<id>.json with {subtitles:[{url, lang}]}, and
 * a stream addon may attach a "subtitles" array to each stream. EVO used to
 * play only the tracks inside the file, so a series that had subtitles in
 * Stremio showed none in EVO.
 *
 * provider_stremio.c fills this list while it resolves a title (the requests
 * run beside the stream requests and never hold them up). The playback worker
 * then picks the one in SUBTITLE LANGUAGE when the file has no embedded track
 * in that language, and the subtitle picker lists them all as "ONLINE" rows.
 *
 * Thread use: entries are added on the main loop (evo_net callbacks), read by
 * the playback worker and the picker. Everything here takes one mutex.
 */
#ifndef EVO_ONLINE_SUBS_H
#define EVO_ONLINE_SUBS_H

#ifdef __cplusplus
extern "C" {
#endif

#define EVO_OSUB_MAX     32
#define EVO_OSUB_URL     1024
#define EVO_OSUB_LABEL   64

typedef struct evo_osub {
    char url[EVO_OSUB_URL];
    char lang[4];               /* ISO 639-2/T, "" when the tag was unknown */
    char lang_tag[16];          /* the tag as the addon sent it */
    char source[EVO_OSUB_LABEL];/* the addon's name */
} evo_osub_t;

/* A new title is being resolved: forget the old list. `item_key` is the
 * provider item id; later calls with another key are ignored. */
void evo_osubs_begin(const char *item_key);

/* One subtitle for `item_key`. Duplicate URLs are dropped. */
void evo_osubs_add(const char *item_key, const char *url, const char *lang_tag,
                   const char *source);

/* Requests still out for `item_key` (+1 when one is sent, -1 when it lands). */
void evo_osubs_pending(const char *item_key, int delta);

/* Wait (sleeping) until no request is out for `item_key` or `timeout_ms`
 * passes. Returns the number of entries. Never call it on the main loop: the
 * callbacks it waits for run there. */
int  evo_osubs_wait(const char *item_key, int timeout_ms);

/* Snapshot of entry `i` into *out; 0 when out of range. */
int  evo_osubs_get(int i, evo_osub_t *out);
int  evo_osubs_count(void);
/* The key the list belongs to ("" none). */
void evo_osubs_key(char *out, int cap);

/*
 * The best entry for SUBTITLE LANGUAGE: the first in the preferred language,
 * else (when `allow_fallback`) the first in English. -1 when there is none.
 * `rank_out` gets its evo_langpref_rank (2 or 1).
 */
int  evo_osubs_best(int allow_fallback, int *rank_out);

/* Which entry is loaded as the external track now (-1 none). */
void evo_osubs_set_active(int i);
int  evo_osubs_active(void);

/* Picker download, off the UI thread: start fetching entry `i` to a file.
 * evo_osubs_take_ready() (main loop) then returns the file path once - the
 * caller loads it - or NULL while it is still running. */
int  evo_osubs_fetch_async(int i);
const char *evo_osubs_take_ready(int *index_out, int *failed_out);
int  evo_osubs_fetching(void);

/* Fetch entry `i` now (blocking - playback worker only) into the data dir.
 * Returns the file path, or NULL. */
const char *evo_osubs_fetch_sync(int i);

/*
 * The online subtitle in use for this title: its file is copied aside so a
 * reopen of the same title (an audio switch clears the external track) can
 * load it again. evo_osubs_begin() forgets it.
 */
void evo_osubs_remember(const char *path, const char *label);
int  evo_osubs_remembered(char *path, int path_cap, char *label, int label_cap);

/* "Italiano · OpenSubtitles v3" for entry `i`. */
void evo_osubs_label(int i, char *out, int cap);

#ifdef __cplusplus
}
#endif

#endif
