/*
 * evo_theme — built-in themes and the .theme file loader.
 *
 * File format, deliberately boring so it can be written by hand:
 *
 *     # /mnt/usb0/evo_themes/mytheme.theme
 *     name        = Solar
 *     bg_top      = #0A0E14
 *     accent      = #FFB000
 *     radius      = 14
 *
 * Colours are #RRGGBB or #RRGGBBAA (alpha defaults to FF). Metrics are plain
 * integers. Unknown keys are ignored, and any key left out inherits from the
 * default theme - so a two-line file that only changes the accent is valid.
 */
#include "evo_theme.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Overridable so the parser can be exercised on a host with a local
 * directory of .theme files instead of a console. */
#ifndef EVO_THEME_DIR
#define EVO_THEME_DIR "/mnt/usb0/evo_themes"
#endif

static evo_theme g_themes[EVO_THEME_MAX];
static int       g_count;
static int       g_active;
static int       g_inited;

/* ------------------------------------------------------------------------ */
/* Built-ins.
 *
 * Shared design rules, so themes stay coherent:
 *   - the page background is a near-black vertical gradient, never flat
 *   - cards sit one step above it and carry their own subtle gradient
 *   - resting borders are low-contrast hairlines; the accent is reserved for
 *     the selected row, icons and rails, so the eye has one thing to follow
 *   - text is three weights: primary, secondary, muted
 * ------------------------------------------------------------------------ */

static const evo_theme k_builtin[] = {
    {
        .name = "MIDNIGHT",
        .bg_top          = EVO_RGBA(0x06, 0x0B, 0x16, 255),
        .bg_bottom       = EVO_RGBA(0x02, 0x04, 0x09, 255),
        .scrim           = EVO_RGBA(0x00, 0x00, 0x00, 150),
        .surface         = EVO_RGBA(0x12, 0x1B, 0x2E, 235),
        .surface_alt     = EVO_RGBA(0x0C, 0x13, 0x22, 235),
        .surface_sel     = EVO_RGBA(0x1B, 0x2E, 0x4C, 245),
        .surface_sel_alt = EVO_RGBA(0x11, 0x1E, 0x36, 245),
        .border          = EVO_RGBA(0x2A, 0x3B, 0x55, 170),
        .border_sel      = EVO_RGBA(0x00, 0xCD, 0xFF, 220),
        .shadow          = EVO_RGBA(0x00, 0x00, 0x00, 130),
        .accent          = EVO_RGBA(0x00, 0xCD, 0xFF, 255),
        .accent_soft     = EVO_RGBA(0x00, 0xA8, 0xFF, 60),
        .accent_alt      = EVO_RGBA(0x7A, 0x5C, 0xFF, 255),
        .danger          = EVO_RGBA(0xFF, 0x5C, 0x5C, 255),
        .text_primary    = EVO_RGBA(0xEC, 0xF3, 0xFF, 255),
        .text_secondary  = EVO_RGBA(0x9F, 0xB2, 0xCC, 255),
        .text_muted      = EVO_RGBA(0x5E, 0x71, 0x8C, 255),
        .radius = 14, .border_px = 1, .shadow_px = 12, .rail_px = 4,
        .row_h = 96, .row_gap = 14, .pad_x = 28,
    },
    {
        .name = "CARBON",
        .bg_top          = EVO_RGBA(0x0F, 0x0F, 0x11, 255),
        .bg_bottom       = EVO_RGBA(0x05, 0x05, 0x06, 255),
        .scrim           = EVO_RGBA(0x00, 0x00, 0x00, 150),
        .surface         = EVO_RGBA(0x1C, 0x1C, 0x20, 235),
        .surface_alt     = EVO_RGBA(0x14, 0x14, 0x18, 235),
        .surface_sel     = EVO_RGBA(0x2A, 0x2A, 0x30, 245),
        .surface_sel_alt = EVO_RGBA(0x1F, 0x1F, 0x24, 245),
        .border          = EVO_RGBA(0x33, 0x33, 0x3A, 170),
        .border_sel      = EVO_RGBA(0xE8, 0xE8, 0xEE, 200),
        .shadow          = EVO_RGBA(0x00, 0x00, 0x00, 140),
        .accent          = EVO_RGBA(0xF2, 0xF2, 0xF5, 255),
        .accent_soft     = EVO_RGBA(0xFF, 0xFF, 0xFF, 40),
        .accent_alt      = EVO_RGBA(0xFF, 0x6B, 0x35, 255),
        .danger          = EVO_RGBA(0xFF, 0x4F, 0x4F, 255),
        .text_primary    = EVO_RGBA(0xF5, 0xF5, 0xF7, 255),
        .text_secondary  = EVO_RGBA(0xA0, 0xA0, 0xA8, 255),
        .text_muted      = EVO_RGBA(0x66, 0x66, 0x6E, 255),
        .radius = 12, .border_px = 1, .shadow_px = 14, .rail_px = 4,
        .row_h = 96, .row_gap = 14, .pad_x = 28,
    },
    {
        .name = "EMBER",
        .bg_top          = EVO_RGBA(0x14, 0x0C, 0x08, 255),
        .bg_bottom       = EVO_RGBA(0x07, 0x04, 0x02, 255),
        .scrim           = EVO_RGBA(0x00, 0x00, 0x00, 150),
        .surface         = EVO_RGBA(0x24, 0x16, 0x0E, 235),
        .surface_alt     = EVO_RGBA(0x18, 0x0E, 0x08, 235),
        .surface_sel     = EVO_RGBA(0x3A, 0x22, 0x11, 245),
        .surface_sel_alt = EVO_RGBA(0x28, 0x17, 0x0C, 245),
        .border          = EVO_RGBA(0x4A, 0x30, 0x1C, 170),
        .border_sel      = EVO_RGBA(0xFF, 0xA5, 0x28, 220),
        .shadow          = EVO_RGBA(0x00, 0x00, 0x00, 140),
        .accent          = EVO_RGBA(0xFF, 0xA5, 0x28, 255),
        .accent_soft     = EVO_RGBA(0xFF, 0x7A, 0x18, 60),
        .accent_alt      = EVO_RGBA(0xFF, 0x4D, 0x4D, 255),
        .danger          = EVO_RGBA(0xFF, 0x6E, 0x4A, 255),
        .text_primary    = EVO_RGBA(0xFF, 0xF3, 0xE6, 255),
        .text_secondary  = EVO_RGBA(0xC7, 0xA9, 0x8E, 255),
        .text_muted      = EVO_RGBA(0x8A, 0x6E, 0x56, 255),
        .radius = 14, .border_px = 1, .shadow_px = 12, .rail_px = 4,
        .row_h = 96, .row_gap = 14, .pad_x = 28,
    },
    {
        .name = "AURORA",
        .bg_top          = EVO_RGBA(0x05, 0x12, 0x12, 255),
        .bg_bottom       = EVO_RGBA(0x01, 0x06, 0x08, 255),
        .scrim           = EVO_RGBA(0x00, 0x00, 0x00, 150),
        .surface         = EVO_RGBA(0x0D, 0x24, 0x24, 235),
        .surface_alt     = EVO_RGBA(0x08, 0x18, 0x1A, 235),
        .surface_sel     = EVO_RGBA(0x11, 0x3A, 0x35, 245),
        .surface_sel_alt = EVO_RGBA(0x0B, 0x26, 0x25, 245),
        .border          = EVO_RGBA(0x1E, 0x4A, 0x46, 170),
        .border_sel      = EVO_RGBA(0x3D, 0xF5, 0xC0, 220),
        .shadow          = EVO_RGBA(0x00, 0x00, 0x00, 130),
        .accent          = EVO_RGBA(0x3D, 0xF5, 0xC0, 255),
        .accent_soft     = EVO_RGBA(0x24, 0xC9, 0xA0, 60),
        .accent_alt      = EVO_RGBA(0x9B, 0x8C, 0xFF, 255),
        .danger          = EVO_RGBA(0xFF, 0x6B, 0x7A, 255),
        .text_primary    = EVO_RGBA(0xE8, 0xFF, 0xF8, 255),
        .text_secondary  = EVO_RGBA(0x93, 0xC3, 0xB8, 255),
        .text_muted      = EVO_RGBA(0x56, 0x82, 0x7A, 255),
        .radius = 16, .border_px = 1, .shadow_px = 12, .rail_px = 4,
        .row_h = 96, .row_gap = 14, .pad_x = 28,
    },
};

#define BUILTIN_COUNT ((int)(sizeof k_builtin / sizeof k_builtin[0]))

/* ------------------------------------------------------------------------ */
/* .theme parsing                                                            */
/* ------------------------------------------------------------------------ */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "#RRGGBB" or "#RRGGBBAA" -> packed. Returns 0 if unparseable. */
static int parse_colour(const char *s, uint32_t *out)
{
    int v[8], n = 0;

    while (*s == ' ' || *s == '\t') s++;
    if (*s == '#') s++;

    while (n < 8 && hexval(*s) >= 0)
        v[n++] = hexval(*s++);

    if (n != 6 && n != 8)
        return 0;

    int r = v[0] * 16 + v[1];
    int g = v[2] * 16 + v[3];
    int b = v[4] * 16 + v[5];
    int a = (n == 8) ? v[6] * 16 + v[7] : 255;

    *out = EVO_RGBA(r, g, b, a);
    return 1;
}

static void trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') memmove(s, s + 1, strlen(s));
    e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' ||
                     e[-1] == ' '  || e[-1] == '\t'))
        *--e = 0;
}

static int load_theme_file(const char *path, evo_theme *t)
{
    FILE *f = fopen(path, "r");
    char line[256];

    if (!f)
        return 0;

    /* Anything not specified inherits from the default theme, so a file that
     * only overrides the accent is perfectly valid. */
    *t = k_builtin[0];
    snprintf(t->name, sizeof t->name, "CUSTOM");

    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        char *eq;
        char key[64], val[128];

        /* '#' starts a comment only when it is not a colour literal, i.e. not
         * immediately after '='. */
        if (hash == line) {
            *hash = 0;                      /* whole-line comment */
        } else if (hash && (hash[-1] == ' ' || hash[-1] == '\t')) {
            /* Walk back over the run of spaces. Indices, not pointers: the
             * pointer form formed `line - 1` when the run reached the start
             * of the buffer. */
            size_t i = (size_t)(hash - line);
            while (i > 0 && (line[i - 1] == ' ' || line[i - 1] == '\t')) i--;
            if (i == 0 || line[i - 1] != '=')
                *hash = 0;
        }

        eq = strchr(line, '=');
        if (!eq) continue;

        *eq = 0;
        snprintf(key, sizeof key, "%s", line);
        snprintf(val, sizeof val, "%s", eq + 1);
        trim(key);
        trim(val);
        if (!key[0] || !val[0]) continue;

        #define COL(k, field)  \
            if (!strcmp(key, k)) { parse_colour(val, &t->field); continue; }
        #define NUM(k, field)  \
            if (!strcmp(key, k)) { t->field = (int16_t)atoi(val); continue; }

        if (!strcmp(key, "name")) {
            snprintf(t->name, sizeof t->name, "%s", val);
            continue;
        }
        COL("bg_top",          bg_top)
        COL("bg_bottom",       bg_bottom)
        COL("scrim",           scrim)
        COL("surface",         surface)
        COL("surface_alt",     surface_alt)
        COL("surface_sel",     surface_sel)
        COL("surface_sel_alt", surface_sel_alt)
        COL("border",          border)
        COL("border_sel",      border_sel)
        COL("shadow",          shadow)
        COL("accent",          accent)
        COL("accent_soft",     accent_soft)
        COL("accent_alt",      accent_alt)
        COL("danger",          danger)
        COL("text_primary",    text_primary)
        COL("text_secondary",  text_secondary)
        COL("text_muted",      text_muted)
        NUM("radius",    radius)
        NUM("border_px", border_px)
        NUM("shadow_px", shadow_px)
        NUM("rail_px",   rail_px)
        NUM("row_h",     row_h)
        NUM("row_gap",   row_gap)
        NUM("pad_x",     pad_x)

        #undef COL
        #undef NUM
    }

    fclose(f);
    return 1;
}

/* ------------------------------------------------------------------------ */

void evo_theme_init(void)
{
    DIR *d;
    struct dirent *e;

    if (g_inited)
        return;
    g_inited = 1;

    for (int i = 0; i < BUILTIN_COUNT && g_count < EVO_THEME_MAX; i++)
        g_themes[g_count++] = k_builtin[i];

    /* Plug and play: whatever is on the stick, in directory order. */
    d = opendir(EVO_THEME_DIR);
    if (!d)
        return;

    while ((e = readdir(d)) != NULL && g_count < EVO_THEME_MAX) {
        size_t n = strlen(e->d_name);
        char path[512];

        if (n < 7 || strcmp(e->d_name + n - 6, ".theme") != 0)
            continue;
        if (e->d_name[0] == '.')
            continue;

        snprintf(path, sizeof path, "%s/%s", EVO_THEME_DIR, e->d_name);
        if (load_theme_file(path, &g_themes[g_count]))
            g_count++;
    }
    closedir(d);
}

const evo_theme *evo_theme_current(void)
{
    if (!g_inited)
        evo_theme_init();
    if (g_active < 0 || g_active >= g_count)
        g_active = 0;
    return &g_themes[g_active];
}

int evo_theme_count(void)
{
    if (!g_inited) evo_theme_init();
    return g_count;
}

const char *evo_theme_name(int index)
{
    if (!g_inited) evo_theme_init();
    if (index < 0 || index >= g_count)
        return NULL;
    return g_themes[index].name;
}

int evo_theme_index(void)
{
    return g_active;
}

int evo_theme_set(int index)
{
    if (!g_inited) evo_theme_init();
    if (g_count <= 0) return 0;
    if (index < 0) index = g_count - 1;
    if (index >= g_count) index = 0;
    g_active = index;
    return g_active;
}

/*
 * Settings persist the theme by name, not index: a USB theme that is present
 * on one boot and absent on the next would otherwise shift every index after
 * it and silently select a different theme. An unknown name falls back to the
 * default rather than failing.
 */
int evo_theme_set_by_name(const char *name)
{
    if (!g_inited) evo_theme_init();
    if (!name || !*name) return g_active;

    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_themes[i].name, name) == 0)
            return evo_theme_set(i);
    }
    return g_active;
}
