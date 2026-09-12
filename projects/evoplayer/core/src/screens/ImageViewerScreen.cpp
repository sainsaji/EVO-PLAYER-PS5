#include "evo/screens/ImageViewerScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_toast.h"

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

    int w = 0, h = 0, ch = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (data && w > 0 && h > 0) {
        m_width = w;
        m_height = h;
        size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
        m_pixels = static_cast<uint32_t*>(std::malloc(pixelCount * sizeof(uint32_t)));
        if (m_pixels) {
            for (size_t i = 0; i < pixelCount; ++i) {
                uint8_t r = data[i * 4 + 0];
                uint8_t g = data[i * 4 + 1];
                uint8_t b = data[i * 4 + 2];
                uint8_t a = data[i * 4 + 3];
                // RGBA in memory: byte 0=R, 1=G, 2=B, 3=A
                m_pixels[i] = (static_cast<uint32_t>(a) << 24) |
                              (static_cast<uint32_t>(b) << 16) |
                              (static_cast<uint32_t>(g) << 8)  |
                              static_cast<uint32_t>(r);
            }
            m_loaded = true;
        }
        stbi_image_free(data);
    }
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
