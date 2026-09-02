#include "pp_stage_breadcrumb.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static uint64_t bc_now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

/*
 * App-module diagnostic channel. In the PPSA99039 sandbox /mnt/usb0 is ENOENT,
 * so the file writes below silently no-op and the whole breadcrumb trail is
 * invisible. When EVO_APP_MODULE is set (scripts/package-app.sh), also push
 * every checkpoint to the system-notification popup + the kernel log - the
 * only channels that reach out of the sandbox. This lights up the existing
 * 001..012 playback trail with no new call sites; Phase 1b task 8.
 */
#if defined(EVO_APP_MODULE)
struct pp_bc_note { char pad[45]; char msg[3075]; };
extern int sceKernelSendNotificationRequest(int, void *, unsigned long, int);
extern int sceKernelDebugOutText(int, const char *);

static void pp_bc_notify(const char *stage_id, const char *detail)
{
    struct pp_bc_note n;
    char line[512];

    memset(&n, 0, sizeof n);
    snprintf(n.msg, sizeof n.msg, "EVO bc: %s%s%s", stage_id,
             (detail && detail[0]) ? " " : "", detail ? detail : "");
    sceKernelSendNotificationRequest(0, &n, sizeof n, 0);

    snprintf(line, sizeof line, "%s\n", n.msg);
    sceKernelDebugOutText(0, line);
}
#else
#define pp_bc_notify(a, b) ((void)0)
#endif

void pp_stage_bc(const char *stage_id, const char *detail)
{
    FILE *f;
    if (!stage_id)
        return;
    pp_bc_notify(stage_id, detail);
    f = fopen(PP_STAGE_BC_PATH, "a");
    if (!f)
        return;
    fprintf(f, "%llu %s%s%s\n", (unsigned long long)bc_now_us(), stage_id,
            (detail && detail[0]) ? " " : "", detail ? detail : "");
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

void pp_stage_bc_checkpoint(const char *stage_id, const char *detail)
{
    FILE *f;
    /* Append full history (also notifies in the app module). */
    pp_stage_bc(stage_id, detail);
    /* Last-alive single-line file for quick pull */
    f = fopen("/mnt/usb0/pp_4k_stage_last.txt", "w");
    if (!f)
        return;
    fprintf(f, "last=%s detail=%s ts=%llu\n", stage_id, detail ? detail : "",
            (unsigned long long)bc_now_us());
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}
