/*
 * evo_boot_log.h — capture boot-time diagnostics that happen BEFORE the
 * sandbox is unjailed (evo_agc_probe, evo_vdec_probe, the jailbreak result
 * itself), so they can be pulled off the console as a file instead of
 * screenshotted one notification at a time.
 *
 * evo_boot_log() buffers the line in memory (app module only) and, when
 * EVO_BOOT_TRACE_POPUP is defined (#51 opt-in, scripts/package-app.sh
 * --breadcrumbs), also pops a notification. evo_boot_log_flush() appends the
 * buffer to /mnt/usb0/evo_boot.log once /mnt/usb0 is reachable. Call flush
 * right after evo_jailbreak_self() and again periodically. No-op on host /
 * payload.
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

#ifdef __cplusplus
}
#endif

#endif /* EVO_BOOT_LOG_H */
