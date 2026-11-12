#include "evo_hui_misc.hpp"

#include "ui/glyphs.hpp"
#include "ui/components/progress.hpp"
#include "ui/components/scroll_area.hpp"
#include "ui/widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

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
        ui::ProgressBar bar;
        bar.style.theme = p.kit_theme();
        bar.style.height = 6.0f;
        bar.style.radius_source = ui::RadiusSource::pill;
        bar.style.track = ui::TrackStyle::flat;
        bar.style.placement = ui::LabelPlacement::none;
        bar.style.percent = false;
        bar.style.finish_flash = false;
        bar.set_bounds({card.x + 40, cy + 10, w - 80, 6});
        bar.set_value(std::max(0.02f, static_cast<float>(progress_pct_)), true);
        ui::Canvas canvas{list, fonts, 0, ctx.time};
        bar.draw(canvas);
        cy += 30.0f;
    }

    if (!actions_.empty())
    {
        ui::Theme theme = p.kit_theme();
        theme.primary = p.accent;
        theme.on_primary = ui::Painter::on(p.accent);
        theme.secondary = kWhite.with_alpha(0.1f);
        theme.on_secondary = kWhite;
        theme.radius = 100.0f;
        ui::Painter paint(list, fonts, theme);
        float aw = (w - 80 - (actions_.size() - 1) * 20.0f) / actions_.size();
        for (std::size_t i = 0; i < actions_.size(); ++i)
        {
            Rect btn{card.x + 40 + i * (aw + 20), card.y + card.h - 80, aw, 48};
            ui::Look look;
            look.focus = (int)i == focused_action_ ? 1.0f : 0.0f;
            paint.button(btn, fit(fonts.semibold, actions_[i].label, 18, aw - 20),
                         actions_[i].is_primary ? ui::ButtonKind::primary : ui::ButtonKind::secondary, look);
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
    busy_ = p.busy != 0;
}

void ToastOverlay::enter()
{
}

void ToastOverlay::update(float dt)
{
    time_ = std::fmod(time_ + dt, 1000.0f);
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

    /* A message with a second line is a card that asks something (the update card,
     * #120): the first line is the detail, wrapped, the second is the button hint. */
    const size_t nl = message_.find('\n');
    if (nl != std::string::npos) {
        const std::string detail = message_.substr(0, nl);
        const std::string hint = message_.substr(nl + 1);
        const float w = 540.0f, inner = w - 72.0f;
        std::vector<std::string> lines;
        std::string cur;
        size_t at = 0;
        while (at <= detail.size() && lines.size() < 3) {
            size_t sp = detail.find(' ', at);
            if (sp == std::string::npos) sp = detail.size();
            const std::string word = detail.substr(at, sp - at);
            const std::string trial = cur.empty() ? word : cur + " " + word;
            if (!cur.empty() && fonts.regular.measure(trial, 18) > inner) {
                lines.push_back(cur);
                cur = word;
            } else {
                cur = trial;
            }
            at = sp + 1;
        }
        if (!cur.empty() && lines.size() < 3) lines.push_back(cur);
        const float h = 58.0f + 26.0f * (float)lines.size() + 78.0f;
        Rect card{1920.0f - w - 40.0f + slide_, 990.0f - h, w, h};
        list.shadow({card.x, card.y + 10, card.w, card.h}, 20, 24, Color::rgb(0x000000, 0.5f));
        list.rounded_rect(card, 24, p.bg_top.with_alpha(0.97f));
        list.bordered_rect(card, 24, Color::rgb(0, 0), 1.0f, kWhite.with_alpha(0.12f));
        list.rounded_rect({card.x + 20, card.y + 22, 6, h - 44}, 3, acc);
        float y = card.y + 44;
        ui::text(list, fonts.semibold, fit(fonts.semibold, title_, 22, inner), card.x + 44, y, 22, kWhite);
        y += 14;
        for (const std::string &l : lines) {
            y += 26;
            ui::text(list, fonts.regular, l, card.x + 44, y, 18, p.text_muted);
        }
        /* The hint is "PRIMARY|SECONDARY": two buttons, Cross and Circle, which the
         * card owns while it is up (Application.cpp reads those two presses). */
        const size_t bar = hint.find('|');
        const std::string labels[2] = {hint.substr(0, bar), bar == std::string::npos ? "" : hint.substr(bar + 1)};
        const ui::Button glyphs[2] = {ui::Button::cross, ui::Button::circle};
        const float gap = 14.0f, bw = (inner - gap) / 2.0f, bh = 46.0f, by = card.y + h - bh - 20.0f;
        for (int i = 0; i < 2; ++i)
        {
            if (labels[i].empty()) continue;
            const Rect b{card.x + 44 + (float)i * (bw + gap), by, bw, bh};
            const bool primary = i == 0;
            list.rounded_rect(b, bh / 2, primary ? acc : kWhite.with_alpha(0.10f));
            if (primary)
                list.bordered_rect(Rect{b.x - 3, b.y - 3, b.w + 6, b.h + 6}, bh / 2 + 3, Color::rgb(0, 0), 2.0f, kWhite.with_alpha(0.9f));
            const float gw = ui::button_width(glyphs[i], 28.0f);
            const float tw = fonts.semibold.measure(labels[i], 18);
            float x = b.x + (bw - (gw + 10.0f + tw)) / 2.0f;
            ui::draw_button(list, fonts, ui::GlyphStyle::dark(), glyphs[i], x, b.y + bh / 2, 28.0f);
            ui::text(list, fonts.semibold, labels[i], x + gw + 10.0f, b.y + bh / 2 + 6.5f, 18,
                     primary ? ui::Painter::on(acc) : kWhite);
        }
        list.pop_opacity();
        return;
    }

    /* Busy: a spinner where the accent bar goes, the text moved past it. */
    const float spin = busy_ ? 40.0f : 0.0f;
    float text_w = std::max(fonts.semibold.measure(title_, 20), fonts.regular.measure(message_, 18));
    float w = std::min(620.0f + spin, text_w + 70.0f + spin);
    float h = 64.0f;
    Rect card{1920.0f - w - 40.0f + slide_, 990.0f - h, w, h};

    list.shadow({card.x, card.y + 10, card.w, card.h}, 20, 24, Color::rgb(0x000000, 0.5f));
    list.rounded_rect(card, 32, p.bg_top.with_alpha(0.95f));
    list.bordered_rect(card, 32, Color::rgb(0,0), 1.0f, kWhite.with_alpha(0.12f));

    if (busy_) {
        const float sx = card.x + 38.0f, sy = card.y + h / 2.0f;
        list.arc(sx, sy, 14.0f, 4.0f, 0.0f, 6.2831853f, kWhite.with_alpha(0.15f));
        list.arc(sx, sy, 14.0f, 4.0f, std::fmod(time_ * 5.5f, 6.2831853f), 1.6f, acc);
    } else {
        list.rounded_rect({card.x + 16, card.y + 16, 6, 32}, 3, acc);
    }

    float cy = card.y + 24;
    ui::text(list, fonts.semibold, fit(fonts.semibold, title_, 20, w - 60 - spin), card.x + 36 + spin, cy, 20, kWhite);
    ui::text(list, fonts.regular, fit(fonts.regular, message_, 18, w - 60 - spin), card.x + 36 + spin, cy + 22, 18, p.text_muted);

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

namespace
{
/* About's layout: the identity card on the left, the facts on the right. */
constexpr Rect kAboutCard{page::kLeft, 236.0f, 560.0f, 724.0f};
constexpr float kAboutTilesX = kAboutCard.x + kAboutCard.w + 32.0f;
constexpr float kAboutTileW = (page::kRight - kAboutTilesX - 32.0f) * 0.5f;
constexpr float kAboutTileH = 220.0f;
constexpr Rect kAboutButton{kAboutCard.x + 80.0f, kAboutCard.y + kAboutCard.h - 120.0f, kAboutCard.w - 160.0f, 64.0f};
} // namespace

void AboutScreen::draw(DrawList &list, const Context &ctx) const
{
    draw_backdrop(list, ctx);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    draw_page_title(list, ctx, "About", tagline_);

    /* identity card: logo, name, version and build, the changelog button */
    const Rect card = kAboutCard;
    list.rounded_rect(card, page::kRadiusCard, p.surface);
    list.bordered_rect(card, page::kRadiusCard, Color::rgb(0, 0), 1.0f, kWhite.with_alpha(0.06f));
    const float cx = card.cx();
    std::uint32_t logo = ctx.textures.icon("../icons/icon_logo.png");
    if (logo)
        list.image(logo, {cx - 72, card.y + 96, 144, 144}, ::hui::gfx::kFullUv, kWhite);
    ui::text(list, fonts.display, fit(fonts.display, app_name_, 44, card.w - 64), cx, card.y + 320, 44, kWhite,
             Align::center);

    const float w1 = fonts.semibold.measure(version_, 18, 1.0f) + 36;
    const float w2 = fonts.semibold.measure(build_tag_, 18, 1.0f) + 36;
    const float chips_w = std::min(card.w - 48.0f, w1 + w2 + 12);
    float px = cx - chips_w * 0.5f;
    list.rounded_rect({px, card.y + 360, w1, 40}, page::kRadiusChip, kWhite.with_alpha(0.1f));
    ui::text(list, fonts.semibold, version_, px + w1 * 0.5f, card.y + 387, 18, kWhite, Align::center, 1.0f);
    px += w1 + 12;
    const float w2_fit = std::min(w2, card.x + card.w - 24.0f - px);
    list.rounded_rect({px, card.y + 360, w2_fit, 40}, page::kRadiusChip, kWhite.with_alpha(0.1f));
    ui::text(list, fonts.semibold, fit(fonts.semibold, build_tag_, 18, w2_fit - 36), px + w2_fit * 0.5f, card.y + 387, 18,
             kWhite, Align::center, 1.0f);

    const Rect btn = kAboutButton;
    if (action_focused_)
    {
        list.glow(btn, btn.h * 0.5f, 20, p.accent.with_alpha(0.5f));
        list.rounded_rect(btn, btn.h * 0.5f, kWhite);
    }
    else
    {
        list.rounded_rect(btn, btn.h * 0.5f, kWhite.with_alpha(0.1f));
    }
    ui::text(list, fonts.semibold, "View Changelog", btn.cx(), btn.cy() + 8, 22, action_focused_ ? kInk : kWhite,
             Align::center);

    /* the facts */
    struct Tile
    {
        const char *title, *desc;
    };
    const Tile tiles[6] = {
        {"DECODE", "Hardware (sceVideodec2), FFmpeg when the hardware cannot"},
        {"RENDER", "ps5-homebrew-ui on sceAgc"},
        {"MEDIA CORE", "FFmpeg"},
        {"TARGET", "PS5 firmware 13.60 and below"},
        {"PROJECT", "github.com/sainsaji/EVO-PLAYER-PS5"},
        {"HELP", "Settings \xE2\x80\xBA System & Diagnostics \xE2\x80\xBA Report a problem"},
    };
    for (int i = 0; i < 6; ++i)
    {
        const Rect t{kAboutTilesX + static_cast<float>(i % 2) * (kAboutTileW + 32.0f),
                     card.y + static_cast<float>(i / 2) * (kAboutTileH + 32.0f), kAboutTileW, kAboutTileH};
        list.rounded_rect(t, page::kRadiusTile, p.surface);
        list.bordered_rect(t, page::kRadiusTile, Color::rgb(0, 0), 1.0f, kWhite.with_alpha(0.05f));
        ui::text(list, fonts.semibold, tiles[i].title, t.x + 32, t.y + 56, 16, p.accent, Align::left, 2.0f);
        ui::paragraph(list, fonts.regular, tiles[i].desc, t.x + 32, t.y + 104, 24, t.w - 64, 34, kWhite, 3);
    }

    const ui::Hint hints[] = {
        {ui::Button::cross, "Changelog"}, {ui::Button::square, "Dev tools"}, {ui::Button::circle, "Back"}};
    draw_page_hints(list, fonts, hints, 3);
}

bool AboutScreen::focus(FocusInfo *out) const
{
    if (action_focused_)
    {
        out->focusable.assign(1, {"about-changelog", "View Changelog"});
        out->id = "about-changelog";
        out->text = "View Changelog";
        out->index = out->total = 1;
        out->rect = kAboutButton;
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
    if (str(p.detail_version) != detail_version_)
        scroll_.scroll_to(0.0f, 0.0f, true);
    detail_version_ = str(p.detail_version);
    detail_tagline_ = str(p.detail_tagline);
    items_.clear();
    for (int i = 0; i < p.item_count && i < EVO_RMLUI_CL_ITEMS_MAX; ++i)
    {
        Item item;
        item.kind = str(p.items[i].kind);
        item.text = str(p.items[i].text);
        items_.push_back(item);
    }
    item_total_ = p.item_total;
    stick_ = p.scroll_stick;
}

void ChangelogScreen::enter()
{
    ring_snapped_ = false;
}

void ChangelogScreen::update(float dt)
{
    if (stick_ != 0.0f)
        scroll_.scroll_by(0.0f, stick_ * std::fabs(stick_) * 1500.0f * dt);
    scroll_.update(dt);
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

    float lx = page::kLeft;
    draw_page_title(list, ctx, title_, subtitle_, lx);

    if (ring_snapped_)
    {
        list.rounded_rect(ring_.value(), page::kRadiusTile, kWhite);
    }

    for (size_t i = 0; i < releases_.size(); ++i)
    {
        const Release &r = releases_[i];
        float y = 260.0f + i * 80.0f;
        Color ink = r.focused ? kInk : kWhite;
        Color ink_muted = r.focused ? kInk.with_alpha(0.6f) : p.text_muted;

        /* Date up beside the version, so the release name gets the whole row. */
        ui::text(list, fonts.semibold, r.version, lx + 20, y + 30, 22, ink);
        ui::text(list, fonts.regular, r.date, lx + 380, y + 30, 15, ink_muted, Align::right);
        ui::text(list, fonts.regular, fit(fonts.regular, r.tagline, 15, 360), lx + 20, y + 54, 15, ink_muted);
    }

    float rx = lx + 440.0f;
    Rect card{rx, 260, 1824 - rx, 740};
    list.rounded_rect(card, page::kRadiusCard, p.surface);
    list.bordered_rect(card, page::kRadiusCard, Color::rgb(0,0), 1.0f, kWhite.with_alpha(0.05f));

    ui::text(list, fonts.display, detail_version_, rx + 40, 330, 40, kWhite);
    ui::text(list, fonts.regular, detail_tagline_, rx + 40, 360, 20, p.text_muted);

    // The rows live in a kit ScrollArea: the right stick scrolls them, the
    // edges fade and a thumb shows the position. A row is as tall as its text
    // wraps (one or two lines).
    const float text_w = card.w - 200.0f;
    const float view_room = card.y + card.h - 392.0f - 20.0f;
    float content_h = 0.0f;
    std::vector<float> heights;
    heights.reserve(items_.size());
    for (const Item &item : items_)
    {
        const int lines = std::min<int>(2, std::max<std::size_t>(1, fonts.regular.font->wrap(item.text, 18, text_w).size()));
        heights.push_back(30.0f + 26.0f * (lines - 1) + 14.0f);
        content_h += heights.back();
    }
    /* The view ends on a row boundary, so the list at rest never shows a row sliced through. */
    float view_h = view_room;
    if (content_h + 8.0f > view_room)
    {
        float edge = 8.0f;
        for (float h : heights)
        {
            if (edge + h > view_room)
                break;
            edge += h;
        }
        view_h = edge;
    }
    const Rect view{rx + 40, 392.0f, card.w - 80.0f, view_h};
    scroll_.set_bounds(view);
    scroll_.set_content_size(view.w, content_h);

    ui::Canvas canvas{list, fonts, 0, ctx.time};
    scroll_.begin(canvas);
    float cy = 8.0f;
    for (size_t idx = 0; idx < items_.size(); ++idx)
    {
        const Item &item = items_[idx];
        const Rect row{0.0f, cy, view.w, heights[idx]};
        const float vis = scroll_.visibility(row);
        if (vis > 0.01f)
        {
            Color kind_color;
            if (item.kind == "FIXED") kind_color = Color::rgb(0x30d158);
            else if (item.kind == "IMPROVED") kind_color = Color::rgb(0x8a8fff);
            else if (item.kind == "REMOVED") kind_color = Color::rgb(0xff5a5f);
            else kind_color = p.accent; // NEW

            list.push_opacity(vis);
            list.rounded_rect({0.0f, cy, 100, 30}, 15, kind_color.with_alpha(0.15f));
            ui::text(list, fonts.semibold, item.kind, 50.0f, cy + 21, 13, kind_color, Align::center, 1.0f);
            ui::paragraph(list, fonts.regular, item.text, 120.0f, cy + 22, 18, text_w, 26.0f, kWhite, 2);
            list.pop_opacity();
        }
        cy += heights[idx];
    }
    scroll_.end(canvas);
    scroll_.draw(canvas);

    const ui::Hint hints[] = {{ui::Button::dpad, "Navigate"}, {ui::Button::circle, "Back"}};
    draw_page_hints(list, fonts, hints, 2);
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
