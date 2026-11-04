/*
 * The Web section (#237): Prospero Browser's UI right of EVO's rail. The
 * browser itself (tabs, address bar, speed dial, bookmarks, history) lives in
 * projects/evoplayer/browser/ and is drawn by ui_kit/src/evo_hui_web.cpp; this
 * screen passes the pad in. docs/ui/browser.md.
 */
#include "evo/screens/WebScreens.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_hui.h"
#include "evo_boot_log.h"
#include "evo_keyboard.h"
#include "evo_browser_engine.h"

namespace evo {

namespace {

constexpr long long kFieldClickMs = 1500;   /* a field focused this soon after a click opens the keyboard */

bool rail_focused()
{
    auto sm = Application::getInstance().getScreenManager();
    return sm && sm->isRailFocused();
}

}  // namespace

WebScreen::WebScreen() : StatefulScreen("WebScreen") {}

void WebScreen::onEnter()
{
    StatefulScreen::onEnter();
    evo_browser_open();   /* the engine starts in the background */
#if defined(EVO_AGC_DEVICE)
    evo_hui_web_set_visible(1);
#endif
    m_crossDown = false;
}

void WebScreen::onExit()
{
#if defined(EVO_AGC_DEVICE)
    if (m_crossDown) evo_hui_web_input(EVO_HUI_WEB_CROSS_UP);
    evo_hui_web_set_visible(0);   /* no painting while EVO shows something else */
#endif
    m_crossDown = false;
    StatefulScreen::onExit();
}

void WebScreen::onFieldSubmitted(const char* text, void* self)
{
    (void)self;
#if defined(EVO_AGC_DEVICE)
    if (text && text[0]) evo_hui_web_insert_text(text);
#else
    (void)text;
#endif
}

void WebScreen::update(double deltaMs)
{
    StatefulScreen::update(deltaMs);
    if (!isScreenActive()) return;
#if defined(EVO_AGC_DEVICE)
    /* Only a field the user just clicked: pages that focus a search box on
     * their own must not throw the keyboard up. */
    if (evo_hui_web_take_text_focus() && evo_log_mono_ms() - m_lastClickMs < kFieldClickMs &&
        !evo_keyboard_is_open())
        evo_keyboard_open("Type", "", 1024, &WebScreen::onFieldSubmitted, this);
    if (evo_keyboard_is_open() || rail_focused()) return;   /* they own the pad */
    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
    Application::getInstance().getLeftStick(&lx, &ly);
    Application::getInstance().getRightStick(&rx, &ry);
    const uint32_t raw = Application::getInstance().getPadButtons();
    evo_hui_web_sticks(lx, ly, rx, ry, (raw & PadButtons::L2) ? 1.0f : 0.0f,
                       (raw & PadButtons::R2) ? 1.0f : 0.0f, static_cast<float>(deltaMs) / 1000.0f);
#endif
}

bool WebScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released)
{
    (void)held;
#if defined(EVO_AGC_DEVICE)
    /* The left stick also arrives as D-pad presses; it drives the browser's
     * pointer through evo_hui_web_sticks(), so only real D-pad presses count. */
    const uint32_t raw = Application::getInstance().getPadButtons();
    if (pressed & raw & PadButtons::Left) {
        if (!evo_hui_web_input(EVO_HUI_WEB_LEFT)) {
            if (auto sm = Application::getInstance().getScreenManager()) sm->setRailFocused(true);
        }
        return true;
    }
    if (pressed & raw & PadButtons::Right) evo_hui_web_input(EVO_HUI_WEB_RIGHT);
    if (pressed & raw & PadButtons::Up) evo_hui_web_input(EVO_HUI_WEB_UP);
    if (pressed & raw & PadButtons::Down) evo_hui_web_input(EVO_HUI_WEB_DOWN);
    if (pressed & PadButtons::Cross) {
        m_crossDown = true;
        m_lastClickMs = evo_log_mono_ms();
        evo_hui_web_input(EVO_HUI_WEB_CROSS_DOWN);
    }
    if ((released & PadButtons::Cross) && m_crossDown) {
        m_crossDown = false;
        evo_hui_web_input(EVO_HUI_WEB_CROSS_UP);
    }
    if (pressed & PadButtons::Circle) evo_hui_web_input(EVO_HUI_WEB_CIRCLE);
    if (pressed & PadButtons::Triangle) evo_hui_web_input(EVO_HUI_WEB_TRIANGLE);
    if (pressed & PadButtons::Square) evo_hui_web_input(EVO_HUI_WEB_SQUARE);
    if (pressed & PadButtons::Options) evo_hui_web_input(EVO_HUI_WEB_OPTIONS);
    if (pressed & PadButtons::L1) evo_hui_web_input(EVO_HUI_WEB_L1);
    if (pressed & PadButtons::R1) evo_hui_web_input(EVO_HUI_WEB_R1);
    if (pressed & PadButtons::TouchPad) evo_hui_web_input(EVO_HUI_WEB_TOUCHPAD);
#else
    (void)pressed;
    (void)released;
#endif
    return true;
}

void WebScreen::render(uint32_t* framebuffer, int width, int height)
{
    evo_rmlui_render_webpage(framebuffer, width, height);
}

} // namespace evo
