#include "evo/screens/SafeToCloseScreen.hpp"
#include "evo_rmlui_bridge.h"

namespace evo {

SafeToCloseScreen::SafeToCloseScreen()
    : StatefulScreen("SafeToCloseScreen") {
}

bool SafeToCloseScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)pressed;
    (void)held;
    (void)released;
    return true;   /* swallow everything - playback and audio are already gone */
}

void SafeToCloseScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_render_closed(framebuffer, width, height);
}

} // namespace evo
