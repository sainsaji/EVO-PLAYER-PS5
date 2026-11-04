// Prospero Browser - WPE WebKit Port for PlayStation 5
// Bookmarks Management and Storage implementation.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later

#include "bookmarks.hpp"
#include <algorithm>
#include <cstdio>

namespace prospero::browser {

Bookmarks::Bookmarks() {
    load_defaults();
}

void Bookmarks::load_defaults() {
    items_ = {
        {"DuckDuckGo", "https://html.duckduckgo.com/html/", "🦆", "Search"},
        {"Wikipedia", "https://en.m.wikipedia.org", "📚", "Reference"},
        {"Hacker News", "https://news.ycombinator.com", "📰", "Tech"},
        {"Old Reddit", "https://old.reddit.com", "💬", "Community"},
        {"GitHub", "https://github.com", "🐙", "Development"},
        {"Homebrew Page", "https://homebrew.page", "🏠", "Console"},
        {"YouTube", "https://youtube.com", "▶️", "Media"},
        {"Internet Archive", "https://archive.org", "🏛️", "Reference"},
    };
}

bool Bookmarks::is_bookmarked(const std::string& url) const {
    return std::any_of(items_.begin(), items_.end(), [&](const BookmarkItem& item) {
        return item.url == url;
    });
}

void Bookmarks::add_bookmark(const std::string& title, const std::string& url,
                             const std::string& icon, const std::string& category) {
    if (is_bookmarked(url)) return;
    items_.push_back({title, url, icon, category});
}

bool Bookmarks::remove_bookmark(const std::string& url) {
    auto it = std::remove_if(items_.begin(), items_.end(), [&](const BookmarkItem& item) {
        return item.url == url;
    });
    if (it != items_.end()) {
        items_.erase(it, items_.end());
        return true;
    }
    return false;
}

bool Bookmarks::load_from_file(const std::string& path) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;

    std::fseek(fp, 0, SEEK_END);
    long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(fp);
        return false;
    }

    std::string content(static_cast<std::size_t>(size), '\0');
    if (size > 0) {
        std::size_t read = std::fread(content.data(), 1, static_cast<std::size_t>(size), fp);
        content.resize(read);
    }
    std::fclose(fp);

    items_.clear();
    std::size_t start = 0;
    while (start < content.size()) {
        std::size_t end = content.find('\n', start);
        if (end == std::string::npos) end = content.size();
        std::string line = content.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        start = end + 1;

        if (line.empty() || line[0] == '#') continue;

        std::size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        std::size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        std::size_t t3 = line.find('\t', t2 + 1);
        if (t3 == std::string::npos) continue;

        std::string title = line.substr(0, t1);
        std::string url = line.substr(t1 + 1, t2 - t1 - 1);
        std::string icon = line.substr(t2 + 1, t3 - t2 - 1);
        std::string category = line.substr(t3 + 1);
        items_.push_back({title, url, icon, category});
    }

    if (items_.empty()) {
        load_defaults();
    }
    return true;
}

bool Bookmarks::save_to_file(const std::string& path) const {
    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;

    for (const auto& item : items_) {
        std::fprintf(fp, "%s\t%s\t%s\t%s\n",
                     item.title.c_str(),
                     item.url.c_str(),
                     item.icon.c_str(),
                     item.category.c_str());
    }
    std::fclose(fp);
    return true;
}

} // namespace prospero::browser
