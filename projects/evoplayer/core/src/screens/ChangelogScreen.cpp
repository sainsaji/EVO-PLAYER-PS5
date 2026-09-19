#include "evo/screens/ChangelogScreen.hpp"
#include "evo/Application.hpp"
#include "evo_changelog.h"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace evo {

ChangelogScreen::ChangelogScreen()
    : StatefulScreen("ChangelogScreen") {
}

void ChangelogScreen::onEnter() {
    StatefulScreen::onEnter();
    m_cursorIndex = 0;
    m_scrollOffset = 0;
}

void ChangelogScreen::onExit() {
    StatefulScreen::onExit();
}

void ChangelogScreen::navigate(int delta) {
    int total = EVO_CHANGELOG_RELEASE_COUNT;
    if (total <= 0) return;

    int next = m_cursorIndex + delta;
    if (next < 0) {
        m_cursorIndex = 0;
        evo_feedback(EVO_FB_BOUNDARY);
    } else if (next >= total) {
        m_cursorIndex = total - 1;
        evo_feedback(EVO_FB_BOUNDARY);
    } else {
        m_cursorIndex = next;
        evo_feedback(EVO_FB_MOVE);
    }

    if (m_cursorIndex < m_scrollOffset) {
        m_scrollOffset = m_cursorIndex;
    } else if (m_cursorIndex >= m_scrollOffset + EVO_RMLUI_CL_RELEASES) {
        m_scrollOffset = m_cursorIndex - EVO_RMLUI_CL_RELEASES + 1;
    }
}

bool ChangelogScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    if (pressed & PadButtons::Up) {
        navigate(-1);
        return true;
    }
    if (pressed & PadButtons::Down) {
        navigate(1);
        return true;
    }
    if (pressed & (PadButtons::Left | PadButtons::Circle | PadButtons::Cross)) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto sm = Application::getInstance().getScreenManager()) {
            if (!sm->navigateBack()) {
                sm->navigateTo(ScreenId::AboutSupport);
            }
        }
        return true;
    }

    return false;
}

void ChangelogScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
}

void ChangelogScreen::render(uint32_t* framebuffer, int width, int height) {
    static const char* kindLabel[] = {
        "NEW", "FIXED", "IMPROVED", "REMOVED", "VERSION"
    };

    evo_rmlui_changelog_params_t p;
    std::memset(&p, 0, sizeof(p));

    p.title = "CHANGELOG";
    p.subtitle = "WHAT CHANGED IN EACH RELEASE";
    p.rail_focused = 0;
    p.release_total = EVO_CHANGELOG_RELEASE_COUNT;
    p.cursor_index = m_cursorIndex;

    int cap = EVO_RMLUI_CL_RELEASES;
    for (int i = 0; i < cap; ++i) {
        int idx = m_scrollOffset + i;
        if (idx >= EVO_CHANGELOG_RELEASE_COUNT) break;
        p.releases[i].version = EVO_CHANGELOG_RELEASES[idx].version;
        p.releases[i].tagline = EVO_CHANGELOG_RELEASES[idx].tagline;
        p.releases[i].date = EVO_CHANGELOG_RELEASES[idx].date;
        p.releases[i].is_focused = (idx == m_cursorIndex);
        p.release_count++;
    }

    if (m_cursorIndex >= 0 && m_cursorIndex < EVO_CHANGELOG_RELEASE_COUNT) {
        const auto& curRelease = EVO_CHANGELOG_RELEASES[m_cursorIndex];
        p.detail_version = curRelease.version;
        p.detail_tagline = curRelease.tagline;
        p.item_total = curRelease.item_count;

        for (int i = 0; i < curRelease.item_count && i < EVO_RMLUI_CL_ITEMS; ++i) {
            int kind = static_cast<int>(curRelease.items[i].kind);
            if (kind < 0 || kind > 4) kind = 0;
            p.items[i].kind = kindLabel[kind];
            p.items[i].text = curRelease.items[i].text;
            p.item_count++;
        }
    }

    evo_rmlui_update_changelog(&p);
    evo_rmlui_render_changelog(framebuffer, width, height);
}

} // namespace evo
