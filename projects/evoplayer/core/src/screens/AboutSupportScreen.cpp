#include "evo/screens/AboutSupportScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"

#include <cstdio>
#include <cstring>

namespace evo {

AboutSupportScreen::AboutSupportScreen()
    : StatefulScreen("AboutSupportScreen") {
}

void AboutSupportScreen::onEnter() {
    StatefulScreen::onEnter();
}

void AboutSupportScreen::onExit() {
    StatefulScreen::onExit();
}

bool AboutSupportScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    if (pressed & PadButtons::Left) {
        if (auto sm = Application::getInstance().getScreenManager()) {
            sm->setRailFocused(true);
            return true;
        }
    }

    if (pressed & PadButtons::Cross) {
        evo_feedback(EVO_FB_CONFIRM);
        if (auto sm = Application::getInstance().getScreenManager()) {
            sm->navigateTo(ScreenId::Changelog);
        }
        return true;
    }

    if (pressed & PadButtons::Square) {
        evo_feedback(EVO_FB_OPEN);
        if (auto sm = Application::getInstance().getScreenManager()) {
            sm->navigateTo(ScreenId::DeveloperTools);
        }
        return true;
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

void AboutSupportScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
}

void AboutSupportScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_about_params_t params;
    std::memset(&params, 0, sizeof(params));

    bool railFocused = false;
    if (auto sm = Application::getInstance().getScreenManager()) {
        railFocused = sm->isRailFocused();
    }

    params.app_name = "EVO PLAYER PRO";
    params.version = "v" EVO_PLAYER_VERSION;
    params.build_tag = "PS5 HOMEBREW (AGC GPU)";
    params.tagline = "CINEMATIC MEDIA PLAYER FOR PLAYSTATION 5 HOMEBREW";
    params.themes_info = "PRESS CROSS FOR CHANGELOG, SQUARE FOR DEV TOOLS";
    params.action_focused = !railFocused;

    evo_rmlui_update_about(&params);
    evo_rmlui_render_about(framebuffer, width, height);
}

} // namespace evo
