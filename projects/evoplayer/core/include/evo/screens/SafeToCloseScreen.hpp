#ifndef EVO_SAFE_TO_CLOSE_SCREEN_HPP
#define EVO_SAFE_TO_CLOSE_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"

namespace evo {

/*
 * The last thing EVO ever draws. Application::requestSoftClose() navigates
 * here, keeps presenting for a couple of seconds so the scanout latches it,
 * then parks the frame loop - so this is the picture that stays on the panel
 * while the user closes EVO from the switcher. Accepts no input: by then
 * there is nothing left running to act on it.
 */
class SafeToCloseScreen : public StatefulScreen {
public:
    SafeToCloseScreen();
    ~SafeToCloseScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::SafeToClose; }
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void render(uint32_t* framebuffer, int width, int height) override;
};

} // namespace evo

#endif // EVO_SAFE_TO_CLOSE_SCREEN_HPP
