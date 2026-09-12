#ifndef EVO_IMAGE_VIEWER_SCREEN_HPP
#define EVO_IMAGE_VIEWER_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include <string>

namespace evo {

class ImageViewerScreen : public StatefulScreen {
public:
    ImageViewerScreen();
    ~ImageViewerScreen() override;

    ScreenId getScreenId() const override { return ScreenId::ImageViewer; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

    void openImage(const std::string& path);

private:
    void freeImage();

    std::string m_imagePath;
    std::string m_imageTitle;
    uint32_t* m_pixels = nullptr;
    int m_width = 0;
    int m_height = 0;
    bool m_loaded = false;
};

} // namespace evo

#endif // EVO_IMAGE_VIEWER_SCREEN_HPP
