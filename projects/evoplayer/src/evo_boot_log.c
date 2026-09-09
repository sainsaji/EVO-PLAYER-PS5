/* evo_boot_log.c — see evo_boot_log.h. The one diagnostic log. */
#include "evo_boot_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef EVO_APP_MODULE

#include <time.h>
#include <unistd.h>

#ifdef EVO_BOOT_TRACE_POPUP
struct bl_note { char pad[45]; char msg[3075]; };
extern int sceKernelSendNotificationRequest(int, void *, unsigned long, int);
#endif

#define EVO_LOG_PATH "/mnt/usb0/evo.log"

/* Two phases:
 *   1. before /mnt/usb0 is reachable (boot, pre-self-unjail) — lines are held
 *      in g_buf. A hard cap; overflow bumps g_dropped and is reported once the
 *      file opens.
 *   2. after evo_boot_log_flush() first opens the file — g_fp stays open and
 *      every line is written straight through (fflush per line; fsync only on
 *      the periodic flush from the render loop). No cap. */
#define BL_CAP 16384
static char   g_buf[BL_CAP];
static size_t g_len;
static int    g_dropped;
static FILE  *g_fp;

void evo_boot_log(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

#ifdef EVO_BOOT_TRACE_POPUP
    /* #51: on-screen popup — opt-in (--breadcrumbs). Off by default; the
     * EVO_LOG_PATH file is the durable channel and is always written. */
    struct bl_note n;
    memset(&n, 0, sizeof n);
    snprintf(n.msg, sizeof n.msg, "%s", line);
    sceKernelSendNotificationRequest(0, &n, sizeof n, 0);
#endif

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char stamped[680];
    int m = snprintf(stamped, sizeof stamped, "[%lld.%03ld] %s\n",
                     (long long)ts.tv_sec, ts.tv_nsec / 1000000L, line);
    if (m < 0)
        return;

    if (g_fp) {
        fwrite(stamped, 1, (size_t)m, g_fp);
        fflush(g_fp);
        return;
    }
    if (g_len + (size_t)m < BL_CAP) {
        memcpy(g_buf + g_len, stamped, (size_t)m);
        g_len += (size_t)m;
    } else {
        g_dropped++;
    }
}

void evo_boot_log_flush(void)
{
    if (!g_fp) {
        g_fp = fopen(EVO_LOG_PATH, "a");
        if (!g_fp)
            return;   /* /mnt/usb0 not reachable yet — try again next call */
        if (g_dropped) {
            fprintf(g_fp, "[log] %d pre-mount line(s) dropped\n", g_dropped);
            g_dropped = 0;
        }
        if (g_len) {
            fwrite(g_buf, 1, g_len, g_fp);
            g_len = 0;
        }
    }
    fflush(g_fp);
    fsync(fileno(g_fp));
}

#else  /* host / payload */

void evo_boot_log(const char *fmt, ...) { (void)fmt; }
void evo_boot_log_flush(void) {}

#endif
