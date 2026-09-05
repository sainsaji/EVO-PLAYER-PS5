/*
 * evo_boot_trace.h - Phase 1b app-module bring-up breadcrumbs.
 *
 * #51: evo_bt("...") always logs to the kernel log (tools/klog.sh) whenever
 * EVO_APP_MODULE is defined - that's the only diagnostics channel visible
 * before VideoOut is up, and it's cheap enough to leave on unconditionally.
 * It ALSO used to pop a system notification on every single call, which was
 * right while debugging blind at Phase 1b but is just noise on the TV now
 * that the app module boots fine. The popup is opt-in: define
 * EVO_BOOT_TRACE_POPUP (scripts/package-app.sh --breadcrumbs) to bring it
 * back for a session where you're watching the TV without klog attached.
 *
 * Compiles to nothing outside the app module (host / payload builds).
 */
#ifndef EVO_BOOT_TRACE_H
#define EVO_BOOT_TRACE_H

#ifdef EVO_APP_MODULE

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

#define evo_bt(...) ((void)0)

#endif

#endif /* EVO_BOOT_TRACE_H */
