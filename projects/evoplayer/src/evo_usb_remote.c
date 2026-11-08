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
#include <pthread.h>

#include "evo_playback.h"
#include "evo_demux.h"
#include "pp_stage_breadcrumb.h"
#include "evo_rmlui_devstate.h"
#include "evo_keyboard.h"
#include "evo_error.h"

#ifdef EVO_HAVE_BUILD_ID
#include "evo_build_id.h"
#else
#define EVO_BUILD_ID "unknown"
#endif

#define CMD_PATH        "/mnt/usb0/evo_cmd"
#define CMD_FALLBACK    "/data/evoplayer/evo_cmd"
#define STATUS_PATH     "/mnt/usb0/evo_status"
#define STATUS_FALLBACK "/data/evoplayer/evo_status"
#define UI_PATH         "/mnt/usb0/evo_ui.json"
#define UI_TMP_PATH     "/mnt/usb0/evo_ui.json.tmp"
#define UI_FALLBACK     "/data/evoplayer/evo_ui.json"
#define UI_TMP_FALLBACK "/data/evoplayer/evo_ui.json.tmp"

extern int    screen;   /* main.c */
extern void   evo_iobench_run(const char *spec);   /* evo_iobench.c */
extern void   evo_pt_probe_run(const char *spec);  /* evo_pt_probe.c */
extern void   evo_hui_web_pointer(int x, int y);   /* ui_kit/src/evo_hui_web.cpp */
extern void   evo_hui_web_open(const char *url);   /* ui_kit/src/evo_hui_web.cpp */

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

    char safe_buf[600];
    evo_url_sanitize(buf, safe_buf, sizeof(safe_buf));
    pp_stage_bc("REMOTE_CMD", safe_buf);

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
    if (strncmp(buf, "set ", 4) == 0) {
        evo_remote_set(buf + 4);
        return;
    }
    if (strncmp(buf, "focus ", 6) == 0) {
        evo_remote_focus(buf + 6);
        return;
    }
    if (strncmp(buf, "provider ", 9) == 0) {
        evo_remote_open_provider(buf + 9);
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
    /* `pointer <x> <y>`: the Web section's free pointer to screen pixel (x, y)
     * of the 1920x1080 UI - the left stick, which `key` cannot send. Then
     * `key cross` clicks there (a text field opens EVO's keyboard). */
    if (strncmp(buf, "pointer ", 8) == 0) {
        int x = -1, y = -1;
        if (sscanf(buf + 8, "%d %d", &x, &y) == 2 && x >= 0 && x < 1920 && y >= 0 && y < 1080)
            evo_hui_web_pointer(x, y);
        else
            pp_stage_bc("REMOTE_POINTER?", buf + 8);
        return;
    }
    /* `weburl <url>`: open <url> in the Web section (any URL the address bar
     * takes, data: URLs included) - for engine tests without the keyboard. */
    if (strncmp(buf, "weburl ", 7) == 0) {
        evo_hui_web_open(buf + 7);
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
    if (strncmp(buf, "playshare ", 10) == 0) {
        evo_open_share_path(buf + 10);
        return;
    }
    if (strncmp(buf, "iobench", 7) == 0) {
        const char *arg = buf + 7;
        while (*arg == ' ')
            ++arg;
        evo_iobench_run(arg);
        return;
    }
    if (strncmp(buf, "ptprobe ", 8) == 0) {
        evo_pt_probe_run(buf + 8);
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
static void write_status_to(const char *path, long long now, int parked)
{
    FILE *sf = fopen(path, "w");
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

static void write_status(long long now, int parked)
{
    write_status_to(STATUS_PATH, now, parked);
    write_status_to(STATUS_FALLBACK, now, parked);
}

static void write_ui_file(const char *tmp_path, const char *path, unsigned seq, long long now, const char *json)
{
    FILE *f = fopen(tmp_path, "w");
    if (!f)
        return;
    /* seq/t first and outside the compared body: they change on every write. */
    fprintf(f, "{\"seq\":%u,\"t_ms\":%lld,\"ui\":%s}\n", seq, now, json);
    fclose(f);
    if (rename(tmp_path, path) != 0) {
        remove(path);
        rename(tmp_path, path);
    }
}

static pthread_t       g_remote_th;
static volatile int    g_remote_up = 0;
static pthread_mutex_t g_remote_mx = PTHREAD_MUTEX_INITIALIZER;
static char            g_remote_cmd[600];
static volatile int    g_remote_cmd_ready = 0;

static char           *g_remote_ui_json = NULL;
static unsigned        g_remote_ui_seq = 0;
static long long       g_remote_ui_now = 0;
static volatile int    g_remote_ui_pending = 0;

static int poll_cmd_file(const char *path, char *out, size_t cap)
{
    FILE *cf = fopen(path, "r");
    if (!cf)
        return 0;

    char line[600];
    int  got = (fgets(line, sizeof line, cf) != NULL);
    int  complete = got && strchr(line, '\n') != NULL;
    /* A full buffer with no newline is not a partial write, it is junk —
     * drop it rather than wedging the remote on it forever. */
    int  junk = got && !complete && strlen(line) >= sizeof line - 1;
    fclose(cf);
    if (complete) {
        remove(path);
        if (out && cap)
            snprintf(out, cap, "%s", line);
        return 1;
    } else if (junk) {
        remove(path);
    }
    return 0;
}

/*
 * Background worker thread: performs all disk/USB file operations off the main
 * frame loop. This completely insulates the 60/120 Hz render loop from USB
 * flash drive latencies, FAT32 cluster updates, and write flushes.
 */
static void *usb_remote_worker(void *arg)
{
    (void)arg;
    long long last_status = 0;
    long long last_usb_status = 0;
    long long last_usb_cmd_check = 0;

    while (g_remote_up) {
        long long now = now_ms_local();
        int active = evo_pb_is_active();

        /* 1. Poll command if main thread is ready to receive one */
        int need_cmd = 0;
        pthread_mutex_lock(&g_remote_mx);
        need_cmd = !g_remote_cmd_ready;
        pthread_mutex_unlock(&g_remote_mx);

        if (need_cmd) {
            char cmd[600];
            int got = poll_cmd_file(CMD_FALLBACK, cmd, sizeof cmd);
            if (!got) {
                /* When video is active, poll USB command file at relaxed cadence (500ms)
                 * instead of hammering /mnt/usb0 on every iteration */
                if (!active || (now - last_usb_cmd_check >= 500)) {
                    last_usb_cmd_check = now;
                    got = poll_cmd_file(CMD_PATH, cmd, sizeof cmd);
                }
            }
            if (got) {
                pthread_mutex_lock(&g_remote_mx);
                snprintf(g_remote_cmd, sizeof g_remote_cmd, "%s", cmd);
                g_remote_cmd_ready = 1;
                pthread_mutex_unlock(&g_remote_mx);
            }
        }

        /* 2. Write UI file if main thread staged an update */
        char *ui_json = NULL;
        unsigned ui_seq = 0;
        long long ui_now = 0;
        pthread_mutex_lock(&g_remote_mx);
        if (g_remote_ui_pending && g_remote_ui_json) {
            size_t len = strlen(g_remote_ui_json) + 1;
            ui_json = malloc(len);
            if (ui_json)
                memcpy(ui_json, g_remote_ui_json, len);
            ui_seq = g_remote_ui_seq;
            ui_now = g_remote_ui_now;
            g_remote_ui_pending = 0;
        }
        pthread_mutex_unlock(&g_remote_mx);

        if (ui_json) {
            write_ui_file(UI_TMP_FALLBACK, UI_FALLBACK, ui_seq, ui_now, ui_json);
            if (!active) {
                write_ui_file(UI_TMP_PATH, UI_PATH, ui_seq, ui_now, ui_json);
            }
            free(ui_json);
        }

        /* 3. Write status once a second */
        if (now - last_status >= 1000) {
            last_status = now;
            /* Internal storage status is always updated at 1 Hz (SSD, fast) */
            write_status_to(STATUS_FALLBACK, now, 0);

            /* USB status: during playback, throttle to once every 15 s so the
             * flash drive's FTL is not overwhelmed by continuous metadata updates.
             * In menus/idle, update at 1 Hz. */
            if (!active || (now - last_usb_status >= 15000)) {
                last_usb_status = now;
                write_status_to(STATUS_PATH, now, 0);
            }
        }

        usleep(50000);  /* 50 ms */
    }
    return NULL;
}

/*
 * #115: Sample DOM state at 10 Hz in memory. When it changes, stage it for the
 * worker thread to write out. Zero filesystem I/O happens on this thread.
 */
static void write_ui_state(long long now)
{
    static long long last_sample = 0;
    static char     *last_json = NULL;
    static unsigned  seq = 0;
    const char      *json;

    if (now - last_sample < 100)
        return;
    last_sample = now;

    json = evo_rmlui_dev_ui_json(screen, evo_pb_is_paused());
    if (last_json && strcmp(json, last_json) == 0)
        return;
    free(last_json);
    {
        size_t n = strlen(json) + 1;
        last_json = malloc(n);
        if (last_json)
            memcpy(last_json, json, n);
    }

    ++seq;
    pthread_mutex_lock(&g_remote_mx);
    free(g_remote_ui_json);
    g_remote_ui_json = NULL;
    if (last_json) {
        size_t n = strlen(last_json) + 1;
        g_remote_ui_json = malloc(n);
        if (g_remote_ui_json)
            memcpy(g_remote_ui_json, last_json, n);
    }
    g_remote_ui_seq = seq;
    g_remote_ui_now = now;
    g_remote_ui_pending = 1;
    pthread_mutex_unlock(&g_remote_mx);
}

void evo_usb_remote_poll(void)
{
    if (!g_remote_up) {
        g_remote_up = 1;
        if (pthread_create(&g_remote_th, NULL, usb_remote_worker, NULL) == 0) {
            pthread_detach(g_remote_th);
        }
    }

    /* Zero filesystem I/O on the main thread: dispatch any pending command */
    char cmd[600];
    int have_cmd = 0;
    pthread_mutex_lock(&g_remote_mx);
    if (g_remote_cmd_ready) {
        snprintf(cmd, sizeof cmd, "%s", g_remote_cmd);
        g_remote_cmd_ready = 0;
        have_cmd = 1;
    }
    pthread_mutex_unlock(&g_remote_mx);

    if (have_cmd) {
        run_command(cmd);
    }

    /* Sample UI in memory for background worker */
    long long now = now_ms_local();
    write_ui_state(now);
}

void evo_usb_remote_mark_parked(void)
{
    g_remote_up = 0;
    write_status(now_ms_local(), 1);
}

#endif /* EVO_USB_REMOTE && EVO_APP_MODULE */
