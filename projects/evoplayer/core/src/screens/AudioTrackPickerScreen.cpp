#include "evo/screens/AudioTrackPickerScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_toast.h"
#include "evo_nav.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <utility>

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
    m_hasQuality = false;

    auto playback = Application::getInstance().getPlaybackController();
    if (!playback) return;

    /*
     * Quality first: an AUTO row and one row per video variant, best first. Only
     * offered when there is a choice - a file with one video stream has nothing
     * to pick and keeps the audio-only list it always had.
     */
    const auto variants = playback->getVideoVariants();
    if (variants.size() > 1) {
        m_hasQuality = true;
        const bool pinned = playback->isVideoQualityPinned();
        const int activeVideo = playback->getActiveVideoStream();

        Entry a;
        a.kind = KindAuto;
        a.streamIndex = -1;
        a.label = "AUTO";
        a.detail = "EVO picks the best quality available";
        a.badge = pinned ? "" : "PLAYING";
        if (!pinned) m_activeIndex = static_cast<int>(m_tracks.size());
        m_tracks.push_back(std::move(a));

        for (const auto& v : variants) {
            Entry e;
            e.kind = KindVideo;
            e.streamIndex = v.streamIndex;

            char label[32];
            std::snprintf(label, sizeof(label), "%dp", v.height);
            e.label = label;

            std::string d;
            char part[48];
            std::snprintf(part, sizeof(part), "%dx%d", v.width, v.height);
            d = part;
            if (!v.codecName.empty()) {
                std::string c = v.codecName;
                std::transform(c.begin(), c.end(), c.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
                d += "  -  " + c;
            }
            if (v.bitrate > 0) {
                std::snprintf(part, sizeof(part), "%.1f Mbps", static_cast<double>(v.bitrate) / 1e6);
                d += "  -  ";
                d += part;
            }
            if (v.fps > 0.5) {
                std::snprintf(part, sizeof(part), "%.0f fps", v.fps);
                d += "  -  ";
                d += part;
            }
            e.detail = d;
            e.badge = (v.streamIndex == activeVideo) ? "PLAYING" : "";

            if (pinned && v.streamIndex == activeVideo)
                m_activeIndex = static_cast<int>(m_tracks.size());
            m_tracks.push_back(std::move(e));
        }
    }

    int active = playback->getActiveAudioStream();
    for (const auto& t : playback->getAudioTracks()) {
        Entry e;
        e.kind = KindAudio;
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
            e.detail += "  -  PLAYING";
            /* With a quality section the cursor starts on the playing quality;
             * without one it starts on the playing audio track, as before. */
            if (!m_hasQuality) m_activeIndex = static_cast<int>(m_tracks.size());
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

    const Entry e = m_tracks[m_selectedIndex];       /* a copy: the reopen rebuilds the list */
    evo_feedback(EVO_FB_CONFIRM);

    if (e.kind == KindAuto) {
        /* Back to EVO's own choice. A no-op when it already is. */
        if (playback->switchVideoVariant(-1)) {
            toast("QUALITY", "AUTO");
        } else {
            toast("QUALITY", "SWITCH FAILED");
        }
    } else if (e.kind == KindVideo) {
        /* Already pinned to this one: nothing to reopen for. */
        if (playback->switchVideoVariant(e.streamIndex)) {
            toast("QUALITY", e.label.c_str());
        } else {
            toast("QUALITY", "SWITCH FAILED");
        }
    } else if (e.streamIndex != playback->getActiveAudioStream()) {
        /* Already the live track: nothing to reopen for. */
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

    params.title = m_hasQuality ? "QUALITY & AUDIO" : "AUDIO TRACKS";
    params.subtitle = m_hasQuality ? "Pick the video quality and the audio track"
                                   : "Select the audio stream for this file";
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
            params.rows[i].icon_path = (m_tracks[idx].kind == KindAudio)
                                     ? "../icons/icon_aspect.png" : "../icons/icon_tv.png";
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
