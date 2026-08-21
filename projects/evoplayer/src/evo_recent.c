/*
 * Module: evo_recent
 *
 * Recent-files database. Persists to /data/evoplayer/evo_recent.txt.
 * Extracted from main.c (lines 600–614, 1611–1723, 1428–1445).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "evo_recent.h"

/* Shared player globals. */
extern char   current_media_path[768];
extern double media_duration_sec;
extern double resume_base_offset_seconds;

/* Functions from main.c that recent depends on. */
extern long long now_ms(void);
extern double    prospero_media_clock_seconds(void);
extern void      clean_media_title(const char *path, char *title, size_t tsz,
                                   char *meta, size_t msz);

/* --------------------------------------------------------------------------
 * State (definitions — extern declarations are in evo_recent.h)
 * -------------------------------------------------------------------------- */

RecentFileEntry recent_files[MAX_RECENT_FILES];
int             recent_file_count = 0;
int             recent_selected   = 0;

/* --------------------------------------------------------------------------
 * Implementation
 * -------------------------------------------------------------------------- */

void recent_clear(void)
{
    memset(recent_files, 0, sizeof(recent_files));
    recent_file_count = 0;
    recent_selected   = 0;
}

void recent_add_or_update(const char *path, const char *title,
                           double last_pos, double duration)
{
    if (!path || !path[0]) return;

    int existing = -1;
    for (int i = 0; i < recent_file_count; i++) {
        if (strcmp(recent_files[i].path, path) == 0) {
            existing = i;
            break;
        }
    }

    RecentFileEntry entry;
    memset(&entry, 0, sizeof(entry));
    snprintf(entry.path,  sizeof(entry.path),  "%s", path);
    snprintf(entry.title, sizeof(entry.title), "%s",
             title && title[0] ? title : path);
    entry.last_pos    = last_pos;
    entry.duration    = duration;
    entry.last_played = now_ms();

    if (existing >= 0) {
        for (int i = existing; i > 0; i--)
            recent_files[i] = recent_files[i - 1];
        recent_files[0] = entry;
    } else {
        int limit = recent_file_count;
        if (limit >= MAX_RECENT_FILES) limit = MAX_RECENT_FILES - 1;
        for (int i = limit; i > 0; i--)
            recent_files[i] = recent_files[i - 1];
        recent_files[0] = entry;
        if (recent_file_count < MAX_RECENT_FILES)
            recent_file_count++;
    }

    recent_selected = 0;
}

void recent_update_current_position(void)
{
    if (!current_media_path[0]) return;

    double pos = resume_base_offset_seconds + prospero_media_clock_seconds();
    if (pos < 0.0) pos = 0.0;

    char title[128], meta[96];
    clean_media_title(current_media_path, title, sizeof(title), meta, sizeof(meta));

    recent_add_or_update(current_media_path, title, pos, media_duration_sec);
    recent_save();
}

void recent_save(void)
{
    FILE *fp = fopen(RECENT_FILE_DB, "w");
    if (!fp) return;

    for (int i = 0; i < recent_file_count; i++) {
        fprintf(fp, "%s\t%s\t%.3f\t%.3f\t%lld\n",
                recent_files[i].path,
                recent_files[i].title,
                recent_files[i].last_pos,
                recent_files[i].duration,
                recent_files[i].last_played);
    }

    fclose(fp);
}

void recent_load(void)
{
    recent_clear();

    FILE *fp = fopen(RECENT_FILE_DB, "r");
    if (!fp) return;

    char line[1024];
    while (fgets(line, sizeof(line), fp) && recent_file_count < MAX_RECENT_FILES) {
        RecentFileEntry *e = &recent_files[recent_file_count];
        memset(e, 0, sizeof(*e));

        char *path        = strtok(line,  "\t");
        char *title       = strtok(NULL,  "\t");
        char *last_pos    = strtok(NULL,  "\t");
        char *duration    = strtok(NULL,  "\t");
        char *last_played = strtok(NULL,  "\t\r\n");

        if (!path || !title) continue;

        snprintf(e->path,  sizeof(e->path),  "%s", path);
        snprintf(e->title, sizeof(e->title), "%s", title);
        e->last_pos    = last_pos    ? atof(last_pos)    : 0.0;
        e->duration    = duration    ? atof(duration)    : 0.0;
        e->last_played = last_played ? atoll(last_played) : 0;

        recent_file_count++;
    }

    fclose(fp);
}

double recent_lookup(const char *path)
{
    if (!path || !path[0]) return 0.0;
    for (int i = 0; i < recent_file_count; i++) {
        if (strcmp(recent_files[i].path, path) == 0)
            return recent_files[i].last_pos;
    }
    return 0.0;
}
