/*
 * evo_draw — GL-5 (#81): the bitmap font, the icon/glyph atlases, the vtable
 * binding and the immediate-mode text/layout helpers are all gone. Every screen
 * and overlay renders text through RmlUi (FreeType) now.
 *
 * What survives is the two small enums other code still keys off:
 *   - evo_face   — the size buckets (subtitle size cycle, text-reader size)
 *   - EVO_IC_*   — icon indices, mapped to PNG paths for the RmlUi screens
 */
#ifndef EVO_DRAW_H
#define EVO_DRAW_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- type faces (size buckets) --------------------------------------- */

typedef enum {
    EVO_FACE_SMALL = 0,   /* footers, badges, hints */
    EVO_FACE_SUB   = 1,   /* row descriptions, breadcrumbs; subtitle SMALL */
    EVO_FACE_MENU  = 2,   /* row titles; subtitle MEDIUM */
    EVO_FACE_TITLE = 3    /* page titles; subtitle LARGE */
} evo_face;

/* ---- icon indices (mapped to PNG icon paths by the RmlUi screens) ---- */

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
    EVO_IC_LOGO      = 14,
    EVO_IC_EMBY      = 15
};

/* Controller glyph indices (mapped to assets/icons/btn_*.png). */
enum {
    EVO_GLYPH_CROSS    = 0,
    EVO_GLYPH_DPAD     = 1,
    EVO_GLYPH_LSTICK   = 2,
    EVO_GLYPH_RSTICK   = 3,
    EVO_GLYPH_CIRCLE   = 4,
    EVO_GLYPH_TRIANGLE = 5,
    EVO_GLYPH_SQUARE   = 6
};

#ifdef __cplusplus
}
#endif

#endif /* EVO_DRAW_H */
