#include "evo/screens/AudioTrackPickerScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_toast.h"
#include "evo_nav.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace evo {

AudioTrackPickerScreen::AudioTrackPickerScreen()
    : StatefulScreen("AudioTrackPickerScreen") {
}

void AudioTrackPickerScreen::onEnter() {
    StatefulScreen::onEnter();
    refreshTracks();
    m_selectedIndex = m_activeIndex;
    m_scrollOffset = 0;
    if (m_selectedIndex >= EVO_RMLUI_LIST_ROWS) {
        m_scrollOffset = m_selectedIndex - EVO_RMLUI_LIST_ROWS + 1;
    }
}

void AudioTrackPickerScreen::onExit() {
    StatefulScreen::onExit();
}

void AudioTrackPickerScreen::refreshTracks() {
    m_tracks.clear();
    m_activeIndex = 0;

    auto playback = Application::getInstance().getPlaybackController();
    if (!playback) return;

    int active = playback->getActiveAudioStream();
    for (const auto& t : playback->getAudioTracks()) {
        Entry e;
        e.streamIndex = t.streamIndex;
        e.label = t.language.empty() ? "UNKNOWN" : t.language;
        if (!t.title.empty()) {
            e.label = t.title;
        }

        char detail[96];
        std::snprintf(detail, sizeof(detail), "%s  %d CH  %d Hz",
                      t.codecName.c_str(), t.channels, t.sampleRate);
        e.detail = detail;

        char badge[16];
        if (t.channels > 6)      std::snprintf(badge, sizeof(badge), "7.1");
        else if (t.channels > 2) std::snprintf(badge, sizeof(badge), "5.1");
        else if (t.channels == 2)std::snprintf(badge, sizeof(badge), "STEREO");
        else                     std::snprintf(badge, sizeof(badge), "MONO");
        e.badge = badge;

        if (t.streamIndex == active) {
            m_activeIndex = static_cast<int>(m_tracks.size());
        }
        m_tracks.push_back(std::move(e));
    }
}

void AudioTrackPickerScreen::navigate(int delta) {
    if (m_tracks.empty()) return;
    int n = static_cast<int>(m_tracks.size());
    int next = m_selectedIndex + delta;
    if (next < 0)      { m_selectedIndex = 0;     evo_feedback(EVO_FB_BOUNDARY); return; }
    if (next >= n)     { m_selectedIndex = n - 1; evo_feedback(EVO_FB_BOUNDARY); return; }
    m_selectedIndex = next;

    if (m_selectedIndex >= m_scrollOffset + EVO_RMLUI_LIST_ROWS) {
        m_scrollOffset = m_selectedIndex - EVO_RMLUI_LIST_ROWS + 1;
    } else if (m_selectedIndex < m_scrollOffset) {
        m_scrollOffset = m_selectedIndex;
    }

    evo_feedback(EVO_FB_MOVE);
}

void AudioTrackPickerScreen::activateSelection() {
    auto sm = Application::getInstance().getScreenManager();
    auto playback = Application::getInstance().getPlaybackController();
    if (!playback || m_tracks.empty() ||
        m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_tracks.size())) {
        return;
    }

    const Entry& e = m_tracks[m_selectedIndex];
    evo_feedback(EVO_FB_CONFIRM);

    /* Already the live track: nothing to reopen for. */
    if (e.streamIndex != playback->getActiveAudioStream()) {
        if (playback->switchAudioTrack(e.streamIndex)) {
            toast("AUDIO TRACK", e.label.c_str());
        } else {
            toast("AUDIO TRACK", "SWITCH FAILED");
        }
    }

    if (sm) {
        if (!sm->navigateBack()) {
            sm->navigateTo(ScreenId::Player);
        }
    }
}

bool AudioTrackPickerScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    if (pressed & PadButtons::Up)    { navigate(-1); return true; }
    if (pressed & PadButtons::Down)  { navigate(1);  return true; }
    if (pressed & PadButtons::Cross) { activateSelection(); return true; }
    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto sm = Application::getInstance().getScreenManager()) {
            if (!sm->navigateBack()) {
                sm->navigateTo(ScreenId::Player);
            }
        }
        return true;
    }
    return false;
}

void AudioTrackPickerScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
}

void AudioTrackPickerScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_list_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.title = "AUDIO TRACKS";
    params.subtitle = "Select the audio stream for this file";
    params.section = -1;          /* not a rail destination */
    params.rail_focused = 0;

    int total = static_cast<int>(m_tracks.size());
    params.total_count = total;
    params.cursor_index = total ? m_selectedIndex : -1;

    if (total == 0) {
        params.is_empty = 1;
        params.empty_title = "No audio tracks";
        params.empty_hint = "This file has no decodable audio stream.";
        params.empty_icon = "../icons/icon_folder.png";
    } else {
        int rowsToDisplay = std::min(EVO_RMLUI_LIST_ROWS, std::max(0, total - m_scrollOffset));
        params.row_count = rowsToDisplay;
        for (int i = 0; i < rowsToDisplay; ++i) {
            int idx = m_scrollOffset + i;
            params.rows[i].title = m_tracks[idx].label.c_str();
            params.rows[i].detail = m_tracks[idx].detail.c_str();
            params.rows[i].badge = m_tracks[idx].badge.c_str();
            params.rows[i].icon_path = "../icons/icon_aspect.png";
            params.rows[i].progress = -1;
            params.rows[i].has_chevron = 0;
            params.rows[i].is_focused = (idx == m_selectedIndex);
        }
    }

    params.hint_count = 2;
    params.hints[0].glyph_path = "../icons/btn_cross.png";
    params.hints[0].label = "SELECT";
    params.hints[1].glyph_path = "../icons/btn_circle.png";
    params.hints[1].label = "BACK";

    evo_rmlui_update_list(&params);
    evo_rmlui_render_list(framebuffer, width, height);
}

} // namespace evo
