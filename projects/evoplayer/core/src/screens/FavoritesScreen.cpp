#include "evo/screens/FavoritesScreen.hpp"
#include "evo/screens/BrowserScreen.hpp"
#include "evo/Application.hpp"
#include "evo_favorites.h"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_toast.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace evo {

FavoritesScreen::FavoritesScreen()
    : StatefulScreen("FavoritesScreen") {
}

void FavoritesScreen::onEnter() {
    StatefulScreen::onEnter();
    if (auto sm = Application::getInstance().getScreenManager()) {
        if (auto bs = dynamic_cast<BrowserScreen*>(sm->getScreen(ScreenId::UsbBrowser))) {
            bs->setSource(2);
        }
        sm->navigateTo(ScreenId::UsbBrowser);
    }
}

void FavoritesScreen::onExit() {
    StatefulScreen::onExit();
}

bool FavoritesScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    if (pressed & PadButtons::Up) {
        if (m_selectedIndex > 0) {
            m_selectedIndex--;
            if (m_selectedIndex < m_scrollOffset) {
                m_scrollOffset = m_selectedIndex;
            }
            evo_feedback(EVO_FB_MOVE);
        } else {
            evo_feedback(EVO_FB_BOUNDARY);
        }
        return true;
    }

    if (pressed & PadButtons::Down) {
        if (m_selectedIndex < favorite_count - 1) {
            m_selectedIndex++;
            if (m_selectedIndex >= m_scrollOffset + EVO_RMLUI_LIST_ROWS) {
                m_scrollOffset = m_selectedIndex - EVO_RMLUI_LIST_ROWS + 1;
            }
            evo_feedback(EVO_FB_MOVE);
        } else {
            evo_feedback(EVO_FB_BOUNDARY);
        }
        return true;
    }

    if (pressed & PadButtons::Left) {
        if (auto sm = Application::getInstance().getScreenManager()) {
            sm->setRailFocused(true);
            return true;
        }
    }

    if (pressed & PadButtons::Cross) {
        if (m_selectedIndex >= 0 && m_selectedIndex < favorite_count) {
            const auto& f = favorite_files[m_selectedIndex];
            evo_feedback(EVO_FB_CONFIRM);
            if (auto pb = Application::getInstance().getPlaybackController()) {
                pb->startPlayback(f.path, 0.0);
            }
            if (auto sm = Application::getInstance().getScreenManager()) {
                sm->navigateTo(ScreenId::Player);
            }
        }
        return true;
    }

    if (pressed & (PadButtons::Square | PadButtons::Triangle)) {
        if (m_selectedIndex >= 0 && m_selectedIndex < favorite_count) {
            favorites_remove(favorite_files[m_selectedIndex].path);
            favorites_save();
            toast("FAVORITES", "Removed");
            evo_feedback(EVO_FB_CANCEL);
            if (m_selectedIndex >= favorite_count && m_selectedIndex > 0) {
                m_selectedIndex--;
            }
        }
        return true;
    }

    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto sm = Application::getInstance().getScreenManager()) {
            sm->navigateTo(ScreenId::MainMenu);
        }
        return true;
    }

    return false;
}

void FavoritesScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
}

void FavoritesScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_list_params_t params;
    std::memset(&params, 0, sizeof(params));

    bool railFocused = false;
    if (auto sm = Application::getInstance().getScreenManager()) {
        railFocused = sm->isRailFocused();
    }

    params.title = "FAVORITES";
    params.subtitle = "MEDIA YOU SAVED FOR LATER";
    params.total_count = favorite_count;
    params.cursor_index = m_selectedIndex;
    params.is_empty = (favorite_count == 0);
    params.empty_title = "NO FAVORITES YET";
    params.empty_hint = "Press Triangle in the browser to add favorites";
    params.rail_focused = railFocused ? 1 : 0;

    int rowsToDisplay = std::min(EVO_RMLUI_LIST_ROWS, std::max(0, favorite_count - m_scrollOffset));
    params.row_count = rowsToDisplay;

    for (int i = 0; i < rowsToDisplay; ++i) {
        int idx = m_scrollOffset + i;
        const auto& f = favorite_files[idx];
        params.rows[i].title = f.title[0] ? f.title : f.path;
        params.rows[i].detail = f.path;
        params.rows[i].icon_path = "../icons/icon_favorites.png";
        params.rows[i].badge = "FAVORITE";
        params.rows[i].progress = -1;
        params.rows[i].is_focused = (!railFocused && idx == m_selectedIndex);
    }

    evo_rmlui_update_list(&params);
    evo_rmlui_render_list(framebuffer, width, height);
}

} // namespace evo
