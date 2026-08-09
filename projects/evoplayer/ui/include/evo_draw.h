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
 * These match rr_icon()'s switch in main.c. Named here so screens stop
 * passing bare integers around; `rr_icon(fb, x, y, 6)` told nobody it was
 * drawing a chevron.
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
    EVO_IC_TRASH     = 12
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
 * a frame counter owned by the caller (usually reset when the selection
 * changes). Falls back to a plain draw when the text fits.
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

#ifdef __cplusplus
}
#endif

#endif /* EVO_DRAW_H */
