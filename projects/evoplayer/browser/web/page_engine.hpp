// Prospero Browser - Engine-neutral web page interface.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later
//
// A PageEngine renders real web pages for one tab (Sony's system WebKit today).
// browser::PageView owns one when an engine is registered and falls back to the
// built-in text renderer otherwise. All methods are called from the UI thread;
// an implementation that renders elsewhere hands frames over via take_frame().

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace prospero::web {

enum class LoadState {
    Idle,
    Loading,
    Committed,
    Finished,
    Failed,
};

// Something the page needs the browser to act on, queued by the engine and
// collected on the UI thread with PageEngine::take_events().
struct EngineEvent {
    enum class Type {
        LoadFailed,         // url, message, code: the page could not be loaded
        ProcessTerminated,  // the web content process died (crash, out of memory)
        CloseRequested,     // window.close()
        Dialog,             // alert/confirm/prompt/beforeunload; answer with reply_dialog(id)
        FullscreenEntered,  // an element went fullscreen (video); hide the browser chrome
        FullscreenExited,
    };
    enum class DialogKind { Alert, Confirm, Prompt, BeforeUnload };

    Type type{Type::LoadFailed};
    std::string url;
    std::string message;
    int code{0};
    DialogKind dialog{DialogKind::Alert};
    std::string default_text;  // Prompt
    uint64_t dialog_id{0};
};

class PageEngine {
public:
    struct Status {
        std::string uri;
        std::string title;
        float progress{1.0f};
        bool loading{false};
        bool can_go_back{false};
        bool can_go_forward{false};
    };

    // Receives a BGRA8 frame (bytes B,G,R,A; rows tightly packed).
    using FrameSink = std::function<void(const void* pixels, uint32_t width, uint32_t height)>;

    virtual ~PageEngine() = default;

    virtual void load_uri(const std::string& uri) = 0;
    virtual void reload() = 0;
    virtual void stop_loading() = 0;
    virtual void go_back() = 0;
    virtual void go_forward() = 0;
    virtual void set_zoom_level(float zoom) = 0;
    virtual void resize(uint32_t width, uint32_t height) = 0;

    // Coordinates are viewport-relative pixels. Buttons: 1 = left, 2 = middle, 3 = right.
    virtual void pointer_motion(int32_t x, int32_t y) = 0;
    virtual void pointer_button(int32_t x, int32_t y, uint32_t button, bool pressed) = 0;
    // Positive delta_y scrolls towards the top of the page.
    virtual void scroll(int32_t x, int32_t y, int32_t delta_x, int32_t delta_y) = 0;
    virtual void key(uint32_t key_code, bool pressed, uint32_t modifiers) = 0;

    // Latest known page state (may lag the engine by a frame).
    virtual Status status() const = 0;
    // Passes the newest unseen frame to `sink`; returns false when there is none.
    virtual bool take_frame(const FrameSink& sink) = 0;

    // Events queued since the last call, oldest first.
    virtual std::vector<EngineEvent> take_events() { return {}; }
    // Answers a Dialog event. `accepted` is OK/Leave; `text` is the prompt's answer.
    // Every dialog must be answered: the page's script is blocked until then.
    virtual void reply_dialog(uint64_t dialog_id, bool accepted, const std::string& text) {
        (void)dialog_id;
        (void)accepted;
        (void)text;
    }
    // Empty restores the engine's own user agent.
    virtual void set_user_agent(const std::string& user_agent) { (void)user_agent; }
    // Background tabs are hidden: the engine throttles their timers and painting.
    virtual void set_visible(bool visible) { (void)visible; }
    // Leaves element fullscreen (Circle while a video is fullscreen).
    virtual void exit_fullscreen() {}
};

// Pages opened by the engine itself (target=_blank, window.open). Created on the
// engine's thread with the opener's configuration, so window.opener works
// (sign-in popups); the browser adopts each into a new tab.
class NewWindowQueue {
public:
    static NewWindowQueue& instance() {
        static NewWindowQueue queue;
        return queue;
    }
    void push(std::unique_ptr<PageEngine> engine) {
        std::lock_guard<std::mutex> lock(mu_);
        pending_.push_back(std::move(engine));
    }
    std::vector<std::unique_ptr<PageEngine>> take() {
        std::lock_guard<std::mutex> lock(mu_);
        return std::exchange(pending_, {});
    }

private:
    std::mutex mu_;
    std::vector<std::unique_ptr<PageEngine>> pending_;
};

// Deletes cookies, site storage and caches of every page. Set by the platform layer.
using ClearWebsiteDataFn = std::function<void()>;
inline ClearWebsiteDataFn& clear_website_data_handler() {
    static ClearWebsiteDataFn handler;
    return handler;
}

using PageEngineFactory = std::function<std::unique_ptr<PageEngine>(uint32_t width, uint32_t height)>;

// Set once at startup by the platform layer when an engine is usable; empty means
// every tab uses the built-in renderer.
inline PageEngineFactory& page_engine_factory() {
    static PageEngineFactory factory;
    return factory;
}

inline std::unique_ptr<PageEngine> create_page_engine(uint32_t width, uint32_t height) {
    auto& factory = page_engine_factory();
    return factory ? factory(width, height) : nullptr;
}

// A navigation the engine will not display (archive, package, PDF, binary).
struct DownloadRequest {
    std::string url;
    std::string suggested_name;  // from Content-Disposition; may be empty
    std::string mime_type;
    int64_t expected_length{-1};
};

// Set by the browser at startup. Called on the engine's thread; must be thread-safe.
using DownloadRequestHandler = std::function<void(const DownloadRequest&)>;
inline DownloadRequestHandler& download_request_handler() {
    static DownloadRequestHandler handler;
    return handler;
}

} // namespace prospero::web
