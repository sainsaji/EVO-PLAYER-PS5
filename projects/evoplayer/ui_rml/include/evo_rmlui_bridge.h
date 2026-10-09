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
    const char* decoder_badge;   /* #59: "Hardware (sceVideodec2)" / "Software (FFmpeg)" */
    /* #103: what the upscaler did to the current frame - "AI UPSCALE",
     * "SHARP UPSCALE", "UPSCALE OFF · 10-BIT"... NULL/"" hides the badge
     * (upscaling switched off in Settings). upscale_active picks the style. */
    const char* upscale_badge;
    int upscale_active;
    /* #105: motion smoothing badge */
    const char* smooth_badge;
    int smooth_active;
    /* #119: Deep Blacks badge; NULL/"" hides it (setting Off). */
    const char* deepblack_badge;
    double position_sec;
    double duration_sec;
    double percentage;
    int paused;
    int scrub_active;
    double scrub_target;
    /* A seek (or the first frames) is still on its way: the centre shows a
     * LOADING spinner instead of a picture that looks frozen. */
    int loading;
    const char* audio_track;
    const char* sub_track;
    int sub_delay_ms;
    /* A live stream: no timeline, no seek, no chapters - the OSD hides them. */
    int is_live;
    int view_mode;
    int show_stats;
    int alpha;
    /* GL-5 (#81): on-video caption overlay, rendered by RmlUi (Unicode) instead
     * of the old ASCII bitmap path. subtitle_text is the active cue as UTF-8,
     * "\n"-separated lines, NULL/"" when nothing is showing. subtitle_face is
     * EVO_FACE_SUB/MENU/TITLE (small/medium/large). chrome_hidden = 1 renders
     * only the caption with the rest of the OSD suppressed (controls faded). */
    const char* subtitle_text;
    int subtitle_face;
    int subtitle_raised;
    /* #110: the secondary caption. subtitle_text2 is drawn directly above the
     * primary (subtitle2_position 0) or alone at the top of the screen (1);
     * subtitle2_color is 0 yellow, 1 cyan, 2 white. Its size follows
     * subtitle_face, one step smaller. NULL/"" when nothing is showing. */
    const char* subtitle_text2;
    int subtitle2_position;
    int subtitle2_color;
    int chrome_hidden;
    /* GL-5 (#81): dev FPS pill (#fps-pill), independent of the OSD chrome. */
    int fps;
    int debug_overlay;
    /* GL-5 (#81): audio-only playback shows the NOW PLAYING visualiser
     * (#music-view) instead of the video-centric centre overlay / captions. */
    int music_mode;
    const char* music_codec;
} evo_playback_osd_params_t;

/*
 * GL-5 (#81) / #63 first pass: the playback diagnostic HUD ("stats for nerds").
 * line_* are preformatted rows; the *_hist arrays are 0..1 sparkline samples
 * (oldest first, `hist_len` valid). Pushed only while the HUD is visible.
 */
typedef struct {
    const char* line_video;
    const char* line_audio;
    const char* line_subs;
    const char* line_perf;
    const char* line_queues;
    const char* line_clocks;
    int          hist_len;
    const float* gpu_hist;
    const float* ram_hist;
    const float* cpu_hist;
    float gpu_pct, gpu_peak_pct;
    float ram_mb,  ram_peak_mb, ram_total_mb;
    float cpu_pct, cpu_peak_pct;
} evo_perf_hud_t;

typedef struct {
    const char* eyebrow;
    const char* title;
    const char* detail;
    double progress_pct; // 0.0 to 1.0, or -1.0
    int action_count;
    int focused_action;  /* #65: index of the D-pad-focused button, -1 = none */
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

/* How a settings row presents itself. macOS-style: booleans are switches you
 * can read at a glance, not ON/OFF text you have to parse. */
enum {
    EVO_RMLUI_ROW_ACTION = 0,  /* chevron only - opens a page or runs something */
    EVO_RMLUI_ROW_TOGGLE = 1,  /* switch, driven by toggle_on */
    EVO_RMLUI_ROW_VALUE  = 2,  /* collapsed: badge + chevron; expands to OPTIONs */
    EVO_RMLUI_ROW_OPTION = 3   /* one choice under an expanded VALUE row */
};

/* Display rows the settings document can show at once. A VALUE row expands
 * in place to list every choice, so the pool has to cover the longest list
 * (themes, EVO_THEME_MAX = 12) plus the section's own rows. */
#define EVO_RMLUI_SETTINGS_ROWS 20

typedef struct {
    const char* title;
    const char* detail;
    const char* icon_path;
    const char* badge;
    int has_chevron;
    int is_focused;
    int kind;       /* EVO_RMLUI_ROW_* */
    int toggle_on;  /* TOGGLE: switch state. OPTION: is this the current choice. */
    int is_disabled; /* dimmed, no chevron; it depends on another setting */
} evo_rmlui_settings_row_t;

typedef struct {
    const char* title;
    const char* subtitle;
    const char* counter;
    int rail_active_idx;
    int rail_focused;
    /* macOS-style two-pane settings: which of the four sections the sidebar
     * highlights (0-3, -1 = none), and whether the sidebar or the detail pane
     * owns focus. The section labels are static, so they live in settings.rml. */
    int section_active;
    int sidebar_focused;
    int row_count;
    evo_rmlui_settings_row_t rows[EVO_RMLUI_SETTINGS_ROWS];
} evo_rmlui_settings_params_t;

typedef struct {
    const char* label;
    const char* detail;
    int is_current;
    int is_focused;
    int is_action;    /* not a track (AUTO-SYNC, #102): no radio check */
    int is_disabled;  /* dimmed, activation does nothing */
    const char* tag;  /* #110: "PRIMARY" / "SECONDARY" badge, NULL/"" for none */
} evo_rmlui_subtitles_track_t;

typedef struct {
    const char* eyebrow;
    const char* title;
    const char* size_str;
    const char* sync_str;
    const char* sync2_str;   /* #110: the secondary track's sync pill, NULL/"" hides it */
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
    const char* decoder;   /* #37: "Hardware (sceVideodec2)" / "Software (FFmpeg)" */
    const char* upscaler;  /* #103: "Off" / "Sharp" / "AI (shader)" (+ bypass reason) */
    const char* motion_smoothing; /* #105: "Off" / "Low" / "High" (+ bypass reason) */
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
#define EVO_RMLUI_LIST_MENU_ROWS 7

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

    /* OPTIONS menu over the list, the same card as the file browser's
     * operations menu. menu_count 0 = closed. */
    int         menu_count;
    int         menu_focus;
    const char* menu_eyebrow;
    const char* menu_title;
    const char* menu_sub;
    const char* menu_icon;
    struct {
        const char* label;
        const char* desc;
        const char* icon_path;
        int         danger;
    } menu[EVO_RMLUI_LIST_MENU_ROWS];
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
    const char* duration;    /* e.g. 02:14:32 or empty */
    int         progress;    /* 0..1000 resume position, or -1 */
    int         is_favorite;
    int         is_focused;
    const uint32_t* art;     /* Thumbnail pixels BGRA or NULL */
    int         art_w;
    int         art_h;
} evo_rmlui_browser_row_t;

typedef struct {
    const char* path;          /* breadcrumb, already trimmed for display */
    const char* title;         /* header title, e.g. the active source's name */
    int         at_root;       /* hides the "back" hint */
    int         rail_focused;

    /* Places / Sources sidebar */
    int         sidebar_focused;
    int         sidebar_index;   /* 0..3 */
    int         active_source;   /* 0..3 */

    /* Folder filter chips. Only the chips this folder earned are sent:
     * "All" plus one per category actually present, so an empty list means
     * there is nothing worth choosing between and the row is hidden. */
    int         filter_focused;
    int         filter_count;    /* 0, or 2..4 */
    int         filter_selected; /* index into filter_labels */
    const char* filter_labels[4];

    int         total_count;   /* items in the folder */
    int         cursor_index;  /* absolute index under the cursor, or -1 */
    int         row_count;     /* rows populated below */
    evo_rmlui_browser_row_t rows[EVO_RMLUI_BROWSER_ROWS];

    int         is_empty;
    const char* empty_title;
    const char* empty_hint;

    /* Inspector & Bottom Status Strip */
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

    /* Media status specifications */
    const char* status_res;
    const char* status_vcodec;
    const char* status_acodec;
    const char* status_duration;
    const char* status_size;

    /* File Operations Action Menu */
    int         action_menu_open;
    int         action_menu_focused;
    const char* action_menu_target;
    int         action_paste_enabled;
    const char* action_target_sub;
    const char* action_clipboard_info;

    /* File Operations Transfer Progress Modal */
    int         transfer_modal_open;
    const char* transfer_op_title;
    const char* transfer_item_name;
    const char* transfer_speed_str;
    const char* transfer_bytes_str;
    const char* transfer_eta_str;
    const char* transfer_percent_str;
    double      transfer_progress_pct;
    const char* transfer_src_path;
    const char* transfer_dst_path;
} evo_rmlui_browser_params_t;

/*
 * Changelog — master-detail. The releases are the master column; the items
 * belong to whichever release the cursor is on.
 */
#define EVO_RMLUI_CL_RELEASES 8
/*
 * 12, not 14: #changelog-detail is a fixed 714dp with overflow:hidden, and a
 * .clitem costs 46dp (32dp chip + 14dp margin). Against the ~586dp the pane
 * has left under its header that is 12 rows; at 13 the last chip already
 * renders 2px past the bottom of the card, and at 14 the whole row lands
 * outside it on the page background. Measured off the rendered 0.10.0 entry,
 * which is the first release long enough to reach the limit at all - every
 * earlier one has nine items or fewer, which is why 14 was never caught.
 * Raising this needs the pane to scroll first.
 */
#define EVO_RMLUI_CL_ITEMS    12

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
 * Surround Sound Studio (#106) - a 2.5D room soundstage, not a list. Every value
 * here is live state from core/src/screens/SurroundTestScreen.cpp and its
 * services; the room maths itself is core/include/evo/audio/SpatialField.hpp.
 *
 * Stage coordinates (speaker dx/dy, orb_x/orb_y, trail) are dp from the
 * listener at the centre of the stage, +y towards the rear of the room.
 * Channel indices are the AudioOut interleave FL FR FC LFE BL BR SL SR.
 */
#define EVO_RMLUI_SURROUND_SPEAKERS 8
#define EVO_RMLUI_SURROUND_ACTIONS  8
#define EVO_RMLUI_SURROUND_TRAIL    5

typedef struct {
    const char* name;   /* "FRONT LEFT" */
    const char* label;  /* "FL" */
    double      hz;     /* test tone frequency */
    int         dx;
    int         dy;
    int         ch;         /* channel index, matches active_channel */
    int         item_idx;   /* matches selected_item when this node has the cursor */
    int         hidden;     /* not part of the current layout (6/7 in 5.1) */
} evo_rmlui_surround_speaker_t;

/* view_mode */
#define EVO_SURROUND_VIEW_STAGE       0   /* actions list / 2D speaker cursor */
#define EVO_SURROUND_VIEW_ORB         1   /* the spatial orb has the controls */
#define EVO_SURROUND_VIEW_CALIBRATION 2   /* AUTO CALIBRATION (MIC)            */

/* cal_phase - mirrors SpeakerCalibrationService::Phase, plus the intro */
#define EVO_SURROUND_CAL_INTRO     0
#define EVO_SURROUND_CAL_MIC_CHECK 1
#define EVO_SURROUND_CAL_MEASURING 2
#define EVO_SURROUND_CAL_ANALYZING 3
#define EVO_SURROUND_CAL_COMPLETE  4
#define EVO_SURROUND_CAL_ERROR     5

typedef struct {
    int         rail_focused;
    int         is_51_layout;
    int         selected_item;   /* 0-7 actions, 8-15 speakers (8 + channel) */
    int         active_channel;  /* -1 = none; driven by the audio test thread */
    int         surround_mode;   /* 0 idle, 1 sweep / auto test running, 2 orb */

    evo_rmlui_surround_speaker_t speakers[EVO_RMLUI_SURROUND_SPEAKERS];
    int         speaker_count;
    float       anim_time;       /* seconds, drives the ripples */
    float       orb_x;           /* orb ground position, stage dp */
    float       orb_y;
    int         orb_active;      /* 1 = the orb has the controls */
    float       proximity[EVO_RMLUI_SURROUND_SPEAKERS]; /* DBAP gain 0..1 per channel */

    int         view_mode;       /* EVO_SURROUND_VIEW_* */
    int         flight_mode;     /* 0 MANUAL, 1 ORBIT, 2 FLYBY */
    int         tone_follow;     /* 3D SOUND FIELD: music panned by the gain matrix */
    int         field_playing;   /* that music is playing right now */
    int         sweep_rotation;  /* the 360 sweep is running (field-panned) */
    int         nearest_channel; /* loudest channel in the gain matrix, -1 none */
    float       orb_z_m;         /* elevation above ear height, metres */
    float       azimuth_deg;     /* 0..360 clockwise from the front */
    float       elevation_deg;
    float       distance_m;      /* listener -> source */
    float       x_m, y_m;        /* +x right, +y FRONT (telemetry convention) */
    float       source_dbfs;     /* test-tone peak level */
    float       px_per_m;        /* stage scale, for the distance rings */
    float       trail_x[EVO_RMLUI_SURROUND_TRAIL];   /* recent ground positions, newest first */
    float       trail_y[EVO_RMLUI_SURROUND_TRAIL];
    int         trail_count;
    int         order[EVO_RMLUI_SURROUND_SPEAKERS];  /* physical order FL C FR SL SR SBL SBR LFE */
    int         order_count;

    /* AUTO CALIBRATION (MIC) */
    int         cal_phase;       /* EVO_SURROUND_CAL_* */
    int         cal_step;        /* 1-based speaker being measured */
    int         cal_total;
    int         cal_channel;     /* channel being measured, -1 none */
    int         cal_verifying;   /* closing FRONT LEFT drift check */
    int         cal_applied;     /* this run's result is the active playback profile */
    int         cal_profile_active; /* a saved calibration trims playback right now */
    float       cal_noise_db;
    float       cal_mic_level;   /* live 0..1 */
    const char* cal_message;
    int         cal_measured[EVO_RMLUI_SURROUND_SPEAKERS];
    int         cal_detected[EVO_RMLUI_SURROUND_SPEAKERS];
    float       cal_trim_db[EVO_RMLUI_SURROUND_SPEAKERS];
    float       cal_path_m[EVO_RMLUI_SURROUND_SPEAKERS];
    float       cal_delay_ms[EVO_RMLUI_SURROUND_SPEAKERS];
} evo_rmlui_surround_params_t;

/*
 * The media-server screen (Emby / Jellyfin / Stremio addons, "mediaserver"): a
 * 4x2 poster page plus a detail pane. Filled by EvoRmlProviderHost from its
 * data model and drawn by the kit. Focus stays with the host (RmlUi's own
 * navigation); `focus` is the local row it reports, -1 for none.
 */
#define EVO_HUI_PROVIDER_ROWS 8
typedef struct {
    const char*     title;
    const char*     subtitle;
    const char*     initial;
    const char*     art_key;     /* stable per poster, for the texture cache */
    const uint32_t* art;         /* 0xAABBGGRR, NULL until the poster arrives */
    int             art_w;
    int             art_h;
    int             is_folder;
    int             played;
    int             progress;    /* 0-100 */
    int             is_live;
    const char*     now;         /* EPG, "" when none */
    const char*     next;
} evo_hui_provider_row_t;

/* One row of the options side panel (Live TV options, guide picker). */
#define EVO_HUI_PROVIDER_PANEL_ROWS 16
typedef struct {
    const char* title;
    const char* detail;
    const char* badge;
    const char* icon;
    int         radio;
    int         on;
    int         warn;
    int         chevron;
    int         badge_live;
    int         focused;
} evo_hui_panel_row_t;

/* variant */
#define EVO_HUI_PROVIDER_MEDIASERVER 0   /* poster wall: Emby, Jellyfin, addons */
#define EVO_HUI_PROVIDER_LIVETV      1   /* channel cards + guide: IPTV */
#define EVO_HUI_PROVIDER_XTREAM      2   /* the same, worded for Xtream Codes */
#define EVO_HUI_PROVIDER_GENERIC     3   /* the fallback skin: any other provider */

typedef struct {
    const char* provider_name;
    const char* breadcrumb;
    const char* status;
    const char* query;
    const char* page_info;
    int         loading;
    int         tuning;
    int         has_error;
    int         empty;
    int         has_multiple_pages;
    int         is_folder_level;
    int         count;
    evo_hui_provider_row_t rows[EVO_HUI_PROVIDER_ROWS];
    int         row_count;
    int         focus;
    /* the detail pane */
    int         has_selected;
    int         selected_is_folder;
    int         selected_played;
    int         selected_progress;
    const char* selected_title;
    const char* selected_subtitle;
    const char* selected_initial;
    const char* selected_overview;
    const char* selected_resume;
    const char* selected_art_key;
    const uint32_t* selected_art;
    int         selected_art_w;
    int         selected_art_h;

    int         variant;                 /* EVO_HUI_PROVIDER_* */
    int         in_folder;
    const char* selected_num;            /* "CH 14" */
    const char* selected_tech;
    const char* selected_now;
    const char* selected_next;
    const char* epg_status;
    int         epg_setup;

    /* The setup screen (empty, no search): up to three cards. */
    int         setup_configured;        /* a source exists: adds SIGN OUT */
    const char* setup_account;
    int         setup_focus;             /* 0 url, 1 usb, 2 sign out, -1 none */

    /* The options side panel. */
    int         panel_open;
    const char* panel_crumb;
    const char* panel_eyebrow;
    const char* panel_title;
    const char* panel_sub;
    const char* panel_note_b;
    const char* panel_note;
    const char* panel_accept;
    const char* panel_back;
    evo_hui_panel_row_t panel_rows[EVO_HUI_PROVIDER_PANEL_ROWS];
    int         panel_row_count;
} evo_hui_provider_params_t;


/* Initialize RmlUi Retained Engine */
bool evo_rmlui_init(int screen_width, int screen_height);
void evo_rmlui_shutdown(void);
bool evo_rmlui_is_initialized(void);

/* GL-3 (#79) B2: 1 if the UI changed since the last render and the device GL
 * frame loop should redraw + eglSwapBuffers this iteration; 0 to hold the front
 * buffer (retained-mode + ps5-opengl is too slow to re-raster every frame). */
int  evo_rmlui_needs_frame(void);
/* 1 if a Context::Render() actually composited to fb 0 since the last call
 * (clears the flag). The device loop swaps only when this is set. */
int  evo_rmlui_consume_drew(void);
/* Gate for this frame: 1 = the dispatch may render (a redraw frame), 0 = hold. */
void evo_rmlui_set_active(int active);
/* Called after a successful present: clears the UI-dirty flag. */
void evo_rmlui_end_frame(void);
/* 1 = CPU rasterise + GL blit (default), 0 = RmlUi renders itself through GL3
 * (the /mnt/usb0/evo_gl_rmlui path). Decides the main.c present step. */
int  evo_rmlui_blit_mode(void);

/* Playback OSD API */
void evo_rmlui_update_playback_params(const evo_playback_osd_params_t* params);
void evo_rmlui_update_perf_hud(const evo_perf_hud_t* hud);   /* #81/#63 */
void evo_rmlui_render_playback_osd(uint32_t* framebuffer, int width, int height);

/* Confirmation & Modal Dialog API */
void evo_rmlui_update_dialog(const evo_rmlui_dialog_params_t* params);
void evo_rmlui_render_dialog(uint32_t* framebuffer, int width, int height);

/* Toast notification API (#75). Renders in its own RmlUi context, composited
 * over whatever the caller already rendered into framebuffer this frame -
 * call evo_rmlui_render_toast() AFTER the screen's own render call. */
void evo_rmlui_update_toast(const evo_rmlui_toast_params_t* params);
void evo_rmlui_render_toast(uint32_t* framebuffer, int width, int height);

/* #90: composite the navigation rail over a provider screen, whose document
 * lives in its own Rml context and therefore does not bring the rail with it.
 * Call AFTER evo_rmlui_provider_render(). */
void evo_rmlui_render_nav_overlay(uint32_t* framebuffer, int width, int height);

/* Dev debug overlay - the menu-screen FPS pill (GL-5 kept the FPS readout
 * player-only; this restores it for every non-player screen). Own RmlUi
 * context, composited like the toast: call update then render AFTER the
 * screen's own render call. `visible` follows show_debug_overlay. */
void evo_rmlui_update_debug_overlay(int fps, int visible);
void evo_rmlui_render_debug_overlay(uint32_t* framebuffer, int width, int height);

/* Virtual keyboard modal API (#81 / closes #34). evo_keyboard.c keeps the
 * buffer, layer and D-pad navigation state and pushes it here; this renders it
 * as an RmlUi document in its own context, composited over the screen like the
 * toast. */
typedef struct {
    int visible;
    int native_only;          /* native IME up: just dim, no panel */
    const char* title;
    const char* text;         /* current buffer (UTF-8) */
    const char* mode_label;   /* "LOWERCASE" / "UPPERCASE" / "SYMBOLS" */
    const char* rows[4];      /* 10 glyphs each, current layer */
    const char* action_labels[6];
    int len;
    int max_len;
    int focus_row;            /* 0..4 (4 = action bar) */
    int focus_col;
    int show_caret;
} evo_keyboard_params_t;
void evo_rmlui_update_keyboard(const evo_keyboard_params_t* params);
void evo_rmlui_render_keyboard(uint32_t* framebuffer, int width, int height);

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

/* Full-screen image viewer (#81). `pixels` is a decoded RGBA (0xAABBGGRR)
 * buffer owned by the caller (main.c's bmp_pixels); NULL / loaded==0 shows the
 * error card. */
typedef struct {
    const char* title;
    const uint32_t* pixels;
    int w;
    int h;
    int loaded;
} evo_rmlui_image_params_t;
void evo_rmlui_update_image(const evo_rmlui_image_params_t* params);
void evo_rmlui_render_image(uint32_t* framebuffer, int width, int height);

/* Surround sound test API */
void evo_rmlui_update_surround(const evo_rmlui_surround_params_t* params);
void evo_rmlui_render_surround(uint32_t* framebuffer, int width, int height);

/* Settings API */
void evo_rmlui_update_settings(const evo_rmlui_settings_params_t* params);
void evo_rmlui_render_settings(uint32_t* framebuffer, int width, int height);

/* About & Support Screen API */
typedef struct {
    const char* app_name;        /* "EVO PLAYER PRO" */
    const char* version;         /* "v0.12.0" */
    const char* build_tag;       /* "PS5 HOMEBREW" */
    const char* tagline;         /* "CINEMATIC MEDIA PLAYER FOR PLAYSTATION 5 HOMEBREW" */
    const char* themes_info;     /* "X AVAILABLE - DROP .THEME FILES ON USB0" */
    int         action_focused;  /* 1 if "View Changelog" button is focused, 0 if rail is focused */
} evo_rmlui_about_params_t;

void evo_rmlui_update_about(const evo_rmlui_about_params_t* params);
void evo_rmlui_render_about(uint32_t* framebuffer, int width, int height);

/* Safe-to-close screen: the frame EVO parks on after QUIT (closed.rml). */
void evo_rmlui_render_closed(uint32_t* framebuffer, int width, int height);

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
    int fps;              /* render FPS for the rail pill */
    int show_fps;         /* 1 = DEBUG OVERLAY is on */
    int storage_locked;   /* 1 = per-title sandbox never lifted; show the
                           * elevated-privileges banner on every screen */
} evo_rmlui_nav_params_t;

void evo_rmlui_update_nav(const evo_rmlui_nav_params_t* params);

#ifdef __cplusplus
}
#endif

#endif /* EVO_RMLUI_BRIDGE_H */
