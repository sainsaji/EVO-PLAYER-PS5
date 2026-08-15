#pragma once
#include <RmlUi/Core.h>
#include <string>
#include <vector>
#include <memory>
#include "evo_rmlui_render.h"
#include "evo_rmlui_system.h"

struct EvoPlaybackState {
    std::string title;
    std::string meta;
    std::string res_badge;
    std::string hdr_badge;
    std::string codec_badge;
    std::string fps_badge;
    std::string audio_badge;
    double position_sec = 0.0;
    double duration_sec = 0.0;
    double percentage = 0.0;
    bool paused = false;
    bool scrub_active = false;
    double scrub_target = 0.0;
    std::string audio_track;
    std::string sub_track;
    int view_mode = 0; // 0=FIT, 1=FILL, 2=STRETCH
    bool show_stats = false;
    int alpha = 255;
};

struct EvoDialogAction {
    std::string icon_path;
    std::string label;
    bool is_primary = false;
};

struct EvoDialogState {
    std::string eyebrow;
    std::string title;
    std::string detail;
    double progress_pct = -1.0; // 0.0 to 1.0, or -1.0 to hide
    std::vector<EvoDialogAction> actions;
};

struct EvoSettingsRow {
    std::string title;
    std::string detail;
    std::string icon_path;
    std::string badge;
    bool has_chevron = true;
    bool is_focused = false;
};

struct EvoSettingsState {
    std::string title;
    std::string subtitle;
    std::string counter;
    int rail_active_idx = 5;
    bool rail_focused = false;
    std::vector<EvoSettingsRow> rows;
};

struct EvoSubtitlesTrack {
    std::string label;
    std::string detail;
    bool is_current = false;
    bool is_focused = false;
};

struct EvoSubtitlesState {
    std::string eyebrow;
    std::string title;
    std::string size_str;
    std::string preview_text;
    int preview_face = 1; // 0=small, 1=medium, 2=large
    std::vector<EvoSubtitlesTrack> tracks;
};

class EvoRmlApp {
public:
    static EvoRmlApp& Instance();

    bool Initialize(int width, int height);
    void Shutdown();

    void UpdatePlaybackState(const EvoPlaybackState& state);
    void RenderPlaybackOSD(uint32_t* framebuffer, int width, int height);

    void UpdateDialogState(const EvoDialogState& state);
    void RenderDialog(uint32_t* framebuffer, int width, int height);

    void UpdateSettingsState(const EvoSettingsState& state);
    void RenderSettings(uint32_t* framebuffer, int width, int height);

    void UpdateSubtitlesState(const EvoSubtitlesState& state);
    void RenderSubtitles(uint32_t* framebuffer, int width, int height);

    bool IsInitialized() const { return m_initialized; }

private:
    EvoRmlApp();
    ~EvoRmlApp();

    bool m_initialized = false;
    int m_width = 1920;
    int m_height = 1080;

    std::unique_ptr<EvoSystemInterface> m_system;
    std::unique_ptr<EvoRenderInterface> m_render;
    Rml::Context* m_context = nullptr;
    Rml::ElementDocument* m_playback_doc = nullptr;
    Rml::ElementDocument* m_dialog_doc = nullptr;
    Rml::ElementDocument* m_settings_doc = nullptr;
    Rml::ElementDocument* m_subtitles_doc = nullptr;

    EvoPlaybackState m_last_state;
    EvoDialogState m_last_dialog;
    EvoSettingsState m_last_settings;
    EvoSubtitlesState m_last_subtitles;
};
