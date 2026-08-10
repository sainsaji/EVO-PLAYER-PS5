#include "evo_nav.h"
#include "evo_draw.h"

static const evo_section_info SECTIONS[EVO_SECTION_COUNT] = {
    { "HOME",      "Back to the launch screen",          EVO_IC_HOME,     EVO_SCREEN_LAUNCH    },
    { "BROWSE",    "Videos and folders on USB storage",  EVO_IC_USB,      EVO_SCREEN_BROWSER   },
    { "RECENT",    "Pick up where you left off",         EVO_IC_RECENT,   EVO_SCREEN_RECENT    },
    { "FAVORITES", "Media you saved for later",          EVO_IC_FAVORITE, EVO_SCREEN_FAVORITES },
    { "SETTINGS",  "Playback profiles and preferences",  EVO_IC_SETTINGS, EVO_SCREEN_SETTINGS  },
    { "TOOLS",     "Reports and diagnostics",      EVO_IC_TOOLS,    EVO_SCREEN_TOOLS     },
    { "ABOUT",     "Credits and project info",  EVO_IC_ABOUT,    EVO_SCREEN_ABOUT     }
};

const evo_section_info *evo_section_get(evo_section s)
{
    if (s < 0 || s >= EVO_SECTION_COUNT) return &SECTIONS[EVO_SECTION_HOME];
    return &SECTIONS[s];
}

evo_section evo_screen_section(evo_screen_id id)
{
    switch (id) {
        case EVO_SCREEN_LAUNCH:    return EVO_SECTION_HOME;
        case EVO_SCREEN_BROWSER:   return EVO_SECTION_BROWSER;
        case EVO_SCREEN_RECENT:    return EVO_SECTION_RECENT;
        case EVO_SCREEN_FAVORITES: return EVO_SECTION_FAVORITES;

        /* The profile picker is a child of settings and keeps SETTINGS lit,
         * so descending into it does not make the rail jump. */
        case EVO_SCREEN_SETTINGS:
        case EVO_SCREEN_PROFILE:   return EVO_SECTION_SETTINGS;

        case EVO_SCREEN_TOOLS:     return EVO_SECTION_TOOLS;

        /* The changelog is a child of About, and keeps ABOUT lit for the same
         * reason the profile picker keeps SETTINGS lit. */
        case EVO_SCREEN_ABOUT:
        case EVO_SCREEN_CHANGELOG: return EVO_SECTION_ABOUT;

        default:                   return EVO_SECTION_NONE;
    }
}

int evo_screen_is_modal(evo_screen_id id)
{
    switch (id) {
        case EVO_SCREEN_PLAYER:
        case EVO_SCREEN_IMAGE:
        case EVO_SCREEN_RESUME_PROMPT:
        case EVO_SCREEN_MEDIA_INFO:
        case EVO_SCREEN_PLAYBACK_FINISHED:
            return 1;
        default:
            return 0;
    }
}

/* ---- stack ------------------------------------------------------------- */

static evo_screen_id g_stack[EVO_NAV_DEPTH] = { EVO_SCREEN_LAUNCH };
static int           g_top;   /* index of the current screen */

void evo_nav_reset(evo_screen_id root)
{
    g_stack[0] = root;
    g_top      = 0;
}

void evo_nav_push(evo_screen_id id)
{
    if (g_top + 1 < EVO_NAV_DEPTH)
        g_top++;
    g_stack[g_top] = id;
}

void evo_nav_replace(evo_screen_id id)
{
    g_stack[g_top] = id;
}

int evo_nav_pop(void)
{
    if (g_top == 0) return 0;
    g_top--;
    return 1;
}

void evo_nav_home(void)
{
    g_top = 0;
}

evo_screen_id evo_nav_top(void)  { return g_stack[g_top]; }
int           evo_nav_depth(void) { return g_top + 1; }
int           evo_nav_can_pop(void) { return g_top > 0; }

evo_section evo_nav_section(void)
{
    return evo_screen_section(g_stack[g_top]);
}
