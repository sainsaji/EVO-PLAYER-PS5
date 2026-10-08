#include "evo_rmlui_bridge.h"
#include <vector>
#include <cstdint>

static void fill_reader_fixture(evo_rmlui_reader_params_t& p)
{
    p.title = "README.md";
    p.subtitle = "evoplayer / docs";
    p.badge = "TEXT";
    p.rail_focused = 0;
    
    static const char* const kLines[] = {
        "# EVO Player",
        "",
        "Welcome to EVO Player, the ultimate media player for your homebrew console.",
        "",
        "## Features",
        "- Hardware decoding for maximum performance",
        "- 4K HDR support where available",
        "- Native user interface built on ps5-homebrew-ui",
        "- Subtitle rendering",
        "- FTP server integration",
        "",
        "## Controls",
        "Use the directional pad to navigate menus. Press Cross to select,",
        "and Circle to go back.",
        "",
        "During playback:",
        "- L2 / R2: Rewind / Fast Forward",
        "- L1 / R1: Previous / Next Chapter",
        "- Square: Audio and Subtitle options",
        "- Triangle: Media Info",
        "",
        "## Installation",
        "Place your media files in the USB drive or internal storage.",
        "The app will automatically discover them.",
        "",
        "## Known Issues",
        "- Some esoteric audio codecs might require software fallback.",
        "- Network streaming performance depends on your Wi-Fi.",
        "",
        "Enjoy your media!"
    };
    
    p.line_count = 30;
    for (int i = 0; i < 30; ++i) {
        p.lines[i] = kLines[i];
    }
    
    p.face = 2;
    p.progress = 0.3;
    p.visible_frac = 0.4;
    p.notice = "";
    p.footnote = "FIRST 10KB OF 45KB";
}

static void fill_image_fixture(evo_rmlui_image_params_t& p, std::vector<uint32_t>& storage)
{
    p.title = "sample_image.jpg";
    p.w = 640;
    p.h = 360;
    p.loaded = 1;
    
    storage.resize(640 * 360);
    for (int y = 0; y < 360; ++y) {
        for (int x = 0; x < 640; ++x) {
            uint8_t r = (x * 255) / 640;
            uint8_t g = (y * 255) / 360;
            uint8_t b = 128;
            uint8_t a = 255;
            storage[y * 640 + x] = (a << 24) | (b << 16) | (g << 8) | r;
        }
    }
    
    p.pixels = storage.data();
}

static void fill_keyboard_fixture(evo_keyboard_params_t& p)
{
    p.visible = 1;
    p.native_only = 0;
    p.title = "SEARCH";
    p.text = "blade";
    p.mode_label = "LOWERCASE";
    
    p.rows[0] = "1234567890";
    p.rows[1] = "qwertyuiop";
    p.rows[2] = "asdfghjkl-";
    p.rows[3] = "zxcvbnm,._";
    
    p.action_labels[0] = "SHIFT";
    p.action_labels[1] = "SPACE";
    p.action_labels[2] = "DELETE";
    p.action_labels[3] = "CANCEL";
    p.action_labels[4] = "DONE";
    p.action_labels[5] = "";
    
    p.focus_row = 1;
    p.focus_col = 3;
    p.show_caret = 1;
    p.len = 5;
    p.max_len = 64;
}
