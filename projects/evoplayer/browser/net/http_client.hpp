/*
 * EVO stand-in for Prospero Browser's net/http_client.hpp (#237).
 * Prospero's built-in renderer fetches http pages itself when no engine is
 * present. In EVO every web page goes to the engine, so a fetch here fails at
 * once with a message (the built-in renderer then shows its error page).
 */
#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace prospero::net {

enum class HttpState { Idle, Connecting, Receiving, Completed, Failed, Cancelled };

struct HttpResponse {
    int status_code = 0;
    std::string body;
    std::string content_type;
    std::string error_message;
    std::string effective_url;
    size_t bytes_downloaded = 0;

    bool ok() const { return (status_code >= 200 && status_code < 400) && error_message.empty(); }
};

class HttpClient {
public:
    using ProgressCallback = std::function<void(size_t bytes_read)>;
    using CompletionCallback = std::function<void(const HttpResponse& response)>;

    bool get_async(const std::string& url, CompletionCallback on_complete, ProgressCallback = nullptr)
    {
        if (on_complete) on_complete(get_sync(url));
        return true;
    }
    void cancel() {}
    HttpResponse get_sync(const std::string& url, ProgressCallback = nullptr)
    {
        HttpResponse r;
        r.effective_url = url;
        r.error_message = "The browser engine is not running";
        return r;
    }
    HttpState state() const { return HttpState::Failed; }
    bool is_busy() const { return false; }
    size_t bytes_downloaded() const { return 0; }
    const std::string& current_url() const { return url_; }
    const std::string& error_message() const { return url_; }

private:
    std::string url_;
};

}  // namespace prospero::net
