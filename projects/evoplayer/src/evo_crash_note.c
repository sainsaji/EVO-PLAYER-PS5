/*
 * evo_crash_note — see evo_crash_note.h for why this exists.
 */
#include "evo_crash_note.h"

#include "evo_data_path.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NOTE_PATH_MAX   512
#define QUARANTINE_MAX  64

static char g_note_file[NOTE_PATH_MAX];
static int  g_note_file_ready;

/* Written by the extractor, read by the signal handler. */
static volatile char g_inflight[NOTE_PATH_MAX];

static char *g_quarantine[QUARANTINE_MAX];
static int   g_quarantine_count;
static int   g_quarantine_loaded;

void evo_crash_note_init(void)
{
    /*
     * Re-resolves every call, because the data root moves. On the app module
     * evo_data_dir() answers /download0/evoplayer until the self-unjail lands
     * and /data/evoplayer after - and only the latter survives a relaunch
     * (issue #46), which is the whole point of the quarantine. So this is
     * called again after evo_data_path_rebind().
     */
    char resolved[NOTE_PATH_MAX];
    snprintf(resolved, sizeof resolved, "%s/thumb_quarantine", evo_data_dir());

    if (g_note_file_ready && strcmp(resolved, g_note_file) == 0)
        return;

    memcpy(g_note_file, resolved, sizeof g_note_file);
    g_note_file_ready = 1;

    /* Different file, so anything cached from the old one is stale. */
    for (int i = 0; i < g_quarantine_count; ++i) {
        free(g_quarantine[i]);
        g_quarantine[i] = NULL;
    }
    g_quarantine_count = 0;
    g_quarantine_loaded = 0;
}

const char *evo_crash_note_path(void)
{
    return g_note_file_ready ? g_note_file : NULL;
}

void evo_crash_note_set(const char *path)
{
    if (!path || !path[0]) {
        g_inflight[0] = '\0';
        return;
    }
    size_t n = strlen(path);
    if (n >= NOTE_PATH_MAX)
        n = NOTE_PATH_MAX - 1;
    /* Length first, then the terminator last, so a handler that fires
     * mid-copy sees either the old string or a short one, never a runaway. */
    for (size_t i = 0; i < n; ++i)
        g_inflight[i] = path[i];
    g_inflight[n] = '\0';
}

void evo_crash_note_commit(void)
{
    if (!g_note_file_ready || !g_inflight[0])
        return;

    /* Async-signal-safe only: no stdio, no malloc, no strlen on a volatile. */
    char line[NOTE_PATH_MAX + 1];
    size_t n = 0;
    while (n < NOTE_PATH_MAX - 1 && g_inflight[n]) {
        line[n] = g_inflight[n];
        n++;
    }
    if (!n)
        return;
    line[n++] = '\n';

    int fd = open(g_note_file, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        return;
    (void)write(fd, line, n);
    (void)close(fd);
}

static void quarantine_load(void)
{
    if (g_quarantine_loaded)
        return;
    g_quarantine_loaded = 1;
    if (!g_note_file_ready)
        return;

    FILE *fp = fopen(g_note_file, "r");
    if (!fp)
        return;
    char line[NOTE_PATH_MAX];
    while (g_quarantine_count < QUARANTINE_MAX && fgets(line, sizeof line, fp)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (!n)
            continue;
        char *dup = (char *)malloc(n + 1);
        if (!dup)
            break;
        memcpy(dup, line, n + 1);
        g_quarantine[g_quarantine_count++] = dup;
    }
    fclose(fp);
}

int evo_crash_note_is_quarantined(const char *path)
{
    if (!path || !path[0])
        return 0;
    quarantine_load();
    for (int i = 0; i < g_quarantine_count; ++i) {
        if (strcmp(g_quarantine[i], path) == 0)
            return 1;
    }
    return 0;
}

void evo_crash_note_clear(void)
{
    for (int i = 0; i < g_quarantine_count; ++i) {
        free(g_quarantine[i]);
        g_quarantine[i] = NULL;
    }
    g_quarantine_count = 0;
    g_quarantine_loaded = 1;   /* nothing to reload - the file is going away */
    if (g_note_file_ready)
        (void)unlink(g_note_file);
}
