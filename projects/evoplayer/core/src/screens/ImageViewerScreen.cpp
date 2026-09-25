#include "evo/screens/ImageViewerScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_toast.h"
#include "evo_boot_log.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../../../stb_image.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace evo {

ImageViewerScreen::ImageViewerScreen()
    : StatefulScreen("ImageViewerScreen") {
}

ImageViewerScreen::~ImageViewerScreen() {
    freeImage();
}

void ImageViewerScreen::freeImage() {
    if (m_pixels) {
        std::free(m_pixels);
        m_pixels = nullptr;
    }
    m_width = 0;
    m_height = 0;
    m_loaded = false;
}

void ImageViewerScreen::openImage(const std::string& path) {
    freeImage();
    m_imagePath = path;

    size_t slash = path.find_last_of('/');
    m_imageTitle = (slash != std::string::npos) ? path.substr(slash + 1) : path;

    /*
     * Load at the file's own channel count, and thin large images down while
     * converting.
     *
     * Asking stb_image for 4 channels makes it decode into a 3-channel buffer
     * and then convert into a second, larger one; adding our own uint32 buffer
     * on top meant a 4K screenshot needed roughly 90 MB at once. Free flexible
     * memory runs 144-192 MB with a file open (448 MB total - see
     * docs/hardware/memory-budget.md), so EVO's own 3840x2160 captures -
     * the ones L3 writes -
     * failed to open in EVO's own viewer, reporting "unsupported or invalid
     * file" when the file was perfectly valid.
     *
     * Native channels avoids the conversion buffer, and since the viewer only
     * ever presents at panel resolution, anything past 1080p is sampled down
     * rather than held at full size.
     */
    int w = 0, h = 0, ch = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 0);
    if (!data || w <= 0 || h <= 0 || ch < 3) {
        const char* why = stbi_failure_reason();
        evo_boot_log("ImageViewer: %s failed (%dx%d ch=%d): %s",
                     m_imageTitle.c_str(), w, h, ch, why ? why : "unknown");
        if (data) stbi_image_free(data);
        return;
    }

    int step = 1;
    while ((w / step) > 1920 || (h / step) > 1080)
        ++step;

    const int outW = w / step;
    const int outH = h / step;
    const size_t pixelCount = static_cast<size_t>(outW) * static_cast<size_t>(outH);
    m_pixels = static_cast<uint32_t*>(std::malloc(pixelCount * sizeof(uint32_t)));
    if (!m_pixels) {
        evo_boot_log("ImageViewer: %s decoded %dx%d but %zu KB for pixels failed",
                     m_imageTitle.c_str(), w, h, (pixelCount * 4) / 1024);
        stbi_image_free(data);
        return;
    }

    m_width = outW;
    m_height = outH;
    for (int y = 0; y < outH; ++y) {
        const unsigned char* row = data + static_cast<size_t>(y) * step
                                        * static_cast<size_t>(w) * ch;
        uint32_t* dst = m_pixels + static_cast<size_t>(y) * outW;
        for (int x = 0; x < outW; ++x) {
            const unsigned char* px = row + static_cast<size_t>(x) * step * ch;
            const uint8_t a = (ch == 4) ? px[3] : 255;
            // RGBA in memory: byte 0=R, 1=G, 2=B, 3=A
            dst[x] = (static_cast<uint32_t>(a) << 24) |
                     (static_cast<uint32_t>(px[2]) << 16) |
                     (static_cast<uint32_t>(px[1]) << 8) |
                     static_cast<uint32_t>(px[0]);
        }
    }
    m_loaded = true;
    if (step > 1)
        evo_boot_log("ImageViewer: %s %dx%d shown at %dx%d (1/%d)",
                     m_imageTitle.c_str(), w, h, outW, outH, step);
    stbi_image_free(data);
}

void ImageViewerScreen::onEnter() {
    StatefulScreen::onEnter();
    if (!m_imagePath.empty() && !m_loaded) {
        openImage(m_imagePath);
    }
}

void ImageViewerScreen::onExit() {
    StatefulScreen::onExit();
    freeImage();
}

bool ImageViewerScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    if (pressed & (PadButtons::Circle | PadButtons::Cross)) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto sm = Application::getInstance().getScreenManager()) {
            if (!sm->navigateBack()) {
                sm->navigateTo(ScreenId::UsbBrowser);
            }
        }
        return true;
    }

    return false;
}

void ImageViewerScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
}

void ImageViewerScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_image_params_t ip;
    std::memset(&ip, 0, sizeof(ip));

    ip.title = m_imageTitle.c_str();
    ip.pixels = m_pixels;
    ip.w = m_width;
    ip.h = m_height;
    ip.loaded = m_loaded ? 1 : 0;

    evo_rmlui_update_image(&ip);
    evo_rmlui_render_image(framebuffer, width, height);
}

} // namespace evo
