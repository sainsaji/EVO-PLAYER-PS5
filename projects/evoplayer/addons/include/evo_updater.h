/*
 * evo_updater.h - in-app self-update from GitHub releases (#120).
 *
 * Check the latest release, stream its .ffpfsc to a staging file beside the
 * installed image, verify the SHA-256 GitHub publishes for the asset, then swap
 * it in with one rename. Nothing is ever replaced before the hash matches, and
 * the user always has to say yes to each step.
 *
 * The download is streamed through FFmpeg's HTTP client (evo_avio_stream), so the
 * 22 MB image never sits in memory. The swap is the same handoff Nuvio Max'd uses
 * for an image install (verified copy, then one rename on the same filesystem);
 * the old image stays mounted under its open inode, so the running EVO is not
 * disturbed. The caller closes EVO afterwards and the user relaunches it.
 *
 * All calls are safe from the UI thread: the network work runs on its own thread
 * and evo_updater_get() only copies a snapshot.
 */
#ifndef EVO_UPDATER_H
#define EVO_UPDATER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EVO_UPD_IDLE = 0,        /* nothing checked yet */
    EVO_UPD_CHECKING,
    EVO_UPD_UP_TO_DATE,
    EVO_UPD_AVAILABLE,       /* newer release found; latest/notes/total are set */
    EVO_UPD_DOWNLOADING,     /* done / total are bytes */
    EVO_UPD_READY,           /* verified and staged; evo_updater_apply() may run */
    EVO_UPD_APPLIED,         /* swapped in; close EVO and relaunch it */
    EVO_UPD_FAILED           /* error says why; the installed image is untouched */
} evo_upd_state_t;

typedef struct {
    evo_upd_state_t state;
    char     latest[24];     /* "v0.13.1" */
    char     notes[4096];   /* release notes, cut to fit */
    uint64_t done, total;    /* download bytes */
    char     error[160];
} evo_updater_status_t;

/* Start a check (no-op while one is running or a download is in progress).
 * There is one image for every console: it asks for the PS5 Pro memory mode,
 * which only a Pro grants. */
void evo_updater_check(void);

/* From AVAILABLE: start the download. Returns 1 if it started. */
int  evo_updater_download(void);

/* Stop a running download and delete the partial file. */
void evo_updater_cancel(void);

/* From READY: swap the verified image in. Returns 1 on success (state APPLIED). */
int  evo_updater_apply(void);

void evo_updater_get(evo_updater_status_t *out);

/* 1 once, the first time a check finds a newer release - for a toast. */
int  evo_updater_take_new_flag(void);

/* The release notes as plain one-line text: markdown marks dropped, line breaks and
 * list items turned into " - " separators, cut to `cap`-1 characters. For the update
 * prompt and the Settings row, which have no markdown renderer. */
void evo_updater_plain_notes(const char *notes, char *out, unsigned cap);

/* Exposed for tests: 1 if `a` is a newer version than `b` ("v1.2.3" or "1.2.3"). */
int  evo_updater_version_newer(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

#endif /* EVO_UPDATER_H */
