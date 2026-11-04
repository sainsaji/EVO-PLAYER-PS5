// Prospero Browser - WPE WebKit Port for PlayStation 5
// Browser UI Chrome and Rendering Layer implementation.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later

#include "browser_ui.hpp"
#include "../net/download_manager.hpp"
#include "../evo_browser_hooks.hpp"  // EVO
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace prospero::ui {

BrowserUi::BrowserUi() = default;

void BrowserUi::render(hui::gfx::DrawList& list,
                       const hui::ui::Fonts& fonts,
                       browser::BrowserApp& app,
                       float dt) {
    pulse_time_ += dt;

    // 1. Draw web content viewport (texture from WPE WebKit)
    render_web_viewport(list, fonts, app);

    // A fullscreen element (video) gets the whole screen: no header or hints.
    if (app.is_fullscreen()) {
        if (app.ui_mode() == browser::UiMode::PageDialog) render_page_dialog(list, fonts, app);
        render_toast(list, fonts, app);
        render_cursor(list, fonts, app);
        return;
    }

    // 2. If active tab is on about:start, draw Speed Dial
    auto* tab_info = app.tabs().active_tab_info();
    if (tab_info && tab_info->url == "about:start") {
        render_speed_dial(list, fonts, app);
    }

    // 3. Draw top navigation header & omnibox
    render_header(list, fonts, app);

    // 4. Draw overlays if active
    if (app.ui_mode() == browser::UiMode::BookmarksDrawer) {
        render_bookmarks_drawer(list, fonts, app);
    } else if (app.ui_mode() == browser::UiMode::SettingsModal) {
        render_settings_dialog(list, fonts, app);
    } else if (app.ui_mode() == browser::UiMode::OmniboxEditing) {
        render_keyboard_overlay(list, fonts, app);
    } else if (app.ui_mode() == browser::UiMode::TabOverview) {
        render_tab_overview(list, fonts, app);
    } else if (app.ui_mode() == browser::UiMode::Downloads) {
        render_downloads(list, fonts, app);
    } else if (app.ui_mode() == browser::UiMode::QuickMenu) {
        render_quick_menu(list, fonts, app);
    } else if (app.ui_mode() == browser::UiMode::HistoryDrawer) {
        render_history_drawer(list, fonts, app);
    } else if (app.ui_mode() == browser::UiMode::PageDialog) {
        render_page_dialog(list, fonts, app);
    }

    // 5. Draw footer hints and any toast above them
    render_footer_hints(list, fonts, app);
    render_toast(list, fonts, app);

    // 6. Draw DualSense virtual cursor (only if in free-pointer mode)
    render_cursor(list, fonts, app);
}

void BrowserUi::render_web_viewport(hui::gfx::DrawList& list,
                                    const hui::ui::Fonts& fonts,
                                    browser::BrowserApp& app) {
    auto* view = app.tabs().active_view();
    if (!view) return;

    hui::gfx::Rect vp{app.viewport_x(), app.viewport_y(),
                      app.viewport_width(), app.viewport_height()};

    // EVO: the engine page is the kit's texture of the engine host's frames.
    std::uint32_t tex_id = 0;
    float u1 = 1.0f, v1 = 1.0f;
    if (view->has_webkit_instance() && evo_browser_page_texture(&tex_id, &u1, &v1)) {
        list.image(tex_id, vp, hui::gfx::Rect{0.0f, 0.0f, u1, v1}, hui::gfx::Color::rgb(0xffffff, 1.0f));
    } else {
        list.rounded_rect(vp, 0.0f, hui::gfx::Color::rgb(0x0a0c14, 1.0f));
    }

    auto* tab_info = app.tabs().active_tab_info();
    if (tab_info && tab_info->url == "about:start") {
        return; // Speed dial rendered on top in render()
    }

    // If active WPE WebKit instance is running and rendering a live web page,
    // the hardware texture already contains the rendered page.
    if (view->has_webkit_instance() && tab_info && tab_info->url.rfind("about:", 0) != 0) {
        return;
    }

    // Clip rendering strictly to viewport bounds
    list.push_clip(vp);

    const auto& doc = view->document();
    float scroll_y = static_cast<float>(view->scroll_offset_y());
    float cur_x = app.cursor().x() - vp.x;
    float cur_y = app.cursor().y() - vp.y + scroll_y;

    // Render parsed document elements
    for (const auto& block : doc.blocks()) {
        float draw_y = vp.y + block.box.y - scroll_y;
        float draw_h = block.box.height;

        // Culling: skip blocks outside vertical screen window
        if ((draw_y + draw_h) < vp.y || draw_y > (vp.y + vp.h)) {
            continue;
        }

        switch (block.type) {
            case web::BlockType::Heading1: {
                hui::ui::text(list, fonts.display, block.text,
                              vp.x + block.box.x, draw_y + 32.0f, 32.0f,
                              hui::gfx::Color::rgb(0xffffff));
                list.rounded_rect({vp.x + block.box.x, draw_y + 40.0f, 100.0f, 3.0f}, 0.0f,
                                  hui::gfx::Color::rgb(0x6366f1));
                break;
            }
            case web::BlockType::Heading2: {
                hui::ui::text(list, fonts.semibold, block.text,
                              vp.x + block.box.x, draw_y + 26.0f, 25.0f,
                              hui::gfx::Color::rgb(0xf1f5f9));
                break;
            }
            case web::BlockType::Heading3: {
                hui::ui::text(list, fonts.semibold, block.text,
                              vp.x + block.box.x, draw_y + 22.0f, 21.0f,
                              hui::gfx::Color::rgb(0xe2e8f0));
                break;
            }
            case web::BlockType::Paragraph: {
                float line_y = draw_y + 18.0f;
                for (const auto& line : block.lines) {
                    if (line_y >= vp.y - 20.0f && line_y <= vp.y + vp.h + 20.0f) {
                        hui::ui::text(list, fonts.regular, line,
                                      vp.x + block.box.x, line_y, 18.0f,
                                      hui::gfx::Color::rgb(0xcbd5e1));
                    }
                    line_y += 26.0f;
                }
                break;
            }
            case web::BlockType::Link: {
                bool is_hovered = (!app.using_dpad() && block.box.contains(cur_x, cur_y));
                hui::gfx::Rect link_rect{vp.x + block.box.x - 4.0f, draw_y, block.box.width + 8.0f, block.box.height};
                if (is_hovered) {
                    list.rounded_rect(link_rect, 6.0f, hui::gfx::Color::rgb(0x38bdf8, 0.25f));
                }
                hui::gfx::Color link_col = is_hovered ?
                    hui::gfx::Color::rgb(0x60a5fa) : hui::gfx::Color::rgb(0x38bdf8);
                hui::ui::text(list, fonts.semibold, block.text,
                              vp.x + block.box.x, draw_y + 18.0f, 18.0f,
                              link_col);
                list.rounded_rect({vp.x + block.box.x, draw_y + 22.0f, std::min(block.box.width, static_cast<float>(block.text.size()) * 10.0f), 1.5f}, 0.0f,
                                  link_col);
                break;
            }
            case web::BlockType::CodeBlock: {
                hui::gfx::Rect code_rect{vp.x + block.box.x, draw_y, block.box.width, block.box.height};
                list.bordered_rect(code_rect, 10.0f,
                                   hui::gfx::Color::rgb(0x080b12),
                                   1.0f, hui::gfx::Color::rgb(0x242d45));
                float code_y = draw_y + 20.0f;
                for (const auto& line : block.lines) {
                    if (code_y >= vp.y - 20.0f && code_y <= vp.y + vp.h + 20.0f) {
                        hui::ui::text(list, fonts.mono, line,
                                      vp.x + block.box.x + 16.0f, code_y, 15.0f,
                                      hui::gfx::Color::rgb(0xa5f3fc));
                    }
                    code_y += 22.0f;
                }
                break;
            }
            case web::BlockType::ListItem: {
                hui::ui::text(list, fonts.semibold, "*",
                              vp.x + block.box.x - 16.0f, draw_y + 18.0f, 18.0f,
                              hui::gfx::Color::rgb(0x818cf8));
                float line_y = draw_y + 18.0f;
                for (const auto& line : block.lines) {
                    if (line_y >= vp.y - 20.0f && line_y <= vp.y + vp.h + 20.0f) {
                        hui::ui::text(list, fonts.regular, line,
                                      vp.x + block.box.x, line_y, 18.0f,
                                      hui::gfx::Color::rgb(0xcbd5e1));
                    }
                    line_y += 26.0f;
                }
                break;
            }
            case web::BlockType::Divider: {
                list.rounded_rect({vp.x + block.box.x, draw_y + 6.0f, block.box.width, 1.5f}, 0.0f,
                                  hui::gfx::Color::rgb(0x1e293b));
                break;
            }
            case web::BlockType::Card: {
                hui::gfx::Rect card_rect{vp.x + block.box.x, draw_y, block.box.width, block.box.height};
                list.bordered_rect(card_rect, 12.0f,
                                   hui::gfx::Color::rgb(0x141828),
                                   1.0f, hui::gfx::Color::rgb(0x2a3250));
                hui::ui::text(list, fonts.semibold, block.text,
                              vp.x + block.box.x + 18.0f, draw_y + 26.0f, 18.0f,
                              hui::gfx::Color::rgb(0xf8fafc));
                break;
            }
        }
    }

    // Draw D-Pad focused link highlight if in console D-Pad navigation mode
    if (app.using_dpad() && app.focus_zone() == browser::FocusZone::Content) {
        const auto& links = doc.links();
        if (app.focused_link_index() >= 0 && app.focused_link_index() < static_cast<int>(links.size())) {
            const auto& link = links[app.focused_link_index()];
            float draw_y = vp.y + link.box.y - scroll_y;
            if ((draw_y + link.box.height) >= vp.y && draw_y <= (vp.y + vp.h)) {
                hui::gfx::Rect focus_rect{vp.x + link.box.x - 8.0f, draw_y - 2.0f, link.box.width + 16.0f, link.box.height + 6.0f};
                float pulse = 0.5f + 0.3f * std::sin(pulse_time_ * 5.0f);
                list.shadow(focus_rect, 14.0f, 20.0f, hui::gfx::Color::rgb(0x818cf8, 0.55f + 0.25f * pulse));
                list.bordered_rect(focus_rect, 8.0f, hui::gfx::Color::rgb(0x312e81, 0.7f), 2.5f, hui::gfx::Color::rgb(0x818cf8));
            }
        }
    }

    // Scrollbar indicator
    if (doc.total_height() > vp.h) {
        float track_x = vp.x + vp.w - 10.0f;
        float track_y = vp.y + 6.0f;
        float track_w = 6.0f;
        float track_h = vp.h - 12.0f;

        list.rounded_rect({track_x, track_y, track_w, track_h}, 3.0f,
                          hui::gfx::Color::rgb(0x334155, 0.35f));

        float thumb_ratio = std::max(0.08f, vp.h / doc.total_height());
        float thumb_h = track_h * thumb_ratio;
        float max_s = doc.max_scroll_y();
        float scroll_ratio = (max_s > 0.0f) ? (scroll_y / max_s) : 0.0f;
        float thumb_y = track_y + (track_h - thumb_h) * scroll_ratio;

        list.rounded_rect({track_x, thumb_y, track_w, thumb_h}, 3.0f,
                          hui::gfx::Color::rgb(0x818cf8, 0.85f));
    }

    // Active loading overlay
    if (view->is_loading()) {
        render_loading_overlay(list, fonts, app, vp);
    }

    list.pop_clip();
}

void BrowserUi::render_loading_overlay(hui::gfx::DrawList& list,
                                       const hui::ui::Fonts& fonts,
                                       browser::BrowserApp& app,
                                       const hui::gfx::Rect& vp) {
    auto* view = app.tabs().active_view();
    if (!view) return;

    // Thin accent loading progress bar at top of viewport
    float progress = view->load_progress();
    list.rounded_rect({vp.x, vp.y, vp.w * std::clamp(progress, 0.05f, 1.0f), 3.5f}, 0.0f,
                      hui::gfx::Color::rgb(0x6366f1));

    // Centered loading dialog card if page is still awaiting initial content
    if (view->document().is_empty()) {
        hui::gfx::Rect card{vp.x + (vp.w - 460.0f) / 2.0f, vp.y + (vp.h - 190.0f) / 2.0f, 460.0f, 190.0f};
        list.shadow(card, 16.0f, 24.0f, hui::gfx::Color::rgb(0x000000, 0.5f));
        list.bordered_rect(card, 18.0f, hui::gfx::Color::rgb(0x131726, 0.96f),
                           1.5f, hui::gfx::Color::rgb(0x313959));

        hui::ui::text(list, fonts.semibold, "Loading Web Page...",
                      card.x + 230.0f, card.y + 44.0f, 22.0f,
                      hui::gfx::Color::rgb(0xffffff), hui::gfx::Align::center);

        std::string host = browser::Navigation::extract_host(view->current_uri());
        if (host.empty()) host = view->current_uri();
        if (host.size() > 40) host = host.substr(0, 37) + "...";
        hui::ui::text(list, fonts.regular, host,
                      card.x + 230.0f, card.y + 78.0f, 16.0f,
                      hui::gfx::Color::rgb(0x818cf8), hui::gfx::Align::center);

        char bytes_str[64];
        std::snprintf(bytes_str, sizeof(bytes_str), "%zu KB received", view->bytes_downloaded() / 1024);
        hui::ui::text(list, fonts.mono, bytes_str,
                      card.x + 230.0f, card.y + 114.0f, 14.0f,
                      hui::gfx::Color::rgb(0x94a3b8), hui::gfx::Align::center);

        // Progress bar inside modal
        list.rounded_rect({card.x + 40.0f, card.y + 144.0f, 380.0f, 6.0f}, 3.0f,
                          hui::gfx::Color::rgb(0x1e2438));
        list.rounded_rect({card.x + 40.0f, card.y + 144.0f, 380.0f * std::clamp(progress, 0.05f, 1.0f), 6.0f}, 3.0f,
                          hui::gfx::Color::rgb(0x6366f1));
    }
}

void BrowserUi::render_header(hui::gfx::DrawList& list,
                              const hui::ui::Fonts& fonts,
                              browser::BrowserApp& app) {
    using namespace hui;

    // Header background with drop shadow and border
    gfx::Rect header_rect{0.0f, 0.0f, static_cast<float>(app.screen_width()), 72.0f};
    list.shadow(header_rect, 0.0f, 16.0f, gfx::Color::rgb(0x000000, 0.45f));
    list.bordered_rect(header_rect, 0.0f, gfx::Color::rgb(0x131622, 0.95f),
                       1.0f, gfx::Color::rgb(0x282e45, 0.6f));

    // Nav buttons: Back, Forward, Reload
    auto* view = app.tabs().active_view();
    bool can_back = view && view->can_go_back();
    bool can_fwd = view && view->can_go_forward();
    bool is_loading = view && view->is_loading();

    float pulse = 0.5f + 0.3f * std::sin(pulse_time_ * 4.0f);
    bool in_top_bar = app.using_dpad() && app.focus_zone() == browser::FocusZone::TopBar;

    // Back button
    bool focus_back = in_top_bar && app.top_bar_index() == 0;
    gfx::Color back_col = can_back ? gfx::Color::rgb(0xffffff) : gfx::Color::rgb(0x555a6e);
    gfx::Rect back_rect{16.0f, 16.0f, 40.0f, 40.0f};
    if (focus_back) {
        list.shadow(back_rect, 12.0f, 16.0f, gfx::Color::rgb(0x818cf8, 0.6f + 0.25f * pulse));
        list.bordered_rect(back_rect, 10.0f, gfx::Color::rgb(0x312e81), 2.5f, gfx::Color::rgb(0x818cf8));
    } else {
        list.rounded_rect(back_rect, 10.0f, gfx::Color::rgb(0x1e2338));
    }
    hui::ui::text(list, fonts.semibold, "<", 36.0f, 42.0f, 22.0f, back_col, gfx::Align::center);

    // Forward button
    bool focus_fwd = in_top_bar && app.top_bar_index() == 1;
    gfx::Color fwd_col = can_fwd ? gfx::Color::rgb(0xffffff) : gfx::Color::rgb(0x555a6e);
    gfx::Rect fwd_rect{64.0f, 16.0f, 40.0f, 40.0f};
    if (focus_fwd) {
        list.shadow(fwd_rect, 12.0f, 16.0f, gfx::Color::rgb(0x818cf8, 0.6f + 0.25f * pulse));
        list.bordered_rect(fwd_rect, 10.0f, gfx::Color::rgb(0x312e81), 2.5f, gfx::Color::rgb(0x818cf8));
    } else {
        list.rounded_rect(fwd_rect, 10.0f, gfx::Color::rgb(0x1e2338));
    }
    hui::ui::text(list, fonts.semibold, ">", 84.0f, 42.0f, 22.0f, fwd_col, gfx::Align::center);

    // Reload / Stop button
    bool focus_reload = in_top_bar && app.top_bar_index() == 2;
    gfx::Rect reload_rect{112.0f, 16.0f, 40.0f, 40.0f};
    if (focus_reload) {
        list.shadow(reload_rect, 12.0f, 16.0f, gfx::Color::rgb(0x818cf8, 0.6f + 0.25f * pulse));
        list.bordered_rect(reload_rect, 10.0f, gfx::Color::rgb(0x312e81), 2.5f, gfx::Color::rgb(0x818cf8));
    } else {
        list.rounded_rect(reload_rect, 10.0f, gfx::Color::rgb(0x1e2338));
    }
    hui::ui::text(list, fonts.semibold, is_loading ? "X" : "@", 132.0f, 42.0f, 20.0f,
                  gfx::Color::rgb(0x818cf8), gfx::Align::center);

    // Omnibox
    render_omnibox(list, fonts, app);

    // Tabs Strip
    render_tab_strip(list, fonts, app);

    // Quick Action button glyphs on the right
    bool focus_bm = in_top_bar && app.top_bar_index() == browser::kTopBookmarks;
    gfx::Rect bm_rect{1652.0f,  // EVO: was 1780 (1920 wide)
                      16.0f, 42.0f, 40.0f};
    if (focus_bm) {
        list.shadow(bm_rect, 12.0f, 16.0f, gfx::Color::rgb(0x818cf8, 0.6f + 0.25f * pulse));
        list.bordered_rect(bm_rect, 10.0f, gfx::Color::rgb(0x312e81), 2.5f, gfx::Color::rgb(0x818cf8));
    }
    hui::ui::draw_button(list, fonts, glyph_style_, hui::ui::Button::square, bm_rect.x + 21.0f, 36.0f, 28.0f);

    bool focus_opt = in_top_bar && app.top_bar_index() == browser::kTopSettings;
    gfx::Rect opt_rect{1707.0f,  // EVO: was 1835 (1920 wide)
                       16.0f, 42.0f, 40.0f};
    if (focus_opt) {
        list.shadow(opt_rect, 12.0f, 16.0f, gfx::Color::rgb(0x818cf8, 0.6f + 0.25f * pulse));
        list.bordered_rect(opt_rect, 10.0f, gfx::Color::rgb(0x312e81), 2.5f, gfx::Color::rgb(0x818cf8));
    }
    hui::ui::draw_button(list, fonts, glyph_style_, hui::ui::Button::options, opt_rect.x + 21.0f, 36.0f, 28.0f);
}

void BrowserUi::render_omnibox(hui::gfx::DrawList& list,
                               const hui::ui::Fonts& fonts,
                               browser::BrowserApp& app) {
    using namespace hui;

    gfx::Rect omni_rect{170.0f, 14.0f, 820.0f, 44.0f};

    bool is_editing = (app.ui_mode() == browser::UiMode::OmniboxEditing);
    bool is_dpad_focused = (app.using_dpad() && app.focus_zone() == browser::FocusZone::TopBar && app.top_bar_index() == 3);
    bool is_focused = is_editing || is_dpad_focused;

    float pulse = 0.5f + 0.3f * std::sin(pulse_time_ * 4.0f);
    gfx::Color border_color = is_focused ? gfx::Color::rgb(0x818cf8) : gfx::Color::rgb(0x2d334d);
    gfx::Color bg_color = is_focused ? gfx::Color::rgb(0x232942) : gfx::Color::rgb(0x1a1e30);

    if (is_focused) {
        list.shadow(omni_rect, 14.0f, 20.0f, gfx::Color::rgb(0x818cf8, 0.5f + 0.25f * pulse));
    }
    list.bordered_rect(omni_rect, 14.0f, bg_color, is_focused ? 2.5f : 1.5f, border_color);

    // SSL Lock or Local icon
    auto* tab = app.tabs().active_tab_info();
    std::string current_url = tab ? tab->url : "about:start";
    bool is_sec = browser::Navigation::is_secure(current_url);

    if (is_sec) {
        // Padlock: shackle outline over a solid body.
        const gfx::Color lock_col = gfx::Color::rgb(0x34d399);
        list.bordered_rect({185.0f, 24.0f, 12.0f, 14.0f}, 6.0f, gfx::Color::rgb(0x000000, 0.0f), 2.0f, lock_col);
        list.rounded_rect({182.0f, 31.0f, 18.0f, 14.0f}, 3.0f, lock_col);
    } else if (current_url.rfind("http://", 0) == 0) {
        hui::ui::text(list, fonts.semibold, "!", 191.0f, 42.0f, 20.0f, gfx::Color::rgb(0xfbbf24), gfx::Align::center);
    }

    // URL or display text; long addresses are cut so they stay inside the box.
    std::string display_url = is_editing ? app.omnibox_text() : current_url;
    if (display_url.empty()) display_url = "Enter URL or search...";
    constexpr size_t kMaxUrlChars = 62;
    if (!is_editing && display_url.size() > kMaxUrlChars) {
        display_url = display_url.substr(0, kMaxUrlChars - 3) + "...";
    }

    gfx::Color text_col = is_focused ? gfx::Color::rgb(0xffffff) : gfx::Color::rgb(0xd1d5db);
    hui::ui::text(list, fonts.regular, display_url, 215.0f, 42.0f, 20.0f, text_col);

    // Loading bar across the full width, along the bottom edge of the header.
    if (tab && tab->is_loading && tab->progress < 1.0f) {
        const float prog = std::max(tab->progress, 0.05f);
        list.rounded_rect({0.0f, 69.0f, static_cast<float>(app.screen_width()) * prog, 3.0f}, 1.5f, gfx::Color::rgb(0x6366f1));
    }

    // Triangle button hint to focus URL
    hui::ui::draw_button(list, fonts, glyph_style_, hui::ui::Button::triangle,
                         omni_rect.x + omni_rect.w - 32.0f, 36.0f, 24.0f);
}

void BrowserUi::render_tab_strip(hui::gfx::DrawList& list,
                                 const hui::ui::Fonts& fonts,
                                 browser::BrowserApp& app) {
    using namespace hui;

    const bool in_top_bar = app.using_dpad() && app.focus_zone() == browser::FocusZone::TopBar &&
                            app.ui_mode() == browser::UiMode::Browsing;
    const float pulse = 0.5f + 0.3f * std::sin(pulse_time_ * 4.0f);
    auto focus_ring = [&](const gfx::Rect& r) {
        list.shadow(r, 12.0f, 16.0f, gfx::Color::rgb(0x818cf8, 0.55f + 0.25f * pulse));
    };

    // Tabs button: tab count + the current tab's title. Opens the overview.
    const gfx::Rect tabs_rect{1010.0f, 16.0f, 300.0f, 40.0f};
    const bool focus_tabs = in_top_bar && app.top_bar_index() == browser::kTopTabs;
    if (focus_tabs) focus_ring(tabs_rect);
    list.bordered_rect(tabs_rect, 10.0f, gfx::Color::rgb(focus_tabs ? 0x353e66 : 0x2a314f),
                       focus_tabs ? 2.5f : 1.0f, gfx::Color::rgb(focus_tabs ? 0x818cf8 : 0x6366f1));

    // Count badge, like the tab button on mobile browsers.
    const gfx::Rect badge{tabs_rect.x + 10.0f, 23.0f, 28.0f, 26.0f};
    list.bordered_rect(badge, 6.0f, gfx::Color::rgb(0x000000, 0.0f), 2.0f, gfx::Color::rgb(0xe2e8f0));
    char count[8];
    std::snprintf(count, sizeof(count), "%zu", app.tabs().count());
    hui::ui::text(list, fonts.semibold, count, badge.x + badge.w / 2.0f, 42.0f, 16.0f,
                  gfx::Color::rgb(0xffffff), gfx::Align::center);

    const auto* tab = app.tabs().active_tab_info();
    std::string title = tab ? tab->title : "";
    if (title.size() > 19) title = title.substr(0, 17) + "..";
    hui::ui::text(list, fonts.semibold, title, tabs_rect.x + 50.0f, 42.0f, 18.0f, gfx::Color::rgb(0xffffff));
    hui::ui::draw_button(list, fonts, glyph_style_, hui::ui::Button::touchpad,
                         tabs_rect.x + tabs_rect.w - 36.0f, 36.0f, 20.0f);

    // New tab
    const gfx::Rect new_rect{1320.0f, 16.0f, 40.0f, 40.0f};
    const bool focus_new = in_top_bar && app.top_bar_index() == browser::kTopNewTab;
    if (focus_new) focus_ring(new_rect);
    list.bordered_rect(new_rect, 10.0f, gfx::Color::rgb(focus_new ? 0x353e66 : 0x1e2338),
                       focus_new ? 2.5f : 1.0f, gfx::Color::rgb(focus_new ? 0x818cf8 : 0x2d334d));
    hui::ui::text(list, fonts.semibold, "+", new_rect.x + 20.0f, 44.0f, 26.0f,
                  gfx::Color::rgb(0x818cf8), gfx::Align::center);

    // Downloads: an arrow into a tray, with a badge while files are downloading.
    const gfx::Rect dl_rect{1370.0f, 16.0f, 40.0f, 40.0f};
    const bool focus_dl = in_top_bar && app.top_bar_index() == browser::kTopDownloads;
    if (focus_dl) focus_ring(dl_rect);
    list.bordered_rect(dl_rect, 10.0f, gfx::Color::rgb(focus_dl ? 0x353e66 : 0x1e2338),
                       focus_dl ? 2.5f : 1.0f, gfx::Color::rgb(focus_dl ? 0x818cf8 : 0x2d334d));
    const gfx::Color ink = gfx::Color::rgb(0x818cf8);
    const float cx = dl_rect.x + 20.0f;
    list.line(cx, 25.0f, cx, 41.0f, 2.5f, ink);
    list.line(cx - 6.0f, 35.0f, cx, 41.0f, 2.5f, ink);
    list.line(cx + 6.0f, 35.0f, cx, 41.0f, 2.5f, ink);
    list.line(cx - 9.0f, 46.0f, cx + 9.0f, 46.0f, 2.5f, ink);
    const int running = net::DownloadManager::instance().running_count();
    if (running > 0) {
        const gfx::Rect dot{dl_rect.x + 26.0f, 10.0f, 20.0f, 20.0f};
        list.rounded_rect(dot, 10.0f, gfx::Color::rgb(0x6366f1));
        char n[8];
        std::snprintf(n, sizeof(n), "%d", running);
        hui::ui::text(list, fonts.semibold, n, dot.x + 10.0f, dot.y + 15.0f, 13.0f,
                      gfx::Color::rgb(0xffffff), gfx::Align::center);
    }
}

namespace {

std::string human_bytes(int64_t bytes) {
    char buf[32];
    if (bytes >= (1LL << 30)) std::snprintf(buf, sizeof(buf), "%.2f GB", static_cast<double>(bytes) / (1LL << 30));
    else if (bytes >= (1LL << 20)) std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1LL << 20));
    else std::snprintf(buf, sizeof(buf), "%lld KB", static_cast<long long>((bytes + 1023) / 1024));
    return buf;
}

} // namespace

void BrowserUi::render_downloads(hui::gfx::DrawList& list,
                                 const hui::ui::Fonts& fonts,
                                 browser::BrowserApp& app) {
    using namespace hui;
    const float top = 72.0f;
    const float bottom = 1080.0f - browser::BrowserApp::kFooterHeight;
    list.rounded_rect({0.0f, top, static_cast<float>(app.screen_width()), bottom - top}, 0.0f, gfx::Color::rgb(0x0b0d16, 1.0f));

    auto& dm = net::DownloadManager::instance();
    hui::ui::text(list, fonts.display, "Downloads", 360.0f, 140.0f, 34.0f, gfx::Color::rgb(0xffffff));
    hui::ui::text(list, fonts.regular, "Saved to " + dm.directory(), 360.0f, 174.0f, 17.0f,
                  gfx::Color::rgb(0x94a3b8));

    const auto items = dm.snapshot();
    if (items.empty()) {
        hui::ui::text(list, fonts.semibold, "No downloads yet", (static_cast<float>(app.screen_width()) / 2.0f), 480.0f, 26.0f,
                      gfx::Color::rgb(0xd1d5db), gfx::Align::center);
        hui::ui::text(list, fonts.regular, "Links to files (zip, pdf, pkg, apk, video...) are saved here.",
                      (static_cast<float>(app.screen_width()) / 2.0f), 520.0f, 18.0f, gfx::Color::rgb(0x94a3b8), gfx::Align::center);
        return;
    }

    constexpr float kRowH = 92.0f;
    constexpr float kRowGap = 12.0f;
    constexpr int kVisible = 7;
    const int sel = std::clamp(app.downloads_index(), 0, static_cast<int>(items.size()) - 1);
    const int first = std::clamp(sel - kVisible / 2, 0, std::max(0, static_cast<int>(items.size()) - kVisible));
    const float pulse = 0.5f + 0.3f * std::sin(pulse_time_ * 4.0f);

    for (int i = first; i < static_cast<int>(items.size()) && i < first + kVisible; ++i) {
        const auto& d = items[static_cast<size_t>(i)];
        const float y = 210.0f + static_cast<float>(i - first) * (kRowH + kRowGap);
        const gfx::Rect row{360.0f, y, 1200.0f, kRowH};
        const bool selected = i == sel;
        if (selected) list.shadow(row, 14.0f, 18.0f, gfx::Color::rgb(0x818cf8, 0.5f + 0.25f * pulse));
        list.bordered_rect(row, 12.0f, gfx::Color::rgb(0x161a2b), selected ? 2.5f : 1.0f,
                           gfx::Color::rgb(selected ? 0x818cf8 : 0x2d334d));

        std::string name = d.filename;
        if (name.size() > 70) name = name.substr(0, 68) + "..";
        hui::ui::text(list, fonts.semibold, name, row.x + 24.0f, y + 36.0f, 20.0f, gfx::Color::rgb(0xffffff));

        std::string status;
        gfx::Color status_col = gfx::Color::rgb(0x94a3b8);
        switch (d.state) {
            case net::DownloadState::Running:
                status = human_bytes(d.received);
                if (d.total > 0) {
                    char pct[16];
                    std::snprintf(pct, sizeof(pct), "  (%d%%)", static_cast<int>(d.received * 100 / d.total));
                    status += " of " + human_bytes(d.total) + pct;
                }
                break;
            case net::DownloadState::Done:
                status = "Done  -  " + human_bytes(d.received);
                status_col = gfx::Color::rgb(0x34d399);
                break;
            case net::DownloadState::Failed:
                status = "Failed: " + d.error;
                status_col = gfx::Color::rgb(0xf87171);
                break;
            case net::DownloadState::Cancelled:
                status = "Cancelled";
                break;
        }
        hui::ui::text(list, fonts.regular, status, row.x + 24.0f, y + 66.0f, 16.0f, status_col);

        if (d.state == net::DownloadState::Running) {
            const gfx::Rect track{row.x + 760.0f, y + 40.0f, 400.0f, 10.0f};
            list.rounded_rect(track, 5.0f, gfx::Color::rgb(0x2d334d));
            if (d.total > 0) {
                const float frac = std::clamp(static_cast<float>(d.received) / static_cast<float>(d.total), 0.0f, 1.0f);
                list.rounded_rect({track.x, track.y, track.w * frac, track.h}, 5.0f, gfx::Color::rgb(0x6366f1));
            } else {
                // Unknown size: a segment sliding along the track.
                const float t = std::fmod(pulse_time_ * 0.6f, 1.0f);
                list.rounded_rect({track.x + t * track.w * 0.75f, track.y, track.w * 0.25f, track.h}, 5.0f,
                                  gfx::Color::rgb(0x6366f1));
            }
        }
    }
}

void BrowserUi::render_tab_overview(hui::gfx::DrawList& list,
                                    const hui::ui::Fonts& fonts,
                                    browser::BrowserApp& app) {
    using namespace hui;

    // Dim everything between the header and the hint strip.
    const float top = 72.0f;
    const float bottom = 1080.0f - browser::BrowserApp::kFooterHeight;
    list.rounded_rect({0.0f, top, static_cast<float>(app.screen_width()), bottom - top}, 0.0f, gfx::Color::rgb(0x0b0d16, 1.0f));

    char heading[48];
    std::snprintf(heading, sizeof(heading), "Tabs  %zu / %zu", app.tabs().count(), browser::BrowserApp::kMaxTabs);
    hui::ui::text(list, fonts.display, heading, 54.0f, 140.0f, 34.0f, gfx::Color::rgb(0xffffff));

    constexpr int kCols = browser::BrowserApp::kOverviewColumns;
    constexpr float kCardW = 340.0f;
    constexpr float kThumbH = 191.0f;  // 16:9
    constexpr float kCardH = kThumbH + 66.0f;
    constexpr float kGap = 28.0f;
    const float x0 = (static_cast<float>(app.screen_width()) - (kCols * kCardW + (kCols - 1) * kGap)) / 2.0f;
    const float y0 = 180.0f;
    const float pulse = 0.5f + 0.3f * std::sin(pulse_time_ * 4.0f);

    const auto& tabs = app.tabs().tabs();
    const int count = app.tab_overview_count();
    for (int i = 0; i < count; ++i) {
        const float x = x0 + static_cast<float>(i % kCols) * (kCardW + kGap);
        const float y = y0 + static_cast<float>(i / kCols) * (kCardH + kGap);
        const gfx::Rect card{x, y, kCardW, kCardH};
        const bool selected = i == app.tab_overview_index();
        const bool is_new_card = i >= static_cast<int>(tabs.size());

        if (selected) {
            list.shadow(card, 16.0f, 20.0f, gfx::Color::rgb(0x818cf8, 0.6f + 0.25f * pulse));
        }
        list.bordered_rect(card, 14.0f, gfx::Color::rgb(0x161a2b), selected ? 3.0f : 1.0f,
                           gfx::Color::rgb(selected ? 0x818cf8 : 0x2d334d));

        if (is_new_card) {
            hui::ui::text(list, fonts.display, "+", x + kCardW / 2.0f, y + kCardH / 2.0f, 64.0f,
                          gfx::Color::rgb(0x818cf8), gfx::Align::center);
            hui::ui::text(list, fonts.semibold, "New tab", x + kCardW / 2.0f, y + kCardH / 2.0f + 44.0f, 20.0f,
                          gfx::Color::rgb(0xd1d5db), gfx::Align::center);
            continue;
        }

        const auto& tab = tabs[static_cast<size_t>(i)];
        const gfx::Rect thumb{x + 6.0f, y + 6.0f, kCardW - 12.0f, kThumbH - 6.0f};
        const browser::PageView* view = tab.view.get();
        if (view && view->has_webkit_instance() && view->texture_id() != 0) {
            // The tab's last page frame; inactive tabs keep their texture.
            list.push_clip(thumb);
            list.image(view->texture_id(), thumb, hui::gfx::kFullUv, gfx::Color::rgb(0xffffff, 1.0f));
            list.pop_clip();
        } else {
            list.rounded_rect(thumb, 10.0f, gfx::Color::rgb(0x0f172a));
            std::string host = tab.url == "about:start" ? "Start" : browser::Navigation::extract_host(tab.url);
            if (host.rfind("www.", 0) == 0) host = host.substr(4);
            hui::ui::text(list, fonts.display, host.empty() ? "?" : host.substr(0, 1),
                          thumb.x + thumb.w / 2.0f, thumb.y + thumb.h / 2.0f + 18.0f, 56.0f,
                          gfx::Color::rgb(0x6366f1), gfx::Align::center);
        }

        std::string title = tab.title.empty() ? tab.url : tab.title;
        if (title.size() > 30) title = title.substr(0, 28) + "..";
        hui::ui::text(list, fonts.semibold, title, x + 14.0f, y + kThumbH + 26.0f, 18.0f, gfx::Color::rgb(0xffffff));
        std::string host = tab.url == "about:start" ? "New tab" : browser::Navigation::extract_host(tab.url);
        if (host.size() > 36) host = host.substr(0, 34) + "..";
        hui::ui::text(list, fonts.regular, host, x + 14.0f, y + kThumbH + 50.0f, 15.0f, gfx::Color::rgb(0x94a3b8));

        if (static_cast<size_t>(i) == app.tabs().active_index()) {
            const gfx::Rect tag{x + kCardW - 92.0f, y + 14.0f, 78.0f, 26.0f};
            list.rounded_rect(tag, 8.0f, gfx::Color::rgb(0x6366f1));
            hui::ui::text(list, fonts.semibold, "Current", tag.x + tag.w / 2.0f, tag.y + 18.0f, 14.0f,
                          gfx::Color::rgb(0xffffff), gfx::Align::center);
        }
    }
}

void BrowserUi::render_quick_menu(hui::gfx::DrawList& list,
                                  const hui::ui::Fonts& fonts,
                                  browser::BrowserApp& app) {
    using namespace hui;
    constexpr float kRowH = 60.0f;
    constexpr float kW = 420.0f;
    const int count = browser::BrowserApp::kMenuItemCount;
    const gfx::Rect panel{static_cast<float>(app.screen_width()) - kW - 40.0f, 80.0f, kW, 24.0f + kRowH * static_cast<float>(count)};
    list.shadow(panel, 18.0f, 26.0f, gfx::Color::rgb(0x000000, 0.55f));
    list.bordered_rect(panel, 16.0f, gfx::Color::rgb(0x131622, 0.98f), 1.5f, gfx::Color::rgb(0x2d334d));
    const float pulse = 0.5f + 0.3f * std::sin(pulse_time_ * 4.0f);
    for (int i = 0; i < count; ++i) {
        const gfx::Rect row{panel.x + 12.0f, panel.y + 12.0f + kRowH * static_cast<float>(i), kW - 24.0f, kRowH - 6.0f};
        const bool selected = i == app.menu_index();
        if (selected) {
            list.shadow(row, 10.0f, 14.0f, gfx::Color::rgb(0x818cf8, 0.45f + 0.25f * pulse));
            list.bordered_rect(row, 10.0f, gfx::Color::rgb(0x312e81), 2.0f, gfx::Color::rgb(0x818cf8));
        }
        hui::ui::text(list, fonts.semibold, app.menu_label(i), row.x + 22.0f, row.y + 35.0f, 20.0f,
                      gfx::Color::rgb(selected ? 0xffffff : 0xd1d5db));
    }
}

void BrowserUi::render_toast(hui::gfx::DrawList& list,
                             const hui::ui::Fonts& fonts,
                             browser::BrowserApp& app) {
    using namespace hui;
    const float alpha = app.toast_alpha();
    if (alpha <= 0.0f || app.toast_text().empty()) return;
    const float w = static_cast<float>(app.toast_text().size()) * 11.0f + 48.0f;
    const gfx::Rect r{(static_cast<float>(app.screen_width()) - w) / 2.0f, 1080.0f - browser::BrowserApp::kFooterHeight - 64.0f, w, 44.0f};
    list.shadow(r, 10.0f, 14.0f, gfx::Color::rgb(0x000000, 0.45f * alpha));
    list.bordered_rect(r, 12.0f, gfx::Color::rgb(0x131622, 0.97f * alpha), 1.5f, gfx::Color::rgb(0x6366f1, alpha));
    hui::ui::text(list, fonts.semibold, app.toast_text(), r.x + w / 2.0f, r.y + 29.0f, 19.0f,
                  gfx::Color::rgb(0xffffff, alpha), gfx::Align::center);
}

void BrowserUi::render_speed_dial(hui::gfx::DrawList& list,
                                  const hui::ui::Fonts& fonts,
                                  browser::BrowserApp& app) {
    using namespace hui;

    // Center start page title
    hui::ui::text(list, fonts.display, "PROSPERO BROWSER", (static_cast<float>(app.screen_width()) / 2.0f), 240.0f, 44.0f,
                  gfx::Color::rgb(0xffffff), gfx::Align::center);
    hui::ui::text(list, fonts.regular, "Native WebKit Engine for PlayStation 5", (static_cast<float>(app.screen_width()) / 2.0f), 282.0f, 20.0f,
                  gfx::Color::rgb(0x818cf8), gfx::Align::center);

    // Speed Dial 4x2 Grid (10-Foot Console Experience)
    const auto& bookmarks = app.bookmarks().items();
    float start_x = (static_cast<float>(app.screen_width()) - (4.0f * 250.0f + 3.0f * 26.0f)) / 2.0f;  // EVO: was 420 (1920 wide)
    float start_y = 350.0f;
    float card_w = 250.0f;
    float card_h = 135.0f;
    float gap_x = 26.0f;
    float gap_y = 26.0f;

    float cur_x = app.cursor().x();
    float cur_y = app.cursor().y();
    float pulse = 0.5f + 0.3f * std::sin(pulse_time_ * 4.0f);

    for (size_t i = 0; i < bookmarks.size() && i < 8; ++i) {
        int col = static_cast<int>(i % 4);
        int row = static_cast<int>(i / 4);

        gfx::Rect card{start_x + col * (card_w + gap_x),
                       start_y + row * (card_h + gap_y),
                       card_w, card_h};

        bool hovered = (!app.using_dpad() && cur_x >= card.x && cur_x <= card.x + card.w &&
                        cur_y >= card.y && cur_y <= card.y + card.h);
        bool dpad_focused = (app.using_dpad() && app.focus_zone() == browser::FocusZone::Content &&
                             app.speed_dial_index() == static_cast<int>(i));

        bool active = hovered || dpad_focused;

        gfx::Color bg = active ? gfx::Color::rgb(0x282f4c) : gfx::Color::rgb(0x181c2c);
        gfx::Color border = active ? gfx::Color::rgb(0x818cf8) : gfx::Color::rgb(0x282f45);

        if (active) {
            list.shadow(card, 20.0f, 28.0f, gfx::Color::rgb(0x818cf8, 0.55f + 0.25f * pulse));
            if (!app.using_dpad()) {
                app.cursor().set_shape(browser::CursorShape::Pointer);
                if (app.cursor().is_button_down()) {
                    app.navigate_to(bookmarks[i].url);
                }
            }
        }

        list.bordered_rect(card, 16.0f, bg, active ? 2.5f : 1.0f, border);

        // Icon badge
        list.rounded_rect({card.x + 20.0f, card.y + 20.0f, 44.0f, 44.0f}, 12.0f,
                          active ? gfx::Color::rgb(0x3730a3) : gfx::Color::rgb(0x252b45));
        hui::ui::text(list, fonts.semibold, bookmarks[i].icon, card.x + 42.0f, card.y + 48.0f, 22.0f,
                      gfx::Color::rgb(0xffffff), gfx::Align::center);

        // Title and Category
        hui::ui::text(list, fonts.semibold, bookmarks[i].title, card.x + 20.0f, card.y + 94.0f, 19.0f,
                      gfx::Color::rgb(0xffffff));
        hui::ui::text(list, fonts.regular, bookmarks[i].category, card.x + 20.0f, card.y + 118.0f, 14.0f,
                      active ? gfx::Color::rgb(0xc7d2fe) : gfx::Color::rgb(0x94a3b8));
    }
}

void BrowserUi::render_footer_hints(hui::gfx::DrawList& list,
                                    const hui::ui::Fonts& fonts,
                                    browser::BrowserApp& app) {
    using namespace hui;

    // Solid strip under the hints, matching the header; the page viewport ends above it.
    const float footer_y = 1080.0f - browser::BrowserApp::kFooterHeight;
    gfx::Rect footer_rect{0.0f, footer_y, static_cast<float>(app.screen_width()), browser::BrowserApp::kFooterHeight};
    list.bordered_rect(footer_rect, 0.0f, gfx::Color::rgb(0x131622, 0.95f),
                       1.0f, gfx::Color::rgb(0x282e45, 0.6f));

    if (!app.hovered_link_url().empty()) {
        std::string link_txt = app.hovered_link_url();
        if (link_txt.size() > 65) {
            link_txt = link_txt.substr(0, 62) + "...";
        }
        float badge_w = static_cast<float>(link_txt.size()) * 10.0f + 36.0f;
        gfx::Rect badge{24.0f, footer_y - 44.0f, badge_w, 32.0f};
        list.shadow(badge, 6.0f, 10.0f, gfx::Color::rgb(0x000000, 0.4f));
        list.bordered_rect(badge, 8.0f, gfx::Color::rgb(0x0f172a, 0.95f), 1.0f, gfx::Color::rgb(0x38bdf8));
        hui::ui::text(list, fonts.semibold, link_txt, 36.0f, footer_y - 22.0f, 15.0f, gfx::Color::rgb(0x38bdf8));
    }

    static const hui::ui::Hint browse_hints[] = {
        {hui::ui::Button::dpad, "Navigate"},
        {hui::ui::Button::cross, "Select"},
        {hui::ui::Button::circle, "Back"},
        {hui::ui::Button::triangle, "URL / Search"},
        {hui::ui::Button::touchpad, "Tabs"},
        {hui::ui::Button::l1, "Switch tab", hui::ui::Button::r1},
        {hui::ui::Button::square, "Bookmarks"},
        {hui::ui::Button::options, "Menu"},
    };
    static const hui::ui::Hint overview_hints[] = {
        {hui::ui::Button::dpad, "Move"},
        {hui::ui::Button::cross, "Open"},
        {hui::ui::Button::square, "Close tab"},
        {hui::ui::Button::triangle, "New tab"},
        {hui::ui::Button::circle, "Back"},
    };
    static const hui::ui::Hint menu_hints[] = {
        {hui::ui::Button::dpad, "Move"},
        {hui::ui::Button::cross, "Select"},
        {hui::ui::Button::circle, "Back"},
    };
    static const hui::ui::Hint downloads_hints[] = {
        {hui::ui::Button::dpad, "Move"},
        {hui::ui::Button::square, "Cancel / Remove"},
        {hui::ui::Button::circle, "Back"},
    };
    static const hui::ui::Hint history_hints[] = {
        {hui::ui::Button::dpad, "Move"},
        {hui::ui::Button::cross, "Open"},
        {hui::ui::Button::square, "Remove"},
        {hui::ui::Button::circle, "Back"},
    };
    static const hui::ui::Hint settings_hints[] = {
        {hui::ui::Button::dpad, "Move"},
        {hui::ui::Button::cross, "Change"},
        {hui::ui::Button::circle, "Close"},
    };
    static const hui::ui::Hint dialog_hints[] = {
        {hui::ui::Button::dpad, "Choose"},
        {hui::ui::Button::cross, "Select"},
        {hui::ui::Button::circle, "Cancel"},
    };
    const hui::ui::Hint* hints = browse_hints;
    int hint_count = 8;
    if (app.ui_mode() == browser::UiMode::TabOverview) {
        hints = overview_hints;
        hint_count = 5;
    } else if (app.ui_mode() == browser::UiMode::Downloads) {
        hints = downloads_hints;
        hint_count = 3;
    } else if (app.ui_mode() == browser::UiMode::QuickMenu) {
        hints = menu_hints;
        hint_count = 3;
    } else if (app.ui_mode() == browser::UiMode::HistoryDrawer) {
        hints = history_hints;
        hint_count = 4;
    } else if (app.ui_mode() == browser::UiMode::SettingsModal) {
        hints = settings_hints;
        hint_count = 3;
    } else if (app.ui_mode() == browser::UiMode::PageDialog) {
        hints = dialog_hints;
        hint_count = 3;
    }

    hui::ui::HintLayout layout{};
    layout.size = 28.0f;
    layout.text_size = 20.0f;
    layout.cy = footer_y + browser::BrowserApp::kFooterHeight / 2.0f;
    layout.item_gap = 36.0f;

    hui::ui::draw_hints(list, fonts, glyph_style_, hints, hint_count, 80.0f, false, layout);
}

void BrowserUi::render_cursor(hui::gfx::DrawList& list,
                              const hui::ui::Fonts& /*fonts*/,
                              browser::BrowserApp& app) {
    if (app.using_dpad()) {
        return; // Mouse pointer is hidden in console D-Pad navigation mode
    }

    using namespace hui;

    float cx = app.cursor().x();
    float cy = app.cursor().y();
    float pulse = app.cursor().click_pulse();

    // Pulse animation ring on click
    if (pulse > 0.0f) {
        float ring_radius = 12.0f + (1.0f - pulse) * 20.0f;
        list.shadow({cx - ring_radius, cy - ring_radius, ring_radius * 2.0f, ring_radius * 2.0f},
                    ring_radius, 8.0f, gfx::Color::rgb(0x818cf8, pulse * 0.8f));
    }

    // Glowing pointer ring
    list.shadow({cx - 14.0f, cy - 14.0f, 28.0f, 28.0f}, 14.0f, 16.0f,
                gfx::Color::rgb(0x6366f1, 0.45f));

    // Outer ring
    list.bordered_rect({cx - 10.0f, cy - 10.0f, 20.0f, 20.0f}, 10.0f,
                       gfx::Color::rgb(0xffffff, 0.9f), 2.0f, gfx::Color::rgb(0x6366f1));

    // Inner bright point
    list.rounded_rect({cx - 4.0f, cy - 4.0f, 8.0f, 8.0f}, 4.0f,
                      gfx::Color::rgb(0x818cf8, 1.0f));
}

void BrowserUi::render_keyboard_overlay(hui::gfx::DrawList& list,
                                        const hui::ui::Fonts& fonts,
                                        browser::BrowserApp& app) {
    using namespace hui;

    // Dim background
    list.rounded_rect({0.0f, 0.0f, static_cast<float>(app.screen_width()), 1080.0f}, 0.0f, gfx::Color::rgb(0x000000, 0.72f));

    // Keyboard container card
    gfx::Rect kbd_box{(static_cast<float>(app.screen_width()) - 1400.0f) / 2.0f, 410.0f, 1400.0f, 560.0f};  // EVO: was x 260 (1920 wide)
    list.shadow(kbd_box, 24.0f, 40.0f, gfx::Color::rgb(0x000000, 0.65f));
    list.bordered_rect(kbd_box, 24.0f, gfx::Color::rgb(0x131626, 0.98f), 2.0f, gfx::Color::rgb(0x2f3653));

    // Title / prompt
    hui::ui::text(list, fonts.semibold, "ENTER WEB ADDRESS OR SEARCH QUERY", (static_cast<float>(app.screen_width()) / 2.0f), 455.0f, 22.0f,
                  gfx::Color::rgb(0x818cf8), gfx::Align::center);

    // Current input display box
    gfx::Rect input_rect{kbd_box.x + 60.0f, 480.0f, 1280.0f, 56.0f};  // EVO: was x 320
    list.shadow(input_rect, 8.0f, 12.0f, gfx::Color::rgb(0x6366f1, 0.25f));
    list.bordered_rect(input_rect, 14.0f, gfx::Color::rgb(0x1b2035), 2.0f, gfx::Color::rgb(0x6366f1));

    float pulse = 0.5f + 0.5f * std::sin(pulse_time_ * 6.0f);
    std::string cursor_char = (pulse > 0.3f) ? "_" : " ";
    hui::ui::text(list, fonts.regular, app.omnibox_text() + cursor_char, 345.0f, 516.0f, 22.0f,
                  gfx::Color::rgb(0xffffff));

    // Keyboard Rows
    static const char* const kRow0[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "/", ".", "Bksp"};
    static const char* const kRow1[] = {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "https://", "www."};
    static const char* const kRow2[] = {"a", "s", "d", "f", "g", "h", "j", "k", "l", ":", "?", ".com"};
    static const char* const kRow3[] = {"z", "x", "c", "v", "b", "n", "m", "Clear", "Space", "Go"};

    const char* const* rows[4] = {kRow0, kRow1, kRow2, kRow3};
    int row_counts[4] = {14, 12, 12, 10};

    float start_y = 560.0f;
    float row_h = 52.0f;
    float row_gap = 14.0f;

    for (int r = 0; r < 4; ++r) {
        float y = start_y + r * (row_h + row_gap);
        int count = row_counts[r];

        float key_gap = 10.0f;
        float total_w = 0.0f;
        for (int c = 0; c < count; ++c) {
            std::string_view lbl = rows[r][c];
            float kw = 72.0f;
            if (lbl == "Bksp" || lbl == "Clear") kw = 108.0f;
            else if (lbl == "https://") kw = 120.0f;
            else if (lbl == "www." || lbl == ".com") kw = 104.0f;
            else if (lbl == "Space") kw = 260.0f;
            else if (lbl == "Go") kw = 110.0f;
            total_w += kw;
            if (c > 0) total_w += key_gap;
        }

        float cur_x = (static_cast<float>(app.screen_width()) / 2.0f) - (total_w / 2.0f);

        for (int c = 0; c < count; ++c) {
            std::string_view lbl = rows[r][c];
            float kw = 72.0f;
            if (lbl == "Bksp" || lbl == "Clear") kw = 108.0f;
            else if (lbl == "https://") kw = 120.0f;
            else if (lbl == "www." || lbl == ".com") kw = 104.0f;
            else if (lbl == "Space") kw = 260.0f;
            else if (lbl == "Go") kw = 110.0f;

            gfx::Rect krect{cur_x, y, kw, row_h};
            bool focused = (app.keyboard_row() == r && app.keyboard_col() == c);

            if (focused) {
                float kpulse = 0.5f + 0.3f * std::sin(pulse_time_ * 5.0f);
                list.shadow(krect, 14.0f, 20.0f, gfx::Color::rgb(0x818cf8, 0.6f + 0.25f * kpulse));
                list.bordered_rect(krect, 12.0f, gfx::Color::rgb(0x4338ca), 2.5f, gfx::Color::rgb(0xa5b4fc));
                hui::ui::text(list, fonts.semibold, std::string(lbl), krect.x + krect.w / 2.0f, krect.y + 34.0f, 20.0f,
                              gfx::Color::rgb(0xffffff), gfx::Align::center);
            } else {
                list.bordered_rect(krect, 12.0f, gfx::Color::rgb(0x191e32), 1.0f, gfx::Color::rgb(0x2d3550));
                hui::ui::text(list, fonts.semibold, std::string(lbl), krect.x + krect.w / 2.0f, krect.y + 34.0f, 18.0f,
                              gfx::Color::rgb(0xcbd5e1), gfx::Align::center);
            }

            cur_x += kw + key_gap;
        }
    }

    // Keyboard hints row
    static const hui::ui::Hint kbd_hints[] = {
        {hui::ui::Button::cross, "Type Key"},
        {hui::ui::Button::square, "Backspace"},
        {hui::ui::Button::triangle, "Submit / Go"},
        {hui::ui::Button::circle, "Cancel"},
    };

    hui::ui::HintLayout layout{};
    layout.size = 24.0f;
    layout.text_size = 18.0f;
    layout.cy = 932.0f;
    layout.item_gap = 40.0f;

    hui::ui::draw_hints(list, fonts, glyph_style_, kbd_hints, 4, (static_cast<float>(app.screen_width()) / 2.0f), false, layout);
}

void BrowserUi::render_bookmarks_drawer(hui::gfx::DrawList& list,
                                        const hui::ui::Fonts& fonts,
                                        browser::BrowserApp& app) {
    using namespace hui;

    // Right slide-out drawer
    gfx::Rect drawer{static_cast<float>(app.screen_width()) - 560.0f, 0.0f, 560.0f, 1080.0f};  // EVO: was 1360 (1920 wide)
    list.shadow(drawer, 0.0f, 32.0f, gfx::Color::rgb(0x000000, 0.7f));
    list.bordered_rect(drawer, 0.0f, gfx::Color::rgb(0x121522), 1.0f, gfx::Color::rgb(0x282f48));

    hui::ui::text(list, fonts.display, "BOOKMARKS", 1400.0f, 70.0f, 30.0f,
                  gfx::Color::rgb(0xffffff));

    const auto& items = app.bookmarks().items();
    float item_y = 120.0f;
    float cur_x = app.cursor().x();
    float cur_y = app.cursor().y();

    for (size_t i = 0; i < items.size(); ++i) {
        const auto& bm = items[i];
        gfx::Rect row_rect{1390.0f, item_y, 500.0f, 60.0f};
        bool hovered = (!app.using_dpad() && cur_x >= row_rect.x && cur_x <= row_rect.x + row_rect.w &&
                        cur_y >= row_rect.y && cur_y <= row_rect.y + row_rect.h);
        bool focused = (app.using_dpad() && app.focus_zone() == browser::FocusZone::Drawer &&
                        app.drawer_index() == static_cast<int>(i));

        bool active = hovered || focused;
        gfx::Color bg = active ? gfx::Color::rgb(0x2b3352) : gfx::Color::rgb(0x171b2b);

        if (active) {
            float pulse = 0.5f + 0.3f * std::sin(pulse_time_ * 4.0f);
            list.shadow(row_rect, 14.0f, 20.0f, gfx::Color::rgb(0x818cf8, 0.55f + 0.25f * pulse));
            list.bordered_rect(row_rect, 12.0f, bg, 2.5f, gfx::Color::rgb(0x818cf8));
        } else {
            list.rounded_rect(row_rect, 12.0f, bg);
        }

        hui::ui::text(list, fonts.semibold, bm.icon, row_rect.x + 20.0f, row_rect.y + 36.0f, 20.0f,
                      gfx::Color::rgb(0xffffff));
        hui::ui::text(list, fonts.semibold, bm.title, row_rect.x + 60.0f, row_rect.y + 36.0f, 20.0f,
                      gfx::Color::rgb(0xffffff));

        if (hovered && !app.using_dpad() && app.cursor().is_button_down()) {
            app.navigate_to(bm.url);
            app.close_overlays();
        }

        item_y += 72.0f;
    }
}

void BrowserUi::render_settings_dialog(hui::gfx::DrawList& list,
                                       const hui::ui::Fonts& fonts,
                                       browser::BrowserApp& app) {
    using namespace hui;

    // Modal dim
    list.rounded_rect({0.0f, 0.0f, static_cast<float>(app.screen_width()), 1080.0f}, 0.0f, gfx::Color::rgb(0x000000, 0.7f));

    constexpr float kRowH = 62.0f;
    const float modal_h = 150.0f + kRowH * static_cast<float>(browser::kSettingsItemCount) + 40.0f;
    gfx::Rect modal{(static_cast<float>(app.screen_width()) - 880.0f) / 2.0f, (1080.0f - modal_h) / 2.0f, 880.0f, modal_h};
    list.shadow(modal, 24.0f, 40.0f, gfx::Color::rgb(0x000000, 0.65f));
    list.bordered_rect(modal, 24.0f, gfx::Color::rgb(0x131625), 1.5f, gfx::Color::rgb(0x2f3653));

    hui::ui::text(list, fonts.display, "SETTINGS", modal.x + 40.0f, modal.y + 64.0f, 30.0f,
                  gfx::Color::rgb(0xffffff));
    hui::ui::text(list, fonts.regular, "Engine: PlayStation 5 system WebKit", modal.x + 40.0f, modal.y + 98.0f,
                  17.0f, gfx::Color::rgb(0x94a3b8));

    const float pulse = 0.5f + 0.3f * std::sin(pulse_time_ * 4.0f);
    float y = modal.y + 124.0f;
    for (int i = 0; i < browser::kSettingsItemCount; ++i) {
        const gfx::Rect row{modal.x + 40.0f, y, modal.w - 80.0f, kRowH - 10.0f};
        const bool selected = i == app.settings_index();
        if (selected) {
            list.shadow(row, 10.0f, 14.0f, gfx::Color::rgb(0x818cf8, 0.45f + 0.25f * pulse));
            list.bordered_rect(row, 12.0f, gfx::Color::rgb(0x312e81), 2.0f, gfx::Color::rgb(0x818cf8));
        } else {
            list.rounded_rect(row, 12.0f, gfx::Color::rgb(0x181c2e));
        }
        hui::ui::text(list, fonts.semibold, app.settings_label(i), row.x + 24.0f, row.y + 33.0f, 19.0f,
                      gfx::Color::rgb(selected ? 0xffffff : 0xe2e8f0));
        y += kRowH;
    }
}

void BrowserUi::render_history_drawer(hui::gfx::DrawList& list,
                                      const hui::ui::Fonts& fonts,
                                      browser::BrowserApp& app) {
    using namespace hui;
    const float top = 72.0f;
    const float bottom = 1080.0f - browser::BrowserApp::kFooterHeight;
    list.rounded_rect({0.0f, top, static_cast<float>(app.screen_width()), bottom - top}, 0.0f, gfx::Color::rgb(0x0b0d16, 1.0f));
    hui::ui::text(list, fonts.display, "History", 360.0f, 140.0f, 34.0f, gfx::Color::rgb(0xffffff));

    const auto& items = app.history().entries();
    if (items.empty()) {
        hui::ui::text(list, fonts.semibold, "No pages visited yet", (static_cast<float>(app.screen_width()) / 2.0f), 480.0f, 26.0f,
                      gfx::Color::rgb(0xd1d5db), gfx::Align::center);
        return;
    }

    constexpr float kRowH = 74.0f;
    constexpr float kRowGap = 10.0f;
    constexpr int kVisible = 9;
    const int count = static_cast<int>(items.size());
    const int sel = std::clamp(app.history_index(), 0, count - 1);
    const int first = std::clamp(sel - kVisible / 2, 0, std::max(0, count - kVisible));
    const float pulse = 0.5f + 0.3f * std::sin(pulse_time_ * 4.0f);
    const std::time_t now = std::time(nullptr);

    for (int i = first; i < count && i < first + kVisible; ++i) {
        const auto& e = items[static_cast<size_t>(i)];
        const float y = 180.0f + static_cast<float>(i - first) * (kRowH + kRowGap);
        const gfx::Rect row{360.0f, y, 1200.0f, kRowH};
        const bool selected = i == sel;
        if (selected) list.shadow(row, 14.0f, 18.0f, gfx::Color::rgb(0x818cf8, 0.5f + 0.25f * pulse));
        list.bordered_rect(row, 12.0f, gfx::Color::rgb(0x161a2b), selected ? 2.5f : 1.0f,
                           gfx::Color::rgb(selected ? 0x818cf8 : 0x2d334d));

        std::string title = e.title.empty() ? browser::Navigation::extract_host(e.url) : e.title;
        title = fonts.semibold.font ? fonts.semibold.font->fit(title, 20.0f, 900.0f) : title.substr(0, 80);
        hui::ui::text(list, fonts.semibold, title, row.x + 24.0f, y + 32.0f, 20.0f, gfx::Color::rgb(0xffffff));
        std::string url = fonts.regular.font ? fonts.regular.font->fit(e.url, 15.0f, 900.0f) : e.url.substr(0, 100);
        hui::ui::text(list, fonts.regular, url, row.x + 24.0f, y + 58.0f, 15.0f, gfx::Color::rgb(0x94a3b8));

        // When: minutes / hours / days ago.
        const long long age = e.visited > 0 ? static_cast<long long>(now - e.visited) : -1;
        char when[32] = "";
        if (age >= 0 && age < 3600) std::snprintf(when, sizeof(when), "%lld min ago", age / 60);
        else if (age >= 0 && age < 86400) std::snprintf(when, sizeof(when), "%lld h ago", age / 3600);
        else if (age >= 0) std::snprintf(when, sizeof(when), "%lld days ago", age / 86400);
        hui::ui::text(list, fonts.regular, when, row.x + row.w - 24.0f, y + 32.0f, 15.0f,
                      gfx::Color::rgb(0x94a3b8), gfx::Align::right);
    }
}

void BrowserUi::render_page_dialog(hui::gfx::DrawList& list,
                                   const hui::ui::Fonts& fonts,
                                   browser::BrowserApp& app) {
    using namespace hui;
    const web::EngineEvent* d = app.page_dialog();
    if (!d) return;
    using Kind = web::EngineEvent::DialogKind;

    list.rounded_rect({0.0f, 0.0f, static_cast<float>(app.screen_width()), 1080.0f}, 0.0f, gfx::Color::rgb(0x000000, 0.6f));

    const auto* tab = app.tabs().active_tab_info();
    std::string heading = tab ? browser::Navigation::extract_host(tab->url) : std::string();
    heading = heading.empty() ? "This page says" : heading + " says";
    std::string message = d->message;
    if (d->dialog == Kind::BeforeUnload) {
        heading = "Leave this page?";
        message = "Changes you made may not be saved.";
    }

    constexpr float kW = 900.0f;
    constexpr float kTextW = kW - 96.0f;
    std::vector<std::string> lines =
        fonts.regular.font ? fonts.regular.font->wrap(message, 20.0f, kTextW) : std::vector<std::string>{message};
    constexpr size_t kMaxLines = 12;
    if (lines.size() > kMaxLines) {
        lines.resize(kMaxLines);
        lines.back() += " ...";
    }
    const bool prompt = d->dialog == Kind::Prompt;
    const float text_h = static_cast<float>(lines.size()) * 30.0f + (prompt ? 70.0f : 0.0f);
    const float h = 130.0f + text_h + 110.0f;
    const gfx::Rect modal{(static_cast<float>(app.screen_width()) - kW) / 2.0f, (1080.0f - h) / 2.0f, kW, h};
    list.shadow(modal, 24.0f, 40.0f, gfx::Color::rgb(0x000000, 0.65f));
    list.bordered_rect(modal, 20.0f, gfx::Color::rgb(0x131625), 1.5f, gfx::Color::rgb(0x2f3653));

    hui::ui::text(list, fonts.semibold, heading, modal.x + 48.0f, modal.y + 66.0f, 24.0f, gfx::Color::rgb(0xffffff));
    float y = modal.y + 116.0f;
    for (const auto& line : lines) {
        hui::ui::text(list, fonts.regular, line, modal.x + 48.0f, y, 20.0f, gfx::Color::rgb(0xe2e8f0));
        y += 30.0f;
    }
    if (prompt) {
        // The answer is the page's default text: typing needs the on-screen keyboard.
        const gfx::Rect field{modal.x + 48.0f, y - 6.0f, kTextW, 50.0f};
        list.bordered_rect(field, 10.0f, gfx::Color::rgb(0x0b0d16), 1.0f, gfx::Color::rgb(0x2d334d));
        const std::string value = d->default_text.empty() ? "(empty answer)" : d->default_text;
        hui::ui::text(list, fonts.regular, value, field.x + 18.0f, field.y + 33.0f, 19.0f,
                      gfx::Color::rgb(d->default_text.empty() ? 0x64748b : 0xffffff));
        y += 70.0f;
    }

    const char* labels[2] = {"OK", "Cancel"};
    if (d->dialog == Kind::BeforeUnload) {
        labels[0] = "Leave";
        labels[1] = "Stay";
    }
    const int buttons = app.dialog_button_count();
    constexpr float kBtnW = 180.0f;
    constexpr float kBtnH = 54.0f;
    const float pulse = 0.5f + 0.3f * std::sin(pulse_time_ * 4.0f);
    float bx = modal.x + modal.w - 48.0f - kBtnW * static_cast<float>(buttons) - 20.0f * static_cast<float>(buttons - 1);
    const float by = modal.y + modal.h - 48.0f - kBtnH;
    for (int i = 0; i < buttons; ++i) {
        const gfx::Rect btn{bx, by, kBtnW, kBtnH};
        const bool selected = i == app.dialog_button();
        if (selected) {
            list.shadow(btn, 12.0f, 14.0f, gfx::Color::rgb(0x818cf8, 0.45f + 0.25f * pulse));
            list.bordered_rect(btn, 12.0f, gfx::Color::rgb(0x4f46e5), 2.0f, gfx::Color::rgb(0x818cf8));
        } else {
            list.bordered_rect(btn, 12.0f, gfx::Color::rgb(0x181c2e), 1.0f, gfx::Color::rgb(0x2d334d));
        }
        hui::ui::text(list, fonts.semibold, labels[i], btn.x + kBtnW / 2.0f, btn.y + 35.0f, 20.0f,
                      gfx::Color::rgb(0xffffff), gfx::Align::center);
        bx += kBtnW + 20.0f;
    }
}

} // namespace prospero::ui
