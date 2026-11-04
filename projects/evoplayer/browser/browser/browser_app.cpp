// Prospero Browser - WPE WebKit Port for PlayStation 5
// Browser Application Core Coordinator implementation.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later

#include "browser_app.hpp"
#include "../net/download_manager.hpp"
#include "../web/page_engine.hpp"
#include "platform/ps5/webkit_dialog.hpp"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#include <cmath>

#if defined(__PROSPERO__)
#include "platform/ps5/system.hpp"
#define BROWSER_LOG(...) hui::sys::log(__VA_ARGS__)
#else
#define BROWSER_LOG(...) do {} while (0)
#endif

namespace prospero::browser {

static const char* const kKbdRow0[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "/", ".", "Bksp"};
static const char* const kKbdRow1[] = {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "https://", "www."};
static const char* const kKbdRow2[] = {"a", "s", "d", "f", "g", "h", "j", "k", "l", ":", "?", ".com"};
static const char* const kKbdRow3[] = {"z", "x", "c", "v", "b", "n", "m", "Clear", "Space", "Go"};

BrowserApp::BrowserApp() = default;
BrowserApp::~BrowserApp() = default;

bool BrowserApp::initialize(uint32_t screen_width, uint32_t screen_height) {
    BROWSER_LOG("[BROWSER_APP] Initializing coordinates (%ux%u)...", screen_width, screen_height);

    screen_width_ = screen_width;
    screen_height_ = screen_height;

    viewport_x_ = 0.0f;
    viewport_y_ = 72.0f;
    viewport_width_ = static_cast<float>(screen_width);
    viewport_height_ = static_cast<float>(screen_height) - viewport_y_ - kFooterHeight;

    cursor_.set_bounds(0.0f, 0.0f, static_cast<float>(screen_width), static_cast<float>(screen_height));
    cursor_.set_position(screen_width / 2.0f, screen_height / 2.0f);

    BROWSER_LOG("[BROWSER_APP] Initializing TabManager (%dx%d)...",
                static_cast<int>(viewport_width_), static_cast<int>(viewport_height_));

    if (!tab_manager_.initialize(static_cast<uint32_t>(viewport_width_),
                                static_cast<uint32_t>(viewport_height_))) {
        BROWSER_LOG("[BROWSER_APP] TabManager::initialize failed!");
        return false;
    }

    BROWSER_LOG("[BROWSER_APP] Loading settings...");
    mkdir("/data/evoplayer/browser", 0777);  // EVO: its own folder. Settings, bookmarks, history and the session are saved here
    settings_.load_from_file(kSettingsPath);
    bookmarks_.load_from_file(kBookmarksPath);
    history_.load_from_file(kHistoryPath);
    tab_manager_.set_zoom_level(settings_.default_zoom);
    if (settings_.restore_tabs) restore_session();
    saved_session_.clear();  // write the session once it has settled

    // Responses the page engine cannot show become downloads (called on the engine thread).
    web::download_request_handler() = [](const web::DownloadRequest& req) {
        net::DownloadManager::instance().start(req.url, req.suggested_name, req.expected_length);
    };
    cursor_.set_speed_multiplier(settings_.cursor_sensitivity);

    using_dpad_ = true;
    focus_zone_ = FocusZone::Content;
    speed_dial_index_ = 0;
    top_bar_index_ = 3;

    BROWSER_LOG("[BROWSER_APP] Initializing WebKitDialog fallback...");
    ps5::WebKitDialog::instance().initialize();

    BROWSER_LOG("[BROWSER_APP] BrowserApp::initialize completed successfully!");
    return true;
}

void BrowserApp::resize(uint32_t screen_width, uint32_t screen_height) {
    screen_width_ = screen_width;
    screen_height_ = screen_height;

    viewport_width_ = static_cast<float>(screen_width);
    viewport_height_ = static_cast<float>(screen_height) - viewport_y_ - kFooterHeight;

    cursor_.set_bounds(0.0f, 0.0f, static_cast<float>(screen_width), static_cast<float>(screen_height));
    tab_manager_.resize(static_cast<uint32_t>(viewport_width_),
                        static_cast<uint32_t>(viewport_height_));
}

void BrowserApp::update(float dt) {
    if (toast_time_ > 0.0f) toast_time_ = std::max(0.0f, toast_time_ - dt);
    std::string notice;
    if (net::DownloadManager::instance().take_notice(&notice)) {
        show_toast(notice);
        // A typed URL that turned out to be a file leaves the tab blank on that URL;
        // like other browsers, stay on (or return to) the page the tab was showing.
        const auto downloads = net::DownloadManager::instance().snapshot();
        auto* tab = tab_manager_.active_tab_info();
        auto* view = tab_manager_.active_view();
        if (!downloads.empty() && downloads.front().id != last_download_id_) {
            last_download_id_ = downloads.front().id;
            if (tab && view && tab->url == downloads.front().url) {
                if (view->can_go_back()) {
                    view->go_back();
                } else {
                    navigate_to("about:start");
                }
            }
        }
    }
    ps5::WebKitDialog::instance().update();

    if (ps5::WebKitDialog::instance().is_running()) {
        const std::string& cur = ps5::WebKitDialog::instance().current_url();
        if (!cur.empty()) {
            auto* tab = tab_manager_.active_tab_info();
            if (tab && tab->url != cur) {
                tab->url = cur;
                tab->title = Navigation::extract_host(cur);
                if (tab->title.empty()) tab->title = cur;
                omnibox_text_ = cur;
            }
        }
    }

    cursor_.update_animation(dt);
    tab_manager_.update(dt);
    adopt_new_windows();
    close_tabs_that_asked();
    handle_page_dialogs();
    sync_fullscreen();
    record_history(dt);
    save_session(dt);
    last_timestamp_ms_ += static_cast<uint32_t>(dt * 1000.0f);

    // Forward pointer position to active webview only if using free pointer
    auto* view = tab_manager_.active_view();
    if (view && ui_mode_ == UiMode::Browsing) {
        if (!using_dpad_) {
            float cx = cursor_.x() - viewport_x_;
            float cy = cursor_.y() - viewport_y_;
            if (cx >= 0 && cx < viewport_width_ && cy >= 0 && cy < viewport_height_) {
                view->handle_pointer_motion(static_cast<int32_t>(cx), static_cast<int32_t>(cy), last_timestamp_ms_);

                // An engine page handles its own links; the built-in document underneath is
                // stale and must not turn X into a jump to one of its old links.
                const auto* link = view->has_webkit_instance() ? nullptr : view->hit_test_link(cx, cy);
                if (link) {
                    hovered_link_url_ = link->url;
                    cursor_.set_shape(CursorShape::Pointer);
                } else {
                    hovered_link_url_.clear();
                    auto* tab_info = tab_manager_.active_tab_info();
                    if (tab_info && tab_info->url != "about:start") {
                        cursor_.set_shape(CursorShape::Arrow);
                    }
                }
            }
        }
    }
}

int BrowserApp::keyboard_keys_in_row(int row) const {
    switch (row) {
        case 0: return 14;
        case 1: return 12;
        case 2: return 12;
        case 3: return 10;
        default: return 0;
    }
}

std::string BrowserApp::keyboard_key_at(int row, int col) const {
    if (col < 0) return "";
    switch (row) {
        case 0: if (col < 14) return kKbdRow0[col]; break;
        case 1: if (col < 12) return kKbdRow1[col]; break;
        case 2: if (col < 12) return kKbdRow2[col]; break;
        case 3: if (col < 10) return kKbdRow3[col]; break;
        default: break;
    }
    return "";
}

void BrowserApp::on_keyboard_backspace() {
    if (!omnibox_text_.empty()) {
        omnibox_text_.pop_back();
    }
}

void BrowserApp::activate_keyboard_key() {
    std::string key = keyboard_key_at(keyboard_row_, keyboard_col_);
    if (key.empty()) return;

    if (key == "Bksp") {
        on_keyboard_backspace();
    } else if (key == "Clear") {
        omnibox_text_.clear();
    } else if (key == "Space") {
        omnibox_text_ += ' ';
    } else if (key == "Go") {
        submit_omnibox(omnibox_text_);
    } else if (key == "https://") {
        if (omnibox_text_.rfind("https://", 0) != 0) {
            omnibox_text_ = "https://" + omnibox_text_;
        }
    } else if (key == "www.") {
        omnibox_text_ += "www.";
    } else if (key == ".com") {
        omnibox_text_ += ".com";
    } else {
        omnibox_text_ += key;
    }
}

void BrowserApp::activate_top_bar_item() {
    switch (top_bar_index_) {
        case kTopBack: go_back(); break;
        case kTopForward: go_forward(); break;
        case kTopReload: reload_current(); break;
        case kTopOmnibox: open_omnibox(); break;
        case kTopTabs: open_tab_overview(); break;
        case kTopNewTab: new_tab(); break;
        case kTopDownloads: open_downloads(); break;
        case kTopBookmarks: toggle_bookmarks(); break;
        case kTopSettings: toggle_settings(); break;
        default: break;
    }
}

void BrowserApp::scroll_link_into_view() {
    auto* view = tab_manager_.active_view();
    if (!view) return;
    const auto& links = view->document().links();
    if (focused_link_index_ < 0 || focused_link_index_ >= static_cast<int>(links.size())) {
        return;
    }
    const auto& link = links[focused_link_index_];
    float box_top = link.box.y;
    float box_bottom = link.box.y + link.box.height;
    int32_t current_s = view->scroll_offset_y();

    float screen_y = box_top - static_cast<float>(current_s);
    if (screen_y < 120.0f) {
        view->set_scroll_offset_y(static_cast<int32_t>(box_top - 200.0f));
    } else if (screen_y > viewport_height_ - 180.0f) {
        view->set_scroll_offset_y(static_cast<int32_t>(box_bottom - viewport_height_ + 200.0f));
    }
}

void BrowserApp::on_dpad_up() {
    using_dpad_ = true;

    if (ui_mode_ == UiMode::QuickMenu) {
        menu_index_ = (menu_index_ + (-1) + kMenuItemCount) % kMenuItemCount;
        return;
    }

    if (ui_mode_ == UiMode::Downloads) {
        const int count = static_cast<int>(net::DownloadManager::instance().snapshot().size());
        downloads_index_ = std::clamp(downloads_index_ + (-1), 0, std::max(0, count - 1));
        return;
    }

    if (ui_mode_ == UiMode::TabOverview) {
        overview_move(0, -1);
        return;
    }

    if (ui_mode_ == UiMode::OmniboxEditing) {
        if (keyboard_row_ > 0) {
            keyboard_row_--;
            keyboard_col_ = std::min(keyboard_col_, keyboard_keys_in_row(keyboard_row_) - 1);
        }
        return;
    }

    if (ui_mode_ == UiMode::BookmarksDrawer) {
        if (drawer_index_ > 0) {
            drawer_index_--;
        }
        return;
    }

    if (ui_mode_ == UiMode::SettingsModal) {
        settings_index_ = (settings_index_ + kSettingsItemCount - 1) % kSettingsItemCount;
        return;
    }

    if (ui_mode_ == UiMode::HistoryDrawer) {
        history_index_ = std::max(0, history_index_ - 1);
        return;
    }

    if (ui_mode_ == UiMode::PageDialog) {
        return;
    }

    if (focus_zone_ == FocusZone::TopBar) {
        return;
    }

    if (focus_zone_ == FocusZone::Content) {
        if (send_engine_key(0x26)) { // arrow key: TV-style sites navigate with these
            return;
        }
        auto* tab_info = tab_manager_.active_tab_info();
        if (tab_info && tab_info->url == "about:start") {
            int row = speed_dial_index_ / 4;
            if (row == 0) {
                focus_zone_ = FocusZone::TopBar;
                top_bar_index_ = 3; // Focus Omnibox
            } else {
                speed_dial_index_ -= 4;
            }
        } else {
            // Web page content
            auto* view = tab_manager_.active_view();
            if (view) {
                const auto& links = view->document().links();
                if (links.empty()) {
                    if (view->scroll_offset_y() > 20) {
                        view->scroll_by_y(-180);
                    } else {
                        focus_zone_ = FocusZone::TopBar;
                        top_bar_index_ = 3;
                    }
                } else {
                    const auto& cur = links[focused_link_index_].box;
                    int best = -1;
                    float min_score = 1e9f;
                    for (int i = 0; i < static_cast<int>(links.size()); ++i) {
                        if (links[i].box.y <= cur.y - 8.0f) {
                            float dy = cur.y - links[i].box.y;
                            float dx = std::abs(cur.x - links[i].box.x);
                            float score = dy * 2.0f + dx;
                            if (score < min_score) {
                                min_score = score;
                                best = i;
                            }
                        }
                    }
                    if (best >= 0) {
                        focused_link_index_ = best;
                        hovered_link_url_ = links[best].url;
                        scroll_link_into_view();
                    } else {
                        if (view->scroll_offset_y() > 40) {
                            view->scroll_by_y(-180);
                        } else {
                            focus_zone_ = FocusZone::TopBar;
                            top_bar_index_ = 3;
                            hovered_link_url_.clear();
                        }
                    }
                }
            }
        }
    }
}

void BrowserApp::on_dpad_down() {
    using_dpad_ = true;

    if (ui_mode_ == UiMode::QuickMenu) {
        menu_index_ = (menu_index_ + (1) + kMenuItemCount) % kMenuItemCount;
        return;
    }

    if (ui_mode_ == UiMode::Downloads) {
        const int count = static_cast<int>(net::DownloadManager::instance().snapshot().size());
        downloads_index_ = std::clamp(downloads_index_ + (1), 0, std::max(0, count - 1));
        return;
    }

    if (ui_mode_ == UiMode::TabOverview) {
        overview_move(0, 1);
        return;
    }

    if (ui_mode_ == UiMode::OmniboxEditing) {
        if (keyboard_row_ < 3) {
            keyboard_row_++;
            keyboard_col_ = std::min(keyboard_col_, keyboard_keys_in_row(keyboard_row_) - 1);
        }
        return;
    }

    if (ui_mode_ == UiMode::BookmarksDrawer) {
        if (drawer_index_ + 1 < static_cast<int>(bookmarks_.items().size())) {
            drawer_index_++;
        }
        return;
    }

    if (ui_mode_ == UiMode::SettingsModal) {
        settings_index_ = (settings_index_ + 1) % kSettingsItemCount;
        return;
    }

    if (ui_mode_ == UiMode::HistoryDrawer) {
        history_index_ = std::clamp(history_index_ + 1, 0, std::max(0, static_cast<int>(history_.entries().size()) - 1));
        return;
    }

    if (ui_mode_ == UiMode::PageDialog) {
        return;
    }

    if (focus_zone_ == FocusZone::TopBar) {
        focus_zone_ = FocusZone::Content;
        auto* tab_info = tab_manager_.active_tab_info();
        if (tab_info && tab_info->url == "about:start") {
            speed_dial_index_ = std::clamp(speed_dial_index_, 0, static_cast<int>(bookmarks_.items().size()) - 1);
        } else {
            auto* view = tab_manager_.active_view();
            if (view && !view->document().links().empty()) {
                focused_link_index_ = 0;
                hovered_link_url_ = view->document().links()[0].url;
                scroll_link_into_view();
            }
        }
        return;
    }

    if (focus_zone_ == FocusZone::Content) {
        if (send_engine_key(0x28)) { // arrow key: TV-style sites navigate with these
            return;
        }
        auto* tab_info = tab_manager_.active_tab_info();
        if (tab_info && tab_info->url == "about:start") {
            int next = speed_dial_index_ + 4;
            int total = static_cast<int>(std::min<size_t>(bookmarks_.items().size(), 8));
            if (next < total) {
                speed_dial_index_ = next;
            }
        } else {
            auto* view = tab_manager_.active_view();
            if (view) {
                const auto& links = view->document().links();
                if (links.empty()) {
                    view->scroll_by_y(180);
                } else {
                    const auto& cur = links[focused_link_index_].box;
                    int best = -1;
                    float min_score = 1e9f;
                    for (int i = 0; i < static_cast<int>(links.size()); ++i) {
                        if (links[i].box.y >= cur.y + 8.0f) {
                            float dy = links[i].box.y - cur.y;
                            float dx = std::abs(links[i].box.x - cur.x);
                            float score = dy * 2.0f + dx;
                            if (score < min_score) {
                                min_score = score;
                                best = i;
                            }
                        }
                    }
                    if (best >= 0) {
                        focused_link_index_ = best;
                        hovered_link_url_ = links[best].url;
                        scroll_link_into_view();
                    } else {
                        view->scroll_by_y(180);
                    }
                }
            }
        }
    }
}

void BrowserApp::on_dpad_left() {
    using_dpad_ = true;

    if (ui_mode_ == UiMode::TabOverview) {
        overview_move(-1, 0);
        return;
    }

    if (ui_mode_ == UiMode::OmniboxEditing) {
        if (keyboard_col_ > 0) {
            keyboard_col_--;
        }
        return;
    }

    if (ui_mode_ == UiMode::PageDialog) {
        dialog_button_ = std::max(0, dialog_button_ - 1);
        return;
    }

    if (ui_mode_ != UiMode::Browsing) {
        return;
    }

    if (focus_zone_ == FocusZone::TopBar) {
        if (top_bar_index_ > 0) {
            top_bar_index_--;
        }
        return;
    }

    if (focus_zone_ == FocusZone::Content) {
        if (send_engine_key(0x25)) { // arrow key: TV-style sites navigate with these
            return;
        }
        auto* tab_info = tab_manager_.active_tab_info();
        if (tab_info && tab_info->url == "about:start") {
            if ((speed_dial_index_ % 4) > 0) {
                speed_dial_index_--;
            }
        } else {
            auto* view = tab_manager_.active_view();
            if (view) {
                const auto& links = view->document().links();
                if (!links.empty()) {
                    const auto& cur = links[focused_link_index_].box;
                    int best = -1;
                    float min_dx = 1e9f;
                    for (int i = 0; i < static_cast<int>(links.size()); ++i) {
                        if (links[i].box.x + links[i].box.width < cur.x + 5.0f &&
                            std::abs(links[i].box.y - cur.y) < 35.0f) {
                            float dx = cur.x - links[i].box.x;
                            if (dx < min_dx) {
                                min_dx = dx;
                                best = i;
                            }
                        }
                    }
                    if (best >= 0) {
                        focused_link_index_ = best;
                        hovered_link_url_ = links[best].url;
                        scroll_link_into_view();
                    } else if (focused_link_index_ > 0) {
                        focused_link_index_--;
                        hovered_link_url_ = links[focused_link_index_].url;
                        scroll_link_into_view();
                    }
                }
            }
        }
    }
}

void BrowserApp::on_dpad_right() {
    using_dpad_ = true;

    if (ui_mode_ == UiMode::TabOverview) {
        overview_move(1, 0);
        return;
    }

    if (ui_mode_ == UiMode::OmniboxEditing) {
        if (keyboard_col_ + 1 < keyboard_keys_in_row(keyboard_row_)) {
            keyboard_col_++;
        }
        return;
    }

    if (ui_mode_ == UiMode::PageDialog) {
        dialog_button_ = std::min(dialog_button_count() - 1, dialog_button_ + 1);
        return;
    }

    if (ui_mode_ != UiMode::Browsing) {
        return;
    }

    if (focus_zone_ == FocusZone::TopBar) {
        if (top_bar_index_ < kTopSettings) {
            top_bar_index_++;
        }
        return;
    }

    if (focus_zone_ == FocusZone::Content) {
        if (send_engine_key(0x27)) { // arrow key: TV-style sites navigate with these
            return;
        }
        auto* tab_info = tab_manager_.active_tab_info();
        if (tab_info && tab_info->url == "about:start") {
            int total = static_cast<int>(std::min<size_t>(bookmarks_.items().size(), 8));
            if ((speed_dial_index_ % 4) < 3 && speed_dial_index_ + 1 < total) {
                speed_dial_index_++;
            }
        } else {
            auto* view = tab_manager_.active_view();
            if (view) {
                const auto& links = view->document().links();
                if (!links.empty()) {
                    const auto& cur = links[focused_link_index_].box;
                    int best = -1;
                    float min_dx = 1e9f;
                    for (int i = 0; i < static_cast<int>(links.size()); ++i) {
                        if (links[i].box.x > cur.x + cur.width - 5.0f &&
                            std::abs(links[i].box.y - cur.y) < 35.0f) {
                            float dx = links[i].box.x - cur.x;
                            if (dx < min_dx) {
                                min_dx = dx;
                                best = i;
                            }
                        }
                    }
                    if (best >= 0) {
                        focused_link_index_ = best;
                        hovered_link_url_ = links[best].url;
                        scroll_link_into_view();
                    } else if (focused_link_index_ + 1 < static_cast<int>(links.size())) {
                        focused_link_index_++;
                        hovered_link_url_ = links[focused_link_index_].url;
                        scroll_link_into_view();
                    }
                }
            }
        }
    }
}

void BrowserApp::on_button_cross_pressed() {
    if (ui_mode_ == UiMode::PageDialog) {
        answer_page_dialog(dialog_button_ == 0);
        return;
    }
    if (ui_mode_ == UiMode::SettingsModal) {
        activate_settings_item();
        return;
    }
    if (ui_mode_ == UiMode::HistoryDrawer) {
        const auto& entries = history_.entries();
        if (history_index_ >= 0 && history_index_ < static_cast<int>(entries.size())) {
            const std::string url = entries[static_cast<size_t>(history_index_)].url;
            close_overlays();
            navigate_to(url);
        }
        return;
    }
    if (ui_mode_ == UiMode::QuickMenu) {
        activate_menu_item();
        return;
    }
    if (ui_mode_ == UiMode::TabOverview) {
        overview_activate();
        return;
    }
    cursor_.set_button_down(true);

    if (using_dpad_) {
        if (ui_mode_ == UiMode::OmniboxEditing) {
            activate_keyboard_key();
            return;
        }
        if (ui_mode_ == UiMode::BookmarksDrawer) {
            const auto& items = bookmarks_.items();
            if (drawer_index_ >= 0 && drawer_index_ < static_cast<int>(items.size())) {
                navigate_to(items[drawer_index_].url);
                close_overlays();
            }
            return;
        }
        if (ui_mode_ == UiMode::SettingsModal) {
            return;
        }
        if (focus_zone_ == FocusZone::TopBar) {
            activate_top_bar_item();
            return;
        }
        if (focus_zone_ == FocusZone::Content) {
            if (send_engine_key(0x0D)) { // Enter on the page's focused element
                return;
            }
            auto* tab_info = tab_manager_.active_tab_info();
            if (tab_info && tab_info->url == "about:start") {
                const auto& items = bookmarks_.items();
                if (speed_dial_index_ >= 0 && speed_dial_index_ < static_cast<int>(items.size())) {
                    navigate_to(items[speed_dial_index_].url);
                }
            } else {
                auto* view = tab_manager_.active_view();
                if (view) {
                    const auto& links = view->document().links();
                    if (focused_link_index_ >= 0 && focused_link_index_ < static_cast<int>(links.size())) {
                        navigate_to(links[focused_link_index_].url);
                    }
                }
            }
            return;
        }
    } else {
        // Free pointer mode
        if (ui_mode_ == UiMode::Browsing) {
            auto* view = tab_manager_.active_view();
            if (view) {
                float cx = cursor_.x() - viewport_x_;
                float cy = cursor_.y() - viewport_y_;
                if (cy < 0) {
                    const int item = top_bar_item_at(cursor_.x(), cursor_.y());
                    if (item >= 0) {
                        top_bar_index_ = item;
                        activate_top_bar_item();
                    }
                } else {
                    if (!hovered_link_url_.empty() && !view->has_webkit_instance()) {
                        navigate_to(hovered_link_url_);
                    } else {
                        view->handle_pointer_button(static_cast<int32_t>(cx), static_cast<int32_t>(cy), 1, true, last_timestamp_ms_);
                    }
                }
            }
        }
    }
}

void BrowserApp::on_button_cross_released() {
    cursor_.set_button_down(false);

    if (!using_dpad_ && ui_mode_ == UiMode::Browsing) {
        auto* view = tab_manager_.active_view();
        if (view) {
            float cx = cursor_.x() - viewport_x_;
            float cy = cursor_.y() - viewport_y_;
            if (cy >= 0) {
                view->handle_pointer_button(static_cast<int32_t>(cx), static_cast<int32_t>(cy), 1, false, last_timestamp_ms_);
            }
        }
    }
}

void BrowserApp::on_button_circle_pressed() {
    if (ui_mode_ == UiMode::PageDialog) {
        answer_page_dialog(false);  // Cancel / Stay; an alert just closes
        return;
    }
    if (fullscreen_) {
        if (auto* view = tab_manager_.active_view()) view->exit_fullscreen();
        return;
    }
    if (ui_mode_ != UiMode::Browsing) {
        close_overlays();
    } else {
        back_in_page();
    }
}

// Circle: the page's own Back first. TV-style apps get Escape (their remote's Back);
// otherwise back in the tab's history; a popup with no history closes, returning
// to the page that opened it.
void BrowserApp::back_in_page() {
    auto* tab = tab_manager_.active_tab_info();
    auto* view = tab_manager_.active_view();
    if (!tab || !view) return;
    if (view->has_webkit_instance() && Navigation::is_tv_app(tab->url)) {
        send_engine_key(0x1B);
        return;
    }
    if (view->can_go_back()) {
        view->go_back();
        return;
    }
    if (tab->opened_by_page && tab_manager_.count() > 1) {
        tab_manager_.close_tab(tab_manager_.active_index());
        show_toast("Closed the page's window");
        return;
    }
    show_toast("No previous page");
}

void BrowserApp::on_button_triangle_pressed() {
    if (ui_mode_ == UiMode::PageDialog) return;
    if (ui_mode_ == UiMode::TabOverview) {
        new_tab();
    } else if (ui_mode_ == UiMode::Browsing) {
        open_omnibox();
    } else if (ui_mode_ == UiMode::OmniboxEditing) {
        submit_omnibox(omnibox_text_);
    }
}

void BrowserApp::on_button_square_pressed() {
    if (ui_mode_ == UiMode::Downloads) {
        // Cancel a running download; drop a finished/failed one from the list.
        auto& dm = net::DownloadManager::instance();
        const auto list = dm.snapshot();
        if (downloads_index_ < static_cast<int>(list.size())) {
            const auto& d = list[static_cast<size_t>(downloads_index_)];
            if (d.state == net::DownloadState::Running) {
                dm.cancel(d.id);
            } else {
                dm.remove(d.id);
                downloads_index_ = std::max(0, std::min(downloads_index_, static_cast<int>(list.size()) - 2));
            }
        }
    } else if (ui_mode_ == UiMode::TabOverview) {
        overview_close_selected();
    } else if (ui_mode_ == UiMode::HistoryDrawer) {
        if (history_.remove(static_cast<size_t>(std::max(0, history_index_)))) {
            history_dirty_ = true;
            history_index_ = std::clamp(history_index_, 0, std::max(0, static_cast<int>(history_.entries().size()) - 1));
        }
    } else if (ui_mode_ == UiMode::PageDialog || ui_mode_ == UiMode::SettingsModal) {
        // the dialog keeps the controller
    } else if (ui_mode_ == UiMode::OmniboxEditing) {
        on_keyboard_backspace();
    } else {
        toggle_bookmarks();
    }
}

void BrowserApp::on_button_options_pressed() {
    if (ui_mode_ == UiMode::PageDialog) return;  // the page waits for an answer
    if (ui_mode_ == UiMode::QuickMenu) {
        close_overlays();
    } else if (ui_mode_ == UiMode::Browsing) {
        ui_mode_ = UiMode::QuickMenu;
        menu_index_ = 0;
        using_dpad_ = true;
    } else {
        close_overlays();  // Options always gets you back to the page
    }
}

std::string BrowserApp::menu_label(int index) const {
    switch (static_cast<MenuItem>(index)) {
        case MenuItem::Downloads: {
            const int running = net::DownloadManager::instance().running_count();
            return running > 0 ? "Downloads (" + std::to_string(running) + " active)" : "Downloads";
        }
        case MenuItem::Tabs: return "Tabs (" + std::to_string(tab_manager_.count()) + ")";
        case MenuItem::NewTab: return "New tab";
        case MenuItem::Bookmarks: return "Bookmarks";
        case MenuItem::History: return "History";
        case MenuItem::ToggleBookmark: {
            const auto* tab = tab_manager_.active_tab_info();
            if (!current_page_bookmarkable()) return "Bookmark this page";
            return bookmarks_.is_bookmarked(tab->url) ? "Remove bookmark" : "Bookmark this page";
        }
        case MenuItem::Settings: return "Settings";
    }
    return "";
}

bool BrowserApp::current_page_bookmarkable() const {
    const auto* tab = tab_manager_.active_tab_info();
    return tab && tab->url.rfind("about:", 0) != 0 && !tab->url.empty();
}

void BrowserApp::activate_menu_item() {
    const auto item = static_cast<MenuItem>(menu_index_);
    close_overlays();
    switch (item) {
        case MenuItem::Downloads: open_downloads(); break;
        case MenuItem::Tabs: open_tab_overview(); break;
        case MenuItem::NewTab: new_tab(); break;
        case MenuItem::Bookmarks: toggle_bookmarks(); break;
        case MenuItem::History: open_history(); break;
        case MenuItem::ToggleBookmark: toggle_current_bookmark(); break;
        case MenuItem::Settings: toggle_settings(); break;
    }
}

void BrowserApp::toggle_current_bookmark() {
    if (!current_page_bookmarkable()) {
        show_toast("Nothing to bookmark on this page");
        return;
    }
    const auto* tab = tab_manager_.active_tab_info();
    if (bookmarks_.is_bookmarked(tab->url)) {
        bookmarks_.remove_bookmark(tab->url);
        show_toast("Bookmark removed");
    } else {
        bookmarks_.add_bookmark(tab->title.empty() ? tab->url : tab->title, tab->url);
        show_toast("Bookmarked: " + (tab->title.empty() ? tab->url : tab->title));
    }
    bookmarks_.save_to_file(kBookmarksPath);
}

void BrowserApp::on_button_l1_pressed() {
    if (ui_mode_ == UiMode::TabOverview) {
        overview_move(-1, 0);
    } else if (ui_mode_ == UiMode::Browsing) {
        switch_tab(false);
    }
}

void BrowserApp::on_button_r1_pressed() {
    if (ui_mode_ == UiMode::TabOverview) {
        overview_move(1, 0);
    } else if (ui_mode_ == UiMode::Browsing) {
        switch_tab(true);
    }
}

void BrowserApp::switch_tab(bool next) {
    if (tab_manager_.count() < 2) {
        show_toast("Only one tab open - press the touchpad for tabs");
        return;
    }
    if (next) {
        tab_manager_.next_tab();
    } else {
        tab_manager_.prev_tab();
    }
    const TabInfo* tab = tab_manager_.active_tab_info();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Tab %zu of %zu", tab_manager_.active_index() + 1, tab_manager_.count());
    show_toast(std::string(buf) + (tab ? "  -  " + tab->title : std::string()));
    hovered_link_url_.clear();
}

void BrowserApp::on_touchpad_pressed() {
    if (ui_mode_ == UiMode::TabOverview) {
        close_overlays();
    } else if (ui_mode_ == UiMode::Browsing) {
        open_tab_overview();
    }
}

void BrowserApp::open_tab_overview() {
    ui_mode_ = UiMode::TabOverview;
    tab_overview_index_ = static_cast<int>(tab_manager_.active_index());
    using_dpad_ = true;
}

int BrowserApp::tab_overview_count() const {
    const int tabs = static_cast<int>(tab_manager_.count());
    return tabs < static_cast<int>(kMaxTabs) ? tabs + 1 : tabs;
}

void BrowserApp::new_tab() {
    if (tab_manager_.count() >= kMaxTabs) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Tab limit reached (%zu) - close one first", kMaxTabs);
        show_toast(buf);
        return;
    }
    tab_manager_.create_tab("about:start");
    close_overlays();
    speed_dial_index_ = 0;
    hovered_link_url_.clear();
    char buf[48];
    std::snprintf(buf, sizeof(buf), "New tab (%zu of %zu)", tab_manager_.active_index() + 1, tab_manager_.count());
    show_toast(buf);
}

void BrowserApp::overview_move(int dx, int dy) {
    const int count = tab_overview_count();
    const int i = tab_overview_index_ + dx + dy * kOverviewColumns;
    if (dy != 0 && (i < 0 || i >= count)) {
        return;  // no card in that row
    }
    tab_overview_index_ = std::clamp(i, 0, count - 1);
}

void BrowserApp::overview_activate() {
    if (tab_overview_index_ >= static_cast<int>(tab_manager_.count())) {
        new_tab();
        return;
    }
    tab_manager_.select_tab(static_cast<size_t>(tab_overview_index_));
    hovered_link_url_.clear();
    close_overlays();
}

void BrowserApp::overview_close_selected() {
    if (tab_overview_index_ >= static_cast<int>(tab_manager_.count())) {
        return;  // the "New tab" card
    }
    tab_manager_.close_tab(static_cast<size_t>(tab_overview_index_));
    tab_overview_index_ = std::min(tab_overview_index_, tab_overview_count() - 1);
    show_toast("Tab closed");
}

void BrowserApp::open_downloads() {
    ui_mode_ = UiMode::Downloads;
    downloads_index_ = 0;
    using_dpad_ = true;
}

int BrowserApp::top_bar_item_at(float x, float y) {
    // Same rectangles as BrowserUi::render_header / render_tab_strip.
    struct Hit { float x, w; int item; };
    static const Hit kHits[] = {
        {16.0f, 40.0f, kTopBack},       {64.0f, 40.0f, kTopForward},  {112.0f, 40.0f, kTopReload},
        {170.0f, 820.0f, kTopOmnibox},  {1010.0f, 300.0f, kTopTabs},  {1320.0f, 40.0f, kTopNewTab},
        {1370.0f, 40.0f, kTopDownloads}, {1652.0f, 42.0f, kTopBookmarks}, {1707.0f, 42.0f, kTopSettings},  // EVO: were 1780, 1835 (1920 wide)
    };
    if (y < 14.0f || y > 58.0f) return -1;
    for (const auto& h : kHits) {
        if (x >= h.x && x <= h.x + h.w) return h.item;
    }
    return -1;
}

void BrowserApp::show_toast(const std::string& text) {
    toast_text_ = text;
    toast_time_ = 2.0f;
}

float BrowserApp::toast_alpha() const {
    return std::clamp(toast_time_ / 0.4f, 0.0f, 1.0f);  // fades out over the last 0.4 s
}

void BrowserApp::update_left_stick(float x, float y, float dt) {
    if (std::hypot(x, y) > 0.4f) {
        using_dpad_ = false;
        if (ui_mode_ == UiMode::Browsing) {
            cursor_.update_stick(x, y, dt);
        }
    }
}

void BrowserApp::update_right_stick(float x, float y, float dt) {
    if (ui_mode_ == UiMode::Browsing) {
        auto* view = tab_manager_.active_view();
        if (view && (std::abs(x) > 0.15f || std::abs(y) > 0.15f)) {
            // Scroll deltas are positive towards the top/left (PageEngine::scroll); the
            // stick is positive down/right, so negate to move the page with the stick.
            int32_t dx = static_cast<int32_t>(-x * 600.0f * dt);
            int32_t dy = static_cast<int32_t>(-y * 800.0f * dt);
            view->handle_scroll(static_cast<int32_t>(cursor_.x() - viewport_x_),
                               static_cast<int32_t>(cursor_.y() - viewport_y_), dx, dy, last_timestamp_ms_);
        }
    }
}

void BrowserApp::update_triggers(float l2, float r2, float dt) {
    if (ui_mode_ == UiMode::Browsing) {
        auto* view = tab_manager_.active_view();
        if (view) {
            // R2 scrolls down: a positive delta scrolls towards the top.
            float scroll_delta = (l2 - r2) * 1200.0f * dt;
            if (std::abs(scroll_delta) > 0.5f) {
                view->handle_scroll(static_cast<int32_t>(cursor_.x() - viewport_x_),
                                   static_cast<int32_t>(cursor_.y() - viewport_y_), 0,
                                   static_cast<int32_t>(scroll_delta), last_timestamp_ms_);
            }
        }
    }
}

void BrowserApp::open_omnibox() {
    auto* tab = tab_manager_.active_tab_info();
    if (tab && !tab->url.empty() && tab->url != "about:start") {
        omnibox_text_ = tab->url;
    } else {
        omnibox_text_.clear();
    }
    ui_mode_ = UiMode::OmniboxEditing;
    focus_zone_ = FocusZone::Keyboard;
    keyboard_row_ = 1;
    keyboard_col_ = 0;
}

void BrowserApp::close_omnibox() {
    ui_mode_ = UiMode::Browsing;
    focus_zone_ = FocusZone::Content;
}

void BrowserApp::submit_omnibox(const std::string& text) {
    close_omnibox();
    if (!text.empty()) {
        std::string resolved = Navigation::resolve_input(text, settings_.search_engine);
        navigate_to(resolved);
    }
}

void BrowserApp::navigate_to(const std::string& url) {
    std::string target = url;
    BROWSER_LOG("[BROWSER] navigate_to -> %s", target.c_str());
    if (target.rfind("webkit:", 0) == 0) {
        std::string native_target = target.substr(7);
        if (native_target.empty()) native_target = "https://duckduckgo.com";
        open_native_browser(native_target);
        return;
    }

    focused_link_index_ = 0;
    hovered_link_url_.clear();
    focus_zone_ = FocusZone::Content;

    auto* tab = tab_manager_.active_tab_info();
    if (tab) {
        tab->url = target;
        tab->title = Navigation::extract_host(target);
        if (tab->title.empty()) tab->title = target;
    }
    omnibox_text_ = target;

    auto* view = tab_manager_.active_view();
    if (view) {
        view->load_uri(target);
    }
}

void BrowserApp::reload_current() {
    auto* view = tab_manager_.active_view();
    if (view) {
        view->reload();
    }
}

void BrowserApp::zoom_step(float delta) {
    // Round to a 10% step so repeated presses land on 100% again.
    const float zoom = std::round((tab_manager_.zoom_level() + delta) * 10.0f) / 10.0f;
    tab_manager_.set_zoom_level(zoom);
    settings_.default_zoom = tab_manager_.zoom_level();
    settings_.save_to_file(kSettingsPath);
    BROWSER_LOG("[BROWSER] zoom %.0f%%", settings_.default_zoom * 100.0f);
}

// D-pad and X on an engine-rendered page go to the page as key presses: the
// built-in renderer's link list is empty there.
bool BrowserApp::send_engine_key(uint32_t vk) {
    auto* tab = tab_manager_.active_tab_info();
    if (tab && tab->url == "about:start") return false;
    auto* view = tab_manager_.active_view();
    if (!view || !view->has_webkit_instance()) return false;
    view->handle_key(vk, true, 0, last_timestamp_ms_);
    view->handle_key(vk, false, 0, last_timestamp_ms_);
    return true;
}

void BrowserApp::go_back() {
    auto* view = tab_manager_.active_view();
    if (view && view->can_go_back()) {
        view->go_back();
    }
}

void BrowserApp::go_forward() {
    auto* view = tab_manager_.active_view();
    if (view && view->can_go_forward()) {
        view->go_forward();
    }
}

void BrowserApp::toggle_bookmarks() {
    if (ui_mode_ == UiMode::BookmarksDrawer) {
        close_overlays();
    } else {
        ui_mode_ = UiMode::BookmarksDrawer;
        focus_zone_ = FocusZone::Drawer;
        drawer_index_ = 0;
    }
}

void BrowserApp::toggle_settings() {
    if (ui_mode_ == UiMode::SettingsModal) {
        close_overlays();
    } else {
        ui_mode_ = UiMode::SettingsModal;
        focus_zone_ = FocusZone::Settings;
        settings_index_ = 0;
        using_dpad_ = true;
    }
}

void BrowserApp::open_history() {
    ui_mode_ = UiMode::HistoryDrawer;
    focus_zone_ = FocusZone::Drawer;
    history_index_ = 0;
    using_dpad_ = true;
}

void BrowserApp::close_overlays() {
    ui_mode_ = UiMode::Browsing;
    focus_zone_ = FocusZone::Content;
}

bool BrowserApp::open_native_browser(const std::string& url) {
    std::string target = url;
    if (target.empty()) {
        auto* tab = tab_manager_.active_tab_info();
        if (tab && !tab->url.empty() && tab->url != "about:start" && tab->url != "about:blank") {
            target = tab->url;
        } else {
            target = "https://duckduckgo.com";
        }
    }
    bool ok = ps5::WebKitDialog::instance().open(target, ps5::DialogMode::Custom);
    if (!ok) {
        auto* view = tab_manager_.active_view();
        if (view) {
            view->load_uri(target);
        }
    }
    return ok;
}

bool BrowserApp::is_native_browser_active() const {
    return ps5::WebKitDialog::instance().is_running();
}

void BrowserApp::close_native_browser() {
    ps5::WebKitDialog::instance().close();
}

bool BrowserApp::is_native_webkit_available() const {
    return ps5::WebKitDialog::instance().is_available();
}

// --- Pages that open, close or question the browser ---------------------------

void BrowserApp::adopt_new_windows() {
    for (auto& engine : web::NewWindowQueue::instance().take()) {
        if (tab_manager_.count() >= kMaxTabs) {
            // Dropping the engine closes the window the page just opened.
            show_toast("Tab limit reached - the page's new window was not opened");
            continue;
        }
        if (tab_manager_.adopt_tab(std::move(engine))) {
            BROWSER_LOG("[BROWSER] new window opened as tab %zu", tab_manager_.active_index() + 1);
            hovered_link_url_.clear();
            focus_zone_ = FocusZone::Content;
            show_toast("Opened in a new tab - Circle closes it");
        }
    }
}

void BrowserApp::close_tabs_that_asked() {
    for (size_t i = tab_manager_.count(); i-- > 0;) {
        auto& tab = tab_manager_.tabs()[i];
        if (!tab.view || !tab.view->take_close_request()) continue;
        BROWSER_LOG("[BROWSER] tab %zu closed by its page (%s)", i + 1, tab.url.c_str());
        tab_manager_.close_tab(i);
        show_toast("The page closed its window");
    }
}

void BrowserApp::handle_page_dialogs() {
    // Every tab shares one web process, so a dialog nobody sees would freeze them all:
    // background tabs get the answer a closed dialog gives.
    auto& tabs = tab_manager_.tabs();
    for (size_t i = 0; i < tabs.size(); ++i) {
        if (i == tab_manager_.active_index() || !tabs[i].view) continue;
        while (tabs[i].view->has_pending_dialog()) {
            const bool leave = tabs[i].view->pending_dialog().dialog == web::EngineEvent::DialogKind::BeforeUnload;
            tabs[i].view->answer_dialog(leave);
        }
    }
    if (page_dialog()) {
        if (ui_mode_ != UiMode::PageDialog) {
            ui_mode_ = UiMode::PageDialog;
            dialog_button_ = 0;
            using_dpad_ = true;
        }
    } else if (ui_mode_ == UiMode::PageDialog) {
        close_overlays();
    }
}

const web::EngineEvent* BrowserApp::page_dialog() const {
    const auto* view = tab_manager_.active_view();
    return view && view->has_pending_dialog() ? &view->pending_dialog() : nullptr;
}

int BrowserApp::dialog_button_count() const {
    const auto* d = page_dialog();
    return d && d->dialog != web::EngineEvent::DialogKind::Alert ? 2 : 1;
}

void BrowserApp::answer_page_dialog(bool accepted) {
    auto* view = tab_manager_.active_view();
    if (!view || !view->has_pending_dialog()) return;
    // A prompt answers with its default text: typing into it needs the on-screen keyboard.
    view->answer_dialog(accepted, view->pending_dialog().default_text);
    dialog_button_ = 0;
    if (!view->has_pending_dialog()) close_overlays();
}

void BrowserApp::sync_fullscreen() {
    auto& tabs = tab_manager_.tabs();
    for (size_t i = 0; i < tabs.size(); ++i) {
        if (i != tab_manager_.active_index() && tabs[i].view && tabs[i].view->is_fullscreen()) {
            tabs[i].view->exit_fullscreen();
        }
    }
    const auto* view = tab_manager_.active_view();
    const bool want = view && view->is_fullscreen();
    if (want != fullscreen_) set_fullscreen(want);
}

void BrowserApp::set_fullscreen(bool fullscreen) {
    BROWSER_LOG("[BROWSER] fullscreen %s", fullscreen ? "on" : "off");
    fullscreen_ = fullscreen;
    viewport_x_ = 0.0f;
    viewport_y_ = fullscreen ? 0.0f : 72.0f;
    viewport_width_ = static_cast<float>(screen_width_);
    viewport_height_ = fullscreen ? static_cast<float>(screen_height_)
                                  : static_cast<float>(screen_height_) - viewport_y_ - kFooterHeight;
    tab_manager_.resize(static_cast<uint32_t>(viewport_width_), static_cast<uint32_t>(viewport_height_));
    if (fullscreen) {
        if (ui_mode_ != UiMode::PageDialog) close_overlays();
        focus_zone_ = FocusZone::Content;
        show_toast("Fullscreen - Circle to exit");
    }
}

// --- History, session, settings ---------------------------------------------

void BrowserApp::record_history(float dt) {
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    for (const auto& t : tab_manager_.tabs()) {
        // Mid-load the title can still be the previous page's: record finished pages only.
        if (!t.view || t.view->showing_engine_failure() || t.view->is_asleep() || t.is_loading) continue;
        auto& last = recorded_[t.id];
        if (last.first == t.url && last.second == t.title) continue;
        last = {t.url, t.title};
        if (history_.record(t.url, t.title, now)) history_dirty_ = true;
    }
    if (recorded_.size() > tab_manager_.count() + 16) {
        std::map<uint32_t, std::pair<std::string, std::string>> live;
        for (const auto& t : tab_manager_.tabs()) {
            auto it = recorded_.find(t.id);
            if (it != recorded_.end()) live.insert(*it);
        }
        recorded_.swap(live);
    }
    history_save_timer_ += dt;
    if (history_dirty_ && history_save_timer_ >= 5.0f) {
        history_save_timer_ = 0.0f;
        if (history_.save_to_file(kHistoryPath)) history_dirty_ = false;
    }
}

// Written whenever the tabs change (the app is usually left with the PS button,
// which gives it no chance to save on the way out).
void BrowserApp::save_session(float dt, bool force) {
    session_save_timer_ += dt;
    if (!force && session_save_timer_ < 2.0f) return;
    session_save_timer_ = 0.0f;
    TabSession session;
    for (const auto& t : tab_manager_.tabs()) {
        session.urls.push_back(is_restorable_url(t.url) ? t.url : "about:start");
    }
    session.active = tab_manager_.active_index();
    const std::string text = session.encode();
    if (text == saved_session_) return;
    if (session.save_to_file(kSessionPath)) saved_session_ = text;
}

void BrowserApp::restore_session() {
    TabSession session;
    if (!session.load_from_file(kSessionPath)) return;
    if (session.urls.size() == 1 && !is_restorable_url(session.urls[0])) return;
    const size_t n = std::min(session.urls.size(), kMaxTabs);
    BROWSER_LOG("[BROWSER] restoring %zu tabs (active %zu)", n, session.active + 1);
    for (size_t i = 0; i < n; ++i) {
        const std::string& url = session.urls[i];
        tab_manager_.create_background_tab(is_restorable_url(url) ? url : "about:start");
    }
    // Select first so only the active tab loads, then drop the initial start tab.
    tab_manager_.select_tab(1 + std::min(session.active, n - 1));
    tab_manager_.close_tab(0);
    if (const auto* tab = tab_manager_.active_tab_info()) omnibox_text_ = tab->url;
}

std::string BrowserApp::settings_label(int index) const {
    switch (static_cast<SettingsItem>(index)) {
        case SettingsItem::SearchEngine:
            return std::string("Search engine: ") + Navigation::search_engine_name(settings_.search_engine);
        case SettingsItem::RestoreTabs:
            return std::string("Reopen tabs at start: ") + (settings_.restore_tabs ? "On" : "Off");
        case SettingsItem::ClearHistory:
            return "Clear history (" + std::to_string(history_.entries().size()) + " pages)";
        case SettingsItem::ClearSiteData: return "Clear cookies, site data and cache";
        case SettingsItem::Close: return "Close";
    }
    return "";
}

void BrowserApp::activate_settings_item() {
    switch (static_cast<SettingsItem>(settings_index_)) {
        case SettingsItem::SearchEngine:
            settings_.search_engine =
                static_cast<SearchEngine>((static_cast<int>(settings_.search_engine) + 1) % kSearchEngineCount);
            break;
        case SettingsItem::RestoreTabs:
            settings_.restore_tabs = !settings_.restore_tabs;
            break;
        case SettingsItem::ClearHistory:
            history_.clear();
            history_.save_to_file(kHistoryPath);
            history_dirty_ = false;
            history_index_ = 0;
            show_toast("History cleared");
            return;
        case SettingsItem::ClearSiteData:
            if (auto& clear = web::clear_website_data_handler()) {
                clear();
                show_toast("Cookies, site data and cache cleared - sites will sign you out");
            } else {
                show_toast("Nothing to clear: no web engine is running");
            }
            return;
        case SettingsItem::Close:
            close_overlays();
            return;
    }
    settings_.save_to_file(kSettingsPath);
}

} // namespace prospero::browser
