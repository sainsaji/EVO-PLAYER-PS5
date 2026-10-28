/*
 * evo_rmlui_provider.h — the host for a provider's own UI document (#90).
 *
 * This is the half of the provider seam that renders. It is deliberately NOT
 * part of EvoRmlApp: EVO's own screens keep the immediate-mode push model they
 * have today - hardcoded document pointers, fixed `lrow-N` ids, theme applied
 * as inline properties, input resolved in C++ before RmlUi sees it - and none
 * of that can work for a document EVO did not write.
 *
 * A provider document cannot know EVO's element ids, so it gets the three
 * things that make it self-sufficient, all of them new to this codebase:
 *
 *   DATA BINDING    a named Rml data model. The markup uses data-for /
 *                   data-if / data-value and never names an EVO element.
 *   NATIVE FOCUS    D-pad goes straight into Rml::Context::ProcessKeyDown, so
 *                   RmlUi's own spatial navigation (the RCSS `nav-*`
 *                   properties) moves focus and `:focus` styling comes from
 *                   the provider's stylesheet rather than a pushed bool.
 *   ITS OWN PALETTE the theme push is opt-OUT here. evo_rmlui_set_theme
 *                   stamps colours onto every element as inline properties,
 *                   which would overwrite exactly what a provider bundle
 *                   exists to supply.
 *
 * One context, created on screen enter and destroyed on exit, following the
 * four-context pattern already proven by main/toast/keyboard/debug (#75).
 *
 * FAILURE IS ALWAYS VISIBLE
 *
 * Every way a bundle can fail ends at the embedded fallback skin with the
 * reason printed on it. There is no path here that produces a blank screen or
 * a hang - on a console with no console, an unreportable failure is worse than
 * an ugly one.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <RmlUi/Core.h>

extern "C" {
#include "evo_provider.h"
#include "evo_rmlui_bridge.h"   /* evo_hui_provider_params_t */

class EvoRenderBridge;
#include "evo_provider_bundle.h"
}

/* ------------------------------------------------------------------------- */
/* The data model                                                            */
/* ------------------------------------------------------------------------- */
/*
 * What a provider bundle may bind to. This IS the API surface #90's manifest
 * `api_version` versions: adding a field is compatible, renaming or removing
 * one is not, and EVO_PROVIDER_API_VERSION must be bumped for the latter.
 *
 * Kept flat and stringly-typed on purpose. A bundle author has no compiler and
 * no debugger; a model of plain strings and bools binds predictably in RCSS
 * and degrades to empty rather than to a crash.
 */
struct EvoProviderRow {
    Rml::String id;         /* opaque provider item id - pass back, never parse */
    Rml::String title;
    Rml::String subtitle;
    Rml::String overview;
    Rml::String art;        /* "evo:mem/..." once the poster has arrived, else "" */
    Rml::String now;        /* EPG now, "" when there is none */
    Rml::String next;
    Rml::String duration;   /* pre-formatted "1:42:10", "" for live and folders */
    /* The title's first character, upper-cased. A placeholder tile for a row
     * with no artwork is something every catalog UI needs and RmlUi has no
     * substring transform, so it is published rather than left to the bundle
     * to fake with clipping. */
    Rml::String initial;
    bool is_folder = false;
    bool is_live = false;
    int  index = 0;         /* position in `rows`, for data-event callbacks */
    /* Media-server rows: how far in (0-100, 0 when not started) and whether
     * the service marks it watched. */
    int  progress = 0;
    Rml::String progress_w;   /* the same as a CSS width ("37%") for data-style-width */
    bool played = false;
    bool favorite = false;
    /* Not bound: where playback resumes, handed to the player on activation. */
    long long resume_sec = 0;
    /* Not bound: what the movie / show detail page (#221) shows. */
    std::string rating, cert, facts, art_url;
    long long duration_sec = 0;
    int  detail = 0;        /* evo_provider_item_t::detail */
};

/* One row of the side panel (Live TV options): Settings' row language. */
struct EvoPanelRow {
    Rml::String id;
    Rml::String title;
    Rml::String detail;
    Rml::String badge;      /* "" = none */
    Rml::String icon;       /* used when !radio */
    std::string art_url, art_key, initial;   /* headshot, its art key, the letter when there is none */
    bool radio = false;     /* a pick-one row: radio instead of an icon */
    bool on = false;        /* radio selected */
    bool warn = false;      /* detail in amber */
    bool chevron = false;   /* leads somewhere */
    bool badge_live = false;
    bool focused = false;
};

/* One poster on a shelf (the media-server home). */
struct EvoShelfCard {
    Rml::String id, title, subtitle, overview, art, initial, now, next;
    std::string art_url, art_key;
    std::string backdrop_url, logo_url;
    std::string rating, cert, facts, cast;   /* the Details block; cast comes later, from the details panel */
    int  cast_state = 0;                     /* 0 not asked, 1 asked, 2 answered */
    bool is_folder = false, is_live = false, played = false, favorite = false, see_all = false;
    int  detail = 0;                         /* evo_provider_item_t::detail */
    int  progress = 0;
    long long resume_sec = 0;
    long long duration_sec = 0;
};
struct EvoShelf {
    std::string parent;          /* the provider id of the level it came from */
    Rml::String title;
    std::vector<EvoShelfCard> cards;
    bool fresh = false;          /* loaded since this home was built */
    bool loading = false;        /* its request is out */
    int  col = 0;               /* focused card */
    int  first = 0;              /* left edge of the window, in cards */
};

struct EvoProviderModel {
    Rml::String provider_name;
    Rml::String breadcrumb;     /* "IPTV / Sports" */
    Rml::String status;         /* "" when idle, else a message to show */
    Rml::String query;          /* current search term, "" when browsing */
    bool loading = false;
    bool tuning = false;
    bool has_error = false;
    bool empty = false;         /* loaded, and there is genuinely nothing */
    /* True when `rows` is a level of FOLDERS rather than playable items - an
     * M3U's group-title level, an Emby library list. A bundle's header cannot
     * work this out from the rows (RCSS data expressions cannot aggregate) and
     * every catalog UI wants to word itself differently for the two. */
    bool is_folder_level = false;
    bool in_folder = false;     /* below the root: an empty level is "no channels", not setup */
    int  count = 0;
    std::vector<EvoProviderRow> rows;

    /* Deterministic page-based navigation (#ProsperoTV design) */
    Rml::String page_info;          /* "PAGE 2 OF 45" */
    int  page_current = 1;
    int  page_count = 1;
    bool has_multiple_pages = false;

    /* Selected / Focused item preview details (for right-hand preview panel) */
    Rml::String selected_id;
    Rml::String selected_title;
    Rml::String selected_subtitle;
    Rml::String selected_art;
    Rml::String selected_initial;
    Rml::String selected_now;
    Rml::String selected_next;
    Rml::String epg_status;   /* why the guide panel is empty (iptv) */
    bool epg_setup = false;   /* offer "set up a guide" in that box */

    /* The setup screen, opened over a provider that already has a source: it
     * must not lose it (Back returns to it; Sign out is the explicit removal). */
    bool setup_configured = false;
    Rml::String setup_account;   /* "Signed in to up.kiwi" - host only, never credentials */

    /* The side panel (Live TV options). */
    bool panel_open = false;
    Rml::String panel_crumb;        /* "OPTIONS › " - dimmed lead-in, may be "" */
    Rml::String panel_eyebrow;      /* "LIVE TV" / "CHANNEL GUIDE" */
    Rml::String panel_title;
    Rml::String panel_sub;
    Rml::String panel_note_b;       /* bold lead of the note, "" = no note */
    Rml::String panel_note;
    Rml::String panel_accept;       /* the X hint: "SELECT" */
    Rml::String panel_back;         /* the O hint: "CLOSE" / "BACK" */
    std::vector<EvoPanelRow> panel_rows;
    Rml::String selected_num;       /* "CH 14" */
    Rml::String selected_tech;      /* "LIVE HLS • 1080p • 60 FPS" */
    bool has_selected = false;
    bool selected_is_folder = false;

    /* Provider-neutral detail fields (media servers, addons). */
    Rml::String selected_overview;
    Rml::String selected_resume;    /* "Resume from 23:10", "" when not started */
    int  selected_progress = 0;     /* 0-100 */
    Rml::String selected_progress_w;  /* "37%" */
    bool selected_played = false;
};

/* ------------------------------------------------------------------------- */
/* The render budget & grid layout                                           */
/* ------------------------------------------------------------------------- */
/*
 * Fixed 2x4 grid of 8 cards per page (inspired by ProsperoTV).
 * Deterministic paging replaces unstable infinite-scroll virtualization.
 * Zero scroll jumping, zero DCB overflows, instant 60 FPS rendering.
 */
constexpr size_t EVO_PROVIDER_COLS = 8;                /* a folder level: 8 cards wide, as the kit draws it */
constexpr size_t EVO_PROVIDER_PAGE_SIZE = 16;          /* two rows */
constexpr size_t EVO_PROVIDER_ROW_WINDOW = 8;
constexpr size_t EVO_PROVIDER_ROW_WINDOW_MAX = 8;
constexpr size_t EVO_PROVIDER_ROW_WINDOW_STEP = 8;
constexpr size_t EVO_PROVIDER_ROW_WINDOW_MARGIN = 0;

/* ------------------------------------------------------------------------- */
/* Host                                                                      */
/* ------------------------------------------------------------------------- */

class EvoRmlProviderHost {
public:
    static EvoRmlProviderHost& Instance();

    /*
     * Open `provider_id`'s screen. Loads the cached bundle if there is one,
     * the embedded fallback skin if there is not, and kicks a background
     * refresh either way - so the first frame is instant and the bundle
     * updates behind it.
     *
     * Returns false only if the provider itself is unknown or disabled. A
     * bundle problem is not a failure to open: it opens on the fallback skin
     * with the reason in `status`.
     */
    bool Open(const char* provider_id, int width, int height);
    void Close();
    bool IsOpen() const { return m_context != nullptr; }
    const char* ProviderId() const { return m_provider_id.c_str(); }

    /* Once per frame while the screen is up: pumps the bundle refresh, the art
     * queue, and any catalog request that has completed. */
    void Tick();

    /* True when the kit drew the screen, rail included. */
    bool Render(uint32_t* framebuffer, int width, int height);
    bool RenderKit(int width, int height);

    /*
     * D-pad and buttons. Returns true when the host consumed the key, false
     * when the caller should handle it - which is how Back gets out: at the
     * root of the provider's catalog Back is NOT consumed, and the rail takes
     * it. Anywhere deeper it pops a level and is consumed.
     */
    enum Key { KeyUp, KeyDown, KeyLeft, KeyRight, KeyAccept, KeyBack, KeySearch, KeyPageUp, KeyPageDown };
    bool HandleKey(Key k);

    /* True when something changed and the screen needs re-rasterising. The
     * device loop only redraws on change (GlNeedsFrame). While tuning or
     * loading, keep frames pumping so the CSS spinner animation rotates. */
    bool NeedsFrame() const { return m_dirty || m_model.tuning || m_model.loading; }
    void ClearFrameFlag() { m_dirty = false; }

    void SetLoading(bool loading, const std::string& status = "");
    void SetTuning(bool tuning);
    void SetStatus(const std::string& msg, bool error);
    void Search(const char* query);
    const char* CurrentQuery() const { return m_model.query.c_str(); }

    int  TakeAction();
    void ShowUsbPlaylists(const std::vector<std::string>& paths);
    bool IsUsbPickerActive() const { return m_is_usb_picker; }
    void ShowSetupScreen();

    /* The side panel over the current level. X on a row, or Circle, is reported
     * through TakeChoice() ("panel:back" for Circle); the caller decides what
     * comes next - another page, or HidePanel(). */
    void ShowPanel(const EvoProviderModel& page, int focus);
    void HidePanel();
    bool PanelOpen() const { return m_model.panel_open; }
    std::string TakeChoice();
    const EvoProviderRow* GetFocusedRow() const;
    void ReloadCurrentLevel();
    const char* CurrentParent() const { return m_stack.empty() ? "" : m_stack.back().c_str(); }
    void OpenLevel(const char* id, const char* title);

    /* The shelves home (media servers): rows of posters instead of one level. */
    bool ShelvesActive() const;

private:
    EvoRmlProviderHost() = default;
    ~EvoRmlProviderHost() = default;
    EvoRmlProviderHost(const EvoRmlProviderHost&) = delete;
    EvoRmlProviderHost& operator=(const EvoRmlProviderHost&) = delete;

    bool BuildContext(int width, int height);
    bool LoadEntryDocument(const std::string& rml_path);
    bool LoadFallbackSkin();
    bool RegisterDataModel();
    void RequestPage(const char* parent_id, int page);
    void ApplyItems(const evo_provider_item_t* items, int count, int has_more);
    /* Publish the current page of 8 items into m_model.rows */
    void PublishRowWindow();
    /* Set page offset (deterministic paging) and focus slot */
    void SetPageOffset(size_t new_offset, int target_slot = -1);
    /* Update the right-side detail preview panel to match current focus */
    void UpdateSelectedPreview();
    void FetchMoreAndAdvance(int col);
    /* Focus the published card at `local_idx`, found by position among the
     * siblings data-for produced. Position, not `rowid`: the attribute still
     * holds the pre-slide value until the next Context::Update(), while the
     * element order is already correct because nothing was re-instanced. */
    bool FocusPublishedRow(int local_idx);
    /* Index into m_model.rows of the focused card, via the `rowid` attribute
     * the seam already requires - so this stays bundle-agnostic. -1 if focus
     * is not on a row. */
    int  FocusedRowIndex() const;
    void PushArtRequests();
    void ApplyPendingActivation();
    bool EnforceDomCap();

    static void ItemsCallback(int ok, const evo_provider_item_t* items,
                              int count, int has_more, void* ud);
    static void BundleCallback(evo_bundle_status_t st,
                               const evo_bundle_manifest_t* m, void* ud);

    std::string m_provider_id;
    const evo_provider_t* m_provider = nullptr;

    Rml::Context* m_context = nullptr;
    Rml::ElementDocument* m_doc = nullptr;
    Rml::DataModelHandle m_model_handle;
    std::string m_model_name;

    EvoProviderModel m_model;

    /* Where we are in the provider's tree. The root is an empty string; each
     * push is an opaque item id, so EVO never builds or parses a path. */
    std::vector<std::string> m_stack;
    std::vector<std::string> m_crumbs;

    /*
     * Parallel to m_model.rows: the art URL each row wants, and the evo:mem
     * key it was promised. Kept out of the data model because a bundle has no
     * business seeing a URL - it binds to `art`, which is either a registered
     * key or empty, and never to something it could turn into a request.
     */
    std::vector<std::string> m_art_urls;
    std::vector<std::string> m_art_keys;

    /*
     * The whole level. m_model.rows is the m_row_window rows starting at
     * m_row_offset.
     *
     * m_art_urls / m_art_keys are parallel to THIS, not to m_model.rows, and
     * are indexed ABSOLUTELY: published row i is m_all_rows[m_row_offset + i].
     * They used to agree by accident because the window always started at 0;
     * once it slides they do not, and reading them at the published index hands
     * one row's poster to another.
     */
    std::vector<EvoProviderRow> m_all_rows;
    size_t m_row_window = 0;
    size_t m_row_offset = 0;

    int  m_page = 0;
    bool m_has_more = false;
    /* A page-down past the loaded rows is waiting on the next page. */
    bool m_advance_after_load = false;
    int  m_advance_col = 0;
    size_t m_saved_offset = 0;
    int    m_saved_slot = -1;
    /*
     * A page has arrived and nothing has focus yet.
     *
     * RmlUi's spatial navigation moves FROM a focused element and has no
     * "pick something" behaviour of its own, so with nothing focused the first
     * D-pad press does nothing at all and the screen looks hung. Focusing the
     * document is not enough either - the document is not a row.
     *
     * Resolved by sending one KI_TAB, which ElementDocument turns into
     * FindNextTabElement: generic, so the host still does not need to know a
     * single element id. It has to happen after Context::Update() has created
     * the data-for elements, which is why it is a flag and not a call.
     */
    bool m_needs_initial_focus = false;

    /*
     * Which card that seeded focus lands on: 0 for a level just opened, the
     * remembered card on Back / reload / page flip. Seeding by KI_TAB alone
     * moved focus one card PAST the one data-for had recycled - so a group
     * opened from slot 5 started on its 7th channel, and a group with fewer
     * channels than that started on the WATCH button or nowhere, D-pad dead.
     */
    int m_focus_slot = 0;
    bool SeedFocus();

    /* Offset + card of every level above this one, so Back lands where the
     * user left, not on the first card of page 1. Parallel to m_stack. */
    struct LevelPos { size_t offset; int slot; };
    std::vector<LevelPos> m_pos_stack;

    /*
     * A row the user activated, waiting for Tick() to act on it.
     *
     * The Rml event handler that receives the activation runs inside
     * Context::Update(), which is mid-walk over the very elements the outcome
     * destroys - a folder push rebuilds `rows`, and starting playback tears
     * down and rebuilds the VideoOut configuration. Both are done one frame
     * later, from Tick(), where nothing is iterating anything.
     */
    std::string m_pending_activation;
    bool m_dirty = true;
    bool m_using_fallback = false;
    /*
     * The bundle version this screen is currently rendering.
     *
     * Without it the refresh callback could not tell "a newer bundle arrived"
     * from "the cache was already current", and reopened the screen either
     * way - which made the reopen kick a fresh refresh, which completed OK and
     * reopened again. The screen was torn down and rebuilt on a loop, so focus
     * reset to the first row every frame and an activation was wiped before
     * Tick() could act on it. Both looked like input bugs.
     */
    std::string m_bundle_version;

    /* Counts the in-flight catalog request generation, so a reply that arrives
     * after the user has already navigated away is dropped instead of
     * repopulating the screen they left. */
    unsigned m_request_generation = 0;

    int  m_pending_action = 0;
    bool m_is_usb_picker = false;
    int  m_panel_focus = 0;
    bool m_setup_shown = false;   /* the setup cards are on screen */
    std::string m_choice;
    void PanelMove(int dir);
    void PublishPanel();
    std::vector<std::string> m_saved_usb_playlists;

    /* ---- shelves (evo_rmlui_provider_shelves.cpp) ---- */
    bool ShelfCapable() const;
    bool BeginShelves(const evo_provider_item_t* items, int count);
    void BuildShelves(const evo_provider_item_t* items, int count);
    void ExpandNextServer();
    void FinishExpand();
    static void ExpandCallback(int ok, const evo_provider_item_t* items, int count, int has_more, void* ud);
    void LoadNextShelf();
    void AddShelfCards(EvoShelf& s, const evo_provider_item_t* items, int count, int has_more);
    bool ShelfKey(Key k);
    int  NextShelf(int from, int dir) const;
    int  ShelvesWithCards() const;
    void SyncShelfFocus();
    void PushShelfArt();
    void PushPanelArt();
    void PushShelfBackdrop();
    void FillShelfParams(evo_hui_provider_params_t& p, EvoRenderBridge* r);
    const EvoProviderRow* ShelfFocusedRow() const;
    static void ShelfCallback(int ok, const evo_provider_item_t* items, int count, int has_more, void* ud);
    static void CastCallback(int ok, const evo_provider_row_t* rows, int count, void* ud);

    std::vector<EvoShelf> m_shelves;
    bool m_shelves_on = false;          /* this provider shows its home as shelves */
    bool m_shelf_wait_root = false;     /* the next root reply defines the shelves */
    int  m_shelf_row = 0;               /* focused shelf (index into m_shelves) */
    unsigned m_shelf_gen = 0;
    bool m_shelf_moved = false;         /* the user has moved the cursor on this home */
    /* The focused card's wide image and logo: asked for once the cursor has rested. */
    std::string m_bd_id;                 /* card the two below belong to */
    std::string m_bd_url, m_bd_key;
    std::string m_logo_url, m_logo_key;
    Rml::String m_bd_art, m_logo_art;    /* ready textures, kept until the next one lands */
    long long m_bd_since_ms = 0;
    std::vector<std::string> m_expand_servers;       /* Plex: servers still to open for their libraries */
    std::vector<evo_provider_item_t> m_expand_items;
    unsigned m_expand_gen = 0;
    mutable EvoProviderRow m_shelf_focus_row;

    /* ---- the movie / show detail page (#221) ---- */
    struct DetailPerson { std::string name, role, art_url, art_key; };
    struct DetailVersion { std::string label, facts; };
    struct DetailState {
        bool open = false;
        std::string item_id, title, meta, rating, cert, overview, art_url, poster_key, resume;
        bool is_show = false;
        long long resume_sec = 0;
        int zone = 0, button = 0, tab = 0;
        std::vector<DetailPerson> cast;
        int cast_state = 0;                 /* EVO_HUI_LOAD_* */
        std::vector<DetailVersion> versions;
        int versions_state = 0;
        unsigned gen = 0;                   /* drops a reply that lands after the page closed */
        std::string backdrop_url;
        int art_wait = 0;                   /* frames spent waiting for pictures; gives up on a failed one */
    };
    DetailState m_detail;
    unsigned m_detail_gen = 0;
    bool m_detail_bypass = false;           /* the next activation opens the folder, not the page */
    bool WantsDetail(const EvoProviderRow& r) const;
    void OpenDetail(const EvoProviderRow& r);
    void CloseDetail();
    bool DetailKey(Key k);
    void DetailPlay(int mode);   /* 0 play, 1 restart, 2 choose a stream */
    void PushDetailArt();
    void FillDetailParams(evo_hui_detail_t& d, EvoRenderBridge* r);
    static void DetailCastCallback(int ok, const evo_provider_row_t* rows, int count, void* ud);

    /* The stream picker in the detail page's look: what the player screen fills each frame it is up,
     * and the item it is for (remembered when the item was activated). */
    evo_hui_detail_t m_pick;
    bool m_pick_fresh = false;
    std::string m_pick_title, m_pick_meta, m_pick_art_url, m_pick_poster_key;
    void RememberPickContext(const std::string& title, const std::string& meta, const std::string& art_url);
    void FillPickParams(evo_hui_detail_t& d, EvoRenderBridge* r);

public:
    bool DetailOpen() const { return m_detail.open; }
    void SetPick(const evo_hui_detail_t* d);
};

/* ------------------------------------------------------------------------- */
/* C entry points                                                            */
/* ------------------------------------------------------------------------- */
/*
 * The frame loop, the screen manager and the input dispatch are all C or
 * C-facing, so the host is driven through these rather than by including a C++
 * header everywhere.
 */
extern "C" {

enum {
    EVO_PROVIDER_ACTION_NONE = 0,
    EVO_PROVIDER_ACTION_SETUP_URL = 1,
    EVO_PROVIDER_ACTION_SETUP_USB = 2,
    EVO_PROVIDER_ACTION_CHOICE = 3,     /* evo_rmlui_provider_take_choice() has it */
};

int  evo_rmlui_provider_open(const char *provider_id, int width, int height);
void evo_rmlui_provider_close(void);
int  evo_rmlui_provider_is_open(void);
void evo_rmlui_provider_tick(void);
/* 1 when the kit drew the screen and the rail with it (no nav overlay needed). */
int evo_rmlui_provider_render(uint32_t *framebuffer, int width, int height);
int  evo_rmlui_provider_needs_frame(void);
void evo_rmlui_provider_clear_frame(void);

/* Returns 1 when consumed. `key` is an EvoRmlProviderHost::Key value. */
int  evo_rmlui_provider_key(int key);

/* The movie / show detail page (#221) is up: it takes every key, and Square, Triangle and
 * Options must leave the screen behind it alone. */
int  evo_rmlui_provider_detail_open(void);
/* The "Resume playback" setting: with it off the page has no Restart button. */
void evo_rmlui_provider_set_resume_enabled(int on);
/* The stream picker: call every frame it is up (strings must outlive the frame), then
 * evo_rmlui_provider_render(). Fill pick = 1 and the streams; title, facts line and poster come from the
 * item last activated. The kit not drawing it leaves render() returning 0. */
void evo_rmlui_provider_pick_set(const evo_hui_detail_t* d);

/*
 * The item the user just activated, for PlaybackController. Empty title means
 * nothing is pending. Cleared by evo_rmlui_provider_take_selection().
 */
typedef struct evo_provider_selection {
    char provider_id[EVO_PROVIDER_MAX_ID];
    char item_id[EVO_PROVIDER_MAX_ITEM_ID];
    char title[EVO_PROVIDER_MAX_TITLE];
    int  is_live;
    long long resume_sec;   /* where the service says playback stopped, 0 = start */
    int  force_pick;        /* open the stream picker whatever the Ask-stream setting says (detail page: CHOOSE STREAM) */
    int  skip_prompt;       /* the detail page already asked Play or Restart: no Continue / Restart / Cancel */
} evo_provider_selection_t;

int  evo_rmlui_provider_take_selection(evo_provider_selection_t *out);
int  evo_rmlui_provider_take_action(void);
void evo_rmlui_provider_set_loading(int loading, const char *status);
void evo_rmlui_provider_set_tuning(int tuning);
void evo_rmlui_provider_set_status(const char *status, int error);
void evo_rmlui_provider_search(const char *query);
const char* evo_rmlui_provider_get_query(void);
void evo_rmlui_provider_show_setup(void);
typedef struct evo_panel_row {
    const char *id, *title, *detail, *badge, *icon;
    int radio, on, warn, chevron, badge_live;
    const char *art_url;                /* a headshot to fetch, or NULL */
    const char *initial;                /* the letter to draw when there is no picture, or NULL */
} evo_panel_row_t;

typedef struct evo_panel {
    const char *crumb, *eyebrow, *title, *sub;
    const char *note_b, *note;          /* note_b "" = no note */
    const char *accept, *back;          /* hint labels */
    const evo_panel_row_t *rows;
    int n, focus;
} evo_panel_t;

void evo_rmlui_provider_show_panel(const evo_panel_t *p);
void evo_rmlui_provider_hide_panel(void);
int  evo_rmlui_provider_panel_open(void);
int  evo_rmlui_provider_take_choice(char *out, size_t out_sz);
const char* evo_rmlui_provider_get_focused_title(void);
const char* evo_rmlui_provider_get_focused_id(void);
int         evo_rmlui_provider_get_focused_is_folder(void);
/* The focused row's server-side state (media servers): watched, favourite. */
void        evo_rmlui_provider_get_focused_flags(int *played, int *favorite);
void        evo_rmlui_provider_reload(void);
/* The folder id the level on screen came from ("" at the root). */
const char* evo_rmlui_provider_current_parent(void);
/* Open `id` as a new level under the focused row, as activating a folder would. */
void        evo_rmlui_provider_open_level(const char *id, const char *title);

} /* extern "C" */
