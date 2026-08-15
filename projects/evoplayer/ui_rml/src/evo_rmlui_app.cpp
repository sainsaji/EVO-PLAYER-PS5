#include "evo_rmlui_app.h"
#include <iostream>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cmath>

EvoRmlApp& EvoRmlApp::Instance() {
    static EvoRmlApp instance;
    return instance;
}

EvoRmlApp::EvoRmlApp() {
}

EvoRmlApp::~EvoRmlApp() {
    Shutdown();
}

static std::string format_time(double seconds) {
    if (seconds < 0) seconds = 0;
    int s = (int)std::floor(seconds);
    int h = s / 3600;
    int m = (s % 3600) / 60;
    int sec = s % 60;
    std::ostringstream oss;
    if (h > 0) {
        oss << std::setfill('0') << std::setw(2) << h << ":"
            << std::setfill('0') << std::setw(2) << m << ":"
            << std::setfill('0') << std::setw(2) << sec;
    } else {
        oss << std::setfill('0') << std::setw(2) << m << ":"
            << std::setfill('0') << std::setw(2) << sec;
    }
    return oss.str();
}

bool EvoRmlApp::Initialize(int width, int height) {
    if (m_initialized) return true;

    m_width = width;
    m_height = height;

    m_system = std::make_unique<EvoSystemInterface>();
    m_render = std::make_unique<EvoRenderInterface>(width, height);

    Rml::SetSystemInterface(m_system.get());
    Rml::SetRenderInterface(m_render.get());

    if (!Rml::Initialise()) {
        std::cerr << "[EVO RmlUi] Failed to initialise RmlUi core!" << std::endl;
        return false;
    }

    std::vector<std::string> font_prefixes = {
        "/data/evoplayer/app/assets/fonts/",
        "/data/homebrew/EVOPlayer/assets/fonts/",
        "/app0/assets/fonts/",
        "assets/fonts/",
        "projects/evoplayer/assets/fonts/",
        "/workspace/projects/evoplayer/assets/fonts/"
    };

    for (const auto& p : font_prefixes) {
        if (Rml::LoadFontFace(p + "LatoLatin-Regular.ttf", true)) {
            Rml::LoadFontFace(p + "LatoLatin-Bold.ttf", true);
            Rml::LoadFontFace(p + "Roboto-Regular.ttf", true);
            Rml::LoadFontFace(p + "Roboto-Bold.ttf", true);
            Rml::LoadFontFace(p + "Roboto-Medium.ttf", true);
            std::cout << "[EVO RmlUi] Loaded font face from " << p << std::endl;
            break;
        }
    }

    m_context = Rml::CreateContext("main_context", Rml::Vector2i(width, height));
    if (!m_context) {
        std::cerr << "[EVO RmlUi] Failed to create RmlUi context!" << std::endl;
        Rml::Shutdown();
        return false;
    }

    std::vector<std::string> rml_prefixes = {
        "/data/evoplayer/app/assets/rml/",
        "/data/homebrew/EVOPlayer/assets/rml/",
        "/app0/assets/rml/",
        "assets/rml/",
        "projects/evoplayer/assets/rml/",
        "/workspace/projects/evoplayer/assets/rml/"
    };

    for (const auto& p : rml_prefixes) {
        m_playback_doc = m_context->LoadDocument(p + "playback.rml");
        if (m_playback_doc) {
            m_playback_doc->Show();
            break;
        }
    }

    if (!m_playback_doc) {
        std::cerr << "[EVO RmlUi] Failed to load playback.rml!" << std::endl;
    }

    m_initialized = true;
    std::cout << "[EVO RmlUi] Retained-mode Playback Engine initialized successfully ("
              << width << "x" << height << ")." << std::endl;
    return true;
}

void EvoRmlApp::Shutdown() {
    if (!m_initialized) return;

    if (m_playback_doc) {
        m_playback_doc->Close();
        m_playback_doc = nullptr;
    }

    if (m_context) {
        Rml::RemoveContext(m_context->GetName());
        m_context = nullptr;
    }

    Rml::Shutdown();

    m_render.reset();
    m_system.reset();
    m_initialized = false;
}

void EvoRmlApp::UpdatePlaybackState(const EvoPlaybackState& state) {
    if (!m_initialized || !m_playback_doc) return;

    m_last_state = state;

    // 1. Title & Meta
    Rml::Element* el_title = m_playback_doc->GetElementById("media-title");
    if (el_title) {
        el_title->SetInnerRML(state.title.empty() ? "Video Playback" : state.title);
    }

    Rml::Element* el_meta = m_playback_doc->GetElementById("media-meta");
    if (el_meta) {
        if (state.meta.empty()) {
            el_meta->SetProperty("display", "none");
        } else {
            el_meta->SetProperty("display", "block");
            el_meta->SetInnerRML(state.meta);
        }
    }

    // 2. Badges (Resolution, HDR, Codec, FPS)
    Rml::Element* el_res = m_playback_doc->GetElementById("badge-res");
    if (el_res) {
        if (state.res_badge.empty()) {
            el_res->SetProperty("display", "none");
        } else {
            el_res->SetProperty("display", "inline-block");
            el_res->SetInnerRML(state.res_badge);
        }
    }

    Rml::Element* el_hdr = m_playback_doc->GetElementById("badge-hdr");
    if (el_hdr) {
        if (state.hdr_badge.empty()) {
            el_hdr->SetProperty("display", "none");
        } else {
            el_hdr->SetProperty("display", "inline-block");
            el_hdr->SetInnerRML(state.hdr_badge);
        }
    }

    Rml::Element* el_codec = m_playback_doc->GetElementById("badge-codec");
    if (el_codec) {
        if (state.codec_badge.empty()) {
            el_codec->SetProperty("display", "none");
        } else {
            el_codec->SetProperty("display", "inline-block");
            el_codec->SetInnerRML(state.codec_badge);
        }
    }

    Rml::Element* el_fps = m_playback_doc->GetElementById("badge-fps");
    if (el_fps) {
        if (state.fps_badge.empty()) {
            el_fps->SetProperty("display", "none");
        } else {
            el_fps->SetProperty("display", "inline-block");
            el_fps->SetInnerRML(state.fps_badge);
        }
    }

    // 3. Times & Progress
    double cur_pos = state.scrub_active ? state.scrub_target : state.position_sec;
    std::string cur_str = format_time(cur_pos);
    std::string dur_str = format_time(state.duration_sec);
    double remaining = state.duration_sec - cur_pos;
    if (remaining < 0) remaining = 0;
    std::string rem_str = "-" + format_time(remaining);
    std::string time_display = rem_str + " / " + dur_str;

    Rml::Element* el_tcur = m_playback_doc->GetElementById("time-current");
    if (el_tcur) el_tcur->SetInnerRML(cur_str);

    Rml::Element* el_tdur = m_playback_doc->GetElementById("time-duration");
    if (el_tdur) el_tdur->SetInnerRML(time_display);

    double pct = state.percentage * 100.0;
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;

    Rml::Element* el_fill = m_playback_doc->GetElementById("progress-fill");
    if (el_fill) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << pct << "%";
        el_fill->SetProperty("width", ss.str());
    }

    // 4. Scrubbing Capsule
    Rml::Element* el_scrub = m_playback_doc->GetElementById("scrub-capsule");
    if (el_scrub) {
        if (state.scrub_active) {
            el_scrub->SetProperty("display", "flex");
            Rml::Element* el_stime = m_playback_doc->GetElementById("scrub-time");
            if (el_stime) el_stime->SetInnerRML(format_time(state.scrub_target));
        } else {
            el_scrub->SetProperty("display", "none");
        }
    }

    // 5. Play/Pause State (Only show PAUSED badge if paused AND not scrubbing)
    Rml::Element* el_pause = m_playback_doc->GetElementById("pause-badge");
    if (el_pause) {
        if (state.paused && !state.scrub_active) {
            el_pause->SetProperty("display", "flex");
        } else {
            el_pause->SetProperty("display", "none");
        }
    }

    Rml::Element* el_pp_label = m_playback_doc->GetElementById("label-playpause");
    if (el_pp_label) {
        el_pp_label->SetInnerRML(state.paused ? "PLAY" : "PAUSE");
    }

    // 6. Track labels
    Rml::Element* el_audio = m_playback_doc->GetElementById("label-audio");
    if (el_audio) {
        std::string a_txt = "AUDIO: " + (state.audio_track.empty() ? "Stereo" : state.audio_track);
        el_audio->SetInnerRML(a_txt);
    }

    Rml::Element* el_subs = m_playback_doc->GetElementById("label-subs");
    if (el_subs) {
        std::string s_txt = "SUBS: " + (state.sub_track.empty() ? "None" : state.sub_track);
        el_subs->SetInnerRML(s_txt);
    }

    Rml::Element* el_aspect = m_playback_doc->GetElementById("label-aspect");
    if (el_aspect) {
        const char* vm = (state.view_mode == 0) ? "FIT" : ((state.view_mode == 1) ? "FILL" : "STRETCH");
        el_aspect->SetInnerRML(std::string("ASPECT: ") + vm);
    }

    // 7. Stats for Nerds HUD
    Rml::Element* el_stats = m_playback_doc->GetElementById("stats-hud");
    if (el_stats) {
        el_stats->SetProperty("display", state.show_stats ? "flex" : "none");
    }
}

void EvoRmlApp::RenderPlaybackOSD(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_playback_doc || !framebuffer) return;

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);

    m_context->Update();
    m_context->Render();
}
