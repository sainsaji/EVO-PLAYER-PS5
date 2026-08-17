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

static std::string to_hex_rgb(uint32_t col) {
    uint8_t r = col & 0xFF;
    uint8_t g = (col >> 8) & 0xFF;
    uint8_t b = (col >> 16) & 0xFF;
    char buf[16];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
    return std::string(buf);
}

static std::string to_hex_rgba(uint32_t col) {
    uint8_t r = col & 0xFF;
    uint8_t g = (col >> 8) & 0xFF;
    uint8_t b = (col >> 16) & 0xFF;
    uint8_t a = (col >> 24) & 0xFF;
    char buf[16];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", r, g, b, a);
    return std::string(buf);
}

void EvoRmlApp::SetTheme(const EvoThemeColors& theme) {
    m_theme = theme;
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
        if (!m_playback_doc) {
            m_playback_doc = m_context->LoadDocument(p + "playback.rml");
            if (m_playback_doc) m_playback_doc->Hide();
        }
        if (!m_dialog_doc) {
            m_dialog_doc = m_context->LoadDocument(p + "dialog.rml");
            if (m_dialog_doc) m_dialog_doc->Hide();
        }
        if (!m_settings_doc) {
            m_settings_doc = m_context->LoadDocument(p + "settings.rml");
            if (m_settings_doc) m_settings_doc->Hide();
        }
        if (!m_subtitles_doc) {
            m_subtitles_doc = m_context->LoadDocument(p + "subtitles.rml");
            if (m_subtitles_doc) m_subtitles_doc->Hide();
        }
        if (!m_mediainfo_doc) {
            m_mediainfo_doc = m_context->LoadDocument(p + "mediainfo.rml");
            if (m_mediainfo_doc) m_mediainfo_doc->Hide();
        }
    }

    if (!m_playback_doc) std::cerr << "[EVO RmlUi] Failed to load playback.rml!" << std::endl;
    if (!m_dialog_doc) std::cerr << "[EVO RmlUi] Failed to load dialog.rml!" << std::endl;
    if (!m_settings_doc) std::cerr << "[EVO RmlUi] Failed to load settings.rml!" << std::endl;
    if (!m_subtitles_doc) std::cerr << "[EVO RmlUi] Failed to load subtitles.rml!" << std::endl;
    if (!m_mediainfo_doc) std::cerr << "[EVO RmlUi] Failed to load mediainfo.rml!" << std::endl;

    m_initialized = true;
    std::cout << "[EVO RmlUi] Retained-mode Full Engine initialized successfully ("
              << width << "x" << height << ")." << std::endl;
    return true;
}

void EvoRmlApp::Shutdown() {
    if (!m_initialized) return;

    if (m_mediainfo_doc) {
        m_mediainfo_doc->Close();
        m_mediainfo_doc = nullptr;
    }

    if (m_subtitles_doc) {
        m_subtitles_doc->Close();
        m_subtitles_doc = nullptr;
    }

    if (m_settings_doc) {
        m_settings_doc->Close();
        m_settings_doc = nullptr;
    }

    if (m_dialog_doc) {
        m_dialog_doc->Close();
        m_dialog_doc = nullptr;
    }

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
        el_fill->SetProperty("background-color", to_hex_rgb(m_theme.accent));
    }

    Rml::Element* el_thumb = m_playback_doc->GetElementById("progress-thumb");
    if (el_thumb) {
        el_thumb->SetProperty("border-color", to_hex_rgb(m_theme.accent));
    }

    // 4. Scrubbing Capsule
    Rml::Element* el_scrub = m_playback_doc->GetElementById("scrub-capsule");
    if (el_scrub) {
        el_scrub->SetProperty("border-color", to_hex_rgb(m_theme.accent));
        if (state.scrub_active) {
            el_scrub->SetProperty("display", "flex");
            Rml::Element* el_stime = m_playback_doc->GetElementById("scrub-time");
            if (el_stime) el_stime->SetInnerRML(format_time(state.scrub_target));
        } else {
            el_scrub->SetProperty("display", "none");
        }
    }

    Rml::Element* el_scrub_lbl = m_playback_doc->GetElementById("scrub-label");
    if (el_scrub_lbl) {
        el_scrub_lbl->SetProperty("color", to_hex_rgb(m_theme.accent));
    }

    // 5. Play/Pause State
    Rml::Element* el_pause = m_playback_doc->GetElementById("pause-badge");
    if (el_pause) {
        el_pause->SetProperty("background-color", to_hex_rgb(m_theme.accent));
        el_pause->SetProperty("border-color", to_hex_rgb(m_theme.border_sel));
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

    if (m_dialog_doc) m_dialog_doc->Hide();
    if (m_settings_doc) m_settings_doc->Hide();
    if (m_subtitles_doc) m_subtitles_doc->Hide();
    if (m_mediainfo_doc) m_mediainfo_doc->Hide();
    m_playback_doc->Show();

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);

    m_context->Update();
    m_context->Render();
}

void EvoRmlApp::UpdateDialogState(const EvoDialogState& state) {
    if (!m_initialized || !m_dialog_doc) return;

    m_last_dialog = state;

    Rml::Element* el_eb = m_dialog_doc->GetElementById("dialog-eyebrow");
    if (el_eb) el_eb->SetInnerRML(state.eyebrow);

    Rml::Element* el_ti = m_dialog_doc->GetElementById("dialog-title");
    if (el_ti) el_ti->SetInnerRML(state.title.empty() ? "Confirmation" : state.title);

    Rml::Element* el_de = m_dialog_doc->GetElementById("dialog-detail");
    if (el_de) el_de->SetInnerRML(state.detail);

    Rml::Element* el_track = m_dialog_doc->GetElementById("dialog-progress-track");
    Rml::Element* el_fill = m_dialog_doc->GetElementById("dialog-progress-fill");
    if (el_track && el_fill) {
        if (state.progress_pct >= 0.0) {
            el_track->SetProperty("display", "block");
            double pct = state.progress_pct * 100.0;
            if (pct < 0.0) pct = 0.0;
            if (pct > 100.0) pct = 100.0;
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(1) << pct << "%";
            el_fill->SetProperty("width", ss.str());
            el_fill->SetProperty("background-color", to_hex_rgb(m_theme.accent));
        } else {
            el_track->SetProperty("display", "none");
        }
    }

    for (int i = 0; i < 3; i++) {
        std::string btn_id = "action-" + std::to_string(i);
        std::string icon_id = "action-icon-" + std::to_string(i);
        std::string label_id = "action-label-" + std::to_string(i);

        Rml::Element* el_btn = m_dialog_doc->GetElementById(btn_id);
        Rml::Element* el_icon = m_dialog_doc->GetElementById(icon_id);
        Rml::Element* el_lbl = m_dialog_doc->GetElementById(label_id);

        if (el_btn) {
            if (i < (int)state.actions.size()) {
                el_btn->SetProperty("display", "flex");
                el_btn->SetClass("btn-primary", state.actions[i].is_primary);
                el_btn->SetClass("btn-secondary", !state.actions[i].is_primary);

                if (state.actions[i].is_primary) {
                    el_btn->SetProperty("background-color", to_hex_rgb(m_theme.accent));
                    el_btn->SetProperty("border-color", to_hex_rgb(m_theme.border_sel));
                    el_btn->SetProperty("color", "#ffffff");
                } else {
                    el_btn->SetProperty("background-color", to_hex_rgba(m_theme.surface));
                    el_btn->SetProperty("border-color", to_hex_rgba(m_theme.border));
                    el_btn->SetProperty("color", "#e2e8f0");
                }

                if (el_icon) el_icon->SetAttribute("src", state.actions[i].icon_path);
                if (el_lbl) el_lbl->SetInnerRML(state.actions[i].label);
            } else {
                el_btn->SetProperty("display", "none");
            }
        }
    }
}

void EvoRmlApp::RenderDialog(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_dialog_doc || !framebuffer) return;

    if (m_playback_doc) m_playback_doc->Hide();
    if (m_settings_doc) m_settings_doc->Hide();
    if (m_subtitles_doc) m_subtitles_doc->Hide();
    if (m_mediainfo_doc) m_mediainfo_doc->Hide();
    m_dialog_doc->Show();

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);

    m_context->Update();
    m_context->Render();
}

void EvoRmlApp::UpdateSettingsState(const EvoSettingsState& state) {
    if (!m_initialized || !m_settings_doc) return;

    m_last_settings = state;

    Rml::Element* el_ti = m_settings_doc->GetElementById("settings-title");
    if (el_ti) el_ti->SetInnerRML(state.title);

    Rml::Element* el_sub = m_settings_doc->GetElementById("settings-subtitle");
    if (el_sub) el_sub->SetInnerRML(state.subtitle);

    Rml::Element* el_cnt = m_settings_doc->GetElementById("settings-counter");
    if (el_cnt) el_cnt->SetInnerRML(state.counter);

    Rml::Element* el_ind = m_settings_doc->GetElementById("header-indicator");
    if (el_ind) {
        el_ind->SetProperty("background-color", to_hex_rgb(m_theme.accent));
    }

    for (int r = 0; r < 7; r++) {
        std::string rid = "rail-" + std::to_string(r);
        Rml::Element* el_r = m_settings_doc->GetElementById(rid);
        if (el_r) {
            bool is_active = (r == state.rail_active_idx);
            bool is_focused = is_active && state.rail_focused;
            el_r->SetClass("rail-active", is_active);
            el_r->SetClass("rail-focused", is_focused);

            if (is_focused) {
                el_r->SetProperty("background-color", to_hex_rgb(m_theme.accent));
                el_r->SetProperty("border-color", "#ffffff");
            } else if (is_active) {
                el_r->SetProperty("background-color", to_hex_rgba(m_theme.surface_sel));
                el_r->SetProperty("border-color", to_hex_rgb(m_theme.border_sel));
            } else {
                el_r->SetProperty("background-color", "transparent");
                el_r->SetProperty("border-color", "transparent");
            }
        }
    }

    for (int i = 0; i < 6; i++) {
        std::string row_id = "row-" + std::to_string(i);
        std::string icon_id = "row-icon-" + std::to_string(i);
        std::string title_id = "row-title-" + std::to_string(i);
        std::string detail_id = "row-detail-" + std::to_string(i);
        std::string badge_id = "row-badge-" + std::to_string(i);
        std::string chev_id = "row-chevron-" + std::to_string(i);

        Rml::Element* el_row = m_settings_doc->GetElementById(row_id);
        Rml::Element* el_icon = m_settings_doc->GetElementById(icon_id);
        Rml::Element* el_title = m_settings_doc->GetElementById(title_id);
        Rml::Element* el_detail = m_settings_doc->GetElementById(detail_id);
        Rml::Element* el_badge = m_settings_doc->GetElementById(badge_id);
        Rml::Element* el_chev = m_settings_doc->GetElementById(chev_id);

        if (el_row) {
            if (i < (int)state.rows.size()) {
                el_row->SetProperty("display", "flex");
                bool is_focused = state.rows[i].is_focused && !state.rail_focused;
                el_row->SetClass("row-focused", is_focused);

                if (is_focused) {
                    el_row->SetProperty("background-color", to_hex_rgba(m_theme.surface_sel));
                    el_row->SetProperty("border-color", to_hex_rgb(m_theme.accent));
                    if (el_badge) {
                        el_badge->SetProperty("background-color", to_hex_rgb(m_theme.accent));
                        el_badge->SetProperty("border-color", "#ffffff");
                        el_badge->SetProperty("color", "#ffffff");
                    }
                } else {
                    el_row->SetProperty("background-color", to_hex_rgba(m_theme.surface));
                    el_row->SetProperty("border-color", to_hex_rgba(m_theme.border));
                    if (el_badge) {
                        el_badge->SetProperty("background-color", to_hex_rgba(m_theme.surface_sel));
                        el_badge->SetProperty("border-color", to_hex_rgb(m_theme.border_sel));
                        el_badge->SetProperty("color", to_hex_rgb(m_theme.accent));
                    }
                }

                if (el_icon) {
                    el_icon->SetAttribute("src", state.rows[i].icon_path.empty() ? "../icons/icon_settings.png" : state.rows[i].icon_path);
                }
                if (el_title) el_title->SetInnerRML(state.rows[i].title);
                if (el_detail) el_detail->SetInnerRML(state.rows[i].detail);
                if (el_badge) {
                    if (state.rows[i].badge.empty()) {
                        el_badge->SetProperty("display", "none");
                    } else {
                        el_badge->SetProperty("display", "inline-block");
                        el_badge->SetInnerRML(state.rows[i].badge);
                    }
                }
                if (el_chev) {
                    el_chev->SetProperty("display", state.rows[i].has_chevron ? "inline-block" : "none");
                }
            } else {
                el_row->SetProperty("display", "none");
            }
        }
    }
}

void EvoRmlApp::RenderSettings(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_settings_doc || !framebuffer) return;

    if (m_playback_doc) m_playback_doc->Hide();
    if (m_dialog_doc) m_dialog_doc->Hide();
    if (m_subtitles_doc) m_subtitles_doc->Hide();
    if (m_mediainfo_doc) m_mediainfo_doc->Hide();
    m_settings_doc->Show();

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);

    m_context->Update();
    m_context->Render();
}

void EvoRmlApp::UpdateSubtitlesState(const EvoSubtitlesState& state) {
    if (!m_initialized || !m_subtitles_doc) return;

    m_last_subtitles = state;

    Rml::Element* el_eb = m_subtitles_doc->GetElementById("subtitles-eyebrow");
    if (el_eb) el_eb->SetInnerRML(state.eyebrow);

    Rml::Element* el_ti = m_subtitles_doc->GetElementById("subtitles-title");
    if (el_ti) el_ti->SetInnerRML(state.title);

    Rml::Element* el_pill = m_subtitles_doc->GetElementById("subtitles-size-pill");
    if (el_pill) {
        el_pill->SetProperty("border-color", to_hex_rgb(m_theme.border_sel));
    }

    Rml::Element* el_sz = m_subtitles_doc->GetElementById("subtitles-size-label");
    if (el_sz) {
        el_sz->SetInnerRML(std::string("SIZE: ") + state.size_str);
        el_sz->SetProperty("color", to_hex_rgb(m_theme.accent));
    }

    Rml::Element* el_pv = m_subtitles_doc->GetElementById("subtitles-preview-text");
    if (el_pv) {
        el_pv->SetInnerRML(state.preview_text.empty() ? "Welcome to EVO Player on PlayStation 5" : state.preview_text);
        el_pv->SetClass("preview-small", state.preview_face == 0);
        el_pv->SetClass("preview-medium", state.preview_face == 1);
        el_pv->SetClass("preview-large", state.preview_face == 2);
    }

    for (int i = 0; i < 6; i++) {
        std::string row_id = "sub-row-" + std::to_string(i);
        std::string chk_id = "sub-check-" + std::to_string(i);
        std::string dot_id = "sub-dot-" + std::to_string(i);
        std::string title_id = "sub-title-" + std::to_string(i);
        std::string detail_id = "sub-detail-" + std::to_string(i);

        Rml::Element* el_row = m_subtitles_doc->GetElementById(row_id);
        Rml::Element* el_chk = m_subtitles_doc->GetElementById(chk_id);
        Rml::Element* el_dot = m_subtitles_doc->GetElementById(dot_id);
        Rml::Element* el_title = m_subtitles_doc->GetElementById(title_id);
        Rml::Element* el_detail = m_subtitles_doc->GetElementById(detail_id);

        if (el_row) {
            if (i < (int)state.tracks.size()) {
                el_row->SetProperty("display", "flex");
                el_row->SetClass("row-focused", state.tracks[i].is_focused);

                if (state.tracks[i].is_focused) {
                    el_row->SetProperty("background-color", to_hex_rgba(m_theme.surface_sel));
                    el_row->SetProperty("border-color", to_hex_rgb(m_theme.accent));
                } else {
                    el_row->SetProperty("background-color", to_hex_rgba(m_theme.surface));
                    el_row->SetProperty("border-color", to_hex_rgba(m_theme.border));
                }

                if (el_chk) {
                    el_chk->SetClass("checked", state.tracks[i].is_current);
                    el_chk->SetProperty("border-color", state.tracks[i].is_current ? to_hex_rgb(m_theme.accent) : to_hex_rgba(m_theme.border));
                }
                if (el_dot) {
                    el_dot->SetProperty("background-color", state.tracks[i].is_current ? to_hex_rgb(m_theme.accent) : "transparent");
                }
                if (el_title) el_title->SetInnerRML(state.tracks[i].label);
                if (el_detail) {
                    if (state.tracks[i].detail.empty()) {
                        el_detail->SetProperty("display", "none");
                    } else {
                        el_detail->SetProperty("display", "inline-block");
                        el_detail->SetInnerRML(state.tracks[i].detail);
                        if (state.tracks[i].is_focused) {
                            el_detail->SetProperty("background-color", to_hex_rgb(m_theme.accent));
                            el_detail->SetProperty("border-color", "#ffffff");
                            el_detail->SetProperty("color", "#ffffff");
                        } else {
                            el_detail->SetProperty("background-color", to_hex_rgba(m_theme.surface_sel));
                            el_detail->SetProperty("border-color", to_hex_rgb(m_theme.border_sel));
                            el_detail->SetProperty("color", to_hex_rgb(m_theme.accent));
                        }
                    }
                }
            } else {
                el_row->SetProperty("display", "none");
            }
        }
    }
}

void EvoRmlApp::RenderSubtitles(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_subtitles_doc || !framebuffer) return;

    if (m_playback_doc) m_playback_doc->Hide();
    if (m_dialog_doc) m_dialog_doc->Hide();
    if (m_settings_doc) m_settings_doc->Hide();
    if (m_mediainfo_doc) m_mediainfo_doc->Hide();
    m_subtitles_doc->Show();

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);

    m_context->Update();
    m_context->Render();
}

void EvoRmlApp::UpdateMediaInfoState(const EvoMediaInfoState& state) {
    if (!m_initialized || !m_mediainfo_doc) return;

    m_last_mediainfo = state;

    Rml::Element* el_ind = m_mediainfo_doc->GetElementById("mediainfo-indicator");
    if (el_ind) {
        el_ind->SetProperty("background-color", to_hex_rgb(m_theme.accent));
    }

    Rml::Element* el_ti = m_mediainfo_doc->GetElementById("mediainfo-title");
    if (el_ti) el_ti->SetInnerRML(state.title.empty() ? "Media Details" : state.title);

    Rml::Element* el_pa = m_mediainfo_doc->GetElementById("mediainfo-path");
    if (el_pa) el_pa->SetInnerRML(state.path);

    // Badges
    Rml::Element* el_res = m_mediainfo_doc->GetElementById("info-badge-res");
    if (el_res) {
        if (state.res_badge.empty()) el_res->SetProperty("display", "none");
        else {
            el_res->SetProperty("display", "inline-block");
            el_res->SetInnerRML(state.res_badge);
            el_res->SetProperty("border-color", to_hex_rgb(m_theme.border_sel));
            el_res->SetProperty("color", to_hex_rgb(m_theme.accent));
        }
    }

    Rml::Element* el_hdr = m_mediainfo_doc->GetElementById("info-badge-hdr");
    if (el_hdr) {
        if (state.hdr_badge.empty()) el_hdr->SetProperty("display", "none");
        else {
            el_hdr->SetProperty("display", "inline-block");
            el_hdr->SetInnerRML(state.hdr_badge);
            el_hdr->SetProperty("border-color", "#ffd700");
            el_hdr->SetProperty("color", "#ffd700");
        }
    }

    Rml::Element* el_codec = m_mediainfo_doc->GetElementById("info-badge-codec");
    if (el_codec) {
        if (state.codec_badge.empty()) el_codec->SetProperty("display", "none");
        else {
            el_codec->SetProperty("display", "inline-block");
            el_codec->SetInnerRML(state.codec_badge);
            el_codec->SetProperty("border-color", to_hex_rgb(m_theme.border_sel));
            el_codec->SetProperty("color", to_hex_rgb(m_theme.accent));
        }
    }

    Rml::Element* el_fps = m_mediainfo_doc->GetElementById("info-badge-fps");
    if (el_fps) {
        if (state.fps_badge.empty()) el_fps->SetProperty("display", "none");
        else {
            el_fps->SetProperty("display", "inline-block");
            el_fps->SetInnerRML(state.fps_badge);
            el_fps->SetProperty("border-color", to_hex_rgb(m_theme.border_sel));
            el_fps->SetProperty("color", to_hex_rgb(m_theme.accent));
        }
    }

    // Specs
    Rml::Element* el_con = m_mediainfo_doc->GetElementById("spec-container");
    if (el_con) el_con->SetInnerRML(state.container);

    Rml::Element* el_sz = m_mediainfo_doc->GetElementById("spec-size");
    if (el_sz) el_sz->SetInnerRML(state.file_size);

    Rml::Element* el_du = m_mediainfo_doc->GetElementById("spec-duration");
    if (el_du) el_du->SetInnerRML(state.duration);

    Rml::Element* el_vc = m_mediainfo_doc->GetElementById("spec-vcodec");
    if (el_vc) el_vc->SetInnerRML(state.video_codec);

    Rml::Element* el_rs = m_mediainfo_doc->GetElementById("spec-res");
    if (el_rs) el_rs->SetInnerRML(state.resolution);

    Rml::Element* el_hd = m_mediainfo_doc->GetElementById("spec-hdr");
    if (el_hd) el_hd->SetInnerRML(state.color_hdr);

    Rml::Element* el_ac = m_mediainfo_doc->GetElementById("spec-acodec");
    if (el_ac) el_ac->SetInnerRML(state.audio_codec);

    Rml::Element* el_ch = m_mediainfo_doc->GetElementById("spec-channels");
    if (el_ch) el_ch->SetInnerRML(state.channels);

    Rml::Element* el_rt = m_mediainfo_doc->GetElementById("spec-rate");
    if (el_rt) el_rt->SetInnerRML(state.sample_rate);

    Rml::Element* el_su = m_mediainfo_doc->GetElementById("spec-subs");
    if (el_su) el_su->SetInnerRML(state.subtitles);

    Rml::Element* el_ou = m_mediainfo_doc->GetElementById("spec-output");
    if (el_ou) el_ou->SetInnerRML(state.output);

    Rml::Element* el_rn = m_mediainfo_doc->GetElementById("spec-renderer");
    if (el_rn) el_rn->SetInnerRML(state.renderer);
}

void EvoRmlApp::RenderMediaInfo(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_mediainfo_doc || !framebuffer) return;

    if (m_playback_doc) m_playback_doc->Hide();
    if (m_dialog_doc) m_dialog_doc->Hide();
    if (m_settings_doc) m_settings_doc->Hide();
    if (m_subtitles_doc) m_subtitles_doc->Hide();
    m_mediainfo_doc->Show();

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);

    m_context->Update();
    m_context->Render();
}
