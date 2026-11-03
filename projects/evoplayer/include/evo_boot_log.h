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
 * remote polls). The problem report (evo_report_write) is built from it.
 */
#ifndef EVO_BOOT_LOG_H
#define EVO_BOOT_LOG_H

#include <stddef.h>
#include <time.h>

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
/* #229: the file the log is going to (/mnt/usb0/evo.log, or
 * /data/evoplayer/evo.log with no USB stick). 0 = no file open yet. */
int evo_log_file_path(char *out, size_t cap);

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
/* #229: the problem report a user can send without a USB stick - version,
 * build, model, the /stats snapshot, the settings file, and the end of evo.log
 * read from disk (earlier sessions too, so a crash before a relaunch is in it),
 * every line redacted again. The phone gets it from GET /report.txt; Settings >
 * REPORT A PROBLEM saves it with evo_report_save.
 *   evo_report_write: feeds the report to `sink` (0 = keep going) in pieces;
 *     returns the bytes written, -1 if the sink stopped it.
 *   evo_report_save:  writes /mnt/usb0/evo_report.txt, or
 *     /data/evoplayer/evo_report.txt with no stick; the path goes to `out`.
 *     1 = saved. */
typedef int (*evo_report_sink_fn)(void *ud, const char *data, size_t n);
long evo_report_write(evo_report_sink_fn sink, void *ud);
int  evo_report_save(char *out, size_t cap);
#define EVO_REPORT_LOG_BYTES (1536 * 1024)   /* the most of evo.log a report carries */

/* Start the live log server (once; later calls do nothing). Port
 * EVO_LOG_SERVER_PORT, read-only, every line already redacted. */
void evo_log_server_start(void);
#define EVO_LOG_SERVER_PORT 9780   /* default; the user can change it (#227) */

/* The port is user-settable. The server binds the saved port, falls back to the
 * next nine when it is busy, and keeps retrying the saved one in the background.
 *   evo_log_server_port:       the port really listening, 0 = none (QR off)
 *   evo_log_server_saved_port: the port the user chose
 *   evo_log_server_state:      EVO_LOGSRV_*
 *   evo_log_server_set_port:   store without testing (settings load, reset)
 *   evo_log_server_try_port:   test-bind first; 0 = accepted and stored, else the
 *                              errno (EADDRINUSE = busy, EINVAL = out of range)
 *                              and the old port is kept. */
enum { EVO_LOGSRV_STARTING = 0, EVO_LOGSRV_SAVED = 1, EVO_LOGSRV_FALLBACK = 2, EVO_LOGSRV_NONE = 3 };
int  evo_log_server_port(void);
int  evo_log_server_saved_port(void);
int  evo_log_server_state(void);
void evo_log_server_set_port(int port);
int  evo_log_server_probe_port(int port);   /* the test-bind alone: stores nothing */
int  evo_log_server_try_port(int port);

/* Preferred names for new code — the file carries far more than the boot. */
#define evo_log        evo_boot_log
#define evo_log_flush  evo_boot_log_flush

/* #216: tagged runtime lines - "net: ...", "ftp: ...", "emby: ...". No
 * "EVO boot:" prefix and no kernel-log write, so cheaper than evo_bt; still
 * not for per-frame code. Host builds (tests, uiview) do not link the log, so
 * there they are a no-op that still type-checks the format. Every line passes
 * evo_log_redact, but never put a password or token in the format anyway. */
static inline void evo_log_nop_(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;
static inline void evo_log_nop_(const char *fmt, ...) { (void)fmt; }
#ifdef EVO_APP_MODULE
#define EVO_LOGI(tag, ...) evo_log_level(0, tag ": " __VA_ARGS__)
#define EVO_LOGW(tag, ...) evo_log_level(1, tag ": " __VA_ARGS__)
#define EVO_LOGE(tag, ...) evo_log_level(2, tag ": " __VA_ARGS__)
#else
#define EVO_LOGI(tag, ...) evo_log_nop_(tag ": " __VA_ARGS__)
#define EVO_LOGW(tag, ...) evo_log_nop_(tag ": " __VA_ARGS__)
#define EVO_LOGE(tag, ...) evo_log_nop_(tag ": " __VA_ARGS__)
#endif

/* Monotonic milliseconds, for "took N ms" in a log line:
 *   const long long t0 = evo_log_mono_ms(); ...; evo_log_mono_ms() - t0 */
static inline long long evo_log_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

/* At most one line per `ms` from this call site, for code that can fail over
 * and over (a reconnect loop, a per-packet error). The lines in between are
 * dropped, not queued; the next one that gets through says how many. */
#define EVO_LOG_EVERY_(ms, LOGM, tag, fmt, ...) do {                              \
        static long long evo_last_;                                               \
        static int evo_skipped_;                                                  \
        const long long evo_now_ = evo_log_mono_ms();                            \
        if (evo_last_ == 0 || evo_now_ - evo_last_ >= (ms)) {                     \
            evo_last_ = evo_now_;                                                 \
            LOGM(tag, fmt " (+%d similar)", ##__VA_ARGS__, evo_skipped_);         \
            evo_skipped_ = 0;                                                     \
        } else {                                                                  \
            evo_skipped_++;                                                       \
        }                                                                         \
    } while (0)
#define EVO_LOGI_EVERY(ms, tag, fmt, ...) EVO_LOG_EVERY_(ms, EVO_LOGI, tag, fmt, ##__VA_ARGS__)
#define EVO_LOGW_EVERY(ms, tag, fmt, ...) EVO_LOG_EVERY_(ms, EVO_LOGW, tag, fmt, ##__VA_ARGS__)
#define EVO_LOGE_EVERY(ms, tag, fmt, ...) EVO_LOG_EVERY_(ms, EVO_LOGE, tag, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* EVO_BOOT_LOG_H */
