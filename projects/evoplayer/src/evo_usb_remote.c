/*
 * evo_usb_remote.c — see evo_usb_remote.h. Real body only under
 * EVO_USB_REMOTE + EVO_APP_MODULE.
 */
#include "evo_usb_remote.h"

#if defined(EVO_USB_REMOTE) && defined(EVO_APP_MODULE)

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "evo_playback.h"
#include "evo_demux.h"
#include "pp_stage_breadcrumb.h"

#ifdef EVO_HAVE_BUILD_ID
#include "evo_build_id.h"
#else
#define EVO_BUILD_ID "unknown"
#endif

#define CMD_PATH    "/mnt/usb0/evo_cmd"
#define STATUS_PATH "/mnt/usb0/evo_status"

extern int    screen;   /* main.c */

static long long now_ms_local(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
        s[--n] = 0;
}

static void run_command(const char *line)
{
    char buf[600];
    snprintf(buf, sizeof buf, "%s", line);
    trim(buf);
    if (!buf[0])
        return;

    pp_stage_bc("REMOTE_CMD", buf);

    if (strncmp(buf, "play ", 5) == 0) {
        evo_open_media_path(buf + 5);
        return;
    }
    if (strncmp(buf, "seek ", 5) == 0) {
        const char *arg = buf + 5;
        double target;
        if (arg[0] == '+' || arg[0] == '-') {
            double rel = atof(arg);              /* atof reads the sign */
            target = evo_pb_position_s() + rel;
        } else {
            target = atof(arg);
        }
        if (target < 0.0)
            target = 0.0;
        prospero_request_inplace_seek(target, 0 /* keep playing */);
        return;
    }
    pp_stage_bc("REMOTE_CMD", "unknown");
}

void evo_usb_remote_poll(void)
{
    static long long last_status = 0;
    long long now = now_ms_local();

    /* Command file — consume then delete so each command runs once. */
    FILE *cf = fopen(CMD_PATH, "r");
    if (cf) {
        char line[600];
        if (fgets(line, sizeof line, cf))
            run_command(line);
        fclose(cf);
        remove(CMD_PATH);
    }

    /* Status once a second. */
    if (now - last_status < 1000)
        return;
    last_status = now;

    FILE *sf = fopen(STATUS_PATH, "w");
    if (!sf)
        return;
    fprintf(sf,
            "build=%s t=%lld scr=%d be=%d pos=%.2f dur=%.1f fps=%.1f "
            "fatal=%d eof=%d active=%d\n",
            EVO_BUILD_ID, now / 1000, screen, evo_pb_active_backend(),
            evo_pb_position_s(), evo_pb_duration_s(), evo_pb_video_fps(),
            evo_pb_decode_fatal(), evo_pb_is_eof(), evo_pb_is_active());
    fclose(sf);
}

#endif /* EVO_USB_REMOTE && EVO_APP_MODULE */
