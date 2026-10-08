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
#include "evo_rmlui_devstate.h"
#include "evo_keyboard.h"

#ifdef EVO_HAVE_BUILD_ID
#include "evo_build_id.h"
#else
#define EVO_BUILD_ID "unknown"
#endif

#define CMD_PATH    "/mnt/usb0/evo_cmd"
#define STATUS_PATH "/mnt/usb0/evo_status"
#define UI_PATH     "/mnt/usb0/evo_ui.json"
#define UI_TMP_PATH "/mnt/usb0/evo_ui.json.tmp"

extern int    screen;   /* main.c */
extern void   evo_iobench_run(const char *spec);   /* evo_iobench.c */

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

/*
 * Synthetic pad input. Button values mirror PadButtons in core/include/evo/
 * Common.hpp - kept as literals because this is C and that header is C++.
 */
static volatile unsigned int g_inject_buttons;

static const struct { const char *name; unsigned int mask; } k_keys[] = {
    { "up",       0x0010 }, { "right",    0x0020 },
    { "down",     0x0040 }, { "left",     0x0080 },
    { "l2",       0x0100 }, { "r2",       0x0200 },
    { "l1",       0x0400 }, { "r1",       0x0800 },
    { "triangle", 0x1000 }, { "circle",   0x2000 },
    { "cross",    0x4000 }, { "square",   0x8000 },
    { "options",  0x0008 }, { "l3",       0x0002 },
    { "r3",       0x0004 },
    { "touchpad",       0x00100000 },
    { "touchpad_left",  0x00200000 },
    { "touchpad_right", 0x00400000 },
    /* aliases for whoever is typing them by hand */
    { "x",        0x4000 }, { "o",        0x2000 },
    { "t",        0x1000 }, { "sq",       0x8000 },
    { "pad_left", 0x00200000 }, { "pad_right", 0x00400000 },
    { "shot",     0x0002 },   /* l3 - the frame loop's screenshot handler */
};

unsigned int evo_usb_remote_take_buttons(void)
{
    unsigned int m = g_inject_buttons;
    g_inject_buttons = 0;
    return m;
}

static int inject_key(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof k_keys / sizeof k_keys[0]; ++i) {
        if (strcmp(name, k_keys[i].name) == 0) {
            g_inject_buttons |= k_keys[i].mask;
            return 1;
        }
    }
    return 0;
}

static void run_command(const char *line)
{
    char buf[600];
    snprintf(buf, sizeof buf, "%s", line);
    trim(buf);
    if (!buf[0])
        return;

    pp_stage_bc("REMOTE_CMD", buf);

    if (strncmp(buf, "image ", 6) == 0) {
        evo_remote_open_image(buf + 6);
        return;
    }
    if (strncmp(buf, "text ", 5) == 0) {
        evo_remote_open_text(buf + 5);
        return;
    }
    if (strncmp(buf, "screen ", 7) == 0) {
        evo_remote_goto_screen(atoi(buf + 7));
        return;
    }
    if (strncmp(buf, "source ", 7) == 0) {
        evo_remote_browser_source(atoi(buf + 7));
        return;
    }
    if (strncmp(buf, "key ", 4) == 0) {
        if (!inject_key(buf + 4))
            pp_stage_bc("REMOTE_KEY?", buf + 4);
        return;
    }
    /*
     * `type <text>` fills the virtual keyboard in one shot, instead of walking
     * its grid with ~5 D-pad presses per character. `kbdone` is its DONE key.
     * Both no-op on the native PS5 IME, which is a system dialog EVO cannot
     * reach - switch KEYBOARD INPUT to VIRTUAL KEYBOARD for unattended tests.
     */
    if (strncmp(buf, "type ", 5) == 0) {
        if (!evo_keyboard_queue_text(buf + 5))
            pp_stage_bc("REMOTE_TYPE?", "no virtual keyboard open");
        return;
    }
    if (strcmp(buf, "kbdone") == 0) {
        if (!evo_keyboard_queue_submit())
            pp_stage_bc("REMOTE_TYPE?", "no virtual keyboard open");
        return;
    }
    /* `kbclose` cancels whichever keyboard is open, the native IME included:
     * once the system dialog is up it takes every pad press, so `key` can no
     * longer reach EVO and a scripted run is stranded until someone presses
     * Circle on a real controller. */
    if (strcmp(buf, "kbclose") == 0) {
        if (evo_keyboard_is_open())
            evo_keyboard_close();
        else
            pp_stage_bc("REMOTE_KBCLOSE?", "no keyboard open");
        return;
    }
    if (strncmp(buf, "play ", 5) == 0) {
        evo_open_media_path(buf + 5);
        return;
    }
    if (strncmp(buf, "iobench", 7) == 0) {
        const char *arg = buf + 7;
        while (*arg == ' ')
            ++arg;
        evo_iobench_run(arg);
        return;
    }
    if (strcmp(buf, "upcompare") == 0) {
        evo_remote_upscale_compare();
        return;
    }
    if (strcmp(buf, "dbcompare") == 0) {
        evo_remote_deepblack_compare();
        return;
    }
    if (strcmp(buf, "stop") == 0) {
        evo_stop_media_playback();
        return;
    }
    if (strcmp(buf, "quit") == 0) {
        evo_remote_soft_close();
        return;
    }
    if (strncmp(buf, "seek ", 5) == 0) {
        const char *arg = buf + 5;
        double target;
        if (arg[0] == '+' || arg[0] == '-') {
            double rel = atof(arg);              /* atof reads the sign */
            /* Absolute position, not evo_pb_position_s(): the raw clock
             * restarts at 0 on a seek, so relative seeks compounded off it
             * landed nowhere near where they were asked to. */
            target = evo_player_position_s() + rel;
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

/*
 * parked=1 is written exactly once, by evo_usb_remote_mark_parked(), as the
 * soft close stops the frame loop - after that t= never moves again. It is the
 * one state in which tools/evo-remote.sh will send the title-aware close
 * controller: the GPU is drained, nothing is submitting.
 */
static void write_status(long long now, int parked)
{
    FILE *sf = fopen(STATUS_PATH, "w");
    if (!sf)
        return;
    fprintf(sf,
            "build=%s t=%lld scr=%d be=%d pos=%.2f dur=%.1f fps=%.1f "
            "fatal=%d eof=%d active=%d parked=%d\n",
            EVO_BUILD_ID, now / 1000, screen, evo_pb_active_backend(),
            evo_player_position_s(), evo_pb_duration_s(), evo_pb_video_fps(),
            evo_pb_decode_fatal(), evo_pb_is_eof(), evo_pb_is_active(), parked);
    fclose(sf);
}

/*
 * #115: /mnt/usb0/evo_ui.json - the screen, the focused element, modal, toast
 * and player OSD state as text, so the remote can navigate without a
 * screenshot. The DOM is sampled at 10 Hz and the file is written only when
 * the document differs from the last one written; an idle UI writes nothing.
 * `seq` goes up by one per write, which is how `evo-remote.sh key` tells the
 * state its press produced from the state before it.
 *
 * Written to a temp file and renamed, so an FTP RETR never reads half of it.
 */
static void write_ui_state(long long now)
{
    static long long last_sample = 0;
    static char     *last_json;
    static unsigned  seq;
    const char      *json;
    FILE            *f;

    if (now - last_sample < 100)
        return;
    last_sample = now;

    json = evo_rmlui_dev_ui_json(screen, evo_pb_is_paused());
    if (last_json && strcmp(json, last_json) == 0)
        return;
    free(last_json);
    {
        size_t n = strlen(json) + 1;   /* no strdup: see native-libc gaps */
        last_json = malloc(n);
        if (last_json)
            memcpy(last_json, json, n);
    }

    f = fopen(UI_TMP_PATH, "w");
    if (!f)
        return;
    /* seq/t first and outside the compared body: they change on every write. */
    fprintf(f, "{\"seq\":%u,\"t_ms\":%lld,\"ui\":%s}\n", ++seq, now, json);
    fclose(f);
    if (rename(UI_TMP_PATH, UI_PATH) != 0) {
        remove(UI_PATH);
        rename(UI_TMP_PATH, UI_PATH);
    }
}

void evo_usb_remote_poll(void)
{
    static long long last_status = 0;
    long long now = now_ms_local();

    /*
     * Command file — consume then delete so each command runs once.
     *
     * The frame loop polls ~300 times a second and the writer is an FTP STOR,
     * so the file is routinely opened while it is still being written: the
     * read then yields nothing (or half a path), the command is lost, and the
     * remove() hides the evidence. Only consume a COMPLETE line — one that
     * arrived with its terminating newline. Anything else is a write still in
     * flight, and is left alone for the next poll.
     */
    FILE *cf = fopen(CMD_PATH, "r");
    if (cf) {
        char line[600];
        int  got = (fgets(line, sizeof line, cf) != NULL);
        int  complete = got && strchr(line, '\n') != NULL;
        /* A full buffer with no newline is not a partial write, it is junk —
         * drop it rather than wedging the remote on it forever. */
        int  junk = got && !complete && strlen(line) >= sizeof line - 1;
        fclose(cf);
        if (complete) {
            remove(CMD_PATH);
            run_command(line);
        } else if (junk) {
            remove(CMD_PATH);
        }
    }

    write_ui_state(now);

    /* Status once a second. */
    if (now - last_status < 1000)
        return;
    last_status = now;
    write_status(now, 0);
}

void evo_usb_remote_mark_parked(void)
{
    write_status(now_ms_local(), 1);
}

#endif /* EVO_USB_REMOTE && EVO_APP_MODULE */
