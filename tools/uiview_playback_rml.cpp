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
    std::vector<uint32_t> fb(width * height, 0xFF06090E);

    if (!evo_rmlui_init(width, height)) {
        std::cerr << "Failed to initialize RmlUi playback engine!" << std::endl;
        return 1;
    }

    // 1. Settings Main Hub
    {
        std::fill(fb.begin(), fb.end(), 0xFF06090E);
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
        evo_rmlui_render_settings(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_settings_main.bmp", fb.data(), width, height);
    }

    // 2. Playback & Video Subsection
    {
        std::fill(fb.begin(), fb.end(), 0xFF06090E);
        evo_rmlui_settings_params_t set;
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
        evo_rmlui_render_settings(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_settings_playback.bmp", fb.data(), width, height);
    }

    // 3. Subtitles Subsection
    {
        std::fill(fb.begin(), fb.end(), 0xFF06090E);
        evo_rmlui_settings_params_t set;
        memset(&set, 0, sizeof(set));
        set.title = "SUBTITLES";
        set.subtitle = "SETTINGS  -  AUTO-DETECT & DEFAULT SIZING";
        set.counter = "1 OF 2";
        set.rail_active_idx = 5;
        set.rail_focused = 0;
        set.row_count = 2;

        set.rows[0].title = "AUTO-DETECT SUBTITLES";
        set.rows[0].detail = "LOAD EXTERNAL .SRT AND EMBEDDED STREAMS";
        set.rows[0].icon_path = "projects/evoplayer/assets/icons/icon_subtitles.png";
        set.rows[0].badge = "ON";
        set.rows[0].has_chevron = 1;
        set.rows[0].is_focused = 1;

        set.rows[1].title = "DEFAULT SUBTITLE SIZE";
        set.rows[1].detail = "MEDIUM (RECOMMENDED FOR 4K TVS)";
        set.rows[1].icon_path = "projects/evoplayer/assets/icons/icon_aspect.png";
        set.rows[1].badge = "MEDIUM";
        set.rows[1].has_chevron = 1;
        set.rows[1].is_focused = 0;

        evo_rmlui_update_settings(&set);
        evo_rmlui_render_settings(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_settings_subtitles.bmp", fb.data(), width, height);
    }

    // 4. Interface & Controls Subsection
    {
        std::fill(fb.begin(), fb.end(), 0xFF06090E);
        evo_rmlui_settings_params_t set;
        memset(&set, 0, sizeof(set));
        set.title = "INTERFACE & CONTROLS";
        set.subtitle = "SETTINGS  -  THEMES, SOUNDS, LIGHTBAR & SORTING";
        set.counter = "1 OF 5";
        set.rail_active_idx = 5;
        set.rail_focused = 0;
        set.row_count = 5;

        set.rows[0].title = "THEME";
        set.rows[0].detail = "MIDNIGHT OBSIDIAN";
        set.rows[0].icon_path = "projects/evoplayer/assets/icons/icon_palette.png";
        set.rows[0].badge = "ACTIVE";
        set.rows[0].has_chevron = 1;
        set.rows[0].is_focused = 1;

        set.rows[1].title = "NAVIGATION SOUNDS";
        set.rows[1].detail = "PLAY AUDIO CLICKS ON INPUT";
        set.rows[1].icon_path = "projects/evoplayer/assets/icons/icon_resume.png";
        set.rows[1].badge = "ON";
        set.rows[1].has_chevron = 1;
        set.rows[1].is_focused = 0;

        set.rows[2].title = "CONTROLLER LIGHTBAR";
        set.rows[2].detail = "SYNC DUALSENSE ACCENT WITH THEME";
        set.rows[2].icon_path = "projects/evoplayer/assets/icons/icon_palette.png";
        set.rows[2].badge = "THEME";
        set.rows[2].has_chevron = 1;
        set.rows[2].is_focused = 0;

        set.rows[3].title = "FOLDER SORTING";
        set.rows[3].detail = "NAME ASCENDING (A-Z)";
        set.rows[3].icon_path = "projects/evoplayer/assets/icons/icon_browse_usb.png";
        set.rows[3].badge = "A-Z";
        set.rows[3].has_chevron = 1;
        set.rows[3].is_focused = 0;

        set.rows[4].title = "ON-SCREEN KEYBOARD";
        set.rows[4].detail = "SYSTEM OS KEYBOARD FOR SEARCH";
        set.rows[4].icon_path = "projects/evoplayer/assets/icons/icon_developer_tools.png";
        set.rows[4].badge = "ENABLED";
        set.rows[4].has_chevron = 1;
        set.rows[4].is_focused = 0;

        evo_rmlui_update_settings(&set);
        evo_rmlui_render_settings(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_settings_interface.bmp", fb.data(), width, height);
    }

    // 5. System & Diagnostics Subsection
    {
        std::fill(fb.begin(), fb.end(), 0xFF06090E);
        evo_rmlui_settings_params_t set;
        memset(&set, 0, sizeof(set));
        set.title = "SYSTEM & DIAGNOSTICS";
        set.subtitle = "SETTINGS  -  DEVELOPER TOOLS & MEDIA TILE";
        set.counter = "1 OF 3";
        set.rail_active_idx = 5;
        set.rail_focused = 0;
        set.row_count = 3;

        set.rows[0].title = "DEVELOPER TOOLS";
        set.rows[0].detail = "COMPATIBILITY REPORT, DEBUG HUD & HARDWARE TESTS";
        set.rows[0].icon_path = "projects/evoplayer/assets/icons/icon_developer_tools.png";
        set.rows[0].badge = "OPEN";
        set.rows[0].has_chevron = 1;
        set.rows[0].is_focused = 1;

        set.rows[1].title = "RESET APPLICATION PREFERENCES";
        set.rows[1].detail = "RESTORE DEFAULT SETTINGS";
        set.rows[1].icon_path = "projects/evoplayer/assets/icons/icon_settings.png";
        set.rows[1].badge = "RESET";
        set.rows[1].has_chevron = 1;
        set.rows[1].is_focused = 0;

        set.rows[2].title = "MEDIA HOME TILE";
        set.rows[2].detail = "REMOVE QUICK LAUNCH TILE FROM PS5 HOME SCREEN";
        set.rows[2].icon_path = "projects/evoplayer/assets/icons/icon_home.png";
        set.rows[2].badge = "INSTALLED";
        set.rows[2].has_chevron = 1;
        set.rows[2].is_focused = 0;

        evo_rmlui_update_settings(&set);
        evo_rmlui_render_settings(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_settings_system.bmp", fb.data(), width, height);
    }

    // 6. Playback Profile Picker
    {
        std::fill(fb.begin(), fb.end(), 0xFF06090E);
        evo_rmlui_settings_params_t set;
        memset(&set, 0, sizeof(set));
        set.title = "PLAYBACK PROFILE";
        set.subtitle = "HOW AGGRESSIVELY THE DECODER IS TUNED";
        set.counter = "2 OF 4";
        set.rail_active_idx = 5;
        set.rail_focused = 0;
        set.row_count = 4;

        set.rows[0].title = "BALANCED";
        set.rows[0].detail = "EVEN TRADE BETWEEN SMOOTHNESS AND FORMAT COVERAGE";
        set.rows[0].icon_path = "projects/evoplayer/assets/icons/icon_settings.png";
        set.rows[0].badge = "";
        set.rows[0].has_chevron = 0;
        set.rows[0].is_focused = 0;

        set.rows[1].title = "PERFORMANCE";
        set.rows[1].detail = "FEWEST DROPPED FRAMES - BEST FOR HIGH BITRATE VIDEO";
        set.rows[1].icon_path = "projects/evoplayer/assets/icons/icon_settings.png";
        set.rows[1].badge = "ACTIVE";
        set.rows[1].has_chevron = 0;
        set.rows[1].is_focused = 1;

        set.rows[2].title = "COMPATIBILITY";
        set.rows[2].detail = "WIDEST FORMAT SUPPORT - TRY THIS WHEN A FILE WILL NOT PLAY";
        set.rows[2].icon_path = "projects/evoplayer/assets/icons/icon_settings.png";
        set.rows[2].badge = "";
        set.rows[2].has_chevron = 0;
        set.rows[2].is_focused = 0;

        set.rows[3].title = "DEBUG";
        set.rows[3].detail = "VERBOSE DIAGNOSTICS AND ON-SCREEN COUNTERS";
        set.rows[3].icon_path = "projects/evoplayer/assets/icons/icon_settings.png";
        set.rows[3].badge = "";
        set.rows[3].has_chevron = 0;
        set.rows[3].is_focused = 0;

        evo_rmlui_update_settings(&set);
        evo_rmlui_render_settings(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_settings_profile.bmp", fb.data(), width, height);
    }

    // 7. Developer Tools Screen
    {
        std::fill(fb.begin(), fb.end(), 0xFF06090E);
        evo_rmlui_settings_params_t set;
        memset(&set, 0, sizeof(set));
        set.title = "DEVELOPER TOOLS";
        set.subtitle = "DIAGNOSTICS & SYSTEM REPORTS";
        set.counter = "1 OF 4";
        set.rail_active_idx = 5;
        set.rail_focused = 0;
        set.row_count = 4;

        set.rows[0].title = "COMPATIBILITY REPORT";
        set.rows[0].detail = "WRITES A CODEC REPORT TO USB0";
        set.rows[0].icon_path = "projects/evoplayer/assets/icons/icon_developer_tools.png";
        set.rows[0].badge = "RUN";
        set.rows[0].has_chevron = 1;
        set.rows[0].is_focused = 1;

        set.rows[1].title = "DEBUG OVERLAY";
        set.rows[1].detail = "ON-SCREEN REALTIME PERFORMANCE STATS";
        set.rows[1].icon_path = "projects/evoplayer/assets/icons/icon_settings.png";
        set.rows[1].badge = "OFF";
        set.rows[1].has_chevron = 0;
        set.rows[1].is_focused = 0;

        set.rows[2].title = "NAVIGATION SOUNDS";
        set.rows[2].detail = "PLAY AUDIO CLICKS ON CONTROLLER INPUT";
        set.rows[2].icon_path = "projects/evoplayer/assets/icons/icon_subtitles.png";
        set.rows[2].badge = "ON";
        set.rows[2].has_chevron = 0;
        set.rows[2].is_focused = 0;

        set.rows[3].title = "LIGHTBAR ACCENTS";
        set.rows[3].detail = "SYNC DUALSENSE LIGHTBAR WITH THEME";
        set.rows[3].icon_path = "projects/evoplayer/assets/icons/icon_palette.png";
        set.rows[3].badge = "THEME";
        set.rows[3].has_chevron = 0;
        set.rows[3].is_focused = 0;

        evo_rmlui_update_settings(&set);
        evo_rmlui_render_settings(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_settings_devtools.bmp", fb.data(), width, height);
    }

    // 8. About & Support Screen
    {
        std::fill(fb.begin(), fb.end(), 0xFF06090E);
        evo_rmlui_settings_params_t set;
        memset(&set, 0, sizeof(set));
        set.title = "ABOUT & SUPPORT";
        set.subtitle = "CREDITS, ENGINE & PROJECT INFO";
        set.counter = "1 OF 6";
        set.rail_active_idx = 6;
        set.rail_focused = 0;
        set.row_count = 6;

        set.rows[0].title = "VERSION";
        set.rows[0].detail = "v0.7.0 HOMEBREW EDITION";
        set.rows[0].icon_path = "projects/evoplayer/assets/icons/icon_about_support.png";
        set.rows[0].badge = "v0.7.0";
        set.rows[0].has_chevron = 0;
        set.rows[0].is_focused = 1;

        set.rows[1].title = "EVO PLAYER PRO";
        set.rows[1].detail = "CINEMATIC MEDIA PLAYER FOR PS5 HOMEBREW";
        set.rows[1].icon_path = "projects/evoplayer/assets/icons/icon_resume.png";
        set.rows[1].badge = "";
        set.rows[1].has_chevron = 0;
        set.rows[1].is_focused = 0;

        set.rows[2].title = "BUILT ON";
        set.rows[2].detail = "FFMPEG, RMLUI & PS5 PAYLOAD SDK";
        set.rows[2].icon_path = "projects/evoplayer/assets/icons/icon_developer_tools.png";
        set.rows[2].badge = "";
        set.rows[2].has_chevron = 0;
        set.rows[2].is_focused = 0;

        set.rows[3].title = "THEMES";
        set.rows[3].detail = "8 AVAILABLE - DROP .THEME FILES ON USB0";
        set.rows[3].icon_path = "projects/evoplayer/assets/icons/icon_palette.png";
        set.rows[3].badge = "";
        set.rows[3].has_chevron = 0;
        set.rows[3].is_focused = 0;

        set.rows[4].title = "SCREENSHOTS";
        set.rows[4].detail = "PRESS L3 OR R3 IN ANY MENU";
        set.rows[4].icon_path = "projects/evoplayer/assets/icons/icon_aspect.png";
        set.rows[4].badge = "";
        set.rows[4].has_chevron = 0;
        set.rows[4].is_focused = 0;

        set.rows[5].title = "CHANGELOG";
        set.rows[5].detail = "WHAT CHANGED IN EACH RELEASE";
        set.rows[5].icon_path = "projects/evoplayer/assets/icons/icon_recent_files.png";
        set.rows[5].badge = "VIEW";
        set.rows[5].has_chevron = 1;
        set.rows[5].is_focused = 0;

        evo_rmlui_update_settings(&set);
        evo_rmlui_render_settings(fb.data(), width, height);
        save_bmp_24("output/uiview/rml_settings_about.bmp", fb.data(), width, height);
    }

    std::cout << "Rendered all settings screens successfully" << std::endl;
    return 0;
}
