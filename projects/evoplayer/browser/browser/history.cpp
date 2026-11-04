// Prospero Browser - Browsing history and the saved tab session.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later

#include "history.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace prospero::browser {

namespace {

bool read_text(const std::string& path, std::string* out) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    std::string data;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) data.append(buf, n);
    std::fclose(fp);
    *out = std::move(data);
    return true;
}

// Write to path.tmp, then rename: a crash mid-write keeps the old file.
bool write_text(const std::string& path, const std::string& data) {
    const std::string tmp = path + ".tmp";
    std::FILE* fp = std::fopen(tmp.c_str(), "wb");
    if (!fp) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), fp) == data.size();
    if (std::fclose(fp) != 0 || !ok) {
        std::remove(tmp.c_str());
        return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(path.c_str());
        if (std::rename(tmp.c_str(), path.c_str()) != 0) return false;
    }
    return true;
}

template <typename Fn>
void for_each_line(const std::string& text, Fn fn) {
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        start = end + 1;
        if (!line.empty()) fn(line);
    }
}

// Titles come from pages: keep them on one field of one line.
std::string one_line(const std::string& s) {
    std::string out = s;
    std::replace_if(out.begin(), out.end(), [](char c) { return c == '\t' || c == '\n' || c == '\r'; }, ' ');
    return out;
}

} // namespace

bool is_restorable_url(const std::string& url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

bool History::record(const std::string& url, const std::string& title, int64_t now) {
    if (!is_restorable_url(url)) return false;
    const std::string clean_title = one_line(title);
    if (!entries_.empty() && entries_.front().url == url) {
        // Same page again (title arriving after the URL, a reload): refresh it in place.
        if (clean_title.empty() || entries_.front().title == clean_title) return false;
        entries_.front().title = clean_title;
        return true;
    }
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const HistoryEntry& e) { return e.url == url; });
    std::string keep_title = clean_title;
    if (it != entries_.end()) {
        if (keep_title.empty()) keep_title = it->title;
        entries_.erase(it);
    }
    entries_.insert(entries_.begin(), HistoryEntry{url, keep_title, now});
    if (entries_.size() > kMaxEntries) entries_.resize(kMaxEntries);
    return true;
}

bool History::remove(size_t index) {
    if (index >= entries_.size()) return false;
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

void History::clear() { entries_.clear(); }

bool History::load_from_file(const std::string& path) {
    std::string text;
    if (!read_text(path, &text)) return false;
    entries_.clear();
    for_each_line(text, [&](const std::string& line) {
        if (line[0] == '#' || entries_.size() >= kMaxEntries) return;
        const size_t a = line.find('\t');
        if (a == std::string::npos) return;
        const size_t b = line.find('\t', a + 1);
        HistoryEntry e;
        e.visited = std::strtoll(line.substr(0, a).c_str(), nullptr, 10);
        e.url = line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
        if (b != std::string::npos) e.title = line.substr(b + 1);
        if (is_restorable_url(e.url)) entries_.push_back(std::move(e));
    });
    return true;
}

bool History::save_to_file(const std::string& path) const {
    std::string text = "# Prospero Browser history: visited<TAB>url<TAB>title, newest first\n";
    for (const auto& e : entries_) {
        text += std::to_string(e.visited) + '\t' + e.url + '\t' + e.title + '\n';
    }
    return write_text(path, text);
}

std::string TabSession::encode() const {
    std::string text = "active=" + std::to_string(active) + '\n';
    for (const auto& url : urls) text += url + '\n';
    return text;
}

bool TabSession::decode(const std::string& text, TabSession* out) {
    TabSession s;
    for_each_line(text, [&](const std::string& line) {
        if (line.rfind("active=", 0) == 0) {
            s.active = static_cast<size_t>(std::strtoul(line.c_str() + 7, nullptr, 10));
        } else if (line.find_first_of(" \t") == std::string::npos) {
            s.urls.push_back(line);
        }
    });
    if (s.urls.empty()) return false;
    if (s.active >= s.urls.size()) s.active = 0;
    *out = std::move(s);
    return true;
}

bool TabSession::load_from_file(const std::string& path) {
    std::string text;
    return read_text(path, &text) && decode(text, this);
}

bool TabSession::save_to_file(const std::string& path) const { return write_text(path, encode()); }

} // namespace prospero::browser
