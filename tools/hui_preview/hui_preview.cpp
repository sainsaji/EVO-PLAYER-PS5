/*
 * hui_preview.cpp - renders EVO's ps5-homebrew-ui screens on the host.
 *
 * usage: hui_preview <repo root> <output dir> [width height]
 *
 * The same screen code the console runs (projects/evoplayer/ui_kit), fed with
 * sample params, drawn through the kit's own OpenGL backend on Mesa's
 * surfaceless EGL (llvmpipe). Every fixture runs ~1.5 s of frames at a fixed
 * 60 Hz so entrances and springs have settled, then writes
 * <output>/hui_<fixture>.png. On the console the same draw lists go through
 * sceAgc (hui_agc_batch.cpp), with the same shader.
 *
 * Built and run by tools/hui_preview.sh.
 */
#include "evo_hui_browser.hpp"
#include "evo_hui_screens.hpp"
#include "evo_hui_settings.hpp"
#include "evo_hui_osd.hpp"
#include "evo_hui_modals.hpp"
#include "evo_hui_misc.hpp"
#include "settings_fixtures.inc"
#include "settings_preview_fixtures.inc"
#include "misc_fixtures.inc"
#include "evo_hui_extra.hpp"
#include "evo_hui_surround.hpp"
#include "evo_hui_provider.hpp"
#include "extra_fixtures.inc"
#include "overflow_check.inc"

#include "core/save_file.hpp"
#include "gfx/gl_program.hpp"
#include "gfx/renderer.hpp"
#include "ui/fonts.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/ps5-homebrew-ui/third_party/stb/stb_image_write.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace
{

bool open_context()
{
    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display =
        get_platform_display != nullptr
            ? get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
            : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) ||
        !eglBindAPI(EGL_OPENGL_API))
        return false;
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                 EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
    return context != EGL_NO_CONTEXT &&
           eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
}

class HostTextures final : public evo::kit::TextureHost
{
  public:
    HostTextures(hui::gfx::Renderer &renderer, std::string assets)
        : renderer_(renderer), assets_(std::move(assets))
    {
    }
    std::uint32_t create_texture(int w, int h, const std::uint8_t *rgba) override
    {
        return renderer_.batch().create_texture(w, h, rgba);
    }
    void release_texture(std::uint32_t handle) override
    {
        GLuint name = handle;
        if (name)
            glDeleteTextures(1, &name);
    }
    bool read_asset(const std::string &path, std::string *bytes) override
    {
        /* Bundle paths are relative to assets/rml ("../icons/x.png"). */
        std::string rel = path;
        while (rel.rfind("../", 0) == 0)
            rel = rel.substr(3);
        return hui::save::read_file(assets_ + "/" + rel, bytes);
    }

  private:
    hui::gfx::Renderer &renderer_;
    std::string assets_;
};

/* A stand-in for a decoded cover: a two-colour gradient with a soft disc,
 * as 0xAABBGGRR words like EVO's cover cache hands out. */
std::vector<std::uint32_t> fake_art(int w, int h, std::uint32_t a, std::uint32_t b, float seed)
{
    std::vector<std::uint32_t> px(static_cast<std::size_t>(w * h));
    auto ch = [](std::uint32_t c, int s) { return static_cast<float>((c >> s) & 0xff); };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const float u = static_cast<float>(x) / static_cast<float>(w);
            const float v = static_cast<float>(y) / static_cast<float>(h);
            float t = 0.5f * u + 0.5f * v;
            const float dx = u - 0.65f - 0.1f * std::sin(seed), dy = (v - 0.45f) * 0.56f;
            const float disc = std::exp(-(dx * dx + dy * dy) * 30.0f);
            t = std::min(1.0f, t + disc * 0.6f);
            const float r = ch(a, 16) * (1 - t) + ch(b, 16) * t;
            const float g = ch(a, 8) * (1 - t) + ch(b, 8) * t;
            const float bl = ch(a, 0) * (1 - t) + ch(b, 0) * t;
            px[static_cast<std::size_t>(y * w + x)] =
                0xff000000u | (static_cast<std::uint32_t>(bl) << 16) |
                (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(r);
        }
    return px;
}

bool load_font(hui::gfx::Renderer &renderer, const std::string &path, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    std::string data;
    if (!hui::save::read_file(path, &data) || !font->load(data))
    {
        std::fprintf(stderr, "cannot load font %s\n", path.c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: hui_preview <repo root> <output dir> [width height]\n");
        return 2;
    }
    const std::string root = argv[1];
    const std::string output = argv[2];
    const int width = argc > 4 ? std::atoi(argv[3]) : 1920;
    const int height = argc > 4 ? std::atoi(argv[4]) : 1080;
    const std::string assets = root + "/projects/evoplayer/assets";
    FILE *overflow_report = std::fopen((output + "/hui_overflow.txt").c_str(), "w");
    if (!overflow_report)
        overflow_report = stderr;
    FILE *metrics = std::fopen((output + "/hui_metrics.tsv").c_str(), "w");
    if (!metrics)
        metrics = stderr;

    if (!open_context())
    {
        std::fprintf(stderr, "no surfaceless EGL OpenGL 4.5 context\n");
        return 1;
    }
    hui::gfx::set_glsl_prefix("#version 450 core\n");
    hui::gfx::Renderer renderer;
    if (!renderer.init())
        return 1;

    hui::gfx::Font regular, semibold, display, mono;
    hui::ui::Fonts fonts;
    const std::string fdir = assets + "/hui/fonts/";
    if (!load_font(renderer, fdir + "inter-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, fdir + "inter-semibold.huifont", &semibold, &fonts.semibold) ||
        !load_font(renderer, fdir + "montserrat-medium.huifont", &display, &fonts.display) ||
        !load_font(renderer, fdir + "dejavu-sans-mono.huifont", &mono, &fonts.mono))
        return 1;
    hui::gfx::Font fallback;
    hui::ui::FontRef fallback_ref;
    if (load_font(renderer, fdir + "noto-fallback.huifont", &fallback, &fallback_ref))
    {
        for (hui::gfx::Font *f : {&regular, &semibold, &display, &mono})
            f->set_fallback(&fallback, fallback_ref.texture);
    }
    fonts.pixel = fonts.mono;
    fonts.hand = fonts.regular;

    HostTextures host(renderer, assets);
    evo::kit::TextureCache textures(host);
    evo::kit::Palette palette;
    /* EVO_HUI_THEME=CARBON|EMBER|AURORA|MIDNIGHT previews a built-in theme (#137). */
    if (const char *tn = std::getenv("EVO_HUI_THEME"))
    {
        auto c = [](unsigned r, unsigned g, unsigned b, unsigned a) -> std::uint32_t
        { return (a << 24) | (b << 16) | (g << 8) | r; };
        evo_rmlui_theme_t t{};
        const std::string n = tn;
        if (n == "MIDNIGHT")
            t = {tn, c(8,13,22,255), c(3,5,10,255), c(16,26,44,240), c(22,34,58,250), c(90,125,180,51), c(255,205,0,255),
                 c(255,205,0,255), c(255,205,0,50), c(0,205,255,255), c(238,244,255,255), c(163,182,208,255), c(94,113,140,255)};
        else if (n == "CARBON")
            t = {tn, c(15,15,17,255), c(5,5,6,255), c(28,28,32,235), c(42,42,48,245), c(51,51,58,170), c(232,232,238,200),
                 c(242,242,245,255), c(255,255,255,40), c(255,107,53,255), c(245,245,247,255), c(160,160,168,255), c(102,102,110,255)};
        else if (n == "EMBER")
            t = {tn, c(20,12,8,255), c(7,4,2,255), c(36,22,14,235), c(58,34,17,245), c(74,48,28,170), c(255,165,40,220),
                 c(255,165,40,255), c(255,122,24,60), c(255,77,77,255), c(255,243,230,255), c(199,169,142,255), c(138,110,86,255)};
        else if (n == "AURORA")
            t = {tn, c(5,18,18,255), c(1,6,8,255), c(13,36,36,235), c(17,58,53,245), c(30,74,70,170), c(61,245,192,220),
                 c(61,245,192,255), c(36,201,160,60), c(155,140,255,255), c(232,255,248,255), c(147,195,184,255), c(86,130,122,255)};
        palette = evo::kit::Palette::from(t);
    }

    GLuint framebuffer = 0, color = 0;
    glGenFramebuffers(1, &framebuffer);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 1;
    hui::save::ensure_directory(output);

    /* ---- sample data ---- */
    const auto hero_art = fake_art(960, 540, 0x1b2a6b, 0xd9468f, 0.3f);
    std::vector<std::vector<std::uint32_t>> covers;
    const std::uint32_t pairs[][2] = {{0x0f3d3e, 0x3fd0b0}, {0x2b1055, 0xd16ba5},
                                      {0x3a1c0b, 0xf2a541}, {0x0b1f3a, 0x5aa9e6},
                                      {0x1e2a12, 0x9bd16b}, {0x331022, 0xff6f91}};
    for (int i = 0; i < 6; ++i)
        covers.push_back(fake_art(320, 180, pairs[i][0], pairs[i][1], static_cast<float>(i)));
    const char *recent_titles[] = {"Blade Runner 2049",  "Arrival",  "Planet Earth III - Ep 4",
                                   "Dune Part Two",      "The Bear S03E02", "Interstellar"};
    const int recent_progress[] = {620, 140, 900, -1, 330, 760};

    evo_rmlui_launch_params_t launch{};
    launch.app_name = "EVO PLAYER";
    launch.version = "VERSION 0.13.5";
    launch.clock = "21:47";
    launch.theme_name = "MIDNIGHT";
    launch.hero_eyebrow = "CONTINUE WATCHING";
    launch.hero_title = "Blade Runner 2049";
    launch.hero_detail = "/mnt/usb0/Movies/Blade.Runner.2049.2017.2160p.UHD.BluRay.mkv";
    launch.hero_action = "RESUME";
    launch.hero_progress = 620;
    launch.hero_art = hero_art.data();
    launch.hero_art_w = 960;
    launch.hero_art_h = 540;
    launch.recent_total = 14;
    launch.recent_visible = 6;
    for (int i = 0; i < 6; ++i)
    {
        launch.recent[i].title = recent_titles[i];
        launch.recent[i].detail = "Recent File";
        launch.recent[i].icon_path = "../icons/icon_recent_files.png";
        launch.recent[i].progress = recent_progress[i];
        launch.recent[i].art = i == 3 ? nullptr : covers[static_cast<std::size_t>(i)].data();
        launch.recent[i].art_w = 320;
        launch.recent[i].art_h = 180;
    }
    const char *lib[][3] = {{"BROWSE", "USB drives", "../icons/icon_browse_usb.png"},
                            {"RECENT", "Resume watching", "../icons/icon_recent_files.png"},
                            {"FAVORITES", "Saved for later", "../icons/icon_favorites.png"},
                            {"PROVIDERS", "Online sources", "../icons/icon_emby.png"},
                            {"SETTINGS", "Video & audio", "../icons/icon_settings.png"},
                            {"ABOUT", "Credits & info", "../icons/icon_about_support.png"}};
    launch.library_visible = 6;
    for (int i = 0; i < 6; ++i)
    {
        launch.library[i].title = lib[i][0];
        launch.library[i].detail = lib[i][1];
        launch.library[i].icon_path = lib[i][2];
        launch.library[i].progress = -1;
    }

    evo_rmlui_nav_params_t nav{};
    nav.visible = 1;

    /* ---- browser ---- */
    const char *bnames[][4] = {
        {"Movies", "Folder - 42 items", "DIR", ""},
        {"Blade Runner 2049 (2017).mkv", "18.2 GB", "4K", "02:43:51"},
        {"Arrival (2016).mkv", "9.1 GB", "HD", "01:56:12"},
        {"Planet Earth III S01E04.mkv", "6.4 GB", "HDR", "00:52:08"},
        {"Dune Part Two (2024).mkv", "31.0 GB", "4K", "02:46:01"},
        {"Concert - Live at Wembley.mkv", "4.4 GB", "HD", "01:31:44"},
        {"Soundtrack.flac", "1.1 GB", "AUDIO", "00:58:20"},
        {"Holiday 2025", "Folder - 318 items", "DIR", ""},
    };
    evo_rmlui_browser_params_t bp{};
    bp.path = "USB Drive / Media / Movies";
    bp.title = "USB Drive";
    bp.active_source = 0;
    bp.sidebar_index = 0;
    bp.filter_count = 3;
    bp.filter_selected = 0;
    bp.filter_labels[0] = "All";
    bp.filter_labels[1] = "Videos";
    bp.filter_labels[2] = "Music";
    bp.total_count = 37;
    bp.row_count = 8;
    for (int i = 0; i < 8; ++i)
    {
        bp.rows[i].name = bnames[i][0];
        bp.rows[i].detail = bnames[i][1];
        bp.rows[i].badge = bnames[i][2];
        bp.rows[i].duration = bnames[i][3];
        bp.rows[i].icon_path = bnames[i][2][0] == 'D' ? "../icons/icon_folder.png" : "../icons/icon_recent_files.png";
        bp.rows[i].progress = (i == 1) ? 620 : (i == 3 ? 300 : -1);
        bp.rows[i].is_favorite = i == 4;
        const bool has_art = i >= 1 && i <= 5;
        bp.rows[i].art = has_art ? covers[static_cast<std::size_t>(i)].data() : nullptr;
        bp.rows[i].art_w = 320;
        bp.rows[i].art_h = 180;
    }
    bp.ins_name = "Planet Earth III S01E04.mkv";
    bp.ins_kind = "Video";
    bp.status_res = "3840x2160";
    bp.status_vcodec = "HEVC Main10 HLG";
    bp.status_acodec = "E-AC3 5.1";
    bp.status_duration = "00:52:08";
    bp.status_size = "6.4 GB";
    bp.action_menu_target = "Planet Earth III S01E04.mkv";
    bp.action_target_sub = "Video file - 6.4 GB";
    bp.action_clipboard_info = "Clipboard: empty";
    bp.transfer_op_title = "Copying to Internal Storage";
    bp.transfer_item_name = "Dune Part Two (2024).mkv";
    bp.transfer_speed_str = "84.2 MB/s";
    bp.transfer_bytes_str = "12.4 / 31.0 GB";
    bp.transfer_eta_str = "3 min left";
    bp.transfer_percent_str = "40%";
    bp.transfer_progress_pct = 0.4;

    /* ---- fixtures ---- */
    /* ---- list (Recent / Favorites / providers) ---- */
    const char *files[][3] = {
        {"Blade Runner 2049 (2017)", "2160p HEVC HDR10 - 18.2 GB", "4K"},
        {"Arrival (2016)", "1080p H.264 - 9.1 GB", "HD"},
        {"Planet Earth III - S01E04 - Ocean", "2160p HEVC HLG - 6.4 GB", "HDR"},
        {"Dune Part Two (2024)", "2160p HEVC DV - 31.0 GB", "4K"},
        {"The Bear - S03E02 - Next", "1080p HEVC - 1.4 GB", ""},
        {"Interstellar (2014)", "2160p HEVC HDR10 - 24.8 GB", "4K"},
        {"Concert - Live at Wembley.mkv", "1080p H.264 - 4.4 GB", ""},
        {"Oppenheimer (2023)", "2160p HEVC HDR10 - 29.9 GB", "4K"},
        {"Soundtrack - Hans Zimmer.flac", "FLAC 24/96 - 1.1 GB", "AUDIO"},
    };
    const int file_progress[] = {620, 140, 900, -1, 330, 760, -1, 50, -1};
    evo_rmlui_list_params_t lp{};
    lp.title = "Recent Files";
    lp.subtitle = "Pick up where you left off";
    lp.section = 0;
    lp.row_count = 9;
    lp.total_count = 23;
    for (int i = 0; i < 9; ++i)
    {
        lp.rows[i].title = files[i][0];
        lp.rows[i].detail = files[i][1];
        lp.rows[i].badge = files[i][2];
        lp.rows[i].icon_path = i == 8 ? "../icons/icon_volume.png" : "../icons/icon_recent_files.png";
        lp.rows[i].progress = file_progress[i];
        lp.rows[i].has_chevron = 1;
    }
    lp.hint_count = 3;
    lp.hints[0] = {"../icons/btn_cross.png", "PLAY"};
    lp.hints[1] = {"../icons/btn_square.png", "OPTIONS"};
    lp.hints[2] = {"../icons/btn_circle.png", "BACK"};
    lp.empty_title = "No favorites yet";
    lp.empty_hint = "Press Triangle on any file in Browse to keep it here.";
    lp.empty_icon = "../icons/icon_favorites.png";
    lp.menu_eyebrow = "FILE OPTIONS";
    lp.menu_title = "Dune Part Two (2024)";
    lp.menu_sub = "/mnt/usb0/Movies/Dune.Part.Two.2024.2160p.mkv";
    lp.menu_icon = "../icons/icon_recent_files.png";
    lp.menu[0] = {"Play from start", "Ignore the saved position", "../icons/icon_resume.png", 0};
    lp.menu[1] = {"Add to favorites", "Keep it on the Favorites list", "../icons/icon_favorites.png", 0};
    lp.menu[2] = {"Media info", "Codecs, streams and HDR", "../icons/icon_report.png", 0};
    lp.menu[3] = {"Remove from recent", "Forget the position too", "../icons/icon_trash.png", 1};

    /* ---- fixtures ---- */
    evo_rmlui_settings_params_t sp{};
    evo_rmlui_dialog_params_t dialogp{};
    evo_rmlui_toast_params_t toastp{};
    evo_rmlui_reader_params_t readerp{};
    evo_rmlui_image_params_t imagep{};
    std::vector<uint32_t> image_px;
    /* #237: the Web page view, with a made-up page standing in for the engine's frame */
    evo_hui_webpage_params_t webp{};
    constexpr int kWebW = EVO_HUI_WEBPAGE_W;
    constexpr int kWebH = EVO_HUI_WEBPAGE_H;
    std::vector<uint32_t> web_px(static_cast<std::size_t>(kWebW) * kWebH, 0xFFFFFFFFu);
    {
        auto bar = [&](int x, int y, int w, int h, uint32_t c) {
            for (int yy = y; yy < y + h && yy < kWebH; ++yy)
                for (int xx = x; xx < x + w && xx < kWebW; ++xx)
                    web_px[static_cast<std::size_t>(yy) * kWebW + xx] = c;
        };
        bar(0, 0, 1920, 110, 0xFFF4F1EEu);            /* site header */
        bar(160, 36, 420, 40, 0xFF555555u);
        bar(160, 170, 900, 54, 0xFF222222u);          /* headline */
        for (int i = 0; i < 9; ++i)                    /* text lines */
            bar(160, 270 + i * 46, i % 3 == 2 ? 1100 : 1500, 20, 0xFFBBBBBBu);
        bar(160, 700, 380, 22, 0xFFD05F1Au);          /* a link */
        bar(1240, 170, 520, 380, 0xFFE0D6CCu);        /* a picture */
    }
    evo_keyboard_params_t kbp{};
    evo_rmlui_surround_params_t srp{};
    evo_hui_provider_params_t prp{};
    std::vector<uint32_t> provider_art;
    evo_rmlui_about_params_t aboutp{};
    evo_rmlui_changelog_params_t changelogp{};

    /* ---- player OSD ---- */
    evo_playback_osd_params_t op{};
    op.title = "Blade Runner 2049";
    op.metadata = "2017  -  2h 43m  -  Blade.Runner.2049.2160p.UHD.BluRay.mkv";
    op.res_badge = "4K";
    op.hdr_badge = "HDR10";
    op.codec_badge = "HEVC";
    op.fps_badge = "23.976";
    op.audio_badge = "TRUEHD 7.1";
    op.decoder_badge = "HARDWARE";
    op.upscale_badge = "AI UPSCALE";
    op.upscale_active = 1;
    op.position_sec = 3725;
    op.duration_sec = 9831;
    op.percentage = 3725.0 / 9831.0;
    op.audio_track = "English TrueHD 7.1";
    op.sub_track = "English (SDH)";
    op.alpha = 255;
    op.subtitle_face = 2;
    op.subtitle_raised = 1;
    op.fps = 60;
    const float hist[] = {0.2f, 0.3f, 0.25f, 0.5f, 0.45f, 0.6f, 0.4f, 0.35f, 0.5f, 0.55f, 0.3f, 0.4f};
    evo_perf_hud_t hud{};
    hud.line_video = "VIDEO  HEVC Main10 3840x2160 23.976  native  drop 0";
    hud.line_audio = "AUDIO  TrueHD 7.1 48kHz -> PCM 7.1";
    hud.line_subs = "SUBS   PGS English (SDH)";
    hud.line_perf = "PERF   frame 8.2 ms  gpu 31%  present 59.9";
    hud.line_queues = "QUEUE  video 12/16  audio 40/64  pkt 220";
    hud.line_clocks = "CLOCK  av -3 ms  vsync 41.6 ms";
    hud.hist_len = 12;
    hud.gpu_hist = hud.ram_hist = hud.cpu_hist = hist;
    hud.gpu_pct = 31;
    hud.ram_mb = 612;
    hud.ram_total_mb = 1024;
    hud.cpu_pct = 22;

    evo_rmlui_subtitles_params_t subp{};
    subp.eyebrow = "SUBTITLES";
    subp.title = "Select subtitle track";
    subp.size_str = "MEDIUM";
    subp.sync_str = "SYNC: +120 ms"; /* as SubtitlePickerScreen sends it */
    subp.preview_text = "The quick brown fox jumps over the lazy dog";
    subp.preview_face = 1;
    subp.track_count = 6;
    const char *tracks[][3] = {{"Subtitles off", "", ""},
                               {"English (SDH)", "PGS - embedded", "PRIMARY"},
                               {"Spanish", "SRT - embedded", ""},
                               {"French", "ASS - embedded", "SECONDARY"},
                               {"External SRT", "Blade.Runner.2049.en.srt", ""},
                               {"Auto-sync", "Match the subtitle timing to the speech", ""}};
    for (int i = 0; i < 6; ++i)
    {
        subp.tracks[i].label = tracks[i][0];
        subp.tracks[i].detail = tracks[i][1];
        subp.tracks[i].tag = tracks[i][2];
    }
    subp.tracks[1].is_current = 1;
    subp.tracks[2].is_focused = 1;
    subp.tracks[5].is_action = 1;

    evo_rmlui_mediainfo_params_t mip{};
    mip.title = "Blade Runner 2049";
    mip.path = "/mnt/usb0/Movies/Blade.Runner.2049.2160p.UHD.BluRay.mkv";
    mip.res_badge = "4K";
    mip.hdr_badge = "HDR10";
    mip.codec_badge = "HEVC";
    mip.fps_badge = "23.976";
    mip.container = "Matroska";
    mip.file_size = "58.3 GB";
    mip.duration = "2:43:51";
    mip.video_codec = "HEVC Main10 @ L5.1";
    mip.resolution = "3840 x 2160";
    mip.color_hdr = "BT.2020 PQ (HDR10)";
    mip.audio_codec = "Dolby TrueHD Atmos";
    mip.channels = "7.1";
    mip.sample_rate = "48 kHz";
    mip.subtitles = "English (SDH)";
    mip.output = "3840x2160 HDR10 60 Hz";
    mip.renderer = "sceAgc";
    mip.decoder = "Hardware (sceVideodec2)";
    mip.upscaler = "Off (native 4K)";
    mip.motion_smoothing = "Off";

    struct Fixture
    {
        const char *name;
        int kind; /* 0 home, 1 list, 2 browser, 3 settings */
        std::function<void()> setup;
    };
    auto clear_focus = [&]() {
        launch.hero_focused = 0;
        launch.recent_cursor = -1;
        for (int i = 0; i < 6; ++i)
            launch.recent[i].is_focused = launch.library[i].is_focused = 0;
        nav.rail_focused = 0;
        for (int i = 0; i < 9; ++i)
        {
            lp.rows[i].is_focused = 0;
            lp.rows[i].pills = nullptr;
        }
        lp.row_count = 9;
        lp.cursor_index = -1;
        lp.panel_count = 0;
        lp.loading = 0;
        lp.is_empty = 0;
        lp.menu_count = 0;
        for (int i = 0; i < 8; ++i)
            bp.rows[i].is_focused = 0;
        bp.cursor_index = -1;
        bp.sidebar_focused = bp.filter_focused = 0;
        bp.action_menu_open = bp.transfer_modal_open = 0;
    };
    const Fixture fixtures[] = {
        {"home", 0, [&] { clear_focus(); launch.hero_focused = 1; }},
        {"home_shelf", 0, [&] { clear_focus(); launch.recent_cursor = 4; launch.recent[4].is_focused = 1; }},
        {"home_library", 0, [&] { clear_focus(); launch.library[1].is_focused = 1; }},
        {"home_rail", 0, [&] { clear_focus(); nav.rail_focused = 1; nav.cursor_index = 1; }},
        {"list", 1, [&] { clear_focus(); lp.rows[2].is_focused = 1; lp.cursor_index = 2; }},
        {"list_scripts", 1, [&] {
             clear_focus();
             static const char *const names[9][2] = {
                 {"قناة الأخبار العربية", "Arabic"},
                 {"MBC مصر 2 (HD)", "Arabic with Latin and numbers"},
                 {"ערוץ 13 - חדשות", "Hebrew"},
                 {"Русский кинозал", "Cyrillic"},
                 {"Ελληνική Τηλεόραση", "Greek"},
                 {"Café Münchën Œuvre Ñandú", "Accented Latin"},
                 {"Phim Việt Nam", "Vietnamese"},
                 {"فيلم فارسی گچپژ", "Persian letters"},
                 {"Arrows ← ↑ → ↓ ★ ✓ ♪", "Symbols"}};
             lp.title = "Scripts";
             lp.subtitle = "Fallback font, Arabic joining, bidi order";
             for (int i = 0; i < 9; ++i)
             {
                 lp.rows[i].title = names[i][0];
                 lp.rows[i].detail = names[i][1];
                 lp.rows[i].badge = "";
                 lp.rows[i].progress = -1;
             }
             lp.rows[0].is_focused = 1;
             lp.cursor_index = 0;
         }},
        {"list_streams", 1, [&] {
             clear_focus();
             static const char *const streams[9][3] = {
                 {"Clarksons Farm s01·e08", "4K HEVC  -  SDR  -  6.3 GB  -  15.4 Mbps  -  DD+ 5.1", "4K"},
                 {"Clarkson's Farm s01·e08", "1080p H264  -  SDR  -  4.0 GB  -  10.4 Mbps  -  DD+ 5.1", "1080P"},
                 {"Clarksons Farm s01·e08", "1080p H264  -  SDR  -  3.8 GB  -  10.0 Mbps  -  AC3 Stereo", "1080P"},
                 {"Clarkson's Farm s01·e08", "1080p H264  -  SDR  -  1.8 GB  -  4.7 Mbps  -  AAC Stereo", "1080P"},
                 {"Clarksons Farm s01·e08", "1080p H264  -  SDR  -  5.1 GB  -  13.6 Mbps  -  DD+ 5.1", "1080P"},
                 {"Server transcode (1080p H.264)", "1080p H264  -  AAC", "1080P"},
                 {"Clarksons Farm s01·e08", "720p H264  -  SDR  -  1.2 GB  -  3.1 Mbps  -  AAC Stereo", "720P"},
                 {"Clarksons Farm s01·e08", "4K HEVC  -  HDR10  -  18.2 GB  -  41.0 Mbps  -  TRUEHD 7.1", "4K"},
                 {"Clarksons Farm s01·e08", "480p H264  -  SDR  -  0.6 GB  -  1.4 Mbps  -  AAC Stereo", "480P"}};
             lp.title = "Choose a stream";
             lp.subtitle = "Clarkson's Farm - S01E08 Harvesting";
             for (int i = 0; i < 9; ++i)
             {
                 lp.rows[i].title = streams[i][0];
                 lp.rows[i].detail = streams[i][1];
                 lp.rows[i].badge = streams[i][2];
                 lp.rows[i].progress = -1;
                 lp.rows[i].icon_path = "../icons/icon_tv.png";
             }
             lp.rows[0].is_focused = 1;
             lp.cursor_index = 0;
             lp.hint_count = 2;
             lp.hints[0] = {"../icons/btn_cross.png", "PLAY"};
             lp.hints[1] = {"../icons/btn_circle.png", "BACK"};
             lp.panel_eyebrow = "STREAM DETAILS";
             lp.panel_title = "Clarksons Farm s01·e08";
             static const char *const facts[9][2] = {
                 {"Quality", "4K"}, {"Source", "Web-dl"}, {"Rating", "5 of 5"},
                 {"Video and audio", "HEVC SDR DD+ 5.1"}, {"Size", "6.28 / 45 GB"},
                 {"Bitrate", "15.4 Mbps"}, {"Added", "1d"}, {"Release", "[TB] ComeTorz · HONE"},
                 {"Languages", "en · sub (en) » web t1 amazon 3635"}};
             lp.panel_count = 9;
             for (int i = 0; i < 9; ++i)
                 lp.panel[i] = {facts[i][0], facts[i][1]};
             static const char *const pills[9] = {
                 "+HEVC\t+HDR10\tDD+ 5.1\t6.3 GB\t15.4 Mbps", "H264\tSDR\tDD+ 5.1\t4.0 GB\t10.4 Mbps",
                 "H264\tSDR\tAC3 2.0\t3.8 GB\t10.0 Mbps",     "H264\tSDR\tAAC 2.0\t1.8 GB\t4.7 Mbps",
                 "H264\tSDR\tDD+ 5.1\t5.1 GB\t13.6 Mbps",     "H264\tAAC\tTranscode",
                 "H264\tSDR\tAAC 2.0\t1.2 GB\t3.1 Mbps",      "+HEVC\t+HDR10\tTRUEHD 7.1\t18.2 GB\t41 Mbps",
                 "H264\tSDR\tAAC 2.0\t0.6 GB"};
             for (int i = 0; i < 9; ++i)
                 lp.rows[i].pills = pills[i];
         }},
        {"list_streams_wide", 1, [&] {
             clear_focus();
             static const char *const titles[9] = {"Clarksons Farm s01·e08", "Clarkson's Farm s01·e08",
                                                   "Clarksons Farm s01·e08", "Clarkson's Farm s01·e08",
                                                   "Clarksons Farm s01·e08", "Server transcode (1080p H.264)",
                                                   "Clarksons Farm s01·e08", "Clarksons Farm s01·e08",
                                                   "Clarksons Farm s01·e08"};
             static const char *const pills[9] = {
                 "+4K\tWeb-dl\t+HEVC\t+HDR10\tDD+ 5.1\tEN\tSUB EN\t6.3 GB\t15.4 Mbps\tComeTorz",
                 "+1080P\tWeb-dl\tH264\tSDR\tDD+ 5.1\tEN\t4.0 GB\t10.4 Mbps",
                 "+1080P\tWeb-dl\tH264\tSDR\tAC3 2.0\tEN\t3.8 GB\t10.0 Mbps",
                 "+1080P\tWebrip\tH264\tSDR\tAAC 2.0\tEN\t1.8 GB\t4.7 Mbps",
                 "+1080P\tWebrip\tH264\tSDR\tDD+ 5.1\tEN\t5.1 GB\t13.6 Mbps",
                 "+1080P\tTranscode\tH264\tAAC",
                 "+720P\tWeb-dl\tH264\tSDR\tAAC 2.0\tEN\t1.2 GB\t3.1 Mbps",
                 "+4K\tRemux\t+HEVC\t+HDR10\tTRUEHD 7.1\tEN\t18.2 GB\t41 Mbps",
                 "+480P\tWebrip\tH264\tSDR\tAAC 2.0\tEN\t0.6 GB"};
             lp.title = "Choose a stream";
             lp.subtitle = "Clarkson's Farm - S01E08 Harvesting";
             for (int i = 0; i < 9; ++i)
             {
                 lp.rows[i].title = titles[i];
                 lp.rows[i].detail = "";
                 lp.rows[i].badge = "";
                 lp.rows[i].pills = pills[i];
                 lp.rows[i].progress = -1;
                 lp.rows[i].icon_path = "../icons/icon_tv.png";
             }
             lp.rows[0].is_focused = 1;
             lp.cursor_index = 0;
             lp.hint_count = 2;
             lp.hints[0] = {"../icons/btn_cross.png", "PLAY"};
             lp.hints[1] = {"../icons/btn_circle.png", "BACK"};
         }},
        {"list_audio_pills", 1, [&] {
             clear_focus();
             static const char *const titles[5] = {"English", "English - Commentary", "Japanese", "French", "Spanish"};
             static const char *const pills[5] = {"+DEFAULT\tDD+\t5.1\t640 kbps", "AAC\t2.0\t128 kbps\tCOMMENTARY",
                                                  "TRUEHD\t7.1\tAtmos\t4.2 Mbps", "AC3\t5.1\t448 kbps",
                                                  "AC3\t2.0\t192 kbps"};
             lp.title = "Audio";
             lp.subtitle = "Clarkson's Farm - S01E08 Harvesting";
             lp.row_count = 6;
             lp.rows[0].title = "Auto";
             lp.rows[0].pills = "FOLLOW SETTINGS\tEnglish first";
             lp.rows[0].badge = "";
             for (int i = 0; i < 5; ++i)
             {
                 lp.rows[i + 1].title = titles[i];
                 lp.rows[i + 1].pills = pills[i];
                 lp.rows[i + 1].badge = "";
             }
             for (int i = 0; i < 6; ++i)
             {
                 lp.rows[i].detail = "";
                 lp.rows[i].progress = -1;
                 lp.rows[i].icon_path = "../icons/icon_volume.png";
             }
             lp.rows[1].is_focused = 1;
             lp.cursor_index = 1;
             lp.hint_count = 2;
             lp.hints[0] = {"../icons/btn_cross.png", "SELECT"};
             lp.hints[1] = {"../icons/btn_circle.png", "BACK"};
         }},
        {"list_audio_loading", 1, [&] {
             clear_focus();
             static const char *const titles[3] = {"Unknown language - track 1", "English", "Italiano"};
             static const char *const pills[3] = {"DD+\t5.1\t48 kHz", "+PLAYING\tAC3\t2.0\t48 kHz", "AAC\t2.0\t48 kHz"};
             lp.title = "Audio";
             lp.subtitle = "Clarkson's Farm - S01E08 Harvesting";
             lp.row_count = 3;
             for (int i = 0; i < 3; ++i)
             {
                 lp.rows[i].title = titles[i];
                 lp.rows[i].pills = pills[i];
                 lp.rows[i].detail = "";
                 lp.rows[i].badge = "";
                 lp.rows[i].progress = -1;
                 lp.rows[i].icon_path = "../icons/icon_aspect.png";
             }
             lp.rows[0].is_focused = 1;
             lp.cursor_index = 0;
             lp.loading = 1;
             lp.loading_text = "Switching audio...";
             lp.hint_count = 2;
             lp.hints[0] = {"../icons/btn_cross.png", "SELECT"};
             lp.hints[1] = {"../icons/btn_circle.png", "BACK"};
         }},
        {"list_audio_card", 1, [&] {
             clear_focus();
             static const char *const titles[5] = {"English", "English - Commentary", "Japanese", "French", "Spanish"};
             static const char *const pills[5] = {"+DD+\t5.1", "AAC\t2.0", "TRUEHD\t7.1", "AC3\t5.1", "AC3\t2.0"};
             lp.title = "Audio";
             lp.subtitle = "Clarkson's Farm - S01E08 Harvesting";
             lp.row_count = 6;
             lp.rows[0].title = "Auto";
             lp.rows[0].pills = "FOLLOW SETTINGS";
             lp.rows[0].badge = "";
             for (int i = 0; i < 5; ++i)
             {
                 lp.rows[i + 1].title = titles[i];
                 lp.rows[i + 1].pills = pills[i];
                 lp.rows[i + 1].badge = i == 0 ? "DEFAULT" : "";
             }
             for (int i = 0; i < 6; ++i)
             {
                 lp.rows[i].detail = "";
                 lp.rows[i].progress = -1;
                 lp.rows[i].icon_path = "../icons/icon_volume.png";
             }
             lp.rows[1].is_focused = 1;
             lp.cursor_index = 1;
             lp.hint_count = 2;
             lp.hints[0] = {"../icons/btn_cross.png", "SELECT"};
             lp.hints[1] = {"../icons/btn_circle.png", "BACK"};
             lp.panel_eyebrow = "TRACK DETAILS";
             lp.panel_title = "English";
             static const char *const facts[6][2] = {
                 {"Codec", "Dolby Digital Plus"}, {"Channels", "5.1 surround"}, {"Bitrate", "640 kbps"},
                 {"Sample rate", "48 kHz"}, {"Output", "Bitstream passthrough to receiver"},
                 {"Track title", "Main audio"}};
             lp.panel_count = 6;
             for (int i = 0; i < 6; ++i)
                 lp.panel[i] = {facts[i][0], facts[i][1]};
         }},
        {"list_empty", 1, [&] { clear_focus(); lp.title = "Favorites"; lp.subtitle = "Saved for later";
                                 lp.is_empty = 1; lp.row_count = 0; lp.total_count = 0; }},
        {"list_menu", 1, [&] { clear_focus(); lp.title = "Recent Files"; lp.row_count = 9; lp.total_count = 23;
                                lp.rows[3].is_focused = 1; lp.cursor_index = 3; lp.menu_count = 4; lp.menu_focus = 1; }},
        {"osd", 4, [&] { clear_focus(); op.subtitle_text = "We're not here to win.\nWe're here to remember."; }},
        {"osd_paused", 4, [&] { clear_focus(); op.paused = 1; op.subtitle_text = nullptr; }},
        {"osd_scrub", 4, [&] { clear_focus(); op.paused = 0; op.scrub_active = 1; op.scrub_target = 5120; }},
        {"osd_captions_only", 4, [&] { clear_focus(); op.scrub_active = 0; op.alpha = 0; op.chrome_hidden = 1;
                                        op.subtitle_text = "Cells interlinked.";
                                        op.subtitle_text2 = "Cellules interconnectees."; }},
        {"osd_stats", 4, [&] { clear_focus(); op.chrome_hidden = 0; op.alpha = 255; op.subtitle_text2 = nullptr;
                                op.show_stats = 1; op.loading = 1; }},
        {"osd_skip", 4, [&] { clear_focus(); op.show_stats = 0; op.loading = 0; op.subtitle_text = nullptr;
                               op.skip_label = "Skip intro"; }},
        {"osd_skip_next", 4, [&] { clear_focus(); op.chrome_hidden = 1; op.alpha = 0;
                                    op.skip_label = "Next episode"; }},
        {"subtitles", 5, [&] { clear_focus(); }},
        {"mediainfo", 6, [&] { clear_focus(); }},
        {"settings_sidebar", 3, [&] { clear_focus(); fill_settings_fixture(sp, 0); }},
        {"settings_rows", 3, [&] { clear_focus(); fill_settings_fixture(sp, 1); }},
        {"settings_deepblacks", 3, [&] { clear_focus(); fill_settings_fixture(sp, 2); }},
        {"settings_upscaling", 3, [&] { clear_focus(); fill_settings_fixture(sp, 3); }},
        {"settings_pv_ai_network", 3, [&] { clear_focus(); fill_settings_pv_fixture(sp, "ai_network"); }},
        {"settings_pv_hdr", 3, [&] { clear_focus(); fill_settings_pv_fixture(sp, "hdr"); }},
        {"settings_pv_motion", 3, [&] { clear_focus(); fill_settings_pv_fixture(sp, "motion"); }},
        {"settings_pv_refresh", 3, [&] { clear_focus(); fill_settings_pv_fixture(sp, "refresh"); }},
        {"settings_pv_match_rate", 3, [&] { clear_focus(); fill_settings_pv_fixture(sp, "match_rate"); }},
        {"settings_pv_aspect", 3, [&] { clear_focus(); fill_settings_pv_fixture(sp, "aspect"); }},
        {"settings_pv_sub_auto", 3, [&] { clear_focus(); fill_settings_pv_fixture(sp, "sub_auto"); }},
        {"settings_pv_sub_font", 3, [&] { clear_focus(); fill_settings_pv_fixture(sp, "sub_font"); }},
        {"settings_pv_sub_pos", 3, [&] { clear_focus(); fill_settings_pv_fixture(sp, "sub_pos"); }},
        {"settings_pv_sub_colour", 3, [&] { clear_focus(); fill_settings_pv_fixture(sp, "sub_colour"); }},
        {"settings_pv_theme", 3, [&] { clear_focus(); fill_settings_pv_fixture(sp, "theme"); }},
        {"settings_pv_lightbar", 3, [&] { clear_focus(); fill_settings_pv_fixture(sp, "lightbar"); }},
        {"settings_pv_channels", 3, [&] { clear_focus(); fill_settings_pv_fixture(sp, "channels"); }},
        {"settings_pv_passthrough", 3, [&] { clear_focus(); fill_settings_pv_fixture(sp, "passthrough"); }},
        {"settings_pv_surround", 3, [&] { clear_focus(); fill_settings_pv_fixture(sp, "surround"); }},
        {"settings_pv_deep_blacks", 3, [&] { clear_focus(); fill_settings_pv_fixture(sp, "deep_blacks"); }},
        {"settings_pv_upscaling", 3, [&] { clear_focus(); fill_settings_pv_fixture(sp, "upscaling"); }},
        {"settings_pv_resume", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "resume"); }},
        {"settings_pv_resume_off", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "resume_off"); }},
        {"settings_pv_decoder", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "decoder"); }},
        {"settings_pv_decoder_sw", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "decoder_sw"); }},
        {"settings_pv_nav_sounds", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "nav_sounds"); }},
        {"settings_pv_nav_sounds_off", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "nav_sounds_off"); }},
        {"settings_pv_folders_first", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "folders_first"); }},
        {"settings_pv_folders_first_off", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "folders_first_off"); }},
        {"settings_pv_port_free", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "port_free"); }},
        {"settings_pv_port_busy", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "port_busy"); }},
        {"settings_pv_port_fallback", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "port_fallback"); }},
        {"settings_pv_port_none", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "port_none"); }},
        {"settings_pv_report", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "report"); }},
        {"settings_pv_report_nolink", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "report_nolink"); }},
        {"settings_pv_keyboard", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "keyboard"); }},
        {"settings_pv_keyboard_ps5", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "keyboard_ps5"); }},
        {"settings_pv_ask_stream", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "ask_stream"); }},
        {"settings_pv_ask_stream_off", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "ask_stream_off"); }},
        {"settings_pv_debug_overlay", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "debug_overlay"); }},
        {"settings_pv_debug_overlay_off", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "debug_overlay_off"); }},
        {"settings_pv_motion_off", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "motion_off"); }},
        {"settings_pv_refresh_60", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "refresh_60"); }},
        {"settings_pv_match_rate_off", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "match_rate_off"); }},
        {"settings_pv_channels_stereo", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "channels_stereo"); }},
        {"settings_pv_lightbar_off", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "lightbar_off"); }},
        {"settings_pv_sub_font_standard", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "sub_font_standard"); }},
        {"settings_pv_sub_font_bold", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "sub_font_bold"); }},
        {"settings_pv_sub_font_condensed", 3, [&] { clear_focus(); fill_settings_pv_extra_fixture(sp, "sub_font_condensed"); }},
        {"settings_pv_info_resume", 3, [&] { clear_focus(); fill_settings_pv_fixture(sp, "info_resume"); }},
        {"settings_pv_info_quit", 3, [&] { clear_focus(); fill_settings_pv_fixture(sp, "info_quit"); }},
        {"browser", 2, [&] { clear_focus(); bp.rows[3].is_focused = 1; bp.cursor_index = 3; }},
        {"browser_sources", 2, [&] { clear_focus(); bp.sidebar_focused = 1; bp.sidebar_index = 3; }},
        {"browser_menu", 2, [&] { clear_focus(); bp.rows[3].is_focused = 1; bp.cursor_index = 3;
                                   bp.action_menu_open = 1; bp.action_menu_focused = 3; }},
        {"browser_transfer", 2, [&] { clear_focus(); bp.rows[3].is_focused = 1; bp.cursor_index = 3;
                                       bp.transfer_modal_open = 1; }},
        {"browser_network", 2, [&] {
             /* #226: server cards with Online / Checking / Offline chips */
             clear_focus();
             static const char *net[4][4] = {
                 {"+ Add Server", "SMB or FTP", "SETUP", ""},
                 {"NAS", "192.168.0.20:445", "SMB", "Online  12 ms"},
                 {"Old PC", "192.168.0.8:4450", "SMB", "Offline"},
                 {"PS5 Local FTP", "127.0.0.1:2121", "FTP", "Checking"},
             };
             bp.row_count = 4;
             for (int i = 0; i < 4; ++i)
             {
                 bp.rows[i].name = net[i][0];
                 bp.rows[i].detail = net[i][1];
                 bp.rows[i].badge = net[i][2];
                 bp.rows[i].duration = "";
                 bp.rows[i].art = nullptr;
                 bp.rows[i].progress = -1;
                 bp.rows[i].is_favorite = 0;
                 bp.rows[i].icon_path = "../icons/icon_folder.png";
                 bp.rows[i].status_text = net[i][3];
                 bp.rows[i].status_kind = i == 0 ? 0 : (i == 1 ? 2 : (i == 2 ? 3 : 1));
             }
             bp.rows[1].is_focused = 1;
             bp.cursor_index = 1;
         }},
        {"about", 7, [&] { clear_focus(); fill_about_fixture(aboutp); aboutp.action_focused = 0; }},
        {"about_focused", 8, [&] { clear_focus(); fill_about_fixture(aboutp); aboutp.action_focused = 1; }},
        {"changelog", 9, [&] { clear_focus(); fill_changelog_fixture(changelogp); changelogp.releases[0].is_focused = 1; }},
        {"closed", 10, [&] { clear_focus(); }},
        {"dialog", 11, [&] { clear_focus(); fill_dialog_fixture(dialogp); }},
        {"reader", 13, [&] { clear_focus(); fill_reader_fixture(readerp); }},
        {"image", 14, [&] { clear_focus(); fill_image_fixture(imagep, image_px); }},
        {"webpage", 18, [&] {
             clear_focus();
             webp = {};
             webp.url = "https://en.wikipedia.org/wiki/PlayStation_5";
             webp.title = "PlayStation 5 - Wikipedia";
             webp.loading = 1;
             webp.can_back = 1;
             webp.state = 2;
             webp.pointer_visible = 1;
             webp.pointer_x = 900;
             webp.pointer_y = 520;
             webp.pointer_hand = 1;
             webp.hints_visible = 1;
         }},
        {"webpage_arrow", 18, [&] {
             clear_focus();
             webp = {};
             webp.url = "https://duckduckgo.com/";
             webp.state = 2;
             webp.pointer_visible = 1;
             webp.pointer_x = 700;
             webp.pointer_y = 400;
             webp.hints_visible = 1;
         }},
        {"webpage_starting", 18, [&] {
             clear_focus();
             webp = {};
             webp.url = "https://duckduckgo.com/";
             webp.state = 1;
             webp.message = "Starting the browser...";
             webp.hints_visible = 1;
         }},
        {"webpage_failed", 18, [&] {
             clear_focus();
             webp = {};
             webp.url = "https://duckduckgo.com/";
             webp.state = 3;
             webp.message = "The browser needs an ELF loader (elfldr on port 9021)";
             webp.hints_visible = 1;
         }},
        {"surround", 16, [&] { clear_focus(); fill_surround_fixture(srp, 0); }},
        {"surround_orb", 16, [&] { clear_focus(); fill_surround_fixture(srp, 1); }},
        {"surround_cal", 16, [&] { clear_focus(); fill_surround_fixture(srp, 2); }},
        {"provider", 17, [&] { clear_focus(); fill_provider_fixture(prp, provider_art, 2); }},
        {"livetv", 17, [&] { clear_focus(); fill_channels_fixture(prp, provider_art, 1, 0); }},
        {"livetv_setup", 17, [&] { clear_focus(); fill_channels_fixture(prp, provider_art, 1, 1); }},
        {"livetv_panel", 17, [&] { clear_focus(); fill_channels_fixture(prp, provider_art, 1, 2); }},
        {"xtream", 17, [&] { clear_focus(); fill_channels_fixture(prp, provider_art, 2, 0); }},
        {"xtream_setup", 17, [&] { clear_focus(); fill_channels_fixture(prp, provider_art, 2, 1); }},
        {"shelves", 17, [&] { clear_focus(); fill_shelves_fixture(prp, provider_art, 0, 2); }},
        {"shelves_low", 17, [&] { clear_focus(); fill_shelves_fixture(prp, provider_art, 2, 4); }},
        {"shelves_stremio", 17, [&] { clear_focus(); fill_provider_shelves_fixture(prp, provider_art, 0, 1, 2); }},
        {"shelves_details_trending", 17, [&] {
             clear_focus();
             fill_provider_shelves_fixture(prp, provider_art, 0, 2, 3);
             prp.selected_title = "Animals";
             prp.selected_subtitle = "2025  \xC2\xB7  2h 04m  \xC2\xB7  Thriller, Mystery";
             prp.selected_rating = "6.9";
             prp.selected_cert = "R";
             prp.selected_overview = "A retired detective is pulled back into a cold case when a stranger arrives with a photograph that should not exist.";
             prp.selected_cast = "Jon Bernthal, Anya Taylor-Joy";
         }},
        {"shelves_plex", 17, [&] { clear_focus(); fill_provider_shelves_fixture(prp, provider_art, 1, 0, 1); }},
        {"shelves_iptv", 17, [&] { clear_focus(); fill_provider_shelves_fixture(prp, provider_art, 2, 1, 0); }},
        {"shelves_iptv_low", 17, [&] { clear_focus(); fill_provider_shelves_fixture(prp, provider_art, 2, 2, 3); }},
        {"shelves_xtream", 17, [&] { clear_focus(); fill_provider_shelves_fixture(prp, provider_art, 3, 0, 1); }},
        {"provider_folder", 17, [&] { clear_focus(); fill_provider_fixture(prp, provider_art, 4); }},
        {"detail_overview", 17, [&] { clear_focus(); fill_detail_fixture(prp, provider_art, 0); }},
        {"detail_cast", 17, [&] { clear_focus(); fill_detail_fixture(prp, provider_art, 1); }},
        {"detail_details", 17, [&] { clear_focus(); fill_detail_fixture(prp, provider_art, 2); }},
        {"detail_show", 17, [&] { clear_focus(); fill_detail_fixture(prp, provider_art, 3); }},
        {"detail_streams", 17, [&] { clear_focus(); fill_detail_fixture(prp, provider_art, 5); }},
        {"detail_loading", 17, [&] { clear_focus(); fill_detail_fixture(prp, provider_art, 4); }},
        {"keyboard", 15, [&] { clear_focus(); fill_keyboard_fixture(kbp); }},
        {"keyboard_phone", 15, [&] { clear_focus(); fill_keyboard_phone_fixture(kbp); }},
        {"keyboard_noport", 15, [&] { clear_focus(); fill_keyboard_noport_fixture(kbp); }},
        {"keyboard_port_free", 15, [&] { clear_focus(); fill_keyboard_port_fixture(kbp, 1); }},
        {"keyboard_port_taken", 15, [&] { clear_focus(); fill_keyboard_port_fixture(kbp, 0); }},
        {"toast", 12, [&] { clear_focus(); fill_toast_fixture(toastp); toastp.visible = 1; toastp.alpha = 255; toastp.slide = 0; }},
    };

    std::vector<unsigned char> pixels(static_cast<std::size_t>(width * height * 4));
    stbi_flip_vertically_on_write(1);
    constexpr float kDt = 1.0f / 60.0f;
    hui::gfx::DrawList list;
    for (const Fixture &f : fixtures)
    {
        evo::kit::HomeScreen home;
        evo::kit::ListScreen lst;
        evo::kit::BrowserScreen brw;
        evo::kit::SettingsScreen stg;
        evo::kit::OsdScreen osd;
        evo::kit::NavRail rail;
        evo::kit::AboutScreen about;
        evo::kit::ChangelogScreen changelog;
        evo::kit::ClosedScreen closed;
        evo::kit::DialogOverlay dialog;
        evo::kit::ToastOverlay toast;
        evo::kit::ReaderScreen reader;
        evo::kit::ImageViewer image;
        evo::kit::WebPage webpage;
        evo::kit::KeyboardOverlay keyboard;
        evo::kit::SurroundScreen surround;
        evo::kit::ProviderGridScreen provider;
        f.setup();
        osd.set(op);
        osd.set_hud(hud);
        osd.enter();
        evo::kit::SubtitlePicker picker;
        evo::kit::MediaInfoCard info;
        picker.set(subp);
        picker.enter();
        info.set(mip);
        info.enter();
        about.set(aboutp);
        about.enter();
        changelog.set(changelogp);
        changelog.enter();
        closed.enter();
        dialog.set(dialogp);
        dialog.enter();
        toast.set(toastp);
        toast.enter();
        reader.set(readerp);
        reader.enter();
        image.set(imagep);
        image.enter();
        webpage.set(webp);
        webpage.enter();
        keyboard.set(kbp);
        keyboard.enter();
        surround.set(srp);
        provider.set(prp);
        provider.enter();
        home.set(launch);
        lst.set(lp);
        brw.set(bp);
        brw.enter();
        stg.set(sp);
        stg.enter();
        home.enter();
        lst.enter();
        rail.set(nav);
        float time = 0.0f;
        for (int frame = 0; frame < 90; ++frame)
        {
            if (f.kind == 0)
                home.update(kDt);
            else if (f.kind == 1)
                lst.update(kDt);
            else if (f.kind == 2)
                brw.update(kDt);
            else if (f.kind == 3)
                stg.update(kDt);
            else if (f.kind == 4)
                osd.update(kDt);
            else if (f.kind == 5)
                picker.update(kDt);
            else if (f.kind == 6)
                info.update(kDt);
            else if (f.kind == 7 || f.kind == 8)
                about.update(kDt);
            else if (f.kind == 9)
                changelog.update(kDt);
            else if (f.kind == 10)
                closed.update(kDt);
            else if (f.kind == 11)
                dialog.update(kDt);
            else if (f.kind == 12)
                toast.update(kDt);
            else if (f.kind == 13)
                reader.update(kDt);
            else if (f.kind == 14)
                image.update(kDt);
            else if (f.kind == 15)
                keyboard.update(kDt);
            else if (f.kind == 16)
                surround.update(kDt);
            else if (f.kind == 17)
                provider.update(kDt);
            else if (f.kind == 18)
                webpage.update(kDt);
            rail.update(kDt);
            textures.tick();
            time += kDt;
        }
        evo::kit::Context ctx{fonts, textures, palette, time};
        list.clear();
        if (f.kind == 0)
            home.draw(list, ctx);
        else if (f.kind == 1)
            lst.draw(list, ctx);
        else if (f.kind == 2)
            brw.draw(list, ctx);
        else if (f.kind == 3)
            stg.draw(list, ctx);
        else if (f.kind == 7 || f.kind == 8)
            about.draw(list, ctx);
        else if (f.kind == 9)
            changelog.draw(list, ctx);
        else if (f.kind == 10)
            closed.draw(list, ctx);
        else if (f.kind == 13)
            reader.draw(list, ctx);
        else if (f.kind == 14)
            image.draw(list, ctx);
        else if (f.kind == 16)
            surround.draw(list, ctx);
        else if (f.kind == 17)
            provider.draw(list, ctx);
        else if (f.kind == 18)
        {
            webpage.set_page(webp.state == 2 ? textures.pixels(web_px.data(), kWebW, kWebH) : 0u, 1.0f, 1.0f);
            webpage.draw(list, ctx);
        }
        const bool over_video = (f.kind >= 4 && f.kind <= 6) || f.kind == 11 || f.kind == 12 || f.kind == 15;
        if (over_video)
        {
            /* A stand-in for the video under the OSD and its pop-ups. */
            list.image(textures.pixels(hero_art.data(), 960, 540), {0, 0, 1920, 1080}, hui::gfx::kFullUv,
                       hui::gfx::Color::rgb(0xffffff));
            if (f.kind == 4)
                osd.draw(list, ctx);
            else if (f.kind == 5)
                picker.draw(list, ctx);
            else if (f.kind == 6)
                info.draw(list, ctx);
            else if (f.kind == 11)
                dialog.draw(list, ctx);
            else if (f.kind == 12)
                toast.draw(list, ctx);
            else
                keyboard.draw(list, ctx);
        }
        else if (f.kind != 10 && f.kind != 14)
        {
            rail.draw(list, ctx);
        }
        renderer.begin();
        renderer.draw(list);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        renderer.present(framebuffer, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (std::size_t i = 3; i < pixels.size(); i += 4)
            pixels[i] = 255;
        const std::string path = output + "/hui_" + f.name + ".png";
        stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4);
        std::fprintf(stderr, "wrote %s: %zu shapes, %zu draw calls, GL error 0x%x\n", path.c_str(),
                     renderer.last_instances(), renderer.last_draw_calls(), glGetError());

        /* Text that does not fit (overflow_check.inc): one line per finding in
         * hui_overflow.txt, and a copy of the frame with the boxes in red. */
        /* Every text run and box of the frame, for the cross-screen
         * consistency report (tools/ui_consistency.py). */
        for (const auto &t : overflow::group_text(list))
            std::fprintf(metrics, "%s\ttext\t%.0f\t%.0f\t%.0f\t%.0f\t%.2f\t%d\t%02x%02x%02x\t%.2f\n", f.name, t.ink.x0,
                         t.ink.y0, t.ink.w(), t.ink.h(), t.range, t.slot, (int)(t.rgba[0] * 255), (int)(t.rgba[1] * 255),
                         (int)(t.rgba[2] * 255), t.rgba[3]);
        for (const auto &c : list.instances())
        {
            const int shape = static_cast<int>(c.params[3]);
            if ((shape == 0 || shape == 8) && c.rect[2] >= 16 && c.rect[3] >= 12 && std::max(c.color_top[3], c.border_color[3]) > 0.04f)
                std::fprintf(metrics, "%s\tbox\t%.0f\t%.0f\t%.0f\t%.0f\t%.1f\t%.1f\t%02x%02x%02x\t%.2f\n", f.name, c.rect[0],
                             c.rect[1], c.rect[2], c.rect[3], c.params[0], c.params[1], (int)(c.color_top[0] * 255),
                             (int)(c.color_top[1] * 255), (int)(c.color_top[2] * 255), c.color_top[3]);
        }
        const auto found = overflow::check(list, 1920.0f, 1080.0f);
        if (!found.empty() && width == 1920 && height == 1080)
        {
            std::vector<unsigned char> marked = pixels;
            for (const auto &o : found)
            {
                std::fprintf(overflow_report, "%-36s %-9s text %4.0f,%4.0f %4.0fx%-3.0f  vs %4.0f,%4.0f %4.0fx%-3.0f  by %.0f px\n",
                             f.name, o.kind, o.box.x0, o.box.y0, o.box.w(), o.box.h(), o.other.x0, o.other.y0,
                             o.other.w(), o.other.h(), o.by);
                overflow::outline(marked, width, height, o.box, 255, 40, 40);
                if (o.other.w() > 0)
                    overflow::outline(marked, width, height, o.other, 255, 200, 0);
            }
            const std::string mpath = output + "/hui_" + f.name + ".overflow.png";
            stbi_write_png(mpath.c_str(), width, height, 4, marked.data(), width * 4);
        }
    }
    return 0;
}
