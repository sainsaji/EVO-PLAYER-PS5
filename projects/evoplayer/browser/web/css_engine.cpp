// Prospero Browser - WPE WebKit Port for PlayStation 5
// CSS Parser and Cascading Style Rule Engine implementation.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later

#include "css_engine.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace prospero::web {

namespace {

struct NamedColor {
    const char* name;
    uint8_t r, g, b, a;
};

static const NamedColor kNamedColors[] = {
    {"white", 255, 255, 255, 255},
    {"black", 0, 0, 0, 255},
    {"red", 239, 68, 68, 255},
    {"green", 34, 197, 94, 255},
    {"blue", 59, 130, 246, 255},
    {"yellow", 234, 179, 8, 255},
    {"cyan", 6, 182, 212, 255},
    {"magenta", 217, 70, 239, 255},
    {"purple", 168, 85, 247, 255},
    {"orange", 249, 115, 22, 255},
    {"gray", 100, 116, 139, 255},
    {"grey", 100, 116, 139, 255},
    {"lightgray", 203, 213, 225, 255},
    {"lightgrey", 203, 213, 225, 255},
    {"darkgray", 51, 65, 85, 255},
    {"darkgrey", 51, 65, 85, 255},
    {"slate", 71, 85, 105, 255},
    {"zinc", 63, 63, 70, 255},
    {"silver", 192, 192, 192, 255},
    {"gold", 245, 158, 11, 255},
    {"indigo", 99, 102, 241, 255},
    {"violet", 139, 92, 246, 255},
    {"pink", 236, 72, 153, 255},
    {"teal", 20, 184, 166, 255},
    {"lime", 132, 204, 22, 255},
    {"navy", 15, 23, 42, 255},
    {"transparent", 0, 0, 0, 0},
    {"none", 0, 0, 0, 0},
    {nullptr, 0, 0, 0, 0}
};

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

float parse_length(std::string_view val, float base_size = 18.0f) {
    while (!val.empty() && std::isspace(static_cast<unsigned char>(val.front()))) {
        val.remove_prefix(1);
    }
    if (val.empty()) return 0.0f;

    char buf[32];
    size_t copy_len = std::min(val.size(), sizeof(buf) - 1);
    std::memcpy(buf, val.data(), copy_len);
    buf[copy_len] = '\0';

    char* end = nullptr;
    float num = std::strtof(buf, &end);
    if (!end || end == buf) return 0.0f;

    std::string_view unit = val.substr(end - buf);
    while (!unit.empty() && std::isspace(static_cast<unsigned char>(unit.front()))) {
        unit.remove_prefix(1);
    }

    if (unit == "px") return num;
    if (unit == "pt") return num * 1.333f;
    if (unit == "em" || unit == "rem") return num * base_size;
    if (unit == "%") return (num / 100.0f) * base_size;
    return num;
}

} // namespace

bool CssColor::parse(std::string_view str, CssColor& out) {
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front()))) str.remove_prefix(1);
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) str.remove_suffix(1);
    if (str.empty()) return false;

    // 1. Hex color
    if (str.front() == '#') {
        std::string_view hex = str.substr(1);
        if (hex.size() == 3) { // #rgb
            int r = hex_val(hex[0]);
            int g = hex_val(hex[1]);
            int b = hex_val(hex[2]);
            if (r >= 0 && g >= 0 && b >= 0) {
                out.r = static_cast<uint8_t>(r * 17);
                out.g = static_cast<uint8_t>(g * 17);
                out.b = static_cast<uint8_t>(b * 17);
                out.a = 255;
                return true;
            }
        } else if (hex.size() == 4) { // #rgba
            int r = hex_val(hex[0]);
            int g = hex_val(hex[1]);
            int b = hex_val(hex[2]);
            int a = hex_val(hex[3]);
            if (r >= 0 && g >= 0 && b >= 0 && a >= 0) {
                out.r = static_cast<uint8_t>(r * 17);
                out.g = static_cast<uint8_t>(g * 17);
                out.b = static_cast<uint8_t>(b * 17);
                out.a = static_cast<uint8_t>(a * 17);
                return true;
            }
        } else if (hex.size() == 6) { // #rrggbb
            int r0 = hex_val(hex[0]), r1 = hex_val(hex[1]);
            int g0 = hex_val(hex[2]), g1 = hex_val(hex[3]);
            int b0 = hex_val(hex[4]), b1 = hex_val(hex[5]);
            if (r0 >= 0 && r1 >= 0 && g0 >= 0 && g1 >= 0 && b0 >= 0 && b1 >= 0) {
                out.r = static_cast<uint8_t>((r0 << 4) | r1);
                out.g = static_cast<uint8_t>((g0 << 4) | g1);
                out.b = static_cast<uint8_t>((b0 << 4) | b1);
                out.a = 255;
                return true;
            }
        } else if (hex.size() == 8) { // #rrggbbaa
            int r0 = hex_val(hex[0]), r1 = hex_val(hex[1]);
            int g0 = hex_val(hex[2]), g1 = hex_val(hex[3]);
            int b0 = hex_val(hex[4]), b1 = hex_val(hex[5]);
            int a0 = hex_val(hex[6]), a1 = hex_val(hex[7]);
            if (r0 >= 0 && r1 >= 0 && g0 >= 0 && g1 >= 0 && b0 >= 0 && b1 >= 0 && a0 >= 0 && a1 >= 0) {
                out.r = static_cast<uint8_t>((r0 << 4) | r1);
                out.g = static_cast<uint8_t>((g0 << 4) | g1);
                out.b = static_cast<uint8_t>((b0 << 4) | b1);
                out.a = static_cast<uint8_t>((a0 << 4) | a1);
                return true;
            }
        }
        return false;
    }

    // 2. rgb(...) or rgba(...)
    if (str.rfind("rgb", 0) == 0) {
        auto open_paren = str.find('(');
        auto close_paren = str.rfind(')');
        if (open_paren != std::string_view::npos && close_paren != std::string_view::npos && close_paren > open_paren) {
            std::string_view inner = str.substr(open_paren + 1, close_paren - open_paren - 1);
            int r = 0, g = 0, b = 0;
            float a = 1.0f;
            char dummy = 0;
            char buf[64];
            size_t copy_len = std::min(inner.size(), sizeof(buf) - 1);
            std::memcpy(buf, inner.data(), copy_len);
            buf[copy_len] = '\0';

            // Replace commas with spaces for parsing
            for (size_t k = 0; k < copy_len; ++k) {
                if (buf[k] == ',' || buf[k] == '/') buf[k] = ' ';
            }

            int n = std::sscanf(buf, "%d %d %d %f %c", &r, &g, &b, &a, &dummy);
            if (n >= 3) {
                out.r = static_cast<uint8_t>(std::clamp(r, 0, 255));
                out.g = static_cast<uint8_t>(std::clamp(g, 0, 255));
                out.b = static_cast<uint8_t>(std::clamp(b, 0, 255));
                out.a = static_cast<uint8_t>(std::clamp(static_cast<int>(a * 255.0f), 0, 255));
                return true;
            }
        }
    }

    // 3. Named color lookup
    std::string lower;
    lower.reserve(str.size());
    for (char c : str) lower.push_back(std::tolower(static_cast<unsigned char>(c)));

    for (int i = 0; kNamedColors[i].name != nullptr; ++i) {
        if (lower == kNamedColors[i].name) {
            out.r = kNamedColors[i].r;
            out.g = kNamedColors[i].g;
            out.b = kNamedColors[i].b;
            out.a = kNamedColors[i].a;
            return true;
        }
    }

    return false;
}

void BlockStyle::merge_from(const BlockStyle& other) {
    if (other.has_color) {
        color = other.color;
        has_color = true;
    }
    if (other.has_bg) {
        background_color = other.background_color;
        has_bg = true;
    }
    if (other.has_border) {
        border_color = other.border_color;
        border_width = other.border_width;
        has_border = true;
    }
    if (other.border_radius > 0.0f) {
        border_radius = other.border_radius;
    }
    if (other.has_font_size) {
        font_size = other.font_size;
        has_font_size = true;
    }
    if (other.has_bold) {
        is_bold = other.is_bold;
        has_bold = true;
    }
    if (other.is_italic) is_italic = true;
    if (other.is_underline) is_underline = true;
    if (other.has_align) {
        text_align = other.text_align;
        has_align = true;
    }
    if (other.display_none) display_none = true;
    if (other.padding_x > 0.0f) padding_x = other.padding_x;
    if (other.padding_y > 0.0f) padding_y = other.padding_y;
    if (other.margin_top > 0.0f) margin_top = other.margin_top;
    if (other.margin_bottom != 12.0f) margin_bottom = other.margin_bottom;
}

std::string CssEngine::trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return std::string(text);
}

std::string CssEngine::to_lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) out.push_back(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

BlockStyle CssEngine::parse_declarations(std::string_view decls) {
    BlockStyle style;
    size_t pos = 0;

    while (pos < decls.size()) {
        auto semi = decls.find(';', pos);
        std::string_view item = (semi == std::string_view::npos) ?
            decls.substr(pos) : decls.substr(pos, semi - pos);
        pos = (semi == std::string_view::npos) ? decls.size() : semi + 1;

        auto colon = item.find(':');
        if (colon == std::string_view::npos) continue;

        std::string prop = to_lower(trim(item.substr(0, colon)));
        std::string_view val = item.substr(colon + 1);
        while (!val.empty() && std::isspace(static_cast<unsigned char>(val.front()))) val.remove_prefix(1);
        while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back()))) val.remove_suffix(1);
        if (prop.empty() || val.empty()) continue;

        if (prop == "color") {
            CssColor c;
            if (CssColor::parse(val, c)) {
                style.color = c;
                style.has_color = true;
            }
        } else if (prop == "background-color" || prop == "background") {
            CssColor c;
            if (CssColor::parse(val, c)) {
                style.background_color = c;
                style.has_bg = true;
            }
        } else if (prop == "border-color") {
            CssColor c;
            if (CssColor::parse(val, c)) {
                style.border_color = c;
                style.has_border = true;
                if (style.border_width <= 0.0f) style.border_width = 1.0f;
            }
        } else if (prop == "border-width") {
            style.border_width = parse_length(val);
            style.has_border = true;
        } else if (prop == "border") {
            // e.g. "1px solid #334155" or "2px solid blue"
            auto sp1 = val.find(' ');
            if (sp1 != std::string_view::npos) {
                style.border_width = parse_length(val.substr(0, sp1));
                auto sp2 = val.find(' ', sp1 + 1);
                std::string_view color_part = (sp2 != std::string_view::npos) ? val.substr(sp2 + 1) : val.substr(sp1 + 1);
                CssColor c;
                if (CssColor::parse(color_part, c)) {
                    style.border_color = c;
                    style.has_border = true;
                }
            }
        } else if (prop == "border-radius") {
            style.border_radius = parse_length(val);
        } else if (prop == "font-size") {
            std::string val_l = to_lower(val);
            if (val_l == "xx-large") style.font_size = 32.0f;
            else if (val_l == "x-large") style.font_size = 26.0f;
            else if (val_l == "large") style.font_size = 22.0f;
            else if (val_l == "medium") style.font_size = 18.0f;
            else if (val_l == "small") style.font_size = 15.0f;
            else if (val_l == "x-small") style.font_size = 12.0f;
            else style.font_size = parse_length(val);
            style.has_font_size = true;
        } else if (prop == "font-weight") {
            std::string val_l = to_lower(val);
            if (val_l == "bold" || val_l == "bolder" || val_l == "600" || val_l == "700" || val_l == "800" || val_l == "900") {
                style.is_bold = true;
                style.has_bold = true;
            } else if (val_l == "normal" || val_l == "lighter" || val_l == "400") {
                style.is_bold = false;
                style.has_bold = true;
            }
        } else if (prop == "font-style") {
            if (to_lower(val) == "italic" || to_lower(val) == "oblique") {
                style.is_italic = true;
            }
        } else if (prop == "text-decoration") {
            if (to_lower(val).find("underline") != std::string::npos) {
                style.is_underline = true;
            }
        } else if (prop == "text-align") {
            std::string val_l = to_lower(val);
            if (val_l == "center") {
                style.text_align = 1;
                style.has_align = true;
            } else if (val_l == "right") {
                style.text_align = 2;
                style.has_align = true;
            } else if (val_l == "left") {
                style.text_align = 0;
                style.has_align = true;
            }
        } else if (prop == "display") {
            if (to_lower(val) == "none") {
                style.display_none = true;
            }
        } else if (prop == "padding") {
            float p = parse_length(val);
            style.padding_x = p;
            style.padding_y = p;
        } else if (prop == "padding-left" || prop == "padding-right") {
            style.padding_x = parse_length(val);
        } else if (prop == "padding-top" || prop == "padding-bottom") {
            style.padding_y = parse_length(val);
        } else if (prop == "margin-top") {
            style.margin_top = parse_length(val);
        } else if (prop == "margin-bottom") {
            style.margin_bottom = parse_length(val);
        } else if (prop == "opacity") {
            float op = std::strtof(std::string(val).c_str(), nullptr);
            op = std::clamp(op, 0.0f, 1.0f);
            style.color.a = static_cast<uint8_t>(style.color.a * op);
            style.background_color.a = static_cast<uint8_t>(style.background_color.a * op);
        }
    }

    return style;
}

CssEngine::CssEngine() {
    load_default_rules();
}

CssEngine::~CssEngine() = default;

void CssEngine::reset() {
    rules_.clear();
    body_bg_ = CssColor{11, 15, 25, 255};
    has_body_bg_ = false;
    load_default_rules();
}

void CssEngine::load_default_rules() {
    // User-agent console web defaults
    parse_stylesheet(
        "body { color: #cbd5e1; background-color: #0d1117; font-size: 18px; }"
        "h1 { color: #ffffff; font-size: 32px; font-weight: bold; margin-bottom: 16px; }"
        "h2 { color: #f1f5f9; font-size: 26px; font-weight: bold; margin-bottom: 12px; }"
        "h3 { color: #e2e8f0; font-size: 21px; font-weight: bold; margin-bottom: 8px; }"
        "h4, h5, h6 { color: #cbd5e1; font-size: 18px; font-weight: bold; margin-bottom: 6px; }"
        "p { color: #cbd5e1; font-size: 18px; margin-bottom: 12px; }"
        "a { color: #38bdf8; text-decoration: underline; }"
        "button { color: #ffffff; background-color: #3b82f6; border-radius: 6px; font-weight: bold; padding: 8px; }"
        "code, pre { color: #a5f3fc; background-color: #161b22; font-size: 15px; border-radius: 6px; padding: 6px; }"
        "blockquote { color: #94a3b8; border-color: #6366f1; border-width: 3px; padding-left: 14px; }"
        "hr { border-color: #334155; border-width: 1px; margin-bottom: 16px; }"
        "table { border-color: #334155; border-width: 1px; border-radius: 6px; }"
        "th { color: #f8fafc; font-weight: bold; background-color: #1e293b; padding: 8px; }"
        "td { color: #cbd5e1; padding: 8px; }"
    );
}

void CssEngine::parse_stylesheet(std::string_view css) {
    size_t i = 0;
    const size_t len = css.size();

    while (i < len) {
        // Skip whitespace
        while (i < len && std::isspace(static_cast<unsigned char>(css[i]))) ++i;
        if (i >= len) break;

        // Skip comments: /* ... */
        if (i + 1 < len && css[i] == '/' && css[i + 1] == '*') {
            auto end_comment = css.find("*/", i + 2);
            if (end_comment == std::string_view::npos) break;
            i = end_comment + 2;
            continue;
        }

        // Find opening brace '{'
        auto open_brace = css.find('{', i);
        if (open_brace == std::string_view::npos) break;

        // Selector string
        std::string_view sel_part = css.substr(i, open_brace - i);

        // Find closing brace '}'
        auto close_brace = css.find('}', open_brace);
        if (close_brace == std::string_view::npos) break;

        std::string_view decls = css.substr(open_brace + 1, close_brace - open_brace - 1);
        i = close_brace + 1;

        // Skip @media or @keyframes blocks if encountered
        if (!sel_part.empty() && sel_part.front() == '@') {
            continue;
        }

        BlockStyle style = parse_declarations(decls);

        // Split comma-separated selectors (e.g. "h1, h2, h3")
        size_t s_pos = 0;
        while (s_pos < sel_part.size()) {
            auto comma = sel_part.find(',', s_pos);
            std::string_view single_sel = (comma == std::string_view::npos) ?
                sel_part.substr(s_pos) : sel_part.substr(s_pos, comma - s_pos);
            s_pos = (comma == std::string_view::npos) ? sel_part.size() : comma + 1;

            std::string clean_sel = to_lower(trim(single_sel));
            if (clean_sel.empty()) continue;

            if (clean_sel == "body" && style.has_bg) {
                body_bg_ = style.background_color;
                has_body_bg_ = true;
            }

            int spec = 1;
            if (clean_sel.front() == '#') spec = 100;
            else if (clean_sel.front() == '.') spec = 10;
            else if (clean_sel.find('.') != std::string::npos) spec = 11;
            else if (clean_sel.find('#') != std::string::npos) spec = 101;

            CssRule r;
            r.selector = std::move(clean_sel);
            r.style = style;
            r.specificity = spec;
            rules_.push_back(std::move(r));
        }
    }
}

BlockStyle CssEngine::compute_style(std::string_view tag,
                                    std::string_view class_attr,
                                    std::string_view id_attr,
                                    std::string_view inline_style) {
    std::string tag_l = to_lower(trim(tag));
    std::string id_l = to_lower(trim(id_attr));
    std::string cls_str = to_lower(trim(class_attr));

    std::vector<std::string> classes;
    size_t c_pos = 0;
    while (c_pos < cls_str.size()) {
        while (c_pos < cls_str.size() && std::isspace(static_cast<unsigned char>(cls_str[c_pos]))) ++c_pos;
        if (c_pos >= cls_str.size()) break;
        size_t end = c_pos;
        while (end < cls_str.size() && !std::isspace(static_cast<unsigned char>(cls_str[end]))) ++end;
        classes.push_back(cls_str.substr(c_pos, end - c_pos));
        c_pos = end;
    }

    BlockStyle computed;

    // Apply rules sorted by specificity
    for (const auto& rule : rules_) {
        bool match = false;
        if (rule.selector == "*") {
            match = true;
        } else if (rule.selector == tag_l) {
            match = true;
        } else if (!id_l.empty() && rule.selector == ("#" + id_l)) {
            match = true;
        } else if (!id_l.empty() && rule.selector == (tag_l + "#" + id_l)) {
            match = true;
        } else {
            for (const auto& c : classes) {
                if (rule.selector == ("." + c) || rule.selector == (tag_l + "." + c)) {
                    match = true;
                    break;
                }
            }
        }

        if (match) {
            computed.merge_from(rule.style);
        }
    }

    // Apply inline styles (highest priority)
    if (!inline_style.empty()) {
        BlockStyle inline_st = parse_declarations(inline_style);
        computed.merge_from(inline_st);
    }

    if (tag_l == "body" && computed.has_bg) {
        body_bg_ = computed.background_color;
        has_body_bg_ = true;
    }

    return computed;
}

} // namespace prospero::web
