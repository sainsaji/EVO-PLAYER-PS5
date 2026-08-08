/* evo_notify.c - on-screen notification helper.
 *
 * A payload launched through ps5-payload-elfldr has nowhere obvious to print:
 * stdout is not attached to anything you can see unless the loader redirects
 * it. The dependable channel is the system notification popup, which is what
 * the SDK's own hello_world sample uses.
 *
 * This wraps sceKernelSendNotificationRequest with printf-style formatting so
 * the test projects can report progress from the couch.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "evo_ps5.h"

void
evo_notify(const char *fmt, ...)
{
    evo_notify_request_t req;
    va_list ap;

    /* The struct is passed to the kernel; zero it so no stack garbage leaks
     * into the leading `useless1` field. */
    memset(&req, 0, sizeof req);

    va_start(ap, fmt);
    vsnprintf(req.message, sizeof req.message, fmt, ap);
    va_end(ap);

    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);

    /* Also emit on stdout. Harmless when nothing is listening, and visible
     * when the payload is launched via websrv (which does redirect stdio). */
    printf("%s\n", req.message);
    fflush(stdout);
}
