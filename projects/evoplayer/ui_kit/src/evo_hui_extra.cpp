/*
 * evo_hui_extra.cpp - Extra EVO screens on ps5-homebrew-ui.
 */
#include "evo_hui_extra.hpp"
#include "ui/glyphs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace evo::kit
{

using ::hui::gfx::Align;
using ::hui::gfx::Color;
using ::hui::gfx::DrawList;
using ::hui::gfx::Rect;
namespace tween = ::hui::tween;
namespace ui = ::hui::ui;

namespace
{
const Color kWhite = Color::rgb(0xffffff);
const Color kInk = Color::rgb(0x0b0d16);
const Color kBlack = Color::rgb(0x000000);
const Color kClear = Color::rgb(0x000000, 0.0f);

std::string fit(const ui::FontRef &font, const std::string &text, float size, float width)
{
    return font.font->fit(text, size, width);
}

void hint_row(DrawList &list, const ui::Fonts &fonts, const ui::Hint *hints, int n, float right, float cy = 1030.0f)
{
    ui::HintLayout layout;
    layout.size = 30;
    layout.text_size = 20;
    layout.cy = cy;
    ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), hints, n, right, true, layout);
}

std::vector<std::string> split_utf8(const std::string& str) {
    std::vector<std::string> res;
    size_t i = 0;
    while (i < str.length()) {
        unsigned char c = str[i];
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        res.push_back(str.substr(i, len));
        i += len;
    }
    return res;
}
} // namespace

/* ----------------------------------------------------------- ReaderScreen */

void ReaderScreen::set(const evo_rmlui_reader_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    title_ = str(p.title);
    subtitle_ = str(p.subtitle);
    badge_ = str(p.badge);
    lines_.clear();
    for (int i = 0; i < p.line_count && i < 64; ++i)
        lines_.push_back(str(p.lines[i]));
    face_ = p.face;
    progress_ = p.progress;
    visible_frac_ = p.visible_frac;
    notice_ = str(p.notice);
    footnote_ = str(p.footnote);
}

void ReaderScreen::enter() {}
void ReaderScreen::update(float dt) {}

void ReaderScreen::draw(DrawList &list, const Context &ctx) const
{
    draw_backdrop(list, ctx);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    float lx = NavRail::kWidth + 64.0f;
    ui::text(list, fonts.display, title_, lx, 140, 44, kWhite);
    ui::text(list, fonts.regular, subtitle_, lx, 180, 20, p.text_muted);
    if (!badge_.empty()) {
        float w = fonts.semibold.measure(badge_, 16, 1.5f) + 20;
        Rect pill{1824.0f - w, 110, w, 32};
        list.rounded_rect(pill, 16, kWhite.with_alpha(0.08f));
        ui::text(list, fonts.semibold, badge_, pill.cx(), pill.y + 22, 16, kWhite, Align::center, 1.5f);
    }

    Rect card{lx, 220, 1824.0f - lx, 760};
    list.rounded_rect(card, 28, p.surface);
    
    /* The footnote has its own strip at the foot of the card. */
    list.push_clip({card.x, card.y, card.w, card.h - (footnote_.empty() ? 0.0f : 44.0f)});
    if (!notice_.empty()) {
        ui::text(list, fonts.regular, notice_, card.cx(), card.cy(), 24, p.text_muted, Align::center);
    } else {
        float size = face_ == 2 ? 24.0f : 20.0f;
        float line_height = size * 1.5f;
        float y = card.y + 40.0f + size; // text y is BASELINE
        for (const std::string& line : lines_) {
            ui::text(list, fonts.regular, line, card.x + 40, y, size, kWhite);
            y += line_height;
        }
    }
    list.pop_clip();

    if (visible_frac_ < 1.0f) {
        float track_h = card.h - 80;
        float thumb_h = std::max(20.0f, track_h * (float)visible_frac_);
        float thumb_y = card.y + 40 + (track_h - thumb_h) * (float)progress_;
        list.rounded_rect({card.x + card.w - 12, thumb_y, 4, thumb_h}, 2, kWhite.with_alpha(0.2f));
    }

    if (!footnote_.empty()) {
        ui::text(list, fonts.regular, footnote_, card.x + 40, card.y + card.h - 20, 16, p.text_faint);
    }

    const ui::Hint hints[] = {
        {ui::Button::circle, "Back"},
        {ui::Button::dpad, "Scroll"},
        {ui::Button::l1, "Page"},
        {ui::Button::r1, "Page"}
    };
    hint_row(list, fonts, hints, 4, 1824.0f);
}

bool ReaderScreen::focus(FocusInfo *out) const
{
    return false;
}

/* ------------------------------------------------------------ ImageViewer */

void ImageViewer::set(const evo_rmlui_image_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    std::string new_title = str(p.title);
    if (title_ != new_title) {
        fade_age_ = 0.0f;
    }
    title_ = new_title;
    pixels_ = p.pixels;
    w_ = p.w;
    h_ = p.h;
    loaded_ = p.loaded != 0;
}

void ImageViewer::enter()
{
    fade_age_ = 0.0f;
}

void ImageViewer::update(float dt)
{
    fade_age_ += dt;
}

void ImageViewer::draw(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    list.rounded_rect({0, 0, 1920, 1080}, 0, kBlack);

    if (loaded_ && pixels_ && w_ > 0 && h_ > 0)
    {
        uint32_t tex = ctx.textures.pixels(pixels_, w_, h_, reinterpret_cast<uint64_t>(pixels_));
        if (tex) {
            float scale = std::min(1920.0f / w_, 1080.0f / h_);
            float iw = w_ * scale;
            float ih = h_ * scale;
            Rect r{960.0f - iw * 0.5f, 540.0f - ih * 0.5f, iw, ih};
            list.image(tex, r, ::hui::gfx::kFullUv, kWhite);
        }

        float fade = 1.0f;
        if (fade_age_ > 3.0f) {
            fade = std::max(0.0f, 1.0f - (fade_age_ - 3.0f) / 0.5f);
        }

        if (fade > 0.01f) {
            list.push_opacity(fade);
            char dim[64];
            std::snprintf(dim, sizeof(dim), "%d x %d", w_, h_);
            std::string t = title_ + "   " + dim;
            float w = fonts.semibold.measure(t, 16, 1.0f) + 32;
            Rect pill{40.0f, 1080.0f - 40.0f - 36.0f, w, 36};
            list.rounded_rect(pill, 18, kBlack.with_alpha(0.6f));
            ui::text(list, fonts.semibold, t, pill.cx(), pill.y + 24, 16, kWhite, Align::center);

            const ui::Hint hints[] = {{ui::Button::circle, "Back"}};
            hint_row(list, fonts, hints, 1, 1880.0f, 1080.0f - 40.0f);
            list.pop_opacity();
        }
    }
    else
    {
        Rect card{960.0f - 300, 540.0f - 100, 600, 200};
        list.rounded_rect(card, 24, p.surface);
        ui::text(list, fonts.display, "Could not open this image", card.cx(), card.y + 80, 40, kWhite, Align::center);
        ui::text(list, fonts.regular, title_, card.cx(), card.y + 130, 20, p.text_muted, Align::center);

        const ui::Hint hints[] = {{ui::Button::circle, "Back"}};
        hint_row(list, fonts, hints, 1, 1880.0f, 1040.0f);
    }
}

bool ImageViewer::focus(FocusInfo *out) const
{
    return false;
}

/* -------------------------------------------------------- KeyboardOverlay */

void KeyboardOverlay::set(const evo_keyboard_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    visible_ = p.visible != 0;
    native_only_ = p.native_only != 0;
    title_ = str(p.title);
    text_ = str(p.text);
    mode_label_ = str(p.mode_label);
    for (int i = 0; i < 4; ++i)
        rows_[i] = split_utf8(str(p.rows[i]));
    action_labels_.clear();
    for (int i = 0; i < 6; ++i) {
        if (p.action_labels[i] && p.action_labels[i][0] != '\0')
            action_labels_.push_back(p.action_labels[i]);
    }
    len_ = p.len;
    max_len_ = p.max_len;
    focus_row_ = p.focus_row;
    focus_col_ = p.focus_col;
    show_caret_ = p.show_caret != 0;
}

void KeyboardOverlay::enter()
{
    enter_scale_.snap(0.96f);
    enter_scale_.target = 1.0f;
    enter_fade_.snap(0.0f);
    enter_fade_.target = 1.0f;
    ring_snapped_ = false;
}

void KeyboardOverlay::update(float dt)
{
    enter_scale_.update(dt, 16.0f);
    enter_fade_.update(dt, 20.0f);

    Rect target{};
    if (focus_row_ >= 0 && focus_row_ < 4) {
        target = {340.0f + 140.0f + focus_col_ * 96.0f, 440.0f + 180.0f + focus_row_ * 72.0f, 96.0f, 72.0f};
    } else if (focus_row_ == 4 && focus_col_ >= 0 && focus_col_ < (int)action_labels_.size()) {
        float aw = 960.0f / std::max(1, (int)action_labels_.size());
        target = {340.0f + 140.0f + focus_col_ * aw, 440.0f + 180.0f + 4 * 72.0f + 20.0f, aw, 56.0f};
    }
    if (target.w > 0) {
        if (!ring_snapped_) {
            ring_.snap(target);
            ring_snapped_ = true;
        }
        ring_.target(target);
        ring_.update(dt, 24.0f);
    }
}

void KeyboardOverlay::draw(DrawList &list, const Context &ctx) const
{
    if (!visible_) return;
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    const float alpha = enter_fade_.value;
    list.rounded_rect({0, 0, 1920, 1080}, 0, kBlack.with_alpha(0.55f * alpha));

    if (native_only_) return;

    list.push_transform(enter_scale_.value, 960, 1080, 0, 0); // scale from bottom
    list.push_opacity(alpha);

    Rect card{340.0f, 440.0f, 1240.0f, 600.0f};
    list.shadow({card.x, card.y + 20, card.w, card.h}, 36, 60, kBlack.with_alpha(0.6f));
    list.rounded_rect(card, 36, p.bg_top.with_alpha(0.97f));
    list.bordered_rect(card, 36, kClear, 1.0f, kWhite.with_alpha(0.12f));

    // title
    ui::text(list, fonts.semibold, ui::upper(title_), card.cx(), card.y + 50, 16, p.text_muted, Align::center, 3.0f);

    // mode_label
    float mw = fonts.semibold.measure(mode_label_, 14, 1.5f) + 24;
    Rect mpill{card.x + card.w - 40 - mw, card.y + 34, mw, 28};
    list.rounded_rect(mpill, 14, kWhite.with_alpha(0.08f));
    ui::text(list, fonts.semibold, mode_label_, mpill.cx(), mpill.y + 19, 14, p.text_muted, Align::center, 1.5f);

    // text field
    Rect well{card.x + 80, card.y + 80, card.w - 160, 70};
    list.rounded_rect(well, 16, kBlack.with_alpha(0.3f));
    list.bordered_rect(well, 16, kClear, 1.0f, kWhite.with_alpha(0.06f));
    ui::text(list, fonts.regular, fit(fonts.regular, text_, 28, well.w - 40), well.x + 20, well.cy() + 10, 28, kWhite);
    if (show_caret_ && fmod(ctx.time, 1.0f) < 0.5f) {
        float tw = fonts.regular.measure(text_, 28);
        list.rounded_rect({well.x + 20 + tw + 2, well.cy() - 16, 2, 32}, 1, p.accent);
    }
    
    char counter[32];
    std::snprintf(counter, sizeof(counter), "%d / %d", len_, max_len_);
    ui::text(list, fonts.regular, counter, well.x + well.w - 20, well.cy() + 10, 16, p.text_faint, Align::right);

    // keys
    float kx = card.x + 140.0f;
    float ky = card.y + 180.0f;

    if (ring_snapped_) {
        list.rounded_rect(ring_.value(), 16, kWhite);
    }

    for (int r = 0; r < 4; ++r) {
        for (size_t c = 0; c < rows_[r].size() && c < 10; ++c) {
            bool focused = (focus_row_ == r && focus_col_ == (int)c);
            Color txt = focused ? kInk : kWhite;
            ui::text(list, fonts.regular, rows_[r][c], kx + c * 96.0f + 48.0f, ky + r * 72.0f + 48.0f, 32, txt, Align::center);
        }
    }

    // action bar
    if (!action_labels_.empty()) {
        float ay = ky + 4 * 72.0f + 20.0f;
        float aw = 960.0f / action_labels_.size();
        for (size_t i = 0; i < action_labels_.size(); ++i) {
            bool focused = (focus_row_ == 4 && focus_col_ == (int)i);
            Color txt = focused ? kInk : kWhite;
            if (!focused) {
                list.rounded_rect({kx + i * aw + 6, ay, aw - 12, 56.0f}, 28, kWhite.with_alpha(0.1f));
            }
            ui::text(list, fonts.semibold, action_labels_[i], kx + i * aw + aw * 0.5f, ay + 34.0f, 18, txt, Align::center);
        }
    }

    list.pop_opacity();
    list.pop_transform();
}

bool KeyboardOverlay::focus(FocusInfo *out) const
{
    if (!visible_ || native_only_) return false;
    out->focusable.clear();
    if (focus_row_ >= 0 && focus_row_ < 4) {
        out->id = "key-r" + std::to_string(focus_row_) + "c" + std::to_string(focus_col_);
        if (focus_col_ >= 0 && focus_col_ < (int)rows_[focus_row_].size())
            out->text = rows_[focus_row_][focus_col_];
        out->rect = {340.0f + 140.0f + focus_col_ * 96.0f, 440.0f + 180.0f + focus_row_ * 72.0f, 96.0f, 72.0f};
        return true;
    } else if (focus_row_ == 4 && focus_col_ >= 0 && focus_col_ < (int)action_labels_.size()) {
        out->id = "action-" + std::to_string(focus_col_);
        out->text = action_labels_[focus_col_];
        float aw = 960.0f / action_labels_.size();
        out->rect = {340.0f + 140.0f + focus_col_ * aw, 440.0f + 180.0f + 4 * 72.0f + 20.0f, aw, 56.0f};
        return true;
    }
    return false;
}

} // namespace evo::kit
