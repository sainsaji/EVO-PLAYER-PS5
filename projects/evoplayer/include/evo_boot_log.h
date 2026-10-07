/*
 * evo_boot_log.h — EVO's single diagnostic log: /mnt/usb0/evo.log
 *
 * Every diagnostic stream funnels here — the boot trace (evo_bt / GL context /
 * jailbreak result), the playback breadcrumbs (pp_stage_bc), the native
 * decoder notes, the per-file playback stats — one timestamped, append-only
 * file so there is a single place to look.
 *
 * evo_boot_log() timestamps the line and, before /mnt/usb0 is reachable,
 * buffers it in memory; evo_boot_log_flush() opens the file once the sandbox
 * is unjailed, drains the buffer, and thereafter every line is written
 * straight through - which is what makes the last line before a crash
 * survive, so keep anything emitted per-frame rare rather than deferring it.
 * Call flush right after evo_jailbreak_self(). With
 * EVO_BOOT_TRACE_POPUP (--breadcrumbs) each line also pops a notification.
 * No-op on host / payload builds.
 *
 * NOT funnelled here: evo_status (a live one-line state snapshot the dev
 * remote polls) and evo_compat_report.txt (a user-triggered report).
 */
#ifndef EVO_BOOT_LOG_H
#define EVO_BOOT_LOG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void evo_boot_log(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;
/* A line with a severity: level 1 = WARN, 2 = ERROR. The word follows the
 * timestamp ("[12.345] ERROR text"), so tools that match "[t] text" still work
 * for info lines. The log viewer colours and filters on it. Use the macros. */
void evo_log_level(int level, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;
#define evo_log_warn(...)  evo_log_level(1, __VA_ARGS__)
#define evo_log_error(...) evo_log_level(2, __VA_ARGS__)

/* Put everything logged so far on the USB stick and fsync it - a breadcrumb.
 * Blocks until the stick has it; the first call also opens the file. */
void evo_boot_log_flush(void);
/* The periodic flush from the render loop: wakes the writer thread and
 * returns at once. Never stalls a frame on a busy USB stick. */
void evo_boot_log_kick(void);
/* Crash-handler use only: write still-queued lines to `fd` (no locks). */
void evo_boot_log_crash_drain(int fd);

/* Mask credentials in a line in place (api_key=, token=, password=, ...,
 * Xtream /live/USER/PASS/). evo_boot_log() applies it to every line. */
void evo_log_redact(char *line, size_t cap);

/* Live tail (evo_log_server.c). The log keeps the last 128 KiB in a ring; a
 * reader holds an absolute byte position into the stream of all logged bytes.
 *   evo_log_ring_total: the position of "now" (pass it to tail from here).
 *   evo_log_ring_read:  copy up to `cap` bytes of whole lines from *pos into
 *     buf and advance *pos, waiting up to wait_ms if nothing is new. A position
 *     the ring has already overwritten is moved to the oldest line still held,
 *     and the skipped byte count is added to *missed (may be NULL).
 *     Returns bytes copied (0 on timeout). */
unsigned long long evo_log_ring_total(void);
size_t evo_log_ring_read(unsigned long long *pos, char *buf, size_t cap,
                         int wait_ms, unsigned long long *missed);
/* Start the live log server (once; later calls do nothing). Port
 * EVO_LOG_SERVER_PORT, read-only, every line already redacted. */
void evo_log_server_start(void);
#define EVO_LOG_SERVER_PORT 9780

/* Preferred names for new code — the file carries far more than the boot. */
#define evo_log        evo_boot_log
#define evo_log_flush  evo_boot_log_flush

#ifdef __cplusplus
}
#endif

#endif /* EVO_BOOT_LOG_H */
