/* evo_boot_log.c — see evo_boot_log.h. */
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

#define BL_CAP 16384
static char  g_buf[BL_CAP];
static size_t g_len;
static int    g_flushed_bytes;   /* how much of g_buf is already on disk */

void evo_boot_log(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

#ifdef EVO_BOOT_TRACE_POPUP
    /* #51: on-screen popup - opt-in (--breadcrumbs). Off by default; the
     * buffered /mnt/usb0/evo_boot.log below is the durable channel. */
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
    if (g_len + (size_t)m < BL_CAP) {
        memcpy(g_buf + g_len, stamped, (size_t)m);
        g_len += (size_t)m;
    }
}

void evo_boot_log_flush(void)
{
    if ((int)g_len <= g_flushed_bytes)
        return;
    FILE *f = fopen("/mnt/usb0/evo_boot.log", "a");
    if (!f)
        return;   /* sandbox not open yet — try again next call */
    fwrite(g_buf + g_flushed_bytes, 1, g_len - (size_t)g_flushed_bytes, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    g_flushed_bytes = (int)g_len;
}

#else  /* host / payload */

void evo_boot_log(const char *fmt, ...) { (void)fmt; }
void evo_boot_log_flush(void) {}

#endif
