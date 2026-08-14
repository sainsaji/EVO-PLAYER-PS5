/*
 * Module: evo_toast
 *
 * State, classification, and renderer for in-app toast notifications.
 * Extracted from main.c (PROSPERO_TOAST_STATE_START / _END and
 * PROSPERO_TOAST_RENDERER_START / _END).
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "evo_toast.h"

/* Shared player globals this module needs read access to. */
extern int show_debug_overlay;

/* PlaybackProfile — mirrored here to avoid a circular include with main.c.
 * Only PROFILE_DEBUG is tested; the numeric value must match the enum. */
#define _PROFILE_DEBUG 3
extern int current_profile;

/* now_ms() — declared in main.c, linked into the same binary. */
extern long long now_ms(void);

/* evo_widget_toast — UI layer renderer, declared in evo_widgets.h. */
#include "evo_widgets.h"

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */

static char      prospero_toast_title[96]   = {0};
static char      prospero_toast_message[256] = {0};
static long long prospero_toast_started_ms  = 0;
static int       prospero_toast_active      = 0;
static int       prospero_toast_kind        = 0;  /* 0=info 1=technical 2=error */

/* --------------------------------------------------------------------------
 * Classification helpers
 * -------------------------------------------------------------------------- */

static int prospero_text_contains_ci(const char *text, const char *needle)
{
    if (!text || !needle || !needle[0])
        return 0;

    size_t needle_length = strlen(needle);
    for (const char *cursor = text; *cursor; cursor++) {
        if (strncasecmp(cursor, needle, needle_length) == 0)
            return 1;
    }
    return 0;
}

static int prospero_toast_is_technical(const char *title, const char *message)
{
    static const char *technical_titles[] = {
        "STREAM INFO", "AUDIO STREAM", "FIRST FRAME",
        "MKV OPEN",   "MKV INFO",     "DECODER"
    };

    for (size_t i = 0;
         i < sizeof(technical_titles) / sizeof(technical_titles[0]);
         i++) {
        if (title && strcasecmp(title, technical_titles[i]) == 0)
            return 1;
    }

    if (title && strcasecmp(title, "AUDIO") == 0) {
        if (prospero_text_contains_ci(message, "OK")      ||
            prospero_text_contains_ci(message, "THREAD")  ||
            prospero_text_contains_ci(message, "DECODER"))
            return 1;
    }

    if (title && strcasecmp(title, "PROFILE") == 0)
        return 1;

    if (title && (prospero_text_contains_ci(title,   "SELECTED") ||
                  prospero_text_contains_ci(message, "SELECTED")))
        return 1;

    return 0;
}

static int prospero_toast_is_error(const char *title, const char *message)
{
    static const char *error_terms[] = {
        "FAIL", "FAILED", "ERROR", "UNSUPPORTED",
        "NOT FOUND", "NO VIDEO", "NO AUDIO", "INVALID"
    };

    for (size_t i = 0;
         i < sizeof(error_terms) / sizeof(error_terms[0]);
         i++) {
        if (prospero_text_contains_ci(title,   error_terms[i]) ||
            prospero_text_contains_ci(message, error_terms[i]))
            return 1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void toast(const char *title, const char *msg)
{
    if (!title) title = "EVO PLAYER";
    if (!msg)   msg   = "";

    int technical   = prospero_toast_is_technical(title, msg);
    int error       = prospero_toast_is_error(title, msg);
    int debug_enabled = show_debug_overlay || (current_profile == _PROFILE_DEBUG);

    /*
     * Normal users do not need decoder initialisation,
     * stream-count or successful-thread messages.
     */
    if (technical && !error && !debug_enabled)
        return;

    snprintf(prospero_toast_title,   sizeof(prospero_toast_title),   "%s", title);
    snprintf(prospero_toast_message, sizeof(prospero_toast_message), "%s", msg);

    prospero_toast_started_ms = now_ms();
    prospero_toast_active     = 1;
    prospero_toast_kind       = error ? 2 : technical ? 1 : 0;

    /*
     * EVO: the console's own notification banner is deliberately not used.
     *
     * This used to also fire sceNotificationSend() for technical messages,
     * which put a PlayStation system toast over the app — the OS chrome, the
     * OS position, the OS timing, none of it themeable and none of it
     * dismissable by us. The player has its own toast, it is themed, and it
     * is drawn inside our own frame; there is no reason for a second
     * notification system that we do not control.
     */
}

void draw_prospero_toast(uint32_t *fb)
{
    /*
     * Timing and state live here; every pixel is evo_widget_toast's.
     *
     * The old renderer built the panel out of eight hardcoded literals —
     * a cyan border, a near-black fill, pale blue text — so it stayed cyan
     * under CARBON, EMBER and AURORA. It was the last thing on screen that
     * ignored the theme.
     */
    const int hold_ms  = 2200;
    const int fade_ms  = 360;
    const int slide_ms = 180;
    const int slide_px = 90;

    long long elapsed;
    evo_toast t;

    if (!prospero_toast_active) return;

    elapsed = now_ms() - prospero_toast_started_ms;

    if (elapsed >= hold_ms + fade_ms) {
        prospero_toast_active = 0;
        return;
    }

    memset(&t, 0, sizeof(t));
    t.title   = prospero_toast_title;
    t.message = prospero_toast_message;
    t.kind    = (prospero_toast_kind == 2) ? EVO_TOAST_ERROR : EVO_TOAST_INFO;

    t.alpha = (elapsed > hold_ms)
        ? 255 - (int)((elapsed - hold_ms) * 255 / fade_ms)
        : 255;
    if (t.alpha < 0) t.alpha = 0;

    t.slide = (elapsed < slide_ms)
        ? slide_px - (int)(elapsed * slide_px / slide_ms)
        : 0;

    evo_widget_toast(fb, &t);
}
