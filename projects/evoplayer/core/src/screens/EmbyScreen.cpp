#include "evo/screens/EmbyScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_toast.h"
#include "addon_emby.h"

#include <cstdio>
#include <cstring>

namespace evo {

EmbyScreen::EmbyScreen()
    : StatefulScreen("EmbyScreen") {
}

void EmbyScreen::onEnter() {
    StatefulScreen::onEnter();
}

void EmbyScreen::onExit() {
    StatefulScreen::onExit();
}

bool EmbyScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    if (pressed & PadButtons::Left) {
        if (auto sm = Application::getInstance().getScreenManager()) {
            sm->setRailFocused(true);
            return true;
        }
    }

    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto sm = Application::getInstance().getScreenManager()) {
            sm->navigateTo(ScreenId::MainMenu);
        }
        return true;
    }

    return false;
}

void EmbyScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
}

void EmbyScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_list_params_t params;
    std::memset(&params, 0, sizeof(params));

    bool railFocused = false;
    if (auto sm = Application::getInstance().getScreenManager()) {
        railFocused = sm->isRailFocused();
    }

    params.title = "EMBY MEDIA SERVER";
    params.subtitle = "REMOTE STREAMING";
    params.total_count = 1;
    params.cursor_index = 0;
    params.row_count = 1;
    params.rail_focused = railFocused ? 1 : 0;

    params.rows[0].title = "Configure Emby Connection";
    params.rows[0].detail = "Set up your server host, port, and credentials in Settings";
    params.rows[0].badge = "EMBY";
    params.rows[0].icon_path = "../icons/icon_emby.png";
    params.rows[0].is_focused = !railFocused;
    params.rows[0].progress = -1;

    evo_rmlui_update_list(&params);
    evo_rmlui_render_list(framebuffer, width, height);
}

} // namespace evo
