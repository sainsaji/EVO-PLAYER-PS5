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
    int  count = 0;
    std::vector<EvoProviderRow> rows;
};

/* ------------------------------------------------------------------------- */
/* The render budget                                                         */
/* ------------------------------------------------------------------------- */
/*
 * How many rows EVO will materialise into a provider document at once.
 *
 * This is EVO's limit, not the provider's, and it belongs here rather than in
 * a bundle because only EVO knows what its renderer costs. Measured on
 * hardware with a 184-group IPTV playlist: every row became a card, every card
 * contributed several clipped elements, and the sceAgc backend logged ~2465
 * scissor changes a frame until the 2MB command buffer filled. Commands past
 * that point are simply not submitted, so the tail of the document - the
 * bundle's status strip, and then EVO's own navigation rail composited after
 * it - vanished, differently on each frame. It read as flashing.
 *
 * A bundle cannot defend against this: it does not know the command buffer
 * exists, and `data-for` has no limit clause. So the host hands the document a
 * window onto the catalog and grows it as focus approaches the end. RmlUi's
 * for-view only instances the elements that are NEW when an array grows
 * (DataViewFor::Update), so extending is cheap and does not disturb focus.
 *
 * `count` stays the size of the whole level, because that is what a header
 * saying "184 GROUPS" means.
 */
constexpr size_t EVO_PROVIDER_ROW_WINDOW = 24;
/* Grow once focus is within this many rows of the end of the window. */
constexpr size_t EVO_PROVIDER_ROW_WINDOW_MARGIN = 8;
/* Step size when expanding window. */
constexpr size_t EVO_PROVIDER_ROW_WINDOW_STEP = 16;
/*
 * The hard cap on LIVE rows, and the reason the window slides instead of
 * growing without bound.
 *
 * Growing is cheap, as above. What is not cheap is the steady state: the
 * per-frame layout and the command buffer both scale with the number of live
 * elements, and at 19 elements per card a 184-group level reached 524288 DCB
 * dwords - exactly the slot capacity. Everything past that point was dropped
 * by a write callback that returns 0 and says nothing, which is the "flashing"
 * described above, now arriving through a different door. Measured on that
 * level: ~58 fps at 24 rows, and the whole screen thrashing between 1868 and
 * 524288 dwords a frame by row 104.
 *
 * So the window grows to this cap and then slides to follow focus. Sliding is
 * as cheap as growing, because DataViewFor binds element i to rows[i] by INDEX
 * and does not re-instance an element when the value at its index changes - it
 * only instances new tail elements and destroys surplus ones. The cost is that
 * focus stays on the ELEMENT rather than the row it was showing, so a slide has
 * to move focus back by the distance it slid. See SlideRowWindow.
 *
 * 32 and not 48, from hardware: with the cap at 48 the `agc health` line read
 * `peak=524288 dcb_full=205652` - the buffer completely full and a six-figure
 * count of rejected writes - while the same run at a window of 24-40 rows
 * reported `peak=86732 dcb_full=0`. So 48 is over the line and 40 was under it.
 * The relationship is not linear in rows, so this is the measured safe side
 * rather than a computed one; dcb_full in the log is what says whether it is
 * still safe, and it is there precisely because this was invisible before.
 */
constexpr size_t EVO_PROVIDER_ROW_WINDOW_MAX = 32;

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

    void Render(uint32_t* framebuffer, int width, int height);

    /*
     * D-pad and buttons. Returns true when the host consumed the key, false
     * when the caller should handle it - which is how Back gets out: at the
     * root of the provider's catalog Back is NOT consumed, and the rail takes
     * it. Anywhere deeper it pops a level and is consumed.
     */
    enum Key { KeyUp, KeyDown, KeyLeft, KeyRight, KeyAccept, KeyBack, KeySearch };
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
    /* Copy the first m_row_window rows of m_all_rows into the bound model. */
    void PublishRowWindow();
    /* Grow the window when focus nears its end, then slide it once it is at
     * EVO_PROVIDER_ROW_WINDOW_MAX. Called once per Tick(). */
    void ExtendRowWindow();
    /* Move the window to `new_offset` and put focus back on the row it was on.
     * `focused_local` is that row's index in the OUTGOING window. */
    void SlideRowWindow(size_t new_offset, int focused_local);
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
    std::vector<std::string> m_saved_usb_playlists;
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
};

int  evo_rmlui_provider_open(const char *provider_id, int width, int height);
void evo_rmlui_provider_close(void);
int  evo_rmlui_provider_is_open(void);
void evo_rmlui_provider_tick(void);
void evo_rmlui_provider_render(uint32_t *framebuffer, int width, int height);
int  evo_rmlui_provider_needs_frame(void);
void evo_rmlui_provider_clear_frame(void);

/* Returns 1 when consumed. `key` is an EvoRmlProviderHost::Key value. */
int  evo_rmlui_provider_key(int key);

/*
 * The item the user just activated, for PlaybackController. Empty title means
 * nothing is pending. Cleared by evo_rmlui_provider_take_selection().
 */
typedef struct evo_provider_selection {
    char provider_id[EVO_PROVIDER_MAX_ID];
    char item_id[EVO_PROVIDER_MAX_ITEM_ID];
    char title[EVO_PROVIDER_MAX_TITLE];
    int  is_live;
} evo_provider_selection_t;

int  evo_rmlui_provider_take_selection(evo_provider_selection_t *out);
int  evo_rmlui_provider_take_action(void);
void evo_rmlui_provider_set_loading(int loading, const char *status);
void evo_rmlui_provider_set_tuning(int tuning);
void evo_rmlui_provider_set_status(const char *status, int error);
void evo_rmlui_provider_search(const char *query);
const char* evo_rmlui_provider_get_query(void);
void evo_rmlui_provider_show_setup(void);

} /* extern "C" */
