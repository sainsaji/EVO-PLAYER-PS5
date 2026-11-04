// Prospero Browser - WPE WebKit Port for PlayStation 5
// Browser UI Chrome and Rendering Layer built with ps5-homebrew-ui.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later

#pragma once

#include "gfx/draw_list.hpp"
#include "ui/fonts.hpp"
#include "ui/glyphs.hpp"
#include "ui/theme.hpp"
#include "../browser/browser_app.hpp"
#include <string>

namespace prospero::ui {

class BrowserUi {
public:
    BrowserUi();

    // Render the complete browser frame into DrawList
    void render(hui::gfx::DrawList& list,
                const hui::ui::Fonts& fonts,
                browser::BrowserApp& app,
                float dt);

private:
    void render_header(hui::gfx::DrawList& list, const hui::ui::Fonts& fonts, browser::BrowserApp& app);
    void render_tab_overview(hui::gfx::DrawList& list, const hui::ui::Fonts& fonts, browser::BrowserApp& app);
    void render_downloads(hui::gfx::DrawList& list, const hui::ui::Fonts& fonts, browser::BrowserApp& app);
    void render_quick_menu(hui::gfx::DrawList& list, const hui::ui::Fonts& fonts, browser::BrowserApp& app);
    void render_toast(hui::gfx::DrawList& list, const hui::ui::Fonts& fonts, browser::BrowserApp& app);
    void render_tab_strip(hui::gfx::DrawList& list, const hui::ui::Fonts& fonts, browser::BrowserApp& app);
    void render_omnibox(hui::gfx::DrawList& list, const hui::ui::Fonts& fonts, browser::BrowserApp& app);
    void render_web_viewport(hui::gfx::DrawList& list, const hui::ui::Fonts& fonts, browser::BrowserApp& app);
    void render_loading_overlay(hui::gfx::DrawList& list, const hui::ui::Fonts& fonts, browser::BrowserApp& app, const hui::gfx::Rect& vp);
    void render_speed_dial(hui::gfx::DrawList& list, const hui::ui::Fonts& fonts, browser::BrowserApp& app);
    void render_cursor(hui::gfx::DrawList& list, const hui::ui::Fonts& fonts, browser::BrowserApp& app);
    void render_footer_hints(hui::gfx::DrawList& list, const hui::ui::Fonts& fonts, browser::BrowserApp& app);

    // Overlays
    void render_keyboard_overlay(hui::gfx::DrawList& list, const hui::ui::Fonts& fonts, browser::BrowserApp& app);
    void render_bookmarks_drawer(hui::gfx::DrawList& list, const hui::ui::Fonts& fonts, browser::BrowserApp& app);
    void render_settings_dialog(hui::gfx::DrawList& list, const hui::ui::Fonts& fonts, browser::BrowserApp& app);
    void render_history_drawer(hui::gfx::DrawList& list, const hui::ui::Fonts& fonts, browser::BrowserApp& app);
    void render_page_dialog(hui::gfx::DrawList& list, const hui::ui::Fonts& fonts, browser::BrowserApp& app);

    hui::ui::GlyphStyle glyph_style_{hui::ui::GlyphStyle::dark()};
    float pulse_time_{0.0f};
};

} // namespace prospero::ui
