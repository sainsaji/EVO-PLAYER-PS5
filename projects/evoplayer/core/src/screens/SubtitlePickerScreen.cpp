#include "evo/screens/SubtitlePickerScreen.hpp"
#include "evo/Application.hpp"
#include "evo_subtitle.h"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_toast.h"

extern "C" {
#include <libavformat/avformat.h>
extern AVFormatContext *play_fmt;
}

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace evo {

SubtitlePickerScreen::SubtitlePickerScreen()
    : StatefulScreen("SubtitlePickerScreen") {
}

void SubtitlePickerScreen::onEnter() {
    StatefulScreen::onEnter();
    refreshTracks();
    m_selectedIndex = m_activeTrackIndex;
    m_scrollOffset = 0;
    if (m_selectedIndex >= 8) {
        m_scrollOffset = m_selectedIndex - 8 + 1;
    }
}

void SubtitlePickerScreen::onExit() {
    StatefulScreen::onExit();
}

void SubtitlePickerScreen::refreshTracks() {
    m_tracks.clear();
    m_activeTrackIndex = 0;

    // 1. Subtitles OFF
    SubtitleTrackEntry offTrack;
    offTrack.trackId = -2;
    offTrack.label = "SUBTITLES OFF";
    offTrack.detail = "Disable subtitles";
    m_tracks.push_back(std::move(offTrack));

    // 2. External SRT if available
    if (prospero_subtitle_count > 0) {
        SubtitleTrackEntry extTrack;
        extTrack.trackId = -1;
        extTrack.label = "EXTERNAL SRT";
        char cueBuf[32];
        std::snprintf(cueBuf, sizeof(cueBuf), "%d CUES", prospero_subtitle_count);
        extTrack.detail = cueBuf;
        if (prospero_subtitle_enabled && prospero_subtitle_use_external) {
            m_activeTrackIndex = static_cast<int>(m_tracks.size());
        }
        m_tracks.push_back(std::move(extTrack));
    }

    // 3. Embedded subtitle streams
    if (play_fmt) {
        for (unsigned int i = 0; i < play_fmt->nb_streams; ++i) {
            AVStream* stream = play_fmt->streams[i];
            if (!stream || !stream->codecpar || stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
                continue;
            }

            SubtitleTrackEntry streamTrack;
            streamTrack.trackId = static_cast<int>(i);

            const AVDictionaryEntry* lang = av_dict_get(stream->metadata, "language", nullptr, 0);
            const AVDictionaryEntry* title = av_dict_get(stream->metadata, "title", nullptr, 0);

            std::string label = (lang && lang->value) ? lang->value : "Track";
            if (title && title->value) {
                label += " - ";
                label += title->value;
            }

            streamTrack.label = label;
            streamTrack.detail = "Embedded Stream";

            if (prospero_subtitle_enabled && !prospero_subtitle_use_external &&
                prospero_embedded_subtitle_stream_index == static_cast<int>(i)) {
                m_activeTrackIndex = static_cast<int>(m_tracks.size());
            }

            m_tracks.push_back(std::move(streamTrack));
        }
    }

    if (!prospero_subtitle_enabled) {
        m_activeTrackIndex = 0;
    }
}

void SubtitlePickerScreen::navigate(int delta) {
    if (m_tracks.empty()) return;
    int count = static_cast<int>(m_tracks.size());
    int next = m_selectedIndex + delta;
    if (next < 0) {
        m_selectedIndex = 0;
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    } else if (next >= count) {
        m_selectedIndex = count - 1;
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    }
    m_selectedIndex = next;

    if (m_selectedIndex >= m_scrollOffset + 8) {
        m_scrollOffset = m_selectedIndex - 8 + 1;
    } else if (m_selectedIndex < m_scrollOffset) {
        m_scrollOffset = m_selectedIndex;
    }

    evo_feedback(EVO_FB_MOVE);
}

void SubtitlePickerScreen::activateSelection() {
    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_tracks.size())) {
        return;
    }

    evo_feedback(EVO_FB_CONFIRM);
    int trackId = m_tracks[m_selectedIndex].trackId;

    if (trackId == -2) {
        // Off
        prospero_subtitle_enabled = 0;
        toast("SUBTITLES", "OFF");
    } else if (trackId == -1) {
        // External
        prospero_subtitle_enabled = 1;
        prospero_subtitle_use_external = 1;
        toast("SUBTITLES", "EXTERNAL SRT");
    } else {
        // Embedded
        prospero_subtitle_enabled = 1;
        prospero_subtitle_use_external = 0;
        prospero_embedded_subtitle_stream_index = trackId;
        toast("SUBTITLES", m_tracks[m_selectedIndex].label.c_str());
    }

    if (auto sm = Application::getInstance().getScreenManager()) {
        if (!sm->navigateBack()) {
            sm->navigateTo(ScreenId::Player);
        }
    }
}

void SubtitlePickerScreen::cycleSize() {
    /*
     * prospero_subtitle_face is 1=SMALL, 2=MEDIUM, 3=LARGE - see its
     * declaration in evo_subtitle.c, and the classes the player applies:
     * subtitle_face == 1/2/3 -> sub-small/sub-medium/sub-large.
     *
     * This cycled with (face + 1) % 3, which produces 0, 1 and 2. So LARGE was
     * unreachable, and face 0 matched no class at all and fell through to
     * #subtitle-box's own font-size - which is 40dp, exactly what sub-medium
     * sets. Two of the three stops therefore rendered identically and the
     * third was the only one that did anything, which is why the size looked
     * like it never changed.
     */
    if (prospero_subtitle_face < 1 || prospero_subtitle_face > 3)
        prospero_subtitle_face = 2;              /* MEDIUM, the documented default */
    prospero_subtitle_face = (prospero_subtitle_face % 3) + 1;   /* 1 -> 2 -> 3 -> 1 */

    const char* names[] = { "SMALL", "MEDIUM", "LARGE" };
    toast("SUBTITLE SIZE", names[prospero_subtitle_face - 1]);
}

bool SubtitlePickerScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
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
    if (pressed & PadButtons::Cross) {
        activateSelection();
        return true;
    }
    if (pressed & (PadButtons::Triangle | PadButtons::Square)) {
        cycleSize();
        return true;
    }
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

void SubtitlePickerScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
}

void SubtitlePickerScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_subtitles_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.eyebrow = "SUBTITLE CONFIGURATION";
    params.title = "SUBTITLE TRACKS";
    /* The picker's own preview classes are 0/1/2 (preview-small/medium/large)
     * while prospero_subtitle_face is 1/2/3, so this is the one place the two
     * conventions meet and the conversion has to be explicit. Both used to be
     * "% 3", which made the label disagree with the size actually applied. */
    const int size_idx = (prospero_subtitle_face >= 1 && prospero_subtitle_face <= 3)
                       ? (prospero_subtitle_face - 1) : 1;
    const char* sizeNames[] = { "SMALL", "MEDIUM", "LARGE" };
    params.size_str = sizeNames[size_idx];
    params.preview_text = "The quick brown fox jumps over the lazy dog";
    params.preview_face = size_idx;

    int total = static_cast<int>(m_tracks.size());
    int rowsToDisplay = std::min(8, std::max(0, total - m_scrollOffset));
    params.track_count = rowsToDisplay;
    for (int i = 0; i < rowsToDisplay; ++i) {
        int idx = m_scrollOffset + i;
        params.tracks[i].label = m_tracks[idx].label.c_str();
        params.tracks[i].detail = m_tracks[idx].detail.c_str();
        params.tracks[i].is_focused = (idx == m_selectedIndex);
        params.tracks[i].is_current = (idx == m_activeTrackIndex);
    }

    evo_rmlui_update_subtitles(&params);
    evo_rmlui_render_subtitles(framebuffer, width, height);
}

} // namespace evo
