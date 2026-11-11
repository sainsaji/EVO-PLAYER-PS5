#include "evo/screens/ProviderHostScreen.hpp"
#include "evo/screens/LaunchScreen.hpp"
#include "evo_features.h"
#include "evo/screens/BrowserScreen.hpp"
#include "evo/Application.hpp"
#include "evo/services/NetworkStorageService.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_cast_receiver.h"
#include "evo_recent.h"
#include "evo_favorites.h"
#include "evo_error.h"
#include "evo_feedback.h"
#include "evo_nav.h"

#include "evo_boot_trace.h"
#include "evo_boot_log.h"
#include "evo_prev_run.h"
#include "evo_toast.h"
#include "evo/animation/AnimationManager.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>

#include <algorithm>

extern "C" const uint32_t* evo_home_art_get(const char*, int, int*, int*);
extern "C" void evo_provider_art_poll(void);

namespace evo {

namespace {

enum LibAction { LIB_BROWSE, LIB_RECENT, LIB_FAVORITES, LIB_EMBY, LIB_SETTINGS, LIB_ABOUT };

struct LibTile {
    const char* title;
    const char* detail;
    const char* icon;
    LibAction   action;
};

/*
 * The library shelf, with disabled features filtered out once. Emby is off by
 * default (EVO_ENABLE_EMBY in evo_features.h) while it is being reworked; the
 * screens behind it still build and still work, they are just not reachable.
 */
const LibTile* libraryTiles() {
    static const LibTile tiles[] = {
        {"BROWSE",    "USB drives",           "../icons/icon_browse_usb.png",    LIB_BROWSE},
        {"RECENT",    "Resume watching",      "../icons/icon_recent_files.png",  LIB_RECENT},
        {"FAVORITES", "Saved for later",      "../icons/icon_favorites.png",     LIB_FAVORITES},
#if EVO_ENABLE_EMBY
        {"PROVIDERS", "Online sources",       "../icons/icon_emby.png",       LIB_EMBY},
#endif
        {"SETTINGS",  "Video & audio",        "../icons/icon_settings.png",      LIB_SETTINGS},
        {"ABOUT",     "Credits & info",       "../icons/icon_about_support.png", LIB_ABOUT},
    };
    return tiles;
}

int libraryTileCount() {
    return EVO_ENABLE_EMBY ? 6 : 5;
}

} // namespace


LaunchScreen::LaunchScreen()
    : StatefulScreen("LaunchScreen")
    , m_launchFsm(LaunchScreenState::HeroFocused, "LaunchFSM")
{
    initLaunchStateMachine();
}

void LaunchScreen::initLaunchStateMachine() {
    m_launchFsm
        .addState(LaunchScreenState::HeroFocused, "HeroFocused")
        .addState(LaunchScreenState::RecentShelf, "RecentShelf")
        .addState(LaunchScreenState::LibraryShelf, "LibraryShelf");

    m_launchFsm
        .addTransition(LaunchScreenState::HeroFocused, LaunchScreenEvent::FocusRecent, LaunchScreenState::RecentShelf)
        .addTransition(LaunchScreenState::HeroFocused, LaunchScreenEvent::FocusLibrary, LaunchScreenState::LibraryShelf)
        .addTransition(LaunchScreenState::RecentShelf, LaunchScreenEvent::FocusHero, LaunchScreenState::HeroFocused)
        .addTransition(LaunchScreenState::RecentShelf, LaunchScreenEvent::FocusLibrary, LaunchScreenState::LibraryShelf)
        .addTransition(LaunchScreenState::LibraryShelf, LaunchScreenEvent::FocusRecent, LaunchScreenState::RecentShelf)
        .addTransition(LaunchScreenState::LibraryShelf, LaunchScreenEvent::FocusHero, LaunchScreenState::HeroFocused);
}

void LaunchScreen::onEnter() {
    StatefulScreen::onEnter();
    evo::animation::AnimationManager::getInstance().setContinuousAnimation(true);
    recent_prune_missing();   // #233: a file deleted outside EVO leaves Jump back in
    /* #229: once per launch, after a crash: where the report of that run is. */
    static bool s_crashNoticeShown = false;
    if (!s_crashNoticeShown && evo_prev_run_state() == EVO_PREV_RUN_CRASHED) {
        s_crashNoticeShown = true;
        EVO_LOGW("app", "the last run crashed; its log is in %s", evo_prev_run_path());
        toast("EVO CLOSED UNEXPECTEDLY", "Settings > System > Report the last run");
    }
    m_selectedRow = (recent_file_count > 0) ? 0 : 2;
    m_selectedCol = 0;
    m_heroSettlePath.clear();
    m_heroSettleMs = 0.0;
    if (m_selectedRow == 0) m_launchFsm.postEvent(LaunchScreenEvent::FocusHero);
    else if (m_selectedRow == 1) m_launchFsm.postEvent(LaunchScreenEvent::FocusRecent);
    else m_launchFsm.postEvent(LaunchScreenEvent::FocusLibrary);
}

void LaunchScreen::onExit() {
    evo::animation::AnimationManager::getInstance().setContinuousAnimation(false);
    StatefulScreen::onExit();
}

std::string LaunchScreen::heroPathForSelection() const {
    /*
     * The hero is the most recently played file and nothing else. It used to
     * follow the recent-shelf cursor and to be overwritten by whichever library
     * tile was hovered, so moving around Home kept rewriting the one panel that
     * is supposed to sit still and say "here is what you were watching".
     */
    if (recent_file_count <= 0) {
        return std::string();
    }
    return std::string(recent_files[0].path);
}

void LaunchScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
    m_launchFsm.update(deltaMs);

    /* Restart the debounce whenever the cursor lands on a different file. */
    std::string heroPath = heroPathForSelection();
    if (heroPath != m_heroSettlePath) {
        m_heroSettlePath = heroPath;
        m_heroSettleMs = 0.0;
    } else if (m_heroSettleMs < HeroSettleMs) {
        m_heroSettleMs += deltaMs;
    }
}

void LaunchScreen::navigate(int dx, int dy) {
    evo::animation::AnimationManager::getInstance().triggerTransition(350.0);
    if (dy < 0) {
        if (m_selectedRow == 2) {
            m_selectedRow = (recent_file_count > 0) ? 1 : 0;
            evo_feedback(EVO_FB_MOVE);
        } else if (m_selectedRow == 1) {
            m_selectedRow = 0;
            evo_feedback(EVO_FB_MOVE);
        } else {
            evo_feedback(EVO_FB_BOUNDARY);
        }
    } else if (dy > 0) {
        if (m_selectedRow == 0) {
            m_selectedRow = (recent_file_count > 0) ? 1 : 2;
            evo_feedback(EVO_FB_MOVE);
        } else if (m_selectedRow == 1) {
            m_selectedRow = 2;
            evo_feedback(EVO_FB_MOVE);
        } else {
            evo_feedback(EVO_FB_BOUNDARY);
        }
    }

    int maxCol = 0;
    if (m_selectedRow == 0) {
        maxCol = 0;
    } else if (m_selectedRow == 1) {
        maxCol = (recent_file_count > 0) ? std::min(recent_file_count - 1, EVO_RMLUI_TILES - 1) : 0;
    } else if (m_selectedRow == 2) {
        maxCol = 5;
    }

    if (dx != 0) {
        int nextCol = m_selectedCol + dx;
        if (nextCol < 0) {
            m_selectedCol = 0;
            evo_feedback(EVO_FB_BOUNDARY);
        } else if (nextCol > maxCol) {
            m_selectedCol = maxCol;
            evo_feedback(EVO_FB_BOUNDARY);
        } else {
            m_selectedCol = nextCol;
            evo_feedback(EVO_FB_MOVE);
        }
    } else {
        if (m_selectedCol > maxCol) m_selectedCol = maxCol;
        if (m_selectedCol < 0) m_selectedCol = 0;
    }
}

void LaunchScreen::activateSelection() {
    auto playback = Application::getInstance().getPlaybackController();
    auto screenMgr = Application::getInstance().getScreenManager();
    if (!playback || !screenMgr) return;

    if (m_selectedRow == 0) {
        // Hero row
        if (recent_file_count > 0) {
            if (!ProviderHostScreen::launchRecent(recent_files[0].path, recent_files[0].title, recent_files[0].last_pos)) {
                if (playback->startPlayback(recent_files[0].path, recent_files[0].last_pos))
                    screenMgr->navigateTo(ScreenId::Player);   // on failure the toast is up and we stay
            }
        } else {
            screenMgr->navigateTo(ScreenId::UsbBrowser);
        }
    } else if (m_selectedRow == 1) {
        // Recent shelf
        if (m_selectedCol >= 0 && m_selectedCol < recent_file_count) {
            if (!ProviderHostScreen::launchRecent(recent_files[m_selectedCol].path, recent_files[m_selectedCol].title, recent_files[m_selectedCol].last_pos)) {
                if (playback->startPlayback(recent_files[m_selectedCol].path, recent_files[m_selectedCol].last_pos))
                    screenMgr->navigateTo(ScreenId::Player);
            }
        }
    } else if (m_selectedRow == 2) {
        // Library shelf navigation. Dispatch on the tile's action rather than
        // its column, so hiding a tile cannot silently shift what the ones
        // after it do.
        if (m_selectedCol < 0 || m_selectedCol >= libraryTileCount())
            return;
        switch (libraryTiles()[m_selectedCol].action) {
            case LIB_BROWSE:
            case LIB_RECENT:
            case LIB_FAVORITES: {
                const int src = (libraryTiles()[m_selectedCol].action == LIB_BROWSE) ? 0
                              : (libraryTiles()[m_selectedCol].action == LIB_RECENT) ? 4
                              : 3;
                if (auto bs = dynamic_cast<BrowserScreen*>(screenMgr->getScreen(ScreenId::UsbBrowser))) {
                    bs->setSource(src);
                }
                screenMgr->navigateTo(ScreenId::UsbBrowser);
                break;
            }
            /* #90: the provider host, not the old Emby setup stub. The rail
             * slot already went here; this tile still pointed at
             * ScreenId::EmbySetup, whose screen is a dead 78-line placeholder
             * that renders one row and calls no emby_* function. Two entry
             * points to one slot, landing on different screens. */
            case LIB_EMBY:     screenMgr->navigateTo(ScreenId::EmbyBrowse); break;
            case LIB_SETTINGS: screenMgr->navigateTo(ScreenId::Settings); break;
            case LIB_ABOUT:    screenMgr->navigateTo(ScreenId::AboutSupport); break;
            default: break;
        }
    }
}

bool LaunchScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    if (pressed & PadButtons::Up) {
        navigate(0, -1);
        return true;
    }
    if (pressed & PadButtons::Down) {
        navigate(0, 1);
        return true;
    }
    if (pressed & PadButtons::Left) {
        if (m_selectedCol <= 0) {
            if (auto sm = Application::getInstance().getScreenManager()) {
                sm->setRailFocused(true);
                return true;
            }
        }
        navigate(-1, 0);
        return true;
    }
    if (pressed & PadButtons::Right) {
        navigate(1, 0);
        return true;
    }
    if (pressed & PadButtons::Cross) {
        evo_feedback(EVO_FB_CONFIRM);
        activateSelection();
        return true;
    }
    if (pressed & PadButtons::Circle) {
        // Switch to USB browser
        evo_feedback(EVO_FB_CANCEL);
        if (auto sm = Application::getInstance().getScreenManager()) {
            sm->navigateTo(ScreenId::UsbBrowser);
        }
        return true;
    }

    return false;
}

void LaunchScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_launch_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.app_name = "EVO PLAYER";
    params.version = (EVO_PLAYER_VERSION[0] == 'v' || EVO_PLAYER_VERSION[0] == 'V')
                     ? EVO_PLAYER_VERSION
                     : ("v" EVO_PLAYER_VERSION);

    // Clock
    char clockBuf[16] = {0};
    time_t now = time(nullptr);
    struct tm tmv;
    if (localtime_r(&now, &tmv)) {
        std::snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        params.clock = clockBuf;
    }

    auto coverService = Application::getInstance().getCoverArtService();
    /* Complete up to two provider artwork decodes per Home frame. */
    evo_provider_art_poll();

    bool railFocused = false;
    if (auto sm = Application::getInstance().getScreenManager()) {
        railFocused = sm->isRailFocused();
    }
    params.hero_focused = (!railFocused && m_selectedRow == 0);

    char safeHeroTitle[128];
    char safeHeroDetail[512];
    char safeRecentTitles[EVO_RMLUI_TILES][128];

    /* A file's name tidied again on the way out, so entries saved before the
     * title cleaner learned release tags read well too. Provider titles are
     * real titles already. */
    auto metaService = Application::getInstance().getMediaMetadataService();
    auto displayTitle = [&](const RecentFileEntry& r) -> std::string {
        if (std::strncmp(r.path, "evo://", 6) != 0 && metaService) {
            std::string t, cat;
            metaService->cleanMediaTitle(r.path, t, cat);
            if (!t.empty()) return t;
        }
        return r.title;
    };

    // Hero - always the most recent file; it does not track the cursor.
    if (recent_file_count > 0) {
        const auto& r = recent_files[0];
        const std::string heroName = displayTitle(r);
        evo_url_sanitize(r.path, safeHeroDetail, sizeof(safeHeroDetail));
        evo_url_sanitize(!heroName.empty() ? heroName.c_str() : safeHeroDetail, safeHeroTitle, sizeof(safeHeroTitle));
        params.hero_eyebrow = (r.last_pos > 1.0) ? "CONTINUE WATCHING" : "START WATCHING";
        /* Say so before Play is pressed, not after a 3 s connect fails. */
        if (NetworkStorageService::serverStatusForUrl(r.path) == 0)
            params.hero_eyebrow = "SERVER OFFLINE";
        params.hero_title = safeHeroTitle;
        params.hero_detail = (std::strncmp(safeHeroDetail, "evo://", 6) == 0) ? "Provider media" : safeHeroDetail;
        params.hero_action = (r.last_pos > 1.0) ? "RESUME" : "PLAY";
        if (r.duration > 1.0) {
            params.hero_progress = static_cast<int>((r.last_pos / r.duration) * 1000.0);
        }
        if (std::strncmp(r.path, "evo://", 6) == 0) {
            int aw = 0, ah = 0;
            params.hero_art = evo_home_art_get(r.path, 1, &aw, &ah);
            params.hero_art_w = aw;
            params.hero_art_h = ah;
        } else if (coverService) {
            /*
             * Only pay for the 960x540 extraction once the cursor has settled
             * (see HeroSettleMs). While it is moving, the shelf's own cached
             * 320x180 poster is the backdrop - it is the same aspect and it is
             * already decoded, so a D-pad press costs nothing.
             */
            if (m_heroSettleMs >= HeroSettleMs) {
                coverService->ensureHeroArt(r.path);
            }

            if (coverService->isHeroArtValid() &&
                coverService->getHeroArtPath() == r.path) {
                params.hero_art = coverService->getHeroArtPixels();
                params.hero_art_w = ICoverArtService::HeroWidth;
                params.hero_art_h = ICoverArtService::HeroHeight;
            } else if (const uint32_t* poster = coverService->peekCoverArt(r.path)) {
                params.hero_art = poster;
                params.hero_art_w = ICoverArtService::PosterWidth;
                params.hero_art_h = ICoverArtService::PosterHeight;
            }
        }
    } else {
        params.hero_eyebrow = "WELCOME";
        params.hero_title = "Welcome to EVO Player";
        params.hero_detail = "Browse storage to select and play media files";
        params.hero_action = "BROWSE USB";
        params.hero_progress = -1;
    }

    // Recent shelf
    params.recent_total = recent_file_count;
    params.recent_cursor = (!railFocused && m_selectedRow == 1) ? m_selectedCol : -1;
    params.recent_visible = (recent_file_count > EVO_RMLUI_TILES) ? EVO_RMLUI_TILES : recent_file_count;

    int coverBudget = 1;
    for (int i = 0; i < params.recent_visible; ++i) {
        const auto& r = recent_files[i];
        char safeRecentPath[512];
        evo_url_sanitize(r.path, safeRecentPath, sizeof(safeRecentPath));
        const std::string name = displayTitle(r);
        evo_url_sanitize(!name.empty() ? name.c_str() : safeRecentPath, safeRecentTitles[i], sizeof(safeRecentTitles[i]));
        params.recent[i].title = safeRecentTitles[i];
        params.recent[i].detail = (std::strncmp(safeRecentPath, "evo://", 6) == 0) ? "Provider media" : "Recent File";
        params.recent[i].icon_path = "../icons/icon_recent_files.png";
        if (std::strncmp(r.path, "evo://", 6) == 0) {      /* no poster: the service's own mark, not a clock */
            static const struct { const char* prefix; const char* icon; } kBrands[] = {
                { "evo://jellyfin/", "../icons/brand_jellyfin.png" }, { "evo://emby/", "../icons/brand_emby.png" },
                { "evo://plex/", "../icons/brand_plex.png" },         { "evo://nuvio/", "../icons/brand_nuvio.png" },
                { "evo://stremio/", "../icons/brand_stremio.png" },   { "evo://addons/", "../icons/brand_stremio.png" },
                { "evo://iptv/", "../icons/icon_tv.png" },
                { "evo://xtream/", "../icons/icon_tv.png" },
            };
            for (const auto& b : kBrands)
                if (std::strncmp(r.path, b.prefix, std::strlen(b.prefix)) == 0) params.recent[i].icon_path = b.icon;
        }
        if (r.duration > 1.0) {
            params.recent[i].progress = static_cast<int>((r.last_pos / r.duration) * 1000.0);
        } else {
            params.recent[i].progress = -1;
        }

        const uint32_t* art = nullptr;
        if (std::strncmp(r.path, "evo://", 6) == 0) {
            int aw = 0, ah = 0;
            art = evo_home_art_get(r.path, 0, &aw, &ah);
            params.recent[i].art_w = aw;
            params.recent[i].art_h = ah;
        } else if (coverService) {
            art = coverService->peekCoverArt(r.path);
            bool tried = coverService->hasTriedCoverArt(r.path);
            if (!art && !tried && coverBudget > 0) {
                art = coverService->getCoverArt(r.path, false);
                coverBudget--;
            }
        }
        if (art) {
            params.recent[i].art = art;
        }
        if (std::strncmp(r.path, "evo://", 6) != 0) {
            params.recent[i].art_w = ICoverArtService::PosterWidth;
            params.recent[i].art_h = ICoverArtService::PosterHeight;
        }
        params.recent[i].is_focused = (!railFocused && m_selectedRow == 1 && m_selectedCol == i);
    }

    // Library shelf (System sections). One table rather than parallel arrays,
    // so a disabled feature drops out of the shelf and out of its activation
    // switch together - see libraryTiles() and EVO_ENABLE_EMBY.
    const LibTile* lib = libraryTiles();
    params.library_visible = libraryTileCount();

    for (int i = 0; i < params.library_visible; ++i) {
        params.library[i].title = lib[i].title;
        params.library[i].detail = lib[i].detail;
        params.library[i].icon_path = lib[i].icon;
        params.library[i].progress = -1;
        params.library[i].art = nullptr;
        params.library[i].is_focused = (!railFocused && m_selectedRow == 2 && m_selectedCol == i);
    }

    fillHomeTips(params);

    evo_rmlui_update_launch(&params);
    evo_rmlui_render_launch(framebuffer, width, height);
}

/* "Did you know" at the foot of Home: what EVO can do that is easy to miss.
 * The phone-page tips name this console's real address, and drop out when
 * the phone & log server has no address or port. */
void LaunchScreen::fillHomeTips(evo_rmlui_launch_params_t& params) {
    static std::string s_tips[EVO_RMLUI_HOME_TIPS];
    char ip[32] = {0};
    evo_cast_receiver_get_local_ip(ip, sizeof(ip));
    const int port = evo_log_server_port();
    const bool web = ip[0] && std::strcmp(ip, "0.0.0.0") != 0 && port > 0;
    const std::string addr = web ? std::string(ip) + ":" + std::to_string(port) : std::string();

    int n = 0;
    auto add = [&](const std::string& t) { if (n < EVO_RMLUI_HOME_TIPS) s_tips[n++] = t; };
    if (web) add("Your phone can be the remote: open " + addr + "/remote");
    if (web) add("Paste a YouTube link and EVO plays the whole video: " + addr + "/input");
    add("Cast from the YouTube app: pick EVO Player in the cast list");
    add("Stereo can fill four speakers: Settings > Audio > Upmix stereo to 4.0");
    add("Phone calls in films can play on the controller speaker: Settings > Audio");
    add("Watch from a PC or NAS: Browse > Add Server (SMB or FTP)");
    add("Jellyfin, Emby, Plex, Stremio and IPTV are under Providers");
    add("Subtitles from any folder: press Down in the player, then Load from file");
    add("Skip intros and credits: press Triangle when the Skip button shows");
    add("Upscaling sharpens SD and HD video on a 4K TV: Settings > Video & Display");
    if (web) add("Live playback health on your phone or PC: " + addr + "/log");
    add("EVO crashed? Settings > System & Diagnostics > Report the last run");

    params.tip_count = n;
    for (int i = 0; i < n; ++i)
        params.tips[i] = s_tips[i].c_str();
}

} // namespace evo
