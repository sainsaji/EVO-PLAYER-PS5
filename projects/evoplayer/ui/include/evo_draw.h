/*
 * evo_draw — text, icons and controller glyphs, decoupled from their assets.
 *
 * The font atlas and the icon sheets are ~1MB of `static const` arrays living
 * in headers that only main.c includes. Including them from a second
 * translation unit would duplicate every byte, so instead main.c binds four
 * function pointers at startup and the whole UI layer draws through those.
 *
 * On top of that this header provides the layout helpers that keep text
 * placement honest:
 *
 *   - measurement, so things can be centred and right-aligned instead of
 *     eyeballed into place
 *   - ellipsised fitting, so a long filename truncates rather than running
 *     out of its card
 *   - ink-bounds vertical centring, which is what a run of glyph boxes
 *     actually needs. Positioning by box height put subtitles 11px below the
 *     bottom edge of their card in the previous UI, because the boxes carry
 *     leading the ink does not fill.
 */
#ifndef EVO_DRAW_H
#define EVO_DRAW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- type faces -------------------------------------------------------- */

typedef enum {
    EVO_FACE_SMALL = 0,   /* footers, badges, hints */
    EVO_FACE_SUB   = 1,   /* row descriptions, breadcrumbs */
    EVO_FACE_MENU  = 2,   /* row titles */
    EVO_FACE_TITLE = 3    /* page titles */
} evo_face;

/* ---- icon indices ------------------------------------------------------ */

/*
 * These index EVO_ICON_TABLE, which tools/gen_icons.py emits into
 * assets/evo_icons.h. Named here so screens stop passing bare integers
 * around; `rr_icon(fb, x, y, 6)` told nobody it was drawing a chevron.
 *
 * They used to match a `switch (idx)` written out by hand in main.c and again
 * in tools/uiview.c. Both are now table lookups, so adding an icon is one
 * edit to the generator and one line here - it can no longer be added to the
 * mock and forgotten in the player.
 */
enum {
    EVO_IC_USB       = 0,
    EVO_IC_RECENT    = 1,
    EVO_IC_FAVORITE  = 2,
    EVO_IC_SETTINGS  = 3,
    EVO_IC_TOOLS     = 4,
    EVO_IC_ABOUT     = 5,
    EVO_IC_CHEVRON   = 6,
    EVO_IC_RESUME    = 7,
    EVO_IC_ASPECT    = 8,
    EVO_IC_SUBTITLES = 9,
    EVO_IC_PALETTE   = 10,
    EVO_IC_FOLDER    = 11,
    EVO_IC_TRASH     = 12,
    EVO_IC_HOME      = 13,
    /* The application mark. Not a section icon - it is the app's own logo,
     * drawn at the top of the rail and in the launch header. */
    EVO_IC_LOGO      = 14,
    /* The Emby streaming service icon mark. */
    EVO_IC_EMBY      = 15
};

/* Controller glyph indices, matching rr_control(). */
enum {
    EVO_GLYPH_CROSS    = 0,
    EVO_GLYPH_DPAD     = 1,
    EVO_GLYPH_LSTICK   = 2,
    EVO_GLYPH_RSTICK   = 3,
    EVO_GLYPH_CIRCLE   = 4,
    EVO_GLYPH_TRIANGLE = 5,
    EVO_GLYPH_SQUARE   = 6
};

/* ---- binding ----------------------------------------------------------- */

typedef struct evo_draw_vtable {
    void (*text)(uint32_t *fb, int x, int y, const char *s,
                 uint32_t colour, int face);
    /* Advance width of `s` in `face`, in pixels. */
    int  (*text_w)(const char *s, int face);
    void (*icon)(uint32_t *fb, int x, int y, int index);
    /* Icons are monochrome, so an explicit tint is possible where the
     * theme accent is not what is wanted (a disabled rail item, say). */
    void (*icon_tinted)(uint32_t *fb, int x, int y, int index, uint32_t tint);
    void (*glyph)(uint32_t *fb, int x, int y, int index);
    /*
     * Controller glyphs are monochrome too, and the default tint is the theme
     * accent - which vanishes on anything filled with the accent. The hero's
     * RESUME chip is exactly that, and its cross was invisible whenever the
     * hero was selected.
     */
    void (*glyph_tinted)(uint32_t *fb, int x, int y, int index, uint32_t tint);
} evo_draw_vtable;

/* Call once, before the first frame. */
void evo_draw_bind(const evo_draw_vtable *v);

/* ---- primitives (safe no-ops before binding) --------------------------- */

void evo_text(uint32_t *fb, int x, int y, const char *s,
              uint32_t colour, evo_face face);
int  evo_text_w(const char *s, evo_face face);
void evo_icon(uint32_t *fb, int x, int y, int index);
void evo_icon_tinted(uint32_t *fb, int x, int y, int index, uint32_t tint);
void evo_glyph(uint32_t *fb, int x, int y, int index);
void evo_glyph_tinted(uint32_t *fb, int x, int y, int index, uint32_t tint);

/* ---- alignment --------------------------------------------------------- */

/* Draws so the string ends at `right_x`. */
void evo_text_right(uint32_t *fb, int right_x, int y, const char *s,
                    uint32_t colour, evo_face face);

/* Draws centred on `centre_x`. */
void evo_text_centre(uint32_t *fb, int centre_x, int y, const char *s,
                     uint32_t colour, evo_face face);

/*
 * Draws `s` clipped to `max_w`, appending ".." when it does not fit.
 * Returns the width actually drawn.
 */
int  evo_text_fit(uint32_t *fb, int x, int y, int max_w, const char *s,
                  uint32_t colour, evo_face face);

/*
 * A long label that scrolls back and forth when it does not fit. `phase` is
 * *milliseconds* since the caller last reset it (usually when the selection
 * changed) - not a frame count, so the scroll runs at one speed regardless of
 * what the render loop is managing. Falls back to a plain draw when it fits.
 */
void evo_text_marquee(uint32_t *fb, int x, int y, int max_w, const char *s,
                      uint32_t colour, evo_face face, int phase);

/* ---- vertical placement ------------------------------------------------ */

/*
 * The y to pass to evo_text() so that the *ink* of `face` sits centred in a
 * box of `box_h` starting at `box_y`. Use this instead of guessing offsets.
 */
int  evo_text_y_centred(int box_y, int box_h, evo_face face);

/*
 * Two stacked lines (title over subtitle) optically centred as a group in a
 * box. Writes both baselines. `gap` is the space between the two ink runs.
 */
void evo_text_y_stacked(int box_y, int box_h, evo_face top_face,
                        evo_face bottom_face, int gap,
                        int *out_top_y, int *out_bottom_y);

/* Height of the inked part of a face, in pixels. */
int  evo_face_ink_h(evo_face face);

/* ---- the character set ------------------------------------------------- */

/*
 * Text renders from two atlases now.
 *
 * The original one holds 69 glyphs - A-Z a-z 0-9 space / . _ : - + - and had
 * no comma, parenthesis, apostrophe or question mark. That was a real
 * constraint on UI copy: an unsupported character is not omitted, it advances
 * 12px and leaves a *hole*, so "CREDITS, PROJECT INFO" rendered as
 * "CREDITS  PROJECT INFO" and read as a spacing bug rather than a missing
 * glyph. Four strings shipped that way before anyone noticed.
 *
 * tools/gen_icons.py now generates a second atlas carrying the punctuation,
 * so ordinary prose renders - which is what made the text reader possible.
 * EVO_FONT_PUNCT_CHARS comes from that generator, so this set cannot drift
 * away from what actually draws.
 *
 * There is still a limit: anything outside the two sets leaves a hole. Check
 * with evo_text_unsupported() rather than assuming.
 */
#include "evo_font_charset.h"

#define EVO_TEXT_CHARSET \
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 /._:-+" \
    EVO_FONT_PUNCT_CHARS

/* First unsupported character in `s`, or 0 when the whole string renders. */
char evo_text_unsupported(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* EVO_DRAW_H */
