/*
 * evo_changelog — what changed in each release, for the About section.
 *
 * WHY THIS IS A TABLE AND NOT A TEXT FILE
 *   Reading CHANGELOG.md at runtime would mean the console needing a copy of
 *   the repository on USB, and the markdown is written for a monospace
 *   terminal - prose sentences, backticks, parentheses. The font atlas is
 *   PP_CHARS:
 *
 *       ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 /._:-+
 *
 *   43 glyphs. No lower case, no comma, no parenthesis, no apostrophe - and
 *   an unknown glyph draws as a hole rather than being skipped, which is what
 *   once turned real filenames into rows of gaps. So the entries here are
 *   written to that alphabet deliberately. Keep new ones inside it.
 *
 * KEEPING IT HONEST
 *   CHANGELOG.md at the repository root stays the source of truth. This is a
 *   condensed reading of it - one line per change, the ones a person holding
 *   a controller would care about. When you add a release section there, add
 *   its rows here in the same commit.
 */
#ifndef EVO_CHANGELOG_H
#define EVO_CHANGELOG_H

typedef enum {
    EVO_CL_VERSION = 0,   /* a release heading */
    EVO_CL_NEW,
    EVO_CL_FIXED,
    EVO_CL_REMOVED
} evo_changelog_kind;

typedef struct evo_changelog_row {
    evo_changelog_kind kind;
    const char        *text;
} evo_changelog_row;

/* Newest first - the last thing that changed is what people look for. */
static const evo_changelog_row EVO_CHANGELOG[] = {
    { EVO_CL_VERSION, "0.3.0 - LAUNCH FROM THE CONSOLE" },
    { EVO_CL_NEW,     "HOME SCREEN TILE - OPEN FROM MEDIA - NO BROWSER" },
    { EVO_CL_NEW,     "THIS CHANGELOG - UNDER ABOUT" },
    { EVO_CL_NEW,     "A REAL APPLICATION ICON - DRAWN FROM VECTORS" },
    { EVO_CL_FIXED,   "THE TILE KEEPS ITS OWN COPY OF THE PLAYER" },

    { EVO_CL_VERSION, "0.2.0 - SUBTITLES AND HOW YOU CHOOSE THEM" },
    { EVO_CL_NEW,     "SUBTITLE TRACK PICKER - PRESS DOWN WHILE PLAYING" },
    { EVO_CL_NEW,     "TRACK NAMES FROM LANGUAGE CODES - 50 MAPPED" },
    { EVO_CL_FIXED,   "TRACKS RANKED BY CUE COUNT - NOT BY METADATA" },
    { EVO_CL_FIXED,   "NEAR EMPTY TRACKS MARKED SIGNS ONLY - STILL OFFERED" },
    { EVO_CL_FIXED,   "MARQUEE SCROLLS AT ONE SPEED AT ANY FRAME RATE" },

    { EVO_CL_VERSION, "0.1.0 - A REBUILD OF THE INTERFACE" },
    { EVO_CL_NEW,     "LAUNCH SCREEN - RESUME HERO AND TWO SHELVES" },
    { EVO_CL_NEW,     "BROWSER INSPECTOR - PREVIEW FRAME AND CODEC DETAIL" },
    { EVO_CL_NEW,     "SIDE NAVIGATION RAIL - BACK IS A STACK NOW" },
    { EVO_CL_NEW,     "HOLD TO SCROLL - SHOULDERS PAGE - L2/R2 JUMP A-Z" },
    { EVO_CL_NEW,     "L3 CAPTURES A SCREENSHOT DURING PLAYBACK" },
    { EVO_CL_FIXED,   "THE EIGHTH SETTINGS ROW WAS UNREACHABLE" },
    { EVO_CL_FIXED,   "AUDIO FAILURES NO LONGER ALL BLAMED ON E-AC3" },
    { EVO_CL_FIXED,   "PREVIEWS NO LONGER PIXELATED BY DOUBLE RESAMPLING" },
    { EVO_CL_FIXED,   "4K CONVERTER KEEPS ITS WORKERS - 11.6 TO 9.2 MS" },
    { EVO_CL_REMOVED, "HAPTICS - EVERY VIBRATION PATH IS DEAD IN THIS SLOT" },

    { EVO_CL_VERSION, "0.0.2 - THEMING" },
    { EVO_CL_NEW,     "THEME FILES FROM USB - FOUR THEMES BUILT IN" },
    { EVO_CL_NEW,     "CARDS DRAWN FROM SDF - GENERATED VECTOR ICONS" },
    { EVO_CL_NEW,     "NAVIGATION SOUNDS AND A THEMED LIGHTBAR" },

    { EVO_CL_VERSION, "0.0.1 - THE FORK" },
    { EVO_CL_NEW,     "7.1 SURROUND OUTPUT WITH STEREO FALLBACK" },
    { EVO_CL_NEW,     "FLIP SYNCHRONISED PRESENTATION - NO TEARING" },
    { EVO_CL_NEW,     "FASTER TILE SWIZZLE AND FOLDERS FIRST BROWSING" }
};

#define EVO_CHANGELOG_COUNT \
    ((int)(sizeof(EVO_CHANGELOG) / sizeof(EVO_CHANGELOG[0])))

#endif /* EVO_CHANGELOG_H */
