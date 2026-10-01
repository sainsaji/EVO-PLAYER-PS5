#include "evo_features.h"
#include "evo_rmlui_app.h"
#include "evo_rmlui_prof.h"
#include "evo_metrics.h"   /* EVO_UI_DESIGN_W/H - the dp authoring canvas */
#include "prospero_thumbnail.h"   /* #32 scrub preview: request/serial/snapshot */
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdarg>

/*
 * #71 REDUX - NO libc++ STREAMS IN THE APP MODULE.
 *
 * std::ostringstream constructs the global std::locale on first use, and in
 * the native-app CRT that reads [null + 0x48] and takes the process down with
 * signal 11. That is the exact fault df7cbf2 recorded for <iostream>'s static
 * initialiser:
 *
 *   std::ios_base::Init::Init() -> std::locale::locale() -> read [null+0x48]
 *
 * Dropping <iostream> in #71 only moved it from load time to first use - every
 * ostringstream in here was still a live landmine. And it is LAYOUT-SENSITIVE,
 * so a build that happens to survive proves nothing: #90's extra .text shifted
 * the addresses and EVO began crashing on the first poster it registered,
 * inside ArtSource()'s stream. Two hardware runs, "CRASH signal=11 addr=48".
 *
 * So: snprintf everywhere, and <sstream>/<iomanip> stay out of this file.
 */
/* The path of the file playing, owned by Bridge.cpp. The scrub preview worker
 * takes a path rather than a handle, so it needs this rather than anything out
 * of EvoPlaybackState. */
extern "C" char current_media_path[768];

static std::string evo_fmt(const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return std::string(buf);
}
#include <cmath>
#include <chrono>

/* EVO_AGC_DEVICE is the console build. The host preview harness
 * (tools/uiview_playback_rml.sh) compiles this same file without it and gets
 * the CPU rasteriser - that is the only other configuration there is. */
#if defined(EVO_AGC_DEVICE)
#include "evo_rmlui_render_agc.h"
#include "evo_agc_runtime.h"
#include "evo_boot_log.h"
#else
static inline int evo_agc_runtime_is_120hz(void) { return 0; }
#endif

/* No <iostream>: its static init (ios_base::Init -> std::locale::locale())
 * crashes at load in the app module's custom CRT _init() pass, layout-
 * sensitively - #71. These diagnostics go to stderr (klog) via C stdio. */

/* FrameBegin/FrameEnd bracket every Context::Render(): no-ops on the CPU
 * rasteriser, real work on the AGC interface. Mark that a Context::Render()
 * actually reached the scanout this frame, so the device loop knows to flip. */
#if defined(EVO_AGC_DEVICE)
#define EVO_MARK_DREW() (m_drew = true)
#else
#define EVO_MARK_DREW() ((void)0)
#endif

#ifdef EVO_RML_PROFILE
#define EVO_PROF_CTX_RENDER() do {                                      \
        double _c0 = evo_prof_now_ms(); m_context->Update();            \
        double _c1 = evo_prof_now_ms();                                 \
        m_render->FrameBegin(); m_context->Render(); m_render->FrameEnd();\
        double _c2 = evo_prof_now_ms(); EVO_MARK_DREW();             \
        g_evo_rml_prof.update_ms += _c1 - _c0; g_evo_rml_prof.update_n++;\
        g_evo_rml_prof.render_ms += _c2 - _c1; g_evo_rml_prof.render_n++;\
    } while (0)
#else
#define EVO_PROF_CTX_RENDER() do {                                      \
        m_context->Update();                                           \
        m_render->FrameBegin(); m_context->Render(); m_render->FrameEnd();\
        EVO_MARK_DREW();                                            \
    } while (0)
#endif

/* Never destroyed - same reason as Application::getInstance(); its teardown
 * is done explicitly by evo_rmlui_shutdown() from Application::shutdown(),
 * and letting it also unwind at exit crashed QUIT EVO. */
EvoRmlApp& EvoRmlApp::Instance() {
    static EvoRmlApp* p_instance = new EvoRmlApp();
    EvoRmlApp& instance = *p_instance;
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
    if (h > 0) return evo_fmt("%02d:%02d:%02d", h, m, sec);
    return evo_fmt("%02d:%02d", m, sec);
}

static std::string to_hex_rgb(uint32_t col) {
    uint8_t r = col & 0xFF;
    uint8_t g = (col >> 8) & 0xFF;
    uint8_t b = (col >> 16) & 0xFF;
    char buf[16];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
    return std::string(buf);
}

/*
 * A label colour that stays readable on `bg`.
 *
 * Every theme accent is a light colour - Midnight #FFCD00, Carbon #F2F2F5,
 * Ember #FFA528, Aurora #3DF5C0 - so a pill that fills itself with the accent
 * and leaves its label white is low-contrast in all four themes, and in Carbon
 * it is white on near-white: the About version and the PAUSED badge were both
 * effectively invisible. Derive the ink from the fill instead of assuming it.
 */
static std::string ink_on(uint32_t bg) {
    const double r = (bg & 0xFF) / 255.0;
    const double g = ((bg >> 8) & 0xFF) / 255.0;
    const double b = ((bg >> 16) & 0xFF) / 255.0;
    const double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;   // Rec. 709
    return lum > 0.45 ? "#0b1017" : "#ffffff";
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

/*
 * Step 1 of docs/evo-pro/gpu-rendering-plan.md — see the comment on m_surface
 * in evo_rmlui_app.h. Rasterise a full-screen menu document into the retained
 * m_surface only when it actually changed, then blit that surface into the
 * caller's (rotating) VideoOut buffer every frame.
 */
void EvoRmlApp::RenderCachedScreen(int screen_id, uint32_t* framebuffer,
                                   int width, int height)
{
#if defined(EVO_AGC_DEVICE)
    /* GL-3 (#79) / AGC. Both modes: only touch anything on an "active" frame - the
     * device loop decided this iteration is a redraw (GlNeedsFrame); otherwise
     * the front buffer holds. */
    if (!m_gl_active)
        return;
    if (!m_blit_mode) {
        /* Mode B / AGC: RmlUi renders itself straight to the hardware default framebuffer;
         * main.c presents. No m_surface. */
        (void)framebuffer;
        m_render->SetFramebuffer(nullptr);
        m_render->SetDimensions(width, height);
        EVO_PROF_CTX_RENDER();   /* also sets m_drew */
        m_cached_screen = screen_id;
        return;
    }
    /* Mode A (default GL): CPU coverage rasteriser into m_surface -> `framebuffer`
     * (the loop's scratch); main.c uploads it as one GL quad. Falls through to
     * the shared cached-raster path below. */
#endif
    {
    const size_t px = (size_t)width * (size_t)height;

    bool resized = (width != m_surface_w || height != m_surface_h);
    if (resized) {
        m_surface.assign(px, 0xFF000000u);
        m_surface_w = width;
        m_surface_h = height;
        m_cached_screen = -1;
    }

    bool screen_changed = (screen_id != m_cached_screen);

    if (m_frame_dirty || screen_changed || resized) {
        m_render->SetFramebuffer(m_surface.data());
        m_render->SetDimensions(width, height);
        EVO_PROF_CTX_RENDER();
        m_frame_dirty = false;
        m_cached_screen = screen_id;
    }

    if (framebuffer != m_surface.data())
        std::memcpy(framebuffer, m_surface.data(), px * sizeof(uint32_t));
    }
#if defined(EVO_AGC_DEVICE)
    m_drew = true;   /* Mode A rasterised into `framebuffer` this frame */
#endif
}

/* See the declaration in evo_rmlui_app.h for why this exists. */
void EvoRmlApp::ShowOnlyScreen(Rml::ElementDocument* keep)
{
    Rml::ElementDocument* const screens[] = {
        m_launch_doc, m_list_doc, m_browser_doc, m_changelog_doc, m_reader_doc,
        m_image_doc, m_surround_doc, m_playback_doc, m_dialog_doc,
        m_settings_doc, m_about_doc, m_subtitles_doc, m_mediainfo_doc,
        m_closed_doc,
    };
    for (Rml::ElementDocument* doc : screens) {
        if (!doc || doc == keep) continue;
        doc->Hide();
    }
    if (keep) keep->Show();
}

bool EvoRmlApp::GlNeedsFrame()
{
    /* Retained-mode + ps5-opengl can't re-raster + MSAA-resolve + swap at 60 Hz,
     * so the device loop only redraws on change. Marquee / RCSS animation is
     * deferred to GL-5; the overlay docs that animate (toast slide, dialog) are
     * caught by the IsVisible checks. Called exactly once per frame by main.c. */
    if (m_gl_warmup > 0) { m_gl_warmup--; return true; }
    if (m_frame_dirty) return true;
    if (m_toast_doc && m_toast_doc->IsVisible()) return true;
    if (m_dialog_doc && m_dialog_doc->IsVisible()) return true;
    if (m_launch_doc && m_launch_doc->IsVisible()) return true;
    /* Dev debug overlay: keep the menu FPS number counting on an idle screen -
     * force a redraw ~2 Hz while it is up. */
    if (m_debug_visible) {
        long long now = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - m_debug_tick_ms >= 500) { m_debug_tick_ms = now; return true; }
    }
    return false;
}

bool EvoRmlApp::Initialize(int width, int height) {
    if (m_initialized) return true;

    m_width = width;
    m_height = height;

    /* Force the first RenderCachedScreen call after a (re)init to rasterise. */
    m_surface_w = m_surface_h = 0;
    m_cached_screen = -1;
    m_frame_dirty = true;

    m_system = std::make_unique<EvoSystemInterface>();

#ifdef EVO_AGC_DEVICE
    evo_log("RmlUi AGC: EVO_AGC_DEVICE compiled in, is_active=%d", evo_agc_runtime_is_active());
    evo_log_flush();
    if (evo_agc_runtime_is_active()) {
        int w = 0, h = 0;
        evo_agc_runtime_get_size(&w, &h);
        if (w <= 0 || h <= 0) {
            w = width;
            h = height;
        }
        auto agc = std::make_unique<EvoRenderInterfaceAGC>(w, h);
        if (agc->Ok()) {
            m_render = std::move(agc);
            m_blit_mode = false;
            evo_log("RmlUi AGC: bare-metal GPU render interface active (%dx%d)", w, h);
            evo_log_flush();
        } else {
            evo_log("RmlUi AGC: failed to construct EvoRenderInterfaceAGC");
            evo_log_flush();
        }
    }
#endif
    if (!m_render)
        m_render = std::make_unique<EvoRenderInterface>(width, height);
    /* #60: must be registered before Rml::Initialise() so every subsequent
     * LoadFontFace/LoadDocument call resolves through the embedded bundle
     * first, never touching the /app0 sandbox's broken directory traversal. */
    m_file_interface = std::make_unique<EvoRmlFileInterface>();

    Rml::SetSystemInterface(m_system.get());
    Rml::SetRenderInterface(m_render->AsRml());
    Rml::SetFileInterface(m_file_interface.get());

    if (!Rml::Initialise()) {
        fprintf(stderr, "[EVO RmlUi] Failed to initialise RmlUi core!\n");
        return false;
    }

    /* #60: "/app0/assets/fonts/" resolves through the embedded bundle
     * (evo_rmlui_fileinterface.h) before any real filesystem call happens, so
     * the /app0 sandbox's broken directory traversal never comes into play.
     * The remaining entries are host/dev fallbacks (fopen against loose
     * files) for when the bundle hasn't been (re)generated. The old
     * /data/evoplayer/app/ and /data/homebrew/EVOPlayer/ search paths - the
     * #44 out-of-band FTP sync target - are gone: that sync is removed
     * (deploy-app.sh), and shipping a self-contained .ffpfsc means those
     * loose console-side copies must have zero effect on rendering. */
    std::vector<std::string> font_prefixes = {
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
            /* GL-5 (#81): Unicode fallback faces for subtitles (and any other
             * non-Latin text). NotoSans covers Latin-ext / Cyrillic / Greek /
             * Vietnamese; DejaVuSans adds Hebrew / Arabic / Armenian / Georgian
             * and more. RmlUi's default FreeType engine has no complex shaping
             * or BiDi, so Arabic renders unjoined and Hebrew LTR, and there is
             * still no CJK face - those are subtitle follow-ups (#42/#43). What
             * this does fix outright: the #35 "every non-ASCII char -> ?" fold. */
            Rml::LoadFontFace(p + "NotoSans-Regular.ttf", true);
            Rml::LoadFontFace(p + "NotoSans-Bold.ttf", true);
            Rml::LoadFontFace(p + "DejaVuSans.ttf", true);
            Rml::LoadFontFace(p + "DejaVuSans-Bold.ttf", true);
            fprintf(stderr, "[EVO RmlUi] Loaded font face from %s\n", p.c_str());
            break;
        }
    }

    m_dp_ratio = (width > 0) ? (float)width / (float)EVO_UI_DESIGN_W : 1.0f;
    m_context = Rml::CreateContext("main_context", Rml::Vector2i(width, height));
    if (!m_context) {
        fprintf(stderr, "[EVO RmlUi] Failed to create RmlUi context!\n");
        Rml::Shutdown();
        return false;
    }

    /*
     * Every stylesheet is authored against the EVO_UI_DESIGN_W x _H canvas
     * (ui/include/evo_metrics.h) and expresses its geometry in dp, so one
     * ratio scales the whole UI to whatever the panel is running at: layout,
     * glyph rasterisation and corner tessellation all happen at the real
     * pixel size instead of being upscaled from a 1080p image. At 1920 wide
     * the ratio is exactly 1 and every dp resolves to the px it replaced,
     * which is what keeps the host renderer shot.sh diff baselines valid.
     */
    m_context->SetDensityIndependentPixelRatio(m_dp_ratio);

    std::vector<std::string> rml_prefixes = {
        "/app0/assets/rml/",     /* #60: resolves through the embedded bundle */
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
        if (!m_reader_doc) {
            m_reader_doc = m_context->LoadDocument(p + "reader.rml");
            if (m_reader_doc) m_reader_doc->Hide();
        }
        if (!m_image_doc) {
            m_image_doc = m_context->LoadDocument(p + "image.rml");
            if (m_image_doc) m_image_doc->Hide();
        }
        if (!m_surround_doc) {
            m_surround_doc = m_context->LoadDocument(p + "surround.rml");
            if (m_surround_doc) m_surround_doc->Hide();
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
        if (!m_about_doc) {
            m_about_doc = m_context->LoadDocument(p + "about.rml");
            if (m_about_doc) m_about_doc->Hide();
        }
        if (!m_closed_doc) {
            m_closed_doc = m_context->LoadDocument(p + "closed.rml");
            if (m_closed_doc) m_closed_doc->Hide();
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

    if (!m_launch_doc) fprintf(stderr, "[EVO RmlUi] Failed to load launch.rml!\n");
    if (!m_list_doc) fprintf(stderr, "[EVO RmlUi] Failed to load list.rml!\n");
    if (!m_browser_doc) fprintf(stderr, "[EVO RmlUi] Failed to load browser.rml!\n");
    if (!m_changelog_doc) fprintf(stderr, "[EVO RmlUi] Failed to load changelog.rml!\n");
    if (!m_reader_doc) fprintf(stderr, "[EVO RmlUi] Failed to load reader.rml!\n");
    if (!m_surround_doc) fprintf(stderr, "[EVO RmlUi] Failed to load surround.rml!\n");
    if (!m_playback_doc) fprintf(stderr, "[EVO RmlUi] Failed to load playback.rml!\n");
    if (!m_dialog_doc) fprintf(stderr, "[EVO RmlUi] Failed to load dialog.rml!\n");
    if (!m_settings_doc) fprintf(stderr, "[EVO RmlUi] Failed to load settings.rml!\n");
    if (!m_about_doc) fprintf(stderr, "[EVO RmlUi] Failed to load about.rml!\n");
    if (!m_closed_doc) fprintf(stderr, "[EVO RmlUi] Failed to load closed.rml!\n");
    if (!m_subtitles_doc) fprintf(stderr, "[EVO RmlUi] Failed to load subtitles.rml!\n");
    if (!m_mediainfo_doc) fprintf(stderr, "[EVO RmlUi] Failed to load mediainfo.rml!\n");
    if (!m_nav_doc) fprintf(stderr, "[EVO RmlUi] Failed to load navbar.rml!\n");
    if (!m_image_doc) fprintf(stderr, "[EVO RmlUi] Failed to load image.rml!\n");

    /* #75: toast lives in its own context - see the comment on
     * EvoToastState for why. Its own tiny context, so a load failure here
     * is a missing notification, never a missing screen - non-fatal. */
    m_toast_context = Rml::CreateContext("toast_context", Rml::Vector2i(width, height));
    if (m_toast_context) {
        m_toast_context->SetDensityIndependentPixelRatio(m_dp_ratio);
        for (const auto& p : rml_prefixes) {
            if (m_toast_doc) break;
            m_toast_doc = m_toast_context->LoadDocument(p + "toast.rml");
        }
        if (m_toast_doc) {
            m_toast_doc->Hide();
        } else {
            fprintf(stderr, "[EVO RmlUi] Failed to load toast.rml!\n");
        }
    } else {
        fprintf(stderr, "[EVO RmlUi] Failed to create toast context!\n");
    }

    /* #81: virtual keyboard - own context (same rationale as the toast). A load
     * failure just means text entry falls back to nothing on screen, which is a
     * bug but not a crash - evo_keyboard.c still has the state. */
    m_keyboard_context = Rml::CreateContext("keyboard_context", Rml::Vector2i(width, height));
    if (m_keyboard_context) {
        m_keyboard_context->SetDensityIndependentPixelRatio(m_dp_ratio);
        for (const auto& p : rml_prefixes) {
            if (m_keyboard_doc) break;
            m_keyboard_doc = m_keyboard_context->LoadDocument(p + "keyboard.rml");
        }
        if (m_keyboard_doc) m_keyboard_doc->Hide();
        else fprintf(stderr, "[EVO RmlUi] Failed to load keyboard.rml!\n");
    } else {
        fprintf(stderr, "[EVO RmlUi] Failed to create keyboard context!\n");
    }

    /* Dev debug overlay (menu FPS pill) - own context, same rationale as the
     * toast. A load failure just means no menu FPS readout - never a crash. */
    m_debug_context = Rml::CreateContext("debug_context", Rml::Vector2i(width, height));
    if (m_debug_context) {
        m_debug_context->SetDensityIndependentPixelRatio(m_dp_ratio);
        for (const auto& p : rml_prefixes) {
            if (m_debug_doc) break;
            m_debug_doc = m_debug_context->LoadDocument(p + "debug.rml");
        }
        if (m_debug_doc) m_debug_doc->Hide();
        else fprintf(stderr, "[EVO RmlUi] Failed to load debug.rml!\n");
    } else {
        fprintf(stderr, "[EVO RmlUi] Failed to create debug context!\n");
    }

    m_initialized = true;
    fprintf(stderr, "[EVO RmlUi] Retained-mode Full Engine initialized successfully (%dx%d).\n",
            width, height);
    return true;
}

void EvoRmlApp::Shutdown() {
    if (!m_initialized) return;

    if (m_about_doc) {
        m_about_doc->Close();
        m_about_doc = nullptr;
    }

    if (m_closed_doc) {
        m_closed_doc->Close();
        m_closed_doc = nullptr;
    }

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

    if (m_reader_doc) {
        m_reader_doc->Close();
        m_reader_doc = nullptr;
    }

    if (m_image_doc) {
        m_image_doc->Close();
        m_image_doc = nullptr;
    }

    if (m_surround_doc) {
        m_surround_doc->Close();
        m_surround_doc = nullptr;
    }

    if (m_context) {
        Rml::RemoveContext(m_context->GetName());
        m_context = nullptr;
    }

    if (m_toast_doc) {
        m_toast_doc->Close();
        m_toast_doc = nullptr;
    }

    if (m_toast_context) {
        Rml::RemoveContext(m_toast_context->GetName());
        m_toast_context = nullptr;
    }

    if (m_keyboard_doc) {
        m_keyboard_doc->Close();
        m_keyboard_doc = nullptr;
    }
    if (m_keyboard_context) {
        Rml::RemoveContext(m_keyboard_context->GetName());
        m_keyboard_context = nullptr;
    }

    if (m_debug_doc) {
        m_debug_doc->Close();
        m_debug_doc = nullptr;
    }
    if (m_debug_context) {
        Rml::RemoveContext(m_debug_context->GetName());
        m_debug_context = nullptr;
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
    return evo_fmt("%.1f%%", pct);
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
            Rml::ReleaseTexture(m_art_source[slot], m_render->AsRml());
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
        Rml::ReleaseTexture(m_art_source[slot], m_render->AsRml());
        m_render->DropMemoryTexture(m_art_source[slot]);
    }

    m_art_generation[slot]++;
    m_art_source[slot] = evo_fmt("evo:mem/art%d-%d", slot, m_art_generation[slot]);
    m_art_last_ptr[slot] = pixels;
    m_art_last_dims[slot][0] = w;
    m_art_last_dims[slot][1] = h;
    m_art_last_tag[slot] = tag;

    m_render->SetMemoryTexture(m_art_source[slot], pixels, w, h);
    return m_art_source[slot];
}

/*
 * Ping-pong scroll for a single-line (white-space: nowrap) text element whose
 * content overflows its parent's fixed-width overflow:hidden clip box,
 * matching the legacy evo_text_marquee: dwell at each end, constant px/sec
 * travel. Slides the element with a negative margin-left inside the parent
 * clip. Reads the post-layout width from the previous frame (one-frame lag,
 * invisible in motion). Call every frame the owning document is rendered.
 */
void EvoRmlApp::MarqueeTick(Rml::Element* text_el, bool active) {
    if (!text_el) return;
    Rml::Element* box_el = text_el->GetParentNode();
    if (!box_el) return;

    const float content = text_el->GetScrollWidth();   /* nowrap block = text width */
    const float box     = box_el->GetClientWidth();     /* the fixed clip window */
    const float travel  = content - box;

    MarqueeState& m = m_marquee[text_el];

    if (!active || travel <= 4.0f) {
        if (m.active) {
            text_el->SetProperty("margin-left", "0px");
            m.active = false;
        }
        return;
    }

    const double now = m_system ? m_system->GetElapsedTime() : 0.0;

    if (!m.active || std::fabs(m.travel - travel) > 1.0f) {
        m.active = true;
        m.start  = now;
        m.travel = travel;
    }

    const double PAUSE = 1.4;   /* seconds held at each end */
    const double SPEED = 85.0;  /* px per second */
    const double leg    = m.travel / SPEED;
    const double period = 2.0 * (leg + PAUSE);
    const double p      = std::fmod(now - m.start, period);

    double off;
    if (p < PAUSE)                    off = 0.0;
    else if (p < PAUSE + leg)         off = (p - PAUSE) * SPEED;
    else if (p < 2.0 * PAUSE + leg)   off = m.travel;
    else                             off = m.travel - (p - 2.0 * PAUSE - leg) * SPEED;

    if (off < 0.0)          off = 0.0;
    if (off > m.travel)     off = m.travel;

    text_el->SetProperty("margin-left", std::to_string(-(int)(off + 0.5)) + "px");
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
    if (el_mark) {
        el_mark->SetProperty("background-color", "#ffcd001a");
        el_mark->SetProperty("border-color", "#ffcd0038");
    }

    Rml::Element* el_logo = m_launch_doc->GetElementById("brand-logo");
    if (el_logo) SetImageColor(el_logo, accent);

    Rml::Element* el_name = m_launch_doc->GetElementById("brand-name");
    if (el_name) el_name->SetProperty("color", text_1);

    /* ---- hero ---- */
    Rml::Element* el_hero = m_launch_doc->GetElementById("hero");
    if (el_hero) {
        el_hero->SetProperty("background-color", surface);
        /* No RCSS rule: the class is how the #115 dev-remote snapshot sees
         * the highlight, which is otherwise only inline properties. */
        el_hero->SetClass("hero-focused", state.hero_focused);
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
                el_chip->SetProperty("background-color", "#1e2e4af5");
                el_chip->SetProperty("border-color", to_hex_rgb(m_theme.accent_alt));
                el_chip->SetProperty("border-width", "2px");
            } else {
                el_chip->SetProperty("background-color", "#16243af2");
                el_chip->SetProperty("border-color", "#00cdff47");
                el_chip->SetProperty("border-width", "1.5px");
            }
        }
    }
    if (el_clabel) {
        el_clabel->SetInnerRML(state.hero_action);
        el_clabel->SetProperty("color", state.hero_focused
                                            ? "#ffffff"
                                            : text_1);
    }
    Rml::Element* el_cglyph = m_launch_doc->GetElementById("hero-chip-glyph");
    if (el_cglyph) {
        SetImageColor(el_cglyph, to_hex_rgb(m_theme.accent_alt));
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
            el_count->SetInnerRML(
                evo_fmt("%d OF %d", state.recent_cursor + 1, state.recent_total));
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
            el_tile->SetProperty("background-color", "#16223a");
            el_tile->SetProperty("border-color", to_hex_rgb(m_theme.accent_alt));
            el_tile->SetProperty("border-width", "2px");
        } else {
            el_tile->SetProperty("background-color", "#0f1828eb");
            el_tile->SetProperty("border-color", "#5a7db433");
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
            if (i != 3) SetImageColor(el_icon, t.is_focused ? to_hex_rgb(m_theme.accent_alt) : "#9fb2cc");
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

    ShowOnlyScreen(m_launch_doc);

    /* Nav rail rendered in the same pass — shown/hidden by UpdateNavState */
    if (m_nav_doc) {
        if (m_last_nav.visible)
            m_nav_doc->Show();
        else
            m_nav_doc->Hide();
    }

    RenderCachedScreen(0, framebuffer, width, height);
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
            e->SetInnerRML(
                evo_fmt("%d OF %d", state.cursor_index + 1, state.total_count));
            e->SetProperty("display", "block");
            e->SetProperty("color", text_3);
        } else if (state.total_count > 0) {
            e->SetInnerRML(evo_fmt("%d%s", state.total_count,
                                   state.total_count == 1 ? " ITEM" : " ITEMS"));
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

    ShowOnlyScreen(m_list_doc);

    if (m_nav_doc) {
        if (m_last_nav.visible) m_nav_doc->Show();
        else                    m_nav_doc->Hide();
    }

    RenderCachedScreen(1, framebuffer, width, height);
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

    /* PlayStation Obsidian & Sapphire Blue theme for storage browser */
    const std::string accent       = "#00cdff"; // Sapphire cyan/blue
    const std::string accent_gold  = "#ffcd00"; // Amber gold for folders & favorites
    const std::string text_1       = "#ffffff"; // Crisp white
    const std::string text_2       = "#c2d2e8"; // Ice blue-white
    const std::string text_3       = "#7e97b8"; // Slate blue-gray

    auto el = [&](const std::string& id) { return m_browser_doc->GetElementById(id); };

    /* 1. Header Toolbar */
    if (Rml::Element* e = el("browser-indicator")) e->SetProperty("background-color", accent);
    if (Rml::Element* e = el("browser-badge-icon")) SetImageColor(e, accent);
    if (Rml::Element* e = el("browser-title")) e->SetInnerRML(state.title);
    if (Rml::Element* e = el("browser-path")) {
        e->SetInnerRML(state.path);
        e->SetProperty("color", text_2);
    }
    if (Rml::Element* e = el("browser-counter")) {
        std::string counter =
            (state.total_count > 0 && state.cursor_index >= 0)
                ? evo_fmt("%d / %d", state.cursor_index + 1, state.total_count)
                : evo_fmt("%d%s", state.total_count,
                          state.total_count == 1 ? " ITEM" : " ITEMS");
        e->SetInnerRML(counter);
    }

    /* 2a. Folder filter chips.
     *
     * Only the chips this folder earned are shown, so the row is empty - and
     * hidden with it - whenever there is nothing to choose between. The fill
     * marks the active filter and the ring marks the cursor; they are separate
     * because the cursor can sit on a chip without it being applied yet. */
    {
        const bool any = !state.filter_labels.empty();
        if (Rml::Element* row = el("browser-filter-row"))
            row->SetProperty("display", any ? "flex" : "none");
        if (Rml::Element* hint = el("browser-filter-hint"))
            hint->SetProperty("display", state.filter_focused ? "none" : "block");
        for (int i = 0; i < 4; i++) {
            Rml::Element* chip = el("browser-chip-" + std::to_string(i));
            if (!chip) continue;
            const bool shown = i < static_cast<int>(state.filter_labels.size());
            chip->SetProperty("display", shown ? "inline-block" : "none");
            if (!shown) continue;
            chip->SetInnerRML(state.filter_labels[i]);
            const bool on = (i == state.filter_selected);
            chip->SetClass("browser-chip-on", on);
            chip->SetClass("browser-chip-focused", state.filter_focused && on);
            if (on) {
                chip->SetProperty("background-color", to_hex_rgb(m_theme.accent));
                chip->SetProperty("color", ink_on(m_theme.accent));
            } else {
                chip->SetProperty("background-color", "#101a2a");
                chip->SetProperty("color", "#b2c3dc");
            }
            /* The ring comes from .browser-chip-focused; setting it here as
             * well just made the two disagree. */
        }
    }

    /* 2b. Left Sidebar (Sources) */
    for (int i = 0; i < 5; i++) {
        const std::string n = std::to_string(i);
        Rml::Element* item = el("sb-item-" + n);
        Rml::Element* icon = el("sb-icon-" + n);
        if (!item) continue;

        bool active = (i == state.active_source);
        bool focused = (state.sidebar_focused && i == state.sidebar_index);

        item->SetClass("sb-item-active", active);
        item->SetClass("sb-item-focused", focused);

        if (icon) {
            if (focused) {
                SetImageColor(icon, accent);
            } else if (active) {
                SetImageColor(icon, accent);
            } else {
                SetImageColor(icon, text_3);
            }
        }
    }

    /* 3. Empty State & Media Grid Cards */
    if (Rml::Element* e = el("browser-grid"))
        e->SetProperty("display", state.is_empty ? "none" : "flex");
    if (Rml::Element* e = el("browser-empty"))
        e->SetProperty("display", state.is_empty ? "flex" : "none");
    if (state.is_empty) {
        if (Rml::Element* e = el("browser-empty-title")) e->SetInnerRML(state.empty_title);
        if (Rml::Element* e = el("browser-empty-hint"))  e->SetInnerRML(state.empty_hint);
        if (Rml::Element* e = el("browser-empty-icon"))  SetImageColor(e, text_3);
    }

    std::string prev = ArtSource(kBrowserArtSlot, state.ins_preview,
                                 state.ins_preview_w, state.ins_preview_h,
                                 state.ins_name);

    for (int i = 0; i < kBrowserRows; i++) {
        const std::string n = std::to_string(i);
        Rml::Element* card     = el("brow-" + n);
        Rml::Element* art      = el("brow-art-" + n);
        Rml::Element* icon     = el("brow-icon-" + n);
        Rml::Element* name     = el("brow-name-" + n);
        Rml::Element* detail   = el("brow-detail-" + n);
        Rml::Element* badge    = el("brow-badge-" + n);
        Rml::Element* fav      = el("brow-fav-" + n);
        Rml::Element* duration = el("brow-duration-" + n);
        Rml::Element* track    = el("brow-track-" + n);
        Rml::Element* fill     = el("brow-fill-" + n);
        Rml::Element* kind     = el("brow-kind-" + n);
        Rml::Element* ext      = el("brow-ext-" + n);

        if (!card) continue;

        if (i >= (int)state.rows.size()) {
            card->SetProperty("display", "none");
            ArtSource(kBrowserCardArtSlot + i, nullptr, 0, 0, std::string());
            continue;
        }

        const EvoBrowserRow& r = state.rows[i];
        bool focused = r.is_focused && !state.rail_focused && !state.sidebar_focused;

        card->SetProperty("display", "flex");
        card->SetClass("grid-card-focused", focused);

        std::string card_art = ArtSource(kBrowserCardArtSlot + i, r.art, r.art_w, r.art_h, r.name);

        if (art && icon) {
            if (!card_art.empty()) {
                art->SetProperty("display", "block");
                art->SetProperty("decorator", "image(" + card_art + " cover)");
                icon->SetProperty("display", "none");
            } else if (focused && !prev.empty()) {
                art->SetProperty("display", "block");
                art->SetProperty("decorator", "image(" + prev + " cover)");
                icon->SetProperty("display", "none");
            } else {
                art->SetProperty("display", "none");
                icon->SetProperty("display", "inline-block");
                if (!r.icon_path.empty()) icon->SetAttribute("src", r.icon_path);
                bool isFolder = (r.icon_path.find("folder") != std::string::npos ||
                                 r.icon_path.find("usb") != std::string::npos);
                SetImageColor(icon, isFolder ? accent_gold : text_3);
            }
        }

        if (name) {
            name->SetInnerRML(r.name);
        }
        if (detail) {
            detail->SetProperty("display", r.detail.empty() ? "none" : "block");
            detail->SetInnerRML(r.detail);
        }
        if (badge) {
            if (r.badge.empty()) {
                badge->SetProperty("display", "none");
            } else {
                badge->SetProperty("display", "inline-block");
                badge->SetInnerRML(r.badge);
            }
        }
        if (fav) {
            fav->SetProperty("display", r.is_favorite ? "inline-block" : "none");
            SetImageColor(fav, accent_gold);
        }
        if (duration) {
            duration->SetProperty("display", r.duration.empty() ? "none" : "block");
            duration->SetInnerRML(r.duration);
        }
        if (track) {
            track->SetProperty("display", r.progress >= 0 ? "block" : "none");
        }
        if (fill) {
            fill->SetProperty("width", pct_string(r.progress));
        }
        if (kind) {
            kind->SetInnerRML(r.badge.empty() ? "MEDIA" : r.badge);
        }
        if (ext) {
            size_t dot = r.name.find_last_of('.');
            if (dot != std::string::npos && dot + 1 < r.name.size()) {
                std::string e = r.name.substr(dot + 1);
                std::transform(e.begin(), e.end(), e.begin(), ::toupper);
                ext->SetInnerRML(e);
                ext->SetProperty("display", "inline-block");
            } else {
                ext->SetInnerRML(r.badge == "DIR" || r.badge == "FOLDER" ? "DIR" : "FILE");
                ext->SetProperty("display", "inline-block");
            }
        }
    }

    /* 4. Bottom Status & Selection Strip */
    if (Rml::Element* e = el("ins-name")) {
        e->SetInnerRML(state.ins_name.empty() ? "No Selection" : state.ins_name);
    }
    if (Rml::Element* e = el("status-pill-res")) {
        e->SetProperty("display", state.status_res.empty() ? "none" : "inline-block");
        e->SetInnerRML(state.status_res);
    }
    if (Rml::Element* e = el("status-pill-vcodec")) {
        e->SetProperty("display", state.status_vcodec.empty() ? "none" : "inline-block");
        e->SetInnerRML(state.status_vcodec);
    }
    if (Rml::Element* e = el("status-pill-acodec")) {
        e->SetProperty("display", state.status_acodec.empty() ? "none" : "inline-block");
        e->SetInnerRML(state.status_acodec);
    }
    if (Rml::Element* e = el("status-pill-duration")) {
        e->SetProperty("display", state.status_duration.empty() ? "none" : "inline-block");
        e->SetInnerRML(state.status_duration);
    }
    if (Rml::Element* e = el("status-pill-size")) {
        e->SetProperty("display", state.status_size.empty() ? "none" : "inline-block");
        e->SetInnerRML(state.status_size);
    }
    if (Rml::Element* e = el("ins-probing")) {
        e->SetProperty("display", state.ins_probing ? "inline-block" : "none");
    }

    if (Rml::Element* e = el("footer-action-label")) {
        if (state.sidebar_focused) {
            e->SetInnerRML("SELECT");
        } else if (state.ins_kind == "Folder" || state.ins_kind == "FOLDER" || state.ins_kind == "DIR") {
            e->SetInnerRML("OPEN");
        } else {
            e->SetInnerRML("PLAY");
        }
    }
    if (Rml::Element* e = el("bhint-back")) {
        e->SetProperty("display", (state.at_root && state.sidebar_focused) ? "none" : "flex");
    }
}

void EvoRmlApp::RenderBrowser(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_browser_doc || !framebuffer) return;

    ShowOnlyScreen(m_browser_doc);

    if (m_nav_doc) {
        if (m_last_nav.visible) m_nav_doc->Show();
        else                    m_nav_doc->Hide();
    }

    RenderCachedScreen(2, framebuffer, width, height);
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
        e->SetInnerRML(
            evo_fmt("%d OF %d", state.cursor_index + 1, state.release_total));
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
            e->SetInnerRML(evo_fmt("+ %d MORE IN THIS RELEASE", hidden));
            e->SetProperty("display", "block");
            e->SetProperty("color", text_3);
        } else {
            e->SetProperty("display", "none");
        }
    }
}

void EvoRmlApp::RenderChangelog(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_changelog_doc || !framebuffer) return;

    ShowOnlyScreen(m_changelog_doc);

    if (m_nav_doc) {
        if (m_last_nav.visible) m_nav_doc->Show();
        else                    m_nav_doc->Hide();
    }

    RenderCachedScreen(3, framebuffer, width, height);
}

/* ==========================================================================
 * Text reader — a single scrolling pane over a fixed pool of line elements.
 * ========================================================================== */

void EvoRmlApp::UpdateReaderState(const EvoReaderState& state) {
    if (!m_initialized || !m_reader_doc) return;
    if (state == m_last_reader && m_theme_generation == m_theme_gen_reader) return;
    m_theme_gen_reader = m_theme_generation;
    m_frame_dirty = true;
    m_last_reader = state;

    const std::string accent  = to_hex_rgb(m_theme.accent);
    const std::string surface = to_hex_rgba(m_theme.surface);
    const std::string border  = to_hex_rgba(m_theme.border);
    const std::string text_1  = to_hex_rgb(m_theme.text_primary);
    const std::string text_2  = to_hex_rgb(m_theme.text_secondary);
    const std::string text_3  = to_hex_rgb(m_theme.text_muted);

    auto el = [&](const std::string& id) { return m_reader_doc->GetElementById(id); };

    if (Rml::Element* e = el("reader-title"))     e->SetInnerRML(state.title);
    if (Rml::Element* e = el("reader-subtitle"))  e->SetInnerRML(state.subtitle);
    if (Rml::Element* e = el("reader-indicator")) e->SetProperty("background-color", accent);
    if (Rml::Element* e = el("reader-badge")) {
        e->SetInnerRML(state.badge);
        e->SetProperty("color", text_3);
    }

    bool show_notice = !state.notice.empty();
    if (Rml::Element* e = el("reader-notice")) {
        e->SetProperty("display", show_notice ? "block" : "none");
        if (show_notice) {
            e->SetInnerRML(state.notice);
            e->SetProperty("color", text_2);
        }
    }
    if (Rml::Element* e = el("reader-lines"))     e->SetProperty("display", show_notice ? "none" : "flex");
    if (Rml::Element* e = el("reader-scrollbar")) {
        e->SetProperty("display", show_notice ? "none" : "flex");
        e->SetProperty("background-color", surface);
        e->SetProperty("border-color", border);
    }

    for (int i = 0; i < kReaderLines; i++) {
        Rml::Element* line = el("rline-" + std::to_string(i));
        if (!line) continue;

        if (show_notice || i >= (int)state.lines.size()) {
            line->SetProperty("display", "none");
            continue;
        }
        line->SetProperty("display", "block");
        line->SetClass("rline-face-0", state.face == 0);
        line->SetClass("rline-face-1", state.face == 1);
        line->SetClass("rline-face-2", state.face != 0 && state.face != 1);
        line->SetInnerRML(state.lines[i]);
        line->SetProperty("color", text_1);
    }

    if (Rml::Element* thumb = el("reader-scrollbar-thumb")) {
        if (show_notice) {
            thumb->SetProperty("display", "none");
        } else {
            double visible_frac = state.visible_frac;
            if (visible_frac < 0.02) visible_frac = 0.02;
            if (visible_frac > 1.0)  visible_frac = 1.0;

            if (visible_frac >= 1.0) {
                thumb->SetProperty("display", "none");
            } else {
                double top_pct = state.progress * (1.0 - visible_frac) * 100.0;
                thumb->SetProperty("display", "block");
                thumb->SetProperty("height", evo_fmt("%g%%", visible_frac * 100.0));
                thumb->SetProperty("top", evo_fmt("%g%%", top_pct));
                thumb->SetProperty("background-color", accent);
            }
        }
    }

    if (Rml::Element* e = el("reader-footnote")) {
        bool has_footnote = !state.footnote.empty();
        e->SetProperty("display", has_footnote ? "block" : "none");
        if (has_footnote) {
            e->SetInnerRML(state.footnote);
            e->SetProperty("color", text_3);
        }
    }
}

/* #81: full-screen image viewer. */
void EvoRmlApp::UpdateImageState(const EvoImageState& state) {
    if (!m_initialized || !m_image_doc) return;
    if (state == m_last_image && m_theme_generation == m_theme_gen_image) return;
    m_theme_gen_image = m_theme_generation;
    m_frame_dirty = true;
    m_last_image = state;

    auto el = [&](const char* id) { return m_image_doc->GetElementById(id); };
    if (Rml::Element* e = el("img-accent"))
        e->SetProperty("background-color", to_hex_rgb(m_theme.accent));
    if (Rml::Element* e = el("img-title"))
        e->SetInnerRML(state.title.empty() ? "IMAGE" : state.title);
    if (Rml::Element* e = el("img-dims"))
        e->SetInnerRML(state.dims.empty() ? "IMAGE FILE" : state.dims);

    std::string src = ArtSource(kImageArtSlot,
                                state.loaded ? state.pixels : nullptr,
                                state.w, state.h, state.title);
    if (Rml::Element* e = el("img-canvas")) {
        e->SetProperty("display", state.loaded ? "block" : "none");
        if (state.loaded && !src.empty())
            e->SetProperty("decorator", "image(" + src + " contain)");
    }
    if (Rml::Element* e = el("img-error"))
        e->SetProperty("display", state.loaded ? "none" : "flex");
}

void EvoRmlApp::RenderImage(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_image_doc || !framebuffer) return;

    ShowOnlyScreen(m_image_doc);
    if (m_nav_doc) m_nav_doc->Hide();   /* full-bleed viewer: no rail */

    RenderCachedScreen(11, framebuffer, width, height);
}

void EvoRmlApp::RenderReader(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_reader_doc || !framebuffer) return;

    ShowOnlyScreen(m_reader_doc);

    if (m_nav_doc) {
        if (m_last_nav.visible) m_nav_doc->Show();
        else                    m_nav_doc->Hide();
    }

    RenderCachedScreen(9, framebuffer, width, height);
}

/* ==========================================================================
 * Surround Sound Studio (#106) - a 2.5D room soundstage. Stage coordinates
 * arrive as dp from the listener (+y to the rear); the room model behind them
 * is core/include/evo/audio/SpatialField.hpp. Everything drawn here is state
 * the screen and its services own - the gain matrix, the orb, the telemetry,
 * the calibration run - nothing is invented on this side.
 * ========================================================================== */

namespace {

const char* const kSrdCyan   = "#00cdff";
const char* const kSrdYellow = "#ffcd00";
const char* const kSrdRed    = "#ff5a4f";

/*
 * This screen animates every frame, so it must not pay for what didn't
 * change: RmlUi re-lays-out the whole document when width / height / display
 * or any text is touched, even with the same value. Everything below goes
 * through a last-value cache, positions are whole dp, and the growing rings
 * scale with `transform` (no layout) instead of width/height. Measured: the
 * uncached version ran this screen at ~10 fps on the console.
 */
std::unordered_map<std::string, std::string> g_srd_last;   /* element+prop -> value */
std::unordered_map<std::string, Rml::Element*> g_srd_els;  /* id -> element */

std::string srd_key(Rml::Element* e, const char* prop) {
    std::string k(reinterpret_cast<const char*>(&e), sizeof(e));
    k += prop;
    return k;
}

void srd_set(Rml::Element* e, const char* prop, const std::string& v) {
    if (!e) return;
    std::string& last = g_srd_last[srd_key(e, prop)];
    if (!last.empty() && last.compare(1, std::string::npos, v) == 0) return;
    last = "=" + v;
    e->SetProperty(prop, v);
}

void srd_text(Rml::Element* e, const std::string& t) {
    if (!e) return;
    /* stored with a '=' prefix so "" is distinguishable from never-set */
    std::string& last = g_srd_last[srd_key(e, "#text")];
    if (!last.empty() && last.compare(1, std::string::npos, t) == 0) return;
    last = "=" + t;
    e->SetInnerRML(t);
}

void srd_attr(Rml::Element* e, const char* name, const std::string& v) {
    if (!e) return;
    std::string& last = g_srd_last[srd_key(e, name)];
    if (!last.empty() && last.compare(1, std::string::npos, v) == 0) return;
    last = "=" + v;
    e->SetAttribute(name, v);
}

void srd_class(Rml::Element* e, const char* cls, bool on) {
    if (e && e->IsClassSet(cls) != on) e->SetClass(cls, on);
}

std::string srd_dp(double v) { return evo_fmt("%.0fdp", std::floor(v + 0.5)); }

std::string srd_cyan(double alpha) {
    if (alpha < 0.0) alpha = 0.0;
    if (alpha > 1.0) alpha = 1.0;
    /* #rrggbbaa: RmlUi's rgba() takes alpha as 0-255, not 0-1 */
    return evo_fmt("#00cdff%02x", (int)(alpha * 255.0 + 0.5));
}

void srd_box(Rml::Element* e, double l, double t, double w, double h) {
    if (!e) return;
    srd_set(e, "left", srd_dp(l));
    srd_set(e, "top", srd_dp(t));
    srd_set(e, "width", srd_dp(w));
    srd_set(e, "height", srd_dp(h));
}

void srd_circle(Rml::Element* e, double cx, double cy, double d) {
    if (!e) return;
    d = std::floor(d + 0.5);
    srd_box(e, cx - d * 0.5, cy - d * 0.5, d, d);
    srd_set(e, "border-radius", srd_dp(d * 0.5));
}

/* A fixed-size ring (its RCSS size = `base`) centred at cx,cy and scaled to
 * diameter d - moves and grows without a relayout. */
void srd_ring(Rml::Element* e, double cx, double cy, double base, double d) {
    if (!e) return;
    srd_set(e, "left", srd_dp(cx - base * 0.5));
    srd_set(e, "top", srd_dp(cy - base * 0.5));
    srd_set(e, "transform", evo_fmt("scale(%.3f)", d / base));
}

void srd_show(Rml::Element* e, bool on, const char* display = "block") {
    srd_set(e, "display", on ? display : "none");
}

/* VU segments lit for a 0..1 level: graduated, never binary. */
int srd_vu_segments(float level) {
    if (level > 0.80f) return 4;
    if (level > 0.55f) return 3;
    if (level > 0.30f) return 2;
    if (level > 0.10f) return 1;
    return 0;
}

} // namespace

void EvoRmlApp::UpdateSurroundState(const EvoSurroundState& st) {
    if (!m_initialized || !m_surround_doc) return;
    if (st == m_last_surround && m_theme_generation == m_theme_gen_surround) return;
    m_theme_gen_surround = m_theme_generation;
    m_frame_dirty = true;
    m_last_surround = st;

    const evo_rmlui_surround_params_t& p = st.p;
    const std::string text_1 = to_hex_rgb(m_theme.text_primary);
    const std::string text_2 = to_hex_rgb(m_theme.text_secondary);
    const std::string text_3 = to_hex_rgb(m_theme.text_muted);

    auto el = [&](const std::string& id) -> Rml::Element* {
        auto it = g_srd_els.find(id);
        if (it != g_srd_els.end()) return it->second;
        Rml::Element* e = m_surround_doc->GetElementById(id);
        g_srd_els[id] = e;
        return e;
    };
    auto set_text = [&](const std::string& id, const std::string& text, const std::string& color) {
        if (Rml::Element* e = el(id)) {
            srd_text(e, text);
            if (!color.empty()) srd_set(e, "color", color);
        }
    };
    /* Numbers that move every frame (gains, telemetry) re-lay-out the page
     * when their text changes, so they refresh at 10 Hz, not per frame. */
    static float s_num_t = -1e9f;
    const bool num_tick = std::fabs(p.anim_time - s_num_t) >= 0.1f;
    if (num_tick) s_num_t = p.anim_time;
    auto set_num = [&](const std::string& id, const std::string& text, const std::string& color) {
        if (num_tick) set_text(id, text, color);
        else if (Rml::Element* e = el(id)) { if (!color.empty()) srd_set(e, "color", color); }
    };

    const bool is120 = (evo_agc_runtime_is_120hz() != 0);
    const bool is51 = (p.is_51_layout != 0);
    const bool v_orb = (p.view_mode == EVO_SURROUND_VIEW_ORB);
    const bool v_cal = (p.view_mode == EVO_SURROUND_VIEW_CALIBRATION);
    const bool v_stage = !v_orb && !v_cal;
    const bool measuring = v_cal && p.cal_phase == EVO_SURROUND_CAL_MEASURING;
    const int  n_spk = std::min(p.speaker_count, (int)EVO_RMLUI_SURROUND_SPEAKERS);
    static const char* kFlight[3] = { "MANUAL", "ORBIT", "FLYBY" };
    const char* flight = kFlight[(p.flight_mode >= 0 && p.flight_mode < 3) ? p.flight_mode : 0];

    auto label_of = [&](int ch) -> std::string {
        return (ch >= 0 && ch < n_spk) ? st.labels[ch] : std::string("-");
    };
    auto name_of = [&](int ch) -> std::string {
        return (ch >= 0 && ch < n_spk) ? st.names[ch] : std::string("-");
    };
    /* The channel emitting right now: the calibration sweep, else the tone. */
    const int emitting = v_cal ? (measuring ? p.cal_channel : -1) : p.active_channel;

    /* ---------------------------------------------------------- header */
    set_text("surround-subtitle",
             evo_fmt("%s \xC2\xB7 %s \xC2\xB7 48 kHz \xC2\xB7 %s",
                     is51 ? "5.1 SYSTEM" : "7.1 SYSTEM",
                     is51 ? "6 CHANNELS" : "8 CHANNELS",
                     is120 ? "120 HZ OUTPUT" : "60 HZ OUTPUT"),
             is120 ? kSrdCyan : text_2);

    /* --------------------------------------------------------- actions */
    static const char* kActionLabel[EVO_RMLUI_SURROUND_ACTIONS] = {
        "3D SOUND FIELD", "AUTO CALIBRATION (MIC)", "SPATIAL ORB - FREE ROAM",
        "360 ROTATION SWEEP", "AUTO TEST 5.1", "AUTO TEST 7.1",
        "SPEAKER LAYOUT", "SILENCE / STOP" };
    static const char* kActionSub[EVO_RMLUI_SURROUND_ACTIONS] = {
        "INTERACTIVE SPATIAL AUDIO TEST", "DUALSENSE MICROPHONE CALIBRATION",
        "POSITIONING TEST", "CIRCULAR SURROUND TEST", "6-CHANNEL SEQUENCE",
        "8-CHANNEL SEQUENCE", "", "STOP ALL AUDIO OUTPUT" };
    const int running = v_cal ? 1 : (v_orb ? (p.tone_follow ? 0 : 2) : -1);
    for (int i = 0; i < EVO_RMLUI_SURROUND_ACTIONS; i++) {
        const std::string n = std::to_string(i);
        Rml::Element* row = el("srd-action-" + n);
        if (!row) continue;
        const bool focused = v_stage && !p.rail_focused && p.selected_item == i;
        const bool live = (i == running);
        srd_class(row, "srd-action-focused", focused);
        srd_set(row, "border-color", focused ? kSrdYellow : (live ? kSrdCyan : "#1f3150"));
        const std::string sub = (i == 6)
            ? std::string(is51 ? "CURRENT: 5.1 SURROUND" : "CURRENT: 7.1 SURROUND")
            : std::string(kActionSub[i]);
        set_text("srd-action-label-" + n, kActionLabel[i], (focused || live) ? text_1 : text_2);
        set_text("srd-action-sub-" + n, sub, focused ? kSrdYellow : (live ? kSrdCyan : text_3));
    }

    /* ---------------------------------------------------------- status */
    std::string mon_title, mon_1, mon_2, mon_status;
    bool mon_live = false;
    if (v_cal) {
        mon_title = "AUTO CALIBRATION";
        mon_1 = "RECEIVER: DUALSENSE MIC @ SWEET SPOT";
        mon_2 = evo_fmt("SWEEPS: %d SPEAKERS + FRONT LEFT DRIFT CHECK", p.cal_total);
        static const char* kPhase[] = { "READY", "MIC CHECK", "MEASURING", "ANALYZING", "COMPLETE", "FAILED" };
        const int ph = (p.cal_phase >= 0 && p.cal_phase <= 5) ? p.cal_phase : 0;
        mon_status = evo_fmt("STATUS: [ %s ]", kPhase[ph]);
        mon_live = (ph >= 1 && ph <= 3);
    } else if (v_orb) {
        mon_title = p.tone_follow ? "3D SOUND FIELD" : "SPATIAL ORB - FREE ROAM";
        mon_1 = evo_fmt("FLIGHT MODE: %s", flight);
        mon_2 = (p.nearest_channel >= 0)
            ? evo_fmt("LOUDEST: %s (%s) \xC2\xB7 %d%% GAIN", name_of(p.nearest_channel).c_str(),
                      label_of(p.nearest_channel).c_str(),
                      (int)(p.proximity[p.nearest_channel] * 100.0f + 0.5f))
            : std::string("NO SPEAKERS IN THIS LAYOUT");
        if (p.tone_follow) {
            mon_live = p.field_playing != 0;
            mon_status = mon_live ? "STATUS: [ MUSIC PANNING ACROSS THE SPEAKERS ]"
                                  : "STATUS: [ PAUSED \xC2\xB7 CROSS TO PLAY ]";
        } else {
            mon_live = (p.active_channel >= 0);
            mon_status = mon_live ? evo_fmt("STATUS: [ TONE ON %s ]", label_of(p.active_channel).c_str())
                                  : std::string("STATUS: [ CROSS PLAYS THE LOUDEST SPEAKER ]");
        }
    } else if (p.selected_item >= EVO_RMLUI_SURROUND_ACTIONS) {
        const int ch = p.selected_item - EVO_RMLUI_SURROUND_ACTIONS;
        mon_title = evo_fmt("%s (%s)", name_of(ch).c_str(), label_of(ch).c_str());
        mon_1 = evo_fmt("TEST TONE: %.0f HZ \xC2\xB7 CHANNEL %d", (ch < n_spk) ? p.speakers[ch].hz : 0.0, ch);
        mon_2 = "D-PAD MOVES BETWEEN SPEAKERS BY ROOM POSITION";
        mon_live = (p.active_channel == ch);
        mon_status = mon_live ? "STATUS: [ EMITTING TONE ]" : "STATUS: [ CROSS TO TEST ]";
    } else {
        const int a = (p.selected_item >= 0 && p.selected_item < EVO_RMLUI_SURROUND_ACTIONS) ? p.selected_item : 0;
        mon_title = kActionLabel[a];
        mon_1 = (a == 6) ? std::string(is51 ? "LAYOUT: 5.1 (6 CHANNELS)" : "LAYOUT: 7.1 (8 CHANNELS)")
                         : std::string(kActionSub[a]);
        mon_2 = "D-PAD RIGHT: SELECT A SPEAKER ON THE STAGE";
        mon_live = (p.surround_mode == 1);
        const int now = p.sweep_rotation ? p.nearest_channel : p.active_channel;
        mon_status = mon_live ? evo_fmt("STATUS: [ %s \xC2\xB7 %s ]", p.sweep_rotation ? "SWEEPING" : "RUNNING",
                                        label_of(now).c_str())
                              : std::string("STATUS: [ IDLE ]");
    }
    if (Rml::Element* e = el("surround-monitor")) srd_set(e, "border-color", mon_live ? kSrdCyan : "#1f3150");
    set_text("surround-monitor-title", mon_title, text_1);
    set_text("surround-monitor-line1", mon_1, text_2);
    set_num("surround-monitor-line2", mon_2, text_2);
    set_text("surround-monitor-status", mon_status, mon_live ? kSrdCyan : text_3);

    /* ----------------------------------------------------------- stage */
    const double cx = 418.0, cy = 360.0;
    const double ppm = p.px_per_m > 1.0f ? p.px_per_m : 100.0;

    /* Perspective floor: rows closer together towards the far wall, columns
     * converging on it. Static, so built once. */
    if (Rml::Element* floor = el("srd-floor")) {
        if (floor->GetNumChildren() == 0) {
            const int rows = 12, cols = 11;
            const double y0 = 64.0, y1 = 694.0;
            auto row_y = [&](int i) { return y0 + (y1 - y0) * std::pow((double)i / (rows - 1), 1.6); };
            auto col_x = [&](int j, double t) { return cx + (j - cols / 2) * (44.0 + 62.0 * t); };
            for (int i = 0; i < rows; i++) {
                const double t = (double)i / (rows - 1);
                Rml::ElementPtr line = m_surround_doc->CreateElement("div");
                line->SetClass("srd-grid-h", true);
                srd_box(line.get(), col_x(0, t), row_y(i), col_x(cols - 1, t) - col_x(0, t), 1.0);
                floor->AppendChild(std::move(line));
            }
            for (int i = 0; i + 1 < rows; i++) {
                const double t = ((double)i + 0.5) / (rows - 1);
                for (int j = 0; j < cols; j++) {
                    Rml::ElementPtr seg = m_surround_doc->CreateElement("div");
                    seg->SetClass("srd-grid-v", true);
                    srd_box(seg.get(), col_x(j, t), row_y(i), 1.0, row_y(i + 1) - row_y(i));
                    floor->AppendChild(std::move(seg));
                }
            }
        }
    }

    for (int r = 1; r <= 3; r++) {
        const double rad = r * ppm;
        srd_circle(el("srd-ring-" + std::to_string(r)), cx, cy, rad * 2.0);
        /* on the front axis, just inside each ring - clear of every speaker */
        if (Rml::Element* lbl = el("srd-ring-label-" + std::to_string(r))) {
            srd_set(lbl, "left", srd_dp(cx + 6.0));
            srd_set(lbl, "top", srd_dp(cy - rad + 3.0));
        }
    }
    /* the person's head (10 dp into the 60 dp box) sits on the sweet spot */
    if (Rml::Element* e = el("srd-listener")) {
        srd_set(e, "left", srd_dp(cx - 30.0));
        srd_set(e, "top", srd_dp(cy - 10.0));
    }
    SetImageColor(el("srd-listener-chair"), "#3d5a85");
    SetImageColor(el("srd-listener-person"), v_cal ? std::string(kSrdCyan) : std::string("#e6f1ff"));
    if (Rml::Element* e = el("srd-listener-label")) {
        srd_set(e, "left", srd_dp(cx - 80.0));
        srd_set(e, "top", srd_dp(cy + 56.0));
    }

    /* Speakers: the gain matrix drives glow, ring and VU; the channel that is
     * really emitting runs full scale. */
    for (int i = 0; i < EVO_RMLUI_SURROUND_SPEAKERS; i++) {
        const std::string n = std::to_string(i);
        Rml::Element* node = el("srd-spk-" + n);
        Rml::Element* wave = el("srd-wave-" + n);
        if (!node) continue;
        if (i >= n_spk || p.speakers[i].hidden) {
            srd_show(node, false);
            srd_show(wave, false);
            continue;
        }
        const evo_rmlui_surround_speaker_t& spk = p.speakers[i];
        const bool tone = (emitting == spk.ch);
        const float gain = p.proximity[i];
        /* while calibrating, only the speaker being measured is live */
        const float level = tone ? 1.0f : (v_cal ? 0.0f : gain);
        const bool selected = v_stage && !p.rail_focused && p.selected_item == spk.item_idx;
        const double sx = cx + spk.dx, sy = cy + spk.dy;

        srd_show(node, true);
        srd_set(node, "left", srd_dp(sx - 62.0));
        srd_set(node, "top", srd_dp(sy - 31.0));
        srd_class(node, "srd-spk-selected", selected);
        if (Rml::Element* cab = el("srd-spk-cab-" + n)) {
            srd_set(cab, "border-color", selected ? std::string(kSrdYellow)
                             : tone ? std::string(kSrdCyan)
                             : (level > 0.1f ? srd_cyan(0.2 + 0.7 * level) : std::string("#223751")));
            srd_set(cab, "background-color", tone ? "#162846" : (level > 0.5f ? "#111e33" : "#0d1626"));
        }

        set_text("srd-spk-label-" + n, st.labels[i], (tone || level > 0.6f) ? kSrdCyan : text_1);
        std::string sub = evo_fmt("%.0f Hz", spk.hz);
        if (v_cal && p.cal_phase == EVO_SURROUND_CAL_COMPLETE)
            sub = !p.cal_detected[i] ? std::string("MISSED")
                : (spk.ch == 3) ? evo_fmt("%.1f ms", p.cal_delay_ms[i])
                : evo_fmt("%+.1f dB", p.cal_trim_db[i]);
        else if (v_orb)
            sub = evo_fmt("%d%%", (int)(gain * 100.0f + 0.5f));
        set_num("srd-spk-sub-" + n, sub, tone ? kSrdCyan : text_3);

        /* the icon: cabinet brightens with the gain, the woofer lights up
         * cyan (white when it is the one emitting) over a glow disc */
        SetImageColor(el("srd-spk-body-" + n),
                      tone ? std::string(kSrdCyan) : (level > 0.5f ? std::string("#9fdcf2") : std::string("#6f88aa")));
        SetImageColor(el("srd-spk-cone-" + n),
                      tone ? std::string("#ffffff") : (level > 0.3f ? std::string(kSrdCyan) : std::string("#6f88aa")));
        if (Rml::Element* g = el("srd-spk-glow-" + n))
            srd_set(g, "background-color", srd_cyan(tone ? 0.45 : (level > 0.1f ? 0.30 * level : 0.0)));

        static const char* kVuOn[5] = { "", "#3ddc97", "#00cdff", "#ffb020", "#ff4d4d" };
        const int lit = srd_vu_segments(level);
        for (int b = 1; b <= 4; b++)
            if (Rml::Element* bar = el("vu-" + n + "-" + std::to_string(b)))
                srd_set(bar, "background-color", b <= lit ? kVuOn[b] : "#18263a");

        /* one wavefront travelling from an emitting speaker to the seat */
        if (wave) {
            if (tone) {
                const double ph = std::fmod((double)p.anim_time * 1.2 + i * 0.13, 1.0);
                const double dx = cx - sx, dy = cy - sy;
                const double dist = std::max(1.0, std::sqrt(dx * dx + dy * dy));
                const double travel = 40.0 + (dist * 0.75 - 40.0) * ph;
                const double d = 40.0 + 70.0 * ph;
                srd_show(wave, true);
                srd_ring(wave, sx + dx / dist * travel, sy + dy / dist * travel, 110.0, d);
                srd_set(wave, "border-color", srd_cyan(0.8 * (1.0 - ph)));
                srd_set(wave, "background-color", srd_cyan(0.08 * (1.0 - ph)));
            } else {
                srd_show(wave, false);
            }
        }
    }

    /* The orb: ground position + elevation. Height lifts it off the floor,
     * scales it, and stretches the stem to a shadow that stays on the floor. */
    const double gx = cx + p.orb_x, gy = cy + p.orb_y;
    const double z = p.orb_z_m;
    const double lift = z * 40.0;
    const double vy = gy - lift;
    double scale = 1.0 + 0.16 * z;
    scale = std::max(0.72, std::min(1.28, scale));
    const double od = 26.0 * scale;
    const bool orb_bright = v_orb || v_cal || p.active_channel >= 0 || p.field_playing;

    srd_circle(el("srd-orb"), gx, vy, od);
    if (Rml::Element* core = el("srd-orb-core")) {
        srd_set(core, "width", srd_dp(od * 0.38));
        srd_set(core, "height", srd_dp(od * 0.38));
        srd_set(core, "border-radius", srd_dp(od * 0.19));
    }
    if (Rml::Element* e = el("srd-orb")) srd_set(e, "opacity", orb_bright ? "1" : "0.75");
    if (Rml::Element* g = el("srd-orb-glow")) {
        srd_circle(g, gx, vy, od * 2.3);
        srd_set(g, "background-color", srd_cyan(orb_bright ? 0.18 : 0.09));
    }
    if (Rml::Element* sh = el("srd-orb-shadow")) {
        const double w = 30.0 + 6.0 * std::fabs(z);
        const double h = w * 0.36;
        srd_box(sh, gx - w * 0.5, gy - h * 0.5 + 4.0, w, h);
        srd_set(sh, "border-radius", srd_dp(h * 0.5));
        srd_set(sh, "opacity", evo_fmt("%.2f", 0.9 - 0.3 * std::fabs(z) / 1.5));
    }
    if (Rml::Element* stem = el("srd-orb-stem")) {
        const bool on = std::fabs(lift) > 3.0;
        srd_show(stem, on);
        if (on) srd_box(stem, gx - 0.5, std::min(vy, gy), 1.0, std::fabs(lift));
    }
    for (int k = 0; k < 2; k++) {
        Rml::Element* rp = el(k == 0 ? "srd-orb-ripple-a" : "srd-orb-ripple-b");
        if (!rp) continue;
        const bool on = v_orb || measuring || p.active_channel >= 0 || p.field_playing;
        srd_show(rp, on);
        if (!on) continue;
        const double ph = std::fmod((double)p.anim_time * 0.9 + k * 0.5, 1.0);
        srd_ring(rp, gx, vy, 104.0, od * 1.2 + od * 2.8 * ph);
        srd_set(rp, "border-color", srd_cyan(0.45 * (1.0 - ph)));
    }
    if (Rml::Element* tag = el("srd-orb-tag")) {
        std::string t;
        if (v_cal) t = "DUALSENSE MIC";
        else if (v_orb && std::fabs(z) >= 0.005) t = evo_fmt("Z %+.2f m", z);
        srd_show(tag, !t.empty());
        srd_text(tag, t);
        srd_set(tag, "left", srd_dp(gx - 60.0));
        /* above the orb when it is parked on the listener, below otherwise */
        srd_set(tag, "top", srd_dp(v_cal ? vy - od * 0.5 - 22.0 : vy + od * 0.5 + 8.0));
    }

    for (int i = 0; i < EVO_RMLUI_SURROUND_TRAIL; i++) {
        Rml::Element* t = el("srd-trail-" + std::to_string(i));
        const bool on = (v_orb || p.sweep_rotation) && i < p.trail_count;
        srd_show(t, on);
        if (!on) continue;
        const double d = od * (0.62 - 0.09 * i);
        srd_circle(t, cx + p.trail_x[i], cy + p.trail_y[i] - lift, d);
        srd_set(t, "background-color", srd_cyan(0.34 * (1.0 - i / 5.0)));
    }

    /* Dotted arc: listener -> orb, or during a sweep, speaker -> microphone. */
    double ax = cx, ay = cy - 18.0, bx = gx, by = vy;
    bool arc_on = v_orb;
    if (measuring && p.cal_channel >= 0 && p.cal_channel < n_spk) {
        ax = cx + p.speakers[p.cal_channel].dx;
        ay = cy + p.speakers[p.cal_channel].dy;
        bx = gx; by = vy;
        arc_on = true;
    }
    const double adx = bx - ax, ady = by - ay;
    const double alen = std::sqrt(adx * adx + ady * ady);
    arc_on = arc_on && alen > 40.0;
    for (int k = 0; k < 8; k++) {
        Rml::Element* dot = el("srd-arc-" + std::to_string(k));
        srd_show(dot, arc_on);
        if (!arc_on) continue;
        const double t = (k + 1) / 9.0;
        const double bulge = std::sin(t * 3.14159265) * std::min(36.0, alen * 0.14);
        const double px = ax + adx * t + (ady / alen) * bulge;
        const double py = ay + ady * t - (adx / alen) * bulge;
        srd_circle(dot, px, py, 4.0);
        srd_set(dot, "background-color", srd_cyan(0.15 + 0.6 * t));
    }

    /* ------------------------------------------------------- telemetry */
    srd_show(el("srd-tele-cells"), !v_cal, "flex");
    srd_show(el("srd-cal-progress"), v_cal, "flex");
    if (!v_cal) {
        const std::string near = (p.nearest_channel >= 0) ? label_of(p.nearest_channel) : std::string("-");
        const std::string cells[8][2] = {
            { "AZIMUTH",   evo_fmt("%.0f\xC2\xB0", p.azimuth_deg) },
            { "ELEVATION", evo_fmt("%.0f\xC2\xB0", p.elevation_deg) },
            { "DISTANCE",  evo_fmt("%.1f m", p.distance_m) },
            { "SOURCE",    evo_fmt("%.1f dBFS", p.source_dbfs) },
            { "X",         evo_fmt("%+.2f m", p.x_m) },
            { "Y",         evo_fmt("%+.2f m", p.y_m) },
            { "Z",         evo_fmt("%+.2f m", p.orb_z_m) },
            { "LOUDEST",   near },
        };
        for (int i = 0; i < 8; i++) {
            set_text("srd-tele-k-" + std::to_string(i), cells[i][0], "");
            set_num("srd-tele-v-" + std::to_string(i), cells[i][1], (i == 7 && p.nearest_channel >= 0) ? kSrdCyan : text_1);
        }
    } else {
        std::string line;
        double frac = 0.0;
        std::string color = text_1;
        switch (p.cal_phase) {
        case EVO_SURROUND_CAL_MIC_CHECK:
            line = "CHECKING MICROPHONE + ROOM NOISE..."; frac = 0.05; break;
        case EVO_SURROUND_CAL_MEASURING:
            line = p.cal_verifying
                ? evo_fmt("VERIFYING: %s (CLOCK DRIFT CHECK)...", name_of(p.cal_channel).c_str())
                : evo_fmt("MEASURING: %s (%d/%d)...", name_of(p.cal_channel).c_str(), p.cal_step, p.cal_total);
            frac = 0.1 + 0.8 * (p.cal_total > 0 ? (double)(p.cal_step - (p.cal_verifying ? 0 : 1)) / p.cal_total : 0.0);
            color = kSrdCyan;
            break;
        case EVO_SURROUND_CAL_ANALYZING:
            line = "CALCULATING LEVELS, DISTANCES AND DELAYS..."; frac = 0.95; break;
        case EVO_SURROUND_CAL_COMPLETE:
            line = p.cal_applied ? "COMPLETE \xC2\xB7 APPLIED TO PLAYBACK" : "COMPLETE \xC2\xB7 SQUARE APPLIES IT TO PLAYBACK";
            frac = 1.0; color = kSrdCyan; break;
        case EVO_SURROUND_CAL_ERROR:
            line = "CALIBRATION STOPPED \xC2\xB7 TRIANGLE TO REPEAT"; color = kSrdRed; break;
        default:
            line = "PLACE THE DUALSENSE AT THE SWEET SPOT \xC2\xB7 CROSS TO START"; break;
        }
        set_text("srd-cal-progress-text", line, color);
        if (Rml::Element* f = el("srd-cal-progress-fill"))
            srd_set(f, "width", srd_dp(800.0 * std::max(0.0, std::min(1.0, frac))));
    }

    /* ---------------------------------------------------- right column */
    srd_show(el("srd-levels-panel"), !v_cal);
    srd_show(el("srd-controls-panel"), !v_cal);
    srd_show(el("srd-quick-panel"), !v_cal);
    srd_show(el("srd-output-panel"), !v_cal);
    srd_show(el("srd-cal-panel"), v_cal);

    for (int i = 0; i < EVO_RMLUI_SURROUND_SPEAKERS; i++) {
        const std::string n = std::to_string(i);
        const bool on = i < p.order_count;
        srd_show(el("srd-lvl-row-" + n), on, "flex");
        if (!on) continue;
        const int ch = p.order[i];
        const bool tone = (emitting == ch);
        const float level = tone ? 1.0f : ((ch >= 0 && ch < EVO_RMLUI_SURROUND_SPEAKERS) ? p.proximity[ch] : 0.0f);
        set_text("srd-lvl-name-" + n, label_of(ch), tone ? std::string(kSrdCyan) : text_1);
        if (Rml::Element* f = el("srd-lvl-fill-" + n)) {
            srd_set(f, "width", evo_fmt("%.0f%%", level * 100.0f));   /* whole % - rebuilt only when it moves */
            srd_set(f, "background-color", tone ? kSrdYellow : kSrdCyan);
        }
        set_num("srd-lvl-pct-" + n, evo_fmt("%d%%", (int)(level * 100.0f + 0.5f)), tone ? std::string(kSrdYellow) : text_2);
    }

    for (int i = 0; i < 3; i++) {
        if (Rml::Element* card = el("srd-qm-" + std::to_string(i))) {
            srd_class(card, "srd-qm-selected", i == p.flight_mode);
            srd_set(card, "opacity", v_orb ? "1" : "0.55");
        }
    }

    set_text("srd-out-line1", evo_fmt("LAYOUT  %s \xC2\xB7 S16 \xC2\xB7 48 kHz", is51 ? "5.1 \xC2\xB7 6 CH" : "7.1 \xC2\xB7 8 CH"), "");
    set_text("srd-out-line2", is120 ? "DISPLAY  120 HZ \xC2\xB7 8.33 ms FRAMES" : "DISPLAY  60 HZ \xC2\xB7 16.67 ms FRAMES", "");
    set_text("srd-out-line3", p.cal_profile_active ? "CALIBRATION  ACTIVE ON PLAYBACK" : "CALIBRATION  NOT SET",
             p.cal_profile_active ? kSrdCyan : text_3);

    if (v_cal) {
        int cur = 0;
        switch (p.cal_phase) {
        case EVO_SURROUND_CAL_MIC_CHECK: cur = 1; break;
        case EVO_SURROUND_CAL_MEASURING: cur = 2; break;
        case EVO_SURROUND_CAL_ANALYZING: cur = 3; break;
        case EVO_SURROUND_CAL_COMPLETE:  cur = 5; break;   /* all five done */
        default: cur = 0; break;
        }
        const bool failed = (p.cal_phase == EVO_SURROUND_CAL_ERROR);
        for (int s = 0; s < 5; s++) {
            const bool done = s < cur;
            const bool now = (s == cur) && !failed;
            if (Rml::Element* d = el("srd-cal-dot-" + std::to_string(s)))
                srd_set(d, "background-color", done ? kSrdCyan : (now ? kSrdYellow : "#2a3d5c"));
            if (Rml::Element* t = el("srd-cal-step-" + std::to_string(s)))
                srd_set(t, "color", now ? std::string(kSrdYellow) : (done ? text_1 : text_3));
        }

        std::string body;
        std::string body_color = text_1;
        switch (p.cal_phase) {
        case EVO_SURROUND_CAL_INTRO:
            body = "Place your DualSense controller on your seat at ear level, facing the screen. "
                   "Unmute its microphone, keep the room quiet, then press CROSS.";
            break;
        case EVO_SURROUND_CAL_MEASURING:
            body = "A short sweep plays from each speaker in turn. Stay still and keep the room quiet.";
            break;
        case EVO_SURROUND_CAL_ERROR:
            body = st.cal_message;
            body_color = kSrdRed;
            break;
        default:
            body = st.cal_message;
            break;
        }
        set_text("srd-cal-text", body, body_color);

        if (Rml::Element* f = el("srd-cal-mic-fill"))
            srd_set(f, "width", evo_fmt("%.0f%%", std::max(0.0f, std::min(1.0f, p.cal_mic_level)) * 100.0f));
        set_text("srd-cal-noise", p.cal_noise_db > -119.0f ? evo_fmt("NOISE %.0f dBFS", p.cal_noise_db) : std::string(""), "");

        const bool results = (p.cal_phase == EVO_SURROUND_CAL_COMPLETE);
        for (int i = 0; i < EVO_RMLUI_SURROUND_SPEAKERS; i++) {
            const std::string n = std::to_string(i);
            const bool on = i < p.order_count;
            srd_show(el("srd-cal-row-" + n), on, "flex");
            if (!on) continue;
            const int ch = p.order[i];
            const bool now = measuring && ch == p.cal_channel;
            set_text("srd-cal-c0-" + n, label_of(ch), now ? kSrdYellow : text_1);
            std::string c1 = "-", c2 = "-", c3 = "-";
            if (results && ch >= 0 && ch < EVO_RMLUI_SURROUND_SPEAKERS) {
                if (p.cal_detected[ch]) {
                    c1 = (ch == 3) ? std::string("NO TRIM") : evo_fmt("%+.1f dB", p.cal_trim_db[ch]);
                    c2 = evo_fmt("+%.2f m", p.cal_path_m[ch]);
                    c3 = evo_fmt("%.1f ms", p.cal_delay_ms[ch]);
                } else {
                    c1 = "NOT HEARD";
                }
            } else if (now) {
                c1 = "MEASURING";
            }
            const std::string col = (results && ch >= 0 && ch < EVO_RMLUI_SURROUND_SPEAKERS && !p.cal_detected[ch])
                                        ? std::string(kSrdRed) : text_2;
            set_text("srd-cal-c1-" + n, c1, now ? std::string(kSrdYellow) : col);
            set_text("srd-cal-c2-" + n, c2, text_2);
            set_text("srd-cal-c3-" + n, c3, text_2);
        }
        set_text("srd-cal-note",
                 "LEVEL = trim that balances each speaker at the seat. PATH = extra distance to the "
                 "nearest speaker; DELAY aligns every arrival. The subwoofer's level is left "
                 "alone - the controller mic hears too little deep bass to judge it.", "");
    }

    /* ---------------------------------------------------------- footer */
    struct Hint { const char* img; const char* pill; const char* text; };
    Hint hints[6] = {};
    int nh = 0;
    auto hint = [&](const char* img, const char* pill, const char* text) {
        if (nh < 6) hints[nh++] = Hint{ img, pill, text };
    };
    if (v_cal) {
        hint("btn_cross", nullptr, p.cal_phase == EVO_SURROUND_CAL_COMPLETE ? "NEXT" : "START");
        hint("btn_triangle", nullptr, "REPEAT");
        hint("btn_square", nullptr, "APPLY");
        hint("btn_circle", nullptr, "BACK");
    } else if (v_orb) {
        hint("btn_cross", nullptr, p.tone_follow ? (p.field_playing ? "PAUSE MUSIC" : "PLAY MUSIC") : "TEST TONE");
        hint("btn_lstick", nullptr, "MOVE SOURCE");
        hint(nullptr, "L1  R1", "HEIGHT");
        hint("btn_triangle", nullptr, "CHANGE MODE");
        hint("btn_square", nullptr, "RESET POSITION");
        hint("btn_circle", nullptr, "BACK");
    } else {
        hint("btn_cross", nullptr, p.selected_item >= EVO_RMLUI_SURROUND_ACTIONS ? "TEST TONE" : "SELECT");
        hint("btn_dpad", nullptr, "2D NAVIGATE");
        hint("btn_square", nullptr, "5.1 / 7.1");
        hint("btn_triangle", nullptr, "SILENCE");
        hint("btn_circle", nullptr, "BACK");
    }
    for (int i = 0; i < 6; i++) {
        const std::string n = std::to_string(i);
        const bool on = i < nh;
        srd_show(el("srd-hint-" + n), on, "flex");
        if (!on) continue;
        srd_show(el("srd-hint-badge-" + n), hints[i].img != nullptr, "flex");
        srd_show(el("srd-hint-pill-" + n), hints[i].pill != nullptr);
        if (hints[i].img)
            if (Rml::Element* img = el("srd-hint-img-" + n))
                srd_attr(img, "src", std::string("../icons/") + hints[i].img + ".png");
        if (hints[i].pill) set_text("srd-hint-pill-" + n, hints[i].pill, "");
        set_text("srd-hint-text-" + n, hints[i].text, "");
    }
}

void EvoRmlApp::RenderSurround(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_surround_doc || !framebuffer) return;

    ShowOnlyScreen(m_surround_doc);

    if (m_nav_doc) {
        if (m_last_nav.visible) m_nav_doc->Show();
        else                    m_nav_doc->Hide();
    }

    RenderCachedScreen(10, framebuffer, width, height);
}

/*
 * Scrub preview (#32): the frame at the position under the play head.
 *
 * prospero_thumbnail owns the decode - its own thread, its own format context
 * held open between requests, its own 32-entry cache - and coalesces repeated
 * requests, so asking on every rendered frame while the user drags is the
 * intended usage rather than an abuse of it. All this does is ask, and upload
 * whatever has landed.
 *
 * The worker decodes into one static buffer, so the pointer never changes and
 * ArtSource()'s pointer/dimension comparison would call every new preview
 * "unchanged". The serial it hands back rides along as the content tag, which
 * is what makes each new preview a new texture name.
 */
void EvoRmlApp::UpdateScrubPreview(const EvoPlaybackState& state)
{
    Rml::Element* el = m_playback_doc ? m_playback_doc->GetElementById("scrub-thumb")
                                      : nullptr;
    if (!el)
        return;

    if (!state.scrub_active) {
        if (m_scrub_thumb_shown) {
            el->SetProperty("display", "none");
            m_scrub_thumb_shown = false;
            MarkFrameDirty();
        }
        /* Drop the texture so the next scrub cannot flash the previous one
         * before its own first frame has decoded. */
        if (m_scrub_thumb_serial) {
            ArtSource(kScrubArtSlot, nullptr, 0, 0, std::string());
            m_scrub_thumb_serial = 0;
        }
        m_scrub_thumb_baseline = prospero_thumbnail_serial();
        return;
    }

    if (current_media_path[0])
        prospero_thumbnail_request(current_media_path, state.scrub_target, 1);

    const int words = PROSPERO_THUMB_W * PROSPERO_THUMB_H;
    if ((int)m_scrub_thumb_pixels.size() < words)
        m_scrub_thumb_pixels.resize((size_t)words);

    /* Cheap poll first: the worker is decoding in the background and most
     * rendered frames land on the same preview, so skip the 230 KB copy and
     * the texture upload unless it has actually published a new one. */
    const unsigned long long serial = prospero_thumbnail_serial();
    if (serial && serial != m_scrub_thumb_serial &&
        serial != m_scrub_thumb_baseline) {
        const unsigned long long got =
            prospero_thumbnail_snapshot(m_scrub_thumb_pixels.data(), words);
        if (got) {
            m_scrub_thumb_serial = got;
            const std::string src = ArtSource(kScrubArtSlot,
                                              m_scrub_thumb_pixels.data(),
                                              PROSPERO_THUMB_W, PROSPERO_THUMB_H,
                                              evo_fmt("%llu", got));
            if (!src.empty()) {
                el->SetAttribute("src", src);
                MarkFrameDirty();
            }
        }
    }

    /* Only show the box once there is something in it - an empty 320x180 hole
     * under the timecode looks like a bug, a capsule that grows when the
     * preview arrives does not. Set on change only: this runs every rendered
     * frame of a drag, and an unconditional SetProperty would re-dirty layout
     * on all of them. */
    const bool show = m_scrub_thumb_serial != 0;
    if (show != m_scrub_thumb_shown) {
        el->SetProperty("display", show ? "block" : "none");
        m_scrub_thumb_shown = show;
        MarkFrameDirty();
    }
}

void EvoRmlApp::UpdatePlaybackState(const EvoPlaybackState& state) {
    if (!m_initialized || !m_playback_doc) return;

    /*
     * Ahead of the unchanged-state early return below, because the preview is
     * decoded asynchronously: hold the stick still and scrub_target stops
     * moving, so the state compares equal and this function returns - while
     * the frame the user is waiting for is still being decoded and lands a
     * couple of hundred milliseconds later. Gating the upload on the state
     * having changed would drop exactly the frame they asked for.
     */
    UpdateScrubPreview(state);

    /* position_sec ticks every playing frame, so this only actually skips
     * while genuinely paused and idle - the theme-generation gate still
     * catches a theme switch during that pause. */
    if (state == m_last_state && m_theme_generation == m_theme_gen_playback) return;
    m_theme_gen_playback = m_theme_generation;
    m_frame_dirty = true;

    m_last_state = state;

    /* GL-5 (#81): caption-only mode — a subtitle cue is up but the playback
     * controls have faded. Suppress the scrims/capsule as a group so only
     * #subtitle-layer shows over the video. */
    {
        const char* scrim = state.chrome_hidden ? "none" : "flex";
        if (auto* e = m_playback_doc->GetElementById("top-scrim"))      e->SetProperty("display", scrim);
        if (auto* e = m_playback_doc->GetElementById("bottom-scrim"))   e->SetProperty("display", scrim);
        if (auto* e = m_playback_doc->GetElementById("center-overlay"))
            e->SetProperty("display", (state.chrome_hidden || state.music_mode) ? "none" : "flex");
    }

    /* #81: NOW PLAYING visualiser for audio-only playback. */
    {
        Rml::Element* mv = m_playback_doc->GetElementById("music-view");
        if (mv) {
            mv->SetProperty("display", state.music_mode ? "flex" : "none");
            if (state.music_mode) {
                mv->SetClass("paused", state.paused);
                if (auto* e = m_playback_doc->GetElementById("music-state"))
                    e->SetInnerRML(state.paused ? "PAUSED" : "PLAYING");
                if (auto* e = m_playback_doc->GetElementById("music-hint")) {
                    std::string h = (state.music_codec.empty() ? std::string("AUDIO")
                                                              : state.music_codec);
                    e->SetInnerRML(h + " \xC2\xB7 CROSS PAUSE \xC2\xB7 L/R SEEK");
                }
            }
        }
    }

    // 0. Subtitle caption overlay (#81), and the secondary line above it (#110)
    {
        // Caption text becomes RML: escape it, and turn line breaks into <br/>.
        auto set_caption = [](Rml::Element* box, const std::string& text) {
            if (!box) return;
            if (text.empty()) {
                box->SetProperty("display", "none");
                return;
            }
            std::string rml;
            rml.reserve(text.size() + 16);
            for (char c : text) {
                switch (c) {
                    case '&':  rml += "&amp;";  break;
                    case '<':  rml += "&lt;";   break;
                    case '>':  rml += "&gt;";   break;
                    case '\n': rml += "<br/>";  break;
                    case '\r': break;
                    default:   rml += c;        break;
                }
            }
            box->SetProperty("display", "inline-block");
            box->SetInnerRML(rml);
        };

        Rml::Element* el_sub_box   = m_playback_doc->GetElementById("subtitle-box");
        Rml::Element* el_sub_layer = m_playback_doc->GetElementById("subtitle-layer");
        if (el_sub_box && el_sub_layer) {
            set_caption(el_sub_box, state.subtitle_text);
            if (!state.subtitle_text.empty()) {
                el_sub_box->SetClass("sub-small",  state.subtitle_face == 1);
                el_sub_box->SetClass("sub-medium", state.subtitle_face == 2);
                el_sub_box->SetClass("sub-large",  state.subtitle_face == 3);
            }
            el_sub_layer->SetClass("raised", state.subtitle_raised);
        }

        // The secondary caption is stacked directly above the primary, or sits
        // alone at the top of the screen. Only the box for the chosen position
        // ever shows; it is one step smaller than the primary and has its own
        // colour, so the two lines never read as one.
        Rml::Element* el_sub2_box = m_playback_doc->GetElementById("subtitle-box-secondary");
        Rml::Element* el_sub2_top = m_playback_doc->GetElementById("subtitle-box-top");
        Rml::Element* el_top_layer = m_playback_doc->GetElementById("subtitle-layer-top");
        const bool on_top = state.subtitle2_position == 1;
        set_caption(el_sub2_box, on_top ? std::string() : state.subtitle2_text);
        set_caption(el_sub2_top, on_top ? state.subtitle2_text : std::string());
        for (Rml::Element* box : {el_sub2_box, el_sub2_top}) {
            if (!box) continue;
            box->SetClass("sub2-small",  state.subtitle_face == 1);
            box->SetClass("sub2-medium", state.subtitle_face == 2);
            box->SetClass("sub2-large",  state.subtitle_face == 3);
            box->SetClass("sub2-yellow", state.subtitle2_color == 0);
            box->SetClass("sub2-cyan",   state.subtitle2_color == 1);
            box->SetClass("sub2-white",  state.subtitle2_color == 2);
        }
        if (el_top_layer) el_top_layer->SetClass("raised", state.subtitle_raised);
    }

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

    // #59: decoder backend badge (Hardware / Software)
    if (Rml::Element* el_dec = m_playback_doc->GetElementById("badge-decoder")) {
        if (state.decoder_badge.empty()) {
            el_dec->SetProperty("display", "none");
        } else {
            el_dec->SetProperty("display", "inline-block");
            el_dec->SetInnerRML(state.decoder_badge);
            bool hw = state.decoder_badge.find("Hardware") != std::string::npos;
            el_dec->SetProperty("background-color", hw ? to_hex_rgb(m_theme.accent)
                                                       : to_hex_rgba(m_theme.surface));
            el_dec->SetProperty("color", hw ? ink_on(m_theme.accent) : "#e2e8f0");
        }
    }
    // #103: upscaler badge - hidden when upscaling is Off in Settings
    if (Rml::Element* el_up = m_playback_doc->GetElementById("badge-upscale")) {
        if (state.upscale_badge.empty()) {
            el_up->SetProperty("display", "none");
        } else {
            el_up->SetProperty("display", "inline-block");
            el_up->SetInnerRML(state.upscale_badge);
            el_up->SetClass("active", state.upscale_active);
        }
    }
    /* #81: dev FPS pill — independent of the OSD chrome (shows with controls faded). */
    if (Rml::Element* el_fps_pill = m_playback_doc->GetElementById("fps-pill")) {
        el_fps_pill->SetProperty("display", state.debug_overlay ? "block" : "none");
        if (Rml::Element* el_fps_val = m_playback_doc->GetElementById("fps-value"))
            el_fps_val->SetInnerRML(std::to_string(state.fps) + " FPS");
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
        el_fill->SetProperty("width", evo_fmt("%.1f%%", pct));
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
        /* The label carries its own colour in the stylesheet, so inheriting
         * from the badge is not enough - set it on the span itself. */
        if (Rml::Element* el_pause_lbl = m_playback_doc->GetElementById("pause-label"))
            el_pause_lbl->SetProperty("color", ink_on(m_theme.accent));
        /* A seek raises the engine's paused flag while it runs: LOADING says
         * what is really going on, PAUSED would be wrong. */
        if (state.paused && !state.scrub_active && !state.loading) {
            el_pause->SetProperty("display", "flex");
        } else {
            el_pause->SetProperty("display", "none");
        }
    }

    if (Rml::Element* el_load = m_playback_doc->GetElementById("loading-badge")) {
        el_load->SetProperty("border-color", to_hex_rgb(m_theme.border_sel));
        el_load->SetProperty("display", (state.loading && !state.scrub_active) ? "flex" : "none");
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

    /* Live TV: there is no timeline to show or seek through, and no chapters.
     * The meta line already reads LIVE. SUBS stays only if the stream has
     * subtitles to turn on. */
    {
        const bool no_subs = state.sub_track.empty() || state.sub_track == "None";
        const char* live_off = state.is_live ? "none" : "flex";
        static const char* const k_vod_only[] = {
            "item-seek", "item-chapter", "progress-container", "time-row" };
        for (const char* id : k_vod_only)
            if (Rml::Element* e = m_playback_doc->GetElementById(id))
                e->SetProperty("display", live_off);
        if (Rml::Element* e = m_playback_doc->GetElementById("item-subs"))
            e->SetProperty("display", (state.is_live && no_subs) ? "none" : "flex");
    }

    Rml::Element* el_subsync_item = m_playback_doc->GetElementById("item-subsync");
    Rml::Element* el_subsync = m_playback_doc->GetElementById("label-subsync");
    if (el_subsync_item && el_subsync) {
        if (state.sub_track.empty() || state.sub_track == "None") {
            el_subsync_item->SetProperty("display", "none");
        } else {
            el_subsync_item->SetProperty("display", "flex");
            char sync_buf[32];
            if (state.sub_delay_ms == 0) {
                std::snprintf(sync_buf, sizeof(sync_buf), "SYNC: 0 ms");
            } else {
                std::snprintf(sync_buf, sizeof(sync_buf), "SYNC: %+d ms", state.sub_delay_ms);
            }
            el_subsync->SetInnerRML(sync_buf);
        }
    }

    Rml::Element* el_aspect = m_playback_doc->GetElementById("label-aspect");
    if (el_aspect) {
        const char* vm = (state.view_mode == 0) ? "FIT" : ((state.view_mode == 1) ? "FILL" : "STRETCH");
        el_aspect->SetInnerRML(std::string("ASPECT: ") + vm);
    }

    // 7. Stats for Nerds HUD
    Rml::Element* el_stats = m_playback_doc->GetElementById("stats-hud");
    if (el_stats) {
        el_stats->SetProperty("display",
            (state.show_stats && !state.chrome_hidden) ? "flex" : "none");
    }
}

/* #81 / #63: the playback diagnostic HUD. Only called while the HUD is visible
 * (OPTIONS on the player), at ~2 Hz, so it writes the elements directly rather
 * than going through the state-diff dance. */
void EvoRmlApp::UpdatePerfHud(const evo_perf_hud_t* h) {
    if (!m_initialized || !m_playback_doc || !h) return;
    m_frame_dirty = true;

    auto set_line = [&](const char* id, const char* text) {
        if (Rml::Element* e = m_playback_doc->GetElementById(id))
            e->SetInnerRML(text && text[0] ? text : "--");
    };
    set_line("stats-video",  h->line_video);
    set_line("stats-audio",  h->line_audio);
    set_line("stats-subs",   h->line_subs);
    set_line("stats-perf",   h->line_perf);
    set_line("stats-queues", h->line_queues);
    set_line("stats-clocks", h->line_clocks);

    struct GraphSpec { const char* graph_id; const char* val_id; const char* colour;
                       const float* hist; float cur; float peak; const char* unit; };
    char gpu_v[48], ram_v[48], cpu_v[48];
    std::snprintf(gpu_v, sizeof gpu_v, "%.0f%% / %.0f", h->gpu_pct, h->gpu_peak_pct);
    std::snprintf(ram_v, sizeof ram_v, "%.0f / %.0fM", h->ram_mb, h->ram_total_mb);
    std::snprintf(cpu_v, sizeof cpu_v, "%.0f%% / %.0f", h->cpu_pct, h->cpu_peak_pct);
    const GraphSpec specs[3] = {
        { "graph-gpu", "graph-gpu-val", "#00d2ff", h->gpu_hist, h->gpu_pct, h->gpu_peak_pct, gpu_v },
        { "graph-ram", "graph-ram-val", "#ffb020", h->ram_hist, h->ram_mb,  h->ram_peak_mb,  ram_v },
        { "graph-cpu", "graph-cpu-val", "#00ffaa", h->cpu_hist, h->cpu_pct, h->cpu_peak_pct, cpu_v },
    };

    int n = h->hist_len;
    if (n < 0) n = 0;
    for (const GraphSpec& s : specs) {
        Rml::Element* g = m_playback_doc->GetElementById(s.graph_id);
        if (!g) continue;
        /* Lazily create the bar elements once, then only move their heights. */
        while ((int)g->GetNumChildren() < n) {
            Rml::ElementPtr bar = g->GetOwnerDocument()->CreateElement("div");
            bar->SetClass("graph-bar", true);
            bar->SetProperty("background-color", s.colour);
            g->AppendChild(std::move(bar));
        }
        for (int i = 0; i < (int)g->GetNumChildren(); i++) {
            Rml::Element* bar = g->GetChild(i);
            if (i >= n || !s.hist) { bar->SetProperty("height", "0px"); continue; }
            float v = s.hist[i];
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            char hbuf[16];
            std::snprintf(hbuf, sizeof hbuf, "%.0fpx", 2.0f + v * 30.0f);
            bar->SetProperty("height", hbuf);
        }
        if (Rml::Element* v = m_playback_doc->GetElementById(s.val_id))
            v->SetInnerRML(s.unit);
    }
}

void EvoRmlApp::RenderPlaybackOSD(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_playback_doc || !framebuffer) return;

    ShowOnlyScreen(m_playback_doc);
    if (m_nav_doc) m_nav_doc->Hide();   /* nothing over the film but the OSD */

    /* Scroll a long movie title. This document is not surface-cached, so the
     * per-frame re-render this relies on is already happening. */
    MarqueeTick(m_playback_doc->GetElementById("media-title"), true);

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);

    /* Overlay over live video — always render straight to the framebuffer. */
    EVO_PROF_CTX_RENDER();
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
            el_fill->SetProperty("width", evo_fmt("%.1f%%", pct));
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

                bool focused = (i == state.focused_action);
                el_btn->SetClass("btn-focused", focused);

                if (state.actions[i].is_primary) {
                    el_btn->SetProperty("background-color", to_hex_rgb(m_theme.accent));
                    el_btn->SetProperty("border-color", to_hex_rgb(m_theme.border_sel));
                    el_btn->SetProperty("color", ink_on(m_theme.accent));
                } else {
                    el_btn->SetProperty("background-color", to_hex_rgba(m_theme.surface));
                    el_btn->SetProperty("border-color", to_hex_rgba(m_theme.border));
                    el_btn->SetProperty("color", "#e2e8f0");
                }
                // #65: D-pad focus ring — a bright accent border over whatever
                // the primary/secondary style set.
                if (focused) {
                    el_btn->SetProperty("border-color", to_hex_rgb(m_theme.accent));
                    el_btn->SetProperty("border-width", "3px");
                } else {
                    el_btn->SetProperty("border-width", "2px");
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

    ShowOnlyScreen(m_dialog_doc);

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);

    /* Modal, sometimes over live video — always render straight to the framebuffer. */
    EVO_PROF_CTX_RENDER();
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
    Rml::Element* el_sicon = m_settings_doc->GetElementById("settings-badge-icon");
    if (el_sicon) {
        SetImageColor(el_sicon, to_hex_rgb(m_theme.accent_alt));
    }

    /* Two-pane sidebar. The section labels are static markup (sb-0 .. sb-4,
     * same order as kSections in SettingsScreen.cpp); only the highlight
     * moves. `sidebar_focused` is set by the index page, where the sidebar IS
     * the page; the section pages light the current row instead. */
    for (int sct = 0; sct < 5; sct++) {
        Rml::Element* el_s = m_settings_doc->GetElementById("sb-" + std::to_string(sct));
        if (!el_s) continue;
        const bool s_active = (sct == state.section_active);
        const bool s_focused = s_active && state.sidebar_focused;
        el_s->SetClass("sb-section-active", s_active && !s_focused);
        el_s->SetClass("sb-section-focused", s_focused);
        /* Paint inline as well as by class. A class change needs a style pass
         * to reach computed values, so on its own the highlight lands one
         * render late - measurable in uiview, where each fixture renders once.
         * The rail block below has always set its colours inline for the same
         * reason; this keeps the two consistent. */
        if (s_focused) {
            el_s->SetProperty("background-color", "#ffcd0026");
            el_s->SetProperty("border-color", to_hex_rgb(m_theme.accent));
        } else if (s_active) {
            el_s->SetProperty("background-color", "#ffffff14");
            el_s->SetProperty("border-color", "#ffffff24");
        } else {
            el_s->SetProperty("background-color", "transparent");
            el_s->SetProperty("border-color", "transparent");
        }
        if (Rml::Element* el_si = m_settings_doc->GetElementById("sb-icon-" + std::to_string(sct))) {
            SetImageColor(el_si, s_focused ? to_hex_rgb(m_theme.accent)
                                           : (s_active ? to_hex_rgb(m_theme.accent_alt)
                                                       : to_hex_rgb(m_theme.text_secondary)));
        }
        if (Rml::Element* el_sl = m_settings_doc->GetElementById("sb-label-" + std::to_string(sct)))
            el_sl->SetProperty("color", s_focused ? to_hex_rgb(m_theme.accent) : "#ffffff");
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

    for (int i = 0; i < EVO_RMLUI_SETTINGS_ROWS; i++) {
        std::string row_id = "row-" + std::to_string(i);
        std::string icon_id = "row-icon-" + std::to_string(i);
        std::string title_id = "row-title-" + std::to_string(i);
        std::string detail_id = "row-detail-" + std::to_string(i);
        std::string badge_id = "row-badge-" + std::to_string(i);
        std::string chev_id = "row-chevron-" + std::to_string(i);
        std::string tog_id = "row-toggle-" + std::to_string(i);
        std::string chk_id = "row-check-" + std::to_string(i);
        std::string ibox_id = "row-iconbox-" + std::to_string(i);

        Rml::Element* el_row = m_settings_doc->GetElementById(row_id);
        Rml::Element* el_icon = m_settings_doc->GetElementById(icon_id);
        Rml::Element* el_title = m_settings_doc->GetElementById(title_id);
        Rml::Element* el_detail = m_settings_doc->GetElementById(detail_id);
        Rml::Element* el_badge = m_settings_doc->GetElementById(badge_id);
        Rml::Element* el_chev = m_settings_doc->GetElementById(chev_id);
        Rml::Element* el_tog = m_settings_doc->GetElementById(tog_id);
        Rml::Element* el_chk = m_settings_doc->GetElementById(chk_id);
        Rml::Element* el_ibox = m_settings_doc->GetElementById(ibox_id);

        if (el_row) {
            if (i < (int)state.rows.size()) {
                el_row->SetProperty("display", "flex");
                bool is_focused = state.rows[i].is_focused && !state.rail_focused;
                el_row->SetClass("row-focused", is_focused);

                if (is_focused) {
                    /* Fill only: the separator hairlines are RCSS-owned so that
                     * .row-focused can swallow the one beneath the highlight. */
                    el_row->SetProperty("background-color", to_hex_rgba(m_theme.surface_sel));
                    if (el_detail) el_detail->SetProperty("color", to_hex_rgb(m_theme.accent));
                    if (el_badge) {
                        el_badge->SetProperty("background-color", "#ffcd0029");
                        el_badge->SetProperty("border-color", to_hex_rgb(m_theme.accent));
                        el_badge->SetProperty("color", to_hex_rgb(m_theme.accent));
                    }
                } else {
                    /* Transparent so the frosted card shows through the row. */
                    el_row->SetProperty("background-color", "transparent");
                    if (el_detail) el_detail->SetProperty("color", to_hex_rgb(m_theme.text_muted));
                    if (el_badge) {
                        el_badge->SetProperty("background-color", "#0a101c99");
                        el_badge->SetProperty("border-color", to_hex_rgba(m_theme.border));
                        el_badge->SetProperty("color", to_hex_rgb(m_theme.text_secondary));
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
                /* A toggle row owns the right-hand slot outright: no badge and
                 * no chevron, so the switch is the only affordance there. */
                const bool is_toggle = (state.rows[i].kind == EVO_RMLUI_ROW_TOGGLE);
                const bool is_option = (state.rows[i].kind == EVO_RMLUI_ROW_OPTION);
                el_row->SetClass("row-option", is_option);
                /* Inline, like the rest of this block: a class-only change lands a
                 * render late. */
                el_row->SetProperty("opacity", state.rows[i].is_disabled ? "0.4" : "1");
                /* Indent and strip the icon inline too - same one-render lag as
                 * the sidebar highlight if this were left to the class alone. */
                el_row->SetProperty("padding-left", is_option ? "76dp" : "18dp");
                if (el_ibox) el_ibox->SetProperty("display", is_option ? "none" : "flex");
                if (el_chk) {
                    const bool checked = is_option && state.rows[i].toggle_on;
                    el_chk->SetProperty("display", checked ? "block" : "none");
                    if (checked) el_chk->SetProperty("background-color", to_hex_rgb(m_theme.accent));
                }
                if (el_tog) {
                    el_tog->SetProperty("display", is_toggle ? "block" : "none");
                    el_tog->SetClass("row-toggle-on", is_toggle && state.rows[i].toggle_on);
                    /* One colour for every switch. This used to paint the
                     * theme accent inline while RCSS painted green, so a switch
                     * was green or purple depending on which won - set both
                     * ends to the same value and always set it inline, since a
                     * class-only change lands a render late. */
                    el_tog->SetProperty("background-color",
                                        state.rows[i].toggle_on ? "#2ecc71" : "#ffffff14");
                    el_tog->SetProperty("border-color",
                                        state.rows[i].toggle_on ? "#2ecc71" : "#ffffff29");
                }
                if (el_badge) {
                    if (is_toggle || is_option || state.rows[i].badge.empty()) {
                        el_badge->SetProperty("display", "none");
                    } else {
                        el_badge->SetProperty("display", "inline-block");
                        el_badge->SetInnerRML(state.rows[i].badge);
                    }
                }
                if (el_chev) {
                    const bool show_chev = state.rows[i].has_chevron && !is_toggle && !is_option &&
                                           !state.rows[i].is_disabled;
                    el_chev->SetProperty("display", show_chev ? "inline-block" : "none");
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

    ShowOnlyScreen(m_settings_doc);

    /* Nav rail rendered in the same pass — shown/hidden by UpdateNavState */
    if (m_nav_doc) {
        if (m_last_nav.visible)
            m_nav_doc->Show();
        else
            m_nav_doc->Hide();
    }

    RenderCachedScreen(6, framebuffer, width, height);
}

void EvoRmlApp::UpdateAboutState(const EvoAboutState& state) {
    if (!m_initialized || !m_about_doc) return;
    if (state == m_last_about && m_theme_generation == m_theme_gen_about) return;
    m_theme_gen_about = m_theme_generation;
    m_frame_dirty = true;

    m_last_about = state;

    if (Rml::Element* el = m_about_doc->GetElementById("about-title"))
        el->SetInnerRML(state.app_name);

    if (Rml::Element* el = m_about_doc->GetElementById("about-version-pill")) {
        std::string v = state.version.empty() ? ("v" + m_version) : state.version;
        el->SetInnerRML(v);
        el->SetProperty("background-color", to_hex_rgb(m_theme.accent));
        el->SetProperty("border-color", to_hex_rgb(m_theme.accent));
        el->SetProperty("color", ink_on(m_theme.accent));
    }

    if (Rml::Element* el = m_about_doc->GetElementById("about-build-tag"))
        el->SetInnerRML(state.build_tag);

    if (Rml::Element* el = m_about_doc->GetElementById("about-tagline"))
        el->SetInnerRML(state.tagline);

    if (Rml::Element* el = m_about_doc->GetElementById("about-themes-detail"))
        el->SetInnerRML(state.themes_info);

    if (Rml::Element* el_ind = m_about_doc->GetElementById("header-indicator"))
        el_ind->SetProperty("background-color", to_hex_rgb(m_theme.accent));

    if (Rml::Element* btn = m_about_doc->GetElementById("action-changelog")) {
        bool focused = state.action_focused;
        btn->SetClass("btn-focused", focused);
        if (focused) {
            btn->SetProperty("background-color", to_hex_rgba(m_theme.surface_sel));
            btn->SetProperty("border-color", to_hex_rgb(m_theme.accent));
        } else {
            btn->SetProperty("background-color", to_hex_rgba(m_theme.surface));
            btn->SetProperty("border-color", to_hex_rgba(m_theme.border));
        }
    }
}

void EvoRmlApp::RenderAbout(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_about_doc || !framebuffer) return;

    ShowOnlyScreen(m_about_doc);

    /* Nav rail rendered in the same pass — shown/hidden by UpdateNavState */
    if (m_nav_doc) {
        if (m_last_nav.visible)
            m_nav_doc->Show();
        else
            m_nav_doc->Hide();
    }

    RenderCachedScreen(7, framebuffer, width, height);
}

/*
 * Safe-to-close (closed.rml). No state to diff: it is drawn for a couple of
 * seconds and then latched by the parked frame loop, so it is simply themed
 * and rendered each time. Nav rail hidden - there is nowhere left to go.
 */
void EvoRmlApp::RenderClosed(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_closed_doc || !framebuffer) return;

    Rml::ElementDocument* d = m_closed_doc;
    const std::string accent = to_hex_rgb(m_theme.accent);
    auto set = [d](const char* id, const char* prop, const std::string& v) {
        if (Rml::Element* el = d->GetElementById(id)) el->SetProperty(prop, v);
    };

    if (Rml::Element* body = d->GetElementById("closed-body")) {
        body->SetProperty("background-color", to_hex_rgb(m_theme.bg_bottom));
        body->SetProperty("decorator", "vertical-gradient(" + to_hex_rgb(m_theme.bg_top) +
                                       " " + to_hex_rgb(m_theme.bg_bottom) + ")");
        body->SetProperty("color", to_hex_rgb(m_theme.text_primary));
    }
    /* A faint wash, not accent_soft: at accent_soft's alpha the ellipse
     * read as a hard-edged shape behind the card rather than a glow. */
    set("closed-glow", "background-color", accent + "0d");
    set("closed-card", "background-color", to_hex_rgba(m_theme.surface));
    set("closed-card", "border-color", to_hex_rgba(m_theme.border));
    set("closed-power-badge", "background-color", accent);
    set("closed-power-badge", "border-color", to_hex_rgb(m_theme.bg_bottom));
    set("closed-eyebrow", "color", accent);
    set("closed-title", "color", to_hex_rgb(m_theme.text_primary));
    set("closed-accent", "background-color", accent);
    set("closed-message", "color", to_hex_rgb(m_theme.text_secondary));
    set("closed-footer", "color", to_hex_rgb(m_theme.text_muted));
    if (Rml::Element* el = d->GetElementById("closed-power"))
        SetImageColor(el, ink_on(m_theme.accent));
    if (Rml::Element* el = d->GetElementById("closed-footer"))
        el->SetInnerRML("EVO PLAYER" + (m_version.empty() ? std::string() : "  v" + m_version));

    Rml::ElementList steps, nums, keys, strongs;
    d->GetElementsByClassName(steps, "closed-step");
    for (Rml::Element* el : steps) {
        el->SetProperty("background-color", to_hex_rgba(m_theme.surface_sel));
        el->SetProperty("border-color", to_hex_rgba(m_theme.border));
    }
    d->GetElementsByClassName(nums, "closed-step-num");
    for (Rml::Element* el : nums) {
        el->SetProperty("background-color", accent);
        el->SetProperty("color", ink_on(m_theme.accent));
    }
    d->GetElementsByClassName(keys, "closed-key");
    for (Rml::Element* el : keys) {
        el->SetProperty("border-color", to_hex_rgba(m_theme.border));
        el->SetProperty("color", to_hex_rgb(m_theme.text_primary));
    }
    d->GetElementsByClassName(strongs, "closed-strong");
    for (Rml::Element* el : strongs)
        el->SetProperty("color", accent);

    m_frame_dirty = true;
    ShowOnlyScreen(m_closed_doc);
    if (m_nav_doc) m_nav_doc->Hide();

    RenderCachedScreen(21, framebuffer, width, height);
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

    Rml::Element* el_sync_pill = m_subtitles_doc->GetElementById("subtitles-sync-pill");
    if (el_sync_pill) {
        el_sync_pill->SetProperty("border-color", to_hex_rgb(m_theme.border_sel));
    }

    Rml::Element* el_sync_lbl = m_subtitles_doc->GetElementById("subtitles-sync-label");
    if (el_sync_lbl) {
        el_sync_lbl->SetInnerRML(state.sync_str.empty() ? "SYNC: 0 ms" : state.sync_str);
        el_sync_lbl->SetProperty("color", to_hex_rgb(m_theme.accent));
    }

    Rml::Element* el_sync2_pill = m_subtitles_doc->GetElementById("subtitles-sync2-pill");
    if (el_sync2_pill) {
        el_sync2_pill->SetProperty("display", state.sync2_str.empty() ? "none" : "flex");
        el_sync2_pill->SetProperty("border-color", to_hex_rgb(m_theme.border_sel));
    }

    Rml::Element* el_sync2_lbl = m_subtitles_doc->GetElementById("subtitles-sync2-label");
    if (el_sync2_lbl) {
        el_sync2_lbl->SetInnerRML(state.sync2_str);
    }

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
        std::string tag_id = "sub-tag-" + std::to_string(i);

        Rml::Element* el_tag = m_subtitles_doc->GetElementById(tag_id);
        Rml::Element* el_row = m_subtitles_doc->GetElementById(row_id);
        Rml::Element* el_chk = m_subtitles_doc->GetElementById(chk_id);
        Rml::Element* el_dot = m_subtitles_doc->GetElementById(dot_id);
        Rml::Element* el_title = m_subtitles_doc->GetElementById(title_id);
        Rml::Element* el_detail = m_subtitles_doc->GetElementById(detail_id);

        if (el_row) {
            if (i < (int)state.tracks.size()) {
                el_row->SetProperty("display", "flex");
                el_row->SetClass("row-focused", state.tracks[i].is_focused);
                el_row->SetClass("row-action", state.tracks[i].is_action);
                el_row->SetClass("row-disabled", state.tracks[i].is_disabled);

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
                if (el_tag) {
                    // #110: which line of dialogue this track is.
                    const std::string& tag = state.tracks[i].tag;
                    el_tag->SetProperty("display", tag.empty() ? "none" : "inline-block");
                    el_tag->SetClass("tag-primary", tag == "PRIMARY");
                    el_tag->SetClass("tag-secondary", tag == "SECONDARY");
                    if (!tag.empty()) el_tag->SetInnerRML(tag);
                }
                if (el_detail) {
                    if (state.tracks[i].detail.empty()) {
                        el_detail->SetProperty("display", "none");
                    } else {
                        el_detail->SetProperty("display", "inline-block");
                        el_detail->SetInnerRML(state.tracks[i].detail);
                        if (state.tracks[i].is_focused) {
                            el_detail->SetProperty("background-color", to_hex_rgb(m_theme.accent));
                            el_detail->SetProperty("border-color", "#ffffff");
                            el_detail->SetProperty("color", ink_on(m_theme.accent));
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

    ShowOnlyScreen(m_subtitles_doc);

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);

    /* Over live video (film keeps playing) — always render straight to the framebuffer. */
    EVO_PROF_CTX_RENDER();
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

    Rml::Element* el_up = m_mediainfo_doc->GetElementById("spec-upscaler");
    if (el_up) el_up->SetInnerRML(state.upscaler);

    Rml::Element* el_dc = m_mediainfo_doc->GetElementById("spec-decoder");
    if (el_dc) el_dc->SetInnerRML(state.decoder);
}

void EvoRmlApp::RenderMediaInfo(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_mediainfo_doc || !framebuffer) return;

    ShowOnlyScreen(m_mediainfo_doc);
    if (m_nav_doc) m_nav_doc->Hide();   /* sits over the film */

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);

    /* Over live video — always render straight to the framebuffer. */
    EVO_PROF_CTX_RENDER();
}

void EvoRmlApp::UpdateNavState(const EvoNavState& state) {
    if (!m_initialized || !m_nav_doc) return;
    if (state == m_last_nav && m_theme_generation == m_theme_gen_nav) return;
    m_theme_gen_nav = m_theme_generation;
    m_frame_dirty = true;
    m_last_nav = state;

    /* FPS pill. Set inline, like every other state in this file that has to be
     * correct on the frame it changes. */
    if (Rml::Element* el_pill = m_nav_doc->GetElementById("nav-fps-pill")) {
        el_pill->SetProperty("display", state.show_fps ? "block" : "none");
        if (state.show_fps) {
            if (Rml::Element* el_v = m_nav_doc->GetElementById("nav-fps-value"))
                el_v->SetInnerRML(std::to_string(state.fps) + " FPS");
        }
    }

    /* ---- elevated-privileges banner ----
     * A condition, not an event: it stays up until the sandbox opens, which is
     * why this is a banner rather than the toast it started as. */
    if (Rml::Element* el_warn = m_nav_doc->GetElementById("nav-privilege-banner"))
        el_warn->SetProperty("display", state.storage_locked ? "flex" : "none");

    /* ---- collapsed icon rail ----
     *
     * navbar.rml carries a fixed five slots, Emby at slot 2. With Emby
     * compiled out (EVO_ENABLE_EMBY) ScreenManager numbers the remaining
     * sections 0..3, so the logical section has to be mapped onto the markup
     * slot that still holds its icon - otherwise Settings would drive the Emby
     * row. The unused slot is hidden so the rail closes up rather than leaving
     * a gap where Emby used to be.
     */
    const int rail_sections = EVO_ENABLE_EMBY ? 5 : 4;
    if (!EVO_ENABLE_EMBY) {
        if (Rml::Element* el_gap = m_nav_doc->GetElementById("nav-wrap-2"))
            el_gap->SetProperty("display", "none");
    }

    /* One entry past the sections: QUIT EVO, markup slot 5 at the foot of
     * the rail. It is never the active section, only ever the cursor. */
    for (int i = 0; i <= rail_sections; i++) {
        const int slot = (i == rail_sections) ? 5
                       : (!EVO_ENABLE_EMBY && i >= 2) ? i + 1 : i;
        std::string item_id  = "nav-item-" + std::to_string(slot);
        std::string bar_id   = "nav-bar-"  + std::to_string(slot);
        std::string icon_id  = "nav-icon-" + std::to_string(slot);

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
         * same trick as the settings/list focused badges. Section 2 (Emby) is
         * a trademark, excluded from the icon swap, so it keeps its own baked
         * colour rather than being retinted. */
        if (el_icon && i != 2) {
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

    /*
     * Same slot mapping as the collapsed rail above: navbar.rml has five fixed
     * expanded rows with Emby at slot 2, and with Emby compiled out the four
     * live sections are numbered 0..3.
     *
     * This panel was missed when the collapsed rail was fixed, and the host
     * preview did not catch it because uiview only renders the rail collapsed
     * - so Emby disappeared from the icon strip and stayed in the side nav the
     * moment anyone opened it.
     */
    if (!EVO_ENABLE_EMBY) {
        if (Rml::Element* el_gap = m_nav_doc->GetElementById("nav-exp-2"))
            el_gap->SetProperty("display", "none");
    }

    for (int i = 0; i <= rail_sections; i++) {   /* + QUIT EVO, slot 5 */
        const int eslot = (i == rail_sections) ? 5
                        : (!EVO_ENABLE_EMBY && i >= 2) ? i + 1 : i;
        std::string exp_id  = "nav-exp-"       + std::to_string(eslot);
        std::string lbl_id  = "nav-exp-label-" + std::to_string(eslot);
        std::string icon_id = "nav-exp-icon-"  + std::to_string(eslot);

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
            if (el_icon && i != 2) SetImageColor(el_icon, to_hex_rgb(m_theme.bg_bottom));
        } else if (is_active) {
            el_exp->SetProperty("background-color", to_hex_rgba(m_theme.surface_sel));
            el_exp->SetProperty("border-color", to_hex_rgba(m_theme.border));
            el_exp->SetProperty("border-width", "1px");
            if (el_lbl) el_lbl->SetProperty("color", to_hex_rgb(m_theme.text_primary));
            if (el_icon && i != 2) SetImageColor(el_icon, to_hex_rgb(m_theme.accent));
        } else {
            el_exp->SetProperty("background-color", "transparent");
            el_exp->SetProperty("border-color", "transparent");
            el_exp->SetProperty("border-width", "0px");
            if (el_lbl) el_lbl->SetProperty("color", to_hex_rgb(m_theme.text_secondary));
            if (el_icon && i != 2) SetImageColor(el_icon, to_hex_rgb(m_theme.text_secondary));
        }
    }
}

/* ==========================================================================
 * Toast notifications (#75)
 * ========================================================================== */

void EvoRmlApp::UpdateToastState(const EvoToastState& state) {
    if (!m_initialized || !m_toast_doc) return;

    if (!state.visible) {
        m_toast_doc->Hide();
        return;
    }

    /* Content only actually changes at the start of a toast (evo_toast.c
     * fires a fresh title/message/kind); alpha/slide tick every frame while
     * it's up. Diffing content separately from the per-frame animation
     * values avoids the SetInnerRML/SetAttribute calls (a real relayout)
     * firing every frame for no reason - the animation values below are
     * cheap inline-style pushes, fine to always re-apply. */
    bool content_changed = (state.title != m_toast_last_title ||
                            state.message != m_toast_last_message ||
                            state.kind != m_toast_last_kind);

    if (content_changed) {
        m_toast_last_title = state.title;
        m_toast_last_message = state.message;
        m_toast_last_kind = state.kind;

        Rml::Element* el_card = m_toast_doc->GetElementById("toast-card");
        Rml::Element* el_rail = m_toast_doc->GetElementById("toast-rail");
        Rml::Element* el_icon = m_toast_doc->GetElementById("toast-icon");
        Rml::Element* el_title = m_toast_doc->GetElementById("toast-title");
        Rml::Element* el_msg = m_toast_doc->GetElementById("toast-message");

        if (el_title) el_title->SetInnerRML(state.title.empty() ? "EVO PLAYER" : state.title);
        if (el_msg) {
            el_msg->SetInnerRML(state.message);
            el_msg->SetProperty("display", state.message.empty() ? "none" : "block");
        }

        /* Same icon/colour pairing evo_widget_toast used: info and error
         * share the "about" glyph and differ only by colour, ok borrows the
         * resume glyph, tech gets its own (previously tech rendered
         * identically to info - #75 gives it the muted styling the issue
         * asked for). EvoThemeColors carries no "danger" channel (nothing
         * else in the RmlUi bridge needed one yet), so error uses a fixed
         * red matching the four legacy themes' near-identical danger colours
         * rather than threading a new field through SetTheme for this alone. */
        static const char* kDangerRed = "#ff5c5c";
        std::string accent;
        std::string icon_path;
        switch (state.kind) {
            case 1: /* tech */
                accent = to_hex_rgb(m_theme.text_muted);
                icon_path = "../icons/icon_developer_tools.png";
                break;
            case 2: /* error */
                accent = kDangerRed;
                icon_path = "../icons/icon_about_support.png";
                break;
            case 3: /* ok */
                accent = to_hex_rgb(m_theme.accent_alt);
                icon_path = "../icons/icon_resume.png";
                break;
            default: /* info */
                accent = to_hex_rgb(m_theme.accent);
                icon_path = "../icons/icon_about_support.png";
                break;
        }

        if (el_card) el_card->SetClass("toast-tech", state.kind == 1);
        if (el_rail) el_rail->SetProperty("background-color", accent);
        if (el_icon) {
            el_icon->SetAttribute("src", icon_path);
            SetImageColor(el_icon, accent);
        }
    }

    Rml::Element* el_card = m_toast_doc->GetElementById("toast-card");
    if (el_card) {
        double opacity = state.alpha / 255.0;
        if (opacity < 0.0) opacity = 0.0;
        if (opacity > 1.0) opacity = 1.0;
        el_card->SetProperty("opacity", std::to_string(opacity));
        el_card->SetProperty("transform",
            "translateX(" + std::to_string(state.slide) + "px)");
    }

    m_toast_doc->Show();
}

/*
 * The overlay contexts (toast, keyboard, debug) are created at the display
 * size - 3840x2160 with a 2x dp ratio on a 4K output - but their C callers
 * still pass the 1080p design size (EVO_SCREEN_W/H, a literal 1920x1080).
 * SetDimensions builds the projection from what it is given, so on 4K every
 * overlay was drawn at twice its coordinates: the top-right toast card landed
 * off-screen (no toast was ever visible at 4K), and whatever did fall inside
 * the panel showed up in the wrong place - the dark rectangle in the
 * bottom-right corner. Project with the context's own size; the argument is
 * only a fallback for a context with none.
 */
void EvoRmlApp::SetOverlayDimensions(Rml::Context* ctx, int width, int height) {
    const Rml::Vector2i d = ctx ? ctx->GetDimensions() : Rml::Vector2i(0, 0);
    if (d.x > 0 && d.y > 0) {
        width = d.x;
        height = d.y;
    }
    m_render->SetDimensions(width, height);
}

void EvoRmlApp::RenderToast(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_toast_context || !m_toast_doc || !framebuffer) return;
    if (!m_toast_doc->IsVisible()) return;

    /* Own context: no other document to hide, and nothing here touches
     * m_frame_dirty / the RenderCachedScreen surface cache for whichever
     * menu screen is underneath - see the comment on EvoToastState. */
    m_render->SetFramebuffer(framebuffer);
    SetOverlayDimensions(m_toast_context, width, height);
    m_toast_context->Update();
    m_render->FrameBegin();
    m_toast_context->Render();
    m_render->FrameEnd();
#if defined(EVO_AGC_DEVICE)
    m_drew = true;
#endif
}

/* ---- #90: nav rail over a provider screen ---------------------------- */

void EvoRmlApp::RenderNavOverlay(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_nav_doc || !framebuffer) return;
    if (!m_last_nav.visible) return;

    /*
     * Hide every EVO screen document and render the main context, so the only
     * thing that draws is the rail. navbar.rcss keeps its body transparent, so
     * this composites over whatever the provider's context already painted.
     *
     * m_cached_screen is invalidated because the next EVO screen to come up
     * must re-rasterise: the cached surface no longer holds what it thinks it
     * holds, and a stale surface blitted over a provider screen is a
     * wrong-looking frame with no error anywhere.
     */
    ShowOnlyScreen(nullptr);
    m_nav_doc->Show();

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);
    m_context->Update();
    m_render->FrameBegin();
    m_context->Render();
    m_render->FrameEnd();

    /*
     * The cached menu surface no longer holds what it thinks it holds, so the
     * next EVO screen has to re-rasterise. Invalidating the id is enough -
     * RenderCachedScreen re-rasters on a screen change - and is deliberately
     * NOT m_frame_dirty: setting that here would make GlNeedsFrame() true on
     * every frame for as long as a provider screen is up, which is a redraw
     * per frame forever. The provider host reports its own liveness through
     * evo_rmlui_provider_needs_frame() instead.
     */
    m_cached_screen = -1;
#if defined(EVO_AGC_DEVICE)
    m_drew = true;
#endif
}

/* ---- #81: virtual keyboard modal ------------------------------------- */

static std::string kb_esc(const char* s) {
    std::string o;
    if (!s) return o;
    for (const char* p = s; *p; ++p) {
        switch (*p) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;";  break;
            case '>': o += "&gt;";  break;
            default:  o += *p;      break;
        }
    }
    return o;
}

void EvoRmlApp::UpdateKeyboard(const evo_keyboard_params_t* p) {
    if (!m_initialized || !m_keyboard_doc || !p) return;

    if (!p->visible) {
        if (m_keyboard_doc->IsVisible()) {
            m_keyboard_doc->Hide();
            m_frame_dirty = true;   /* force the screen underneath to redraw clean */
        }
        m_kb_sig.clear();
        return;
    }

    /* cheap change gate */
    std::string sig;
    sig.reserve(160);
    sig += p->native_only ? "N" : "V";
    sig += p->title ? p->title : "";     sig += '\x1f';
    sig += p->text ? p->text : "";       sig += '\x1f';
    sig += p->mode_label ? p->mode_label : "";
    for (int i = 0; i < 4; i++) { sig += '\x1f'; sig += (p->rows[i] ? p->rows[i] : ""); }
    sig += char('0' + (p->focus_row & 7));
    sig += char('0' + (p->focus_col & 15));
    sig += p->show_caret ? '1' : '0';
    { char b[24]; std::snprintf(b, sizeof b, "|%d/%d", p->len, p->max_len); sig += b; }

    if (!m_keyboard_doc->IsVisible()) m_keyboard_doc->Show();
    if (sig == m_kb_sig) return;
    m_kb_sig = sig;
    m_frame_dirty = true;   /* a keystroke / focus move -> redraw the screen + modal */

    Rml::Element* panel = m_keyboard_doc->GetElementById("kb-panel");
    if (panel) panel->SetProperty("display", p->native_only ? "none" : "flex");
    if (p->native_only) return;

    if (Rml::Element* e = m_keyboard_doc->GetElementById("kb-title"))
        e->SetInnerRML(kb_esc(p->title && p->title[0] ? p->title : "ENTER TEXT"));
    if (Rml::Element* e = m_keyboard_doc->GetElementById("kb-mode"))
        e->SetInnerRML(p->mode_label ? p->mode_label : "");
    if (Rml::Element* e = m_keyboard_doc->GetElementById("kb-text"))
        e->SetInnerRML(kb_esc(p->text));
    if (Rml::Element* e = m_keyboard_doc->GetElementById("kb-caret"))
        e->SetClass("hidden", !p->show_caret);
    if (Rml::Element* e = m_keyboard_doc->GetElementById("kb-count")) {
        char b[24]; std::snprintf(b, sizeof b, "%d / %d", p->len, p->max_len);
        e->SetInnerRML(b);
    }

    for (int r = 0; r < 4; r++) {
        const char* row = p->rows[r] ? p->rows[r] : "";
        int rl = (int)std::strlen(row);
        for (int c = 0; c < 10; c++) {
            char id[8]; std::snprintf(id, sizeof id, "k%d", r * 10 + c);
            Rml::Element* key = m_keyboard_doc->GetElementById(id);
            if (!key) continue;
            char ch[2] = { c < rl ? row[c] : ' ', 0 };
            key->SetInnerRML(kb_esc(ch));
            key->SetClass("focused", p->focus_row == r && p->focus_col == c);
        }
    }
    for (int a = 0; a < 6; a++) {
        char id[8]; std::snprintf(id, sizeof id, "a%d", a);
        Rml::Element* act = m_keyboard_doc->GetElementById(id);
        if (!act) continue;
        if (p->action_labels[a]) act->SetInnerRML(p->action_labels[a]);
        act->SetClass("focused", p->focus_row == 4 && p->focus_col == a);
    }
}

void EvoRmlApp::RenderKeyboard(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_keyboard_context || !m_keyboard_doc || !framebuffer) return;
    if (!m_keyboard_doc->IsVisible()) return;
    m_render->SetFramebuffer(framebuffer);
    SetOverlayDimensions(m_keyboard_context, width, height);
    m_keyboard_context->Update();
    m_render->FrameBegin();
    m_keyboard_context->Render();
    m_render->FrameEnd();
#if defined(EVO_AGC_DEVICE)
    m_drew = true;
#endif
}

void EvoRmlApp::UpdateDebugOverlay(int fps, bool visible) {
    if (!m_initialized || !m_debug_doc) return;
    m_debug_visible = visible;
    if (!visible) {
        if (m_debug_doc->IsVisible()) m_debug_doc->Hide();
        m_debug_last_fps = -1;
        return;
    }
    if (!m_debug_doc->IsVisible()) m_debug_doc->Show();
    if (fps != m_debug_last_fps) {
        m_debug_last_fps = fps;
        if (Rml::Element* e = m_debug_doc->GetElementById("debug-fps"))
            e->SetInnerRML(std::to_string(fps) + " FPS");
    }
}

void EvoRmlApp::RenderDebugOverlay(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_debug_context || !m_debug_doc || !framebuffer) return;
    if (!m_debug_doc->IsVisible()) return;
    m_render->SetFramebuffer(framebuffer);
    SetOverlayDimensions(m_debug_context, width, height);
    m_debug_context->Update();
    m_render->FrameBegin();
    m_debug_context->Render();
    m_render->FrameEnd();
#if defined(EVO_AGC_DEVICE)
    m_drew = true;
#endif
}

