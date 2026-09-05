/*
 * evo_metrics — the layout grid, in one place.
 *
 * Every screen positions against these constants rather than inventing its
 * own margins. That is the whole point: before this file, the main menu used
 * x=180, the browser header used x=82 and its rows used x=180, and the
 * settings footer sat on a different rule than the browser footer. Nothing
 * lined up between pages because nothing shared a grid.
 *
 * Authored at 1080p, which is the only resolution the UI is ever drawn at
 * (4K playback presents through a separate path and never composites UI).
 */
#ifndef EVO_METRICS_H
#define EVO_METRICS_H

/* ---- surface ----------------------------------------------------------- */

#define EVO_SCREEN_W        1920
#define EVO_SCREEN_H        1080

/*
 * Title-safe inset. TV overscan can eat ~3% per edge; nothing that must be
 * readable is placed outside this.
 */
#define EVO_SAFE_X          64
#define EVO_SAFE_Y          40

/* ---- side navigation rail ---------------------------------------------- */

/*
 * The rail is always drawn at EVO_RAIL_W and grows to EVO_RAIL_W_OPEN as an
 * overlay when focus moves into it, so content never reflows underneath.
 * Reflowing on focus makes the whole page twitch every time you brush the
 * left edge of a list.
 */
#define EVO_RAIL_W          108
#define EVO_RAIL_W_OPEN     344
#define EVO_RAIL_ITEM_H     76
#define EVO_RAIL_ITEM_PITCH 88
#define EVO_RAIL_TOP        232
#define EVO_RAIL_ICON       72   /* native icon size from gen_icons.py */
#define EVO_RAIL_ICON_X     18   /* centres a 72px icon in a 108px rail */
/* Centre line of the application mark above the section icons. It clears the
 * first rail item (EVO_RAIL_TOP) by 78px with a 72px glyph. */
#define EVO_RAIL_MARK_CY    118

/* ---- header ------------------------------------------------------------ */

#define EVO_HEADER_MARK_X   152  /* accent tick left of the title */
#define EVO_HEADER_MARK_Y   84
#define EVO_HEADER_MARK_H   64
#define EVO_HEADER_MARK_W   4

#define EVO_HEADER_TEXT_X   192
#define EVO_HEADER_TITLE_Y  78
#define EVO_HEADER_SUB_Y    140
#define EVO_HEADER_RULE_Y   190

/* ---- content ----------------------------------------------------------- */

/* Left edge of content on a page that shows the rail. */
#define EVO_CONTENT_X       152
/* Right edge (exclusive). Symmetric with the rail-side margin. */
#define EVO_CONTENT_R       (EVO_SCREEN_W - EVO_SAFE_X)
#define EVO_CONTENT_W       (EVO_CONTENT_R - EVO_CONTENT_X)

#define EVO_CONTENT_Y       222
#define EVO_CONTENT_B       930   /* clears the footer rule */
#define EVO_CONTENT_H       (EVO_CONTENT_B - EVO_CONTENT_Y)

/* Full-bleed pages (the launch screen) ignore the rail. */
#define EVO_BLEED_X         88
#define EVO_BLEED_R         (EVO_SCREEN_W - EVO_BLEED_X)
#define EVO_BLEED_W         (EVO_BLEED_R - EVO_BLEED_X)

/* ---- list rows --------------------------------------------------------- */

#define EVO_ROW_H           92
#define EVO_ROW_PITCH       104
#define EVO_ROW_PAD_X       28
#define EVO_ROW_ICON        56
#define EVO_ROW_TEXT_X      108   /* past a 56px icon at pad 28 */

/* Colour chips on a row - the settings THEME preview. */
#define EVO_SWATCH_W        34
#define EVO_SWATCH_H        24
#define EVO_SWATCH_GAP      8

/* ---- inspector (file browser right-hand column) ------------------------ */

#define EVO_INSPECT_W       560
#define EVO_INSPECT_X       (EVO_CONTENT_R - EVO_INSPECT_W)
#define EVO_INSPECT_GAP     32
/* The list stops here so it never runs under the inspector. */
#define EVO_LIST_W          (EVO_INSPECT_X - EVO_INSPECT_GAP - EVO_CONTENT_X)

#define EVO_PREVIEW_W       EVO_INSPECT_W
#define EVO_PREVIEW_H       315   /* 16:9 */

/* ---- footer ------------------------------------------------------------ */

#define EVO_FOOTER_RULE_Y   948
#define EVO_FOOTER_Y        949
#define EVO_FOOTER_H        (EVO_SCREEN_H - EVO_FOOTER_Y)
#define EVO_FOOTER_X        EVO_CONTENT_X
#define EVO_FOOTER_HINT_GAP 44    /* space between one hint and the next */
#define EVO_FOOTER_GLYPH    48    /* controller glyphs are 48x48 */
#define EVO_FOOTER_GLYPH_GAP 12   /* glyph to its own label */

/* ---- launch screen ----------------------------------------------------- */

/*
 * Full-bleed, no rail: this is the one page that should feel like a home
 * screen rather than a section of one.
 *
 * The vertical budget is tight and entirely determined by the footer rule at
 * 948, so it is worked out here once rather than guessed at per element:
 *
 *   header   40 .. 140
 *   hero    146 .. 464   (318)
 *   shelf 1 492 .. 696   (label 36 + tiles 166)
 *   shelf 2 724 .. 928   (label 36 + tiles 166)
 *   footer  948 .. 1080
 *
 * Six tiles across the bleed width: 6*274 + 5*20 = 1744 = EVO_BLEED_W exactly,
 * so both shelves and the hero share one left and right edge.
 */
#define EVO_HERO_Y          146
#define EVO_HERO_H          318

#define EVO_SHELF1_LABEL_Y  492
#define EVO_SHELF1_Y        530
#define EVO_SHELF2_LABEL_Y  724
#define EVO_SHELF2_Y        762

#define EVO_SHELF_LABEL_H   36
#define EVO_TILE_W          274
#define EVO_TILE_H          166
#define EVO_TILE_GAP        20
#define EVO_TILE_PITCH      (EVO_TILE_W + EVO_TILE_GAP)
#define EVO_TILES_VISIBLE   6

/*
 * Height reserved at the bottom of a tile for its caption. Tiles use a
 * smaller type ramp than rows (SUB over SMALL), so the band is shorter than
 * it was and the artwork or icon above it gets the difference back.
 */
#define EVO_TILE_CAPTION_H  62

/* ---- motion ------------------------------------------------------------ */

/*
 * Focus glide is a fixed-point exponential approach. The denominator is the
 * fraction of remaining distance closed per frame, so smaller is snappier.
 * At 60fps /2 settles in about 4 frames, which reads as instant but smooth.
 */
#define EVO_GLIDE_DIV       2
#define EVO_GLIDE_SNAP_FP   160   /* below this, jump - avoids 1px crawling */

#endif /* EVO_METRICS_H */
