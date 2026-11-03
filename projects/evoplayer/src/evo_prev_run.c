/*
 * evo_prev_run.c - see include/evo_prev_run.h (#229).
 *
 * Plain stdio, no EVO state, so the host tests drive it with a made-up log.
 */
#include "evo_prev_run.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PREV_SCAN_BYTES (8L * 1024 * 1024)   /* how far back a session start is looked for */

static evo_prev_run_t s_state = EVO_PREV_RUN_NONE;
static char s_path[256];

static const char kSessionStart[] = "INFO  log session start:";

evo_prev_run_t evo_prev_run_save(const char *log_path, const char *prev_path, long max_bytes)
{
    s_state = EVO_PREV_RUN_NONE;
    s_path[0] = '\0';
    if (!log_path || !prev_path) return s_state;

    FILE *in = fopen(log_path, "rb");
    if (!in) return s_state;
    if (fseek(in, 0, SEEK_END) != 0) { fclose(in); return s_state; }
    const long size = ftell(in);
    if (size <= 0) { fclose(in); return s_state; }

    /* Pass 1: where the last session starts. A log from before session headers
     * (or one that rotated mid-session) has none: then the tail is the run. */
    long from = size > PREV_SCAN_BYTES ? size - PREV_SCAN_BYTES : 0;
    fseek(in, from, SEEK_SET);
    char line[1024];
    long pos = from, start = -1;
    int partial = from > 0;
    while (fgets(line, sizeof line, in)) {
        const long here = pos;
        const size_t n = strlen(line);
        pos += (long)n;
        const int whole = n && line[n - 1] == '\n';
        if (partial) { partial = !whole; continue; }
        if (strstr(line, kSessionStart)) start = here;
        partial = !whole;   /* a line longer than the buffer: its rest is not a line start */
    }
    if (start < 0) start = from;
    int cut = 0;
    if (max_bytes > 0 && size - start > max_bytes) { start = size - max_bytes; cut = 1; }

    /* Pass 2: copy it, and read how it ended. */
    FILE *out = fopen(prev_path, "wb");
    if (!out) { fclose(in); return s_state; }
    fseek(in, start, SEEK_SET);
    int crashed = 0, closed = 0, any = 0;
    partial = cut;
    while (fgets(line, sizeof line, in)) {
        const size_t n = strlen(line);
        const int whole = n && line[n - 1] == '\n';
        if (partial) { partial = !whole; continue; }
        if (strstr(line, "CRASH signal=")) crashed = 1;
        if (strstr(line, "soft close: parked")) closed = 1;
        fputs(line, out);
        any = 1;
    }
    fclose(in);
    if (fclose(out) != 0 || !any) {
        unlink(prev_path);
        return s_state;
    }
    s_state = crashed ? EVO_PREV_RUN_CRASHED : closed ? EVO_PREV_RUN_CLOSED : EVO_PREV_RUN_ENDED;
    snprintf(s_path, sizeof s_path, "%s", prev_path);
    return s_state;
}

evo_prev_run_t evo_prev_run_state(void) { return s_state; }
const char *evo_prev_run_path(void) { return s_path; }

const char *evo_prev_run_text(evo_prev_run_t s)
{
    switch (s) {
    case EVO_PREV_RUN_CRASHED: return "crashed";
    case EVO_PREV_RUN_CLOSED:  return "closed from EVO";
    case EVO_PREV_RUN_ENDED:   return "ended without closing (PS button or crash)";
    default:                   return "no earlier run";
    }
}
