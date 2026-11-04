// Prospero Browser - WPE WebKit Port for PlayStation 5
// HTML Document Parser and Visual Layout Engine implementation.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later

#include "html_document.hpp"
#include "../browser/navigation.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace prospero::web {

namespace {

bool tag_matches(std::string_view tag, std::string_view name) {
    if (tag.size() < name.size()) return false;
    for (size_t i = 0; i < name.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(tag[i])) !=
            std::tolower(static_cast<unsigned char>(name[i]))) {
            return false;
        }
    }
    if (tag.size() == name.size()) return true;
    char next = tag[name.size()];
    return std::isspace(static_cast<unsigned char>(next)) || next == '>' || next == '/';
}

std::string extract_attribute(std::string_view tag, std::string_view attr_name) {
    size_t search_pos = 0;
    while (search_pos < tag.size()) {
        auto pos = tag.find(attr_name, search_pos);
        if (pos == std::string_view::npos) return "";

        if (pos > 0 && !std::isspace(static_cast<unsigned char>(tag[pos - 1]))) {
            search_pos = pos + 1;
            continue;
        }

        pos += attr_name.size();
        while (pos < tag.size() && (tag[pos] == ' ' || tag[pos] == '=' || tag[pos] == '\t')) {
            ++pos;
        }
        if (pos >= tag.size()) return "";

        char quote = 0;
        if (tag[pos] == '"' || tag[pos] == '\'') {
            quote = tag[pos];
            ++pos;
        }

        size_t end_pos = pos;
        while (end_pos < tag.size()) {
            if (quote != 0) {
                if (tag[end_pos] == quote) break;
            } else {
                if (std::isspace(static_cast<unsigned char>(tag[end_pos])) || tag[end_pos] == '>') break;
            }
            ++end_pos;
        }

        return std::string(tag.substr(pos, end_pos - pos));
    }
    return "";
}

size_t find_case_insensitive(std::string_view haystack, std::string_view needle, size_t start_pos = 0) {
    if (needle.empty()) return start_pos;
    if (needle.size() > haystack.size() || start_pos > haystack.size() - needle.size()) {
        return std::string_view::npos;
    }

    for (size_t i = start_pos; i <= haystack.size() - needle.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) return i;
    }
    return std::string_view::npos;
}

size_t find_closing_tag(std::string_view html, std::string_view tag_name, size_t start_pos) {
    size_t pos = start_pos;
    while (pos < html.size()) {
        size_t close_start = html.find("</", pos);
        if (close_start == std::string_view::npos) return std::string_view::npos;

        size_t after_slash = close_start + 2;
        while (after_slash < html.size() && std::isspace(static_cast<unsigned char>(html[after_slash]))) {
            after_slash++;
        }

        if (after_slash + tag_name.size() <= html.size()) {
            bool matches = true;
            for (size_t j = 0; j < tag_name.size(); ++j) {
                if (std::tolower(static_cast<unsigned char>(html[after_slash + j])) !=
                    std::tolower(static_cast<unsigned char>(tag_name[j]))) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                size_t after_tag = after_slash + tag_name.size();
                if (after_tag < html.size()) {
                    char c = html[after_tag];
                    if (c == '>' || std::isspace(static_cast<unsigned char>(c)) || c == '/') {
                        size_t gt = html.find('>', after_tag);
                        if (gt != std::string_view::npos) {
                            return gt + 1;
                        }
                    }
                }
            }
        }
        pos = close_start + 2;
    }
    return std::string_view::npos;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

} // namespace

HtmlDocument::HtmlDocument() = default;
HtmlDocument::~HtmlDocument() = default;

void HtmlDocument::clear() {
    title_ = "New Tab";
    url_ = "about:blank";
    blocks_.clear();
    links_.clear();
    total_height_ = 0.0f;
    max_scroll_y_ = 0.0f;
}

std::string HtmlDocument::trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

std::string HtmlDocument::decode_entities(std::string_view text) {
    std::string out;
    out.reserve(text.size());

    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '&') {
            auto semi = text.find(';', i);
            if (semi != std::string_view::npos && semi - i <= 10) {
                std::string_view ent = text.substr(i, semi - i + 1);
                // Named entities
                if (ent == "&amp;") { out.push_back('&'); i = semi; continue; }
                if (ent == "&lt;") { out.push_back('<'); i = semi; continue; }
                if (ent == "&gt;") { out.push_back('>'); i = semi; continue; }
                if (ent == "&quot;") { out.push_back('"'); i = semi; continue; }
                if (ent == "&apos;") { out.push_back('\''); i = semi; continue; }
                if (ent == "&nbsp;") { out.push_back(' '); i = semi; continue; }
                if (ent == "&bull;") { out.push_back('*'); i = semi; continue; }
                if (ent == "&ndash;" || ent == "&mdash;") { out.push_back('-'); i = semi; continue; }
                if (ent == "&hellip;") { out.append("..."); i = semi; continue; }
                if (ent == "&copy;") { out.append("(c)"); i = semi; continue; }
                if (ent == "&reg;") { out.append("(R)"); i = semi; continue; }
                if (ent == "&trade;") { out.append("(TM)"); i = semi; continue; }
                if (ent == "&lsquo;" || ent == "&rsquo;") { out.push_back('\''); i = semi; continue; }
                if (ent == "&ldquo;" || ent == "&rdquo;") { out.push_back('"'); i = semi; continue; }
                if (ent == "&euro;") { out.append("EUR"); i = semi; continue; }
                if (ent == "&pound;") { out.append("GBP"); i = semi; continue; }
                if (ent == "&yen;") { out.append("JPY"); i = semi; continue; }
                if (ent == "&deg;") { out.append("°"); i = semi; continue; }
                if (ent == "&plusmn;") { out.append("+/-"); i = semi; continue; }
                if (ent == "&times;") { out.append("x"); i = semi; continue; }
                if (ent == "&divide;") { out.append("/"); i = semi; continue; }
                if (ent == "&laquo;") { out.append("<<"); i = semi; continue; }
                if (ent == "&raquo;") { out.append(">>"); i = semi; continue; }

                // Numeric hex: &#x[0-9a-fA-F]+; or &#X...
                if (ent.size() >= 4 && (ent[1] == '#' && (ent[2] == 'x' || ent[2] == 'X'))) {
                    std::string_view hex_str = ent.substr(3, ent.size() - 4);
                    uint32_t cp = 0;
                    bool valid = !hex_str.empty();
                    for (char c : hex_str) {
                        if (c >= '0' && c <= '9') cp = (cp << 4) | (c - '0');
                        else if (c >= 'a' && c <= 'f') cp = (cp << 4) | (c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') cp = (cp << 4) | (c - 'A' + 10);
                        else { valid = false; break; }
                    }
                    if (valid && cp > 0) {
                        append_utf8(out, cp);
                        i = semi;
                        continue;
                    }
                }

                // Numeric decimal: &#[0-9]+;
                if (ent.size() >= 3 && ent[1] == '#') {
                    std::string_view dec_str = ent.substr(2, ent.size() - 3);
                    uint32_t cp = 0;
                    bool valid = !dec_str.empty();
                    for (char c : dec_str) {
                        if (c >= '0' && c <= '9') cp = cp * 10 + (c - '0');
                        else { valid = false; break; }
                    }
                    if (valid && cp > 0) {
                        append_utf8(out, cp);
                        i = semi;
                        continue;
                    }
                }
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

bool HtmlDocument::parse_and_layout(const std::string& html,
                                    const std::string& base_url,
                                    float viewport_width,
                                    float viewport_height) {
    clear();
    url_ = base_url;

    // 1. Extract <title>
    size_t title_open = find_case_insensitive(html, "<title");
    if (title_open != std::string_view::npos) {
        size_t tag_close = html.find('>', title_open);
        if (tag_close != std::string_view::npos) {
            size_t title_end = find_case_insensitive(html, "</title", tag_close);
            if (title_end != std::string_view::npos) {
                title_ = trim(decode_entities(html.substr(tag_close + 1, title_end - tag_close - 1)));
            }
        }
    }
    if (title_.empty() || title_ == "New Tab") {
        title_ = browser::Navigation::extract_host(base_url);
        if (title_.empty()) title_ = "Web Page";
    }

    // 2. Locate starting position in document: start at <body...>, or after </head>
    size_t i = 0;
    const size_t len = html.size();
    size_t body_pos = find_case_insensitive(html, "<body");
    if (body_pos != std::string_view::npos) {
        size_t body_close = html.find('>', body_pos);
        if (body_close != std::string_view::npos) {
            i = body_close + 1;
        }
    } else {
        size_t head_close = find_case_insensitive(html, "</head");
        if (head_close != std::string_view::npos) {
            size_t gt = html.find('>', head_close);
            if (gt != std::string_view::npos) {
                i = gt + 1;
            }
        }
    }

    // 3. Tokenize and extract structured blocks
    std::string active_link_url;
    BlockType cur_type = BlockType::Paragraph;
    std::string cur_text;

    auto flush_block = [&]() {
        std::string cleaned = trim(decode_entities(cur_text));
        if (!cleaned.empty()) {
            DocumentBlock b;
            b.type = cur_type;
            b.text = cleaned;
            if (!active_link_url.empty()) {
                b.link_url = active_link_url;
                if (b.type == BlockType::Paragraph) {
                    b.type = BlockType::Link;
                }
            }
            blocks_.push_back(std::move(b));
        }
        cur_text.clear();
        cur_type = BlockType::Paragraph;
    };

    while (i < len) {
        if (blocks_.size() >= 1000) {
            DocumentBlock footer_b;
            footer_b.type = BlockType::Paragraph;
            footer_b.text = "--- End of Document (Content Display Limit Reached) ---";
            blocks_.push_back(std::move(footer_b));
            break;
        }

        if (html[i] == '<') {
            // Check for comments: <!-- ... -->
            if (i + 4 <= len && html.substr(i, 4) == "<!--") {
                auto end_comment = html.find("-->", i + 4);
                if (end_comment != std::string::npos) {
                    i = end_comment + 3;
                    continue;
                }
            }

            auto close_angle = html.find('>', i);
            if (close_angle == std::string::npos) break;

            std::string_view tag = std::string_view(html).substr(i + 1, close_angle - i - 1);
            i = close_angle + 1;

            // Ignore scripts, styles, svg, and other non-content tags completely
            if (tag_matches(tag, "script")) {
                size_t close = find_closing_tag(html, "script", i);
                if (close != std::string_view::npos) i = close;
                continue;
            }
            if (tag_matches(tag, "style")) {
                size_t close = find_closing_tag(html, "style", i);
                if (close != std::string_view::npos) i = close;
                continue;
            }
            if (tag_matches(tag, "svg")) {
                size_t close = find_closing_tag(html, "svg", i);
                if (close != std::string_view::npos) i = close;
                continue;
            }
            if (tag_matches(tag, "noscript")) {
                size_t close = find_closing_tag(html, "noscript", i);
                if (close != std::string_view::npos) i = close;
                continue;
            }
            if (tag_matches(tag, "iframe")) {
                size_t close = find_closing_tag(html, "iframe", i);
                if (close != std::string_view::npos) i = close;
                continue;
            }
            if (tag_matches(tag, "template")) {
                size_t close = find_closing_tag(html, "template", i);
                if (close != std::string_view::npos) i = close;
                continue;
            }

            // Inline formatting tags: do not break paragraphs or links
            if (tag_matches(tag, "b") || tag_matches(tag, "/b") ||
                tag_matches(tag, "strong") || tag_matches(tag, "/strong") ||
                tag_matches(tag, "i") || tag_matches(tag, "/i") ||
                tag_matches(tag, "em") || tag_matches(tag, "/em") ||
                tag_matches(tag, "span") || tag_matches(tag, "/span") ||
                tag_matches(tag, "font") || tag_matches(tag, "/font") ||
                tag_matches(tag, "mark") || tag_matches(tag, "/mark") ||
                tag_matches(tag, "small") || tag_matches(tag, "/small") ||
                tag_matches(tag, "sub") || tag_matches(tag, "/sub") ||
                tag_matches(tag, "sup") || tag_matches(tag, "/sup")) {
                continue;
            }

            // Tag classification
            if (tag_matches(tag, "h1")) {
                flush_block();
                cur_type = BlockType::Heading1;
            } else if (tag_matches(tag, "h2")) {
                flush_block();
                cur_type = BlockType::Heading2;
            } else if (tag_matches(tag, "h3") || tag_matches(tag, "h4") ||
                       tag_matches(tag, "h5") || tag_matches(tag, "h6")) {
                flush_block();
                cur_type = BlockType::Heading3;
            } else if (tag_matches(tag, "a")) {
                flush_block();
                std::string href = extract_attribute(tag, "href");
                if (!href.empty()) {
                    active_link_url = browser::Navigation::resolve_link(base_url, href);
                    cur_type = BlockType::Link;
                }
            } else if (tag_matches(tag, "/a")) {
                flush_block();
                active_link_url.clear();
            } else if (tag_matches(tag, "pre") || tag_matches(tag, "code")) {
                flush_block();
                cur_type = BlockType::CodeBlock;
            } else if (tag_matches(tag, "/pre") || tag_matches(tag, "/code")) {
                flush_block();
            } else if (tag_matches(tag, "li")) {
                flush_block();
                cur_type = BlockType::ListItem;
            } else if (tag_matches(tag, "/li")) {
                flush_block();
            } else if (tag_matches(tag, "hr")) {
                flush_block();
                DocumentBlock b;
                b.type = BlockType::Divider;
                blocks_.push_back(std::move(b));
            } else if (tag_matches(tag, "p") || tag_matches(tag, "div") ||
                       tag_matches(tag, "section") || tag_matches(tag, "article") ||
                       tag_matches(tag, "tr") || tag_matches(tag, "main") ||
                       tag_matches(tag, "header") || tag_matches(tag, "footer")) {
                flush_block();
            } else if (tag_matches(tag, "br")) {
                cur_text.push_back('\n');
            } else if (tag_matches(tag, "input")) {
                std::string val = extract_attribute(tag, "value");
                if (!val.empty()) {
                    cur_text += " [" + val + "] ";
                }
            }
        } else {
            // Text character
            char c = html[i++];
            if (c == '\r' || c == '\n' || c == '\t') c = ' ';
            if (c == ' ' && !cur_text.empty() && cur_text.back() == ' ') {
                continue; // Collapse whitespace
            }
            cur_text.push_back(c);
        }
    }
    flush_block();

    // 4. Layout all extracted blocks into lines and bounding boxes
    layout_blocks(viewport_width, viewport_height);
    return true;
}

void HtmlDocument::layout_blocks(float viewport_width, float viewport_height) {
    links_.clear();

    const float content_width = std::min(std::max(400.0f, viewport_width - 120.0f), 1300.0f);
    const float start_x = std::max(60.0f, (viewport_width - content_width) / 2.0f);
    float cur_y = 36.0f;

    for (auto& block : blocks_) {
        float line_height = 26.0f;
        float char_width = 9.5f;
        float margin_top = 8.0f;
        float margin_bottom = 8.0f;

        switch (block.type) {
            case BlockType::Heading1:
                line_height = 42.0f;
                char_width = 17.0f;
                margin_top = 28.0f;
                margin_bottom = 16.0f;
                break;
            case BlockType::Heading2:
                line_height = 34.0f;
                char_width = 13.5f;
                margin_top = 22.0f;
                margin_bottom = 12.0f;
                break;
            case BlockType::Heading3:
                line_height = 28.0f;
                char_width = 11.5f;
                margin_top = 16.0f;
                margin_bottom = 8.0f;
                break;
            case BlockType::Paragraph:
                line_height = 26.0f;
                char_width = 9.5f;
                margin_top = 6.0f;
                margin_bottom = 6.0f;
                break;
            case BlockType::Link:
                line_height = 26.0f;
                char_width = 9.5f;
                margin_top = 6.0f;
                margin_bottom = 6.0f;
                break;
            case BlockType::CodeBlock:
                line_height = 22.0f;
                char_width = 9.0f;
                margin_top = 12.0f;
                margin_bottom = 12.0f;
                break;
            case BlockType::ListItem:
                line_height = 26.0f;
                char_width = 9.5f;
                margin_top = 4.0f;
                margin_bottom = 4.0f;
                break;
            case BlockType::Divider:
                margin_top = 18.0f;
                margin_bottom = 18.0f;
                break;
            case BlockType::Card:
                line_height = 26.0f;
                char_width = 9.5f;
                margin_top = 14.0f;
                margin_bottom = 14.0f;
                break;
        }

        cur_y += margin_top;

        if (block.type == BlockType::Divider) {
            block.box = Rect{start_x, cur_y, content_width, 2.0f};
            cur_y += 2.0f + margin_bottom;
            continue;
        }

        // Line-breaking word wrapper
        block.lines.clear();
        std::string current_line;
        float current_line_width = 0.0f;

        size_t word_start = 0;
        const size_t text_len = block.text.size();

        while (word_start < text_len) {
            size_t word_end = word_start;
            while (word_end < text_len && block.text[word_end] != ' ' && block.text[word_end] != '\n') {
                ++word_end;
            }

            std::string word = block.text.substr(word_start, word_end - word_start);
            float word_width = static_cast<float>(word.size()) * char_width;

            if (current_line_width + word_width + char_width > content_width && !current_line.empty()) {
                block.lines.push_back(current_line);
                current_line = word;
                current_line_width = word_width;
            } else {
                if (!current_line.empty()) {
                    current_line.push_back(' ');
                    current_line_width += char_width;
                }
                current_line += word;
                current_line_width += word_width;
            }

            if (word_end < text_len && block.text[word_end] == '\n') {
                block.lines.push_back(current_line);
                current_line.clear();
                current_line_width = 0.0f;
            }

            word_start = word_end + 1;
        }

        if (!current_line.empty()) {
            block.lines.push_back(current_line);
        }

        float block_height = std::max(line_height, static_cast<float>(block.lines.size()) * line_height);
        if (block.type == BlockType::CodeBlock || block.type == BlockType::Card) {
            block_height += 24.0f; // Padding
        }

        block.box = Rect{start_x, cur_y, content_width, block_height};

        // If link, register hit-testing target
        if (block.type == BlockType::Link && !block.link_url.empty()) {
            LinkTarget lt;
            lt.box = block.box;
            lt.url = block.link_url;
            lt.text = block.text;
            links_.push_back(std::move(lt));
        }

        cur_y += block_height + margin_bottom;
    }

    total_height_ = cur_y + 80.0f;
    max_scroll_y_ = std::max(0.0f, total_height_ - viewport_height);
}

const LinkTarget* HtmlDocument::hit_test_link(float page_x, float page_y) const {
    for (const auto& link : links_) {
        if (link.box.contains(page_x, page_y)) {
            return &link;
        }
    }
    return nullptr;
}

void HtmlDocument::create_start_page(float viewport_width, float viewport_height) {
    clear();
    title_ = "Prospero Browser - Start";
    url_ = "about:start";
    layout_blocks(viewport_width, viewport_height);
}

void HtmlDocument::create_version_page(float viewport_width, float viewport_height) {
    clear();
    title_ = "About Prospero Browser";
    url_ = "about:version";

    blocks_.push_back({BlockType::Heading1, "Prospero Browser", "", {}, {}});
    blocks_.push_back({BlockType::Paragraph,
        "Native high-performance WebKit web browser built for PlayStation 5 homebrew, "
        "featuring hardware accelerated rendering via EGL/OpenGL 4.6, DualSense virtual cursor, "
        "and ps5-homebrew-ui user interface.", "", {}, {}});
    blocks_.push_back({BlockType::Divider, "", "", {}, {}});

    blocks_.push_back({BlockType::Heading2, "System Architecture", "", {}, {}});
    blocks_.push_back({BlockType::ListItem, "Target Console: PlayStation 5 (Target: x86_64-sie-ps5)", "", {}, {}});
    blocks_.push_back({BlockType::ListItem, "Web Engine: WPE WebKit with native Prospero backend", "", {}, {}});
    blocks_.push_back({BlockType::ListItem, "Graphics: EGL 1.4 / Mesa 26.2.0 Core Profile + AMD AGC", "", {}, {}});
    blocks_.push_back({BlockType::ListItem, "UI Framework: ps5-homebrew-ui with Instanced Signed Distance Field (SDF) fonts", "", {}, {}});
    blocks_.push_back({BlockType::ListItem, "Network Stack: Native PS5 libSceHttp + libSceSsl + libSceNet", "", {}, {}});
    blocks_.push_back({BlockType::ListItem, "Controller: DualSense Gamepad with analog cursor and smooth scrolling", "", {}, {}});
    blocks_.push_back({BlockType::ListItem, "Audio: PS5 48kHz Stereo AudioOut (libSceAudioOut)", "", {}, {}});
    blocks_.push_back({BlockType::Divider, "", "", {}, {}});

    blocks_.push_back({BlockType::Heading2, "Quick Navigation Links", "", {}, {}});
    blocks_.push_back({BlockType::Link, "Return to Start Page", "about:start", {}, {}});
    blocks_.push_back({BlockType::Link, "DualSense Controller Shortcuts", "about:help", {}, {}});
    blocks_.push_back({BlockType::Link, "PlayStation Homebrew Catalog", "https://homebrew.page", {}, {}});

    layout_blocks(viewport_width, viewport_height);
}

void HtmlDocument::create_help_page(float viewport_width, float viewport_height) {
    clear();
    title_ = "DualSense Controller Shortcuts";
    url_ = "about:help";

    blocks_.push_back({BlockType::Heading1, "DualSense Controller Guide", "", {}, {}});
    blocks_.push_back({BlockType::Paragraph, "Control your web browsing experience with the PS5 DualSense Wireless Controller:", "", {}, {}});
    blocks_.push_back({BlockType::Divider, "", "", {}, {}});

    blocks_.push_back({BlockType::Heading2, "Navigation & Browsing", "", {}, {}});
    blocks_.push_back({BlockType::ListItem, "Left Stick: Move the virtual cursor across the page and toolbar", "", {}, {}});
    blocks_.push_back({BlockType::ListItem, "Cross (X): Click links, buttons, speed dial cards, and focus address bar", "", {}, {}});
    blocks_.push_back({BlockType::ListItem, "Circle (O): Go back to previous page in history, or close drawers", "", {}, {}});
    blocks_.push_back({BlockType::ListItem, "Triangle (Triangle): Focus address bar (omnibox) for typing", "", {}, {}});
    blocks_.push_back({BlockType::ListItem, "Right Stick: Smooth kinetic scrolling up and down the web page", "", {}, {}});
    blocks_.push_back({BlockType::ListItem, "L2 / R2 Triggers: Analog progressive page scrolling", "", {}, {}});
    blocks_.push_back({BlockType::ListItem, "L1 / R1 Bumpers: Switch between active browser tabs", "", {}, {}});
    blocks_.push_back({BlockType::ListItem, "Square (Square): Toggle Bookmarks & History drawer", "", {}, {}});
    blocks_.push_back({BlockType::ListItem, "Options (Options): Open Browser Settings modal", "", {}, {}});
    blocks_.push_back({BlockType::Divider, "", "", {}, {}});

    blocks_.push_back({BlockType::Link, "Back to Start Page", "about:start", {}, {}});

    layout_blocks(viewport_width, viewport_height);
}

void HtmlDocument::create_error_page(const std::string& failed_url,
                                     const std::string& error_msg,
                                     int status_code,
                                     float viewport_width,
                                     float viewport_height) {
    clear();
    title_ = "Page Load Error";
    url_ = failed_url;

    blocks_.push_back({BlockType::Heading1, "Unable to Load Web Page", "", {}, {}});
    char status_info[128];
    if (status_code > 0) {
        std::snprintf(status_info, sizeof(status_info), "HTTP Status: %d", status_code);
    } else {
        std::snprintf(status_info, sizeof(status_info), "Connection Status: Failed");
    }
    blocks_.push_back({BlockType::Heading3, status_info, "", {}, {}});

    std::string detail = error_msg.empty() ? "The server could not be reached or timed out." : error_msg;
    blocks_.push_back({BlockType::Paragraph, "Error: " + detail, "", {}, {}});
    blocks_.push_back({BlockType::Paragraph, "Requested URL: " + failed_url, "", {}, {}});
    blocks_.push_back({BlockType::Divider, "", "", {}, {}});

    blocks_.push_back({BlockType::Heading2, "Troubleshooting Suggestions", "", {}, {}});
    blocks_.push_back({BlockType::ListItem, "Check your PlayStation 5 Internet connection in Settings > Network", "", {}, {}});
    blocks_.push_back({BlockType::ListItem, "Verify that the URL address is spelled correctly in the Omnibox", "", {}, {}});
    blocks_.push_back({BlockType::ListItem, "Ensure DNS is configured correctly on the console", "", {}, {}});
    blocks_.push_back({BlockType::Divider, "", "", {}, {}});

    blocks_.push_back({BlockType::Link, "Try Loading Again (Reload)", failed_url, {}, {}});
    blocks_.push_back({BlockType::Link, "Return to Start Page", "about:start", {}, {}});
    blocks_.push_back({BlockType::Link, "Controller Shortcuts Guide", "about:help", {}, {}});

    layout_blocks(viewport_width, viewport_height);
}

void HtmlDocument::create_crash_page(const std::string& url,
                                     const std::string& reason,
                                     float viewport_width,
                                     float viewport_height) {
    clear();
    title_ = "Page Crashed";
    url_ = url;

    blocks_.push_back({BlockType::Heading1, "This Page Crashed", "", {}, {}});
    blocks_.push_back({BlockType::Paragraph, reason.empty() ? "The page's content process stopped." : reason,
                       "", {}, {}});
    blocks_.push_back({BlockType::Paragraph, "Page: " + url, "", {}, {}});
    blocks_.push_back({BlockType::Paragraph,
                       "Every tab shares one content process, so other open tabs reload when you visit them. "
                       "Heavy pages run out of memory more easily with many tabs open.", "", {}, {}});
    blocks_.push_back({BlockType::Divider, "", "", {}, {}});

    blocks_.push_back({BlockType::Link, "Reload This Page", url, {}, {}});
    blocks_.push_back({BlockType::Link, "Return to Start Page", "about:start", {}, {}});

    layout_blocks(viewport_width, viewport_height);
}

} // namespace prospero::web
