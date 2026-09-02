/*
 * RmlUi frame profiler — host, no console.
 *
 * Answers docs/evo-pro/gpu-rendering-plan.md §6: attribute the ~90 ms RmlUi
 * frame and decide whether Step 1 (dirty-flag the surface) alone is enough or
 * Step 2 (AGC) is required.
 *
 * Links the real drawing code (evo_rmlui_render.cpp, evo_rmlui_app.cpp) exactly
 * as tools/uiview_playback_rml.cpp does. Build + run: tools/prof_rmlui.sh
 *
 * For each representative screen it measures two regimes:
 *   IDLE  — same state pushed every frame (a static menu just sitting there).
 *           This is the cost Step 1 deletes: nothing changed, yet the whole
 *           context re-rasterises.
 *   NAV   — cursor moves every frame (D-pad held). Step 1 cannot skip these;
 *           this is the floor a dirty-flag approach leaves on the table.
 *
 * Then a composite/copy microbench: what Step 1 *adds back* every frame
 * (blit the cached surface into the rotating VideoOut buffer) at 1080p and 4K.
 */

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>

#include "../projects/evoplayer/ui_rml/include/evo_rmlui_bridge.h"
#include "../projects/evoplayer/ui_rml/include/evo_rmlui_prof.h"

static double now_ms() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

static const char* ICON = "projects/evoplayer/assets/icons/icon_settings.png";
static const char* ICON_R = "projects/evoplayer/assets/icons/icon_recent_files.png";

/* ---- demo artwork (same raw-BGRA path the decoder cover cache feeds) ---- */
static std::vector<uint32_t> make_art(int w, int h, int seed) {
    std::vector<uint32_t> px((size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int r = (seed * 53 + x / 3) & 0xFF;
            int g = (seed * 97 + y / 3) & 0xFF;
            int b = (seed * 31 + (x + y) / 4) & 0xFF;
            px[(size_t)y * w + x] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    return px;
}

/* ------------------------------------------------------------------ */

struct Row { double idle_total, nav_total; };

static void print_breakdown(const char* label, const EvoRmlProf& p, int n, double wall_per_frame) {
    double upd = p.update_ms / n, ren = p.render_ms / n;
    double quad = p.geo_quad_ms / n, tri = p.geo_tri_ms / n;
    double clip = p.clip_ms / n, gtex = p.gentex_ms / n, ltex = p.loadtex_ms / n;
    printf("  %-6s  wall %6.2f ms/frame  (%.0f fps)\n", label, wall_per_frame,
           wall_per_frame > 0 ? 1000.0 / wall_per_frame : 0);
    printf("            Context::Update   %6.2f ms\n", upd);
    printf("            Context::Render   %6.2f ms\n", ren);
    printf("              fast quads      %6.2f ms  x%-6.1f\n", quad, (double)p.geo_quad_n / n);
    printf("              tri rasterizer  %6.2f ms  x%-6.1f\n", tri, (double)p.geo_tri_n / n);
    printf("              clip masks      %6.2f ms  x%-6.1f\n", clip, (double)p.clip_n / n);
    printf("              glyph raster    %6.2f ms  x%-6.1f  (%ld px)\n",
           gtex, (double)p.gentex_n / n, p.gentex_px / n);
    printf("              texture load    %6.2f ms  x%-6.1f\n", ltex, (double)p.loadtex_n / n);
    double acct = upd + ren;
    printf("            accounted %.2f ms / wall %.2f ms  (%.0f%%)\n",
           acct, wall_per_frame, wall_per_frame > 0 ? 100.0 * acct / wall_per_frame : 0);
}

/* -------- launch screen (the 6-7 fps case from app.h) -------- */
static void build_launch(evo_rmlui_launch_params_t& p,
                         std::vector<uint32_t>& hero,
                         std::vector<std::vector<uint32_t>>& covers,
                         int focus_tile) {
    memset(&p, 0, sizeof(p));
    p.app_name = "EVO PLAYER";
    p.version = "VERSION 0.7.0";
    p.clock = "21:48";
    p.theme_name = "MIDNIGHT";
    p.hero_eyebrow = "CONTINUE WATCHING";
    p.hero_title = "Blade Runner 2049";
    p.hero_detail = "1H 42M LEFT  -  HEVC MAIN 10  -  HDR10  -  DTS-HD MA 5.1";
    p.hero_action = "RESUME";
    p.hero_progress = 412;
    p.hero_art = hero.data();
    p.hero_art_w = 560; p.hero_art_h = 315;
    p.hero_focused = (focus_tile == 0);

    p.recent_total = 9;
    p.recent_visible = 6;
    p.recent_cursor = (focus_tile >= 1 && focus_tile <= 6) ? focus_tile - 1 : -1;
    static const char* t[6] = { "Blade Runner 2049", "Dune Part Two", "Grand Budapest Hotel",
                                "Arrival", "Interstellar 2160p HDR10", "Atmos Demo Disc 2024" };
    for (int i = 0; i < 6; i++) {
        p.recent[i].title = t[i];
        p.recent[i].detail = "1H 42M LEFT";
        p.recent[i].icon_path = ICON_R;
        p.recent[i].progress = 400 + i * 40;
        p.recent[i].art = covers[i].data();
        p.recent[i].art_w = 320; p.recent[i].art_h = 180;
        p.recent[i].is_focused = (focus_tile == i + 1);
    }
    static const char* lib[6] = { "BROWSE", "RECENT", "FAVORITES", "EMBY", "SETTINGS", "ABOUT" };
    p.library_visible = 6;
    for (int i = 0; i < 6; i++) {
        p.library[i].title = lib[i];
        p.library[i].detail = "Library destination";
        p.library[i].icon_path = ICON;
        p.library[i].progress = -1;
        p.library[i].is_focused = (focus_tile == i + 7);
    }
}

static void build_settings(evo_rmlui_settings_params_t& s, int focus) {
    memset(&s, 0, sizeof(s));
    s.title = "SETTINGS";
    s.subtitle = "APPLICATION & PLAYBACK PREFERENCES";
    s.counter = "1 OF 4";
    s.rail_active_idx = 5;
    s.row_count = 4;
    static const char* t[4] = { "PLAYBACK & VIDEO", "SUBTITLES", "INTERFACE & CONTROLS", "SYSTEM & DIAGNOSTICS" };
    for (int i = 0; i < 4; i++) {
        s.rows[i].title = t[i];
        s.rows[i].detail = "PROFILE, ASPECT RATIO & RESUME";
        s.rows[i].icon_path = ICON;
        s.rows[i].has_chevron = 1;
        s.rows[i].is_focused = (i == focus);
    }
}

static void build_browser(evo_rmlui_browser_params_t& p,
                          std::vector<uint32_t>& preview, int focus) {
    memset(&p, 0, sizeof(p));
    p.path = "/usb0/Movies";
    p.title = "USB DRIVE";
    p.at_root = 0;
    p.total_count = 37;
    p.cursor_index = focus;
    p.row_count = 12;
    for (int i = 0; i < 12; i++) {
        p.rows[i].name = "Blade Runner 2049.mkv";
        p.rows[i].detail = "MKV - 24.8 GB - 2H 44M";
        p.rows[i].icon_path = ICON_R;
        p.rows[i].progress = (i % 3 == 0) ? 412 : -1;
        p.rows[i].is_favorite = (i % 4 == 0);
        p.rows[i].is_focused = (i == focus);
    }
    p.ins_name = "Blade Runner 2049.mkv";
    p.ins_kind = "VIDEO";
    p.ins_ext = "MKV";
    p.ins_preview_badge = "2H 44M";
    p.ins_preview = preview.data();
    p.ins_preview_w = 560; p.ins_preview_h = 315;
    static const char* k[7] = { "SIZE", "CONTAINER", "DURATION", "RESOLUTION", "VIDEO", "AUDIO", "SUBTITLES" };
    static const char* v[7] = { "24.8 GB", "MATROSKA", "2H 44M", "3840 x 2160", "HEVC MAIN 10", "DTS-HD MA 5.1", "3" };
    p.ins_prop_count = 7;
    for (int i = 0; i < 7; i++) { p.ins_props[i].key = k[i]; p.ins_props[i].value = v[i]; }
}

/* ------------------------------------------------------------------ */
enum Screen { S_LAUNCH, S_SETTINGS, S_BROWSER };

static void run_screen(const char* name, Screen sc, std::vector<uint32_t>& fb,
                       int W, int H, int N) {
    std::vector<uint32_t> hero = make_art(560, 315, 3);
    std::vector<std::vector<uint32_t>> covers;
    for (int i = 0; i < 6; i++) covers.push_back(make_art(320, 180, 5 + i * 7));
    std::vector<uint32_t> preview = make_art(560, 315, 11);

    evo_rmlui_nav_params_t nav; memset(&nav, 0, sizeof(nav));
    nav.active_section = (sc == S_LAUNCH) ? 0 : (sc == S_BROWSER ? 1 : 5);
    nav.cursor_index = nav.active_section;
    nav.visible = 1;
    evo_rmlui_update_nav(&nav);

    auto push = [&](int focus) {
        if (sc == S_LAUNCH) {
            evo_rmlui_launch_params_t p; build_launch(p, hero, covers, focus);
            evo_rmlui_update_launch(&p);
            evo_rmlui_update_nav(&nav);
            evo_rmlui_render_launch(fb.data(), W, H);
        } else if (sc == S_SETTINGS) {
            evo_rmlui_settings_params_t p; build_settings(p, focus % 4);
            evo_rmlui_update_settings(&p);
            evo_rmlui_update_nav(&nav);
            evo_rmlui_render_settings(fb.data(), W, H);
        } else {
            evo_rmlui_browser_params_t p; build_browser(p, preview, focus % 12);
            evo_rmlui_update_browser(&p);
            evo_rmlui_update_nav(&nav);
            evo_rmlui_render_browser(fb.data(), W, H);
        }
    };

    /* warm up: font faces, atlases, icon PNGs, geometry compile */
    for (int i = 0; i < 5; i++) push(1);

    printf("\n=== %s ===\n", name);

    /* IDLE: identical state every frame */
    evo_prof_reset();
    double t0 = now_ms();
    for (int f = 0; f < N; f++) push(1);
    double idle_wall = (now_ms() - t0) / N;
    EvoRmlProf idle = g_evo_rml_prof;
    print_breakdown("IDLE", idle, N, idle_wall);

    /* NAV: cursor moves every frame */
    evo_prof_reset();
    t0 = now_ms();
    for (int f = 0; f < N; f++) push(1 + (f % 6));
    double nav_wall = (now_ms() - t0) / N;
    EvoRmlProf navp = g_evo_rml_prof;
    print_breakdown("NAV", navp, N, nav_wall);
}

/* -------- composite / copy microbench -------- */
static void composite_bench() {
    printf("\n=== composite / framebuffer copy (what Step 1 adds back per frame) ===\n");
    struct Res { const char* n; int w, h; };
    Res res[] = { { "1080p", 1920, 1080 }, { "4K", 3840, 2160 } };
    const int N = 200;

    for (auto& r : res) {
        size_t px = (size_t)r.w * r.h;
        std::vector<uint32_t> src(px, 0x80303030), dst(px, 0xFF000000);

        double t0 = now_ms();
        for (int i = 0; i < N; i++) std::memcpy(dst.data(), src.data(), px * 4);
        double memcpy_ms = (now_ms() - t0) / N;

        t0 = now_ms();
        for (int i = 0; i < N; i++) std::fill(dst.begin(), dst.end(), 0xFF000000u);
        double fill_ms = (now_ms() - t0) / N;

        /* premultiplied "over" blend, scalar — worst case for an alpha surface
         * composite (no SIMD, mirrors blend_premul in evo_rmlui_render.cpp) */
        t0 = now_ms();
        for (int i = 0; i < N; i++) {
            uint32_t* d = dst.data(); const uint32_t* s = src.data();
            for (size_t k = 0; k < px; k++) {
                uint32_t sp = s[k], a = sp >> 24, inv = 255 - a;
                uint32_t dp = d[k];
                uint32_t rb = (((dp & 0x00FF00FF) * inv) >> 8) & 0x00FF00FF;
                uint32_t g  = (((dp & 0x0000FF00) * inv) >> 8) & 0x0000FF00;
                d[k] = 0xFF000000u | (((sp & 0x00FFFFFF)) + rb + g);
            }
        }
        double blend_ms = (now_ms() - t0) / N;

        printf("  %-6s  memcpy %5.2f ms   std::fill %5.2f ms   scalar alpha-composite %5.2f ms\n",
               r.n, memcpy_ms, fill_ms, blend_ms);
    }
}

int main() {
    const int W = 1920, H = 1080;
    std::vector<uint32_t> fb((size_t)W * H, 0xFF0E0906);

    if (!evo_rmlui_init(W, H)) { printf("init failed\n"); return 1; }
    evo_rmlui_set_version("0.7.0");

    evo_rmlui_theme_t th; memset(&th, 0, sizeof(th));
    th.name = "MIDNIGHT";
    th.bg_top = 0xFF160B06; th.bg_bottom = 0xFF090402;
    th.surface = 0xEB2E1B12; th.surface_sel = 0xF54C2E1B;
    th.border = 0xAA553B2A; th.border_sel = 0xDCFFCD00;
    th.accent = 0xFFF8BD38; th.accent_soft = 0x3CFFA800; th.accent_alt = 0xFFFF5C7A;
    th.text_primary = 0xFFFFF3EC; th.text_secondary = 0xFFCCB29F; th.text_muted = 0xFF8C715E;
    evo_rmlui_set_theme(&th);

    const int N = 120;
    run_screen("LAUNCH  (hero + recent shelf + library)", S_LAUNCH, fb, W, H, N);
    run_screen("SETTINGS  (4-row list)", S_SETTINGS, fb, W, H, N);
    run_screen("BROWSER  (12-row list + inspector)", S_BROWSER, fb, W, H, N);

    composite_bench();

    evo_rmlui_shutdown();
    return 0;
}
