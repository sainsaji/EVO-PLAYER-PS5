// Prospero Browser - Engine-neutral tab view implementation.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later

#include "page_view.hpp"
#include "navigation.hpp"
#include <algorithm>
#include <cstdio>

#if defined(__PROSPERO__)
#include "platform/ps5/system.hpp"
#define PAGE_LOG(...) hui::sys::log(__VA_ARGS__)
#else
#define PAGE_LOG(...) do {} while (0)
#endif

namespace prospero::browser {

namespace {

bool read_file_string(const char* path, std::string& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;

    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 4 * 1024 * 1024) {
        std::fclose(f);
        return false;
    }

    out.resize(static_cast<size_t>(size));
    size_t read_bytes = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    out.resize(read_bytes);
    return read_bytes > 0;
}

bool read_web_asset(const char* filename, std::string& out) {
    static const char *const kWebBases[] = {
        "/app0/assets/web/",
        "/mnt/usb0/homebrew/PPSA99003/assets/web/",
        "/data/homebrew/PPSA99003/assets/web/",
        "assets/web/",
        nullptr
    };
    for (int i = 0; kWebBases[i]; ++i) {
        std::string p = std::string(kWebBases[i]) + filename;
        if (read_file_string(p.c_str(), out)) return true;
    }
    return false;
}

bool is_web_uri(const std::string& uri) {
    return uri.rfind("http://", 0) == 0 || uri.rfind("https://", 0) == 0;
}

} // namespace

PageView::PageView() = default;

PageView::~PageView() {
    shutdown();
}

bool PageView::initialize(uint32_t width, uint32_t height) {
    PAGE_LOG("[PAGE_VIEW] initialize (%ux%u)", width, height);
    width_ = width;
    height_ = height;
    if (!texture_.init(width, height)) {
        PAGE_LOG("[PAGE_VIEW] texture init failed");
        return false;
    }

    engine_ = web::create_page_engine(width, height);
    PAGE_LOG("[PAGE_VIEW] web engine: %s", engine_ ? "available" : "none, built-in renderer only");

    http_client_ = std::make_unique<net::HttpClient>();

    // Default start page
    load_uri("about:start");
    return true;
}

bool PageView::initialize_with_engine(uint32_t width, uint32_t height, std::unique_ptr<web::PageEngine> engine,
                                      const std::string& uri) {
    PAGE_LOG("[PAGE_VIEW] adopt engine page %s (%ux%u)", uri.c_str(), width, height);
    width_ = width;
    height_ = height;
    if (!engine || !texture_.init(width, height)) {
        PAGE_LOG("[PAGE_VIEW] adopt failed");
        return false;
    }
    engine_ = std::move(engine);
    engine_->resize(width, height);
    http_client_ = std::make_unique<net::HttpClient>();
    history_.assign(1, uri);
    history_index_ = 0;
    showing_engine_ = true;
    current_uri_ = uri;
    current_title_ = Navigation::extract_host(uri);
    if (current_title_.empty()) current_title_ = uri.empty() ? "New window" : uri;
    update_state(LoadState::Loading, 0.10f);
    return true;
}

void PageView::shutdown() {
    title_callback_ = nullptr;
    uri_callback_ = nullptr;
    load_callback_ = nullptr;
    dialogs_.clear();  // the engine answers them as it goes
    if (http_client_) {
        http_client_->cancel();
        http_client_.reset();
    }
    engine_.reset();
    showing_engine_ = false;
    texture_.release();
    history_.clear();
    history_index_ = 0;
    state_ = LoadState::Idle;
}

void PageView::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    width_ = width;
    height_ = height;
    texture_.resize(width, height);
    if (engine_) {
        engine_->resize(width, height);
    }
    if (!document_.is_empty()) {
        // Recompute layout for new width/height
        document_.parse_and_layout(current_html_, current_uri_, static_cast<float>(width), static_cast<float>(height));
    }
    render_page_content();
}

void PageView::load_builtin_page(const std::string& uri) {
    showing_engine_ = false;
    engine_failure_ = false;
    scroll_offset_y_ = 0;
    current_uri_ = uri;

    float w = static_cast<float>(width());
    float h = static_cast<float>(height());

    if (uri == "about:start") {
        document_.create_start_page(w, h);
    } else if (uri == "about:version" || uri == "about:about") {
        std::string html;
        if (read_web_asset("about.html", html)) {
            current_html_ = html;
            document_.parse_and_layout(html, uri, w, h);
        } else {
            document_.create_version_page(w, h);
        }
    } else if (uri == "about:help" || uri == "about:shortcuts") {
        document_.create_help_page(w, h);
    } else if (uri == "about:offline") {
        std::string html;
        if (read_web_asset("offline.html", html)) {
            current_html_ = html;
            document_.parse_and_layout(html, uri, w, h);
        } else {
            document_.create_error_page(uri, "No active network connection", 0, w, h);
        }
    } else {
        // about:blank
        document_.clear();
    }

    current_title_ = document_.title();
    update_state(LoadState::Finished, 1.0f);

    if (title_callback_) title_callback_(current_title_);
    if (uri_callback_) uri_callback_(current_uri_);
    render_page_content();
}

void PageView::load_with_engine(const std::string& uri) {
    if (http_client_) {
        http_client_->cancel();
    }
    showing_engine_ = true;
    engine_failure_ = false;
    document_.clear();
    current_html_.clear();
    current_uri_ = uri;
    current_title_ = Navigation::extract_host(uri);
    if (current_title_.empty()) current_title_ = uri;
    update_state(LoadState::Loading, 0.10f);
    if (uri_callback_) uri_callback_(current_uri_);
    if (title_callback_) title_callback_(current_title_);
    PAGE_LOG("[PAGE_VIEW] loading via web engine: %s", uri.c_str());
    engine_->load_uri(uri);
}

void PageView::load_uri(const std::string& uri) {
    if (uri.empty()) return;

    scroll_offset_y_ = 0;
    engine_failure_ = false;

    // Update history stack
    if (history_index_ + 1 < history_.size()) {
        history_.erase(history_.begin() + static_cast<ptrdiff_t>(history_index_) + 1, history_.end());
    }
    history_.push_back(uri);
    history_index_ = history_.size() - 1;

    // Built-in about: pages
    if (uri.rfind("about:", 0) == 0) {
        load_builtin_page(uri);
        return;
    }

    // Local file loading
    if (uri.rfind("file://", 0) == 0) {
        std::string path = uri.substr(7);
        std::string html;
        if (read_file_string(path.c_str(), html)) {
            load_html(html, uri);
        } else {
            showing_engine_ = false;
            float w = static_cast<float>(width());
            float h = static_cast<float>(height());
            document_.create_error_page(uri, "File not found: " + path, 404, w, h);
            current_title_ = document_.title();
            update_state(LoadState::Failed, 1.0f);
        }
        return;
    }

    if (engine_ && is_web_uri(uri)) {
        load_with_engine(uri);
        return;
    }

    // Remote HTTP / HTTPS page via the built-in renderer
    showing_engine_ = false;
    current_uri_ = uri;
    current_title_ = Navigation::extract_host(uri);
    if (current_title_.empty()) current_title_ = uri;

    update_state(LoadState::Loading, 0.15f);
    loading_timer_ = 0.0f;

    if (uri_callback_) uri_callback_(current_uri_);
    if (title_callback_) title_callback_(current_title_);

    render_page_content();

    if (!http_client_) {
        http_client_ = std::make_unique<net::HttpClient>();
    }

    http_client_->get_async(
        uri,
        [this, uri](const net::HttpResponse& resp) {
            float w = static_cast<float>(width());
            float h = static_cast<float>(height());

            if (resp.ok() && !resp.body.empty()) {
                PAGE_LOG("[PAGE_VIEW] load_uri OK: %s (%zu bytes)", resp.effective_url.c_str(), resp.body.size());
                current_html_ = resp.body;
                std::string page_url = resp.effective_url.empty() ? uri : resp.effective_url;
                current_uri_ = page_url;
                if (uri_callback_) uri_callback_(current_uri_);
                document_.parse_and_layout(resp.body, page_url, w, h);
                current_title_ = document_.title();
                update_state(LoadState::Finished, 1.0f);
            } else {
                std::string err = resp.error_message.empty() ? "Network request failed" : resp.error_message;
                PAGE_LOG("[PAGE_VIEW] load_uri FAILED: %s, error=%s, code=%d", uri.c_str(), err.c_str(), resp.status_code);
                std::string page_url = resp.effective_url.empty() ? uri : resp.effective_url;
                current_uri_ = page_url;
                if (uri_callback_) uri_callback_(current_uri_);
                document_.create_error_page(page_url, err, resp.status_code, w, h);
                current_title_ = document_.title();
                update_state(LoadState::Failed, 1.0f);
            }

            if (title_callback_) title_callback_(current_title_);
            render_page_content();
        },
        [this](size_t bytes) {
            float progress = std::min(0.85f, 0.15f + (static_cast<float>(bytes) / 100000.0f) * 0.70f);
            update_state(LoadState::Committed, progress);
        }
    );
}

void PageView::load_html(const std::string& html, const std::string& base_uri) {
    showing_engine_ = false;
    engine_failure_ = false;
    current_html_ = html;
    current_uri_ = base_uri.empty() ? "about:custom" : base_uri;
    scroll_offset_y_ = 0;

    float w = static_cast<float>(width());
    float h = static_cast<float>(height());
    document_.parse_and_layout(html, current_uri_, w, h);
    current_title_ = document_.title();

    update_state(LoadState::Finished, 1.0f);

    if (uri_callback_) uri_callback_(current_uri_);
    if (title_callback_) title_callback_(current_title_);
    render_page_content();
}

void PageView::reload() {
    if (engine_failure_) {
        load_with_engine(current_uri_);  // Retry: the same entry, not a new one
        return;
    }
    if (showing_engine_) {
        engine_->reload();
        update_state(LoadState::Loading, 0.10f);
        return;
    }
    load_uri(current_uri_);
}

void PageView::reload_bypass_cache() {
    reload();
}

void PageView::stop_loading() {
    if (showing_engine_) {
        engine_->stop_loading();
    }
    if (http_client_) {
        http_client_->cancel();
    }
    if (is_loading()) {
        update_state(LoadState::Finished, 1.0f);
    }
}

bool PageView::can_go_back() const {
    if ((showing_engine_ || engine_failure_) && engine_->status().can_go_back) {
        return true;
    }
    return history_index_ > 0;
}

bool PageView::can_go_forward() const {
    if (showing_engine_ && engine_->status().can_go_forward) {
        return true;
    }
    return history_index_ + 1 < history_.size();
}

void PageView::go_back() {
    // In-page history first (links followed inside the engine), then the tab's own stack.
    // A failed load adds no entry, so from its error page this is the page before it.
    if ((showing_engine_ || engine_failure_) && engine_->status().can_go_back) {
        if (engine_failure_) {
            engine_failure_ = false;
            showing_engine_ = true;
            document_.clear();
            update_state(LoadState::Loading, 0.10f);
        }
        engine_->go_back();
        return;
    }
    if (history_index_ > 0) {
        --history_index_;
        std::string uri = history_[history_index_];
        if (uri.rfind("about:", 0) == 0) {
            load_builtin_page(uri);
        } else if (engine_ && is_web_uri(uri)) {
            load_with_engine(uri);
        } else {
            load_uri(uri);
        }
    }
}

void PageView::go_forward() {
    if (showing_engine_ && engine_->status().can_go_forward) {
        engine_->go_forward();
        return;
    }
    if (history_index_ + 1 < history_.size()) {
        ++history_index_;
        std::string uri = history_[history_index_];
        if (uri.rfind("about:", 0) == 0) {
            load_builtin_page(uri);
        } else if (engine_ && is_web_uri(uri)) {
            load_with_engine(uri);
        } else {
            load_uri(uri);
        }
    }
}

void PageView::set_zoom_level(float zoom) {
    zoom_level_ = std::clamp(zoom, 0.5f, 3.0f);
    if (engine_) {
        engine_->set_zoom_level(zoom_level_);
    }
    render_page_content();
}

void PageView::evaluate_javascript(const std::string& script,
                                   std::function<void(const std::string& result)> callback) {
    (void)script;
    if (callback) {
        callback("undefined");
    }
}

void PageView::handle_pointer_motion(int32_t x, int32_t y, uint32_t timestamp_ms) {
    (void)timestamp_ms;
    if (showing_engine_) {
        engine_->pointer_motion(x, y);
    }
}

void PageView::handle_pointer_button(int32_t x, int32_t y, uint32_t button, bool pressed, uint32_t timestamp_ms) {
    (void)timestamp_ms;
    if (showing_engine_) {
        engine_->pointer_button(x, y, button, pressed);
    }
}

void PageView::set_scroll_offset_y(int32_t offset) {
    int32_t max_s = static_cast<int32_t>(document_.max_scroll_y());
    scroll_offset_y_ = std::clamp(offset, 0, std::max(0, max_s));
    render_page_content();
}

void PageView::scroll_by_y(int32_t delta) {
    if (showing_engine_) {
        engine_->scroll(static_cast<int32_t>(width_ / 2), static_cast<int32_t>(height_ / 2), 0, -delta);
        return;
    }
    set_scroll_offset_y(scroll_offset_y_ + delta);
}

void PageView::handle_scroll(int32_t x, int32_t y, int32_t delta_x, int32_t delta_y, uint32_t timestamp_ms) {
    (void)timestamp_ms;
    if (showing_engine_) {
        engine_->scroll(x, y, delta_x, delta_y);
        return;
    }
    int32_t max_s = static_cast<int32_t>(document_.max_scroll_y());
    scroll_offset_y_ = std::clamp(scroll_offset_y_ - delta_y, 0, std::max(0, max_s));
    render_page_content();
}

void PageView::handle_key(uint32_t key_code, bool pressed, uint32_t modifiers, uint32_t timestamp_ms) {
    (void)timestamp_ms;
    if (showing_engine_) {
        engine_->key(key_code, pressed, modifiers);
    }
}

const web::LinkTarget* PageView::hit_test_link(float vx, float vy) const {
    float page_x = vx;
    float page_y = vy + static_cast<float>(scroll_offset_y_);
    return document_.hit_test_link(page_x, page_y);
}

void PageView::sync_engine_status() {
    const auto st = engine_->status();
    if (!st.uri.empty() && st.uri != current_uri_) {
        current_uri_ = st.uri;
        if (uri_callback_) uri_callback_(current_uri_);
    }
    if (!st.title.empty() && st.title != current_title_) {
        current_title_ = st.title;
        if (title_callback_) title_callback_(current_title_);
    }
    const LoadState next = st.loading ? LoadState::Loading : LoadState::Finished;
    const float progress = st.loading ? std::clamp(st.progress, 0.10f, 0.95f) : 1.0f;
    if (next != state_ || progress != load_progress_) {
        update_state(next, progress);
    }
}

void PageView::update(float dt) {
    if (showing_engine_) {
        sync_engine_status();
        engine_->take_frame([this](const void* pixels, uint32_t w, uint32_t h) {
            texture_.update_pixels(pixels, w, h);
        });
        return;
    }

    if (state_ == LoadState::Loading || state_ == LoadState::Committed) {
        loading_timer_ += dt;
        // Keep progress gently incrementing while waiting for network
        if (load_progress_ < 0.85f) {
            load_progress_ = std::min(0.85f, load_progress_ + dt * 0.05f);
        }
    }
}

void PageView::poll_events() {
    if (!engine_) return;
    for (auto& e : engine_->take_events()) {
        switch (e.type) {
            case web::EngineEvent::Type::LoadFailed:
                if (showing_engine_) show_engine_failure(e);
                break;
            case web::EngineEvent::Type::ProcessTerminated:
                fullscreen_ = false;
                if (!showing_engine_) break;
                if (active_) {
                    show_engine_failure(e);
                } else {
                    reload_when_active_ = true;  // reload quietly once the tab is selected
                }
                break;
            case web::EngineEvent::Type::CloseRequested:
                close_requested_ = true;
                break;
            case web::EngineEvent::Type::Dialog:
                dialogs_.push_back(std::move(e));
                break;
            case web::EngineEvent::Type::FullscreenEntered:
                fullscreen_ = true;
                break;
            case web::EngineEvent::Type::FullscreenExited:
                fullscreen_ = false;
                break;
        }
    }
}

void PageView::show_engine_failure(const web::EngineEvent& event) {
    const std::string url = event.url.empty() ? current_uri_ : event.url;
    PAGE_LOG("[PAGE_VIEW] engine %s: %s (%d %s)",
             event.type == web::EngineEvent::Type::LoadFailed ? "load failed" : "process terminated", url.c_str(),
             event.code, event.message.c_str());
    showing_engine_ = false;
    engine_failure_ = true;
    fullscreen_ = false;
    scroll_offset_y_ = 0;
    current_uri_ = url;
    const float w = static_cast<float>(width());
    const float h = static_cast<float>(height());
    if (event.type == web::EngineEvent::Type::LoadFailed) {
        document_.create_error_page(url, event.message, 0, w, h);
    } else {
        document_.create_crash_page(url, event.message, w, h);
    }
    current_title_ = document_.title();
    update_state(LoadState::Failed, 1.0f);
    if (uri_callback_) uri_callback_(current_uri_);
    if (title_callback_) title_callback_(current_title_);
    render_page_content();
}

void PageView::set_active(bool active) {
    if (active == active_) return;
    active_ = active;
    if (engine_) engine_->set_visible(active);
    if (!active) return;
    if (!pending_uri_.empty()) {
        const std::string uri = std::exchange(pending_uri_, {});
        if (!engine_) {
            engine_ = web::create_page_engine(width_, height_);
            if (engine_) {
                engine_->set_zoom_level(zoom_level_);
                if (!user_agent_.empty()) engine_->set_user_agent(user_agent_);
            }
        }
        // Back to the page itself, not a second history entry for it.
        if (engine_ && is_web_uri(uri)) {
            load_with_engine(uri);
        } else {
            load_uri(uri);
        }
        return;
    }
    if (reload_when_active_) {
        reload_when_active_ = false;
        if (showing_engine_) engine_->reload();
    }
}

void PageView::set_user_agent(const std::string& user_agent) {
    user_agent_ = user_agent;
    if (engine_) engine_->set_user_agent(user_agent);
}

void PageView::load_when_active(const std::string& uri) {
    if (active_) {
        load_uri(uri);
        return;
    }
    history_.assign(1, uri);
    history_index_ = 0;
    pending_uri_ = uri;
    current_uri_ = uri;
    current_title_ = Navigation::extract_host(uri);
    if (current_title_.empty()) current_title_ = uri;
    update_state(LoadState::Idle, 1.0f);
    if (uri_callback_) uri_callback_(current_uri_);
    if (title_callback_) title_callback_(current_title_);
}

bool PageView::sleep() {
    if (active_ || !engine_ || !showing_engine_ || !dialogs_.empty()) return false;
    PAGE_LOG("[PAGE_VIEW] sleeping background tab %s", current_uri_.c_str());
    pending_uri_ = current_uri_;
    engine_.reset();
    showing_engine_ = false;
    engine_failure_ = false;
    reload_when_active_ = false;
    fullscreen_ = false;
    update_state(LoadState::Idle, 1.0f);
    return true;
}

void PageView::answer_dialog(bool accepted, const std::string& text) {
    if (dialogs_.empty()) return;
    if (engine_) engine_->reply_dialog(dialogs_.front().dialog_id, accepted, text);
    dialogs_.erase(dialogs_.begin());
}

void PageView::exit_fullscreen() {
    if (engine_ && fullscreen_) engine_->exit_fullscreen();
}

void PageView::update_state(LoadState new_state, float progress) {
    state_ = new_state;
    load_progress_ = progress;
    if (load_callback_) {
        load_callback_(state_, load_progress_);
    }
}

void PageView::render_page_content() {
    // Engine pages own the texture; the built-in renderer draws over a cleared one.
    if (showing_engine_ || !texture_.is_valid()) return;

    texture_.bind();

    // Render page background color based on URI
    if (current_uri_ == "about:start") {
        glClearColor(0.06f, 0.08f, 0.14f, 1.0f);
    } else if (current_uri_ == "about:blank") {
        glClearColor(0.10f, 0.11f, 0.15f, 1.0f);
    } else {
        // Modern console web viewport backdrop
        glClearColor(0.08f, 0.10f, 0.16f, 1.0f);
    }

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    texture_.unbind();
}

} // namespace prospero::browser
