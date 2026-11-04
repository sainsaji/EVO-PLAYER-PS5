// Prospero Browser - Engine-neutral tab view.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later
//
// One PageView per tab. about:/file: pages and, when no engine is registered,
// every page go through the built-in renderer (HttpClient + HtmlDocument).
// http(s) pages go to a web::PageEngine when one is available.

#pragma once

#include "page_texture.hpp"
#include "../net/http_client.hpp"
#include "../web/html_document.hpp"
#include "../web/page_engine.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace prospero::browser {

using web::LoadState;

class PageView {
public:
    using LoadCallback = std::function<void(LoadState state, float progress)>;
    using TitleCallback = std::function<void(const std::string& title)>;
    using UriCallback = std::function<void(const std::string& uri)>;

    PageView();
    ~PageView();

    // Initialize view with specified dimensions
    bool initialize(uint32_t width, uint32_t height);
    // Wraps a page the engine opened itself (target=_blank, window.open). It is
    // already loading `uri`, so nothing is loaded here.
    bool initialize_with_engine(uint32_t width, uint32_t height, std::unique_ptr<web::PageEngine> engine,
                                const std::string& uri);
    void shutdown();

    // Viewport dimensions
    void resize(uint32_t width, uint32_t height);
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    // Navigation methods
    void load_uri(const std::string& uri);
    void load_html(const std::string& html, const std::string& base_uri = "");
    void reload();
    void reload_bypass_cache();
    void stop_loading();

    // History navigation
    bool can_go_back() const;
    bool can_go_forward() const;
    void go_back();
    void go_forward();

    // Zoom level (1.0 = 100%, 1.25 = 125%, etc.)
    void set_zoom_level(float zoom);
    float zoom_level() const { return zoom_level_; }

    // JavaScript evaluation (not wired to any engine yet; reports "undefined")
    void evaluate_javascript(const std::string& script,
                             std::function<void(const std::string& result)> callback = nullptr);

    // Input events (relative to web view viewport)
    void handle_pointer_motion(int32_t x, int32_t y, uint32_t timestamp_ms);
    void handle_pointer_button(int32_t x, int32_t y, uint32_t button, bool pressed, uint32_t timestamp_ms);
    void handle_scroll(int32_t x, int32_t y, int32_t delta_x, int32_t delta_y, uint32_t timestamp_ms);
    void handle_key(uint32_t key_code, bool pressed, uint32_t modifiers, uint32_t timestamp_ms);

    // Callbacks
    void set_load_callback(LoadCallback callback) { load_callback_ = std::move(callback); }
    void set_title_callback(TitleCallback callback) { title_callback_ = std::move(callback); }
    void set_uri_callback(UriCallback callback) { uri_callback_ = std::move(callback); }

    // Texture handle for OpenGL compositing in ps5-homebrew-ui
    GLuint texture_id() const { return texture_.texture_id(); }

    // State queries
    const std::string& current_uri() const { return current_uri_; }
    const std::string& current_title() const { return current_title_; }
    float load_progress() const { return load_progress_; }
    bool is_loading() const { return state_ == LoadState::Loading || state_ == LoadState::Committed; }
    LoadState state() const { return state_; }
    size_t bytes_downloaded() const { return http_client_ ? http_client_->bytes_downloaded() : 0; }

    // History stack representation (built-in renderer)
    const std::vector<std::string>& history() const { return history_; }
    size_t history_index() const { return history_index_; }

    // Document and scrolling (built-in renderer)
    const web::HtmlDocument& document() const { return document_; }
    int32_t scroll_offset_y() const { return scroll_offset_y_; }
    void set_scroll_offset_y(int32_t offset);
    void scroll_by_y(int32_t delta);
    const web::LinkTarget* hit_test_link(float vx, float vy) const;

    // Tick: pulls engine state and frames, advances built-in loading progress
    void update(float dt);

    // Collects the engine's events (errors, crashes, dialogs, close, fullscreen).
    // Called every frame for every tab, active or not.
    void poll_events();

    // The selected tab is active; the others are hidden so the engine throttles them.
    void set_active(bool active);
    bool is_active() const { return active_; }

    // Empty = the engine's own user agent. Applies to pages loaded from now on.
    void set_user_agent(const std::string& user_agent);

    // Loads `uri` now if the tab is active, otherwise the first time it becomes
    // active (tabs restored at startup).
    void load_when_active(const std::string& uri);
    // Frees a background tab's engine page (web-process memory); it reloads its
    // URL when selected again. Returns false if there was nothing to free.
    bool sleep();
    bool is_asleep() const { return !pending_uri_.empty(); }

    // JavaScript dialogs waiting for an answer, oldest first.
    bool has_pending_dialog() const { return !dialogs_.empty(); }
    const web::EngineEvent& pending_dialog() const { return dialogs_.front(); }
    void answer_dialog(bool accepted, const std::string& text = "");

    // The page called window.close() since the last call.
    bool take_close_request() { return std::exchange(close_requested_, false); }

    // An element of the page (usually a video) is fullscreen.
    bool is_fullscreen() const { return fullscreen_; }
    void exit_fullscreen();

    // True while an error or crash page replaces an engine page that failed.
    bool showing_engine_failure() const { return engine_failure_; }

    // True while the texture shows a page rendered by the engine
    bool has_webkit_instance() const { return engine_ && showing_engine_; }

private:
    void show_engine_failure(const web::EngineEvent& event);
    void render_page_content();
    void update_state(LoadState new_state, float progress);
    void load_builtin_page(const std::string& uri);
    void load_with_engine(const std::string& uri);
    void sync_engine_status();

    uint32_t width_{1920};
    uint32_t height_{1080};
    PageTexture texture_;
    std::unique_ptr<web::PageEngine> engine_;
    bool showing_engine_{false};
    bool engine_failure_{false};      // an error/crash page stands in for the engine page
    bool reload_when_active_{false};  // crashed while in the background
    bool active_{true};
    bool close_requested_{false};
    bool fullscreen_{false};
    std::vector<web::EngineEvent> dialogs_;
    std::string pending_uri_;  // loads when the tab becomes active
    std::string user_agent_;

    std::unique_ptr<net::HttpClient> http_client_;
    web::HtmlDocument document_;

    std::string current_uri_{"about:start"};
    std::string current_title_{"Prospero Browser - Start"};
    std::string current_html_;

    std::vector<std::string> history_;
    size_t history_index_{0};

    LoadState state_{LoadState::Idle};
    float load_progress_{1.0f};
    float zoom_level_{1.0f};
    int32_t scroll_offset_y_{0};
    float loading_timer_{0.0f};

    LoadCallback load_callback_;
    TitleCallback title_callback_;
    UriCallback uri_callback_;
};

} // namespace prospero::browser
