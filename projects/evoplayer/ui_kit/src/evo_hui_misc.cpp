#include "evo_hui_misc.hpp"

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

std::string fit(const ui::FontRef &font, const std::string &text, float size, float width)
{
    return font.font->fit(text, size, width);
}

} // namespace

/* ----------------------------------------------------------- DialogOverlay */

void DialogOverlay::set(const evo_rmlui_dialog_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    eyebrow_ = str(p.eyebrow);
    title_ = str(p.title);
    detail_ = str(p.detail);
    progress_pct_ = p.progress_pct;
    focused_action_ = p.focused_action;
    actions_.clear();
    for (int i = 0; i < p.action_count && i < 3; ++i)
    {
        Action a;
        a.icon_path = str(p.actions[i].icon_path);
        a.label = str(p.actions[i].label);
        a.is_primary = p.actions[i].is_primary != 0;
        actions_.push_back(a);
    }
}

void DialogOverlay::enter()
{
    enter_scale_.snap(0.95f);
    enter_scale_.target = 1.0f;
    enter_fade_.snap(0.0f);
    enter_fade_.target = 1.0f;
}

void DialogOverlay::update(float dt)
{
    enter_scale_.update(dt, 16.0f);
    enter_fade_.update(dt, 20.0f);
}

void DialogOverlay::draw(DrawList &list, const Context &ctx) const
{
    const float alpha = enter_fade_.value;
    if (alpha < 0.01f)
        return;

    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    list.rounded_rect({0, 0, 1920, 1080}, 0, Color::rgb(0x000000, 0.5f * alpha));

    const float scale = enter_scale_.value;
    list.push_transform(scale, 960, 540, 0, 0);
    list.push_opacity(alpha);

    const float w = 760.0f;
    float h = 64.0f; 
    if (!eyebrow_.empty()) h += 24.0f;
    h += 50.0f;
    if (!detail_.empty()) h += 32.0f * std::min(4, (int)fonts.regular.font->wrap(detail_, 24, w - 80).size()) + 20.0f;
    if (progress_pct_ >= 0.0) h += 30.0f;
    if (!actions_.empty()) h += 80.0f;

    const Rect card{960.0f - w * 0.5f, 540.0f - h * 0.5f, w, h};
    list.shadow({card.x, card.y + 24, card.w, card.h}, 48, 64, Color::rgb(0x000000, 0.6f));
    list.rounded_rect(card, 32, p.bg_top.with_alpha(0.97f));
    list.bordered_rect(card, 32, Color::rgb(0,0), 1.0f, kWhite.with_alpha(0.14f));

    float cy = card.y + 40.0f;
    if (!eyebrow_.empty())
    {
        ui::text(list, fonts.semibold, ui::upper(eyebrow_), card.cx(), cy, 16, p.accent, Align::center, 3.0f);
        cy += 30.0f;
    }
    ui::text(list, fonts.display, fit(fonts.display, title_, 38, w - 80), card.cx(), cy + 24, 38, kWhite, Align::center);
    cy += 50.0f;

    if (!detail_.empty())
    {
        cy = ui::paragraph(list, fonts.regular, detail_, card.cx(), cy + 20, 24, w - 80, 32.0f, p.text_muted, 4, Align::center);
        cy += 10.0f;
    }

    if (progress_pct_ >= 0)
    {
        const Rect bar{card.x + 40, cy + 10, w - 80, 6};
        list.rounded_rect(bar, 3, kWhite.with_alpha(0.16f));
        list.rounded_rect({bar.x, bar.y, std::max(6.0f, bar.w * (float)progress_pct_), bar.h}, 3, p.accent);
        cy += 30.0f;
    }

    if (!actions_.empty())
    {
        float aw = (w - 80 - (actions_.size() - 1) * 20.0f) / actions_.size();
        for (std::size_t i = 0; i < actions_.size(); ++i)
        {
            Rect btn{card.x + 40 + i * (aw + 20), card.y + card.h - 80, aw, 48};
            bool focused = (int)i == focused_action_;
            if (focused)
            {
                list.rounded_rect(btn, 24, kWhite);
            }
            else
            {
                list.rounded_rect(btn, 24, kWhite.with_alpha(0.1f));
                if (actions_[i].is_primary)
                    list.bordered_rect(btn, 24, Color::rgb(0,0), 2.0f, p.accent);
            }
            Color txt = focused ? kInk : kWhite;
            ui::text(list, fonts.semibold, fit(fonts.semibold, actions_[i].label, 18, aw - 20), btn.cx(), btn.cy() + 7, 18, txt, Align::center);
        }
    }

    list.pop_opacity();
    list.pop_transform();
}

bool DialogOverlay::focus(FocusInfo *out) const
{
    if (focused_action_ >= 0 && focused_action_ < (int)actions_.size())
    {
        out->focusable.clear();
        for (size_t i = 0; i < actions_.size(); ++i)
            out->focusable.emplace_back("action-" + std::to_string(i), actions_[i].label);
        out->id = "action-" + std::to_string(focused_action_);
        out->text = actions_[focused_action_].label;
        out->index = focused_action_ + 1;
        out->total = static_cast<int>(actions_.size());
        out->rect = {620.0f, 580.0f, 680.0f, 48.0f};
        return true;
    }
    return false;
}

/* ------------------------------------------------------------ ToastOverlay */

void ToastOverlay::set(const evo_rmlui_toast_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    title_ = str(p.title);
    message_ = str(p.message);
    kind_ = p.kind;
    visible_ = p.visible != 0;
    alpha_ = p.alpha;
    slide_ = p.slide;
}

void ToastOverlay::enter()
{
}

void ToastOverlay::update(float dt)
{
}

void ToastOverlay::draw(DrawList &list, const Context &ctx) const
{
    if (!visible_ || alpha_ <= 0)
        return;
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    Color acc;
    if (kind_ == 1) acc = Color::rgb(0x8a8fff);
    else if (kind_ == 2) acc = Color::rgb(0xff5a5f);
    else if (kind_ == 3) acc = Color::rgb(0x30d158);
    else acc = p.accent;

    list.push_opacity((float)alpha_ / 255.0f);

    float text_w = std::max(fonts.semibold.measure(title_, 20), fonts.regular.measure(message_, 18));
    float w = std::min(620.0f, text_w + 70.0f);
    float h = 64.0f;
    Rect card{1920.0f - w - 40.0f + slide_, 990.0f - h, w, h};

    list.shadow({card.x, card.y + 10, card.w, card.h}, 20, 24, Color::rgb(0x000000, 0.5f));
    list.rounded_rect(card, 32, p.bg_top.with_alpha(0.95f));
    list.bordered_rect(card, 32, Color::rgb(0,0), 1.0f, kWhite.with_alpha(0.12f));

    list.rounded_rect({card.x + 16, card.y + 16, 6, 32}, 3, acc);
    
    float cy = card.y + 24;
    ui::text(list, fonts.semibold, fit(fonts.semibold, title_, 20, w - 60), card.x + 36, cy, 20, kWhite);
    ui::text(list, fonts.regular, fit(fonts.regular, message_, 18, w - 60), card.x + 36, cy + 22, 18, p.text_muted);

    list.pop_opacity();
}

bool ToastOverlay::focus(FocusInfo *out) const
{
    return false;
}

/* ------------------------------------------------------------- AboutScreen */

void AboutScreen::set(const evo_rmlui_about_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    app_name_ = str(p.app_name);
    version_ = str(p.version);
    build_tag_ = str(p.build_tag);
    tagline_ = str(p.tagline);
    themes_info_ = str(p.themes_info);
    action_focused_ = p.action_focused != 0;
}

void AboutScreen::enter()
{
}

void AboutScreen::update(float dt)
{
}

void AboutScreen::draw(DrawList &list, const Context &ctx) const
{
    draw_backdrop(list, ctx);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    float cx = (NavRail::kWidth + 1920.0f) * 0.5f;

    std::uint32_t logo = ctx.textures.icon("../icons/icon_logo.png");
    if (logo)
        list.image(logo, {cx - 60, 140, 120, 120}, ::hui::gfx::kFullUv, kWhite);

    ui::text(list, fonts.display, app_name_, cx, 320, 64, kWhite, Align::center);

    float w1 = fonts.semibold.measure(version_, 16) + 30;
    float w2 = fonts.semibold.measure(build_tag_, 16) + 30;
    float px = cx - (w1 + w2 + 10) * 0.5f;
    list.rounded_rect({px, 350, w1, 32}, 16, kWhite.with_alpha(0.1f));
    ui::text(list, fonts.semibold, version_, px + w1 * 0.5f, 372, 16, kWhite, Align::center);
    px += w1 + 10;
    list.rounded_rect({px, 350, w2, 32}, 16, kWhite.with_alpha(0.1f));
    ui::text(list, fonts.semibold, build_tag_, px + w2 * 0.5f, 372, 16, kWhite, Align::center);

    ui::text(list, fonts.semibold, ui::upper(tagline_), cx, 420, 16, p.text_muted, Align::center, 3.0f);

    struct Tile { std::string title, desc; };
    Tile tiles[6] = {
        {"DECODE", "Hardware"},
        {"RENDER", "ps5-homebrew-ui on sceAgc"},
        {"MEDIA CORE", "FFmpeg"},
        {"TARGET", "Firmware below 13.60"},
        {"PROJECT", "github.com/sainsaji/EVO-PLAYER-PS5"},
        {"THEMES", themes_info_}
    };

    float start_x = cx - 440;
    float start_y = 520;
    for (int i = 0; i < 6; ++i)
    {
        float tx = start_x + (i % 3) * 300;
        float ty = start_y + (i / 3) * 120;
        list.rounded_rect({tx, ty, 280, 100}, 16, p.surface);
        list.bordered_rect({tx, ty, 280, 100}, 16, Color::rgb(0,0), 1.0f, kWhite.with_alpha(0.05f));
        ui::text(list, fonts.semibold, tiles[i].title, tx + 20, ty + 40, 14, p.accent, Align::left, 2.0f);
        ui::text(list, fonts.regular, fit(fonts.regular, tiles[i].desc, 16, 240), tx + 20, ty + 70, 16, kWhite);
    }

    Rect btn{cx - 150, 840, 300, 56};
    if (action_focused_)
    {
        list.glow(btn, 28, 20, p.accent.with_alpha(0.5f));
        list.rounded_rect(btn, 28, kWhite);
    }
    else
    {
        list.rounded_rect(btn, 28, kWhite.with_alpha(0.1f));
    }
    ui::text(list, fonts.semibold, "View Changelog", btn.cx(), btn.cy() + 8, 20, action_focused_ ? kInk : kWhite, Align::center);
}

bool AboutScreen::focus(FocusInfo *out) const
{
    if (action_focused_)
    {
        out->focusable.assign(1, {"about-changelog", "View Changelog"});
        out->id = "about-changelog";
        out->text = "View Changelog";
        out->index = out->total = 1;
        out->rect = {810, 840, 300, 56};
        return true;
    }
    return false;
}

/* --------------------------------------------------------- ChangelogScreen */

void ChangelogScreen::set(const evo_rmlui_changelog_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    title_ = str(p.title);
    subtitle_ = str(p.subtitle);
    releases_.clear();
    for (int i = 0; i < p.release_count && i < 8; ++i)
    {
        Release r;
        r.version = str(p.releases[i].version);
        r.tagline = str(p.releases[i].tagline);
        r.date = str(p.releases[i].date);
        r.focused = p.releases[i].is_focused != 0;
        releases_.push_back(r);
    }
    detail_version_ = str(p.detail_version);
    detail_tagline_ = str(p.detail_tagline);
    items_.clear();
    for (int i = 0; i < p.item_count && i < 12; ++i)
    {
        Item item;
        item.kind = str(p.items[i].kind);
        item.text = str(p.items[i].text);
        items_.push_back(item);
    }
    item_total_ = p.item_total;
}

void ChangelogScreen::enter()
{
    ring_snapped_ = false;
}

void ChangelogScreen::update(float dt)
{
    Rect target;
    for (size_t i = 0; i < releases_.size(); ++i)
    {
        if (releases_[i].focused)
        {
            target = {NavRail::kWidth + 64, 260.0f + i * 80.0f, 400.0f, 70.0f};
            break;
        }
    }
    if (target.w > 0)
    {
        if (!ring_snapped_)
        {
            ring_.snap(target);
            ring_snapped_ = true;
        }
        ring_.target(target);
        ring_.update(dt, 20.0f);
    }
}

void ChangelogScreen::draw(DrawList &list, const Context &ctx) const
{
    draw_backdrop(list, ctx);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    float lx = NavRail::kWidth + 64.0f;
    ui::text(list, fonts.display, title_, lx, 140, 48, kWhite);
    ui::text(list, fonts.regular, subtitle_, lx, 180, 20, p.text_muted);

    if (ring_snapped_)
    {
        list.rounded_rect(ring_.value(), 35, kWhite);
    }

    for (size_t i = 0; i < releases_.size(); ++i)
    {
        const Release &r = releases_[i];
        float y = 260.0f + i * 80.0f;
        Color ink = r.focused ? kInk : kWhite;
        Color ink_muted = r.focused ? kInk.with_alpha(0.6f) : p.text_muted;

        ui::text(list, fonts.semibold, r.version, lx + 20, y + 30, 22, ink);
        ui::text(list, fonts.regular, fit(fonts.regular, r.tagline, 16, 200), lx + 20, y + 54, 16, ink_muted);
        ui::text(list, fonts.regular, r.date, lx + 380, y + 42, 16, ink_muted, Align::right);
    }

    float rx = lx + 440.0f;
    Rect card{rx, 260, 1824 - rx, 740};
    list.rounded_rect(card, 24, p.surface);
    list.bordered_rect(card, 24, Color::rgb(0,0), 1.0f, kWhite.with_alpha(0.05f));

    ui::text(list, fonts.display, detail_version_, rx + 40, 330, 40, kWhite);
    ui::text(list, fonts.regular, detail_tagline_, rx + 40, 360, 20, p.text_muted);

    // Rows are sized by how many lines their text wraps to, and the list stops
    // at the card's bottom edge (leaving room for "+N more") instead of
    // running past it.
    const float text_w = card.w - 200.0f;
    const float bottom = card.y + card.h - 28.0f;
    const float more_h = 34.0f;
    float cy = 400.0f;
    int shown = 0;
    for (const Item &item : items_)
    {
        const int lines = std::min<int>(2, std::max<std::size_t>(1, fonts.regular.font->wrap(item.text, 18, text_w).size()));
        const float row_h = 30.0f + 26.0f * (lines - 1) + 14.0f;
        const bool last = shown + 1 == (int)items_.size() && item_total_ <= (int)items_.size();
        if (cy + row_h > bottom - (last ? 0.0f : more_h))
            break;

        Color kind_color;
        if (item.kind == "FIXED") kind_color = Color::rgb(0x30d158);
        else if (item.kind == "IMPROVED") kind_color = Color::rgb(0x8a8fff);
        else if (item.kind == "REMOVED") kind_color = Color::rgb(0xff5a5f);
        else kind_color = p.accent; // NEW

        list.rounded_rect({rx + 40, cy, 100, 30}, 15, kind_color.with_alpha(0.15f));
        ui::text(list, fonts.semibold, item.kind, rx + 90, cy + 21, 13, kind_color, Align::center, 1.0f);

        ui::paragraph(list, fonts.regular, item.text, rx + 160, cy + 22, 18, text_w, 26.0f, kWhite, 2);
        cy += row_h;
        ++shown;
    }

    if (item_total_ > shown)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "+%d more", item_total_ - shown);
        ui::text(list, fonts.regular, buf, rx + 160, cy + 22, 18, p.text_faint);
    }
}

bool ChangelogScreen::focus(FocusInfo *out) const
{
    for (size_t i = 0; i < releases_.size(); ++i)
    {
        if (releases_[i].focused)
        {
            out->focusable.clear();
            for (std::size_t k = 0; k < releases_.size(); ++k)
                out->focusable.emplace_back("release-" + std::to_string(k), releases_[k].version);
            out->id = "release-" + std::to_string(i);
            out->text = releases_[i].version + " " + releases_[i].tagline;
            out->index = static_cast<int>(i) + 1;
            out->total = static_cast<int>(releases_.size());
            out->rect = {NavRail::kWidth + 64, 260.0f + static_cast<float>(i) * 80.0f, 400.0f, 70.0f};
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------ ClosedScreen */

void ClosedScreen::enter() {}
void ClosedScreen::update(float dt) {}

void ClosedScreen::draw(DrawList &list, const Context &ctx) const
{
    draw_backdrop(list, ctx);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    float cx = 1920.0f * 0.5f;
    std::uint32_t logo = ctx.textures.icon("../icons/icon_logo.png");
    if (logo)
        list.image(logo, {cx - 60, 240, 120, 120}, ::hui::gfx::kFullUv, kWhite);

    ui::text(list, fonts.display, "EVO Player", cx, 440, 56, kWhite, Align::center);
    ui::text(list, fonts.semibold, "Safe to close", cx, 490, 28, p.accent, Align::center);
    
    ui::text(list, fonts.regular, "Press the PS button to open the control centre.", cx, 550, 20, p.text_muted, Align::center);
    ui::text(list, fonts.regular, "Highlight EVO Player and close it from the options menu.", cx, 580, 20, p.text_muted, Align::center);
}

bool ClosedScreen::focus(FocusInfo *out) const
{
    return false;
}

} // namespace evo::kit
