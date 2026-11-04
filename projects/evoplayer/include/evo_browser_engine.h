/*
 * The browser engine seam (#237). The engine (ProsperoWeb now, the WPE GPU
 * engine later) runs outside EVO as separate payload processes; EVO only
 * starts and stops them, shows their frames and passes input in.
 *
 * Phase 0, spike S1 - lifetime only. EVO sends ProsperoWeb's broker ELF to
 * elfldr, connects to the broker on 127.0.0.1:17771 and keeps that one
 * connection for as long as the engine should live. The broker kills every
 * child and exits when the connection closes, so closing it is the stop, and
 * a killed EVO (PS button, `evo-remote.sh close`) stops it the same way.
 *
 * Dev trigger for the spike: /mnt/usb0/evo_browser_spike. Its first line may
 * name the broker ELF (default /mnt/usb0/prosperoweb-broker.elf).
 */
#ifndef EVO_BROWSER_ENGINE_H
#define EVO_BROWSER_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

#define EVO_BROWSER_BROKER_PORT 17771
/* Where the engine's two ELFs are looked for, in order (until they are
 * bundled in the .ffpfsc). */
#define EVO_BROWSER_ELF_DIRS { "/mnt/usb0", "/data/evoplayer/browser" }

/* At boot: starts the engine in the background when the spike trigger file
 * exists. Never blocks. */
void evo_browser_engine_boot(void);

/* Starts the engine in the background (send the broker, connect, ping).
 * Never blocks. 0 when the start was begun, -1 when it already runs or the
 * ELF could not be read. */
int evo_browser_engine_start(const char *broker_elf_path);

/* 1 once the broker answered the ping and EVO holds its connection. */
int evo_browser_engine_running(void);

/* Soft close, first frame: drop the broker connection so it terminates its
 * children and exits. Never blocks. */
void evo_browser_engine_stop(void);

/* Soft close, before parking: wait up to timeout_ms for the start worker to
 * finish and for 17771 to stop answering. 0 when the broker is gone (or was
 * never started), -1 when it still answers. Only acts if EVO started one. */
int evo_browser_engine_wait_stopped(int timeout_ms);

/* ---- frames (spike S2b; wire format in evo_browser_wire.h) ----
 * Main thread only. A frame is one of the host's shared buffers, complete,
 * plus the rectangles that changed since the host last painted it. */
typedef struct {
    int buf;
    unsigned seq;
    const unsigned char *pixels;   /* RGBA8 premultiplied, top row first */
    int width, height, stride;
    int nrects;
    int rects[16][4];              /* x, y, w, h */
} evo_browser_frame_t;

/* 1 while a host has sent its buffers and its channel is open. */
int evo_browser_frame_shown(void);
/* The newest frame not yet taken: 1 and *out, else 0. Hand it back with
 * evo_browser_frame_release() once copied. */
int evo_browser_frame_acquire(evo_browser_frame_t *out);
void evo_browser_frame_release(int buf);
/* Called by the drawer once per copied frame, for the log's statistics. */
void evo_browser_frame_note_copy(long long copy_us, long long bytes);
/* 1 only in the spike (trigger file): frames then draw full screen over
 * everything; otherwise the Browser page screen draws them. */
int evo_browser_spike_overlay(void);

/* ---- Phase 2: what the Browser section uses ----
 * Main thread. None of these block: commands are sent with MSG_DONTWAIT and
 * dropped (and logged) if the engine is not reading. */

typedef enum {
    EVO_BROWSER_OFF = 0,       /* never opened */
    EVO_BROWSER_STARTING = 1,  /* broker and engine host on their way */
    EVO_BROWSER_READY = 2,     /* frames and commands flow */
    EVO_BROWSER_FAILED = 3,    /* could not start, or the engine went away */
} evo_browser_state_t;

/* Starts the engine in the background unless it runs or is starting.
 * Needs prosperoweb-broker.elf and prosperoweb.elf (EVO_BROWSER_ELF_DIRS)
 * and an ELF loader on 127.0.0.1:9021. */
void evo_browser_open(void);
/* The state; for FAILED, a short reason for the user in why. */
evo_browser_state_t evo_browser_state(char *why, int why_cap);

/* Load a URL now, or as soon as the engine is ready. */
void evo_browser_load(const char *url);
void evo_browser_nav(int op);   /* EVOB_NAV_BACK / _FORWARD / _RELOAD / _STOP */
/* Page pixels; action EVOB_PTR_MOVE / _DOWN / _UP, button 1 left. */
void evo_browser_pointer(int x, int y, int action, int button);
void evo_browser_scroll(int x, int y, int dx, int dy);
void evo_browser_key(unsigned key_code, unsigned modifiers, int pressed, const char *text);
void evo_browser_text(const char *utf8);
/* The page size; at most the size of the engine's buffers (1920x1080). */
void evo_browser_resize(int width, int height);
/* 0 while EVO plays a video or leaves the page: the engine stops painting. */
void evo_browser_set_visible(int visible);

/* Page events, oldest first. kind is an EVOB_EV_* value. */
typedef struct {
    int kind;
    unsigned arg;
    char text[1024];   /* cut to fit */
} evo_browser_event_t;
int evo_browser_poll_event(evo_browser_event_t *ev);

#ifdef __cplusplus
}
#endif

#endif
