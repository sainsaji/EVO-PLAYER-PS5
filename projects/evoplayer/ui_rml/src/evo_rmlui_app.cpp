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
    m_theme_generation++;
}

void EvoRmlApp::SetImageColor(Rml::Element* el, const std::string& color) {
    if (!el) return;
    auto it = m_image_color_cache.find(el);
    if (it != m_image_color_cache.end() && it->second == color) return;
    el->SetProperty("image-color", color);
    m_image_color_cache[el] = color;
}

bool EvoRmlApp::ShouldFullRender(int screen_id) {
    bool screen_changed = (screen_id != m_last_rendered_screen);
    m_last_rendered_screen = screen_id;

    if (screen_changed || m_frame_dirty)
        m_settle_frames_left = kBufferSettleFrames;

    bool full = m_settle_frames_left > 0;
    if (full) m_settle_frames_left--;

    m_frame_dirty = false;
    return full;
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
        if (!m_launch_doc) {
            m_launch_doc = m_context->LoadDocument(p + "launch.rml");
            if (m_launch_doc) m_launch_doc->Hide();
    if (m_list_doc) m_list_doc->Hide();
    if (m_browser_doc) m_browser_doc->Hide();
    if (m_changelog_doc) m_changelog_doc->Hide();
        }
        if (!m_list_doc) {
            m_list_doc = m_context->LoadDocument(p + "list.rml");
            if (m_list_doc) m_list_doc->Hide();
    if (m_browser_doc) m_browser_doc->Hide();
    if (m_changelog_doc) m_changelog_doc->Hide();
        }
        if (!m_browser_doc) {
            m_browser_doc = m_context->LoadDocument(p + "browser.rml");
            if (m_browser_doc) m_browser_doc->Hide();
    if (m_changelog_doc) m_changelog_doc->Hide();
        }
        if (!m_changelog_doc) {
            m_changelog_doc = m_context->LoadDocument(p + "changelog.rml");
            if (m_changelog_doc) m_changelog_doc->Hide();
        }
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
        if (!m_nav_doc) {
            m_nav_doc = m_context->LoadDocument(p + "navbar.rml");
            if (m_nav_doc) m_nav_doc->Hide();
        }
    }

    if (!m_launch_doc) std::cerr << "[EVO RmlUi] Failed to load launch.rml!" << std::endl;
    if (!m_list_doc) std::cerr << "[EVO RmlUi] Failed to load list.rml!" << std::endl;
    if (!m_browser_doc) std::cerr << "[EVO RmlUi] Failed to load browser.rml!" << std::endl;
    if (!m_changelog_doc) std::cerr << "[EVO RmlUi] Failed to load changelog.rml!" << std::endl;
    if (!m_playback_doc) std::cerr << "[EVO RmlUi] Failed to load playback.rml!" << std::endl;
    if (!m_dialog_doc) std::cerr << "[EVO RmlUi] Failed to load dialog.rml!" << std::endl;
    if (!m_settings_doc) std::cerr << "[EVO RmlUi] Failed to load settings.rml!" << std::endl;
    if (!m_subtitles_doc) std::cerr << "[EVO RmlUi] Failed to load subtitles.rml!" << std::endl;
    if (!m_mediainfo_doc) std::cerr << "[EVO RmlUi] Failed to load mediainfo.rml!" << std::endl;
    if (!m_nav_doc) std::cerr << "[EVO RmlUi] Failed to load navbar.rml!" << std::endl;

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

    if (m_nav_doc) {
        m_nav_doc->Close();
        m_nav_doc = nullptr;
    }

    if (m_launch_doc) {
        m_launch_doc->Close();
        m_launch_doc = nullptr;
    }

    if (m_list_doc) {
        m_list_doc->Close();
        m_list_doc = nullptr;
    }

    if (m_browser_doc) {
        m_browser_doc->Close();
        m_browser_doc = nullptr;
    }

    if (m_changelog_doc) {
        m_changelog_doc->Close();
        m_changelog_doc = nullptr;
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

/* ==========================================================================
 * Launch / home screen
 * ========================================================================== */

static std::string pct_string(int permille) {
    double pct = permille / 10.0;
    if (pct < 0.0)   pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << pct << "%";
    return ss.str();
}

/*
 * Name the texture for one art slot, registering the pixels with the render
 * interface if they have changed. RmlUi caches by source string, so the name
 * carries a generation counter: reusing it would serve the previous poster
 * forever, and the cover cache hands back the same buffer for different
 * files. Returns an empty string when there is no artwork.
 */
std::string EvoRmlApp::ArtSource(int slot, const uint32_t* pixels, int w, int h,
                                 const std::string& tag)
{
    if (slot < 0 || slot >= kArtSlots) return std::string();

    if (!pixels || w <= 0 || h <= 0) {
        if (!m_art_source[slot].empty()) {
            Rml::ReleaseTexture(m_art_source[slot], m_render.get());
            m_render->DropMemoryTexture(m_art_source[slot]);
            m_art_source[slot].clear();
        }
        m_art_last_ptr[slot] = nullptr;
        m_art_last_tag[slot].clear();
        return std::string();
    }

    bool unchanged = !m_art_source[slot].empty() &&
                     pixels == m_art_last_ptr[slot] &&
                     w == m_art_last_dims[slot][0] &&
                     h == m_art_last_dims[slot][1] &&
                     tag == m_art_last_tag[slot];
    if (unchanged) return m_art_source[slot];

    if (!m_art_source[slot].empty()) {
        Rml::ReleaseTexture(m_art_source[slot], m_render.get());
        m_render->DropMemoryTexture(m_art_source[slot]);
    }

    m_art_generation[slot]++;
    std::ostringstream ss;
    ss << "evo:mem/art" << slot << "-" << m_art_generation[slot];

    m_art_source[slot] = ss.str();
    m_art_last_ptr[slot] = pixels;
    m_art_last_dims[slot][0] = w;
    m_art_last_dims[slot][1] = h;
    m_art_last_tag[slot] = tag;

    m_render->SetMemoryTexture(m_art_source[slot], pixels, w, h);
    return m_art_source[slot];
}

void EvoRmlApp::UpdateLaunchState(const EvoLaunchState& state) {
    if (!m_initialized || !m_launch_doc) return;
    if (state == m_last_launch && m_theme_generation == m_theme_gen_launch) return;
    m_theme_gen_launch = m_theme_generation;
    m_frame_dirty = true;

    m_last_launch = state;

    const std::string accent    = to_hex_rgb(m_theme.accent);
    const std::string accent_bg = to_hex_rgba((m_theme.accent & 0x00FFFFFFu) | (0x26u << 24));
    const std::string surface   = to_hex_rgba(m_theme.surface);
    const std::string surf_sel  = to_hex_rgba(m_theme.surface_sel);
    const std::string border    = to_hex_rgba(m_theme.border);
    const std::string text_1    = to_hex_rgb(m_theme.text_primary);
    const std::string text_2    = to_hex_rgb(m_theme.text_secondary);
    const std::string text_3    = to_hex_rgb(m_theme.text_muted);

    auto set_text = [&](const char* id, const std::string& value) {
        Rml::Element* el = m_launch_doc->GetElementById(id);
        if (el) el->SetInnerRML(value);
        return el;
    };

    /* ---- header ---- */
    set_text("brand-name", state.app_name.empty() ? "EVO PLAYER" : state.app_name);

    Rml::Element* el_ver = m_launch_doc->GetElementById("brand-version");
    if (el_ver) {
        el_ver->SetProperty("display", state.version.empty() ? "none" : "block");
        el_ver->SetInnerRML(state.version);
        el_ver->SetProperty("color", text_3);
    }

    Rml::Element* el_clock = m_launch_doc->GetElementById("status-clock");
    if (el_clock) {
        el_clock->SetProperty("display", state.clock.empty() ? "none" : "block");
        el_clock->SetInnerRML(state.clock);
        el_clock->SetProperty("color", text_1);
    }

    Rml::Element* el_theme = m_launch_doc->GetElementById("status-theme");
    if (el_theme) {
        el_theme->SetProperty("display", state.theme_name.empty() ? "none" : "block");
        el_theme->SetInnerRML(state.theme_name);
        el_theme->SetProperty("color", text_3);
    }

    Rml::Element* el_mark = m_launch_doc->GetElementById("brand-mark");
    if (el_mark) el_mark->SetProperty("background-color", accent_bg);

    Rml::Element* el_name = m_launch_doc->GetElementById("brand-name");
    if (el_name) el_name->SetProperty("color", text_1);

    /* ---- hero ---- */
    Rml::Element* el_hero = m_launch_doc->GetElementById("hero");
    if (el_hero) {
        el_hero->SetProperty("background-color", surface);
        if (state.hero_focused) {
            el_hero->SetProperty("border-color", accent);
            el_hero->SetProperty("border-width", "2px");
        } else {
            el_hero->SetProperty("border-color", border);
            el_hero->SetProperty("border-width", "1px");
        }
    }

    std::string hero_src = ArtSource(0, state.hero_art, state.hero_art_w,
                                     state.hero_art_h, state.hero_title);
    Rml::Element* el_hero_art = m_launch_doc->GetElementById("hero-art");
    if (el_hero_art) {
        if (hero_src.empty()) {
            el_hero_art->SetProperty("display", "none");
        } else {
            el_hero_art->SetProperty("display", "block");
            el_hero_art->SetProperty("decorator", "image(" + hero_src + " cover)");
        }
    }

    /* The horizontal fade only earns its place over artwork; without a
     * poster it would paint a seam across flat surface colour. */
    const std::string surf_opaque = to_hex_rgba((m_theme.surface & 0x00FFFFFFu) | 0xFF000000u);

    Rml::Element* el_fade_l = m_launch_doc->GetElementById("hero-fade-l");
    if (el_fade_l) {
        el_fade_l->SetProperty("display", hero_src.empty() ? "none" : "block");
        el_fade_l->SetProperty("background-color", surf_opaque);
    }

    Rml::Element* el_fade_h = m_launch_doc->GetElementById("hero-fade-h");
    if (el_fade_h) {
        el_fade_h->SetProperty("display", hero_src.empty() ? "none" : "block");
        el_fade_h->SetProperty("decorator",
            "horizontal-gradient(" + surf_opaque + " " +
            to_hex_rgba(m_theme.surface & 0x00FFFFFFu) + ")");
    }

    Rml::Element* el_fade_b = m_launch_doc->GetElementById("hero-fade-b");
    if (el_fade_b) {
        el_fade_b->SetProperty("display", hero_src.empty() ? "none" : "block");
    }

    set_text("hero-eyebrow", state.hero_eyebrow);
    Rml::Element* el_eyebrow = m_launch_doc->GetElementById("hero-eyebrow");
    if (el_eyebrow) el_eyebrow->SetProperty("color", accent);

    Rml::Element* el_htitle = set_text("hero-title", state.hero_title);
    if (el_htitle) el_htitle->SetProperty("color", text_1);

    Rml::Element* el_hdetail = m_launch_doc->GetElementById("hero-detail");
    if (el_hdetail) {
        el_hdetail->SetProperty("display", state.hero_detail.empty() ? "none" : "block");
        el_hdetail->SetInnerRML(state.hero_detail);
        el_hdetail->SetProperty("color", text_2);
    }

    Rml::Element* el_ptrack = m_launch_doc->GetElementById("hero-progress-track");
    Rml::Element* el_pfill  = m_launch_doc->GetElementById("hero-progress-fill");
    if (el_ptrack) {
        el_ptrack->SetProperty("display", state.hero_progress >= 0 ? "block" : "none");
        el_ptrack->SetProperty("background-color", border);
    }
    if (el_pfill) {
        el_pfill->SetProperty("width", pct_string(state.hero_progress));
        el_pfill->SetProperty("background-color", accent);
    }

    /*
     * On the selected chip the fill IS the accent, so the glyph and the label
     * have to flip to the same dark colour or they disappear into it.
     */
    Rml::Element* el_chip   = m_launch_doc->GetElementById("hero-chip");
    Rml::Element* el_clabel = m_launch_doc->GetElementById("hero-chip-label");
    if (el_chip) {
        if (state.hero_action.empty()) {
            el_chip->SetProperty("display", "none");
        } else {
            el_chip->SetProperty("display", "inline-flex");
            if (state.hero_focused) {
                el_chip->SetProperty("background-color", accent);
                el_chip->SetProperty("border-color", "#ffffff");
            } else {
                el_chip->SetProperty("background-color", surface);
                el_chip->SetProperty("border-color", border);
            }
        }
    }
    if (el_clabel) {
        el_clabel->SetInnerRML(state.hero_action);
        el_clabel->SetProperty("color", state.hero_focused
                                            ? to_hex_rgb(m_theme.bg_bottom)
                                            : text_1);
    }

    /* ---- shelves ---- */
    const bool has_recent = !state.recent.empty();

    Rml::Element* el_shelf_r = m_launch_doc->GetElementById("shelf-recent");
    if (el_shelf_r) el_shelf_r->SetProperty("display", has_recent ? "block" : "none");

    Rml::Element* el_tick_r = m_launch_doc->GetElementById("shelf-recent-tick");
    if (el_tick_r) el_tick_r->SetProperty("background-color", accent);
    Rml::Element* el_tick_l = m_launch_doc->GetElementById("shelf-library-tick");
    if (el_tick_l) el_tick_l->SetProperty("background-color", accent);

    /* "n OF m" only when the shelf actually scrolls, so it is clear there is
     * more off the right edge than the six tiles on screen. */
    Rml::Element* el_count = m_launch_doc->GetElementById("shelf-recent-count");
    if (el_count) {
        if (state.recent_total > (int)state.recent.size() && state.recent_cursor >= 0) {
            std::ostringstream ss;
            ss << (state.recent_cursor + 1) << " OF " << state.recent_total;
            el_count->SetInnerRML(ss.str());
            el_count->SetProperty("display", "block");
            el_count->SetProperty("color", text_3);
        } else {
            el_count->SetProperty("display", "none");
        }
    }

    /* Recent shelf: posters, captions and a resume bar. */
    for (int i = 0; i < 6; i++) {
        const std::string n = std::to_string(i);
        Rml::Element* el_tile   = m_launch_doc->GetElementById("rec-tile-" + n);
        Rml::Element* el_art    = m_launch_doc->GetElementById("rec-art-" + n);
        Rml::Element* el_ibox   = m_launch_doc->GetElementById("rec-iconbox-" + n);
        Rml::Element* el_icon   = m_launch_doc->GetElementById("rec-icon-" + n);
        Rml::Element* el_title  = m_launch_doc->GetElementById("rec-title-" + n);
        Rml::Element* el_detail = m_launch_doc->GetElementById("rec-detail-" + n);
        Rml::Element* el_track  = m_launch_doc->GetElementById("rec-prog-track-" + n);
        Rml::Element* el_fill   = m_launch_doc->GetElementById("rec-prog-fill-" + n);

        if (!el_tile) continue;

        if (i >= (int)state.recent.size()) {
            el_tile->SetProperty("display", "none");
            ArtSource(1 + i, nullptr, 0, 0, std::string());
            continue;
        }

        const EvoLaunchTile& t = state.recent[i];
        el_tile->SetProperty("display", "block");
        el_tile->SetClass("tile-focused", t.is_focused);
        if (t.is_focused) {
            el_tile->SetProperty("background-color", surf_sel);
            el_tile->SetProperty("border-color", accent);
            el_tile->SetProperty("border-width", "2px");
        } else {
            el_tile->SetProperty("background-color", surface);
            el_tile->SetProperty("border-color", border);
            el_tile->SetProperty("border-width", "1px");
        }

        std::string src = ArtSource(1 + i, t.art, t.art_w, t.art_h, t.title);
        if (el_art) {
            if (src.empty()) {
                el_art->SetProperty("display", "none");
            } else {
                el_art->SetProperty("display", "block");
                el_art->SetProperty("decorator", "image(" + src + " cover)");
            }
        }
        /* No poster: fall back to the recent glyph in the icon position. */
        if (el_ibox) el_ibox->SetProperty("display", src.empty() ? "flex" : "none");
        if (el_icon) {
            if (!t.icon_path.empty()) el_icon->SetAttribute("src", t.icon_path);
            SetImageColor(el_icon, t.is_focused ? accent : text_2);
        }

        if (el_title)  el_title->SetInnerRML(t.title);
        if (el_detail) {
            el_detail->SetProperty("display", t.detail.empty() ? "none" : "block");
            el_detail->SetInnerRML(t.detail);
            el_detail->SetProperty("color", text_2);
        }
        if (el_track) {
            el_track->SetProperty("display", t.progress >= 0 ? "block" : "none");
            el_track->SetProperty("background-color", border);
        }
        if (el_fill) {
            el_fill->SetProperty("width", pct_string(t.progress));
            el_fill->SetProperty("background-color", accent);
        }
    }

    /* Library shelf: destinations, so a large soft icon rather than a poster. */
    for (int i = 0; i < 6; i++) {
        const std::string n = std::to_string(i);
        Rml::Element* el_tile   = m_launch_doc->GetElementById("lib-tile-" + n);
        Rml::Element* el_bloom  = m_launch_doc->GetElementById("lib-bloom-" + n);
        Rml::Element* el_icon   = m_launch_doc->GetElementById("lib-icon-" + n);
        Rml::Element* el_title  = m_launch_doc->GetElementById("lib-title-" + n);
        Rml::Element* el_detail = m_launch_doc->GetElementById("lib-detail-" + n);

        if (!el_tile) continue;

        if (i >= (int)state.library.size()) {
            el_tile->SetProperty("display", "none");
            continue;
        }

        const EvoLaunchTile& t = state.library[i];
        el_tile->SetProperty("display", "block");
        el_tile->SetClass("tile-focused", t.is_focused);
        if (t.is_focused) {
            el_tile->SetProperty("background-color", surf_sel);
            el_tile->SetProperty("border-color", accent);
            el_tile->SetProperty("border-width", "2px");
        } else {
            el_tile->SetProperty("background-color", surface);
            el_tile->SetProperty("border-color", border);
            el_tile->SetProperty("border-width", "1px");
        }

        if (el_bloom) {
            el_bloom->SetProperty("background-color",
                t.is_focused ? accent_bg
                             : to_hex_rgba(m_theme.accent & 0x00FFFFFFu));
        }
        if (el_icon) {
            if (!t.icon_path.empty()) el_icon->SetAttribute("src", t.icon_path);
            /* Library slot 3 is the Emby destination - icon_emby.png is a
             * trademark excluded from the icon swap/tint, kept as baked. */
            if (i != 3) SetImageColor(el_icon, t.is_focused ? accent : text_2);
        }
        if (el_title)  el_title->SetInnerRML(t.title);
        if (el_detail) {
            el_detail->SetInnerRML(t.detail);
            el_detail->SetProperty("color", text_2);
        }
    }
}

void EvoRmlApp::RenderLaunch(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_launch_doc || !framebuffer) return;

    if (m_playback_doc)  m_playback_doc->Hide();
    if (m_dialog_doc)    m_dialog_doc->Hide();
    if (m_settings_doc)  m_settings_doc->Hide();
    if (m_subtitles_doc) m_subtitles_doc->Hide();
    if (m_mediainfo_doc) m_mediainfo_doc->Hide();
    if (m_list_doc)      m_list_doc->Hide();
    if (m_browser_doc)   m_browser_doc->Hide();
    if (m_changelog_doc) m_changelog_doc->Hide();
    m_launch_doc->Show();

    /* Nav rail rendered in the same pass — shown/hidden by UpdateNavState */
    if (m_nav_doc) {
        if (m_last_nav.visible)
            m_nav_doc->Show();
        else
            m_nav_doc->Hide();
    }

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);

    if (ShouldFullRender(0)) {
        m_context->Update();
        m_context->Render();
    }
}

/* ==========================================================================
 * Generic list screen — recent, favorites, emby setup, emby browse
 * ========================================================================== */

void EvoRmlApp::UpdateListState(const EvoListState& state) {
    if (!m_initialized || !m_list_doc) return;
    if (state == m_last_list && m_theme_generation == m_theme_gen_list) return;
    m_theme_gen_list = m_theme_generation;
    m_frame_dirty = true;

    if (!m_version.empty()) {
        if (Rml::Element* vel = m_list_doc->GetElementById("list-footer-version"))
            vel->SetInnerRML(m_version);
    }

    m_last_list = state;

    const std::string accent   = to_hex_rgb(m_theme.accent);
    const std::string surface  = to_hex_rgba(m_theme.surface);
    const std::string surf_sel = to_hex_rgba(m_theme.surface_sel);
    const std::string border   = to_hex_rgba(m_theme.border);
    const std::string text_1   = to_hex_rgb(m_theme.text_primary);
    const std::string text_2   = to_hex_rgb(m_theme.text_secondary);
    const std::string text_3   = to_hex_rgb(m_theme.text_muted);

    auto el = [&](const std::string& id) { return m_list_doc->GetElementById(id); };

    if (Rml::Element* e = el("list-title"))    e->SetInnerRML(state.title);
    if (Rml::Element* e = el("list-subtitle")) e->SetInnerRML(state.subtitle);
    if (Rml::Element* e = el("list-indicator")) e->SetProperty("background-color", accent);

    /* "n OF m" only when the list is longer than the window — otherwise the
     * count is already on screen and the marker is noise. */
    if (Rml::Element* e = el("list-counter")) {
        if (state.total_count > (int)state.rows.size() && state.cursor_index >= 0) {
            std::ostringstream ss;
            ss << (state.cursor_index + 1) << " OF " << state.total_count;
            e->SetInnerRML(ss.str());
            e->SetProperty("display", "block");
            e->SetProperty("color", text_3);
        } else if (state.total_count > 0) {
            std::ostringstream ss;
            ss << state.total_count << (state.total_count == 1 ? " ITEM" : " ITEMS");
            e->SetInnerRML(ss.str());
            e->SetProperty("display", "block");
            e->SetProperty("color", text_3);
        } else {
            e->SetProperty("display", "none");
        }
    }

    /* Empty state replaces the rows outright rather than sitting under them. */
    if (Rml::Element* e = el("list-rows"))
        e->SetProperty("display", state.is_empty ? "none" : "flex");
    if (Rml::Element* e = el("list-empty"))
        e->SetProperty("display", state.is_empty ? "flex" : "none");

    if (state.is_empty) {
        if (Rml::Element* e = el("list-empty-title")) {
            e->SetInnerRML(state.empty_title);
            e->SetProperty("color", text_1);
        }
        if (Rml::Element* e = el("list-empty-hint")) {
            e->SetInnerRML(state.empty_hint);
            e->SetProperty("color", text_3);
        }
        if (Rml::Element* e = el("list-empty-icon")) {
            if (!state.empty_icon.empty()) e->SetAttribute("src", state.empty_icon);
            SetImageColor(e, text_3);
        }
        if (Rml::Element* e = el("list-empty-icon-box")) {
            e->SetProperty("background-color", surface);
            e->SetProperty("border-color", border);
        }
    }

    for (int i = 0; i < kListRows; i++) {
        const std::string n = std::to_string(i);
        Rml::Element* row    = el("lrow-" + n);
        Rml::Element* icon   = el("lrow-icon-" + n);
        Rml::Element* title  = el("lrow-title-" + n);
        Rml::Element* detail = el("lrow-detail-" + n);
        Rml::Element* badge  = el("lrow-badge-" + n);
        Rml::Element* chev   = el("lrow-chevron-" + n);
        Rml::Element* track  = el("lrow-track-" + n);
        Rml::Element* fill   = el("lrow-fill-" + n);

        if (!row) continue;

        if (i >= (int)state.rows.size()) {
            row->SetProperty("display", "none");
            continue;
        }

        const EvoListRow& r = state.rows[i];
        bool focused = r.is_focused && !state.rail_focused;

        row->SetProperty("display", "flex");
        row->SetClass("list-row-focused", focused);
        if (focused) {
            row->SetProperty("background-color", surf_sel);
            row->SetProperty("border-color", accent);
            row->SetProperty("border-width", "1.5px");
        } else {
            row->SetProperty("background-color", surface);
            row->SetProperty("border-color", border);
            row->SetProperty("border-width", "1px");
        }

        if (icon) {
            if (!r.icon_path.empty()) icon->SetAttribute("src", r.icon_path);
            SetImageColor(icon, focused ? accent : text_3);
        }
        if (title) {
            title->SetInnerRML(r.title);
            title->SetProperty("color", text_1);
        }
        if (detail) {
            detail->SetProperty("display", r.detail.empty() ? "none" : "block");
            detail->SetInnerRML(r.detail);
            detail->SetProperty("color", text_2);
        }
        if (badge) {
            if (r.badge.empty()) {
                badge->SetProperty("display", "none");
            } else {
                badge->SetProperty("display", "inline-block");
                badge->SetInnerRML(r.badge);
                if (focused) {
                    badge->SetProperty("background-color", accent);
                    badge->SetProperty("border-color", "#ffffff");
                    badge->SetProperty("color", to_hex_rgb(m_theme.bg_bottom));
                } else {
                    badge->SetProperty("background-color", surf_sel);
                    badge->SetProperty("border-color", border);
                    badge->SetProperty("color", accent);
                }
            }
        }
        if (chev) {
            chev->SetProperty("display", r.has_chevron ? "inline-block" : "none");
            SetImageColor(chev, focused ? accent : text_3);
        }
        if (track) {
            track->SetProperty("display", r.progress >= 0 ? "block" : "none");
            track->SetProperty("background-color", border);
        }
        if (fill) {
            fill->SetProperty("width", pct_string(r.progress));
            fill->SetProperty("background-color", accent);
        }
    }

    for (int i = 0; i < 4; i++) {
        const std::string n = std::to_string(i);
        Rml::Element* hint  = el("lhint-" + n);
        Rml::Element* glyph = el("lhint-glyph-" + n);
        Rml::Element* label = el("lhint-label-" + n);

        if (!hint) continue;

        if (i >= (int)state.hints.size()) {
            hint->SetProperty("display", "none");
            continue;
        }
        hint->SetProperty("display", "flex");
        if (glyph && !state.hints[i].glyph_path.empty())
            glyph->SetAttribute("src", state.hints[i].glyph_path);
        if (label) {
            label->SetInnerRML(state.hints[i].label);
            label->SetProperty("color", text_2);
        }
    }
}

void EvoRmlApp::RenderList(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_list_doc || !framebuffer) return;

    if (m_launch_doc)    m_launch_doc->Hide();
    if (m_playback_doc)  m_playback_doc->Hide();
    if (m_dialog_doc)    m_dialog_doc->Hide();
    if (m_settings_doc)  m_settings_doc->Hide();
    if (m_subtitles_doc) m_subtitles_doc->Hide();
    if (m_mediainfo_doc) m_mediainfo_doc->Hide();
    if (m_browser_doc)   m_browser_doc->Hide();
    if (m_changelog_doc) m_changelog_doc->Hide();
    m_list_doc->Show();

    if (m_nav_doc) {
        if (m_last_nav.visible) m_nav_doc->Show();
        else                    m_nav_doc->Hide();
    }

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);

    if (ShouldFullRender(1)) {
        m_context->Update();
        m_context->Render();
    }
}

/* ==========================================================================
 * USB storage browser
 * ========================================================================== */

void EvoRmlApp::UpdateBrowserState(const EvoBrowserState& state) {
    if (!m_initialized || !m_browser_doc) return;
    if (state == m_last_browser && m_theme_generation == m_theme_gen_browser) return;
    m_theme_gen_browser = m_theme_generation;
    m_frame_dirty = true;

    if (!m_version.empty()) {
        if (Rml::Element* vel = m_browser_doc->GetElementById("browser-footer-version"))
            vel->SetInnerRML(m_version);
    }

    m_last_browser = state;

    const std::string accent   = to_hex_rgb(m_theme.accent);
    const std::string surface  = to_hex_rgba(m_theme.surface);
    const std::string surf_sel = to_hex_rgba(m_theme.surface_sel);
    const std::string border   = to_hex_rgba(m_theme.border);
    const std::string text_1   = to_hex_rgb(m_theme.text_primary);
    const std::string text_2   = to_hex_rgb(m_theme.text_secondary);
    const std::string text_3   = to_hex_rgb(m_theme.text_muted);

    auto el = [&](const std::string& id) { return m_browser_doc->GetElementById(id); };

    if (Rml::Element* e = el("browser-indicator")) e->SetProperty("background-color", accent);
    if (Rml::Element* e = el("browser-title")) e->SetInnerRML(state.title);
    if (Rml::Element* e = el("browser-path")) {
        e->SetInnerRML(state.path);
        e->SetProperty("color", text_2);
    }
    if (Rml::Element* e = el("browser-counter")) {
        std::ostringstream ss;
        if (state.total_count > 0 && state.cursor_index >= 0)
            ss << (state.cursor_index + 1) << " OF " << state.total_count;
        else
            ss << state.total_count << " ITEMS";
        e->SetInnerRML(ss.str());
        e->SetProperty("color", text_3);
    }
    /* At the root there is nowhere to go back to, so the hint would be a lie. */
    if (Rml::Element* e = el("bhint-back"))
        e->SetProperty("display", state.at_root ? "none" : "flex");

    if (Rml::Element* e = el("browser-list"))
        e->SetProperty("display", state.is_empty ? "none" : "flex");
    if (Rml::Element* e = el("browser-empty"))
        e->SetProperty("display", state.is_empty ? "flex" : "none");
    if (state.is_empty) {
        if (Rml::Element* e = el("browser-empty-title")) e->SetInnerRML(state.empty_title);
        if (Rml::Element* e = el("browser-empty-hint"))  e->SetInnerRML(state.empty_hint);
        if (Rml::Element* e = el("browser-empty-icon"))  SetImageColor(e, text_3);
    }

    for (int i = 0; i < kBrowserRows; i++) {
        const std::string n = std::to_string(i);
        Rml::Element* row    = el("brow-" + n);
        Rml::Element* icon   = el("brow-icon-" + n);
        Rml::Element* name   = el("brow-name-" + n);
        Rml::Element* detail = el("brow-detail-" + n);
        Rml::Element* fav    = el("brow-fav-" + n);
        Rml::Element* badge  = el("brow-badge-" + n);
        Rml::Element* track  = el("brow-track-" + n);
        Rml::Element* fill   = el("brow-fill-" + n);

        if (!row) continue;

        if (i >= (int)state.rows.size()) {
            row->SetProperty("display", "none");
            continue;
        }

        const EvoBrowserRow& r = state.rows[i];
        bool focused = r.is_focused && !state.rail_focused;

        row->SetProperty("display", "flex");
        row->SetClass("brow-focused", focused);
        if (focused) {
            row->SetProperty("background-color", surf_sel);
            row->SetProperty("border-color", accent);
            row->SetProperty("border-width", "1.5px");
        } else {
            row->SetProperty("background-color", surface);
            row->SetProperty("border-color", border);
            row->SetProperty("border-width", "1px");
        }

        if (icon) {
            if (!r.icon_path.empty()) icon->SetAttribute("src", r.icon_path);
            SetImageColor(icon, focused ? accent : text_3);
        }
        if (name) {
            name->SetInnerRML(r.name);
            name->SetProperty("color", text_1);
        }
        if (detail) {
            detail->SetProperty("display", r.detail.empty() ? "none" : "block");
            detail->SetInnerRML(r.detail);
            detail->SetProperty("color", text_2);
        }
        if (fav) {
            fav->SetProperty("display", r.is_favorite ? "inline-block" : "none");
            SetImageColor(fav, accent);
        }
        if (badge) {
            if (r.badge.empty()) {
                badge->SetProperty("display", "none");
            } else {
                badge->SetProperty("display", "inline-block");
                badge->SetInnerRML(r.badge);
                if (focused) {
                    badge->SetProperty("background-color", accent);
                    badge->SetProperty("border-color", "#ffffff");
                    badge->SetProperty("color", to_hex_rgb(m_theme.bg_bottom));
                } else {
                    badge->SetProperty("background-color", surf_sel);
                    badge->SetProperty("border-color", border);
                    badge->SetProperty("color", accent);
                }
            }
        }
        if (track) {
            track->SetProperty("display", r.progress >= 0 ? "block" : "none");
            track->SetProperty("background-color", border);
        }
        if (fill) {
            fill->SetProperty("width", pct_string(r.progress));
            fill->SetProperty("background-color", accent);
        }
    }

    /* ---- inspector ---- */
    if (Rml::Element* e = el("inspector")) {
        e->SetProperty("background-color", surface);
        e->SetProperty("border-color", border);
    }

    std::string prev = ArtSource(kBrowserArtSlot, state.ins_preview,
                                 state.ins_preview_w, state.ins_preview_h,
                                 state.ins_name);
    if (Rml::Element* e = el("ins-preview-art")) {
        if (prev.empty()) {
            e->SetProperty("display", "none");
        } else {
            e->SetProperty("display", "block");
            e->SetProperty("decorator", "image(" + prev + " cover)");
        }
    }
    if (Rml::Element* e = el("ins-preview-empty"))
        e->SetProperty("display", prev.empty() ? "block" : "none");

    if (Rml::Element* e = el("ins-preview-badge")) {
        if (state.ins_preview_badge.empty() || prev.empty()) {
            e->SetProperty("display", "none");
        } else {
            e->SetProperty("display", "block");
            e->SetInnerRML(state.ins_preview_badge);
        }
    }

    if (Rml::Element* e = el("ins-name")) {
        e->SetInnerRML(state.ins_name);
        e->SetProperty("color", text_1);
    }
    if (Rml::Element* e = el("ins-kind")) {
        e->SetInnerRML(state.ins_kind);
        e->SetProperty("color", accent);
    }
    if (Rml::Element* e = el("ins-ext")) {
        e->SetProperty("display", state.ins_ext.empty() ? "none" : "inline-block");
        e->SetInnerRML(state.ins_ext);
        e->SetProperty("background-color", surf_sel);
        e->SetProperty("border-color", border);
        e->SetProperty("color", text_2);
    }
    if (Rml::Element* e = el("ins-probing"))
        e->SetProperty("display", state.ins_probing ? "block" : "none");

    for (int i = 0; i < kBrowserProps; i++) {
        const std::string n = std::to_string(i);
        Rml::Element* prop = el("ins-prop-" + n);
        Rml::Element* key  = el("ins-key-" + n);
        Rml::Element* val  = el("ins-val-" + n);

        if (!prop) continue;

        if (i >= (int)state.ins_props.size()) {
            prop->SetProperty("display", "none");
            continue;
        }
        prop->SetProperty("display", "flex");
        if (key) {
            key->SetInnerRML(state.ins_props[i].first);
            key->SetProperty("color", text_3);
        }
        if (val) {
            val->SetInnerRML(state.ins_props[i].second);
            val->SetProperty("color", text_1);
        }
    }
}

void EvoRmlApp::RenderBrowser(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_browser_doc || !framebuffer) return;

    if (m_launch_doc)    m_launch_doc->Hide();
    if (m_list_doc)      m_list_doc->Hide();
    if (m_playback_doc)  m_playback_doc->Hide();
    if (m_dialog_doc)    m_dialog_doc->Hide();
    if (m_settings_doc)  m_settings_doc->Hide();
    if (m_subtitles_doc) m_subtitles_doc->Hide();
    if (m_mediainfo_doc) m_mediainfo_doc->Hide();
    if (m_changelog_doc) m_changelog_doc->Hide();
    m_browser_doc->Show();

    if (m_nav_doc) {
        if (m_last_nav.visible) m_nav_doc->Show();
        else                    m_nav_doc->Hide();
    }

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);

    if (ShouldFullRender(2)) {
        m_context->Update();
        m_context->Render();
    }
}

/* ==========================================================================
 * Changelog — master-detail
 * ========================================================================== */

void EvoRmlApp::UpdateChangelogState(const EvoChangelogState& state) {
    if (!m_initialized || !m_changelog_doc) return;
    if (state == m_last_changelog && m_theme_generation == m_theme_gen_changelog) return;
    m_theme_gen_changelog = m_theme_generation;
    m_frame_dirty = true;

    if (!m_version.empty()) {
        if (Rml::Element* vel = m_changelog_doc->GetElementById("changelog-footer-version"))
            vel->SetInnerRML(m_version);
    }

    m_last_changelog = state;

    const std::string accent   = to_hex_rgb(m_theme.accent);
    const std::string surface  = to_hex_rgba(m_theme.surface);
    const std::string surf_sel = to_hex_rgba(m_theme.surface_sel);
    const std::string border   = to_hex_rgba(m_theme.border);
    const std::string text_1   = to_hex_rgb(m_theme.text_primary);
    const std::string text_2   = to_hex_rgb(m_theme.text_secondary);
    const std::string text_3   = to_hex_rgb(m_theme.text_muted);

    auto el = [&](const std::string& id) { return m_changelog_doc->GetElementById(id); };

    if (Rml::Element* e = el("changelog-title"))     e->SetInnerRML(state.title);
    if (Rml::Element* e = el("changelog-subtitle"))  e->SetInnerRML(state.subtitle);
    if (Rml::Element* e = el("changelog-indicator")) e->SetProperty("background-color", accent);
    if (Rml::Element* e = el("changelog-counter")) {
        std::ostringstream ss;
        ss << (state.cursor_index + 1) << " OF " << state.release_total;
        e->SetInnerRML(ss.str());
        e->SetProperty("color", text_3);
    }

    for (int i = 0; i < kChangelogReleases; i++) {
        const std::string n = std::to_string(i);
        Rml::Element* rel  = el("clrel-" + n);
        Rml::Element* ver  = el("clrel-ver-" + n);
        Rml::Element* tag  = el("clrel-tag-" + n);
        Rml::Element* date = el("clrel-date-" + n);

        if (!rel) continue;

        if (i >= (int)state.releases.size()) {
            rel->SetProperty("display", "none");
            continue;
        }

        const EvoChangelogRelease& r = state.releases[i];
        bool focused = r.is_focused && !state.rail_focused;

        rel->SetProperty("display", "flex");
        rel->SetClass("clrel-focused", focused);
        if (focused) {
            rel->SetProperty("background-color", surf_sel);
            rel->SetProperty("border-color", accent);
            rel->SetProperty("border-width", "1.5px");
        } else {
            rel->SetProperty("background-color", surface);
            rel->SetProperty("border-color", border);
            rel->SetProperty("border-width", "1px");
        }

        if (ver) {
            ver->SetInnerRML(r.version);
            ver->SetProperty("color", text_1);
        }
        if (tag) {
            tag->SetInnerRML(r.tagline);
            tag->SetProperty("color", text_2);
        }
        if (date) {
            date->SetInnerRML(r.date);
            date->SetProperty("color", text_3);
        }
    }

    if (Rml::Element* e = el("changelog-detail")) {
        e->SetProperty("background-color", surface);
        e->SetProperty("border-color", border);
    }
    if (Rml::Element* e = el("cldetail-ver")) {
        e->SetInnerRML(state.detail_version);
        e->SetProperty("color", text_1);
    }
    if (Rml::Element* e = el("cldetail-tag")) {
        e->SetInnerRML(state.detail_tagline);
        e->SetProperty("color", accent);
    }

    for (int i = 0; i < kChangelogItems; i++) {
        const std::string n = std::to_string(i);
        Rml::Element* item = el("clitem-" + n);
        Rml::Element* kind = el("clitem-kind-" + n);
        Rml::Element* text = el("clitem-text-" + n);

        if (!item) continue;

        if (i >= (int)state.items.size()) {
            item->SetProperty("display", "none");
            continue;
        }
        item->SetProperty("display", "flex");
        if (kind) {
            kind->SetInnerRML(state.items[i].first);
            kind->SetProperty("background-color", surf_sel);
            kind->SetProperty("border-color", border);
            kind->SetProperty("color", accent);
        }
        if (text) {
            text->SetInnerRML(state.items[i].second);
            text->SetProperty("color", text_2);
        }
    }

    if (Rml::Element* e = el("cldetail-more")) {
        int hidden = state.item_total - (int)state.items.size();
        if (hidden > 0) {
            std::ostringstream ss;
            ss << "+ " << hidden << " MORE IN THIS RELEASE";
            e->SetInnerRML(ss.str());
            e->SetProperty("display", "block");
            e->SetProperty("color", text_3);
        } else {
            e->SetProperty("display", "none");
        }
    }
}

void EvoRmlApp::RenderChangelog(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_changelog_doc || !framebuffer) return;

    if (m_launch_doc)    m_launch_doc->Hide();
    if (m_list_doc)      m_list_doc->Hide();
    if (m_browser_doc)   m_browser_doc->Hide();
    if (m_playback_doc)  m_playback_doc->Hide();
    if (m_dialog_doc)    m_dialog_doc->Hide();
    if (m_settings_doc)  m_settings_doc->Hide();
    if (m_subtitles_doc) m_subtitles_doc->Hide();
    if (m_mediainfo_doc) m_mediainfo_doc->Hide();
    m_changelog_doc->Show();

    if (m_nav_doc) {
        if (m_last_nav.visible) m_nav_doc->Show();
        else                    m_nav_doc->Hide();
    }

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);

    if (ShouldFullRender(3)) {
        m_context->Update();
        m_context->Render();
    }
}

void EvoRmlApp::UpdatePlaybackState(const EvoPlaybackState& state) {
    if (!m_initialized || !m_playback_doc) return;
    /* position_sec ticks every playing frame, so this only actually skips
     * while genuinely paused and idle - the theme-generation gate still
     * catches a theme switch during that pause. */
    if (state == m_last_state && m_theme_generation == m_theme_gen_playback) return;
    m_theme_gen_playback = m_theme_generation;
    m_frame_dirty = true;

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

    if (m_launch_doc) m_launch_doc->Hide();
    if (m_list_doc) m_list_doc->Hide();
    if (m_browser_doc) m_browser_doc->Hide();
    if (m_changelog_doc) m_changelog_doc->Hide();
    if (m_dialog_doc) m_dialog_doc->Hide();
    if (m_settings_doc) m_settings_doc->Hide();
    if (m_subtitles_doc) m_subtitles_doc->Hide();
    if (m_mediainfo_doc) m_mediainfo_doc->Hide();
    if (m_nav_doc) m_nav_doc->Hide();
    m_playback_doc->Show();

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);

    if (ShouldFullRender(4)) {
        m_context->Update();
        m_context->Render();
    }
}

void EvoRmlApp::UpdateDialogState(const EvoDialogState& state) {
    if (!m_initialized || !m_dialog_doc) return;
    if (state == m_last_dialog && m_theme_generation == m_theme_gen_dialog) return;
    m_theme_gen_dialog = m_theme_generation;
    m_frame_dirty = true;

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
                    el_btn->SetProperty("color", to_hex_rgb(m_theme.bg_bottom));
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

    if (m_launch_doc) m_launch_doc->Hide();
    if (m_list_doc) m_list_doc->Hide();
    if (m_browser_doc) m_browser_doc->Hide();
    if (m_changelog_doc) m_changelog_doc->Hide();
    if (m_playback_doc) m_playback_doc->Hide();
    if (m_settings_doc) m_settings_doc->Hide();
    if (m_subtitles_doc) m_subtitles_doc->Hide();
    if (m_mediainfo_doc) m_mediainfo_doc->Hide();
    m_dialog_doc->Show();

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);

    if (ShouldFullRender(5)) {
        m_context->Update();
        m_context->Render();
    }
}

void EvoRmlApp::UpdateSettingsState(const EvoSettingsState& state) {
    if (!m_initialized || !m_settings_doc) return;
    if (state == m_last_settings && m_theme_generation == m_theme_gen_settings) return;
    m_theme_gen_settings = m_theme_generation;
    m_frame_dirty = true;

    if (!m_version.empty()) {
        if (Rml::Element* vel = m_settings_doc->GetElementById("footer-version"))
            vel->SetInnerRML(m_version);
    }

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
                        /* The fill IS the accent, and every built-in accent
                         * is a light colour, so white type on it is close to
                         * invisible — this is what made a selected row harder
                         * to read than an unselected one. Flip to the darkest
                         * theme colour instead. */
                        el_badge->SetProperty("background-color", to_hex_rgb(m_theme.accent));
                        el_badge->SetProperty("border-color", "#ffffff");
                        el_badge->SetProperty("color", to_hex_rgb(m_theme.bg_bottom));
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
                    SetImageColor(el_icon, is_focused
                        ? to_hex_rgb(m_theme.accent)
                        : to_hex_rgb(m_theme.text_secondary));
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
                    SetImageColor(el_chev, is_focused
                        ? to_hex_rgb(m_theme.accent)
                        : to_hex_rgb(m_theme.text_secondary));
                }
            } else {
                el_row->SetProperty("display", "none");
            }
        }
    }
}

void EvoRmlApp::RenderSettings(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_settings_doc || !framebuffer) return;

    if (m_launch_doc) m_launch_doc->Hide();
    if (m_list_doc) m_list_doc->Hide();
    if (m_browser_doc) m_browser_doc->Hide();
    if (m_changelog_doc) m_changelog_doc->Hide();
    if (m_playback_doc) m_playback_doc->Hide();
    if (m_dialog_doc) m_dialog_doc->Hide();
    if (m_subtitles_doc) m_subtitles_doc->Hide();
    if (m_mediainfo_doc) m_mediainfo_doc->Hide();
    m_settings_doc->Show();

    /* Nav rail rendered in the same pass — shown/hidden by UpdateNavState */
    if (m_nav_doc) {
        if (m_last_nav.visible)
            m_nav_doc->Show();
        else
            m_nav_doc->Hide();
    }

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);

    if (ShouldFullRender(6)) {
        m_context->Update();
        m_context->Render();
    }
}

void EvoRmlApp::UpdateSubtitlesState(const EvoSubtitlesState& state) {
    if (!m_initialized || !m_subtitles_doc) return;
    if (state == m_last_subtitles && m_theme_generation == m_theme_gen_subtitles) return;
    m_theme_gen_subtitles = m_theme_generation;
    m_frame_dirty = true;

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
                            el_detail->SetProperty("color", to_hex_rgb(m_theme.bg_bottom));
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

    if (m_launch_doc) m_launch_doc->Hide();
    if (m_list_doc) m_list_doc->Hide();
    if (m_browser_doc) m_browser_doc->Hide();
    if (m_changelog_doc) m_changelog_doc->Hide();
    if (m_playback_doc) m_playback_doc->Hide();
    if (m_dialog_doc) m_dialog_doc->Hide();
    if (m_settings_doc) m_settings_doc->Hide();
    if (m_mediainfo_doc) m_mediainfo_doc->Hide();
    m_subtitles_doc->Show();

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);

    if (ShouldFullRender(7)) {
        m_context->Update();
        m_context->Render();
    }
}

void EvoRmlApp::UpdateMediaInfoState(const EvoMediaInfoState& state) {
    if (!m_initialized || !m_mediainfo_doc) return;
    if (state == m_last_mediainfo && m_theme_generation == m_theme_gen_mediainfo) return;
    m_theme_gen_mediainfo = m_theme_generation;
    m_frame_dirty = true;

    m_last_mediainfo = state;

    Rml::Element* el_ind = m_mediainfo_doc->GetElementById("mediainfo-indicator");
    if (el_ind) {
        el_ind->SetProperty("background-color", to_hex_rgb(m_theme.accent));
    }

    for (int i = 0; i < 4; i++) {
        Rml::Element* el_ci = m_mediainfo_doc->GetElementById("card-icon-" + std::to_string(i));
        if (el_ci) SetImageColor(el_ci, to_hex_rgb(m_theme.accent));
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

    if (m_launch_doc) m_launch_doc->Hide();
    if (m_list_doc) m_list_doc->Hide();
    if (m_browser_doc) m_browser_doc->Hide();
    if (m_changelog_doc) m_changelog_doc->Hide();
    if (m_playback_doc) m_playback_doc->Hide();
    if (m_dialog_doc) m_dialog_doc->Hide();
    if (m_settings_doc) m_settings_doc->Hide();
    if (m_subtitles_doc) m_subtitles_doc->Hide();
    if (m_nav_doc) m_nav_doc->Hide();
    m_mediainfo_doc->Show();

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);

    if (ShouldFullRender(8)) {
        m_context->Update();
        m_context->Render();
    }
}

void EvoRmlApp::UpdateNavState(const EvoNavState& state) {
    if (!m_initialized || !m_nav_doc) return;
    if (state == m_last_nav && m_theme_generation == m_theme_gen_nav) return;
    m_theme_gen_nav = m_theme_generation;
    m_frame_dirty = true;
    m_last_nav = state;

    /* ---- collapsed icon rail ---- */
    for (int i = 0; i < 7; i++) {
        std::string item_id  = "nav-item-" + std::to_string(i);
        std::string bar_id   = "nav-bar-"  + std::to_string(i);
        std::string icon_id  = "nav-icon-" + std::to_string(i);

        Rml::Element* el_item = m_nav_doc->GetElementById(item_id);
        Rml::Element* el_bar  = m_nav_doc->GetElementById(bar_id);
        Rml::Element* el_icon = m_nav_doc->GetElementById(icon_id);

        if (!el_item) continue;

        bool is_active = (i == state.active_section);
        bool is_cursor = state.rail_focused && (i == state.cursor_index);

        /* Icon pill */
        el_item->SetClass("rail-item-active",  is_active && !is_cursor);
        el_item->SetClass("rail-item-cursor",   is_cursor);

        if (is_cursor) {
            el_item->SetProperty("background-color", to_hex_rgb(m_theme.accent));
            el_item->SetProperty("border-color", "#ffffff");
            el_item->SetProperty("border-width", "1.5px");
        } else if (is_active) {
            el_item->SetProperty("background-color", to_hex_rgba(m_theme.surface_sel));
            el_item->SetProperty("border-color", to_hex_rgba(m_theme.border));
            el_item->SetProperty("border-width", "1px");
        } else {
            el_item->SetProperty("background-color", "transparent");
            el_item->SetProperty("border-color", "transparent");
            el_item->SetProperty("border-width", "0px");
        }

        /* Icon glyph: the cursor pill fills solid accent, so the glyph has to
         * flip to the darkest theme colour there or it disappears into it -
         * same trick as the settings/list focused badges. Section 4 (Emby) is
         * a trademark, excluded from the icon swap, so it keeps its own baked
         * colour rather than being retinted. */
        if (el_icon && i != 4) {
            if (is_cursor)
                SetImageColor(el_icon, to_hex_rgb(m_theme.bg_bottom));
            else if (is_active)
                SetImageColor(el_icon, to_hex_rgb(m_theme.accent));
            else
                SetImageColor(el_icon, to_hex_rgb(m_theme.text_secondary));
        }

        /* Accent bar */
        if (el_bar) {
            el_bar->SetClass("rail-accent-bar-visible", is_active && !state.rail_focused);
            if (is_active)
                el_bar->SetProperty("background-color", to_hex_rgb(m_theme.accent));
        }
    }

    /* ---- expanded overlay ---- */
    Rml::Element* scrim    = m_nav_doc->GetElementById("nav-scrim");
    Rml::Element* expanded = m_nav_doc->GetElementById("nav-expanded");
    if (scrim)    scrim->SetProperty("display",    state.rail_focused ? "block" : "none");
    if (expanded) expanded->SetProperty("display", state.rail_focused ? "block" : "none");

    for (int i = 0; i < 7; i++) {
        std::string exp_id  = "nav-exp-"       + std::to_string(i);
        std::string lbl_id  = "nav-exp-label-" + std::to_string(i);
        std::string icon_id = "nav-exp-icon-"  + std::to_string(i);

        Rml::Element* el_exp  = m_nav_doc->GetElementById(exp_id);
        Rml::Element* el_lbl  = m_nav_doc->GetElementById(lbl_id);
        Rml::Element* el_icon = m_nav_doc->GetElementById(icon_id);

        if (!el_exp) continue;

        bool is_active = (i == state.active_section);
        bool is_cursor = (i == state.cursor_index);

        el_exp->SetClass("rail-exp-item-active",  is_active && !is_cursor);
        el_exp->SetClass("rail-exp-item-cursor",   is_cursor);

        if (is_cursor) {
            el_exp->SetProperty("background-color", to_hex_rgb(m_theme.accent));
            el_exp->SetProperty("border-color", "#ffffff");
            el_exp->SetProperty("border-width", "1.5px");
            if (el_lbl) el_lbl->SetProperty("color", "#060b16");
            if (el_icon && i != 4) SetImageColor(el_icon, to_hex_rgb(m_theme.bg_bottom));
        } else if (is_active) {
            el_exp->SetProperty("background-color", to_hex_rgba(m_theme.surface_sel));
            el_exp->SetProperty("border-color", to_hex_rgba(m_theme.border));
            el_exp->SetProperty("border-width", "1px");
            if (el_lbl) el_lbl->SetProperty("color", to_hex_rgb(m_theme.text_primary));
            if (el_icon && i != 4) SetImageColor(el_icon, to_hex_rgb(m_theme.accent));
        } else {
            el_exp->SetProperty("background-color", "transparent");
            el_exp->SetProperty("border-color", "transparent");
            el_exp->SetProperty("border-width", "0px");
            if (el_lbl) el_lbl->SetProperty("color", to_hex_rgb(m_theme.text_secondary));
            if (el_icon && i != 4) SetImageColor(el_icon, to_hex_rgb(m_theme.text_secondary));
        }
    }
}

