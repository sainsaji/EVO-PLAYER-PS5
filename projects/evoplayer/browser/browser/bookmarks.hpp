// Prospero Browser - WPE WebKit Port for PlayStation 5
// Bookmarks Management and Storage.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

namespace prospero::browser {

struct BookmarkItem {
    std::string title;
    std::string url;
    std::string icon;
    std::string category;
};

class Bookmarks {
public:
    Bookmarks();

    void load_defaults();
    bool load_from_file(const std::string& path);
    bool save_to_file(const std::string& path) const;

    const std::vector<BookmarkItem>& items() const { return items_; }

    bool is_bookmarked(const std::string& url) const;
    void add_bookmark(const std::string& title, const std::string& url,
                      const std::string& icon = "🌐", const std::string& category = "User");
    bool remove_bookmark(const std::string& url);

private:
    std::vector<BookmarkItem> items_;
};

} // namespace prospero::browser
