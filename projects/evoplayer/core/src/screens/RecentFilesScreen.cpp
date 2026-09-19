#include "evo/screens/RecentFilesScreen.hpp"
#include "evo/screens/BrowserScreen.hpp"
#include "evo/Application.hpp"
#include "evo_recent.h"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_toast.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace evo {

RecentFilesScreen::RecentFilesScreen()
    : StatefulScreen("RecentFilesScreen") {
}

void RecentFilesScreen::onEnter() {
    StatefulScreen::onEnter();
    if (auto sm = Application::getInstance().getScreenManager()) {
        if (auto bs = dynamic_cast<BrowserScreen*>(sm->getScreen(ScreenId::UsbBrowser))) {
            bs->setSource(3);
        }
        sm->navigateTo(ScreenId::UsbBrowser);
    }
}

void RecentFilesScreen::onExit() {
    StatefulScreen::onExit();
}

bool RecentFilesScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
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
        if (m_selectedIndex < recent_file_count - 1) {
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
        if (m_selectedIndex >= 0 && m_selectedIndex < recent_file_count) {
            const auto& r = recent_files[m_selectedIndex];
            evo_feedback(EVO_FB_CONFIRM);
            if (auto pb = Application::getInstance().getPlaybackController()) {
                pb->startPlayback(r.path, r.last_pos);
            }
            if (auto sm = Application::getInstance().getScreenManager()) {
                sm->navigateTo(ScreenId::Player);
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

void RecentFilesScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
}

void RecentFilesScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_list_params_t params;
    std::memset(&params, 0, sizeof(params));

    bool railFocused = false;
    if (auto sm = Application::getInstance().getScreenManager()) {
        railFocused = sm->isRailFocused();
    }

    params.title = "RECENT FILES";
    params.subtitle = "PICK UP WHERE YOU LEFT OFF";
    params.total_count = recent_file_count;
    params.cursor_index = m_selectedIndex;
    params.is_empty = (recent_file_count == 0);
    params.empty_title = "NO RECENT FILES";
    params.empty_hint = "Files you play will appear here";
    params.rail_focused = railFocused ? 1 : 0;

    int rowsToDisplay = std::min(EVO_RMLUI_LIST_ROWS, std::max(0, recent_file_count - m_scrollOffset));
    params.row_count = rowsToDisplay;

    for (int i = 0; i < rowsToDisplay; ++i) {
        int idx = m_scrollOffset + i;
        const auto& r = recent_files[idx];
        params.rows[i].title = r.title[0] ? r.title : r.path;
        params.rows[i].detail = r.path;
        params.rows[i].icon_path = "../icons/icon_recent_files.png";
        params.rows[i].badge = "RECENT";
        if (r.duration > 1.0) {
            params.rows[i].progress = static_cast<int>((r.last_pos / r.duration) * 1000.0);
        } else {
            params.rows[i].progress = -1;
        }
        params.rows[i].is_focused = (!railFocused && idx == m_selectedIndex);
    }

    evo_rmlui_update_list(&params);
    evo_rmlui_render_list(framebuffer, width, height);
}

} // namespace evo
