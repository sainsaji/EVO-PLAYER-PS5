/*
 * Module: evo_favorites
 *
 * Favorites database. Persists to /mnt/usb0/evo_favorites.txt.
 * Extracted from main.c (lines 617–721, 1591–1607).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "evo_favorites.h"
#include "evo_toast.h"

/* Shared player globals. */
extern char   current_media_path[768];
extern double media_duration_sec;
extern void   clean_media_title(const char *path, char *title, size_t tsz,
                                char *meta, size_t msz);

/* --------------------------------------------------------------------------
 * State (definitions — extern declarations are in evo_favorites.h)
 * -------------------------------------------------------------------------- */

FavoriteEntry favorite_files[MAX_FAVORITES];
int           favorite_count    = 0;
int           favorite_selected = 0;

/* --------------------------------------------------------------------------
 * Implementation
 * -------------------------------------------------------------------------- */

void favorites_clear(void)
{
    memset(favorite_files, 0, sizeof(favorite_files));
    favorite_count    = 0;
    favorite_selected = 0;
}

int favorites_find(const char *path)
{
    if (!path || !path[0]) return -1;
    for (int i = 0; i < favorite_count; i++) {
        if (strcmp(favorite_files[i].path, path) == 0) return i;
    }
    return -1;
}

int favorites_is_favorite(const char *path)
{
    return favorites_find(path) >= 0;
}

void favorites_add(const char *path, const char *title, double duration)
{
    if (!path || !path[0]) return;
    if (favorites_find(path) >= 0) return;
    if (favorite_count >= MAX_FAVORITES) return;

    FavoriteEntry *e = &favorite_files[favorite_count];
    memset(e, 0, sizeof(*e));
    snprintf(e->path,  sizeof(e->path),  "%s", path);
    snprintf(e->title, sizeof(e->title), "%s",
             title && title[0] ? title : path);
    e->duration = duration;

    favorite_count++;
}

void favorites_remove(const char *path)
{
    int idx = favorites_find(path);
    if (idx < 0) return;

    for (int i = idx; i < favorite_count - 1; i++)
        favorite_files[i] = favorite_files[i + 1];

    if (favorite_count > 0) favorite_count--;

    if (favorite_selected >= favorite_count)
        favorite_selected = favorite_count > 0 ? favorite_count - 1 : 0;
}

void favorites_save(void)
{
    FILE *fp = fopen(FAVORITES_FILE_DB, "w");
    if (!fp) return;

    for (int i = 0; i < favorite_count; i++) {
        fprintf(fp, "%s\t%s\t%.3f\n",
                favorite_files[i].path,
                favorite_files[i].title,
                favorite_files[i].duration);
    }

    fclose(fp);
}

void favorites_load(void)
{
    favorites_clear();

    FILE *fp = fopen(FAVORITES_FILE_DB, "r");
    if (!fp) return;

    char line[1024];
    while (fgets(line, sizeof(line), fp) && favorite_count < MAX_FAVORITES) {
        FavoriteEntry *e = &favorite_files[favorite_count];
        memset(e, 0, sizeof(*e));

        char *path     = strtok(line, "\t");
        char *title    = strtok(NULL, "\t");
        char *duration = strtok(NULL, "\t\r\n");

        if (!path || !title) continue;

        snprintf(e->path,  sizeof(e->path),  "%s", path);
        snprintf(e->title, sizeof(e->title), "%s", title);
        e->duration = duration ? atof(duration) : 0.0;

        favorite_count++;
    }

    fclose(fp);
}

void favorites_toggle_current_media(void)
{
    if (!current_media_path[0]) return;

    if (favorites_is_favorite(current_media_path)) {
        favorites_remove(current_media_path);
        favorites_save();
        toast("FAVORITES", "Removed");
    } else {
        char title[128], meta[96];
        clean_media_title(current_media_path, title, sizeof(title),
                          meta, sizeof(meta));
        favorites_add(current_media_path, title, media_duration_sec);
        favorites_save();
        toast("FAVORITES", "Added");
    }
}
