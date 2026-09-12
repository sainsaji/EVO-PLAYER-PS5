#include "evo/screens/LaunchScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_recent.h"
#include "evo_favorites.h"
#include "evo_feedback.h"
#include "evo_nav.h"

#include "evo_boot_trace.h"
#include "evo_boot_log.h"
#include "evo/animation/AnimationManager.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>

#include <algorithm>

namespace evo {

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
    m_selectedRow = (recent_file_count > 0) ? 0 : 2;
    m_selectedCol = 0;
    if (m_selectedRow == 0) m_launchFsm.postEvent(LaunchScreenEvent::FocusHero);
    else if (m_selectedRow == 1) m_launchFsm.postEvent(LaunchScreenEvent::FocusRecent);
    else m_launchFsm.postEvent(LaunchScreenEvent::FocusLibrary);
}

void LaunchScreen::onExit() {
    evo::animation::AnimationManager::getInstance().setContinuousAnimation(false);
    StatefulScreen::onExit();
}

void LaunchScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
    m_launchFsm.update(deltaMs);
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
            playback->startPlayback(recent_files[0].path, recent_files[0].last_pos);
            screenMgr->navigateTo(ScreenId::Player);
        } else {
            screenMgr->navigateTo(ScreenId::UsbBrowser);
        }
    } else if (m_selectedRow == 1) {
        // Recent shelf
        if (m_selectedCol >= 0 && m_selectedCol < recent_file_count) {
            playback->startPlayback(recent_files[m_selectedCol].path, recent_files[m_selectedCol].last_pos);
            screenMgr->navigateTo(ScreenId::Player);
        }
    } else if (m_selectedRow == 2) {
        // Library shelf navigation
        switch (m_selectedCol) {
            case 0: screenMgr->navigateTo(ScreenId::UsbBrowser); break;
            case 1: screenMgr->navigateTo(ScreenId::RecentFiles); break;
            case 2: screenMgr->navigateTo(ScreenId::Favorites); break;
            case 3: screenMgr->navigateTo(ScreenId::EmbySetup); break;
            case 4: screenMgr->navigateTo(ScreenId::Settings); break;
            case 5: screenMgr->navigateTo(ScreenId::AboutSupport); break;
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
    params.version = "VERSION " EVO_PLAYER_VERSION;

    // Clock
    char clockBuf[16] = {0};
    time_t now = time(nullptr);
    struct tm tmv;
    if (localtime_r(&now, &tmv)) {
        std::snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        params.clock = clockBuf;
    }

    auto coverService = Application::getInstance().getCoverArtService();

    bool railFocused = false;
    if (auto sm = Application::getInstance().getScreenManager()) {
        railFocused = sm->isRailFocused();
    }
    params.hero_focused = (!railFocused && m_selectedRow == 0);

    // Hero
    if (recent_file_count > 0) {
        int heroIdx = 0;
        if (!railFocused && m_selectedRow == 1 && m_selectedCol >= 0 && m_selectedCol < recent_file_count) {
            heroIdx = m_selectedCol;
        }
        const auto& r = recent_files[heroIdx];
        params.hero_eyebrow = (r.last_pos > 1.0) ? "CONTINUE WATCHING" : "START WATCHING";
        params.hero_title = r.title[0] ? r.title : r.path;
        params.hero_detail = r.path;
        params.hero_action = (r.last_pos > 1.0) ? "RESUME" : "PLAY";
        if (r.duration > 1.0) {
            params.hero_progress = static_cast<int>((r.last_pos / r.duration) * 1000.0);
        }
        if (coverService) {
            coverService->ensureHeroArt(r.path);
            if (coverService->isHeroArtValid()) {
                params.hero_art = coverService->getHeroArtPixels();
                params.hero_art_w = ICoverArtService::HeroWidth;
                params.hero_art_h = ICoverArtService::HeroHeight;
            }
        }
    } else {
        params.hero_eyebrow = "WELCOME";
        params.hero_title = "Welcome to EVO Player";
        params.hero_detail = "Browse storage to select and play media files";
        params.hero_action = "BROWSE USB";
        params.hero_progress = -1;
    }

    // If hovering library shelf, preview library destination in hero
    if (!railFocused && m_selectedRow == 2 && m_selectedCol >= 0 && m_selectedCol < 6) {
        const evo_section_info* info = evo_section_get(static_cast<evo_section>(m_selectedCol + 1));
        if (info) {
            params.hero_eyebrow = "LIBRARY";
            params.hero_title = info->label;
            params.hero_detail = info->blurb;
            params.hero_action = "OPEN";
            params.hero_progress = -1;
        }
    }

    // Recent shelf
    params.recent_total = recent_file_count;
    params.recent_cursor = (!railFocused && m_selectedRow == 1) ? m_selectedCol : -1;
    params.recent_visible = (recent_file_count > EVO_RMLUI_TILES) ? EVO_RMLUI_TILES : recent_file_count;

    int coverBudget = 1;
    for (int i = 0; i < params.recent_visible; ++i) {
        const auto& r = recent_files[i];
        params.recent[i].title = r.title[0] ? r.title : r.path;
        params.recent[i].detail = "Recent File";
        params.recent[i].icon_path = "../icons/icon_recent_files.png";
        if (r.duration > 1.0) {
            params.recent[i].progress = static_cast<int>((r.last_pos / r.duration) * 1000.0);
        } else {
            params.recent[i].progress = -1;
        }

        const uint32_t* art = nullptr;
        if (coverService) {
            art = coverService->peekCoverArt(r.path);
            bool tried = coverService->hasTriedCoverArt(r.path);
            if (!art && !tried && coverBudget > 0) {
                art = coverService->getCoverArt(r.path, false);
                coverBudget--;
            }
        }
        params.recent[i].art = art;
        params.recent[i].art_w = ICoverArtService::PosterWidth;
        params.recent[i].art_h = ICoverArtService::PosterHeight;
        params.recent[i].is_focused = (!railFocused && m_selectedRow == 1 && m_selectedCol == i);
    }

    // Library shelf (System sections)
    static const char* libIcons[EVO_SECTION_COUNT - 1] = {
        "../icons/icon_browse_usb.png",
        "../icons/icon_recent_files.png",
        "../icons/icon_favorites.png",
        "../icons/icon_emby.png",
        "../icons/icon_settings.png",
        "../icons/icon_about_support.png"
    };

    params.library_visible = EVO_SECTION_COUNT - 1;
    if (params.library_visible > EVO_RMLUI_TILES) {
        params.library_visible = EVO_RMLUI_TILES;
    }

    for (int i = 0; i < params.library_visible; ++i) {
        const evo_section_info* info = evo_section_get(static_cast<evo_section>(i + 1));
        params.library[i].title = info ? info->label : "";
        params.library[i].detail = info ? info->blurb : "";
        params.library[i].icon_path = libIcons[i];
        params.library[i].progress = -1;
        params.library[i].art = nullptr;
        params.library[i].is_focused = (!railFocused && m_selectedRow == 2 && m_selectedCol == i);
    }

    evo_rmlui_update_launch(&params);
    evo_rmlui_render_launch(framebuffer, width, height);
}

} // namespace evo
