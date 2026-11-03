/*
 * Module: evo_recent
 *
 * Recent-files database. Persists to /data/evoplayer/evo_recent.txt.
 * Extracted from main.c (lines 600–614, 1611–1723, 1428–1445).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "evo_recent.h"
#include "evo_error.h"
#include "evo_readdir.h"

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

    char clean_path[512];
    evo_url_sanitize(path, clean_path, sizeof(clean_path));

    int existing = -1;
    for (int i = 0; i < recent_file_count; i++) {
        if (strcmp(recent_files[i].path, clean_path) == 0) {
            existing = i;
            break;
        }
    }

    RecentFileEntry entry;
    memset(&entry, 0, sizeof(entry));
    snprintf(entry.path, sizeof(entry.path), "%s", clean_path);

    char clean_title[128];
    evo_url_sanitize(title && title[0] ? title : clean_path, clean_title, sizeof(clean_title));
    snprintf(entry.title, sizeof(entry.title), "%s", clean_title);
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

        evo_url_sanitize(path, e->path, sizeof(e->path));
        evo_url_sanitize(title, e->title, sizeof(e->title));
        e->last_pos    = last_pos    ? atof(last_pos)    : 0.0;
        e->duration    = duration    ? atof(duration)    : 0.0;
        e->last_played = last_played ? atoll(last_played) : 0;

        recent_file_count++;
    }

    fclose(fp);
}

void recent_remove_at(int index)
{
    if (index < 0 || index >= recent_file_count) return;
    memmove(&recent_files[index], &recent_files[index + 1],
            (size_t)(recent_file_count - index - 1) * sizeof recent_files[0]);
    recent_file_count--;
    memset(&recent_files[recent_file_count], 0, sizeof recent_files[0]);
    if (recent_selected >= recent_file_count) recent_selected = recent_file_count ? recent_file_count - 1 : 0;
    recent_save();
}

void recent_clear_all(void)
{
    recent_clear();
    recent_save();
}

int recent_forget(const char *path)
{
    if (!path || !path[0]) return 0;
    char clean_path[512];
    evo_url_sanitize(path, clean_path, sizeof(clean_path));
    size_t n = strlen(clean_path);
    while (n > 1 && clean_path[n - 1] == '/') clean_path[--n] = '\0';

    int gone = 0;
    for (int i = 0; i < recent_file_count; ) {
        const char *p = recent_files[i].path;
        /* The file itself, or anything inside a deleted folder - not a
         * sibling that merely starts with the same name ("a.mkv" vs "a.mkv2"). */
        if (strncmp(p, clean_path, n) == 0 && (p[n] == '\0' || p[n] == '/')) {
            memmove(&recent_files[i], &recent_files[i + 1],
                    (size_t)(recent_file_count - i - 1) * sizeof recent_files[0]);
            recent_file_count--;
            memset(&recent_files[recent_file_count], 0, sizeof recent_files[0]);
            gone++;
        } else {
            i++;
        }
    }
    if (gone) {
        if (recent_selected >= recent_file_count) recent_selected = recent_file_count ? recent_file_count - 1 : 0;
        recent_save();
    }
    return gone;
}

/* Is `dir` there and does it list anything? An unplugged USB stick leaves an
 * empty mount point (or no folder at all): its files are away, not gone. */
static int dir_has_entries(const char *dir)
{
    evo_dir_t *d = evo_opendir(dir);
    if (!d) return 0;
    int any = 0;
    struct dirent *de;
    while (!any && (de = evo_readdir(d)) != NULL)
        any = strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0;
    evo_closedir(d);
    return any;
}

int recent_prune_missing(void)
{
    int gone = 0;
    for (int i = 0; i < recent_file_count; ) {
        const char *p = recent_files[i].path;
        struct stat st;
        int missing = 0;
        if (p[0] == '/' && stat(p, &st) != 0 && errno == ENOENT) {
            char dir[512];
            snprintf(dir, sizeof dir, "%s", p);
            char *slash = strrchr(dir, '/');
            if (slash && slash != dir) {
                *slash = '\0';
                missing = dir_has_entries(dir);
            }
        }
        if (missing) {
            memmove(&recent_files[i], &recent_files[i + 1],
                    (size_t)(recent_file_count - i - 1) * sizeof recent_files[0]);
            recent_file_count--;
            memset(&recent_files[recent_file_count], 0, sizeof recent_files[0]);
            gone++;
        } else {
            i++;
        }
    }
    if (gone) {
        if (recent_selected >= recent_file_count) recent_selected = recent_file_count ? recent_file_count - 1 : 0;
        recent_save();
    }
    return gone;
}

double recent_lookup(const char *path)
{
    if (!path || !path[0]) return 0.0;
    char clean_path[512];
    evo_url_sanitize(path, clean_path, sizeof(clean_path));
    for (int i = 0; i < recent_file_count; i++) {
        if (strcmp(recent_files[i].path, clean_path) == 0)
            return recent_files[i].last_pos;
    }
    return 0.0;
}
