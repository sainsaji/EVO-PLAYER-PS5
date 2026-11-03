#ifndef EVO_PREV_RUN_H
#define EVO_PREV_RUN_H

/*
 * evo_prev_run - keep the previous run's log, and say how that run ended (#229).
 *
 * evo.log is appended to, with a "log session start" line per launch, so a run
 * that crashed is still in it after a relaunch. But a report carries only the
 * end of the file, and a long new session pushes the crashed one out. So at
 * start, before the new session header, the last session is copied on its own
 * to evo.prev.log next to evo.log, and the report can be made from that.
 *
 * How it ended is read from its last lines:
 *   CRASHED   a "CRASH signal=" line (the crash handler writes it)
 *   CLOSED    "soft close: parked" - closed from EVO
 *   ENDED     neither: the PS button, a kill, or a crash with no time to log
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EVO_PREV_RUN_NONE = 0,   /* no earlier session in the log */
    EVO_PREV_RUN_CRASHED,
    EVO_PREV_RUN_CLOSED,
    EVO_PREV_RUN_ENDED
} evo_prev_run_t;

/* Copies the last session of `log_path` to `prev_path` (at most `max_bytes`,
 * from a line start) and returns how it ended. NONE (and no file written) when
 * the log is missing or empty. */
evo_prev_run_t evo_prev_run_save(const char *log_path, const char *prev_path, long max_bytes);

/* What the last save found, and where it went ("" when NONE). */
evo_prev_run_t evo_prev_run_state(void);
const char    *evo_prev_run_path(void);

/* "crashed", "closed from EVO", "ended without closing (PS button or crash)". */
const char    *evo_prev_run_text(evo_prev_run_t s);

#ifdef __cplusplus
}
#endif

#endif /* EVO_PREV_RUN_H */
