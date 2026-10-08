/*
 * evo_hui_browser.cpp - the storage browser on ps5-homebrew-ui. See the header.
 */
#include "evo_hui_browser.hpp"

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
const Color kClear = Color::rgb(0x000000, 0.0f);
const Color kDanger = Color::rgb(0xff5a5f);
const Color kStar = Color::rgb(0xffd166);

constexpr float kLeft = NavRail::kWidth + 64.0f;
constexpr float kRight = 1824.0f;
constexpr float kSourceW = 260.0f;
constexpr float kSourceTop = 250.0f;
constexpr float kSourceH = 58.0f;
constexpr float kGridX = kLeft + kSourceW + 40.0f;
constexpr float kGridY = 262.0f;
constexpr float kCardW = 300.0f;
constexpr float kCardH = 169.0f;
constexpr float kGapX = 28.0f;
constexpr float kRowPitch = kCardH + 76.0f; /* card + its caption */
constexpr float kChipsY = 196.0f;
constexpr float kStripY = 760.0f;
constexpr float kMenuW = 660.0f;
constexpr float kActionH = 58.0f;

struct Source
{
    const char *label;
    const char *icon;
};
const Source kSources[5] = {
    {"USB Drive", "../icons/icon_browse_usb.png"},
    {"Internal Storage", "../icons/icon_folder.png"},
    {"Network (FTP)", "../icons/icon_browse_usb.png"},
    {"Favorites", "../icons/icon_favorites.png"},
    {"Recent Media", "../icons/icon_recent_files.png"},
};

struct Action
{
    const char *label;
    const char *desc;
    const char *icon;
};
const Action kActions[7] = {
    {"Copy", "Copy item to clipboard", "../icons/icon_recent_files.png"},
    {"Cut (Move)", "Move item to clipboard for relocation", "../icons/icon_recent_files.png"},
    {"Paste", "Paste clipboard item into this folder", "../icons/icon_folder.png"},
    {"Rename", "Change file or folder name", "../icons/icon_type.png"},
    {"Delete", "Permanently remove this item", "../icons/icon_trash.png"},
    {"New Folder", "Create directory in current location", "../icons/icon_folder.png"},
    {"Close", "Dismiss menu and return to browser", "../icons/btn_circle.png"},
};

std::string fit(const ui::FontRef &font, const std::string &text, float size, float width)
{
    return font.font->fit(text, size, width);
}

std::uint64_t hash_of(const std::string &s)
{
    std::uint64_t h = 1469598103934665603ull;
    for (char c : s)
        h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    return h;
}

} // namespace

void BrowserScreen::set(const evo_rmlui_browser_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    raw_ = p;
    path_ = str(p.path);
    title_ = str(p.title);
    chips_.clear();
    for (int i = 0; i < p.filter_count && i < 4; ++i)
        chips_.push_back(str(p.filter_labels[i]));
    cards_.clear();
    for (int i = 0; i < p.row_count && i < EVO_RMLUI_BROWSER_ROWS; ++i)
    {
        const evo_rmlui_browser_row_t &r = p.rows[i];
        Card c;
        c.name = str(r.name);
        c.detail = str(r.detail);
        c.icon = str(r.icon_path);
        c.badge = str(r.badge);
        c.duration = str(r.duration);
        c.progress = r.progress;
        c.favorite = r.is_favorite != 0;
        c.focused = r.is_focused != 0;
        c.art = r.art;
        c.art_w = r.art_w;
        c.art_h = r.art_h;
        cards_.push_back(c);
    }
    empty_title_ = str(p.empty_title);
    empty_hint_ = str(p.empty_hint);
    ins_name_ = str(p.ins_name);
    ins_kind_ = str(p.ins_kind);
    props_.clear();
    for (int i = 0; i < p.ins_prop_count && i < EVO_RMLUI_BROWSER_PROPS; ++i)
        props_.emplace_back(str(p.ins_props[i].key), str(p.ins_props[i].value));
    status_[0] = str(p.status_res);
    status_[1] = str(p.status_vcodec);
    status_[2] = str(p.status_acodec);
    status_[3] = str(p.status_duration);
    status_[4] = str(p.status_size);
    if (!p.action_menu_open)
        action_snapped_ = false;
    act_target_ = str(p.action_menu_target);
    act_sub_ = str(p.action_target_sub);
    act_clip_ = str(p.action_clipboard_info);
    tr_title_ = str(p.transfer_op_title);
    tr_item_ = str(p.transfer_item_name);
    tr_speed_ = str(p.transfer_speed_str);
    tr_bytes_ = str(p.transfer_bytes_str);
    tr_eta_ = str(p.transfer_eta_str);
    tr_pct_ = str(p.transfer_percent_str);
    tr_progress_ = p.transfer_progress_pct;
    /* Strings in raw_ are dangling after this call: never read them. */
    raw_.path = raw_.title = nullptr;

    /* A new page (or folder) re-runs the cards' entrance. */
    const int first = p.cursor_index >= 0 ? (p.cursor_index / 8) * 8 : 0;
    const std::string key = path_ + "#" + std::to_string(first) + "#" + std::to_string(p.active_source);
    if (static_cast<int>(hash_of(key) & 0x7fffffff) != last_first_)
    {
        last_first_ = static_cast<int>(hash_of(key) & 0x7fffffff);
        page_age_ = 0.0f;
    }
}

void BrowserScreen::enter()
{
    age_ = 0.0f;
    page_age_ = 0.0f;
    ring_snapped_ = false;
    action_snapped_ = false;
}

Rect BrowserScreen::card_rect(int i) const
{
    return {kGridX + static_cast<float>(i % 4) * (kCardW + kGapX),
            kGridY + static_cast<float>(i / 4) * kRowPitch, kCardW, kCardH};
}

Rect BrowserScreen::source_rect(int i) const
{
    return {kLeft, kSourceTop + static_cast<float>(i) * (kSourceH + 6.0f), kSourceW, kSourceH};
}

float BrowserScreen::chips_x(int i) const
{
    float x = kGridX;
    for (int k = 0; k < i && k < static_cast<int>(chip_w_.size()); ++k)
        x += chip_w_[static_cast<std::size_t>(k)] + 10.0f;
    return x;
}

Rect BrowserScreen::chip_rect(int i, const Context *) const
{
    const float w = i < static_cast<int>(chip_w_.size()) ? chip_w_[static_cast<std::size_t>(i)] : 100.0f;
    return {chips_x(i), kChipsY, w, 40.0f};
}

Rect BrowserScreen::action_rect(int i) const
{
    const float h = 170.0f + 7.0f * (kActionH + 4.0f) + 24.0f;
    const float top = 540.0f - h * 0.5f;
    return {960.0f - kMenuW * 0.5f + 24.0f, top + 170.0f + static_cast<float>(i) * (kActionH + 4.0f),
            kMenuW - 48.0f, kActionH};
}

void BrowserScreen::update(float dt)
{
    age_ += dt;
    page_age_ += dt;

    Rect target{};
    bool any = false;
    if (raw_.sidebar_focused)
    {
        target = source_rect(std::clamp(raw_.sidebar_index, 0, 4));
        any = true;
    }
    else if (raw_.filter_focused && !chips_.empty())
    {
        target = chip_rect(std::clamp(raw_.filter_selected, 0, static_cast<int>(chips_.size()) - 1), nullptr);
        any = true;
    }
    else
    {
        for (std::size_t i = 0; i < cards_.size(); ++i)
            if (cards_[i].focused)
            {
                const Rect r = card_rect(static_cast<int>(i));
                target = {r.x - 8, r.y - 8, r.w + 16, r.h + 16};
                any = true;
            }
    }
    if (any)
    {
        if (!ring_snapped_)
        {
            ring_.snap(target);
            ring_snapped_ = true;
        }
        ring_.target(target);
    }
    ring_.update(dt, 22.0f);
    ring_alpha_.target = (any && !raw_.rail_focused) ? 1.0f : 0.0f;
    ring_alpha_.update(dt, 14.0f);

    source_ring_.target(source_rect(std::clamp(raw_.active_source, 0, 4)));
    source_ring_.update(dt, 18.0f);

    menu_.target = raw_.action_menu_open ? 1.0f : 0.0f;
    menu_.update(dt, 18.0f);
    if (raw_.action_menu_open)
    {
        const Rect t = action_rect(std::clamp(raw_.action_menu_focused, 0, 6));
        if (!action_snapped_)
        {
            action_ring_.snap(t);
            action_snapped_ = true;
        }
        action_ring_.target(t);
        action_ring_.update(dt, 24.0f);
    }
    transfer_.target = raw_.transfer_modal_open ? 1.0f : 0.0f;
    transfer_.update(dt, 16.0f);
    transfer_bar_.target = static_cast<float>(std::clamp(tr_progress_, 0.0, 1.0));
    transfer_bar_.update(dt, 10.0f);
}

void BrowserScreen::draw(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    /* Washed behind everything: the focused file's own picture. */
    std::uint32_t wash = 0;
    for (const Card &c : cards_)
        if (c.focused && c.art)
            wash = ctx.textures.pixels(c.art, c.art_w, c.art_h);
    draw_backdrop(list, ctx, wash, 0.6f);

    /* Chip widths are measured here (fonts arrive with the draw). */
    auto &widths = const_cast<std::vector<float> &>(chip_w_);
    widths.clear();
    for (const std::string &c : chips_)
        widths.push_back(fonts.semibold.measure(c, 18) + 40.0f);

    /* ---- header ---- */
    {
        const float in = tween::stagger(age_, 0, 0.05f, 0.45f);
        list.push_opacity(in);
        ui::text(list, fonts.display, fit(fonts.display, title_.empty() ? "Files" : title_, 48, 900), kLeft - 2,
                 126, 48, kWhite);
        ui::text(list, fonts.regular, fit(fonts.regular, path_, 20, 1100), kLeft, 164, 20, p.text_muted);
        char counter[48];
        if (raw_.total_count > 0 && raw_.cursor_index >= 0)
            std::snprintf(counter, sizeof(counter), "%d OF %d", raw_.cursor_index + 1, raw_.total_count);
        else
            std::snprintf(counter, sizeof(counter), "%d %s", raw_.total_count,
                          raw_.total_count == 1 ? "ITEM" : "ITEMS");
        ui::text(list, fonts.regular, counter, kRight, 126, 20, p.text_faint, Align::right, 2.0f);
        list.pop_opacity();
    }

    /* ---- sources ---- */
    {
        list.rounded_rect(source_ring_.value(), 18, kWhite.with_alpha(raw_.sidebar_focused ? 0.0f : 0.10f));
        for (int i = 0; i < 5; ++i)
        {
            const float in = tween::stagger(age_, i + 1, 0.04f, 0.4f);
            list.push_opacity(in);
            const Rect r = source_rect(i);
            const bool focused = raw_.sidebar_focused && i == raw_.sidebar_index;
            const bool active = i == raw_.active_source;
            if (focused)
            {
                list.glow(r, 18, 16, p.accent.with_alpha(0.4f));
                list.rounded_rect(r, 18, kWhite);
            }
            if (active && !focused)
                list.rounded_rect({r.x + 4, r.y + 16, 4, r.h - 32}, 2, p.accent);
            const Color ink = focused ? kInk : (active ? kWhite : kWhite.with_alpha(0.6f));
            const std::uint32_t icon = ctx.textures.icon(kSources[i].icon);
            if (icon)
                list.image(icon, {r.x + 20, r.cy() - 13, 26, 26}, ::hui::gfx::kFullUv, focused ? kInk : (active ? p.accent : ink));
            ui::text(list, focused || active ? fonts.semibold : fonts.regular, kSources[i].label, r.x + 62,
                     r.cy() + 8, 21, ink);
            list.pop_opacity();
        }
    }

    /* ---- filter chips ---- */
    if (!chips_.empty())
    {
        list.push_opacity(tween::stagger(age_, 2, 0.05f, 0.4f));
        for (std::size_t i = 0; i < chips_.size(); ++i)
        {
            const Rect r = chip_rect(static_cast<int>(i), &ctx);
            const bool selected = static_cast<int>(i) == raw_.filter_selected;
            const bool focused = selected && raw_.filter_focused;
            if (focused)
                list.rounded_rect(r, 20, kWhite);
            else if (selected)
                list.rounded_rect(r, 20, p.accent.with_alpha(0.9f));
            else
                list.bordered_rect(r, 20, kWhite.with_alpha(0.06f), 1.0f, kWhite.with_alpha(0.16f));
            ui::text(list, fonts.semibold, chips_[i], r.cx(), r.y + 27, 18,
                     focused ? kInk : (selected ? kInk : kWhite.with_alpha(0.85f)), Align::center);
        }
        list.pop_opacity();
    }

    /* ---- grid ---- */
    if (raw_.is_empty)
    {
        const float cx = kGridX + (kRight - kGridX) * 0.5f;
        list.push_opacity(tween::stagger(age_, 2, 0.08f, 0.5f));
        const std::uint32_t icon = ctx.textures.icon("../icons/icon_browse_usb.png");
        if (icon)
            list.image(icon, {cx - 44, 360, 88, 88}, ::hui::gfx::kFullUv, kWhite.with_alpha(0.35f));
        ui::text(list, fonts.display, empty_title_.empty() ? "Folder is empty" : empty_title_, cx, 530, 36,
                 kWhite, Align::center);
        ui::text(list, fonts.regular, empty_hint_, cx, 574, 22, p.text_muted, Align::center);
        list.pop_opacity();
    }
    else
    {
        if (ring_alpha_.value > 0.01f && !raw_.sidebar_focused && !raw_.filter_focused)
        {
            list.push_opacity(ring_alpha_.value);
            list.glow(ring_.value(), 22, 22, p.accent.with_alpha(0.35f + 0.1f * ui::breathe(ctx.time)));
            list.pop_opacity();
        }
        for (std::size_t i = 0; i < cards_.size(); ++i)
        {
            const Card &c = cards_[i];
            const float in = tween::stagger(page_age_, static_cast<int>(i), 0.035f, 0.38f);
            list.push_opacity(in);
            Rect r = card_rect(static_cast<int>(i));
            r.y += 20.0f * (1.0f - tween::cubic_out(in));
            if (c.focused)
                r = {r.x - 8, r.y - 8, r.w + 16, r.h + 16};
            list.shadow({r.x, r.y + 12, r.w, r.h}, 18, 22, Color::rgb(0x000000, 0.45f));
            const std::uint32_t tex = ctx.textures.pixels(c.art, c.art_w, c.art_h);
            if (tex)
            {
                list.image(tex, r, ::hui::gfx::kFullUv, kWhite.with_alpha(c.focused ? 1.0f : 0.85f), 16);
            }
            else
            {
                list.gradient_rect(r, 16, p.surface_sel, p.surface);
                const std::uint32_t icon = ctx.textures.icon(c.icon);
                if (icon)
                    list.image(icon, {r.cx() - 30, r.cy() - 30, 60, 60}, ::hui::gfx::kFullUv,
                               kWhite.with_alpha(0.55f));
            }
            if (!c.badge.empty())
            {
                const float w = fonts.semibold.measure(c.badge, 14, 1.5f) + 20;
                const Rect pill{r.x + 10, r.y + 10, w, 26};
                list.rounded_rect(pill, 13, Color::rgb(0x000000, 0.6f));
                ui::text(list, fonts.semibold, c.badge, pill.cx(), pill.y + 18, 14, kWhite, Align::center, 1.5f);
            }
            if (c.favorite)
                list.star(r.x + r.w - 22, r.y + 22, 11, kStar);
            if (!c.duration.empty())
            {
                const float w = fonts.mono.measure(c.duration, 15) + 16;
                const Rect pill{r.x + r.w - w - 10, r.y + r.h - 34, w, 24};
                list.rounded_rect(pill, 8, Color::rgb(0x000000, 0.65f));
                ui::text(list, fonts.mono, c.duration, pill.cx(), pill.y + 17, 15, kWhite, Align::center);
            }
            if (c.progress >= 0)
            {
                const float f = tween::clamp01(static_cast<float>(c.progress) / 1000.0f);
                const Rect bar{r.x + 12, r.y + r.h - 8, r.w - 24, 4};
                list.rounded_rect(bar, 2, kWhite.with_alpha(0.25f));
                list.rounded_rect({bar.x, bar.y, std::max(4.0f, bar.w * f), bar.h}, 2, p.accent);
            }
            ui::text(list, c.focused ? fonts.semibold : fonts.regular, fit(fonts.semibold, c.name, 21, kCardW),
                     r.x + (c.focused ? 8.0f : 0.0f), r.y + r.h + 32, 21, c.focused ? kWhite : kWhite.with_alpha(0.85f));
            ui::text(list, fonts.regular, fit(fonts.regular, c.detail, 16, kCardW), r.x + (c.focused ? 8.0f : 0.0f),
                     r.y + r.h + 56, 16, p.text_faint);
            list.pop_opacity();
        }
    }
    if (ring_alpha_.value > 0.01f && !raw_.action_menu_open)
    {
        list.push_opacity(ring_alpha_.value);
        const float radius = raw_.sidebar_focused ? 20.0f : (raw_.filter_focused ? 22.0f : 22.0f);
        if (!raw_.sidebar_focused && !raw_.filter_focused)
            list.bordered_rect(ring_.value().inset(-5), radius, kClear, 3.5f, kWhite);
        list.pop_opacity();
    }

    /* ---- details strip ---- */
    if (!raw_.is_empty && !ins_name_.empty())
    {
        list.push_opacity(tween::stagger(age_, 5, 0.06f, 0.45f));
        const Rect strip{kGridX, kStripY, kRight - kGridX, 150};
        list.rounded_rect(strip, 26, kWhite.with_alpha(0.05f));
        list.bordered_rect(strip, 26, kClear, 1.0f, kWhite.with_alpha(0.08f));
        ui::text(list, fonts.semibold, ui::upper(ins_kind_.empty() ? "File" : ins_kind_), strip.x + 28,
                 strip.y + 40, 15, p.accent, Align::left, 3.0f);
        ui::text(list, fonts.display, fit(fonts.display, ins_name_, 30, strip.w - 56), strip.x + 28,
                 strip.y + 80, 30, kWhite);
        float x = strip.x + 28;
        const float y = strip.y + 100;
        auto pill = [&](const std::string &text, bool accent) {
            if (text.empty())
                return;
            const float w = fonts.semibold.measure(text, 16, 1.0f) + 28;
            if (x + w > strip.x + strip.w - 20)
                return;
            list.rounded_rect({x, y, w, 32}, 16, accent ? p.accent.with_alpha(0.22f) : kWhite.with_alpha(0.08f));
            ui::text(list, fonts.semibold, text, x + w * 0.5f, y + 22, 16, accent ? p.accent : p.text_muted,
                     Align::center, 1.0f);
            x += w + 10;
        };
        if (raw_.ins_probing)
            pill("Reading file...", false);
        for (int i = 0; i < 5; ++i)
            pill(status_[i], i == 0);
        if (status_[0].empty() && status_[1].empty())
            for (const auto &kv : props_)
                pill(kv.first + "  " + kv.second, false);
        list.pop_opacity();
    }

    /* ---- hints ---- */
    {
        ui::Hint hints[4];
        int n = 0;
        hints[n++] = {ui::Button::cross, "Open"};
        hints[n++] = {ui::Button::square, "Options"};
        if (!raw_.at_root)
            hints[n++] = {ui::Button::circle, "Back"};
        ui::HintLayout layout;
        layout.size = 30;
        layout.text_size = 20;
        layout.cy = 1046;
        list.push_opacity(tween::stagger(age_, 7, 0.05f, 0.4f));
        ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), hints, n, kRight, true, layout);
        list.pop_opacity();
    }

    /* ---- file operations menu ---- */
    const float m = menu_.value;
    if (m > 0.01f)
    {
        list.rounded_rect({0, 0, 1920, 1080}, 0, Color::rgb(0x000000, 0.45f * m));
        list.push_transform(0.96f + 0.04f * m, 960, 540, 0, 0);
        list.push_opacity(m);
        const Rect first = action_rect(0);
        const float h = 170.0f + 7.0f * (kActionH + 4.0f) + 24.0f;
        const Rect card{960.0f - kMenuW * 0.5f, first.y - 170.0f, kMenuW, h};
        list.shadow({card.x, card.y + 24, card.w, card.h}, 32, 48, Color::rgb(0x000000, 0.6f));
        list.rounded_rect(card, 32, p.bg_top.with_alpha(0.97f));
        list.bordered_rect(card, 32, kClear, 1.5f, kWhite.with_alpha(0.14f));
        ui::text(list, fonts.semibold, "FILE OPERATIONS", card.x + 32, card.y + 54, 15, p.accent, Align::left, 3.0f);
        ui::text(list, fonts.display, fit(fonts.display, act_target_, 30, card.w - 64), card.x + 32, card.y + 94,
                 30, kWhite);
        ui::text(list, fonts.regular, fit(fonts.regular, act_sub_, 18, card.w - 64), card.x + 32, card.y + 124,
                 18, p.text_muted);
        if (!act_clip_.empty())
            ui::text(list, fonts.regular, fit(fonts.regular, act_clip_, 16, card.w - 64), card.x + 32,
                     card.y + 150, 16, p.text_faint);
        list.rounded_rect(action_ring_.value(), 18, kWhite);
        for (int i = 0; i < 7; ++i)
        {
            const Rect r = action_rect(i);
            const bool focused = i == raw_.action_menu_focused;
            const bool disabled = i == 2 && !raw_.action_paste_enabled;
            const Color ink = focused ? kInk : (i == 4 ? kDanger : kWhite);
            list.push_opacity(disabled ? 0.35f : 1.0f);
            const std::uint32_t icon = ctx.textures.icon(kActions[i].icon);
            if (icon)
                list.image(icon, {r.x + 18, r.cy() - 13, 26, 26}, ::hui::gfx::kFullUv, ink);
            ui::text(list, fonts.semibold, kActions[i].label, r.x + 60, r.y + 27, 20, ink);
            ui::text(list, fonts.regular, kActions[i].desc, r.x + 60, r.y + 48, 15,
                     focused ? kInk.with_alpha(0.65f) : p.text_faint);
            list.pop_opacity();
        }
        list.pop_opacity();
        list.pop_transform();
    }

    /* ---- transfer progress ---- */
    const float t = transfer_.value;
    if (t > 0.01f)
    {
        list.rounded_rect({0, 0, 1920, 1080}, 0, Color::rgb(0x000000, 0.55f * t));
        list.push_opacity(t);
        const Rect card{960 - 400, 540 - 170, 800, 340};
        list.shadow({card.x, card.y + 24, card.w, card.h}, 32, 48, Color::rgb(0x000000, 0.6f));
        list.rounded_rect(card, 32, p.bg_top.with_alpha(0.97f));
        list.bordered_rect(card, 32, kClear, 1.5f, kWhite.with_alpha(0.14f));
        ui::text(list, fonts.semibold, ui::upper(tr_title_), card.x + 40, card.y + 62, 16, p.accent, Align::left, 3.0f);
        ui::text(list, fonts.display, fit(fonts.display, tr_item_, 30, card.w - 80), card.x + 40, card.y + 108, 30,
                 kWhite);
        const Rect bar{card.x + 40, card.y + 150, card.w - 80, 12};
        list.rounded_rect(bar, 6, kWhite.with_alpha(0.12f));
        list.rounded_rect({bar.x, bar.y, std::max(12.0f, bar.w * transfer_bar_.value), bar.h}, 6, p.accent);
        ui::text(list, fonts.display, tr_pct_, card.x + 40, card.y + 222, 40, kWhite);
        ui::text(list, fonts.regular, tr_speed_, card.x + card.w - 40, card.y + 204, 20, p.text_muted, Align::right);
        ui::text(list, fonts.regular, tr_bytes_ + "   " + tr_eta_, card.x + card.w - 40, card.y + 232, 18,
                 p.text_faint, Align::right);
        ui::text(list, fonts.regular, "Circle to cancel", card.x + 40, card.y + 300, 18, p.text_faint);
        list.pop_opacity();
    }
}

bool BrowserScreen::focus(FocusInfo *out) const
{
    out->focusable.clear();
    if (raw_.action_menu_open)
    {
        for (int i = 0; i < 7; ++i)
            out->focusable.emplace_back("action-" + std::to_string(i), kActions[i].label);
        const int at = std::clamp(raw_.action_menu_focused, 0, 6);
        out->id = "action-" + std::to_string(at);
        out->text = kActions[at].label;
        out->index = at + 1;
        out->total = 7;
        out->rect = action_rect(at);
        return true;
    }
    if (raw_.sidebar_focused)
    {
        for (int i = 0; i < 5; ++i)
            out->focusable.emplace_back("source-" + std::to_string(i), kSources[i].label);
        const int at = std::clamp(raw_.sidebar_index, 0, 4);
        out->id = "source-" + std::to_string(at);
        out->text = kSources[at].label;
        out->index = at + 1;
        out->total = 5;
        out->rect = source_rect(at);
        return true;
    }
    if (raw_.filter_focused && !chips_.empty())
    {
        for (std::size_t i = 0; i < chips_.size(); ++i)
            out->focusable.emplace_back("chip-" + std::to_string(i), chips_[i]);
        const int at = std::clamp(raw_.filter_selected, 0, static_cast<int>(chips_.size()) - 1);
        out->id = "chip-" + std::to_string(at);
        out->text = chips_[static_cast<std::size_t>(at)];
        out->index = at + 1;
        out->total = static_cast<int>(chips_.size());
        out->rect = chip_rect(at, nullptr);
        return true;
    }
    for (std::size_t i = 0; i < cards_.size(); ++i)
        out->focusable.emplace_back("card-" + std::to_string(i), cards_[i].name);
    for (std::size_t i = 0; i < cards_.size(); ++i)
    {
        if (!cards_[i].focused)
            continue;
        out->id = "card-" + std::to_string(i);
        out->text = cards_[i].name;
        out->index = raw_.cursor_index >= 0 ? raw_.cursor_index + 1 : static_cast<int>(i) + 1;
        out->total = raw_.total_count;
        out->rect = card_rect(static_cast<int>(i));
        return true;
    }
    out->focusable.clear();
    return false;
}

} // namespace evo::kit
