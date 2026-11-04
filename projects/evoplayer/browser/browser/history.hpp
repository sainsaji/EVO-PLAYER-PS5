// Prospero Browser - Browsing history and the saved tab session.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace prospero::browser {

struct HistoryEntry {
    std::string url;
    std::string title;
    int64_t visited{0};  // seconds since the epoch
};

// Visited pages, newest first, one entry per URL.
class History {
public:
    static constexpr size_t kMaxEntries = 500;

    // Moves `url` to the top (or adds it). Only http(s) pages are kept.
    // Returns true if the list changed.
    bool record(const std::string& url, const std::string& title, int64_t now);
    bool remove(size_t index);
    void clear();

    const std::vector<HistoryEntry>& entries() const { return entries_; }
    bool empty() const { return entries_.empty(); }

    // One entry per line: visited<TAB>url<TAB>title.
    bool load_from_file(const std::string& path);
    bool save_to_file(const std::string& path) const;

private:
    std::vector<HistoryEntry> entries_;
};

// The tabs open when the browser last ran, restored at the next start.
struct TabSession {
    std::vector<std::string> urls;
    size_t active{0};

    // "active=<n>" then one URL per line.
    std::string encode() const;
    static bool decode(const std::string& text, TabSession* out);
    bool load_from_file(const std::string& path);
    bool save_to_file(const std::string& path) const;
};

// http:// and https:// pages; the only ones history and sessions keep.
bool is_restorable_url(const std::string& url);

} // namespace prospero::browser
