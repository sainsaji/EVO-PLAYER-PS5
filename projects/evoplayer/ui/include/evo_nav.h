/*
 * evo_nav — where you are, and how you get back.
 *
 * Navigation used to be an integer `screen` plus two long if/else chains: one
 * to decide what CROSS did on each screen, another to decide what CIRCLE did.
 * Back was therefore hardcoded per screen ("from FAVORITES, go to 0"), which
 * means a screen reachable from two places can only ever return to one of
 * them. Media Info always returned to the player even when it was opened
 * from the browser.
 *
 * A stack fixes that: Back pops to wherever you actually came from.
 *
 * Screens also declare which navigation *section* owns them, so the rail can
 * highlight the right entry without every draw function being told.
 */
#ifndef EVO_NAV_H
#define EVO_NAV_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Screen identities. The numeric values are pinned to the legacy SCREEN_*
 * defines in main.c because playback code compares `screen` against bare
 * integers (`screen == 2`) in a dozen places, and those comparisons are load
 * bearing in the video path. Renumbering is a separate, riskier change.
 */
typedef enum {
    EVO_SCREEN_LAUNCH            = 0,
    EVO_SCREEN_BROWSER           = 1,
    EVO_SCREEN_PLAYER            = 2,
    EVO_SCREEN_IMAGE             = 3,
    EVO_SCREEN_RESUME_PROMPT     = 4,
    EVO_SCREEN_SETTINGS          = 10,
    EVO_SCREEN_PROFILE           = 11,
    EVO_SCREEN_RECENT            = 12,
    EVO_SCREEN_FAVORITES         = 13,
    EVO_SCREEN_ABOUT             = 14,
    EVO_SCREEN_TOOLS             = 15,
    EVO_SCREEN_MEDIA_INFO        = 16,
    EVO_SCREEN_PLAYBACK_FINISHED = 17,
    EVO_SCREEN_SUBTITLE_PICKER   = 18,
    EVO_SCREEN_CHANGELOG         = 19
} evo_screen_id;

/* Rail sections. Order is the order they appear in the rail. */
typedef enum {
    EVO_SECTION_HOME = 0,
    EVO_SECTION_BROWSER,
    EVO_SECTION_RECENT,
    EVO_SECTION_FAVORITES,
    EVO_SECTION_SETTINGS,
    EVO_SECTION_TOOLS,
    EVO_SECTION_ABOUT,
    EVO_SECTION_COUNT,
    EVO_SECTION_NONE = -1
} evo_section;

/* ---- section metadata (drives the rail, and the launch tiles) ---------- */

typedef struct evo_section_info {
    const char   *label;      /* rail label, expanded state */
    const char   *blurb;      /* one line, used by the launch tiles */
    int           icon;       /* EVO_IC_* */
    evo_screen_id screen;     /* where selecting it goes */
} evo_section_info;

const evo_section_info *evo_section_get(evo_section s);

/* Which section owns a screen. EVO_SECTION_NONE for the player and prompts,
 * which are modal and show no rail. */
evo_section evo_screen_section(evo_screen_id id);

/* Screens that are modal overlays: no rail, no standard footer. */
int evo_screen_is_modal(evo_screen_id id);

/* ---- the stack --------------------------------------------------------- */

#define EVO_NAV_DEPTH 8

/* Clears the stack and makes `root` the only entry. */
void evo_nav_reset(evo_screen_id root);

/* Pushes a screen. Silently replaces the top if the stack is full, which is
 * the right failure for a depth-8 stack in a media player: losing one step of
 * history beats refusing to navigate. */
void evo_nav_push(evo_screen_id id);

/* Replaces the top without growing the stack - for lateral moves, e.g.
 * jumping between rail sections. */
void evo_nav_replace(evo_screen_id id);

/* Pops one level. Returns 0 and does nothing when already at the root, which
 * is how a screen knows to show "press PS to exit" instead. */
int evo_nav_pop(void);

/* Pops back to the root in one step. */
void evo_nav_home(void);

evo_screen_id evo_nav_top(void);
int           evo_nav_depth(void);
int           evo_nav_can_pop(void);

/* Convenience: the section that should be lit in the rail right now. */
evo_section evo_nav_section(void);

#ifdef __cplusplus
}
#endif

#endif /* EVO_NAV_H */
