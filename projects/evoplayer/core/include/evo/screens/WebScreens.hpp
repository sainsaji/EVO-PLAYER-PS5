#ifndef EVO_WEB_SCREENS_HPP
#define EVO_WEB_SCREENS_HPP

/*
 * The Web section (#237, docs/ui/browser.md): Prospero Browser's app and kit
 * UI (projects/evoplayer/browser/, drawn by ui_kit/src/evo_hui_web.cpp) on the
 * out-of-process browser engine (include/evo_browser_engine.h). This screen
 * only passes the pad in and opens EVO's keyboard for the page's text fields;
 * the browser keeps its own address bar, speed dial, bookmarks and history.
 */

#include "evo/screens/StatefulScreen.hpp"

namespace evo {

class WebScreen : public StatefulScreen {
public:
    WebScreen();

    ScreenId getScreenId() const override { return ScreenId::WebHome; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    static void onFieldSubmitted(const char* text, void* self);

    bool m_crossDown = false;
    long long m_lastClickMs = -100000;
};

} // namespace evo

#endif
