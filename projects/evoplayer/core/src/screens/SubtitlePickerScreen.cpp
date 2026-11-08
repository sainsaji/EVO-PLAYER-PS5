#include "evo/screens/SubtitlePickerScreen.hpp"
#include "evo/Application.hpp"
#include "evo_subtitle.h"
#include "evo_langname.h"
#include "evo_subsync.h"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_toast.h"
#include "evo_online_subs.h"

extern "C" {
#include <libavformat/avformat.h>
extern AVFormatContext *play_fmt;
}

#include <cstdio>
#include <cstring>
#include <cctype>
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
    if (m_selectedIndex >= kVisibleRows) {
        m_scrollOffset = m_selectedIndex - kVisibleRows + 1;
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

    // 2. External SRT if available (a sidecar, a media server's, or an online one)
    if (prospero_subtitle_count > 0) {
        SubtitleTrackEntry extTrack;
        extTrack.trackId = -1;
        extTrack.label = prospero_subtitle_external_label[0] ? prospero_subtitle_external_label
                                                             : "EXTERNAL SRT";
        char cueBuf[48];
        std::snprintf(cueBuf, sizeof(cueBuf), "%s%d CUES",
                      prospero_subtitle_external_label[0] ? "ONLINE · " : "", prospero_subtitle_count);
        extTrack.detail = cueBuf;
        if (prospero_subtitle_enabled && prospero_subtitle_use_external) {
            m_activeTrackIndex = static_cast<int>(m_tracks.size());
        }
        extTrack.isSecondary = prospero_secondary_use_external && prospero_secondary_subtitle_active();
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

            /* The language's own name ("Русский", "日本語") when it is known. */
            const char* native = (lang && lang->value) ? evo_language_native_name(lang->value) : nullptr;
            std::string label = native ? native : (lang && lang->value) ? lang->value : "Track";
            if (title && title->value) {
                label += " - ";
                label += title->value;
            }

            streamTrack.label = label;
            /* What the track is, so a forced or picture track is not a surprise */
            std::string detail;
            const AVCodecID cid = stream->codecpar->codec_id;
            if (cid == AV_CODEC_ID_HDMV_PGS_SUBTITLE)      detail = "PGS (PICTURE)";
            else if (cid == AV_CODEC_ID_SUBRIP)            detail = "SRT";
            else if (cid == AV_CODEC_ID_ASS || cid == AV_CODEC_ID_SSA) detail = "ASS";
            else if (cid == AV_CODEC_ID_WEBVTT)            detail = "WEBVTT";
            else if (cid == AV_CODEC_ID_MOV_TEXT)          detail = "MP4 TEXT";
            else {
                const char* cn = avcodec_get_name(cid);
                detail = cn ? cn : "EMBEDDED";
                for (auto& ch : detail) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            }
            if (!prospero_embedded_subtitle_supported(cid) && !prospero_embedded_subtitle_is_bitmap(cid))
                detail += " · NOT SUPPORTED";
            if (prospero_subtitle_stream_is_forced(stream))
                detail += " · FORCED (SIGNS ONLY)";
            const int cues = prospero_subtitle_declared_cues(stream);
            if (cues >= 0) {
                char cb[32];
                std::snprintf(cb, sizeof cb, " · %d CUES", cues);
                detail += cb;
            }
            streamTrack.detail = detail;
            streamTrack.isSecondary =
                prospero_secondary_subtitle_stream_index == static_cast<int>(i);

            if (prospero_subtitle_enabled && !prospero_subtitle_use_external &&
                prospero_embedded_subtitle_stream_index == static_cast<int>(i)) {
                m_activeTrackIndex = static_cast<int>(m_tracks.size());
            }

            m_tracks.push_back(std::move(streamTrack));
        }
    }

    // 4. Online subtitles from the Stremio addons for this title (not yet loaded)
    bool onlineForThis = false;
    if (auto pb = Application::getInstance().getPlaybackController()) {
        char key[512];
        evo_osubs_key(key, sizeof key);
        const PlaybackSource& src = pb->getSource();
        onlineForThis = src.isProvider() && !src.item_id.empty() && src.item_id == key;
    }
    if (onlineForThis) {
        const int n = evo_osubs_count();
        const int active = (prospero_subtitle_count > 0 && prospero_subtitle_external_label[0])
                               ? evo_osubs_active() : -1;
        for (int i = 0; i < n && i < kMaxOnlineRows; ++i) {
            if (i == active) continue;          /* it is the external row above */
            evo_osub_t s;
            if (!evo_osubs_get(i, &s)) continue;
            SubtitleTrackEntry row;
            row.trackId = kOnlineBase - i;
            char lang[64];
            evo_osubs_label(i, lang, sizeof lang);
            row.label = lang;
            row.detail = std::string("ONLINE · ") + s.source;
            m_tracks.push_back(std::move(row));
        }
    }

    // AUTO-SYNC (#102): matches the active track - external SRT or an
    // embedded text track - to the audio. Second row, so a long track list
    // never scrolls it away. Its detail is live (progress); render() fills it.
    if (m_tracks.size() > 1) {
        SubtitleTrackEntry syncRow;
        syncRow.trackId = kAutoSyncTrack;
        syncRow.label = "AUTO-SYNC";
        m_tracks.insert(m_tracks.begin() + 1, std::move(syncRow));
        if (m_activeTrackIndex >= 1) {
            ++m_activeTrackIndex;
        }
    }

    // LOAD FROM FILE...: an .srt from anywhere on the console. Last, so the
    // tracks this file already has come first.
    {
        SubtitleTrackEntry loadRow;
        loadRow.trackId = kLoadFileTrack;
        loadRow.label = "LOAD FROM FILE...";
        loadRow.detail = "Pick an .srt from a drive or folder";
        m_tracks.push_back(std::move(loadRow));
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

    if (m_selectedIndex >= m_scrollOffset + kVisibleRows) {
        m_scrollOffset = m_selectedIndex - kVisibleRows + 1;
    } else if (m_selectedIndex < m_scrollOffset) {
        m_scrollOffset = m_selectedIndex;
    }

    evo_feedback(EVO_FB_MOVE);
}

void SubtitlePickerScreen::activateSelection() {
    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_tracks.size())) {
        return;
    }

    int trackId = m_tracks[m_selectedIndex].trackId;

    if (trackId == kLoadFileTrack) {
        evo_feedback(EVO_FB_OPEN);
        if (auto sm = Application::getInstance().getScreenManager()) sm->navigateTo(ScreenId::SubtitleFile);
        return;
    }

    if (trackId == kAutoSyncTrack) {
        // Start, or cancel a run. Stay on the picker so the progress shows;
        // the result is applied by prospero_subtitle_autosync_pump().
        if (!prospero_subtitle_autosync_running() && !prospero_subtitle_autosync_available()) {
            evo_feedback(EVO_FB_ERROR);
            return;
        }
        evo_feedback(EVO_FB_CONFIRM);
        prospero_subtitle_autosync_toggle();
        return;
    }

    evo_feedback(EVO_FB_CONFIRM);
    if (m_selectedIndex != m_activeTrackIndex) {
        // A run measures the active track; switching away ends it.
        evo_subsync_cancel();
    }

    if (trackId <= kOnlineBase) {
        // An online subtitle: fetched off the UI thread, loaded by the player
        // when it lands (PlayerScreen::update -> pumpOnlineSubtitles).
        if (evo_osubs_fetch_async(kOnlineBase - trackId) != 0) {
            evo_feedback(EVO_FB_ERROR);
            toast("SUBTITLES", "A DOWNLOAD IS ALREADY RUNNING");
            return;
        }
        toast("SUBTITLES", "DOWNLOADING...");
    } else if (trackId == -2) {
        // Off
        prospero_subtitle_enabled = 0;
        toast("SUBTITLES", "OFF");
    } else if (!prospero_subtitle_select_primary(trackId)) {
        evo_feedback(EVO_FB_ERROR);
        return;
    } else if (trackId == -1) {
        toast("SUBTITLES", "EXTERNAL SRT");
    }
    // An embedded track toasts its own language and codec as it opens.

    if (auto sm = Application::getInstance().getScreenManager()) {
        if (!sm->navigateBack()) {
            sm->navigateTo(ScreenId::Player);
        }
    }
}

// #110: Square makes the focused track the secondary (the second line of
// dialogue), or clears it when the track already is. Unlike Cross this stays
// on the picker, so the [SECONDARY] tag is seen moving.
void SubtitlePickerScreen::activateSecondary() {
    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_tracks.size())) {
        return;
    }

    const SubtitleTrackEntry& entry = m_tracks[m_selectedIndex];

    if (entry.trackId == kAutoSyncTrack || entry.trackId == kLoadFileTrack ||
        entry.trackId <= kOnlineBase) {
        evo_feedback(EVO_FB_ERROR);   // an online row must be loaded first
        return;
    }

    if (entry.trackId == -2 || entry.isSecondary) {
        // The OFF row, or the secondary itself: clear the second line.
        if (!prospero_secondary_subtitle_active()) {
            evo_feedback(EVO_FB_ERROR);
            return;
        }
        prospero_secondary_subtitle_select(PROSPERO_SECONDARY_NONE);
    } else if (!prospero_secondary_subtitle_select(entry.trackId)) {
        evo_feedback(EVO_FB_ERROR);     // it toasted why
        return;
    }

    evo_feedback(EVO_FB_CONFIRM);
    refreshTracks();
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
    if (prospero_subtitle_face < 1 || prospero_subtitle_face > PROSPERO_SUBTITLE_SIZES)
        prospero_subtitle_face = 2;              /* MEDIUM, the documented default */
    /* 1 -> 2 -> 3 -> 4 -> 1; #231 added EXTRA LARGE. This is for this session
     * only: Settings -> SUBTITLE SIZE is the saved default. */
    prospero_subtitle_face = (prospero_subtitle_face % PROSPERO_SUBTITLE_SIZES) + 1;

    const char* names[] = { "SMALL", "MEDIUM", "LARGE", "EXTRA LARGE" };
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
    if (pressed & PadButtons::Triangle) {
        cycleSize();
        return true;
    }
    if (pressed & PadButtons::Square) {
        activateSecondary();
        return true;
    }
    if (pressed & (PadButtons::L1 | PadButtons::R1)) {
        // The secondary track keeps its own sync (#110).
        if (!prospero_secondary_subtitle_active()) {
            evo_feedback(EVO_FB_ERROR);
            return true;
        }
        prospero_secondary_nudge_delay((pressed & PadButtons::R1) ? +100 : -100);
        evo_feedback(EVO_FB_MOVE);
        return true;
    }
    if (pressed & (PadButtons::Left | PadButtons::TouchPadLeft | PadButtons::L2)) {
        prospero_subtitle_nudge_delay(-100);
        evo_feedback(EVO_FB_MOVE);
        return true;
    }
    if (pressed & (PadButtons::Right | PadButtons::TouchPadRight)) {
        prospero_subtitle_nudge_delay(+100);
        evo_feedback(EVO_FB_MOVE);
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
    const int size_idx = (prospero_subtitle_face >= 1 && prospero_subtitle_face <= PROSPERO_SUBTITLE_SIZES)
                       ? (prospero_subtitle_face - 1) : 1;
    const char* sizeNames[] = { "SMALL", "MEDIUM", "LARGE", "EXTRA LARGE" };
    params.size_str = sizeNames[size_idx];

    static char s_syncBuf[64];
    if (prospero_subtitle_delay_ms == 0) {
        std::snprintf(s_syncBuf, sizeof(s_syncBuf), "SYNC: 0 ms");
    } else {
        std::snprintf(s_syncBuf, sizeof(s_syncBuf), "SYNC: %+d ms", prospero_subtitle_delay_ms);
    }
    if (prospero_subtitle_time_scale != 1.0) {
        // A framerate fix from AUTO-SYNC; the nudge still fine-tunes the offset.
        if (const char* ratio = evo_subsync_ratio_label(prospero_subtitle_time_scale)) {
            size_t len = std::strlen(s_syncBuf);
            std::snprintf(s_syncBuf + len, sizeof(s_syncBuf) - len, "  %s", ratio);
        }
    }
    params.sync_str = s_syncBuf;

    // The secondary track's own sync pill, only while there is one (#110).
    static char s_sync2Buf[32];
    if (prospero_subtitle_enabled && prospero_secondary_subtitle_active()) {
        if (prospero_secondary_delay_ms == 0)
            std::snprintf(s_sync2Buf, sizeof(s_sync2Buf), "2ND: 0 ms");
        else
            std::snprintf(s_sync2Buf, sizeof(s_sync2Buf), "2ND: %+d ms", prospero_secondary_delay_ms);
        params.sync2_str = s_sync2Buf;
    } else {
        params.sync2_str = "";
    }

    static char s_primaryPreview[PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE];
    static char s_secondaryPreview[PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE];
    double currentPos = 0.0;
    auto playback = Application::getInstance().getPlaybackController();
    if (playback) {
        currentPos = playback->getPositionSeconds();
    }
    prospero_subtitle_sample_cue_text(currentPos, s_primaryPreview, sizeof(s_primaryPreview),
                                      s_secondaryPreview, sizeof(s_secondaryPreview));

    params.preview_text = s_primaryPreview;
    params.preview_text2 = s_secondaryPreview;
    params.has_secondary = (prospero_subtitle_enabled && prospero_secondary_subtitle_active()) ? 1 : 0;
    params.secondary_position = prospero_secondary_position;
    params.secondary_color = prospero_secondary_color;
    params.primary_color = prospero_subtitle_color;
    params.preview_face = size_idx;

    int total = static_cast<int>(m_tracks.size());
    int rowsToDisplay = std::min(kVisibleRows, std::max(0, total - m_scrollOffset));
    static char s_autoSyncDetail[48];
    params.track_count = rowsToDisplay;
    for (int i = 0; i < rowsToDisplay; ++i) {
        int idx = m_scrollOffset + i;
        params.tracks[i].label = m_tracks[idx].label.c_str();
        params.tracks[i].detail = m_tracks[idx].detail.c_str();
        params.tracks[i].is_focused = (idx == m_selectedIndex);
        params.tracks[i].is_current = (idx == m_activeTrackIndex);
        // [PRIMARY] / [SECONDARY]: which line of dialogue each track is (#110).
        params.tracks[i].tag = m_tracks[idx].isSecondary ? "SECONDARY"
                             : (idx == m_activeTrackIndex && idx != 0 &&
                                m_tracks[idx].trackId != kAutoSyncTrack) ? "PRIMARY" : "";
        if (m_tracks[idx].trackId == kLoadFileTrack) {
            params.tracks[i].is_current = 0;
            params.tracks[i].is_action = 1;
            params.tracks[i].tag = "";
        }
        if (m_tracks[idx].trackId == kAutoSyncTrack) {
            params.tracks[i].detail = prospero_subtitle_autosync_detail(s_autoSyncDetail, sizeof(s_autoSyncDetail));
            params.tracks[i].is_current = 0;
            params.tracks[i].is_action = 1;
            params.tracks[i].is_disabled = !prospero_subtitle_autosync_running() &&
                                           !prospero_subtitle_autosync_available();
        }
    }

    evo_rmlui_update_subtitles(&params);
    evo_rmlui_render_subtitles(framebuffer, width, height);
}

} // namespace evo
