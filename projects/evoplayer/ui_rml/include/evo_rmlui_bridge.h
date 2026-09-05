#ifndef EVO_RMLUI_BRIDGE_H
#define EVO_RMLUI_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* title;
    const char* metadata;
    const char* res_badge;
    const char* hdr_badge;
    const char* codec_badge;
    const char* fps_badge;
    const char* audio_badge;
    double position_sec;
    double duration_sec;
    double percentage;
    int paused;
    int scrub_active;
    double scrub_target;
    const char* audio_track;
    const char* sub_track;
    int view_mode;
    int show_stats;
    int alpha;
} evo_playback_osd_params_t;

typedef struct {
    const char* eyebrow;
    const char* title;
    const char* detail;
    double progress_pct; // 0.0 to 1.0, or -1.0
    int action_count;
    struct {
        const char* icon_path;
        const char* label;
        int is_primary;
    } actions[3];
} evo_rmlui_dialog_params_t;

/* #75: toast notifications. kind: 0=info 1=tech 2=error 3=ok. alpha/slide
 * are the caller-owned fade/slide-in values evo_toast.c already computes
 * (0..255 and px-still-to-travel, matching the old evo_toast/evo_widget_toast
 * struct exactly) - the bridge just forwards them to CSS opacity/transform. */
typedef struct {
    const char* title;
    const char* message;
    int kind;
    int visible;
    int alpha;
    int slide;
} evo_rmlui_toast_params_t;

typedef struct {
    const char* title;
    const char* detail;
    const char* icon_path;
    const char* badge;
    int has_chevron;
    int is_focused;
} evo_rmlui_settings_row_t;

typedef struct {
    const char* title;
    const char* subtitle;
    const char* counter;
    int rail_active_idx;
    int rail_focused;
    int row_count;
    evo_rmlui_settings_row_t rows[8];
} evo_rmlui_settings_params_t;

typedef struct {
    const char* label;
    const char* detail;
    int is_current;
    int is_focused;
} evo_rmlui_subtitles_track_t;

typedef struct {
    const char* eyebrow;
    const char* title;
    const char* size_str;
    const char* preview_text;
    int preview_face; // 0=small, 1=medium, 2=large
    int track_count;
    evo_rmlui_subtitles_track_t tracks[8];
} evo_rmlui_subtitles_params_t;

typedef struct {
    const char* title;
    const char* path;
    const char* res_badge;
    const char* hdr_badge;
    const char* codec_badge;
    const char* fps_badge;
    const char* container;
    const char* file_size;
    const char* duration;
    const char* video_codec;
    const char* resolution;
    const char* color_hdr;
    const char* audio_codec;
    const char* channels;
    const char* sample_rate;
    const char* subtitles;
    const char* output;
    const char* renderer;
} evo_rmlui_mediainfo_params_t;

/*
 * Launch (home) screen.
 *
 * Tiles carry raw BGRA artwork straight from the decoder's cover cache
 * rather than a path, because those frames never exist as files. A NULL
 * `art` falls back to the tile icon, which is what the library shelf uses.
 */
#define EVO_RMLUI_TILES 6

typedef struct {
    const char* title;
    const char* detail;
    const char* icon_path;   /* used when art is NULL */
    int         progress;    /* 0..1000, or -1 for no bar */
    const uint32_t* art;     /* BGRA 0xAABBGGRR, or NULL */
    int         art_w;
    int         art_h;
    int         is_focused;
} evo_rmlui_launch_tile_t;

typedef struct {
    const char* app_name;      /* "EVO PLAYER" */
    const char* version;       /* "VERSION 0.7.0", or NULL */
    const char* clock;         /* "04:55", or NULL */
    const char* theme_name;

    /* Hero */
    const char* hero_eyebrow;  /* "CONTINUE WATCHING" / "WELCOME" */
    const char* hero_title;
    const char* hero_detail;
    const char* hero_action;   /* chip label, or NULL to hide the chip */
    int         hero_progress; /* 0..1000, or -1 */
    const uint32_t* hero_art;
    int         hero_art_w;
    int         hero_art_h;
    int         hero_focused;

    /* "JUMP BACK IN" — the visible window, already sliced by the caller */
    int         recent_total;      /* full count, for the "n OF m" marker */
    int         recent_cursor;     /* absolute index under the cursor, or -1 */
    int         recent_visible;    /* how many of `recent` are populated */
    evo_rmlui_launch_tile_t recent[EVO_RMLUI_TILES];

    /* "LIBRARY" — the destination shelf */
    int         library_visible;
    evo_rmlui_launch_tile_t library[EVO_RMLUI_TILES];
} evo_rmlui_launch_params_t;

/*
 * Generic scrolling list screen — RECENT, FAVORITES, EMBY SETUP and EMBY
 * BROWSE are all `evo_list_model` in the immediate-mode UI, so they are one
 * document here too rather than four near-identical ones.
 *
 * The caller hands over the visible window, already sliced, exactly as the
 * launch shelves do: the DOM holds a fixed number of rows and never grows,
 * so a library of five thousand files costs the same as one of five.
 */
#define EVO_RMLUI_LIST_ROWS 9

typedef struct {
    const char* title;
    const char* detail;
    const char* icon_path;
    const char* badge;
    int         progress;     /* 0..1000, or -1 for no bar */
    int         has_chevron;
    int         is_focused;
} evo_rmlui_list_row_t;

typedef struct {
    const char* title;
    const char* subtitle;
    int         section;      /* EVO_SECTION_* — lights the rail */
    int         rail_focused;

    int         row_count;    /* rows populated below (the visible window) */
    int         total_count;  /* full list length, for the "n OF m" counter */
    int         cursor_index; /* absolute index under the cursor, or -1 */
    evo_rmlui_list_row_t rows[EVO_RMLUI_LIST_ROWS];

    /* Empty state. A list with nothing in it says why, rather than
     * presenting an empty frame the user has to interpret. */
    int         is_empty;
    const char* empty_title;
    const char* empty_hint;
    const char* empty_icon;

    /* Footer hints vary per screen — FAVORITES offers REMOVE, EMBY does not. */
    int         hint_count;
    struct {
        const char* glyph_path;
        const char* label;
    } hints[4];
} evo_rmlui_list_params_t;

/*
 * USB storage browser — the two-pane screen.
 *
 * `rows` is the visible window only, already sliced by the caller: a folder
 * can hold hundreds of files, and formatting all of them every frame to draw
 * twelve is work nobody sees.
 */
#define EVO_RMLUI_BROWSER_ROWS 12
#define EVO_RMLUI_BROWSER_PROPS 9

typedef struct {
    const char* name;
    const char* detail;
    const char* icon_path;
    const char* badge;       /* DIR / 4K / HD / AUDIO */
    int         progress;    /* 0..1000 resume position, or -1 */
    int         is_favorite;
    int         is_focused;
} evo_rmlui_browser_row_t;

typedef struct {
    const char* path;          /* breadcrumb, already trimmed for display */
    const char* title;         /* header title, e.g. the active source's name */
    int         at_root;       /* hides the "back" hint */
    int         rail_focused;

    int         total_count;   /* items in the folder */
    int         cursor_index;  /* absolute index under the cursor, or -1 */
    int         row_count;     /* rows populated below */
    evo_rmlui_browser_row_t rows[EVO_RMLUI_BROWSER_ROWS];

    int         is_empty;
    const char* empty_title;
    const char* empty_hint;

    /* Inspector */
    const char* ins_name;
    const char* ins_kind;
    const char* ins_ext;
    int         ins_probing;
    const char* ins_preview_badge;
    const uint32_t* ins_preview;   /* BGRA frame from the file, or NULL */
    int         ins_preview_w;
    int         ins_preview_h;
    int         ins_prop_count;
    struct {
        const char* key;
        const char* value;
    } ins_props[EVO_RMLUI_BROWSER_PROPS];
} evo_rmlui_browser_params_t;

/*
 * Changelog — master-detail. The releases are the master column; the items
 * belong to whichever release the cursor is on.
 */
#define EVO_RMLUI_CL_RELEASES 8
#define EVO_RMLUI_CL_ITEMS    14

typedef struct {
    const char* version;
    const char* tagline;
    const char* date;
    int         is_focused;
} evo_rmlui_cl_release_t;

typedef struct {
    const char* kind;   /* NEW / FIXED / IMPROVED / REMOVED */
    const char* text;
} evo_rmlui_cl_item_t;

typedef struct {
    const char* title;
    const char* subtitle;
    int         rail_focused;

    int         release_count;
    int         release_total;
    int         cursor_index;
    evo_rmlui_cl_release_t releases[EVO_RMLUI_CL_RELEASES];

    const char* detail_version;
    const char* detail_tagline;
    int         item_count;
    int         item_total;    /* to report anything the panel could not fit */
    evo_rmlui_cl_item_t items[EVO_RMLUI_CL_ITEMS];
} evo_rmlui_changelog_params_t;

/*
 * Text reader — a single scrolling pane. The visible window (lines[]) is
 * already wrapped and paged by evo_textreader.c; this only ever displays it.
 */
#define EVO_RMLUI_READER_LINES 64

typedef struct {
    const char* title;
    const char* subtitle;
    const char* badge;
    int         rail_focused;

    const char* lines[EVO_RMLUI_READER_LINES];
    int         line_count;
    int         face;          /* EVO_FACE_MENU or EVO_FACE_SMALL - reading size */

    double      progress;      /* 0..1, how far down the document the top is */
    double      visible_frac;  /* 0..1, how much of it is on screen */

    const char* notice;        /* shown instead of lines: error / empty file */
    const char* footnote;      /* shown under the lines: "FIRST 2MB OF 47MB" */
} evo_rmlui_reader_params_t;

/*
 * Surround sound test — a spatial room diagram, not a list. Each speaker
 * carries its own screen offset from the listener position (dx, dy), matching
 * the layout evo_screen_surround_test() already draws.
 */
#define EVO_RMLUI_SURROUND_SPEAKERS 8

typedef struct {
    const char* name;   /* "FRONT LEFT" */
    const char* label;  /* "FL" */
    double      hz;     /* test tone frequency */
    int         dx;
    int         dy;
    int         ch;         /* channel index, matches active_channel */
    int         item_idx;   /* matches selected_item when this node has the cursor */
    int         hidden;     /* side-surround speakers, hidden in 5.1 layout */
} evo_rmlui_surround_speaker_t;

typedef struct {
    int         rail_focused;
    int         is_51_layout;
    int         selected_item;   /* 0-4 auto/layout/silence actions, 5-12 speakers */
    int         active_channel;  /* -1 = none; driven by the audio test thread */
    int         surround_mode;   /* 0 = idle */

    evo_rmlui_surround_speaker_t speakers[EVO_RMLUI_SURROUND_SPEAKERS];
    int         speaker_count;
} evo_rmlui_surround_params_t;

/* Initialize RmlUi Retained Engine */
bool evo_rmlui_init(int screen_width, int screen_height);
void evo_rmlui_shutdown(void);
bool evo_rmlui_is_initialized(void);

/* Playback OSD API */
void evo_rmlui_update_playback_params(const evo_playback_osd_params_t* params);
void evo_rmlui_render_playback_osd(uint32_t* framebuffer, int width, int height);

/* Confirmation & Modal Dialog API */
void evo_rmlui_update_dialog(const evo_rmlui_dialog_params_t* params);
void evo_rmlui_render_dialog(uint32_t* framebuffer, int width, int height);

/* Toast notification API (#75). Renders in its own RmlUi context, composited
 * over whatever the caller already rendered into framebuffer this frame -
 * call evo_rmlui_render_toast() AFTER the screen's own render call. */
void evo_rmlui_update_toast(const evo_rmlui_toast_params_t* params);
void evo_rmlui_render_toast(uint32_t* framebuffer, int width, int height);

/* Launch / home screen API */
void evo_rmlui_update_launch(const evo_rmlui_launch_params_t* params);
void evo_rmlui_render_launch(uint32_t* framebuffer, int width, int height);

/* Generic list screen API (recent, favorites, emby setup, emby browse) */
void evo_rmlui_update_list(const evo_rmlui_list_params_t* params);
void evo_rmlui_render_list(uint32_t* framebuffer, int width, int height);

/* USB storage browser API */
void evo_rmlui_update_browser(const evo_rmlui_browser_params_t* params);
void evo_rmlui_render_browser(uint32_t* framebuffer, int width, int height);

/* Changelog API */
void evo_rmlui_update_changelog(const evo_rmlui_changelog_params_t* params);
void evo_rmlui_render_changelog(uint32_t* framebuffer, int width, int height);

/* Text reader API */
void evo_rmlui_update_reader(const evo_rmlui_reader_params_t* params);
void evo_rmlui_render_reader(uint32_t* framebuffer, int width, int height);

/* Surround sound test API */
void evo_rmlui_update_surround(const evo_rmlui_surround_params_t* params);
void evo_rmlui_render_surround(uint32_t* framebuffer, int width, int height);

/* Settings API */
void evo_rmlui_update_settings(const evo_rmlui_settings_params_t* params);
void evo_rmlui_render_settings(uint32_t* framebuffer, int width, int height);

/* About & Support Screen API */
typedef struct {
    const char* app_name;        /* "EVO PLAYER PRO" */
    const char* version;         /* "v0.9.0" */
    const char* build_tag;       /* "PS5 HOMEBREW" */
    const char* tagline;         /* "CINEMATIC MEDIA PLAYER FOR PLAYSTATION 5 HOMEBREW" */
    const char* themes_info;     /* "X AVAILABLE - DROP .THEME FILES ON USB0" */
    int         action_focused;  /* 1 if "View Changelog" button is focused, 0 if rail is focused */
} evo_rmlui_about_params_t;

void evo_rmlui_update_about(const evo_rmlui_about_params_t* params);
void evo_rmlui_render_about(uint32_t* framebuffer, int width, int height);

/* Subtitles Track Selection Modal API */
void evo_rmlui_update_subtitles(const evo_rmlui_subtitles_params_t* params);
void evo_rmlui_render_subtitles(uint32_t* framebuffer, int width, int height);

/* Media Info Technical Specifications Modal API */
void evo_rmlui_update_mediainfo(const evo_rmlui_mediainfo_params_t* params);
void evo_rmlui_render_mediainfo(uint32_t* framebuffer, int width, int height);

/* Footer build number, shown on every screen. Set once at startup — it was
 * a literal in each .rml and had already drifted a release behind. */
void evo_rmlui_set_version(const char* version);

/* Centralized Theme API */
typedef struct {
    const char* name;
    uint32_t bg_top;
    uint32_t bg_bottom;
    uint32_t surface;
    uint32_t surface_sel;
    uint32_t border;
    uint32_t border_sel;
    uint32_t accent;
    uint32_t accent_soft;
    uint32_t accent_alt;
    uint32_t text_primary;
    uint32_t text_secondary;
    uint32_t text_muted;
} evo_rmlui_theme_t;

void evo_rmlui_set_theme(const evo_rmlui_theme_t* theme);

/* Sidebar navigation rail API */
typedef struct {
    int active_section;   /* 0=HOME 1=BROWSE 2=RECENT 3=FAV 4=EMBY 5=SETTINGS 6=ABOUT */
    int rail_focused;     /* 0=collapsed, 1=expanded */
    int cursor_index;     /* which item has cursor when rail is expanded */
    int visible;          /* 1=show rail, 0=hide (full-screen OSD, modals) */
} evo_rmlui_nav_params_t;

void evo_rmlui_update_nav(const evo_rmlui_nav_params_t* params);

/*
 * #28 Phase 4: GPU geometry present. evo_rmlui_agc_geo_active() is 1 when the
 * last cached menu render diverted its solid geometry to the GPU sink (mesh
 * shaders up + /mnt/usb0/evo_agc_ui set). main.c then calls
 * evo_rmlui_agc_geo_present() at flip time instead of the CPU tiler /
 * pp_agc_present_ui: it submits that batch as one DCB + queues the flip.
 * Returns the pp_agc_present_geo rc (0 submitted / -1 fail / -2 watchdog) or
 * 1 if there was nothing to present (caller should fall back).
 */
int evo_rmlui_agc_geo_active(void);
int evo_rmlui_agc_geo_present(int vout_handle, uint32_t buf_idx, void* gpu_target,
                              int target_linear, uint32_t out_w, uint32_t out_h,
                              int64_t flip_marker);

#ifdef __cplusplus
}
#endif

#endif /* EVO_RMLUI_BRIDGE_H */
