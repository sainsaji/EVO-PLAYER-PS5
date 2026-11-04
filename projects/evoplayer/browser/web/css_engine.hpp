// Prospero Browser - WPE WebKit Port for PlayStation 5
// CSS Parser and Cascading Style Rule Engine.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace prospero::web {

struct CssColor {
    uint8_t r = 203;
    uint8_t g = 213;
    uint8_t b = 225;
    uint8_t a = 255;

    bool is_transparent() const { return a == 0; }
    uint32_t to_u32() const {
        return (static_cast<uint32_t>(r) << 24) |
               (static_cast<uint32_t>(g) << 16) |
               (static_cast<uint32_t>(b) << 8)  |
               static_cast<uint32_t>(a);
    }

    static bool parse(std::string_view str, CssColor& out);
};

struct BlockStyle {
    CssColor color{203, 213, 225, 255};       // Default text color (slate-300)
    bool has_color = false;

    CssColor background_color{0, 0, 0, 0};     // Default transparent
    bool has_bg = false;

    CssColor border_color{51, 65, 85, 255};    // Default border slate-700
    bool has_border = false;
    float border_width = 0.0f;
    float border_radius = 0.0f;

    float font_size = 18.0f;
    bool has_font_size = false;

    bool is_bold = false;
    bool has_bold = false;
    bool is_italic = false;
    bool is_underline = false;

    uint8_t text_align = 0; // 0 = Left, 1 = Center, 2 = Right
    bool has_align = false;

    bool display_none = false;

    float padding_x = 0.0f;
    float padding_y = 0.0f;
    float margin_top = 0.0f;
    float margin_bottom = 12.0f;

    void merge_from(const BlockStyle& other);
};

struct CssRule {
    std::string selector; // Tag, .class, #id, or tag.class
    BlockStyle style;
    int specificity = 0; // 1 for tag, 10 for .class, 100 for #id
};

class CssEngine {
public:
    CssEngine();
    ~CssEngine();

    void reset();
    void load_default_rules();
    void parse_stylesheet(std::string_view css);

    static BlockStyle parse_declarations(std::string_view decls);

    BlockStyle compute_style(std::string_view tag,
                             std::string_view class_attr,
                             std::string_view id_attr,
                             std::string_view inline_style = "");

    const CssColor& body_background() const { return body_bg_; }
    bool has_body_background() const { return has_body_bg_; }

private:
    static std::string trim(std::string_view text);
    static std::string to_lower(std::string_view text);

    std::vector<CssRule> rules_;
    CssColor body_bg_{11, 15, 25, 255};
    bool has_body_bg_ = false;
};

} // namespace prospero::web
