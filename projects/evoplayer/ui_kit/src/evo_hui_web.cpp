/*
 * evo_hui_web.cpp - the Web section (#237): Prospero Browser's app and kit UI
 * (copied in browser/), on EVO's browser engine, right of EVO's rail.
 *
 * Prospero's BrowserApp holds the browser state (tabs, address bar, speed
 * dial, bookmarks, history, overlays) and BrowserUi draws it; both are used
 * unchanged. EVO lays them out for the screen minus the rail and shifts the
 * drawing right by the rail's width. Pad input comes from the core screen
 * (WebScreens.cpp) through evo_hui_web_input() / evo_hui_web_sticks().
 */
#include "evo_hui.h"
#include "evo_hui_screens.hpp"

#include "../../browser/browser/browser_app.hpp"
#include "../../browser/evo_page_engine.hpp"
#include "../../browser/ui/browser_ui.hpp"

#include <memory>
#include <string>

namespace
{

constexpr float kRailW = static_cast<float>(EVO_HUI_WEBPAGE_X);
constexpr uint32_t kWidth = 1920u - EVO_HUI_WEBPAGE_X;
constexpr uint32_t kHeight = 1080u;

std::unique_ptr<prospero::browser::BrowserApp> g_app;
prospero::ui::BrowserUi *g_ui = nullptr;

/* A dev-remote pointer move waiting for the next draw (screen pixels). */
bool g_pointer_pending = false;
float g_pointer_x = 0.0f, g_pointer_y = 0.0f;

prospero::browser::BrowserApp *app()
{
    if (!g_app)
    {
        evo::web::EvoPageEngine::install();
        g_app = std::make_unique<prospero::browser::BrowserApp>();
        g_app->initialize(kWidth, kHeight);
        g_ui = new prospero::ui::BrowserUi();
    }
    return g_app.get();
}

const char *mode_name(prospero::browser::UiMode m)
{
    using prospero::browser::UiMode;
    switch (m)
    {
    case UiMode::Browsing: return "page";
    case UiMode::OmniboxEditing: return "address";
    case UiMode::BookmarksDrawer: return "bookmarks";
    case UiMode::HistoryDrawer: return "history";
    case UiMode::SettingsModal: return "settings";
    case UiMode::TabOverview: return "tabs";
    case UiMode::Downloads: return "downloads";
    case UiMode::QuickMenu: return "menu";
    case UiMode::PageDialog: return "dialog";
    }
    return "?";
}

}  // namespace

namespace evo::kit
{

void web_browser_draw(hui::gfx::DrawList &list, const Context &ctx, float dt)
{
    auto *a = app();
    if (g_pointer_pending)
    {
        /* BrowserApp's space starts right of the rail. update() then sends the
         * page the motion, as it does every frame for a stick-moved pointer. */
        g_pointer_pending = false;
        a->set_using_dpad(false);
        a->cursor().set_position(g_pointer_x - kRailW, g_pointer_y);
    }
    a->update(dt);
    list.push_transform(1.0f, 0.0f, 0.0f, kRailW, 0.0f);
    g_ui->render(list, ctx.fonts, *a, dt);
    list.pop_transform();
}

/* For the dev remote's `ui`: the overlay or header item that has the cursor. */
bool web_browser_focus(FocusInfo *f)
{
    if (!g_app || !f)
        return false;
    const auto *a = g_app.get();
    f->id = std::string("web-") + mode_name(a->ui_mode());
    if (a->ui_mode() == prospero::browser::UiMode::Browsing)
    {
        if (a->focus_zone() == prospero::browser::FocusZone::TopBar)
            f->id += "-top-" + std::to_string(a->top_bar_index());
        else
            f->id += a->using_dpad() ? "-content" : "-pointer";
    }
    const auto *tab = a->tabs().active_tab_info();
    f->text = tab ? tab->url : std::string();
    f->index = 1;
    f->total = 1;
    return true;
}

}  // namespace evo::kit

int evo_hui_web_input(int what)
{
    auto *a = app();
    using prospero::browser::FocusZone;
    using prospero::browser::UiMode;
    switch (what)
    {
    case EVO_HUI_WEB_LEFT:
        /* D-pad Left opens EVO's rail, as on every EVO screen: from the
         * leftmost header button, from the speed dial's first column, and
         * always on a web page (Prospero would send the page an arrow key,
         * and the D-pad would never leave it; sideways scrolling is on the
         * right stick). */
        if (a->ui_mode() == UiMode::Browsing)
        {
            if (a->focus_zone() == FocusZone::TopBar)
            {
                if (a->top_bar_index() == 0)
                    return 0;
            }
            else if (a->focus_zone() == FocusZone::Content)
            {
                const auto *tab = a->tabs().active_tab_info();
                const auto *view = a->tabs().active_view();
                if (tab && tab->url == "about:start")
                {
                    if (a->speed_dial_index() % 4 == 0)
                        return 0;
                }
                else if (view && view->has_webkit_instance())
                {
                    return 0;
                }
            }
        }
        a->on_dpad_left();
        break;
    case EVO_HUI_WEB_RIGHT: a->on_dpad_right(); break;
    case EVO_HUI_WEB_UP: a->on_dpad_up(); break;
    case EVO_HUI_WEB_DOWN: a->on_dpad_down(); break;
    case EVO_HUI_WEB_CROSS_DOWN: a->on_button_cross_pressed(); break;
    case EVO_HUI_WEB_CROSS_UP: a->on_button_cross_released(); break;
    case EVO_HUI_WEB_CIRCLE: a->on_button_circle_pressed(); break;
    case EVO_HUI_WEB_TRIANGLE: a->on_button_triangle_pressed(); break;
    case EVO_HUI_WEB_SQUARE: a->on_button_square_pressed(); break;
    case EVO_HUI_WEB_OPTIONS: a->on_button_options_pressed(); break;
    case EVO_HUI_WEB_L1: a->on_button_l1_pressed(); break;
    case EVO_HUI_WEB_R1: a->on_button_r1_pressed(); break;
    case EVO_HUI_WEB_TOUCHPAD: a->on_touchpad_pressed(); break;
    default: break;
    }
    return 1;
}

void evo_hui_web_sticks(float lx, float ly, float rx, float ry, float l2, float r2, float dt)
{
    auto *a = app();
    a->update_left_stick(lx, ly, dt);
    a->update_right_stick(rx, ry, dt);
    a->update_triggers(l2, r2, dt);
}

void evo_hui_web_open(const char *url)
{
    if (url && url[0])
        app()->navigate_to(url);
}

void evo_hui_web_pointer(int x, int y)
{
    g_pointer_x = static_cast<float>(x);
    g_pointer_y = static_cast<float>(y);
    g_pointer_pending = true;
}

int evo_hui_web_take_text_focus(void)
{
    auto *e = evo::web::EvoPageEngine::current();
    return e && e->take_text_focus() ? 1 : 0;
}

void evo_hui_web_insert_text(const char *utf8)
{
    if (auto *e = evo::web::EvoPageEngine::current())
        e->insert_text(utf8 ? utf8 : "");
}

void evo_hui_web_set_visible(int visible)
{
    if (auto *e = evo::web::EvoPageEngine::current())
        e->set_visible(visible != 0);
}
