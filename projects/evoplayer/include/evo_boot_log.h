/*
 * evo_boot_log.h — EVO's single diagnostic log: /mnt/usb0/evo.log
 *
 * Every diagnostic stream funnels here — the boot trace (evo_bt / pp_agc /
 * jailbreak result), the playback breadcrumbs (pp_stage_bc), the native
 * decoder notes, the VO-debug trace, the per-file playback stats — one
 * timestamped, append-only file so there is a single place to look.
 *
 * evo_boot_log() timestamps the line and, before /mnt/usb0 is reachable,
 * buffers it in memory; evo_boot_log_flush() opens the file once the sandbox
 * is unjailed, drains the buffer, and thereafter every line is written
 * straight through. Call flush right after evo_jailbreak_self() and again
 * periodically (the render loop does, every 64 frames). With
 * EVO_BOOT_TRACE_POPUP (--breadcrumbs) each line also pops a notification.
 * No-op on host / payload builds.
 *
 * NOT funnelled here: evo_status (a live one-line state snapshot the dev
 * remote polls) and evo_compat_report.txt (a user-triggered report).
 */
#ifndef EVO_BOOT_LOG_H
#define EVO_BOOT_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

void evo_boot_log(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;
void evo_boot_log_flush(void);

/* Preferred names for new code — the file carries far more than the boot. */
#define evo_log        evo_boot_log
#define evo_log_flush  evo_boot_log_flush

#ifdef __cplusplus
}
#endif

#endif /* EVO_BOOT_LOG_H */
