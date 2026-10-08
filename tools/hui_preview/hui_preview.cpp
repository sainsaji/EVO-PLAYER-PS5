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
#include "misc_fixtures.inc"
#include "evo_hui_extra.hpp"
#include "extra_fixtures.inc"

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
    fonts.pixel = fonts.mono;
    fonts.hand = fonts.regular;

    HostTextures host(renderer, assets);
    evo::kit::TextureCache textures(host);
    evo::kit::Palette palette;

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
    launch.version = "VERSION 0.12.0";
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
    const char *lib[][3] = {{"BROWSE", "Videos and folders on USB storage", "../icons/icon_browse_usb.png"},
                            {"RECENT", "Pick up where you left off", "../icons/icon_recent_files.png"},
                            {"FAVORITES", "Media you saved for later", "../icons/icon_favorites.png"},
                            {"PROVIDERS", "Network sources: IPTV, Emby, Jellyfin", "../icons/icon_emby.png"},
                            {"SETTINGS", "Playback and display preferences", "../icons/icon_settings.png"},
                            {"ABOUT", "Credits and project info", "../icons/icon_about_support.png"}};
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
    evo_keyboard_params_t kbp{};
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
    subp.sync_str = "+120 ms";
    subp.preview_text = "Cells interlinked within cells interlinked.";
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
            lp.rows[i].is_focused = 0;
        lp.cursor_index = -1;
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
        {"subtitles", 5, [&] { clear_focus(); }},
        {"mediainfo", 6, [&] { clear_focus(); }},
        {"settings_sidebar", 3, [&] { clear_focus(); fill_settings_fixture(sp, 0); }},
        {"settings_rows", 3, [&] { clear_focus(); fill_settings_fixture(sp, 1); }},
        {"browser", 2, [&] { clear_focus(); bp.rows[3].is_focused = 1; bp.cursor_index = 3; }},
        {"browser_sources", 2, [&] { clear_focus(); bp.sidebar_focused = 1; bp.sidebar_index = 3; }},
        {"browser_menu", 2, [&] { clear_focus(); bp.rows[3].is_focused = 1; bp.cursor_index = 3;
                                   bp.action_menu_open = 1; bp.action_menu_focused = 3; }},
        {"browser_transfer", 2, [&] { clear_focus(); bp.rows[3].is_focused = 1; bp.cursor_index = 3;
                                       bp.transfer_modal_open = 1; }},
        {"about", 7, [&] { clear_focus(); fill_about_fixture(aboutp); aboutp.action_focused = 0; }},
        {"about_focused", 8, [&] { clear_focus(); fill_about_fixture(aboutp); aboutp.action_focused = 1; }},
        {"changelog", 9, [&] { clear_focus(); fill_changelog_fixture(changelogp); changelogp.releases[0].is_focused = 1; }},
        {"closed", 10, [&] { clear_focus(); }},
        {"dialog", 11, [&] { clear_focus(); fill_dialog_fixture(dialogp); }},
        {"reader", 13, [&] { clear_focus(); fill_reader_fixture(readerp); }},
        {"image", 14, [&] { clear_focus(); fill_image_fixture(imagep, image_px); }},
        {"keyboard", 15, [&] { clear_focus(); fill_keyboard_fixture(kbp); }},
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
        evo::kit::KeyboardOverlay keyboard;
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
        keyboard.set(kbp);
        keyboard.enter();
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
    }
    return 0;
}
