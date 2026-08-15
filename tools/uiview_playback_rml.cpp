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
    std::vector<uint32_t> fb(width * height);

    // Simulate cinematic film frame background
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint8_t r = 18 + (y * 22 / height);
            uint8_t g = 24 + (x * 18 / width);
            uint8_t b = 40 + (y * 35 / height);
            fb[y * width + x] = 0xFF000000 | (b << 16) | (g << 8) | r;
        }
    }

    if (!evo_rmlui_init(width, height)) {
        std::cerr << "Failed to initialize RmlUi playback engine!" << std::endl;
        return 1;
    }

    // Test 1: Playing (Not paused, not scrubbing)
    evo_playback_osd_params_t p;
    memset(&p, 0, sizeof(p));
    p.title = "Big Buck Bunny (2008)";
    p.metadata = "1080p  H.264  6CH";
    p.res_badge = "1080p FHD";
    p.hdr_badge = ""; // SDR video
    p.codec_badge = "AVC / H.264";
    p.fps_badge = "60 FPS";
    p.audio_badge = "5.1 Surround";
    p.position_sec = 142.0; // 02:22
    p.duration_sec = 596.0; // 09:56
    p.percentage = 142.0 / 596.0;
    p.paused = 0; // Playing!
    p.scrub_active = 0; // Not scrubbing!
    p.scrub_target = 0.0;
    p.audio_track = "English AC3 5.1";
    p.sub_track = "None";
    p.view_mode = 0; // FIT
    p.show_stats = 0; // Stats off
    p.alpha = 255;

    evo_rmlui_update_playback_params(&p);
    evo_rmlui_render_playback_osd(fb.data(), width, height);

    save_bmp_24("output/uiview/rml_playback.bmp", fb.data(), width, height);
    std::cout << "Rendered output/uiview/rml_playback.bmp successfully" << std::endl;
    return 0;
}
