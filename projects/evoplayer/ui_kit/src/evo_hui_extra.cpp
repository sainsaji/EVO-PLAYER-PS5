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

/* A dark plate behind a page hint row, for rows drawn over a picture. */
void hint_plate(DrawList &list, const ui::Fonts &fonts, const ui::Hint *hints, int n, float right)
{
    ui::HintLayout layout;
    layout.size = 30;
    layout.text_size = 20;
    const float w = ui::measure_hints(fonts, hints, n, layout) + 32.0f;
    list.rounded_rect({right - w + 16.0f, page::kHintsY - 18.0f, w, 36.0f}, 18, kBlack.with_alpha(0.6f));
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

    float lx = page::kLeft;
    draw_page_title(list, ctx, title_, subtitle_, lx);
    if (!badge_.empty()) {
        float w = fonts.semibold.measure(badge_, 16, 1.5f) + 20;
        Rect pill{1824.0f - w, 110, w, 32};
        list.rounded_rect(pill, 16, kWhite.with_alpha(0.08f));
        ui::text(list, fonts.semibold, badge_, pill.cx(), pill.y + 22, 16, kWhite, Align::center, 1.5f);
    }

    Rect card{lx, 220, 1824.0f - lx, 760};
    list.rounded_rect(card, page::kRadiusCard, p.surface);
    
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
        {ui::Button::dpad, "Scroll"},
        {ui::Button::l1, "Page", ui::Button::r1},
        {ui::Button::circle, "Back"}
    };
    draw_page_hints(list, fonts, hints, 3);
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
            Rect pill{1920.0f - page::kRight, page::kHintsY - 18.0f, w, 36};
            list.rounded_rect(pill, 18, kBlack.with_alpha(0.6f));
            ui::text(list, fonts.semibold, t, pill.cx(), pill.y + 24, 16, kWhite, Align::center);

            const ui::Hint hints[] = {{ui::Button::circle, "Back"}};
            hint_plate(list, fonts, hints, 1, page::kRight);
            draw_page_hints(list, fonts, hints, 1);
            list.pop_opacity();
        }
    }
    else
    {
        Rect card{960.0f - 300, 540.0f - 100, 600, 200};
        list.rounded_rect(card, page::kRadiusCard, p.surface);
        ui::text(list, fonts.display, "Could not open this image", card.cx(), card.y + 80, 40, kWhite, Align::center);
        ui::text(list, fonts.regular, title_, card.cx(), card.y + 130, 20, p.text_muted, Align::center);

        const ui::Hint hints[] = {{ui::Button::circle, "Back"}};
        draw_page_hints(list, fonts, hints, 1);
    }
}

bool ImageViewer::focus(FocusInfo *out) const
{
    return false;
}

/* -------------------------------------------------------- KeyboardOverlay */

namespace
{
/* The card, the text field and the key board, in 1920 x 1080 space. */
const Rect kCard{340.0f, 440.0f, 1240.0f, 600.0f};
const Rect kWell{kCard.x + 80.0f, kCard.y + 80.0f, kCard.w - 160.0f, 70.0f};
const Rect kBoard{kCard.x + 92.0f, kCard.y + 180.0f, 1056.0f, 360.0f};
const float kClearR = 22.0f;
const float kClearX = kWell.x + kWell.w - 20.0f - kClearR;
const float kClearY = kWell.y + kWell.h * 0.5f;

const char *kind_word(int kind)
{
    switch (kind) {
    case EVO_KB_KEY_SPACE: return "Space";
    case EVO_KB_KEY_SHIFT: return "Shift";
    case EVO_KB_KEY_BACKSPACE: return "Backspace";
    case EVO_KB_KEY_LAYOUT: return "Symbols";
    case EVO_KB_KEY_DONE: return "Done";
    default: return "";
    }
}
} // namespace

void KeyboardOverlay::set(const evo_keyboard_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    visible_ = p.visible != 0;
    native_only_ = p.native_only != 0;
    title_ = str(p.title);
    text_ = str(p.text);
    mode_label_ = str(p.mode_label);
    len_ = p.len;
    max_len_ = p.max_len;
    focus_row_ = p.focus_row;
    focus_col_ = p.focus_col;
    show_caret_ = p.show_caret != 0;
    cursor_ = p.cursor;
    clear_visible_ = p.clear_visible != 0;
    layer_ = p.layer;
    for (int l = 0; l < EVO_KB_LAYERS; ++l)
        for (int r = 0; r < EVO_KB_ROWS; ++r) {
            key_rows_[l][r] = p.key_rows[l][r];
            key_row_len_[l][r] = p.key_row_len[l][r];
        }
    qr_size_ = (p.qr_modules && p.qr_size > 0) ? p.qr_size : 0;
    qr_.assign(p.qr_modules, p.qr_modules + (size_t)qr_size_ * qr_size_);
    phone_title_ = p.phone_title ? p.phone_title : "";
    phone_notice_ = p.phone_notice ? p.phone_notice : "";
    phone_action_ = p.phone_action ? p.phone_action : "";
    phone_kind_ = p.phone_kind;
    key_columns_ = p.key_columns > 0 ? p.key_columns : 11;
    sync_keys(p);
}

/* The kit's Keyboard only draws here; evo_keyboard.c owns the layout, the focus,
 * the shift state and the text. Mirror its two layers and its state into the
 * component, which draws the keys, the key icons and the shift / caps lock look. */
void KeyboardOverlay::sync_keys(const evo_keyboard_params_t &p)
{
    if (layouts_built_ && built_columns_ != p.key_columns) layouts_built_ = false;   /* pad <-> full board */
    if (!layouts_built_ && p.key_row_len[0][0] > 0) {
        std::vector<ui::KeyboardLayout> layouts;
        for (int l = 0; l < EVO_KB_LAYERS; ++l) {
            ui::KeyboardLayout layout;
            layout.name = l == 0 ? "ABC" : "&123"; /* a layout key names where it leads */
            layout.columns = std::max(p.key_columns, 1);
            for (int r = 0; r < EVO_KB_ROWS; ++r) {
                std::vector<ui::KeyboardKey> row;
                if (p.key_row_len[l][r] <= 0) continue;
                for (int c = 0; c < p.key_row_len[l][r]; ++c) {
                    const evo_kb_key_t &k = p.key_rows[l][r][c];
                    ui::KeyboardKey key;
                    key.kind = static_cast<ui::KeyKind>(k.kind);
                    key.span = k.span;
                    key.text = k.text ? k.text : "";
                    key.shifted = k.shifted ? k.shifted : "";
                    row.push_back(std::move(key));
                }
                layout.add_row(std::move(row));
            }
            layouts.push_back(std::move(layout));
        }
        keys_.set_layouts(std::move(layouts));
        /* The pad buttons that do these things, drawn on the keys. Nothing here
         * calls the kit's own input handling: evo_keyboard.c takes the buttons. */
        keys_.style.bindings.backspace = ::hui::Action::west;    /* Square */
        keys_.style.bindings.space = ::hui::Action::north;       /* Triangle */
        keys_.style.bindings.shift = ::hui::Action::jump_prev;   /* L2 */
        keys_.style.bindings.done = ::hui::Action::jump_next;    /* R2 */
        layouts_built_ = true;
        built_columns_ = p.key_columns;
    }
    if (!layouts_built_) return;

    keys_.set_layout(layer_);
    keys_.set_shift(p.shift_state == 2 ? ui::KeyboardShift::lock
                    : p.shift_state == 1 ? ui::KeyboardShift::once : ui::KeyboardShift::off);
    const bool on_keys = focus_row_ >= 0 && focus_row_ < EVO_KB_ROWS;
    keys_.set_active(on_keys);
    if (on_keys) keys_.set_focus(focus_row_, focus_col_, !keys_ready_);
    keys_ready_ = true;
}

void KeyboardOverlay::enter()
{
    enter_scale_.snap(0.96f);
    enter_scale_.target = 1.0f;
    enter_fade_.snap(0.0f);
    enter_fade_.target = 1.0f;
    clear_focus_.snap(0.0f);
    keys_ready_ = false;
    keys_.enter();
}

void KeyboardOverlay::update(float dt)
{
    enter_scale_.update(dt, 16.0f);
    enter_fade_.update(dt, 20.0f);
    clear_focus_.target = (focus_row_ == EVO_KB_CLEAR_ROW) ? 1.0f : 0.0f;
    clear_focus_.update(dt, 24.0f);
    keys_.update(dt);
}

void KeyboardOverlay::draw(DrawList &list, const Context &ctx) const
{
    if (!visible_) return;
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    const float alpha = enter_fade_.value;
    list.rounded_rect({0, 0, 1920, 1080}, 0, kBlack.with_alpha(0.55f * alpha));

    if (native_only_) return;
    if (key_columns_ < 11) { draw_pad(list, ctx); return; }

    list.push_transform(enter_scale_.value, 960, 1080, 0, 0); // scale from bottom
    list.push_opacity(alpha);

    const Rect card = kCard;
    list.shadow({card.x, card.y + 20, card.w, card.h}, page::kRadiusCard, 60, kBlack.with_alpha(0.6f));
    list.rounded_rect(card, page::kRadiusCard, p.bg_top.with_alpha(0.97f));
    list.bordered_rect(card, page::kRadiusCard, kClear, 1.0f, kWhite.with_alpha(0.12f));

    // title
    ui::text(list, fonts.semibold, ui::upper(title_), card.cx(), card.y + 50, 16, p.text_muted, Align::center, 3.0f);

    // mode pill: the layer and the shift state (the Shift key itself also lights up)
    float mw = fonts.semibold.measure(mode_label_, 14, 1.5f) + 24;
    Rect mpill{card.x + card.w - 40 - mw, card.y + 34, mw, 28};
    list.rounded_rect(mpill, 14, kWhite.with_alpha(0.08f));
    ui::text(list, fonts.semibold, mode_label_, mpill.cx(), mpill.y + 19, 14, p.text_muted, Align::center, 1.5f);

    // text field
    const Rect well = kWell;
    list.rounded_rect(well, 16, kBlack.with_alpha(0.3f));
    list.bordered_rect(well, 16, kClear, 1.0f, kWhite.with_alpha(0.06f));

    /* Right side of the field, from the right edge: the clear button (when there
     * is text), the counter, then the L1 / R1 icons that move the caret. */
    float right = well.x + well.w - 20.0f;
    if (clear_visible_) {
        const float f = clear_focus_.value;
        list.circle(kClearX, kClearY, kClearR, kWhite.with_alpha(0.10f + 0.90f * f));
        const Color x = f > 0.5f ? kInk : kWhite.with_alpha(0.75f);
        const float a = 8.0f;
        list.line(kClearX - a, kClearY - a, kClearX + a, kClearY + a, 3.0f, x);
        list.line(kClearX - a, kClearY + a, kClearX + a, kClearY - a, 3.0f, x);
        right = kClearX - kClearR - 18.0f;
    }
    char counter[32];
    std::snprintf(counter, sizeof(counter), "%d / %d", len_, max_len_);
    ui::text(list, fonts.regular, counter, right, well.cy() + 10, 16, p.text_faint, Align::right);
    {
        const float gs = 22.0f;
        const float cw = fonts.regular.measure(counter, 16);
        const float w1 = ui::button_width(ui::Button::l1, gs), w2 = ui::button_width(ui::Button::r1, gs);
        float gx = right - cw - 20.0f - w1 - 6.0f - w2;
        ui::draw_button(list, fonts, ui::GlyphStyle::dark(), ui::Button::l1, gx, well.cy() + 3, gs);
        ui::draw_button(list, fonts, ui::GlyphStyle::dark(), ui::Button::r1, gx + w1 + 6.0f, well.cy() + 3, gs);
    }

    const float text_room = (right - well.x) - 20.0f - 230.0f;
    ui::text(list, fonts.regular, fit(fonts.regular, text_, 28, text_room), well.x + 20, well.cy() + 10, 28, kWhite);
    if (show_caret_ && fmod(ctx.time, 1.0f) < 0.5f) {
        /* At the caret, not the end of the text: L1 / R1 move it. */
        const size_t at = (cursor_ < 0 || (size_t)cursor_ > text_.size()) ? text_.size() : (size_t)cursor_;
        float tw = fonts.regular.measure(text_.substr(0, at), 28);
        tw = std::min(tw, text_room - 4.0f);
        list.rounded_rect({well.x + 20 + tw + 2, well.cy() - 16, 2, 32}, 1, p.accent);
    }

    // keys
    {
        ui::Keyboard &keys = const_cast<ui::Keyboard &>(keys_);
        keys.style.theme = p.kit_theme();
        /* Done is the one key that confirms: green, so the white focus plate never matches it. */
        keys.style.theme.primary = Color::rgb(0x3ddc84);
        keys.style.theme.on_primary = Color::rgb(0x10131a);
        keys.style.highlight.color = p.accent;   /* the focus plate has its own colour */
        keys.style.panel = false;
        keys.style.surfaces = true;
        /* A pad keeps the key size of the full board, so it is as narrow as its columns, centred. */
        Rect board = kBoard;
        if (key_columns_ < 11) {
            board.w = kBoard.w * static_cast<float>(key_columns_) / 11.0f;
            board.x = kBoard.x + (kBoard.w - board.w) * 0.5f;
        }
        keys.set_bounds(board);
        ui::Canvas canvas{list, fonts, 0, ctx.time};
        keys_.draw(canvas);
    }

    // phone link: a white card with the QR code, right of the keyboard
    if (qr_size_ > 0) {
        const float px = 6.0f, quiet = 4.0f;
        const float side = (qr_size_ + 2 * quiet) * px;
        Rect qcard{card.x + card.w + 30.0f, card.y, side, side};
        list.rounded_rect(qcard, 12, kWhite);
        for (int y = 0; y < qr_size_; ++y) {
            for (int x = 0; x < qr_size_;) {
                if (!qr_[(size_t)y * qr_size_ + x]) { ++x; continue; }
                int run = x;
                while (run < qr_size_ && qr_[(size_t)y * qr_size_ + run]) ++run;
                list.rounded_rect({qcard.x + (quiet + x) * px, qcard.y + (quiet + y) * px, (run - x) * px, px}, 0, kInk);
                x = run;
            }
        }
        ui::text(list, fonts.semibold, "SCAN TO TYPE", qcard.cx(), qcard.y + side + 30.0f, 14, p.text_muted, Align::center, 2.0f);
        if (!phone_title_.empty())
            ui::text(list, fonts.semibold, phone_title_, qcard.cx(), qcard.y + side + 58.0f, 17, kWhite, Align::center);
    } else if (!phone_notice_.empty()) {
        // no QR (no port is free, or the port is being set): say so, where the QR would have been
        const Color tone = phone_kind_ == 1 ? Color::rgb(0x3ddc84)
                         : phone_kind_ == 2 ? Color::rgb(0xffb02e) : Color::rgb(0xff5a5f);
        const Rect nc{card.x + card.w + 30.0f, card.y, 250.0f, 300.0f};
        list.rounded_rect(nc, 16, kWhite.with_alpha(0.06f));
        list.bordered_rect(nc, 16, kClear, 1.0f, tone.with_alpha(0.55f));
        list.rounded_rect({nc.x + 20.0f, nc.y + 28.0f, 12.0f, 12.0f}, 6, tone);
        ui::text(list, fonts.semibold, phone_title_, nc.x + 42.0f, nc.y + 40.0f, 14, tone, Align::left, 2.0f);
        ui::paragraph(list, fonts.regular, phone_notice_, nc.x + 20.0f, nc.y + 82.0f, 18, nc.w - 40.0f, 25,
                      kWhite.with_alpha(0.85f), 6);
        if (!phone_action_.empty()) {
            // the pad button that jumps to the port setting
            const ui::Hint hint[] = {{ui::Button::right_stick, phone_action_.c_str()}};
            ui::HintLayout layout;
            layout.size = 30;
            layout.text_size = 20;
            layout.cy = nc.y + nc.h - 36.0f;
            ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), hint, 1, nc.x + 20.0f, false, layout);
        }
    }

    list.pop_opacity();
    list.pop_transform();
}

/* A digits-only entry (the port): one compact dialog. A big readout on top, the pad on
 * the left, and on the right the result of checking the number, all inside one card. */
void KeyboardOverlay::draw_pad(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    list.push_transform(enter_scale_.value, 960, 1080, 0, 0);
    list.push_opacity(enter_fade_.value);

    const Rect card{460.0f, 230.0f, 1000.0f, 620.0f};
    list.shadow({card.x, card.y + 20, card.w, card.h}, page::kRadiusCard, 60, kBlack.with_alpha(0.6f));
    list.rounded_rect(card, page::kRadiusCard, p.bg_top.with_alpha(0.97f));
    list.bordered_rect(card, page::kRadiusCard, kClear, 1.0f, kWhite.with_alpha(0.12f));
    ui::text(list, fonts.semibold, ui::upper(title_), card.cx(), card.y + 54.0f, 16, p.text_muted, Align::center, 3.0f);

    // the number, large
    const Rect well{card.x + 60.0f, card.y + 84.0f, card.w - 120.0f, 100.0f};
    list.rounded_rect(well, 16, kBlack.with_alpha(0.3f));
    list.bordered_rect(well, 16, kClear, 1.0f, kWhite.with_alpha(0.06f));
    ui::text(list, fonts.semibold, text_, well.x + 32.0f, well.cy() + 18.0f, 52, kWhite);
    if (show_caret_ && fmod(ctx.time, 1.0f) < 0.5f) {
        const float tw = fonts.semibold.measure(text_, 52);
        list.rounded_rect({well.x + 32.0f + tw + 4.0f, well.cy() - 28.0f, 3, 56}, 1, p.accent);
    }
    char counter[32];
    std::snprintf(counter, sizeof(counter), "%d / %d", len_, max_len_);
    ui::text(list, fonts.regular, counter, well.x + well.w - 28.0f, well.cy() + 8.0f, 18, p.text_faint, Align::right);

    // the pad, key size as on the full keyboard
    const Rect board{card.x + 60.0f, card.y + 214.0f, kBoard.w * static_cast<float>(key_columns_) / 11.0f, 340.0f};
    {
        ui::Keyboard &keys = const_cast<ui::Keyboard &>(keys_);
        keys.style.theme = p.kit_theme();
        /* Done is the one key that confirms: green, so the white focus plate never matches it. */
        keys.style.theme.primary = Color::rgb(0x3ddc84);
        keys.style.theme.on_primary = Color::rgb(0x10131a);
        keys.style.highlight.color = p.accent;   /* the focus plate has its own colour */
        keys.style.panel = false;
        keys.style.surfaces = true;
        keys.set_bounds(board);
        ui::Canvas canvas{list, fonts, 0, ctx.time};
        keys_.draw(canvas);
    }

    // the check result, beside the pad
    const Color tone = phone_kind_ == 1 ? Color::rgb(0x3ddc84)
                     : phone_kind_ == 2 ? Color::rgb(0xffb02e) : Color::rgb(0xff5a5f);
    const float sx = board.x + board.w + 48.0f;
    const Rect sp{sx, board.y, card.x + card.w - 60.0f - sx, board.h};
    list.rounded_rect(sp, 20, tone.with_alpha(0.08f));
    list.bordered_rect(sp, 20, kClear, 1.0f, tone.with_alpha(0.45f));
    const float ix = sp.x + 56.0f, iy = sp.y + 66.0f, r = 30.0f, a = 11.0f;
    list.circle(ix, iy, r, tone);
    if (phone_kind_ == 1) {
        list.line(ix - a, iy + 1.0f, ix - 3.0f, iy + 9.0f, 4.0f, kInk);
        list.line(ix - 3.0f, iy + 9.0f, ix + a + 2.0f, iy - 8.0f, 4.0f, kInk);
    } else {
        list.line(ix - a + 2.0f, iy - a + 2.0f, ix + a - 2.0f, iy + a - 2.0f, 4.0f, kInk);
        list.line(ix - a + 2.0f, iy + a - 2.0f, ix + a - 2.0f, iy - a + 2.0f, 4.0f, kInk);
    }
    ui::text(list, fonts.semibold, phone_title_, sp.x + 108.0f, iy + 7.0f, 20, tone, Align::left, 2.0f);
    ui::paragraph(list, fonts.regular, phone_notice_, sp.x + 36.0f, sp.y + 140.0f, 21, sp.w - 72.0f, 30,
                  kWhite.with_alpha(0.85f), 4);

    // what the buttons do
    const ui::Hint save[] = {{ui::Button::options, "Save"}, {ui::Button::circle, "Cancel"}};
    const ui::Hint cancel[] = {{ui::Button::circle, "Cancel"}};
    ui::HintLayout layout;
    layout.size = 30;
    layout.text_size = 20;
    layout.cy = sp.y + sp.h - 38.0f;
    if (phone_kind_ == 1) ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), save, 2, sp.x + 36.0f, false, layout);
    else ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), cancel, 1, sp.x + 36.0f, false, layout);

    list.pop_opacity();
    list.pop_transform();
}

bool KeyboardOverlay::focus(FocusInfo *out) const
{
    if (!visible_ || native_only_) return false;
    out->focusable.clear();
    if (focus_row_ == EVO_KB_CLEAR_ROW) {
        out->id = "clear";
        out->text = "Clear";
        out->rect = {kClearX - kClearR, kClearY - kClearR, kClearR * 2, kClearR * 2};
        return true;
    }
    if (focus_row_ < 0 || focus_row_ >= EVO_KB_ROWS || focus_col_ < 0 ||
        focus_col_ >= key_row_len_[layer_][focus_row_] || !key_rows_[layer_][focus_row_])
        return false;
    const evo_kb_key_t &k = key_rows_[layer_][focus_row_][focus_col_];
    out->id = "key-r" + std::to_string(focus_row_) + "c" + std::to_string(focus_col_);
    out->text = k.kind == EVO_KB_KEY_CHAR ? std::string(k.text ? k.text : "") : kind_word(k.kind);
    out->rect = keys_.key_rect(focus_row_, focus_col_);
    return true;
}

} // namespace evo::kit
