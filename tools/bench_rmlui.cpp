#include <iostream>
#include <chrono>
#include <vector>
#include <cstdint>
#include <cstring>
#include "../projects/evoplayer/ui_rml/include/evo_rmlui_bridge.h"

extern double g_prof_quad_time;
extern double g_prof_tri_time;
extern double g_prof_clip_time;
extern int g_prof_quad_count;
extern int g_prof_tri_count;
extern int g_prof_clip_count;
extern double g_prof_update_ctx_time;
extern double g_prof_render_ctx_time;
extern bool g_debug_print_draws;

int main() {
    const int W = 1920, H = 1080;
    std::vector<uint32_t> fb(W * H, 0xFF000000);

    if (!evo_rmlui_init(W, H)) {
        std::cerr << "Init failed\n";
        return 1;
    }

    evo_rmlui_theme_t theme;
    memset(&theme, 0, sizeof(theme));
    theme.name = "MIDNIGHT";
    theme.bg_top = 0xFF160B06;
    theme.bg_bottom = 0xFF090402;
    theme.surface = 0xEB2E1B12;
    theme.surface_sel = 0xF54C2E1B;
    theme.border = 0xAA553B2A;
    theme.border_sel = 0xDCFFCD00;
    theme.accent = 0xFFFFCD00;
    theme.accent_soft = 0x3CFFA800;
    theme.accent_alt = 0xFFFF5C7A;
    theme.text_primary = 0xFFFFF3EC;
    theme.text_secondary = 0xFFCCB29F;
    theme.text_muted = 0xFF8C715E;
    evo_rmlui_set_theme(&theme);

    evo_rmlui_nav_params_t nav = { 1, 0, 1, 1 };
    evo_rmlui_update_nav(&nav);

    evo_rmlui_browser_params_t p;
    memset(&p, 0, sizeof(p));
    p.path = "/mnt/usb0/Movies/4K_Collection";
    p.title = "USB 3.0 MEDIA DRIVE";
    p.total_count = 50;
    p.cursor_index = 1;
    p.row_count = 12;
    p.at_root = 0;
    p.rail_focused = 0;

    for (int i = 0; i < 12; i++) {
        p.rows[i].name = "Movie.Title.2026.2160p.UHD.Remux.mkv";
        p.rows[i].detail = "42.5 GB • 2h 15m";
        p.rows[i].icon_path = "../icons/icon_resume.png";
        p.rows[i].badge = "4K";
        p.rows[i].progress = (i == 2) ? 450 : -1;
        p.rows[i].is_favorite = (i % 3 == 0);
        p.rows[i].is_focused = (i == 1);
    }

    p.ins_name = "Movie.Title.2026.2160p.UHD.Remux.mkv";
    p.ins_kind = "VIDEO / MATROSKA";
    p.ins_ext = "MKV";
    p.ins_prop_count = 7;
    p.ins_props[0].key = "SIZE";       p.ins_props[0].value = "42.5 GB";
    p.ins_props[1].key = "CONTAINER";  p.ins_props[1].value = "Matroska / MKV";
    p.ins_props[2].key = "DURATION";   p.ins_props[2].value = "02:15:30";
    p.ins_props[3].key = "RESOLUTION"; p.ins_props[3].value = "3840 x 2160 (16:9)";
    p.ins_props[4].key = "VIDEO";      p.ins_props[4].value = "HEVC Main 10 (HDR10)";
    p.ins_props[5].key = "AUDIO";      p.ins_props[5].value = "TrueHD 7.1 Atmos (48 kHz)";
    p.ins_props[6].key = "SUBTITLES";  p.ins_props[6].value = "English (PGS), French";

    evo_rmlui_update_browser(&p);
    evo_rmlui_update_nav(&nav);

    // Warm up
    evo_rmlui_render_browser(fb.data(), W, H);

    // Benchmark 20 continuous navigation frames
    g_prof_quad_time = 0; g_prof_tri_time = 0; g_prof_clip_time = 0;
    g_prof_quad_count = 0; g_prof_tri_count = 0; g_prof_clip_count = 0;
    g_prof_update_ctx_time = 0; g_prof_render_ctx_time = 0;

    int N = 20;
    auto start_all = std::chrono::high_resolution_clock::now();
    for (int frame = 0; frame < N; frame++) {
        p.cursor_index = frame % 12;
        for (int i = 0; i < 12; i++) {
            p.rows[i].is_focused = (i == p.cursor_index);
        }
        evo_rmlui_update_browser(&p);
        evo_rmlui_update_nav(&nav);
        evo_rmlui_render_browser(fb.data(), W, H);
    }
    auto end_all = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(end_all - start_all).count();

    std::cout << "Render Breakdown per frame (avg over " << N << " frames):\n";
    std::cout << "  Context::Update:       " << (g_prof_update_ctx_time / N) << " ms\n";
    std::cout << "  Context::Render:       " << (g_prof_render_ctx_time / N) << " ms\n";
    std::cout << "    - Fast Quads:        " << (g_prof_quad_time / N) << " ms (" << (g_prof_quad_count / N) << " calls)\n";
    std::cout << "    - Triangles Acc:     " << (g_prof_tri_time / N) << " ms (" << (g_prof_tri_count / N) << " calls)\n";
    std::cout << "    - Clip Mask Op:      " << (g_prof_clip_time / N) << " ms (" << (g_prof_clip_count / N) << " calls)\n";
    std::cout << "  Total Navigation Frame Time: " << (total_ms / N) << " ms (" << (1000.0 / (total_ms / N)) << " FPS)\n";

    evo_rmlui_shutdown();
    return 0;
}
