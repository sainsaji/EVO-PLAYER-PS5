// Prospero Browser - WPE WebKit Port for PlayStation 5
// Multi-Tab Manager.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later

#pragma once

#include "page_view.hpp"
#include <memory>
#include <vector>
#include <string>

namespace prospero::browser {

struct TabInfo {
    uint32_t id{0};
    std::string title{"New Tab"};
    std::string url{"about:start"};
    float progress{1.0f};
    bool is_loading{false};
    float last_active{0.0f};  // TabManager clock when the tab was last selected
    bool opened_by_page{false};  // a popup / target=_blank: Circle with no history closes it
    std::unique_ptr<PageView> view;
};

class TabManager {
public:
    TabManager();
    ~TabManager();

    // Initialize with viewport dimensions
    bool initialize(uint32_t width, uint32_t height);
    void resize(uint32_t width, uint32_t height);

    // Tab lifecycle
    size_t create_tab(const std::string& url = "about:start");
    // Opens a page the engine created (target=_blank, window.open) as a new tab
    // next to the active one, and selects it.
    bool adopt_tab(std::unique_ptr<web::PageEngine> engine);
    // A tab that loads `url` only when first selected (restored sessions). Keeps the selection.
    size_t create_background_tab(const std::string& url);
    bool close_tab(size_t index);
    void select_tab(size_t index);

    // Controller shortcuts
    void next_tab();
    void prev_tab();

    // Active tab access
    PageView* active_view();
    const PageView* active_view() const;
    TabInfo* active_tab_info();
    const TabInfo* active_tab_info() const;

    size_t active_index() const { return active_index_; }
    size_t count() const { return tabs_.size(); }
    const std::vector<TabInfo>& tabs() const { return tabs_; }

    // Page zoom for every tab, including tabs created later.
    void set_zoom_level(float zoom);
    float zoom_level() const { return zoom_level_; }

    // User agent for every tab, including tabs created later; empty = engine default.
    void set_user_agent(const std::string& user_agent);

    TabInfo* find_tab(uint32_t id);
    std::vector<TabInfo>& tabs() { return tabs_; }

    // Engine events for every tab; frames and state for the active one.
    void update(float dt);

private:
    void wire_callbacks(TabInfo& tab);
    void apply_activity();  // the active tab is visible, the rest are hidden

    void sleep_idle_tabs();

    // Pages kept alive in the background before the oldest goes to sleep, and how
    // long a tab must have been in the background first. Every page shares the one
    // web process and the title's memory budget.
    static constexpr size_t kAwakeTabs = 3;
    static constexpr float kSleepAfterSeconds = 180.0f;

    float clock_{0.0f};
    uint32_t width_{1920};
    uint32_t height_{960};
    uint32_t next_id_{1};
    size_t active_index_{0};
    float zoom_level_{1.0f};
    std::string user_agent_;
    std::vector<TabInfo> tabs_;
};

} // namespace prospero::browser
