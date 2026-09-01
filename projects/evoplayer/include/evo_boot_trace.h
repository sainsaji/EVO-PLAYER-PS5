/*
 * evo_boot_trace.h - Phase 1b app-module bring-up breadcrumbs.
 *
 * When EVO_BOOT_TRACE is defined (scripts/package-app.sh sets it for the
 * app-module build), evo_bt("...") pops a system notification - the only
 * channel visible before VideoOut is up. Compiles to nothing otherwise.
 * Remove the -DEVO_BOOT_TRACE once milestone 1 is signed off.
 */
#ifndef EVO_BOOT_TRACE_H
#define EVO_BOOT_TRACE_H

#ifdef EVO_BOOT_TRACE

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

    /* On-screen popup (visible when watching the TV). */
    sceKernelSendNotificationRequest(0, &r, sizeof r, 0);

    /* Kernel log - captured remotely by tools/klog.sh, so an unattended
     * deploy/launch/collect loop needs no TV. */
    snprintf(line, sizeof line, "%s\n", r.msg);
    sceKernelDebugOutText(0, line);
}

#ifdef __cplusplus
}
#endif

#define evo_bt(...) evo_bt_("EVO boot: " __VA_ARGS__)

#else /* !EVO_BOOT_TRACE */

#define evo_bt(...) ((void)0)

#endif

#endif /* EVO_BOOT_TRACE_H */
