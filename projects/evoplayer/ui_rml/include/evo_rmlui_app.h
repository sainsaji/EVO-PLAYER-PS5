#pragma once
#include <RmlUi/Core.h>
#include <string>
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

class EvoRmlApp {
public:
    static EvoRmlApp& Instance();

    bool Initialize(int width, int height);
    void Shutdown();

    void UpdatePlaybackState(const EvoPlaybackState& state);
    void RenderPlaybackOSD(uint32_t* framebuffer, int width, int height);

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

    EvoPlaybackState m_last_state;
};
