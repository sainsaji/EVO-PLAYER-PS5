#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include "../projects/evoplayer/ui_rml/include/evo_rmlui_bridge.h"

static void save_bmp_24(const char* filename, const uint32_t* fb, int width, int height) {
    std::ofstream file(filename, std::ios::binary);
    if (!file) return;

    int row_padded = (width * 3 + 3) & (~3);
    int image_size = row_padded * height;
    int file_size = 54 + image_size;

    uint8_t header[54] = {
        'B', 'M',
        (uint8_t)(file_size), (uint8_t)(file_size >> 8), (uint8_t)(file_size >> 16), (uint8_t)(file_size >> 24),
        0, 0, 0, 0,
        54, 0, 0, 0,
        40, 0, 0, 0,
        (uint8_t)(width), (uint8_t)(width >> 8), (uint8_t)(width >> 16), (uint8_t)(width >> 24),
        (uint8_t)(height), (uint8_t)(height >> 8), (uint8_t)(height >> 16), (uint8_t)(height >> 24),
        1, 0,
        24, 0,
        0, 0, 0, 0,
        (uint8_t)(image_size), (uint8_t)(image_size >> 8), (uint8_t)(image_size >> 16), (uint8_t)(image_size >> 24),
        0x13, 0x0B, 0, 0,
        0x13, 0x0B, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };
    file.write((const char*)header, 54);

    std::vector<uint8_t> row_buf(row_padded, 0);
    for (int y = height - 1; y >= 0; y--) {
        const uint32_t* src_row = &fb[y * width];
        for (int x = 0; x < width; x++) {
            uint32_t pixel = src_row[x];
            uint8_t r = pixel & 0xFF;
            uint8_t g = (pixel >> 8) & 0xFF;
            uint8_t b = (pixel >> 16) & 0xFF;
            row_buf[x * 3 + 0] = b;
            row_buf[x * 3 + 1] = g;
            row_buf[x * 3 + 2] = r;
        }
        file.write((const char*)row_buf.data(), row_padded);
    }
}

int main(int argc, char** argv) {
    const int width = 1920;
    const int height = 1080;
    std::vector<uint32_t> fb_playback(width * height);
    std::vector<uint32_t> fb_dialog(width * height);
    std::vector<uint32_t> fb_settings(width * height);
    std::vector<uint32_t> fb_settings_sub(width * height);

    // Simulate cinematic film frame background for player
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint8_t r = 18 + (y * 22 / height);
            uint8_t g = 24 + (x * 18 / width);
            uint8_t b = 40 + (y * 35 / height);
            uint32_t px = 0xFF000000 | (b << 16) | (g << 8) | r;
            fb_playback[y * width + x] = px;
            fb_dialog[y * width + x] = px;
            fb_settings[y * width + x] = 0xFF06090E;
            fb_settings_sub[y * width + x] = 0xFF06090E;
        }
    }

    if (!evo_rmlui_init(width, height)) {
        std::cerr << "Failed to initialize RmlUi playback engine!" << std::endl;
        return 1;
    }

    // 1. Render Playback OSD
    evo_playback_osd_params_t p;
    memset(&p, 0, sizeof(p));
    p.title = "Big Buck Bunny (2008)";
    p.metadata = "1080p  H.264  6CH";
    p.res_badge = "1080p FHD";
    p.hdr_badge = "";
    p.codec_badge = "AVC / H.264";
    p.fps_badge = "60 FPS";
    p.audio_badge = "5.1 Surround";
    p.position_sec = 142.0;
    p.duration_sec = 596.0;
    p.percentage = 142.0 / 596.0;
    p.paused = 0;
    p.scrub_active = 0;
    p.scrub_target = 0.0;
    p.audio_track = "English AC3 5.1";
    p.sub_track = "None";
    p.view_mode = 0;
    p.show_stats = 0;
    p.alpha = 255;

    evo_rmlui_update_playback_params(&p);
    evo_rmlui_render_playback_osd(fb_playback.data(), width, height);
    save_bmp_24("output/uiview/rml_playback.bmp", fb_playback.data(), width, height);

    // 2. Render Confirmation / Resume Dialog
    evo_rmlui_dialog_params_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.eyebrow = "RESUME PLAYBACK";
    dlg.title = "Big Buck Bunny (2008)";
    dlg.detail = "STOPPED AT 02:22 OF 09:56";
    dlg.progress_pct = 142.0 / 596.0;
    dlg.action_count = 2;
    dlg.actions[0].icon_path = "projects/evoplayer/assets/icons/btn_cross.png";
    dlg.actions[0].label = "RESUME";
    dlg.actions[0].is_primary = 1;
    dlg.actions[1].icon_path = "projects/evoplayer/assets/icons/btn_circle.png";
    dlg.actions[1].label = "START OVER";
    dlg.actions[1].is_primary = 0;

    evo_rmlui_update_dialog(&dlg);
    evo_rmlui_render_dialog(fb_dialog.data(), width, height);
    save_bmp_24("output/uiview/rml_dialog.bmp", fb_dialog.data(), width, height);

    // 3. Render Settings Hub
    evo_rmlui_settings_params_t set;
    memset(&set, 0, sizeof(set));
    set.title = "SETTINGS";
    set.subtitle = "APPLICATION & PLAYBACK PREFERENCES";
    set.counter = "1 OF 4";
    set.rail_active_idx = 5;
    set.rail_focused = 0;
    set.row_count = 4;

    set.rows[0].title = "PLAYBACK & VIDEO";
    set.rows[0].detail = "PROFILE, ASPECT RATIO & RESUME";
    set.rows[0].icon_path = "projects/evoplayer/assets/icons/icon_resume.png";
    set.rows[0].has_chevron = 1;
    set.rows[0].is_focused = 1;

    set.rows[1].title = "SUBTITLES";
    set.rows[1].detail = "AUTO-DETECT & DEFAULT SIZING";
    set.rows[1].icon_path = "projects/evoplayer/assets/icons/icon_subtitles.png";
    set.rows[1].has_chevron = 1;
    set.rows[1].is_focused = 0;

    set.rows[2].title = "INTERFACE & CONTROLS";
    set.rows[2].detail = "THEMES, SOUNDS, LIGHTBAR & SORTING";
    set.rows[2].icon_path = "projects/evoplayer/assets/icons/icon_palette.png";
    set.rows[2].has_chevron = 1;
    set.rows[2].is_focused = 0;

    set.rows[3].title = "SYSTEM & DIAGNOSTICS";
    set.rows[3].detail = "DEVELOPER TOOLS & MEDIA TILE";
    set.rows[3].icon_path = "projects/evoplayer/assets/icons/icon_developer_tools.png";
    set.rows[3].has_chevron = 1;
    set.rows[3].is_focused = 0;

    evo_rmlui_update_settings(&set);
    evo_rmlui_render_settings(fb_settings.data(), width, height);
    save_bmp_24("output/uiview/rml_settings.bmp", fb_settings.data(), width, height);

    // 4. Render Settings Subscreen (Playback)
    memset(&set, 0, sizeof(set));
    set.title = "PLAYBACK & VIDEO";
    set.subtitle = "SETTINGS  -  PROFILES, ASPECT RATIO & RESUME";
    set.counter = "1 OF 4";
    set.rail_active_idx = 5;
    set.rail_focused = 0;
    set.row_count = 4;

    set.rows[0].title = "PLAYBACK PROFILE";
    set.rows[0].detail = "SELECT ENGINE PROFILE";
    set.rows[0].icon_path = "projects/evoplayer/assets/icons/icon_settings.png";
    set.rows[0].badge = "Performance";
    set.rows[0].has_chevron = 1;
    set.rows[0].is_focused = 1;

    set.rows[1].title = "DEFAULT ASPECT RATIO";
    set.rows[1].detail = "FIT, FILL OR STRETCH";
    set.rows[1].icon_path = "projects/evoplayer/assets/icons/icon_aspect.png";
    set.rows[1].badge = "FIT";
    set.rows[1].has_chevron = 1;
    set.rows[1].is_focused = 0;

    set.rows[2].title = "RESUME PLAYBACK";
    set.rows[2].detail = "REMEMBER PLAYBACK POSITION";
    set.rows[2].icon_path = "projects/evoplayer/assets/icons/icon_resume.png";
    set.rows[2].badge = "ON";
    set.rows[2].has_chevron = 1;
    set.rows[2].is_focused = 0;

    set.rows[3].title = "SURROUND SOUND TEST";
    set.rows[3].detail = "5.1 & 7.1 SPEAKER CHANNEL VERIFICATION";
    set.rows[3].icon_path = "projects/evoplayer/assets/icons/icon_resume.png";
    set.rows[3].badge = "";
    set.rows[3].has_chevron = 1;
    set.rows[3].is_focused = 0;

    evo_rmlui_update_settings(&set);
    evo_rmlui_render_settings(fb_settings_sub.data(), width, height);
    save_bmp_24("output/uiview/rml_settings_sub.bmp", fb_settings_sub.data(), width, height);

    std::cout << "Rendered all preview screens successfully" << std::endl;
    return 0;
}
