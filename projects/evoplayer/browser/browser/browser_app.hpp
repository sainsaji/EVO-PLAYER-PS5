// Prospero Browser - WPE WebKit Port for PlayStation 5
// Browser Application Core Coordinator.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later

#pragma once

#include "tab_manager.hpp"
#include "virtual_cursor.hpp"
#include "navigation.hpp"
#include "bookmarks.hpp"
#include "browser_settings.hpp"
#include "history.hpp"
#include <map>
#include <string>
#include <memory>
#include <utility>

namespace prospero::browser {

enum class UiMode {
    Browsing,           // Interacting with webpage
    OmniboxEditing,     // Entering URL / search query with keyboard
    BookmarksDrawer,    // Bookmarks side panel open
    HistoryDrawer,      // History panel open
    SettingsModal,      // Settings dialog open
    TabOverview,        // Grid of open tabs (touchpad)
    Downloads,          // Download list
    QuickMenu,          // Options menu: downloads, tabs, bookmarks, settings
    PageDialog,         // The page's alert/confirm/prompt/beforeunload
};

// Rows of the settings dialog, top to bottom.
// No user-agent row: Sony's web process ignores WKPageSetCustomUserAgent (see CLAUDE.md).
enum class SettingsItem { SearchEngine, RestoreTabs, ClearHistory, ClearSiteData, Close };
constexpr int kSettingsItemCount = 5;

// Header items, left to right, as reached with the D-pad in the top bar.
enum TopBarItem : int {
    kTopBack,
    kTopForward,
    kTopReload,
    kTopOmnibox,
    kTopTabs,      // opens the tab overview
    kTopNewTab,
    kTopDownloads,
    kTopBookmarks,
    kTopSettings,
};

enum class FocusZone {
    TopBar,             // Top bar navigation (Back, Forward, Reload, Omnibox, Tabs, Bookmarks, Settings)
    Content,            // Main viewport (Speed Dial cards or Web Page links)
    Drawer,             // Bookmarks drawer
    Settings,           // Settings modal
    Keyboard,           // On-screen virtual keyboard overlay
};

class BrowserApp {
public:
    BrowserApp();
    ~BrowserApp();

    bool initialize(uint32_t screen_width, uint32_t screen_height);
    void resize(uint32_t screen_width, uint32_t screen_height);

    // Frame update with controller state and dt
    void update(float dt);

    // Controller button triggers
    void on_button_cross_pressed();
    void on_button_cross_released();
    void on_button_circle_pressed();
    void on_button_triangle_pressed();
    void on_button_square_pressed();
    void on_button_options_pressed();
    void on_button_l1_pressed();
    void on_button_r1_pressed();
    // L3 / R3: page zoom out / in, saved as the default for every tab.
    void zoom_step(float delta);

    // D-Pad navigation triggers
    void on_dpad_up();
    void on_dpad_down();
    void on_dpad_left();
    void on_dpad_right();

    // D-Pad and focus accessors
    FocusZone focus_zone() const { return focus_zone_; }
    void set_focus_zone(FocusZone zone) { focus_zone_ = zone; }

    int top_bar_index() const { return top_bar_index_; }
    int speed_dial_index() const { return speed_dial_index_; }
    int focused_link_index() const { return focused_link_index_; }
    int drawer_index() const { return drawer_index_; }
    int keyboard_row() const { return keyboard_row_; }
    int keyboard_col() const { return keyboard_col_; }

    bool using_dpad() const { return using_dpad_; }
    void set_using_dpad(bool enable) { using_dpad_ = enable; }

    // Keyboard helpers
    int keyboard_keys_in_row(int row) const;
    std::string keyboard_key_at(int row, int col) const;
    void on_keyboard_backspace();
    void activate_keyboard_key();

    // Analog stick updates
    void update_left_stick(float x, float y, float dt);
    void update_right_stick(float x, float y, float dt);
    void update_triggers(float l2, float r2, float dt);

    // Omnibox actions
    void open_omnibox();
    void close_omnibox();
    void submit_omnibox(const std::string& text);
    const std::string& omnibox_text() const { return omnibox_text_; }
    void set_omnibox_text(const std::string& text) { omnibox_text_ = text; }

    // Navigation shortcuts
    void navigate_to(const std::string& url);
    void reload_current();
    void go_back();
    void go_forward();

    // Tabs: the overview grid (touchpad), new tab, quick switching (L1/R1).
    static constexpr size_t kMaxTabs = 1;   // EVO: the engine host shows one page (was 8)
    static constexpr int kOverviewColumns = 5;
    static constexpr const char* kBookmarksPath = "/data/evoplayer/browser/bookmarks.txt";
    static constexpr const char* kHistoryPath = "/data/evoplayer/browser/history.txt";
    static constexpr const char* kSessionPath = "/data/evoplayer/browser/session.txt";
    static constexpr const char* kSettingsPath = "/data/evoplayer/browser/settings.ini";
    void on_touchpad_pressed();
    void open_tab_overview();
    void new_tab();
    int tab_overview_index() const { return tab_overview_index_; }
    // Number of cards in the overview: the tabs plus a "New tab" card while below the limit.
    int tab_overview_count() const;

    // Options menu: the controller's way to everything in the header, from any page.
    enum class MenuItem { Downloads, Tabs, NewTab, Bookmarks, History, ToggleBookmark, Settings };
    static constexpr int kMenuItemCount = 7;
    int menu_index() const { return menu_index_; }
    // Label for a menu row; ToggleBookmark depends on the current page.
    std::string menu_label(int index) const;
    bool current_page_bookmarkable() const;

    // Downloads panel (header button): list of downloads, newest first.
    void open_downloads();
    int downloads_index() const { return downloads_index_; }
    // Header item under a screen point, or -1 (pointer clicks on the header).
    static int top_bar_item_at(float x, float y);

    // Short message shown above the button hints ("Tab 2 of 3").
    void show_toast(const std::string& text);
    const std::string& toast_text() const { return toast_text_; }
    float toast_alpha() const;

    // Drawer / UI toggles
    void toggle_bookmarks();
    void toggle_settings();
    void open_history();
    void close_overlays();

    // History panel: visited pages, newest first.
    History& history() { return history_; }
    const History& history() const { return history_; }
    int history_index() const { return history_index_; }

    // Settings dialog rows.
    int settings_index() const { return settings_index_; }
    std::string settings_label(int index) const;
    void activate_settings_item();

    // The active page's JavaScript dialog (UiMode::PageDialog). Button 0 is OK /
    // Leave, 1 is Cancel / Stay (confirm, prompt and beforeunload only).
    const web::EngineEvent* page_dialog() const;
    int dialog_button() const { return dialog_button_; }
    int dialog_button_count() const;
    void answer_page_dialog(bool accepted);

    // An element of the page (video) is fullscreen: the browser chrome is hidden
    // and the page fills the screen.
    bool is_fullscreen() const { return fullscreen_; }

    // Accessors
    TabManager& tabs() { return tab_manager_; }
    const TabManager& tabs() const { return tab_manager_; }

    VirtualCursor& cursor() { return cursor_; }
    const VirtualCursor& cursor() const { return cursor_; }

    Bookmarks& bookmarks() { return bookmarks_; }
    const Bookmarks& bookmarks() const { return bookmarks_; }

    BrowserSettings& settings() { return settings_; }
    const BrowserSettings& settings() const { return settings_; }

    UiMode ui_mode() const { return ui_mode_; }
    void set_ui_mode(UiMode mode) { ui_mode_ = mode; }

    // Viewport dimensions for web content
    float viewport_x() const { return viewport_x_; }
    // Button-hint strip along the bottom edge; pages end above it.
    static constexpr float kFooterHeight = 64.0f;
    float viewport_y() const { return viewport_y_; }
    float viewport_width() const { return viewport_width_; }
    float viewport_height() const { return viewport_height_; }

    uint32_t screen_width() const { return screen_width_; }
    uint32_t screen_height() const { return screen_height_; }

    // Native WebKit Dialog controls
    bool open_native_browser(const std::string& url = "");
    bool is_native_browser_active() const;
    void close_native_browser();
    bool is_native_webkit_available() const;
    bool send_engine_key(uint32_t vk);

    const std::string& hovered_link_url() const { return hovered_link_url_; }

private:
    uint32_t screen_width_{1920};
    uint32_t screen_height_{1080};

    // Web content viewport within the 1920x1080 canvas
    float viewport_x_{0.0f};
    float viewport_y_{72.0f};    // 72px top navigation header
    float viewport_width_{1920.0f};
    float viewport_height_{944.0f};

    TabManager tab_manager_;
    VirtualCursor cursor_;
    Bookmarks bookmarks_;
    BrowserSettings settings_;

    UiMode ui_mode_{UiMode::Browsing};
    std::string omnibox_text_{"about:start"};
    std::string hovered_link_url_;
    uint32_t last_timestamp_ms_{0};

    // D-Pad navigation state & helpers
    void activate_top_bar_item();
    void activate_speed_dial_item();
    void activate_focused_link();
    void scroll_link_into_view();

    FocusZone focus_zone_{FocusZone::Content};
    int top_bar_index_{3};       // Default to Omnibox
    int speed_dial_index_{0};    // 0..7
    int focused_link_index_{0};  // Web link index
    int drawer_index_{0};        // Bookmarks drawer index
    int keyboard_row_{1};        // Virtual keyboard row (row 1 = 'q')
    int keyboard_col_{0};        // Virtual keyboard col
    bool using_dpad_{true};      // Default to console D-Pad navigation mode
    int tab_overview_index_{0};  // Selected card in the tab overview
    int downloads_index_{0};     // Selected row in the downloads panel
    int menu_index_{0};          // Selected row in the options menu
    void activate_menu_item();
    void toggle_current_bookmark();
    uint32_t last_download_id_{0};  // Newest download already reacted to
    std::string toast_text_;
    float toast_time_{0.0f};     // Seconds the toast stays up

    void back_in_page();
    void adopt_new_windows();
    void close_tabs_that_asked();
    void handle_page_dialogs();
    void sync_fullscreen();
    void set_fullscreen(bool fullscreen);
    void record_history(float dt);
    void save_session(float dt, bool force = false);
    void restore_session();

    History history_;
    int history_index_{0};
    int settings_index_{0};
    int dialog_button_{0};
    bool fullscreen_{false};
    std::map<uint32_t, std::pair<std::string, std::string>> recorded_;  // tab id -> url, title
    bool history_dirty_{false};
    float history_save_timer_{0.0f};
    std::string saved_session_;
    float session_save_timer_{0.0f};

    void overview_move(int dx, int dy);
    void overview_activate();
    void overview_close_selected();
    void switch_tab(bool next);
};

} // namespace prospero::browser
