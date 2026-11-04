/*
 * EVO stand-in for Prospero Browser's platform/ps5/webkit_dialog.hpp (#237).
 * Prospero opens Sony's system browser dialog as a fallback; EVO's pages come
 * from its own engine, so the dialog is never available here. Same API, so
 * browser_app.cpp stays an unchanged copy.
 */
#pragma once

#include <string>

namespace prospero::ps5 {

enum class DialogMode : int {
    Invalid = 0,
    Default = 1,
    Custom = 2,
};

class WebKitDialog {
public:
    static WebKitDialog& instance()
    {
        static WebKitDialog d;
        return d;
    }
    bool initialize() { return false; }
    void update() {}
    bool open(const std::string&, DialogMode) { return false; }
    void close() {}
    bool is_running() const { return false; }
    bool is_available() const { return false; }
    const std::string& current_url() const { return url_; }

private:
    std::string url_;
};

}  // namespace prospero::ps5
