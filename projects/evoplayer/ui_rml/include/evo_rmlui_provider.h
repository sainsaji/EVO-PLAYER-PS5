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
     * device loop only redraws on change (GlNeedsFrame). */
    bool NeedsFrame() const { return m_dirty; }
    void ClearFrameFlag() { m_dirty = false; }

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
    void PushArtRequests();
    void ApplyPendingActivation();
    void SetStatus(const std::string& msg, bool error);
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

} /* extern "C" */
