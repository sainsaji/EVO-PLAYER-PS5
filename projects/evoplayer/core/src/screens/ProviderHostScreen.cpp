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
#include "evo_favorites.h"
#include "evo_stream_io.h"                          /* evo_stream_headers */
#include "evo_hls_variants.h"                    /* the stream picker's quality list */
#include "evo_net.h"                               /* evo_net_discover_* */
#include "evo/interfaces/ISettingsService.hpp"
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cctype>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
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
/* The stream picker shows its rows in the generic list document, which holds
 * nine. */
constexpr int kMaxChoices = EVO_RMLUI_LIST_ROWS;

/* Where a row of the picker came from. */
enum ChoiceKind {
    KindListed = 0,     /* the URL the playlist gave                        */
    KindVariant,        /* a quality variant read from its HLS master       */
    KindGuess,          /* an alternative the provider derived (.ts <-> .m3u8) */
    KindSource          /* one of several real sources (a Stremio addon's list) */
};

struct PendingPlay {
    char provider[EVO_PROVIDER_MAX_ID];
    char item_id[EVO_PROVIDER_MAX_ITEM_ID];
    char title[EVO_PROVIDER_MAX_TITLE];
    int  is_live;
    long long resume_sec;   /* the service's resume point, 0 = from the start */
    evo_stream_choice_t choices[kMaxChoices];
    int  kind[kMaxChoices];
    int  choice_count;
    int  active_choice;
    int  ask;           /* the picker is on for this activation              */
    int  picked;        /* the user chose this stream: no automatic fallback */
};
PendingPlay g_pending;

/*
 * Where the picker is in its life. All of it runs on the main thread - the
 * resolver and the variants fetch both deliver from evo_net_poll() - so plain
 * globals are enough; the generation only exists so a reply that lands after the
 * user has backed out is recognised as stale and dropped.
 */
enum class PickStage { Idle, Ready };
PickStage g_pick_stage = PickStage::Idle;
unsigned  g_pick_gen = 0;

/*
 * The quality variants are read while the picker is already on screen: it opens at
 * once with the rows it knows (the URL as listed, and any guess) and the variants
 * are inserted when the read lands. Waiting for the read before showing anything
 * cost the user 8 s of nothing on a host whose first address was dead, and then
 * showed no qualities at all.
 */
enum class VariantsState { None, Reading, Done, Failed };
VariantsState g_variants = VariantsState::None;
int       g_variants_added = 0;         /* rows inserted, waiting to be reflected in the cursor */
bool      g_variants_merged = false;
std::chrono::steady_clock::time_point g_variants_started;
constexpr int kVariantsGiveUpMs = 15000; /* stop saying "reading" after this */

/*
 * Set by on_resolved, cleared by update(). The callback must not reach back
 * into the screen - it can land on a frame after the user has navigated away -
 * so it reports completion through a flag the screen polls instead.
 */
bool g_resolve_finished = false;

static PlaybackSource g_start_src;
static double g_start_resume = 0.0;
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

    bool ok = pb->startPlaybackSource(g_start_src, g_start_resume);
    g_start_success = ok;
    g_start_done = true;
    return nullptr;
}

/* Starts `idx` of the pending list on the playback worker. 0 or a pthread error. */
int launch_choice(int idx)
{
    g_pending.active_choice = idx;
    const evo_stream_choice_t& c = g_pending.choices[idx];
    if (const evo_provider_t* p = evo_provider_find(g_pending.provider))
        if (p->stream_chosen) p->stream_chosen(g_pending.item_id, &c);

    PlaybackSource src;
    src.url      = c.url;
    src.title    = g_pending.title;
    src.provider = g_pending.provider;
    src.item_id  = g_pending.item_id;
    /* Either side may know it is live: the provider's catalog said so, or the
     * resolved choice did (an HLS playlist with no EXT-X-ENDLIST). */
    src.is_live  = (g_pending.is_live || c.is_live) ? true : false;

    /* Read by evo_stream_io_open() on the worker started below. */
    snprintf(evo_stream_headers, sizeof evo_stream_headers, "%s", c.headers);
    evo_stream_user_agent[0] = '\0';

    g_start_src = src;
    g_start_resume = src.is_live ? 0.0 : (double)g_pending.resume_sec;
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
    }
    return rc;
}

bool choice_is_hls(const evo_stream_choice_t& c)
{
    if (!std::strcmp(c.container, "hls")) return true;
    std::string u(c.url);
    std::transform(u.begin(), u.end(), u.begin(), [](unsigned char ch) { return std::tolower(ch); });
    return u.find(".m3u8") != std::string::npos;
}

/*
 * What a source row says about its file, when the provider knows anything:
 * "4K HEVC  -  Dolby Vision  -  24.5 GB  -  45.0 Mbps  -  TRUEHD 7.1" (#116).
 * "" when it knows nothing, and the row falls back to "Source N of M".
 */
std::string source_detail(const evo_stream_choice_t& c)
{
    std::string d;
    auto add = [&d](const std::string& s) {
        if (s.empty()) return;
        if (!d.empty()) d += "  -  ";
        d += s;
    };
    auto upper = [](const char* s) {
        std::string u(s);
        std::transform(u.begin(), u.end(), u.begin(), [](unsigned char ch) { return std::toupper(ch); });
        return u;
    };
    char buf[48];
    std::string video;
    if (c.width >= 3200 || c.height >= 1800)      video = "4K";
    else if (c.width >= 1800 || c.height >= 1000) video = "1080p";
    else if (c.width >= 1200 || c.height >= 700)  video = "720p";
    else if (c.height > 0) { std::snprintf(buf, sizeof buf, "%dp", c.height); video = buf; }
    if (c.video_codec[0]) video += (video.empty() ? "" : " ") + upper(c.video_codec);
    add(video);
    add(c.video_range);
    if (c.size_bytes > 0) {
        const double gb = (double)c.size_bytes / (1024.0 * 1024.0 * 1024.0);
        if (gb >= 1.0) std::snprintf(buf, sizeof buf, "%.1f GB", gb);
        else           std::snprintf(buf, sizeof buf, "%.0f MB", gb * 1024.0);
        add(buf);
    }
    if (c.bitrate_bps > 0) {
        std::snprintf(buf, sizeof buf, "%.1f Mbps", (double)c.bitrate_bps / 1e6);
        add(buf);
    }
    std::string audio = upper(c.audio_codec);
    if (c.audio_channels > 0) {
        const int ch = c.audio_channels;
        if (ch == 1)      std::snprintf(buf, sizeof buf, "Mono");
        else if (ch == 2) std::snprintf(buf, sizeof buf, "Stereo");
        else              std::snprintf(buf, sizeof buf, "%d.1", ch - 1);
        audio += (audio.empty() ? "" : " ") + std::string(buf);
    }
    add(audio);
    return d;
}

/*
 * The master playlist's variants have come back (or the read failed: count 0).
 * The list becomes: the URL as listed, then the variants best first, then the
 * provider's own alternatives, which are guesses.
 */
void on_variants(int count, const evo_stream_choice_t* variants, void* ud)
{
    if ((unsigned)(uintptr_t)ud != g_pick_gen || g_variants != VariantsState::Reading)
        return;                                       /* the user has moved on */

    PendingPlay& pp = g_pending;
    const int before = pp.choice_count;
    evo_stream_choice_t listed = pp.choices[0];
    evo_stream_choice_t guesses[kMaxChoices];
    int ng = 0;
    for (int i = 1; i < pp.choice_count && ng < kMaxChoices; ++i)
        guesses[ng++] = pp.choices[i];

    int n = 0;
    pp.choices[n] = listed;  pp.kind[n++] = KindListed;
    for (int i = 0; i < count && n < kMaxChoices; ++i) {
        pp.choices[n] = variants[i];
        pp.kind[n++] = KindVariant;
    }
    for (int i = 0; i < ng && n < kMaxChoices; ++i) {
        pp.choices[n] = guesses[i];
        pp.kind[n++] = KindGuess;
    }
    pp.choice_count = n;
    evo_bt("provider: stream picker: %d variant(s) read for %s, %d choice(s) in all",
           count, pp.title, n);

    /* update() reflects it on screen: the picker is already open, and the rows
     * after the listed one have just moved down. */
    g_variants = (count > 0) ? VariantsState::Done : VariantsState::Failed;
    g_variants_added = n - before;
    g_variants_merged = true;
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
     * Best first, by contract. Anything after the first is an alternative the
     * provider derived: with the picker off it is only ever tried automatically
     * if the first fails to open; with it on it is listed, and marked a guess.
     */
    pp->choice_count = std::min(count, kMaxChoices);
    const evo_provider_t* rp = evo_provider_find(pp->provider);
    const bool sources = rp && (rp->caps & EVO_PROVIDER_CAP_PICK);
    for (int i = 0; i < pp->choice_count; ++i) {
        pp->choices[i] = choices[i];
        pp->kind[i] = sources ? KindSource : (i == 0) ? KindListed : KindGuess;
    }
    pp->active_choice = 0;

    if (pp->ask && sources) {
        /* Every row is a real source the addon listed: no guesses, and no HLS
         * quality read - its rows would be inserted as if after a playlist URL. */
        g_variants = VariantsState::None;
        if (pp->choice_count > 1) {
            g_pick_stage = PickStage::Ready;
            return;
        }
    } else if (pp->ask) {
        /* An HLS master lists the same channel at several qualities. The list
         * opens now with what is known and the qualities are added when the read
         * lands; if the read cannot even be queued the list has fewer rows. */
        g_variants = VariantsState::None;
        if (choice_is_hls(pp->choices[0])) {
            g_variants_started = std::chrono::steady_clock::now();
            g_variants = (evo_hls_variants_fetch(pp->choices[0].url, on_variants,
                                                 (void*)(uintptr_t)g_pick_gen) == 0)
                       ? VariantsState::Reading : VariantsState::Failed;
        }
        if (pp->choice_count > 1 || g_variants == VariantsState::Reading) {
            g_pick_stage = PickStage::Ready;              /* update() opens the picker */
            return;
        }
        /* One stream, nothing behind it, nothing being read: nothing to choose. */
    }

    launch_choice(0);
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
            /* A service that tracks progress has just been told where playback
             * stopped: re-read the level so the card shows it (same cursor). */
            const evo_provider_t* rp = evo_provider_find(m_providerId.c_str());
            if (rp && (rp->caps & EVO_PROVIDER_CAP_PROGRESS) && !(rp->caps & EVO_PROVIDER_CAP_LIVE))
                evo_rmlui_provider_reload();
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
    if (wp && (wp->caps & EVO_PROVIDER_CAP_WEBUI) && !isNativeWeb(wp)) {
        if (m_opened) {
            evo_rmlui_provider_close();
            m_opened = false;
        }
        m_providerId = want;
        openWebProvider();
        return;
    }
    m_web = false;

    /* A media server with no session yet: sign in first, then come back. */
    if (wp && wp->needs_sign_in && wp->needs_sign_in()) {
        if (m_opened) {
            evo_rmlui_provider_close();
            m_opened = false;
        }
        m_providerId = want;
        beginSignIn();
        return;
    }

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
    if (m_opened) {
        /* First run: nothing to browse yet, so land on the provider's setup
         * page (IPTV: type an M3U URL or pick one from USB). */
        const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
        if (p && p->is_configured && !p->is_configured() && (p->caps & EVO_PROVIDER_CAP_CONFIG))
            evo_rmlui_provider_show_setup();
    }
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
    /* A server behind a reverse proxy at a path ("https://host/jellyfin"):
     * the proxy upstream is scheme://host[:port], and the page lives under
     * the path. */
    std::string upstream = url, page = p->web_ui_path ? p->web_ui_path : "/";
    {
        size_t s = upstream.find("://");
        size_t slash = upstream.find('/', s == std::string::npos ? 0 : s + 3);
        if (slash != std::string::npos) {
            std::string path = upstream.substr(slash);
            while (!path.empty() && path.back() == '/') path.pop_back();
            upstream.resize(slash);
            page = path + page;
        }
    }
    int rc = evo_webui_open_ex(upstream.c_str(), page.c_str(), p->web_ui_hook);
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
                   : p->ui_embedded ? "No addons yet - press X to add one"
                   : "Not set up - press X to add a playlist";
        } else if (isNativeWeb(p)) {
            const char* src = p->get_source ? p->get_source() : "";
            detail = std::string(src ? src : "") +
                     ((p->needs_sign_in && p->needs_sign_in()) ? " - press X to sign in" : "");
        } else if (p->caps & EVO_PROVIDER_CAP_WEBUI) {
            /* An address carries no secret; a playlist URL can (Xtream user and
             * password), so only the web kind shows its source. */
            const char* src = p->get_source ? p->get_source() : "";
            detail = std::string("Web UI - ") + (src ? src : "");
        } else {
            detail = "Ready";
        }
        /* The footer has four fixed slots, all taken on a media-server row, so
         * the sign-out button is announced on the row it applies to. */
        if (p->sign_out && p->is_signed_in && p->is_signed_in())
            detail += "  \u00b7  Signed in - OPTIONS to sign out";
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
        if ((p->caps & EVO_PROVIDER_CAP_WEBUI) || p->ui_embedded) {
            openSourceEditor();     /* the address; opens the provider once set */
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

    if (m_signIn == SignIn::QcStart || m_signIn == SignIn::QcWait ||
        m_signIn == SignIn::QcPolling) {
        renderQuickConnect(framebuffer, width, height);
        return;
    }

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
        bool web = p && (p->caps & EVO_PROVIDER_CAP_WEBUI) && !isNativeWeb(p);
        bool server = p && isNativeWeb(p);
        params.rows[i].title = p ? p->name : m_pickIds[i].c_str();
        params.rows[i].detail = m_pickDetail[i].c_str();
        params.rows[i].icon_path = (web || server) ? "../icons/icon_emby.png" : "../icons/icon_folder.png";
        params.rows[i].badge = web ? "WEB" : server ? "SERVER"
                             : (p && (p->caps & EVO_PROVIDER_CAP_LIVE)) ? "LIVE" : "";
        params.rows[i].progress = -1;
        params.rows[i].has_chevron = 1;
        params.rows[i].is_focused = (!railFocused && i == m_pickIndex);
    }

    const evo_provider_t* fp = (m_pickIndex >= 0 && m_pickIndex < total)
                             ? evo_provider_find(m_pickIds[m_pickIndex].c_str()) : nullptr;
    params.hint_count = 0;
    params.hints[params.hint_count].glyph_path = "../icons/btn_cross.png";
    params.hints[params.hint_count++].label = "OPEN";
    params.hints[params.hint_count].glyph_path = "../icons/btn_square.png";
    params.hints[params.hint_count++].label = (fp && (fp->caps & EVO_PROVIDER_CAP_WEBUI))
                                              ? "EDIT ADDRESS"
                                              : (fp && fp->ui_embedded) ? "ADD ADDON" : "EDIT PLAYLIST";
    if (fp && isNativeWeb(fp) && fp->is_configured()) {
        params.hints[params.hint_count].glyph_path = "../icons/btn_triangle.png";
        params.hints[params.hint_count++].label = "WEB VERSION";
    }
    params.hints[params.hint_count].glyph_path = "../icons/btn_circle.png";
    params.hints[params.hint_count++].label = "BACK";

    evo_rmlui_update_list(&params);
    evo_rmlui_render_list(framebuffer, width, height);
}

void ProviderHostScreen::enterStreamPicker()
{
    g_pick_stage = PickStage::Idle;
    m_streamPick = true;
    m_streamIndex = 0;
    m_resolving = false;
    m_tunePending = false;
    evo_rmlui_provider_set_tuning(0);
    evo_rmlui_provider_set_loading(0, "");
    evo_feedback(EVO_FB_OPEN);
}

void ProviderHostScreen::cancelStreamPicker()
{
    m_streamPick = false;
    g_pick_stage = PickStage::Idle;
    ++g_pick_gen;
    g_variants = VariantsState::None;
    g_variants_merged = false;
    m_resolving = false;
}

void ProviderHostScreen::chooseStream(int index)
{
    if (index < 0 || index >= g_pending.choice_count) return;
    if (g_start_running) return;

    evo_feedback(EVO_FB_CONFIRM);
    g_pending.picked = 1;
    m_streamPick = false;
    m_resolving = true;

    /* A quality read still in flight is no longer wanted: drop its reply. */
    ++g_pick_gen;
    if (g_variants == VariantsState::Reading) g_variants = VariantsState::None;

    char msg[128];
    std::snprintf(msg, sizeof(msg), "Opening %s...", g_pending.choices[index].label);
    /* A media server's versions are not live TV (#116): name the provider. */
    const evo_provider_t* tp = evo_provider_find(g_pending.provider);
    const bool live = g_pending.is_live || (tp && (tp->caps & EVO_PROVIDER_CAP_LIVE));
    toast(live ? "LIVE TV" : (tp ? tp->name : "PROVIDER"), msg);
    evo_rmlui_provider_set_tuning(1);
    evo_rmlui_provider_set_loading(1, msg);
    evo_bt("prov_screen: stream %d chosen for %s: %s", index, g_pending.title,
           g_pending.choices[index].url);

    if (launch_choice(index) != 0) {
        m_resolving = false;
        m_streamPick = true;                 /* it would not start: keep the list */
    }
}

void ProviderHostScreen::renderStreamPicker(uint32_t* framebuffer, int width, int height)
{
    evo_rmlui_list_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.section = EVO_SECTION_EMBY;
    params.rail_focused = 0;
    params.title = "CHOOSE A STREAM";

    /* Say what the list is waiting for, or why it has no qualities: silence here
     * looks like the feature does not exist. */
    static char subtitle[EVO_PROVIDER_MAX_TITLE + 64];
    const char* note = (g_variants == VariantsState::Reading) ? "reading the available qualities..."
                     : (g_variants == VariantsState::Failed)  ? "qualities unavailable - the server did not answer"
                     : nullptr;
    if (note)
        std::snprintf(subtitle, sizeof subtitle, "%s  -  %s", g_pending.title, note);
    else
        std::snprintf(subtitle, sizeof subtitle, "%s", g_pending.title);
    params.subtitle = subtitle;

    static char detail[kMaxChoices][192];
    const int total = g_pending.choice_count;
    params.total_count = total;
    params.cursor_index = total ? m_streamIndex : -1;
    const int rows = std::min(EVO_RMLUI_LIST_ROWS, total);
    params.row_count = rows;

    for (int i = 0; i < rows; ++i) {
        const evo_stream_choice_t& c = g_pending.choices[i];
        const int kind = g_pending.kind[i];

        if (kind == KindSource) {
            const std::string d = source_detail(c);
            if (!d.empty())
                std::snprintf(detail[i], sizeof detail[i], "%s", d.c_str());
            else
                std::snprintf(detail[i], sizeof detail[i], "Source %d of %d%s", i + 1, total,
                              choice_is_hls(c) ? "  -  HLS" : "");
        } else if (kind == KindListed) {
            std::snprintf(detail[i], sizeof detail[i], "%s",
                          choice_is_hls(c) ? "As the playlist lists it - EVO picks the quality"
                                           : "As the playlist lists it");
        } else if (kind == KindGuess) {
            std::snprintf(detail[i], sizeof detail[i], "%s",
                          "The same address with a different extension - a guess");
        } else {
            /* a quality variant: what the master says about it */
            std::string d;
            auto add = [&d](const std::string& s) {
                if (s.empty()) return;
                if (!d.empty()) d += "  -  ";
                d += s;
            };
            char buf[48];
            if (c.width > 0 && c.height > 0) {
                std::snprintf(buf, sizeof buf, "%dx%d", c.width, c.height);
                add(buf);
            }
            auto upper = [](const char* s) {
                std::string u(s);
                std::transform(u.begin(), u.end(), u.begin(), [](unsigned char ch) { return std::toupper(ch); });
                return u;
            };
            add(upper(c.video_codec));
            add(upper(c.audio_codec));
            if (c.bitrate_bps > 0) {
                std::snprintf(buf, sizeof buf, "%.1f Mbps", (double)c.bitrate_bps / 1e6);
                add(buf);
            }
            std::snprintf(detail[i], sizeof detail[i], "%s", d.empty() ? "Quality variant" : d.c_str());
        }

        params.rows[i].title = c.label[0] ? c.label : "Stream";
        params.rows[i].detail = detail[i];
        params.rows[i].icon_path = "../icons/icon_tv.png";
        params.rows[i].badge = kind == KindSource ? "SOURCE" : kind == KindListed ? "LISTED"
                             : kind == KindGuess ? "GUESS" : "HLS";
        params.rows[i].progress = -1;
        params.rows[i].has_chevron = 1;
        params.rows[i].is_focused = (i == m_streamIndex);
    }

    params.hint_count = 2;
    params.hints[0].glyph_path = "../icons/btn_cross.png";
    params.hints[0].label = "PLAY";
    params.hints[1].glyph_path = "../icons/btn_circle.png";
    params.hints[1].label = "BACK";

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
    m_streamPick = false;
    g_pick_stage = PickStage::Idle;
    ++g_pick_gen;
    g_variants = VariantsState::None;
    g_variants_merged = false;
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

    /* Discovery takes ~1.5 s at most and then opens the keyboard itself; a
     * sign-in in flight finishes or fails on its own the same way. */
    if (m_discovering) return true;
    if (m_signIn == SignIn::Suggest || m_signIn == SignIn::Signing) return true;

    if (m_streamPick) {
        const int n = g_pending.choice_count;
        if (pressed & PadButtons::Up) {
            if (m_streamIndex > 0) { --m_streamIndex; evo_feedback(EVO_FB_MOVE); }
            else evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        if (pressed & PadButtons::Down) {
            if (m_streamIndex + 1 < n) { ++m_streamIndex; evo_feedback(EVO_FB_MOVE); }
            else evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        if (pressed & PadButtons::Cross) {
            chooseStream(m_streamIndex);
            return true;
        }
        if (pressed & PadButtons::Circle) {
            evo_feedback(EVO_FB_CANCEL);
            cancelStreamPicker();
            return true;
        }
        return true;        /* the list has the pad while it is open */
    }
    if (signInQcInput(pressed)) return true;

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
        if (pressed & PadButtons::Triangle) {
            /* The site itself, in the system browser - for what EVO's screens
             * do not cover (server settings, admin). */
            const evo_provider_t* tp = (m_pickIndex >= 0 && m_pickIndex < n)
                                     ? evo_provider_find(m_pickIds[m_pickIndex].c_str()) : nullptr;
            if (tp && isNativeWeb(tp) && tp->is_configured()) {
                evo_feedback(EVO_FB_OPEN);
                m_providerId = tp->id;
                m_picking = false;
                openWebProvider();
                if (!m_web) enterPicker();
            }
            return true;
        }
        if (pressed & PadButtons::Options) {
            /* Sign out of the focused provider. Twice, within a few seconds:
             * one stray press must not cost the user a login. */
            const evo_provider_t* op = (m_pickIndex >= 0 && m_pickIndex < n)
                                     ? evo_provider_find(m_pickIds[m_pickIndex].c_str()) : nullptr;
            if (!op || !op->sign_out || !op->is_signed_in || !op->is_signed_in()) {
                evo_feedback(EVO_FB_BOUNDARY);
                return true;
            }
            uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (m_signOutArmed == op->id && now - m_signOutAt < 4000) {
                op->sign_out();
                m_signOutArmed.clear();
                evo_feedback(EVO_FB_CONFIRM);
                evo_bt("prov_screen: signed out of '%s'", op->id);
                toast(op->name, "Signed out");
                int keep = m_pickIndex;
                enterPicker();
                m_pickIndex = keep;
            } else {
                m_signOutArmed = op->id;
                m_signOutAt = now;
                evo_feedback(EVO_FB_OPEN);
                toast(op->name, "Press OPTIONS again to sign out");
            }
            return true;
        }
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

    /* The Live TV options panel: the d-pad and X work its rows, Circle steps back,
     * Options closes it, and nothing may act on the grid behind it. */
    if (evo_rmlui_provider_panel_open()) {
        if (pressed & PadButtons::Options) {
            evo_feedback(EVO_FB_CANCEL);
            evo_rmlui_provider_hide_panel();
        } else if (pressed & PadButtons::Up) {
            evo_feedback(EVO_FB_MOVE);
            evo_rmlui_provider_key(EvoRmlProviderHost::KeyUp);
        } else if (pressed & PadButtons::Down) {
            evo_feedback(EVO_FB_MOVE);
            evo_rmlui_provider_key(EvoRmlProviderHost::KeyDown);
        } else if (pressed & PadButtons::Cross) {
            evo_feedback(EVO_FB_CONFIRM);
            evo_rmlui_provider_key(EvoRmlProviderHost::KeyAccept);
        } else if (pressed & PadButtons::Circle) {
            evo_feedback(EVO_FB_CANCEL);
            evo_rmlui_provider_key(EvoRmlProviderHost::KeyBack);
        }
        return true;
    }

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
    if (pressed & PadButtons::L1) {
        evo_feedback(EVO_FB_MOVE);
        return evo_rmlui_provider_key(EvoRmlProviderHost::KeyPageUp) != 0;
    }
    if (pressed & PadButtons::R1) {
        evo_feedback(EVO_FB_MOVE);
        return evo_rmlui_provider_key(EvoRmlProviderHost::KeyPageDown) != 0;
    }

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
        /* Out of a provider's root: back to the chooser when there is one to
         * go back to, so switching provider is one Circle away rather than
         * a trip through the main menu. */
        if (pickable_count(nullptr) > 1) {
            evo_feedback(EVO_FB_CANCEL);
            enterPicker();
            return true;
        }
        if (sm) sm->navigateTo(ScreenId::MainMenu);
        return true;
    }

    if (pressed & PadButtons::Square) {
        openSearch();
        return true;
    }

    if (pressed & PadButtons::Triangle) {
        if (!evo_rmlui_provider_get_focused_is_folder()) {
            const char* title = evo_rmlui_provider_get_focused_title();
            const char* id = evo_rmlui_provider_get_focused_id();
            if (title && title[0]) {
                /* Heap, not stack: a resolve can answer later from
                 * evo_net_poll() (Stremio, and Emby/Jellyfin since #116 ask
                 * the server for its versions), long after this frame. */
                struct FavCtx {
                    char title[128];
                    char id[128];
                };
                auto* ctx = static_cast<FavCtx*>(std::calloc(1, sizeof(FavCtx)));
                if (!ctx) return true;
                std::snprintf(ctx->title, sizeof(ctx->title), "%s", title);
                std::snprintf(ctx->id, sizeof(ctx->id), "%s", id ? id : "");

                /* Toggle: the resolved stream URL when there is one, so the
                 * favourite path points at the stream; else the title. */
                auto toggle = [](FavCtx* c, const char* url) {
                    int idx = url ? favorites_find(url) : -1;
                    if (idx < 0) idx = favorites_find(c->title);
                    if (idx < 0 && !url && c->id[0]) idx = favorites_find(c->id);
                    if (idx >= 0) {
                        favorites_remove(favorite_files[idx].path);
                        favorites_save();
                        toast("FAVORITES", "Removed from favorites");
                        evo_feedback(EVO_FB_CANCEL);
                    } else {
                        favorites_add(url ? url : c->title, c->title, 0.0);
                        favorites_save();
                        toast("FAVORITES", "Added to favorites");
                        evo_feedback(EVO_FB_CONFIRM);
                    }
                };
                static void (*s_toggle)(FavCtx*, const char*) = toggle;

                if (evo_provider_resolve_chain(m_providerId.c_str(), id, [](int ok, const evo_stream_choice_t* choices, int count, void* ud) {
                        auto* c = static_cast<FavCtx*>(ud);
                        const bool have = ok && count > 0 && choices && choices[0].url[0];
                        s_toggle(c, have ? choices[0].url : nullptr);
                        std::free(c);
                    }, ctx) != 0) {
                    toggle(ctx, nullptr);
                    std::free(ctx);
                }
                evo_rmlui_provider_reload();
                return true;
            }
        }
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
        const evo_provider_t* op = evo_provider_find(m_providerId.c_str());
        if (op && (isNativeWeb(op) || op->ui_embedded)) {
            /* No setup page: the chooser is where the address is edited and
             * the web version opened. */
            enterPicker();
        } else if (op && std::strcmp(op->id, "iptv") == 0 &&
                   !evo_rmlui_provider_get_query()[0]) {
            openProviderMenu();     /* playlist or guide, for the one on screen */
        } else {
            evo_rmlui_provider_show_setup();
        }
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
        /* #101: a web-UI provider opens straight away on its new address. A
         * media server browsed natively signs in first (openProvider). */
        toast(p->name, "Server saved");
        if (isNativeWeb(p))
            self->openProvider(p->id);
        else
            self->openWebProvider();
        return;
    }

    if (value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0) {
        self->m_lastTypedUrl = value;
    }

    /* Reopen the host so the catalog is re-fetched from the new source. */
    if (p->ui_embedded)
        toast(p->name, value.empty() ? "Addons cleared" : "Addon added");
    else
        toast("PROVIDERS", value.empty() ? "Playlist cleared" : "Playlist updated");

    std::string id = self->m_providerId;
    if (self->m_opened) {
        evo_rmlui_provider_close();
        self->m_opened = false;
    }
    self->m_opened = evo_rmlui_provider_open(id.c_str(),
                                             DisplayWidth, DisplayHeight) != 0;
}

void ProviderHostScreen::OnGuideSubmitted(const char* text, void* userdata)
{
    (void)userdata;
    std::string value = text ? text : "";
    size_t b = value.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        value.clear();
    } else {
        size_t e = value.find_last_not_of(" \t\r\n");
        value = value.substr(b, e - b + 1);
    }
    bool ok = value.empty() || value.rfind("http://", 0) == 0 ||
              value.rfind("https://", 0) == 0 || value[0] == '/';
    if (!ok) {
        toast("IPTV", "Guide must start with http(s):// or /");
        return;
    }
    if (provider_iptv_pin_guide(value.c_str()) != 0) {
        toast("IPTV", "Open a playlist first");
        return;
    }
    toast("LIVE TV", value.empty() ? "Guide: automatic" : "Guide saved for this playlist");
    evo_rmlui_provider_hide_panel();
    evo_rmlui_provider_reload();
}

static std::string baseName(const std::string& p)
{
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

/* What a guide is called on screen: a file's name, a web guide's host. */
static std::string guideLabel(const std::string& url)
{
    if (url.empty()) return "";
    if (url[0] == '/') return baseName(url);
    size_t b = url.find("://");
    b = (b == std::string::npos) ? 0 : b + 3;
    size_t e = url.find('/', b);
    return url.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

static std::string ageText(long long s)
{
    char buf[48];
    if (s < 90)            return "just now";
    if (s < 3600)          std::snprintf(buf, sizeof buf, "%lld min ago", s / 60);
    else if (s < 2 * 86400) std::snprintf(buf, sizeof buf, "%lld h ago", s / 3600);
    else                    std::snprintf(buf, sizeof buf, "%lld days ago", s / 86400);
    return buf;
}

/* Owns the strings behind an evo_panel_t until it has been handed over. */
struct PanelBuild {
    struct Row { std::string id, title, detail, badge, icon; int radio = 0, on = 0, warn = 0, chevron = 0, live = 0; };
    std::vector<Row> rows;
    void show(const char* crumb, const char* eyebrow, const std::string& title,
              const std::string& sub, const char* note_b, const char* note,
              const char* accept, const char* back, int focus)
    {
        std::vector<evo_panel_row_t> r;
        for (const Row& x : rows)
            r.push_back({ x.id.c_str(), x.title.c_str(), x.detail.c_str(), x.badge.c_str(),
                          x.icon.c_str(), x.radio, x.on, x.warn, x.chevron, x.live });
        evo_panel_t p = { crumb, eyebrow, title.c_str(), sub.c_str(), note_b, note,
                          accept, back, r.data(), (int)r.size(), focus };
        evo_rmlui_provider_show_panel(&p);
    }
};

void ProviderHostScreen::openProviderMenu(int focus)
{
    evo_feedback(EVO_FB_OPEN);
    m_panelPage = "menu";

    const int n = provider_iptv_channel_count();
    const std::string pl = baseName(provider_iptv_playlist_name());
    const std::string pin = provider_iptv_pinned_guide();
    const std::string inUse = provider_iptv_guide_in_use();

    PanelBuild pb;
    PanelBuild::Row r;

    r.id = "menu:playlist"; r.title = "Playlist"; r.icon = "/assets/icons/icon_tv.png";
    r.detail = pl.empty() ? std::string("None yet")
                          : pl + " · " + std::to_string(n) + " channels";
    r.chevron = 1;
    pb.rows.push_back(r);

    r = PanelBuild::Row();
    r.id = "menu:guide"; r.title = "Channel guide"; r.icon = "/assets/icons/icon_recent_files.png";
    r.chevron = 1;
    if (pin == "none") {
        r.detail = "Turned off for this playlist";
    } else if (!inUse.empty()) {
        r.detail = guideLabel(inUse) + " · covers " + std::to_string(provider_iptv_guide_matched()) +
                   " of " + std::to_string(n) + " channels";
    } else {
        r.detail = "None yet - choose one";
        r.warn = 1;
    }
    pb.rows.push_back(r);

    r = PanelBuild::Row();
    r.id = "menu:refresh"; r.title = "Refresh guide"; r.icon = "/assets/icons/icon_activity.png";
    long long age = provider_iptv_guide_age();
    r.detail = inUse.empty() ? std::string("No guide loaded")
             : inUse[0] == '/' ? std::string("Reads the USB file again")
             : age >= 0 ? "Updated " + ageText(age) : std::string("Download it again");
    pb.rows.push_back(r);

    pb.show("", "LIVE TV", "Options", pl, "", "", "SELECT", "CLOSE", focus);
}

void ProviderHostScreen::openGuidePicker()
{
    evo_feedback(EVO_FB_OPEN);
    m_panelPage = "guide";

    const int n = provider_iptv_channel_count();
    const std::string pl = baseName(provider_iptv_playlist_name());
    const std::string pin = provider_iptv_pinned_guide();
    const std::string inUse = provider_iptv_guide_in_use();

    PanelBuild pb;
    int focus = 0;

    PanelBuild::Row a;
    a.id = "guide:auto"; a.title = "Automatic"; a.radio = 1; a.on = pin.empty();
    a.detail = (pin.empty() && !inUse.empty()) ? "Picked " + guideLabel(inUse)
                                               : std::string("EVO picks a guide that fits");
    pb.rows.push_back(a);

    /* Every guide EVO knows of for this playlist: files on the stick, the one in
     * use, the one chosen, the one in iptv.conf. */
    std::vector<std::string> urls;
    char found[16][512];
    int nf = provider_iptv_usb_guides(found, 16);
    for (int i = 0; i < nf; ++i) urls.emplace_back(found[i]);
    for (const std::string& u : { inUse, (pin == "none" ? std::string() : pin),
                                  std::string(provider_iptv_xmltv_url()) })
        if (!u.empty() && std::find(urls.begin(), urls.end(), u) == urls.end())
            urls.push_back(u);

    for (const std::string& u : urls) {
        PanelBuild::Row r;
        r.id = "guide:" + u;
        r.title = guideLabel(u);
        r.radio = 1;
        r.on = (pin == u);
        int cov = provider_iptv_guide_coverage(u.c_str());
        std::string where = u[0] == '/' ? "USB" : "Web";
        if (cov < 0)       r.detail = where + " · not tried yet";
        else if (cov == 0) { r.detail = where + " · covers none of these channels"; r.warn = 1; }
        else               r.detail = where + " · covers " + std::to_string(cov) + " of " +
                                      std::to_string(n) + " channels";
        if (u == inUse) { r.badge = "IN USE"; r.live = 1; }
        if (r.on) focus = (int)pb.rows.size();
        pb.rows.push_back(r);
    }

    PanelBuild::Row none;
    none.id = "guide:none"; none.title = "None"; none.radio = 1; none.on = (pin == "none");
    none.detail = "Show no guide for this playlist";
    if (none.on) focus = (int)pb.rows.size();
    pb.rows.push_back(none);

    PanelBuild::Row url;
    url.id = "guide:url"; url.title = "Enter guide address…";
    url.detail = "Web address of an XMLTV guide";
    url.icon = "/assets/icons/icon_keyboard.png"; url.chevron = 1;
    pb.rows.push_back(url);

    pb.show("OPTIONS  ›  ", "CHANNEL GUIDE", "Channel guide", "For " + pl,
            "Guides are not part of a playlist.",
            "Pick one once and EVO remembers it for this playlist. Put .xml guides on your "
            "USB stick and they show up here.",
            "USE THIS GUIDE", "BACK", focus);
}

void ProviderHostScreen::applyChoice(const std::string& id)
{
    if (id == "panel:back") {
        if (m_panelPage == "guide") openProviderMenu(1);
        else evo_rmlui_provider_hide_panel();
    } else if (id == "menu:playlist") {
        evo_rmlui_provider_hide_panel();
        evo_rmlui_provider_show_setup();
    } else if (id == "menu:guide") {
        openGuidePicker();
    } else if (id == "menu:refresh") {
        provider_iptv_refresh_guide();
        evo_rmlui_provider_hide_panel();
        toast("LIVE TV", "Refreshing the guide...");
    } else if (id == "guide:url") {
        const std::string pin = provider_iptv_pinned_guide();
        evo_keyboard_open("Guide address (XMLTV, http(s)://...)",
                          (!pin.empty() && pin[0] != '/' && pin != "none") ? pin.c_str() : "https://",
                          512, &ProviderHostScreen::OnGuideSubmitted, this);
    } else if (id.rfind("guide:", 0) == 0) {
        std::string choice = id.substr(6);
        std::string pinv = choice == "auto" ? std::string() : choice;
        if (provider_iptv_pin_guide(pinv.c_str()) != 0) {
            toast("LIVE TV", "Open a playlist first");
            return;
        }
        evo_rmlui_provider_hide_panel();
        toast("LIVE TV", choice == "auto" ? "Guide: automatic"
                       : choice == "none" ? "Guide turned off for this playlist"
                       : ("Guide: " + guideLabel(choice)).c_str());
        evo_rmlui_provider_reload();
    }
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
        /* Nothing set yet: Emby and Jellyfin answer a LAN broadcast, so look
         * first and pre-fill what answers - typing an address with the D-pad
         * took 103 presses on hardware. update() opens the keyboard. */
        const char* product = m_providerId == "emby"     ? "EmbyServer"
                            : m_providerId == "jellyfin" ? "JellyfinServer"
                            : nullptr;
        if (product) {
            evo_net_discover_start(product);
            m_discovering = true;
            evo_feedback(EVO_FB_OPEN);
            char msg[96];
            std::snprintf(msg, sizeof msg, "Looking for %s on your network...", p->name);
            toast(p->name, msg);
            return;
        }
        initial = "http://";
    } else if (p->ui_embedded) {
        initial = "https://";       /* an addon to ADD, not a source to edit */
    } else if (!m_lastTypedUrl.empty()) {
        initial = m_lastTypedUrl;
    }
    openSourceKeyboard(initial);
}

void ProviderHostScreen::openSourceKeyboard(const std::string& initial)
{
    const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
    if (!p) return;

    char title[96];
    if (p->caps & EVO_PROVIDER_CAP_WEBUI)
        std::snprintf(title, sizeof title, "%s server (http(s)://host:port)", p->name);
    else if (p->ui_embedded)
        std::snprintf(title, sizeof title, "Add a Stremio addon (its manifest URL)");
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
    g_pending.resume_sec = sel.resume_sec;
    g_resolve_finished = false;

    /* Read the setting now, not when the reply lands: what was on when the
     * channel was opened is what applies to it. */
    if (auto st = Application::getInstance().getSettingsService())
        g_pending.ask = st->isAskStreamEnabled() ? 1 : 0;
    if (const evo_provider_t* pp = evo_provider_find(sel.provider_id))
        if (pp->caps & EVO_PROVIDER_CAP_PICK) g_pending.ask = 1;
    g_pending.picked = 0;
    g_pick_stage = PickStage::Idle;
    ++g_pick_gen;
    g_variants = VariantsState::None;
    g_variants_merged = false;

    m_tunePending = true;
    m_tuneFrames = 0;

    char msg[128];
    const evo_provider_t* tp = evo_provider_find(sel.provider_id);
    const bool live = tp && (tp->caps & EVO_PROVIDER_CAP_LIVE);
    std::snprintf(msg, sizeof(msg), live ? "Tuning %s..." : "Opening %s...", sel.title);
    toast(live ? "LIVE TV" : (tp ? tp->name : "PROVIDER"), msg);
    evo_rmlui_provider_set_tuning(1);
    evo_rmlui_provider_set_loading(1, msg);
}

void ProviderHostScreen::update(double deltaMs)
{
    StatefulScreen::update(deltaMs);

    signInStep();

    if (m_discovering) {
        char addr[128];
        int rc = evo_net_discover_poll(addr, sizeof addr);
        if (rc != 0) {
            m_discovering = false;
            const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
            if (rc > 0) {
                evo_bt("prov_screen: discovery found %s for '%s'", addr, m_providerId.c_str());
                toast(p ? p->name : "PROVIDER", "Found a server - press Triangle to use it");
                openSourceKeyboard(addr);
            } else {
                evo_bt("prov_screen: discovery found nothing for '%s'", m_providerId.c_str());
                toast(p ? p->name : "PROVIDER", "No server found - type its address");
                openSourceKeyboard("http://");
            }
        }
    }

    /* #101: the web UI's session ended when the user closed the browser (a
     * handed-over stream keeps it alive). Back to the chooser, or out when
     * this is the only provider. */
    if (m_web) {
        /* EVO ended it (server down, page never loaded): say why, and stay
         * on the chooser so the address can be fixed right there. Checked
         * first: a refused connect can end the session before this screen
         * ever saw it running. */
        char err[256];
        if (!evo_webui_session_active() && evo_webui_take_error(err, sizeof err)) {
            m_web = false;
            m_webSeen = false;
            const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
            toast(p ? p->name : "PROVIDER", err);
            enterPicker();
        } else if (evo_webui_session_active()) {
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

    /* The server rejected the saved session (expired, revoked, a server
     * reset): the catalog cannot load, so go to sign-in rather than leave
     * "Could not load the catalog" with no way forward. */
    if (const evo_provider_t* sp = evo_provider_find(m_providerId.c_str())) {
        if (sp->needs_sign_in && sp->sign_in && sp->needs_sign_in()) {
            evo_bt("prov_screen: '%s' session rejected - back to sign-in", m_providerId.c_str());
            toast(sp->name, "Please sign in again");
            evo_rmlui_provider_close();
            m_opened = false;
            beginSignIn();
            return;
        }
    }

    /* Bundle refresh, artwork decode, and any catalog reply that landed. */
    evo_rmlui_provider_tick();

    int action = evo_rmlui_provider_take_action();
    if (action == EVO_PROVIDER_ACTION_SETUP_URL) {
        openSourceEditor();
    } else if (action == EVO_PROVIDER_ACTION_SETUP_USB) {
        browseUsb();
    } else if (action == EVO_PROVIDER_ACTION_CHOICE) {
        char choice[1100];
        if (evo_rmlui_provider_take_choice(choice, sizeof choice))
            applyChoice(choice);
    }

    if (m_providerId == "iptv") {
        char note[160];
        if (provider_iptv_take_guide_notice(note, sizeof note))
            toast("LIVE TV", (std::string(note) + " · change in Options").c_str());
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
            if (g_pending.picked) {
                /* The user chose this stream. Nothing else is tried for them:
                 * say what happened and give the list back. */
                const evo_stream_choice_t& bad = g_pending.choices[g_pending.active_choice];
                char msg[128];
                std::snprintf(msg, sizeof(msg), "%s did not open - pick another", bad.label);
                toast("STREAM", msg);
                evo_bt("prov_screen: chosen stream %d failed (%s) - back to the list",
                       g_pending.active_choice, bad.url);
                evo_rmlui_provider_set_tuning(0);
                evo_rmlui_provider_set_loading(0, "");
                m_resolving = false;
                m_streamPick = true;
                m_streamIndex = g_pending.active_choice;
                return;
            }
            if (g_pending.active_choice + 1 < g_pending.choice_count) {
                g_pending.active_choice++;
                evo_bt("prov_screen: stream choice %d failed, falling back to choice %d: %s",
                       g_pending.active_choice - 1, g_pending.active_choice,
                       g_pending.choices[g_pending.active_choice].url);
                char retry_msg[128];
                std::snprintf(retry_msg, sizeof(retry_msg), "Connecting to backup (%s)...",
                              g_pending.choices[g_pending.active_choice].label);
                toast("STREAM", retry_msg);
                evo_rmlui_provider_set_status(retry_msg, 0);

                launch_choice(g_pending.active_choice);
                return;
            }
            evo_rmlui_provider_set_tuning(0);
            evo_rmlui_provider_set_loading(0, "");
            evo_rmlui_provider_set_status("Stream unavailable or connection timed out", 1);
        }
    }

    /* The qualities have landed while the picker is open: the rows after the listed
     * one just moved down, so the cursor moves with them. */
    if (g_variants_merged) {
        g_variants_merged = false;
        if (m_streamPick && m_streamIndex > 0)
            m_streamIndex += g_variants_added;
        g_variants_added = 0;
    }

    /* The read is bounded in what it claims: after a while the list stops saying
     * "reading" and a late reply is dropped. */
    if (g_variants == VariantsState::Reading) {
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_variants_started).count();
        if (waited > kVariantsGiveUpMs) {
            ++g_pick_gen;
            g_variants = VariantsState::Failed;
            evo_bt("provider: stream picker: the master did not answer in %d ms", kVariantsGiveUpMs);
        }
    }

    /* One row and no qualities coming is not a choice: play it. (Once - if the
     * user's own pick fails the list returns and stays.) */
    if (m_streamPick && g_variants == VariantsState::Failed && g_pending.choice_count == 1 &&
        !g_pending.picked && !g_start_running) {
        chooseStream(0);
    }

    if (g_pick_stage == PickStage::Ready && !m_streamPick)
        enterStreamPicker();

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
    if (m_streamPick) {
        renderStreamPicker(framebuffer, width, height);
        return;
    }
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

/* ------------------------------------------------------------------------- */
/* Media-server sign-in                                                      */
/* ------------------------------------------------------------------------- */

bool ProviderHostScreen::isNativeWeb(const ::evo_provider* p)
{
    return p && (p->caps & EVO_PROVIDER_CAP_WEBUI) &&
           (p->caps & EVO_PROVIDER_CAP_CATALOG) && p->list_catalog;
}

void ProviderHostScreen::beginSignIn()
{
    const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
    if (!p || !p->sign_in) return;
    std::string id = m_providerId;
    enterPicker();                  /* the chooser sits behind the keyboards */
    m_providerId = id;
    m_signInUser.clear();
    m_signInPass.clear();
    m_qcCode.clear();
    evo_bt("prov_screen: sign-in to '%s' begins", id.c_str());
    /* Quick Connect first where the server has it: approving a six-digit code
     * on a phone beats spelling a password with the D-pad. A server with it
     * switched off answers the start with an error and falls through. */
    if (p->qc_start) {
        m_signIn = SignIn::QcStart;
        if (p->qc_start(&ProviderHostScreen::OnQcCode, this) == 0) return;
    }
    beginPasswordSignIn();
}

void ProviderHostScreen::beginPasswordSignIn()
{
    const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
    if (!p || !p->sign_in) { m_signIn = SignIn::None; return; }
    m_signIn = SignIn::Suggest;
    /* The server's first public user pre-fills the name - one press, not a
     * spelled-out user name. A server that hides its users just gives "". */
    if (!p->suggest_user || p->suggest_user(&ProviderHostScreen::OnSignInSuggest, this) != 0)
        m_signIn = SignIn::OpenUser;
}

static long long qc_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void ProviderHostScreen::OnQcCode(int ok, const char* code, void* ud)
{
    auto* self = static_cast<ProviderHostScreen*>(ud);
    if (!self || self->m_signIn != SignIn::QcStart) return;
    if (!ok || !code || !code[0]) {
        evo_bt("prov_screen: quick connect unavailable for '%s' - password sign-in",
               self->m_providerId.c_str());
        self->beginPasswordSignIn();
        return;
    }
    self->m_qcCode = code;
    self->m_qcNextPollMs = qc_now_ms() + 3000;
    /* Jellyfin drops a pending request after about ten minutes. */
    self->m_qcDeadlineMs = qc_now_ms() + 9 * 60 * 1000;
    self->m_signIn = SignIn::QcWait;
    evo_bt("prov_screen: quick connect code %s for '%s'", code, self->m_providerId.c_str());
}

void ProviderHostScreen::OnQcPoll(int state, const char* msg, void* ud)
{
    auto* self = static_cast<ProviderHostScreen*>(ud);
    if (!self || self->m_signIn != SignIn::QcPolling) return;
    const evo_provider_t* p = evo_provider_find(self->m_providerId.c_str());
    if (state == 0) {
        self->m_qcNextPollMs = qc_now_ms() + 3000;
        self->m_signIn = SignIn::QcWait;
        return;
    }
    self->m_signIn = SignIn::None;
    self->m_qcCode.clear();
    evo_bt("prov_screen: quick connect for '%s' -> %s (%s)", self->m_providerId.c_str(),
           state > 0 ? "signed in" : "failed", msg ? msg : "");
    toast(p ? p->name : "PROVIDER", msg && msg[0] ? msg : (state > 0 ? "Signed in" : "Quick Connect failed"));
    if (state > 0)
        self->openProvider(self->m_providerId);
}

/* While the code is on screen: Square switches to the password, Circle gives
 * up. Everything else is swallowed - there is nothing else to press. */
bool ProviderHostScreen::signInQcInput(uint32_t pressed)
{
    if (m_signIn != SignIn::QcStart && m_signIn != SignIn::QcWait &&
        m_signIn != SignIn::QcPolling)
        return false;
    if (pressed & PadButtons::Square) {
        evo_feedback(EVO_FB_OPEN);
        m_qcCode.clear();
        beginPasswordSignIn();
    } else if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        m_qcCode.clear();
        m_signIn = SignIn::None;
        evo_bt("prov_screen: quick connect for '%s' cancelled", m_providerId.c_str());
    }
    return true;
}

void ProviderHostScreen::renderQuickConnect(uint32_t* framebuffer, int width, int height)
{
    evo_rmlui_list_params_t params;
    std::memset(&params, 0, sizeof(params));
    params.section = EVO_SECTION_EMBY;
    const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
    static char title[64], hint[160], code[16];
    std::snprintf(title, sizeof title, "SIGN IN TO %s", p ? p->name : "THE SERVER");
    params.title = title;
    params.subtitle = "Quick Connect - no password needed";
    params.is_empty = 1;
    if (m_qcCode.size() == 6)
        std::snprintf(code, sizeof code, "%.3s %.3s", m_qcCode.c_str(), m_qcCode.c_str() + 3);
    else
        std::snprintf(code, sizeof code, "%s", m_qcCode.empty() ? "..." : m_qcCode.c_str());
    params.empty_title = code;
    std::snprintf(hint, sizeof hint,
                  "In %s on your phone or computer, open Settings > Quick Connect and enter this code.",
                  p ? p->name : "the app");
    params.empty_hint = hint;
    params.empty_icon = "../icons/icon_emby.png";
    params.hint_count = 2;
    params.hints[0].glyph_path = "../icons/btn_square.png";
    params.hints[0].label = "USE PASSWORD";
    params.hints[1].glyph_path = "../icons/btn_circle.png";
    params.hints[1].label = "CANCEL";
    evo_rmlui_update_list(&params);
    evo_rmlui_render_list(framebuffer, width, height);
}

void ProviderHostScreen::OnSignInSuggest(const char* name, void* ud)
{
    auto* self = static_cast<ProviderHostScreen*>(ud);
    if (!self || self->m_signIn != SignIn::Suggest) return;
    self->m_signInUser = name ? name : "";
    self->m_signIn = SignIn::OpenUser;
}

void ProviderHostScreen::OnSignInUser(const char* text, void* ud)
{
    auto* self = static_cast<ProviderHostScreen*>(ud);
    if (!self) return;
    self->m_signInUser = text ? text : "";
    self->m_signIn = self->m_signInUser.empty() ? SignIn::None : SignIn::OpenPass;
}

void ProviderHostScreen::OnSignInPass(const char* text, void* ud)
{
    auto* self = static_cast<ProviderHostScreen*>(ud);
    if (!self) return;
    self->m_signInPass = text ? text : "";
    const evo_provider_t* p = evo_provider_find(self->m_providerId.c_str());
    self->m_signIn = SignIn::Signing;
    int rc = (p && p->sign_in)
           ? p->sign_in(self->m_signInUser.c_str(), self->m_signInPass.c_str(),
                        &ProviderHostScreen::OnSignInDone, self)
           : -1;
    self->m_signInPass.assign(self->m_signInPass.size(), '\0');
    self->m_signInPass.clear();
    if (rc != 0) {
        self->m_signIn = SignIn::None;
        toast(p ? p->name : "PROVIDER", "Could not reach the server");
    }
}

void ProviderHostScreen::OnSignInDone(int ok, const char* msg, void* ud)
{
    auto* self = static_cast<ProviderHostScreen*>(ud);
    if (!self || self->m_signIn != SignIn::Signing) return;
    self->m_signIn = SignIn::None;
    const evo_provider_t* p = evo_provider_find(self->m_providerId.c_str());
    evo_bt("prov_screen: sign-in to '%s' -> %s (%s)", self->m_providerId.c_str(),
           ok ? "ok" : "failed", msg ? msg : "");
    toast(p ? p->name : "PROVIDER", msg ? msg : (ok ? "Signed in" : "Sign-in failed"));
    if (ok)
        self->openProvider(self->m_providerId);
}

/* Called every frame from update(). Opens the next keyboard, and notices a
 * keyboard the user cancelled (Circle closes it without a callback). */
void ProviderHostScreen::signInStep()
{
    const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
    const char* name = p ? p->name : "Server";
    char title[96];
    switch (m_signIn) {
    case SignIn::QcWait: {
        const long long now = qc_now_ms();
        if (now > m_qcDeadlineMs) {
            m_signIn = SignIn::None;
            m_qcCode.clear();
            toast(name, "The code expired - press X to get a new one");
            break;
        }
        if (now >= m_qcNextPollMs && p && p->qc_poll) {
            m_signIn = SignIn::QcPolling;
            if (p->qc_poll(&ProviderHostScreen::OnQcPoll, this) != 0) {
                m_signIn = SignIn::QcWait;
                m_qcNextPollMs = now + 3000;
            }
        }
        break;
    }
    case SignIn::OpenUser:
        std::snprintf(title, sizeof title, "%s user name", name);
        m_signIn = SignIn::User;
        evo_keyboard_open(title, m_signInUser.c_str(), 63,
                          &ProviderHostScreen::OnSignInUser, this);
        break;
    case SignIn::OpenPass:
        std::snprintf(title, sizeof title, "Password for %s (empty if none)", m_signInUser.c_str());
        m_signIn = SignIn::Pass;
        evo_keyboard_open(title, "", 63, &ProviderHostScreen::OnSignInPass, this);
        break;
    case SignIn::User:
    case SignIn::Pass:
        if (!evo_keyboard_is_open()) {
            evo_bt("prov_screen: sign-in to '%s' cancelled", m_providerId.c_str());
            m_signIn = SignIn::None;
        }
        break;
    default:
        break;
    }
}

} // namespace evo
