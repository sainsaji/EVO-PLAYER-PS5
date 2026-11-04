// Prospero Browser - WPE WebKit Port for PlayStation 5
// HTML Document Parser and Visual Layout Engine.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace prospero::web {

enum class BlockType {
    Heading1,
    Heading2,
    Heading3,
    Paragraph,
    Link,
    CodeBlock,
    ListItem,
    Divider,
    Card
};

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    bool contains(float px, float py) const {
        return px >= x && px <= (x + width) && py >= y && py <= (y + height);
    }
};

struct LinkTarget {
    Rect box;
    std::string url;
    std::string text;
};

struct DocumentBlock {
    BlockType type = BlockType::Paragraph;
    std::string text;
    std::string link_url;
    std::vector<std::string> lines;
    Rect box;
};

class HtmlDocument {
public:
    HtmlDocument();
    ~HtmlDocument();

    // Parse HTML string and compute layout for viewport width/height
    bool parse_and_layout(const std::string& html,
                          const std::string& base_url,
                          float viewport_width,
                          float viewport_height);

    // Create built-in pages
    void create_start_page(float viewport_width, float viewport_height);
    void create_version_page(float viewport_width, float viewport_height);
    void create_help_page(float viewport_width, float viewport_height);
    void create_error_page(const std::string& failed_url,
                           const std::string& error_msg,
                           int status_code,
                           float viewport_width,
                           float viewport_height);
    // The web content process died while showing `url` (crash, out of memory).
    void create_crash_page(const std::string& url,
                           const std::string& reason,
                           float viewport_width,
                           float viewport_height);

    // Hit-testing for cursor hover / click
    const LinkTarget* hit_test_link(float page_x, float page_y) const;

    // Document properties
    const std::string& title() const { return title_; }
    const std::string& url() const { return url_; }
    const std::vector<DocumentBlock>& blocks() const { return blocks_; }
    const std::vector<LinkTarget>& links() const { return links_; }
    float total_height() const { return total_height_; }
    float max_scroll_y() const { return max_scroll_y_; }
    bool is_empty() const { return blocks_.empty(); }

    void clear();

private:
    void layout_blocks(float viewport_width, float viewport_height);
    static std::string decode_entities(std::string_view text);
    static std::string trim(std::string_view text);

    std::string title_{"New Tab"};
    std::string url_{"about:blank"};
    std::vector<DocumentBlock> blocks_;
    std::vector<LinkTarget> links_;
    float total_height_{0.0f};
    float max_scroll_y_{0.0f};
};

} // namespace prospero::web
