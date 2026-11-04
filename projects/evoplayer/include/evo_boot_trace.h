/*
 * evo_boot_trace.h - Phase 1b app-module bring-up breadcrumbs.
 *
 * #51: evo_bt("...") writes to three places whenever EVO_APP_MODULE is defined:
 *   - the single diagnostic log /mnt/usb0/evo.log (via evo_boot_log) — durable,
 *     the place to look after the fact;
 *   - the kernel log (tools/klog.sh) — visible live before VideoOut is up and
 *     without a USB stick;
 *   - a system notification, but only with EVO_BOOT_TRACE_POPUP
 *     (scripts/package-app.sh --breadcrumbs) — noise on the TV otherwise.
 *
 * Compiles to nothing outside the app module (host / payload builds).
 */
#ifndef EVO_BOOT_TRACE_H
#define EVO_BOOT_TRACE_H

#ifdef EVO_APP_MODULE

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "evo_boot_log.h"   /* evo_bt lines also land in /mnt/usb0/evo.log */

#ifdef __cplusplus
extern "C" {
#endif

struct evo_bt_req { char pad[45]; char msg[3075]; };
extern int sceKernelSendNotificationRequest(int, void *, unsigned long, int);
extern int sceKernelDebugOutText(int, const char *);   /* -> kernel log (klog.sh) */

static inline void evo_bt_(const char *fmt, ...)
{
    struct evo_bt_req r;
    char line[512];
    va_list ap;

    memset(&r, 0, sizeof r);
    va_start(ap, fmt);
    vsnprintf(r.msg, sizeof r.msg, fmt, ap);
    va_end(ap);

#ifdef EVO_BOOT_TRACE_POPUP
    /* On-screen popup - opt-in (--breadcrumbs), for watching the TV without
     * klog attached. */
    sceKernelSendNotificationRequest(0, &r, sizeof r, 0);
#endif

    /* Durable: the one diagnostic log. */
    evo_boot_log("%s", r.msg);

    /* Kernel log - captured remotely by tools/klog.sh, so an unattended
     * deploy/launch/collect loop needs no TV. Always on in the app module. */
    snprintf(line, sizeof line, "%s\n", r.msg);
    sceKernelDebugOutText(0, line);
}

#ifdef __cplusplus
}
#endif

#define evo_bt(...) evo_bt_("EVO boot: " __VA_ARGS__)

#else /* !EVO_APP_MODULE */

/* Off outside the app module, but the arguments are still type-checked
 * against the format (a wrong %d/%s in a trace only shows on the console
 * otherwise), and variables kept for a trace do not read as unused. The
 * call is inline and empty, so nothing is evaluated beyond the arguments. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
static inline void evo_bt_off_(const char *fmt, ...) { (void)fmt; }
#define evo_bt(...) evo_bt_off_("EVO boot: " __VA_ARGS__)

#endif

#endif /* EVO_BOOT_TRACE_H */
