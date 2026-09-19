#ifndef EVO_ABOUT_SUPPORT_SCREEN_HPP
#define EVO_ABOUT_SUPPORT_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"

namespace evo {

class AboutSupportScreen : public StatefulScreen {
public:
    AboutSupportScreen();
    ~AboutSupportScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::AboutSupport; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;
};

} // namespace evo

#endif // EVO_ABOUT_SUPPORT_SCREEN_HPP
