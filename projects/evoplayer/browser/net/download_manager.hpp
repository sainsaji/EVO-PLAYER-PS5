/*
 * EVO stand-in for Prospero Browser's net/download_manager.hpp (#237).
 * Downloads are not wired to EVO's engine yet: start() says so in a notice and
 * the list stays empty. Same API, so browser_app.cpp and browser_ui.cpp stay
 * unchanged copies.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace prospero::net {

enum class DownloadState {
    Running,
    Done,
    Failed,
    Cancelled,
};

struct Download {
    uint32_t id{0};
    std::string url;
    std::string filename;
    std::string path;
    std::string error;
    int64_t total{-1};
    int64_t received{0};
    DownloadState state{DownloadState::Running};
};

class DownloadManager {
public:
    static DownloadManager& instance()
    {
        static DownloadManager m;
        return m;
    }
    uint32_t start(const std::string&, const std::string&, int64_t)
    {
        notice_ = true;
        return 0;
    }
    void cancel(uint32_t) {}
    void remove(uint32_t) {}
    std::vector<Download> snapshot() const { return {}; }
    int running_count() const { return 0; }
    std::string directory() const { return {}; }
    bool take_notice(std::string* text)
    {
        if (!notice_) return false;
        notice_ = false;
        if (text) *text = "Downloads are not supported in EVO yet";
        return true;
    }

private:
    bool notice_ = false;   /* only touched on the UI thread in EVO */
};

}  // namespace prospero::net
