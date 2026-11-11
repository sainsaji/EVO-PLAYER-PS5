#ifndef EVO_RECENT_H
#define EVO_RECENT_H

/*
 * Module: evo_recent
 *
 * Recent-files database. Persists to /data/evoplayer/evo_recent.txt.
 * Stores the last 25 played files with path, title, position and duration.
 *
 * Public surface:
 *   recent_clear()
 *   recent_add_or_update(path, title, last_pos, duration)
 *   recent_update_current_position()   -- snapshot current playback pos
 *   recent_save()
 *   recent_load()
 *   recent_lookup(path)                -- returns last position, or 0.0
 *
 * State is exposed read-only via the arrays below for the UI draw code.
 */

#include "evo_data_path.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RECENT_FILE_DB   evo_data_path("evo_recent.txt")
#define MAX_RECENT_FILES 25

typedef struct {
    char      path[512];
    char      title[128];
    double    last_pos;
    double    duration;
    long long last_played;
    int       audio_stream;   /* -1 for unset/default, or active audio stream index */
} RecentFileEntry;

extern RecentFileEntry recent_files[MAX_RECENT_FILES];
extern int             recent_file_count;
extern int             recent_selected;

void   recent_clear(void);
#ifdef __cplusplus
void   recent_add_or_update(const char *path, const char *title,
                             double last_pos, double duration,
                             int audio_stream = -1);
#else
void   recent_add_or_update(const char *path, const char *title,
                             double last_pos, double duration,
                             int audio_stream);
#endif
void   recent_update_current_position(void);
void   recent_save(void);
void   recent_load(void);
double recent_lookup(const char *path);   /* returns last_pos or 0.0 */
int    recent_lookup_audio(const char *path); /* returns audio_stream or -1 */

/* #233: drop the entry for `path`, and every entry under it when `path` is a
 * folder that was deleted. Saves when anything went. Returns how many went. */
int    recent_forget(const char *path);
/* #233: remove one entry by index / empty the list; both save. */
void   recent_remove_at(int index);
void   recent_clear_all(void);
/* #233: drop local entries whose file is gone (deleted outside EVO). Only when
 * the folder is there and lists something - an unplugged USB stick keeps its
 * entries. Saves when anything went. */
int    recent_prune_missing(void);

#ifdef __cplusplus
}
#endif

#endif /* EVO_RECENT_H */
