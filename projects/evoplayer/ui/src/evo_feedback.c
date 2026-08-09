#include "evo_feedback.h"
#include "evo_theme.h"

#include <string.h>

/* ---- libScePad ---------------------------------------------------------- */

/*
 * Declared locally rather than pulled from a header: the payload SDK ships
 * libScePad.so with these exports (verified with nm) but no public prototype
 * for them, and the struct is stable across the PS4/PS5 pad API.
 */
typedef struct ScePadColor {
    uint8_t r, g, b, a;
} ScePadColor;

int scePadSetLightBar(int handle, const ScePadColor *param);
int scePadResetLightBar(int handle);

/* ---- patterns ----------------------------------------------------------- */

/*
 * Sound is the only per-event channel now, so a "pattern" is just which blip
 * to play. The mapping is kept as a table anyway: it puts every event's
 * treatment on one screen, which is what made the missing BOUNDARY cue
 * obvious when the haptic channel was removed.
 *
 * These values match main.c's enum { EVO_SFX_NONE, MOVE, CONFIRM, BACK,
 * TOGGLE }. Kept as plain integers so this module does not have to include
 * main.c's private declarations.
 */
#define SFX_NONE    0
#define SFX_MOVE    1
#define SFX_CONFIRM 2
#define SFX_BACK    3
#define SFX_TOGGLE  4

static const int PATTERN_SFX[EVO_FB_COUNT] = {
    /* MOVE     */ SFX_MOVE,
    /* WRAP     */ SFX_MOVE,
    /* BOUNDARY */ SFX_BACK,
    /* CONFIRM  */ SFX_CONFIRM,
    /* OPEN     */ SFX_CONFIRM,
    /* CANCEL   */ SFX_BACK,
    /* TOGGLE   */ SFX_TOGGLE,
    /* ERROR    */ SFX_BACK
};

/* ---- state -------------------------------------------------------------- */

static int   g_pad = -1;
static void (*g_sfx)(int);

static int   g_sound_on    = 1;
static int   g_lightbar_on = 1;

/* Lightbar flash overlay. */
static uint64_t g_flash_until_ms;
static uint8_t  g_flash_r, g_flash_g, g_flash_b;
static uint32_t g_lightbar_last;   /* what we last pushed, to avoid resends */

/* ---- helpers ------------------------------------------------------------ */

static void push_lightbar(uint8_t r, uint8_t g, uint8_t b)
{
    ScePadColor c;
    uint32_t    key = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;

    if (g_pad < 0) return;
    if (key == g_lightbar_last) return;   /* the pad does not need a resend */

    c.r = r;
    c.g = g;
    c.b = b;
    c.a = 255;

    scePadSetLightBar(g_pad, &c);
    g_lightbar_last = key;
}

/* Theme colours are packed 0xAABBGGRR to match the framebuffer. */
static void theme_accent_rgb(uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint32_t a = evo_theme_current()->accent;

    *r = (uint8_t)( a        & 0xFF);
    *g = (uint8_t)((a >>  8) & 0xFF);
    *b = (uint8_t)((a >> 16) & 0xFF);
}

/* ---- api ---------------------------------------------------------------- */

void evo_feedback_init(int pad_handle, void (*sfx)(int))
{
    g_pad           = pad_handle;
    g_sfx           = sfx;
    g_lightbar_last = 0xFFFFFFFFu;   /* force the first push through */

    evo_feedback_refresh_lightbar();
}

void evo_feedback_set_pad(int pad_handle)
{
    if (g_pad == pad_handle) return;

    g_pad           = pad_handle;
    g_lightbar_last = 0xFFFFFFFFu;

    evo_feedback_refresh_lightbar();
}

void evo_feedback(evo_feedback_kind kind)
{
    if (kind < 0 || kind >= EVO_FB_COUNT) return;

    if (g_sound_on && g_sfx && PATTERN_SFX[kind] != SFX_NONE)
        g_sfx(PATTERN_SFX[kind]);

    if (kind == EVO_FB_CONFIRM || kind == EVO_FB_OPEN) {
        /* A short brightening rather than a colour change: the lightbar
         * should still read as "this is the theme accent". */
        uint8_t r, g, b;
        theme_accent_rgb(&r, &g, &b);
        g_flash_r = (uint8_t)(r + (255 - r) / 2);
        g_flash_g = (uint8_t)(g + (255 - g) / 2);
        g_flash_b = (uint8_t)(b + (255 - b) / 2);
        g_flash_until_ms = 0;   /* set on the next tick, which has the clock */
    } else if (kind == EVO_FB_ERROR) {
        g_flash_r = 220; g_flash_g = 40; g_flash_b = 40;
        g_flash_until_ms = 0;
    }
}

void evo_feedback_tick(uint64_t now_ms)
{
    if (!g_lightbar_on || g_pad < 0) return;

    if (g_flash_until_ms == 0 && (g_flash_r || g_flash_g || g_flash_b))
        g_flash_until_ms = now_ms + 130;

    if (g_flash_until_ms != 0 && now_ms < g_flash_until_ms) {
        push_lightbar(g_flash_r, g_flash_g, g_flash_b);
    } else {
        uint8_t r, g, b;

        if (g_flash_until_ms != 0) {
            g_flash_until_ms = 0;
            g_flash_r = g_flash_g = g_flash_b = 0;
        }

        theme_accent_rgb(&r, &g, &b);
        push_lightbar(r, g, b);
    }
}

void evo_feedback_set_sound(int enabled) { g_sound_on = enabled ? 1 : 0; }

void evo_feedback_set_lightbar(int enabled)
{
    g_lightbar_on = enabled ? 1 : 0;

    if (!g_lightbar_on && g_pad >= 0) {
        scePadResetLightBar(g_pad);
        g_lightbar_last = 0xFFFFFFFFu;
    } else {
        evo_feedback_refresh_lightbar();
    }
}

int evo_feedback_sound_enabled(void)    { return g_sound_on; }
int evo_feedback_lightbar_enabled(void) { return g_lightbar_on; }

void evo_feedback_refresh_lightbar(void)
{
    uint8_t r, g, b;

    if (!g_lightbar_on || g_pad < 0) return;

    theme_accent_rgb(&r, &g, &b);
    push_lightbar(r, g, b);
}
