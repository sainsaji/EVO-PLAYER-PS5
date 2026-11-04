// Prospero Browser - WPE WebKit Port for PlayStation 5
// Multi-Tab Manager implementation.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later

#include "tab_manager.hpp"
#include <algorithm>

#if defined(__PROSPERO__)
#include "platform/ps5/system.hpp"
#define TAB_LOG(...) hui::sys::log(__VA_ARGS__)
#else
#define TAB_LOG(...) do {} while (0)
#endif

namespace prospero::browser {

TabManager::TabManager() = default;
TabManager::~TabManager() {
    for (auto& tab : tabs_) {
        if (tab.view) {
            tab.view->shutdown();
        }
    }
    tabs_.clear();
}

bool TabManager::initialize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    tabs_.clear();
    create_tab("about:start");
    return !tabs_.empty();
}

void TabManager::resize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    for (auto& tab : tabs_) {
        if (tab.view) {
            tab.view->resize(width, height);
        }
    }
}

void TabManager::wire_callbacks(TabInfo& tab) {
    const uint32_t tab_id = tab.id;
    tab.view->set_zoom_level(zoom_level_);
    if (!user_agent_.empty()) tab.view->set_user_agent(user_agent_);

    tab.view->set_title_callback([this, tab_id](const std::string& title) {
        if (TabInfo* t = find_tab(tab_id)) t->title = title;
    });
    tab.view->set_uri_callback([this, tab_id](const std::string& uri) {
        if (TabInfo* t = find_tab(tab_id)) t->url = uri;
    });
    tab.view->set_load_callback([this, tab_id](LoadState state, float progress) {
        if (TabInfo* t = find_tab(tab_id)) {
            t->progress = progress;
            t->is_loading = (state == LoadState::Loading || state == LoadState::Committed);
        }
    });
}

TabInfo* TabManager::find_tab(uint32_t id) {
    for (auto& t : tabs_) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

size_t TabManager::create_tab(const std::string& url) {
    TAB_LOG("[TAB_MANAGER] create_tab start: %s (%ux%u)", url.c_str(), width_, height_);

    TabInfo tab;
    tab.id = next_id_++;
    tab.url = url;
    tab.title = (url == "about:start") ? "New Tab" : url;
    tab.progress = 1.0f;
    tab.is_loading = false;

    TAB_LOG("[TAB_MANAGER] instantiating PageView...");
    tab.view = std::make_unique<PageView>();

    TAB_LOG("[TAB_MANAGER] calling PageView::initialize...");
    if (tab.view->initialize(width_, height_)) {
        wire_callbacks(tab);
        tabs_.push_back(std::move(tab));
        active_index_ = tabs_.size() - 1;
        tabs_.back().view->load_uri(url);
        apply_activity();
        return active_index_;
    }

    return active_index_;
}

bool TabManager::adopt_tab(std::unique_ptr<web::PageEngine> engine) {
    if (!engine) return false;
    const std::string url = engine->status().uri;
    TabInfo tab;
    tab.id = next_id_++;
    tab.url = url;
    tab.view = std::make_unique<PageView>();
    if (!tab.view->initialize_with_engine(width_, height_, std::move(engine), url)) return false;
    tab.title = tab.view->current_title();
    tab.is_loading = true;
    tab.progress = 0.1f;
    tab.opened_by_page = true;
    wire_callbacks(tab);
    // Next to the page that opened it, like a desktop browser.
    const size_t at = tabs_.empty() ? 0 : std::min(active_index_ + 1, tabs_.size());
    tabs_.insert(tabs_.begin() + static_cast<ptrdiff_t>(at), std::move(tab));
    active_index_ = at;
    apply_activity();
    TAB_LOG("[TAB_MANAGER] adopted new window %s as tab %zu", url.c_str(), at + 1);
    return true;
}

size_t TabManager::create_background_tab(const std::string& url) {
    const size_t keep = active_index_;
    const bool had_tabs = !tabs_.empty();
    create_tab("about:start");
    const size_t index = tabs_.size() - 1;
    if (had_tabs) {
        active_index_ = keep;
        apply_activity();
    }
    tabs_[index].view->load_when_active(url);
    return index;
}

void TabManager::apply_activity() {
    for (size_t i = 0; i < tabs_.size(); ++i) {
        if (i == active_index_) tabs_[i].last_active = clock_;
        if (tabs_[i].view) tabs_[i].view->set_active(i == active_index_);
    }
}

void TabManager::sleep_idle_tabs() {
    size_t awake = 0;
    TabInfo* oldest = nullptr;
    for (size_t i = 0; i < tabs_.size(); ++i) {
        auto& t = tabs_[i];
        if (!t.view || !t.view->has_webkit_instance()) continue;
        ++awake;
        if (i == active_index_ || clock_ - t.last_active < kSleepAfterSeconds) continue;
        if (!oldest || t.last_active < oldest->last_active) oldest = &t;
    }
    if (awake > kAwakeTabs + 1 && oldest) {  // + 1: the active tab
        TAB_LOG("[TAB_MANAGER] %zu pages awake, sleeping tab %u (%s)", awake, oldest->id, oldest->url.c_str());
        oldest->view->sleep();
    }
}

void TabManager::set_user_agent(const std::string& user_agent) {
    user_agent_ = user_agent;
    for (auto& t : tabs_) {
        if (t.view) t.view->set_user_agent(user_agent);
    }
}

bool TabManager::close_tab(size_t index) {
    if (index >= tabs_.size()) return false;
    if (tabs_.size() == 1) {
        // Last tab: reset to start page rather than leaving zero tabs
        tabs_[0].view->load_uri("about:start");
        tabs_[0].title = "New Tab";
        tabs_[0].url = "about:start";
        return true;
    }

    if (tabs_[index].view) {
        tabs_[index].view->shutdown();
    }
    tabs_.erase(tabs_.begin() + static_cast<ptrdiff_t>(index));
    if (index < active_index_) {
        --active_index_;  // keep the same tab active
    } else if (active_index_ >= tabs_.size()) {
        active_index_ = tabs_.size() - 1;
    }
    apply_activity();
    return true;
}

void TabManager::select_tab(size_t index) {
    if (index < tabs_.size()) {
        active_index_ = index;
        apply_activity();
    }
}

void TabManager::next_tab() {
    if (tabs_.size() > 1) {
        active_index_ = (active_index_ + 1) % tabs_.size();
        apply_activity();
    }
}

void TabManager::prev_tab() {
    if (tabs_.size() > 1) {
        active_index_ = (active_index_ + tabs_.size() - 1) % tabs_.size();
        apply_activity();
    }
}

PageView* TabManager::active_view() {
    if (active_index_ < tabs_.size()) {
        return tabs_[active_index_].view.get();
    }
    return nullptr;
}

const PageView* TabManager::active_view() const {
    if (active_index_ < tabs_.size()) {
        return tabs_[active_index_].view.get();
    }
    return nullptr;
}

TabInfo* TabManager::active_tab_info() {
    if (active_index_ < tabs_.size()) {
        return &tabs_[active_index_];
    }
    return nullptr;
}

const TabInfo* TabManager::active_tab_info() const {
    if (active_index_ < tabs_.size()) {
        return &tabs_[active_index_];
    }
    return nullptr;
}

void TabManager::update(float dt) {
    clock_ += dt;
    for (auto& t : tabs_) {
        if (t.view) t.view->poll_events();
    }
    if (active_index_ < tabs_.size()) tabs_[active_index_].last_active = clock_;
    sleep_idle_tabs();
    auto* view = active_view();
    if (view) {
        view->update(dt);
        if (active_index_ < tabs_.size()) {
            tabs_[active_index_].progress = view->load_progress();
            tabs_[active_index_].is_loading = view->is_loading();
        }
    }
}

void TabManager::set_zoom_level(float zoom) {
    zoom_level_ = std::clamp(zoom, 0.5f, 3.0f);
    for (auto& t : tabs_) {
        if (t.view) t.view->set_zoom_level(zoom_level_);
    }
}

} // namespace prospero::browser
