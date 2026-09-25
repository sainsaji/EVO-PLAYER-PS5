#include "evo/screens/ProviderHostScreen.hpp"
#include "evo/Application.hpp"
#include "evo/screens/ScreenManager.hpp"
#include "evo/services/PlaybackController.hpp"

#include "evo_rmlui_provider.h"
#include "evo_rmlui_bridge.h"   /* evo_rmlui_render_nav_overlay */
#include "evo_feedback.h"
#include "evo_keyboard.h"
#include "evo_toast.h"
#include "evo_webui.h"      /* #101: web-UI providers open in the system browser */
#include "evo_nav.h"        /* EVO_SECTION_EMBY - the rail slot the chooser lights */

extern "C" {
#include "evo_provider.h"
#include "evo_boot_trace.h"
#include "evo_readdir.h"
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cctype>
#include <algorithm>
#include <atomic>
#include <pthread.h>

namespace evo {

namespace {

/* Set by the launch tile / rail before navigating here. Empty means "whichever
 * provider is enabled", which is what the single shared rail slot does. */
std::string g_pendingProvider;

/* #101: what the chooser lists - every provider that is set up, plus every one
 * that can be set up from it. With one entry the rail slot goes straight in;
 * with more it shows the chooser, which is also the only place a second
 * provider can get its source (hardware, 2026-09-26: with just IPTV set up the
 * slot went straight to IPTV and Emby could never be reached). */
bool is_pickable(const evo_provider_t* p)
{
    return p && (evo_provider_is_enabled(p->id) || (p->caps & EVO_PROVIDER_CAP_CONFIG));
}

int pickable_count(std::string* only)
{
    int n = 0;
    for (int i = 0; i < evo_provider_count(); ++i) {
        const evo_provider_t* p = evo_provider_at(i);
        if (!is_pickable(p)) continue;
        ++n;
        if (only) *only = p->id;
    }
    return n;
}

/*
 * The resolve callback's state.
 *
 * File-scope rather than a member because the callback is a C function pointer
 * with a void* and the screen outlives the process; carrying the screen pointer
 * would be an invitation to use it after a navigate. What it actually needs is
 * only the title and identity to build a PlaybackSource, all of which is copied
 * here at activation time.
 */
struct PendingPlay {
    char provider[EVO_PROVIDER_MAX_ID];
    char item_id[EVO_PROVIDER_MAX_ITEM_ID];
    char title[EVO_PROVIDER_MAX_TITLE];
    int  is_live;
};
PendingPlay g_pending;

/*
 * Set by on_resolved, cleared by update(). The callback must not reach back
 * into the screen - it can land on a frame after the user has navigated away -
 * so it reports completion through a flag the screen polls instead.
 */
bool g_resolve_finished = false;

static PlaybackSource g_start_src;
static pthread_t g_start_thread;
static std::atomic<bool> g_start_running{false};
static std::atomic<bool> g_start_done{false};
static std::atomic<bool> g_start_success{false};

static void* start_playback_worker(void* arg)
{
    (void)arg;
    auto pb = Application::getInstance().getPlaybackController();
    if (!pb) {
        g_start_success = false;
        g_start_done = true;
        return nullptr;
    }

    bool ok = pb->startPlaybackSource(g_start_src, 0.0);
    g_start_success = ok;
    g_start_done = true;
    return nullptr;
}

void on_resolved(int ok, const evo_stream_choice_t* choices, int count, void* ud)
{
    PendingPlay* pp = (PendingPlay*)ud;

    if (!ok || count <= 0 || !choices) {
        g_resolve_finished = true;
        evo_bt("provider: resolve failed for %s/%s", pp->provider, pp->item_id);
        toast("STREAM", "Failed to resolve channel stream");
        evo_rmlui_provider_set_tuning(0);
        evo_rmlui_provider_set_loading(0, "");
        evo_rmlui_provider_set_status("Failed to resolve stream for channel", 1);
        return;
    }

    /*
     * Best first, by contract. A quality picker over `choices` is a
     * per-provider story - the seam's job is to have produced more than one
     * and said which is preferred.
     */
    const evo_stream_choice_t& c = choices[0];

    PlaybackSource src;
    src.url      = c.url;
    src.title    = pp->title;
    src.provider = pp->provider;
    src.item_id  = pp->item_id;
    /* Either side may know it is live: the provider's catalog said so, or the
     * resolved choice did (an HLS playlist with no EXT-X-ENDLIST). */
    src.is_live  = (pp->is_live || c.is_live) ? true : false;

    g_start_src = src;
    g_start_done = false;
    g_start_success = false;
    g_start_running = true;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    int rc = pthread_create(&g_start_thread, &attr, start_playback_worker, nullptr);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        g_start_running = false;
        g_resolve_finished = true;
        evo_bt("provider: pthread_create failed for playback worker rc=%d", rc);
        evo_rmlui_provider_set_tuning(0);
        evo_rmlui_provider_set_loading(0, "");
        evo_rmlui_provider_set_status("Failed to start playback worker thread", 1);
        return;
    }
}

} // namespace

ProviderHostScreen::ProviderHostScreen()
    : StatefulScreen("ProviderHostScreen") {
}

void ProviderHostScreen::setPendingProvider(const std::string& id)
{
    g_pendingProvider = id;
}

void ProviderHostScreen::onEnter()
{
    StatefulScreen::onEnter();
    m_resolving = false;
    m_tunePending = false;
    m_tuneFrames = 0;
    evo_rmlui_provider_set_tuning(0);
    evo_rmlui_provider_set_loading(0, "");

    /* #101: back from a stream a web page handed over. The page reopens by
     * itself (evo_webui.c); this screen just keeps hosting it. */
    if (m_web && evo_webui_session_active()) {
        m_navigatingToPlayer = false;
        return;
    }

    std::string want = g_pendingProvider;
    if (want.empty()) {
        /* Returning from Player playback: keep the active session intact so
         * the user lands back in the exact folder and on the exact channel
         * card they left. */
        if (m_opened && m_navigatingToPlayer) {
            m_navigatingToPlayer = false;
            evo_bt("prov_screen: returning from playback, keeping provider '%s' open",
                   m_providerId.c_str());
            return;
        }
        /* One provider to offer: the rail slot means that provider, as it
         * always has. Several: the chooser. */
        std::string only;
        if (pickable_count(&only) != 1) {
            enterPicker();
            return;
        }
        want = only;
    }
    openProvider(want);
}

void ProviderHostScreen::openProvider(const std::string& want)
{
    m_picking = false;
    const evo_provider_t* wp = evo_provider_find(want.c_str());
    if (wp && (wp->caps & EVO_PROVIDER_CAP_WEBUI)) {
        if (m_opened) {
            evo_rmlui_provider_close();
            m_opened = false;
        }
        m_providerId = want;
        openWebProvider();
        return;
    }
    m_web = false;

    /* Returning from Player playback into the same provider: keep it. */
    if (m_opened && m_providerId == want) {
        m_navigatingToPlayer = false;
        evo_bt("prov_screen: returning from playback, keeping provider '%s' open", m_providerId.c_str());
        return;
    }

    /* If switching to a different provider, close previous host */
    if (m_opened) {
        evo_rmlui_provider_close();
        m_opened = false;
    }

    m_navigatingToPlayer = false;
    m_providerId = want;
    if (m_lastTypedUrl.empty()) {
        const evo_provider_t* p = evo_provider_find(want.c_str());
        if (p && p->get_source) {
            const char* s = p->get_source();
            if (s && (std::strncmp(s, "http://", 7) == 0 || std::strncmp(s, "https://", 8) == 0)) {
                m_lastTypedUrl = s;
            }
        }
    }
    evo_bt("prov_screen: onEnter calling evo_rmlui_provider_open('%s')", m_providerId.c_str());
    m_opened = evo_rmlui_provider_open(m_providerId.c_str(),
                                        DisplayWidth, DisplayHeight) != 0;
    evo_bt("prov_screen: onEnter evo_rmlui_provider_open returned opened=%d", m_opened ? 1 : 0);
    if (!m_opened) {
        evo_bt("provider: could not open host for '%s'", m_providerId.c_str());
        toast("PROVIDERS", "That provider is not available");
        if (auto sm = Application::getInstance().getScreenManager())
            sm->navigateTo(ScreenId::MainMenu);
    }
}

/* ------------------------------------------------------------------------- */
/* #101: web-UI providers                                                    */
/* ------------------------------------------------------------------------- */

void ProviderHostScreen::openWebProvider()
{
    const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
    if (!p || !p->web_ui_url) return;
    const char* url = p->web_ui_url();
    if (!url || !url[0]) {
        /* Not set up yet: ask for the address, and open once it is in. The
         * chooser sits behind the prompt, so cancelling it is not a blank
         * screen with no way out. */
        std::string id = m_providerId;
        enterPicker();
        m_providerId = id;
        openSourceEditor();
        return;
    }
    int rc = evo_webui_open(url, "/web/index.html");
    evo_bt("prov_screen: web UI '%s' -> %s rc=%d", p->id, url, rc);
    if (rc < 0) {
        toast(p->name, "Set the server as http(s)://<host>:<port>");
        enterPicker();
        return;
    }
    m_web = true;
    m_webSeen = false;
}

/* ------------------------------------------------------------------------- */
/* #101: the chooser                                                         */
/* ------------------------------------------------------------------------- */

void ProviderHostScreen::enterPicker()
{
    if (m_opened) {
        evo_rmlui_provider_close();
        m_opened = false;
    }
    m_web = false;
    m_webSeen = false;
    m_pickIds.clear();
    m_pickDetail.clear();

    /* Every provider that is set up, plus every one that can be set up from
     * here - so the chooser is also where a new provider gets its source. */
    for (int i = 0; i < evo_provider_count(); ++i) {
        const evo_provider_t* p = evo_provider_at(i);
        if (!is_pickable(p)) continue;

        std::string detail;
        if (!p->is_configured()) {
            detail = (p->caps & EVO_PROVIDER_CAP_WEBUI)
                   ? "Not set up - press X to enter the server address"
                   : "Not set up - press X to add a playlist";
        } else if (p->caps & EVO_PROVIDER_CAP_WEBUI) {
            /* An address carries no secret; a playlist URL can (Xtream user and
             * password), so only the web kind shows its source. */
            const char* src = p->get_source ? p->get_source() : "";
            detail = std::string("Web UI - ") + (src ? src : "");
        } else {
            detail = "Ready";
        }
        m_pickIds.push_back(p->id);
        m_pickDetail.push_back(detail);
    }

    if (m_pickIds.empty()) {
        /* No provider can even be configured. Not an error - it is the state a
         * build with every provider compiled out is in. */
        toast("PROVIDERS", "No provider is set up yet");
        if (auto sm = Application::getInstance().getScreenManager())
            sm->navigateTo(ScreenId::Settings);
        return;
    }
    if (m_pickIndex < 0 || m_pickIndex >= (int)m_pickIds.size())
        m_pickIndex = 0;
    m_picking = true;
}

void ProviderHostScreen::choosePicked(bool editSource)
{
    if (m_pickIndex < 0 || m_pickIndex >= (int)m_pickIds.size()) return;
    const std::string id = m_pickIds[m_pickIndex];
    const evo_provider_t* p = evo_provider_find(id.c_str());
    if (!p) return;

    m_providerId = id;
    bool configured = p->is_configured() != 0;
    if (editSource || !configured) {
        if (!(p->caps & EVO_PROVIDER_CAP_CONFIG)) {
            toast(p->name, "Nothing to set up here");
            return;
        }
        evo_feedback(EVO_FB_OPEN);
        if (p->caps & EVO_PROVIDER_CAP_WEBUI) {
            openSourceEditor();     /* the address; opens the web UI once set */
            return;
        }
        /* A bundle provider has its own setup page (IPTV: type a URL, or pick
         * an .m3u from USB) - open the provider and go straight to it rather
         * than to a bare keyboard. */
        openProvider(id);
        if (m_opened)
            evo_rmlui_provider_show_setup();
        return;
    }
    /* Configured but switched off (a flag saved before it had a source):
     * choosing it is the user asking for it. */
    if (!evo_provider_is_enabled(p->id))
        evo_provider_set_enabled(p->id, 1);
    evo_feedback(EVO_FB_CONFIRM);
    openProvider(id);
}

void ProviderHostScreen::renderPicker(uint32_t* framebuffer, int width, int height)
{
    evo_rmlui_list_params_t params;
    std::memset(&params, 0, sizeof(params));

    bool railFocused = false;
    if (auto sm = Application::getInstance().getScreenManager())
        railFocused = sm->isRailFocused();

    params.section = EVO_SECTION_EMBY;
    params.rail_focused = railFocused ? 1 : 0;

    if (m_web) {
        /* Behind the browser, which covers everything right of the rail. */
        const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
        params.title = p ? p->name : "PROVIDER";
        params.subtitle = "Opening the web UI";
        params.is_empty = 1;
        params.empty_title = "Opening...";
        params.empty_hint = "The page opens beside the menu. Close it to come back here.";
        params.empty_icon = "../icons/icon_emby.png";
        evo_rmlui_update_list(&params);
        evo_rmlui_render_list(framebuffer, width, height);
        return;
    }

    params.title = "PROVIDERS";
    params.subtitle = "Choose where to watch from";
    int total = (int)m_pickIds.size();
    params.total_count = total;
    params.cursor_index = total ? m_pickIndex : -1;
    int rows = std::min(EVO_RMLUI_LIST_ROWS, total);
    params.row_count = rows;
    for (int i = 0; i < rows; ++i) {
        const evo_provider_t* p = evo_provider_find(m_pickIds[i].c_str());
        bool web = p && (p->caps & EVO_PROVIDER_CAP_WEBUI);
        params.rows[i].title = p ? p->name : m_pickIds[i].c_str();
        params.rows[i].detail = m_pickDetail[i].c_str();
        params.rows[i].icon_path = web ? "../icons/icon_emby.png" : "../icons/icon_folder.png";
        params.rows[i].badge = web ? "WEB"
                             : (p && (p->caps & EVO_PROVIDER_CAP_LIVE)) ? "LIVE" : "";
        params.rows[i].progress = -1;
        params.rows[i].has_chevron = 1;
        params.rows[i].is_focused = (!railFocused && i == m_pickIndex);
    }

    const evo_provider_t* fp = (m_pickIndex >= 0 && m_pickIndex < total)
                             ? evo_provider_find(m_pickIds[m_pickIndex].c_str()) : nullptr;
    params.hint_count = 3;
    params.hints[0].glyph_path = "../icons/btn_cross.png";
    params.hints[0].label = "OPEN";
    params.hints[1].glyph_path = "../icons/btn_square.png";
    params.hints[1].label = (fp && (fp->caps & EVO_PROVIDER_CAP_WEBUI)) ? "EDIT ADDRESS"
                                                                         : "EDIT PLAYLIST";
    params.hints[2].glyph_path = "../icons/btn_circle.png";
    params.hints[2].label = "BACK";

    evo_rmlui_update_list(&params);
    evo_rmlui_render_list(framebuffer, width, height);
}

void ProviderHostScreen::onExit()
{
    if (g_start_running) {
        pthread_join(g_start_thread, nullptr);
        g_start_running = false;
    }
    m_tunePending = false;
    m_tuneFrames = 0;
    evo_rmlui_provider_set_tuning(0);
    evo_rmlui_provider_set_loading(0, "");
    if (m_opened && !m_navigatingToPlayer) {
        evo_rmlui_provider_close();
        m_opened = false;
    }
    m_resolving = false;
    m_picking = false;
    if (m_web && !evo_webui_session_active()) {
        m_web = false;
        m_webSeen = false;
    }
    StatefulScreen::onExit();
}

bool ProviderHostScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released)
{
    (void)held;
    (void)released;

    if (m_picking) {
        auto psm = Application::getInstance().getScreenManager();
        if (psm && psm->isRailFocused()) return false;
        int n = (int)m_pickIds.size();
        if (pressed & PadButtons::Up) {
            if (m_pickIndex > 0) { --m_pickIndex; evo_feedback(EVO_FB_MOVE); }
            else evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        if (pressed & PadButtons::Down) {
            if (m_pickIndex + 1 < n) { ++m_pickIndex; evo_feedback(EVO_FB_MOVE); }
            else evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        if (pressed & PadButtons::Left) {
            if (psm) { psm->setRailFocused(true); return true; }
            return false;
        }
        if (pressed & PadButtons::Cross)   { choosePicked(false); return true; }
        if (pressed & PadButtons::Square)  { choosePicked(true);  return true; }
        if (pressed & PadButtons::Circle) {
            evo_feedback(EVO_FB_CANCEL);
            if (psm) psm->navigateTo(ScreenId::MainMenu);
            return true;
        }
        return false;
    }
    if (!m_opened) return false;

    auto sm = Application::getInstance().getScreenManager();
    bool railFocused = sm && sm->isRailFocused();

    /* While the rail has focus the provider document must not move: the rail
     * owns Up/Down and Left/Right in that state, exactly as on every other
     * screen. */
    if (railFocused) return false;

    /*
     * Everything below goes into RmlUi. EVO does not translate a direction into
     * a row index anywhere in this function - that is the difference between
     * this screen and every other one in the tree.
     */
    if (pressed & PadButtons::Up)
        return evo_rmlui_provider_key(EvoRmlProviderHost::KeyUp) != 0;
    if (pressed & PadButtons::Down)
        return evo_rmlui_provider_key(EvoRmlProviderHost::KeyDown) != 0;
    if (pressed & PadButtons::Right)
        return evo_rmlui_provider_key(EvoRmlProviderHost::KeyRight) != 0;

    if (pressed & PadButtons::Left) {
        /* Let the document have it first - a grid's leftmost column is where
         * the rail should take over, and only the document knows where that
         * is. An unconsumed Left focuses the rail. */
        if (evo_rmlui_provider_key(EvoRmlProviderHost::KeyLeft) != 0)
            return true;
        if (sm) { sm->setRailFocused(true); return true; }
        return false;
    }

    if (pressed & PadButtons::Cross) {
        if (m_resolving || m_tunePending || g_start_running) return true;      /* one activation at a time */
        evo_feedback(EVO_FB_OPEN);
        return evo_rmlui_provider_key(EvoRmlProviderHost::KeyAccept) != 0;
    }

    if (pressed & PadButtons::Circle) {
        if (m_resolving || m_tunePending || g_start_running) return true;
        evo_feedback(EVO_FB_CANCEL);
        /*
         * The exit contract. The host consumes Back while it is deeper than the
         * provider's root and pops a level; at the root it declines, and the
         * screen leaves. A provider that swallowed Back at its root would be a
         * screen the user cannot get out of, and the only remaining way out is
         * the PS button - the close path that panicked the console.
         */
        if (evo_rmlui_provider_key(EvoRmlProviderHost::KeyBack) != 0)
            return true;
        if (sm) sm->navigateTo(ScreenId::MainMenu);
        return true;
    }

    if (pressed & PadButtons::Square) {
        openSearch();
        return true;
    }

    /*
     * OPTIONS, not Triangle.
     *
     * Triangle is the virtual keyboard's own "Quick Done" (evo_keyboard.c),
     * so opening the editor with it submitted the prompt on the spot: the
     * hardware log showed seven "source set to <unchanged url>" lines from
     * seven presses, each one closing and reopening the host - which is what
     * the flashing was. Options is free on this screen and is where a console
     * user looks for settings anyway.
     */
    if (pressed & PadButtons::Options) {
        evo_feedback(EVO_FB_OPEN);
        evo_rmlui_provider_show_setup();
        return true;
    }

    return false;
}

/* ------------------------------------------------------------------------- */
/* Search                                                                    */
/* ------------------------------------------------------------------------- */

void ProviderHostScreen::openSearch()
{
    const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
    if (!p || !(p->caps & EVO_PROVIDER_CAP_SEARCH)) {
        toast("SEARCH", "This provider does not support search");
        return;
    }

    evo_feedback(EVO_FB_OPEN);
    evo_keyboard_open("Search channels...", m_searchQuery.c_str(), 64,
                      &ProviderHostScreen::OnSearchSubmitted, this);
}

void ProviderHostScreen::OnSearchSubmitted(const char* text, void* userdata)
{
    auto* self = static_cast<ProviderHostScreen*>(userdata);
    if (!self) return;

    std::string value = text ? text : "";
    size_t b = value.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        value.clear();
    } else {
        size_t e = value.find_last_not_of(" \t\r\n");
        value = value.substr(b, e - b + 1);
    }

    self->m_searchQuery = value;
    evo_rmlui_provider_search(value.c_str());
}

/* ------------------------------------------------------------------------- */
/* Source editing - the typed M3U URL                                        */
/* ------------------------------------------------------------------------- */

void ProviderHostScreen::OnSourceSubmitted(const char* text, void* userdata)
{
    auto* self = static_cast<ProviderHostScreen*>(userdata);
    if (!self) return;

    const evo_provider_t* p = evo_provider_find(self->m_providerId.c_str());
    if (!p || !(p->caps & EVO_PROVIDER_CAP_CONFIG) || !p->set_source) return;

    /* Trim - a keyboard picks up trailing spaces very easily, and a URL with
     * one on the end fails in a way that looks like the URL being wrong. */
    std::string value = text ? text : "";
    size_t b = value.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        value.clear();
    } else {
        size_t e = value.find_last_not_of(" \t\r\n");
        value = value.substr(b, e - b + 1);
    }

    bool web = (p->caps & EVO_PROVIDER_CAP_WEBUI) != 0;
    if (p->set_source(value.c_str()) != 0) {
        toast(p->name, web ? "Use http(s)://<host>:<port>"
                           : "That does not look like an M3U URL");
        return;
    }

    /* Persisted by the provider. Enable it now that it has a source. */
    evo_provider_set_enabled(p->id, p->is_configured() ? 1 : 0);
    self->m_picking = false;
    if (web) {
        /* #101: a web-UI provider opens straight away on its new address. */
        toast(p->name, "Server saved");
        self->openWebProvider();
        return;
    }

    if (value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0) {
        self->m_lastTypedUrl = value;
    }

    /* Reopen the host so the catalog is re-fetched from the new source. */
    toast("PROVIDERS", value.empty() ? "Playlist cleared" : "Playlist updated");

    std::string id = self->m_providerId;
    if (self->m_opened) {
        evo_rmlui_provider_close();
        self->m_opened = false;
    }
    self->m_opened = evo_rmlui_provider_open(id.c_str(),
                                             DisplayWidth, DisplayHeight) != 0;
}

void ProviderHostScreen::openSourceEditor()
{
    const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
    if (!p || !(p->caps & EVO_PROVIDER_CAP_CONFIG) || !p->get_source) {
        toast("PROVIDERS", "This provider has no playlist to set");
        return;
    }

    const char* current = p->get_source();
    std::string initial;
    if (current && (std::strncmp(current, "http://", 7) == 0 || std::strncmp(current, "https://", 8) == 0)) {
        initial = current;
    } else if (p->caps & EVO_PROVIDER_CAP_WEBUI) {
        initial = "http://";
    } else if (!m_lastTypedUrl.empty()) {
        initial = m_lastTypedUrl;
    }

    char title[96];
    if (p->caps & EVO_PROVIDER_CAP_WEBUI)
        std::snprintf(title, sizeof title, "%s server (http(s)://host:port)", p->name);
    else
        std::snprintf(title, sizeof title, "%s playlist URL (clear to reset)", p->name);

    evo_feedback(EVO_FB_OPEN);
    /* EVO_PROVIDER_MAX_URL is 2048, but a keyboard field that long is not
     * usable and no real M3U link needs it; 512 covers an Xtream get.php URL
     * with credentials and leaves the field navigable. */
    evo_keyboard_open(title, initial.c_str(), 512,
                      &ProviderHostScreen::OnSourceSubmitted, this);
}

std::vector<std::string> ProviderHostScreen::scanUsbPlaylists()
{
    std::vector<std::string> results;
    static const char* kRoots[] = { "/mnt/usb0", "/mnt/usb1", nullptr };

    auto isM3u = [](const std::string& name) -> bool {
        if (name.size() < 4) return false;
        std::string ext;
        size_t dot = name.find_last_of('.');
        if (dot == std::string::npos) return false;
        for (size_t i = dot; i < name.size(); ++i)
            ext += (char)std::tolower((unsigned char)name[i]);
        return (ext == ".m3u" || ext == ".m3u8");
    };

    for (int r = 0; kRoots[r]; ++r) {
        const char* root = kRoots[r];
        evo_dir_t* d = evo_opendir(root);
        if (!d) continue;

        struct dirent* entry;
        std::vector<std::string> subdirs;
        while ((entry = evo_readdir(d)) != nullptr) {
            if (entry->d_name[0] == '.') continue;
            if (std::strcmp(entry->d_name, "$RECYCLE.BIN") == 0) continue;
            if (std::strcmp(entry->d_name, "System Volume Information") == 0) continue;

            std::string name = entry->d_name;
            std::string fullPath = std::string(root) + "/" + name;

            if (entry->d_type == 4 /* DT_DIR */) {
                subdirs.push_back(fullPath);
            } else if (isM3u(name)) {
                results.push_back(fullPath);
            }
        }
        evo_closedir(d);

        for (const auto& sdir : subdirs) {
            evo_dir_t* sd = evo_opendir(sdir.c_str());
            if (!sd) continue;
            while ((entry = evo_readdir(sd)) != nullptr) {
                if (entry->d_name[0] == '.') continue;
                if (isM3u(entry->d_name)) {
                    results.push_back(sdir + "/" + entry->d_name);
                }
            }
            evo_closedir(sd);
        }
    }

#ifndef __PS5__
    /* On PC/host dev environment, also check current directory or sample files if /mnt/usb0 is absent */
    if (results.empty()) {
        static const char* kHostRoots[] = { ".", "assets", nullptr };
        for (int r = 0; kHostRoots[r]; ++r) {
            evo_dir_t* d = evo_opendir(kHostRoots[r]);
            if (!d) continue;
            struct dirent* entry;
            while ((entry = evo_readdir(d)) != nullptr) {
                if (entry->d_name[0] == '.') continue;
                if (isM3u(entry->d_name)) {
                    results.push_back(std::string(kHostRoots[r]) + "/" + entry->d_name);
                }
            }
            evo_closedir(d);
        }
    }
#endif

    return results;
}

void ProviderHostScreen::browseUsb()
{
    const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
    if (!p || !(p->caps & EVO_PROVIDER_CAP_CONFIG) || !p->set_source) {
        toast("PROVIDERS", "This provider has no playlist to set");
        return;
    }

    evo_feedback(EVO_FB_OPEN);
    std::vector<std::string> playlists = scanUsbPlaylists();

    if (playlists.empty()) {
        toast("USB SCAN", "No M3U files found on USB");
        evo_rmlui_provider_set_status("No .m3u or .m3u8 files found on USB drive (/mnt/usb0, /mnt/usb1). Insert a USB drive with an M3U playlist.", 1);
        return;
    }

    if (playlists.size() == 1) {
        const std::string& path = playlists[0];
        if (p->set_source(path.c_str()) != 0) {
            toast("PROVIDERS", "Failed to load playlist from USB");
            return;
        }
        evo_provider_set_enabled(p->id, 1);
        size_t last_slash = path.find_last_of("/\\");
        std::string fname = (last_slash != std::string::npos) ? path.substr(last_slash + 1) : path;
        toast("IPTV USB", ("Loaded " + fname).c_str());

        /* Reopen provider so catalog is reloaded from USB */
        std::string id = m_providerId;
        if (m_opened) {
            evo_rmlui_provider_close();
            m_opened = false;
        }
        m_opened = evo_rmlui_provider_open(id.c_str(), DisplayWidth, DisplayHeight) != 0;
        return;
    }

    /* Multiple playlists found: present them as cards in the grid */
    EvoRmlProviderHost::Instance().ShowUsbPlaylists(playlists);
}

void ProviderHostScreen::startSelected()
{
    if (m_tunePending) {
        m_tuneFrames++;
        if (m_tuneFrames >= 10) {
            m_tunePending = false;
            m_resolving = true;
            if (evo_provider_resolve_chain(g_pending.provider, g_pending.item_id,
                                            on_resolved, &g_pending) != 0) {
                m_resolving = false;
                toast("STREAM", "That item could not be played");
                evo_rmlui_provider_set_tuning(0);
                evo_rmlui_provider_set_loading(0, "");
                evo_rmlui_provider_set_status("Failed to resolve stream for channel", 1);
            }
        }
        return;
    }

    evo_provider_selection_t sel;
    if (!evo_rmlui_provider_take_selection(&sel)) return;
    if (m_resolving || g_start_running) return;

    std::memset(&g_pending, 0, sizeof g_pending);
    std::snprintf(g_pending.provider, sizeof g_pending.provider, "%s", sel.provider_id);
    std::snprintf(g_pending.item_id,  sizeof g_pending.item_id,  "%s", sel.item_id);
    std::snprintf(g_pending.title,    sizeof g_pending.title,    "%s", sel.title);
    g_pending.is_live = sel.is_live;
    g_resolve_finished = false;

    m_tunePending = true;
    m_tuneFrames = 0;

    char msg[128];
    std::snprintf(msg, sizeof(msg), "Tuning %s...", sel.title);
    toast("LIVE TV", msg);
    evo_rmlui_provider_set_tuning(1);
    evo_rmlui_provider_set_loading(1, msg);
}

void ProviderHostScreen::update(double deltaMs)
{
    StatefulScreen::update(deltaMs);

    /* #101: the web UI's session ended when the user closed the browser (a
     * handed-over stream keeps it alive). Back to the chooser, or out when
     * this is the only provider. */
    if (m_web) {
        if (evo_webui_session_active()) {
            m_webSeen = true;
        } else if (m_webSeen) {
            m_web = false;
            m_webSeen = false;
            if (pickable_count(nullptr) > 1) {
                enterPicker();
            } else if (auto sm = Application::getInstance().getScreenManager()) {
                sm->navigateTo(ScreenId::MainMenu);
            }
        }
        return;
    }
    if (!m_opened) return;

    /* Bundle refresh, artwork decode, and any catalog reply that landed. */
    evo_rmlui_provider_tick();

    int action = evo_rmlui_provider_take_action();
    if (action == EVO_PROVIDER_ACTION_SETUP_URL) {
        openSourceEditor();
    } else if (action == EVO_PROVIDER_ACTION_SETUP_USB) {
        browseUsb();
    }

    if (g_start_running && g_start_done) {
        pthread_join(g_start_thread, nullptr);
        g_start_running = false;
        m_resolving = false;
        g_resolve_finished = false;

        if (g_start_success) {
            evo_rmlui_provider_set_tuning(0);
            evo_rmlui_provider_set_loading(0, "");
            setNavigatingToPlayer(true);
            if (auto sm = Application::getInstance().getScreenManager()) {
                sm->navigateTo(ScreenId::Player);
            }
        } else {
            evo_rmlui_provider_set_tuning(0);
            evo_rmlui_provider_set_loading(0, "");
            evo_rmlui_provider_set_status("Stream unavailable or connection timed out", 1);
        }
    }

    /*
     * Take the activated item here rather than inside the Rml event handler
     * that produced it. Starting playback tears down and rebuilds the VideoOut
     * configuration, and doing that from underneath Context::Update() - which
     * is still walking the element tree that raised the event - is the shape of
     * bug that cost two console cycles in #32.
     */
    startSelected();

    /* A resolve that finished - either way - releases the guard, so a failed
     * one does not leave the screen permanently unable to activate anything. */
    if (m_resolving && g_resolve_finished) {
        m_resolving = false;
        g_resolve_finished = false;
    }
}

void ProviderHostScreen::render(uint32_t* framebuffer, int width, int height)
{
    if (m_picking || m_web) {
        renderPicker(framebuffer, width, height);
        return;
    }
    if (!m_opened) return;
    evo_rmlui_provider_render(framebuffer, width, height);
    /* The rail is a document in the MAIN context, so it does not come with the
     * provider's own context - it is composited on top afterwards. */
    evo_rmlui_render_nav_overlay(framebuffer, width, height);
    evo_rmlui_provider_clear_frame();
}

} // namespace evo
