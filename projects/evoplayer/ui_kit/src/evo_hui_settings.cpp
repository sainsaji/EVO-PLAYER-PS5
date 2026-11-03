/*
 * evo_hui_settings.cpp - the settings screen on ps5-homebrew-ui. See the header.
 */
#include "evo_hui_settings.hpp"
#include "ui/widgets.hpp"

#include "ui/glyphs.hpp"

#include <algorithm>

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

constexpr float kLeft = NavRail::kWidth + 64.0f;
constexpr float kPreviewW = 560.0f; /* preview card, right of the rows */
constexpr float kBadgeMax = 170.0f;  /* a long value ("AUTO (MATCH CONTENT)") is cut short */
const Color kWhite = Color::rgb(0xffffff);
const Color kInk = Color::rgb(0x0b0d16);

std::string fit(const ui::FontRef &font, const std::string &text, float size, float width,
                float tracking = 0.0f)
{
    return font.font->fit(text, size, width, tracking);
}

struct SectionDef
{
    const char *label;
    const char *icon;
};

const SectionDef kSections[6] = {
    {"Video & Display", "../icons/icon_tv.png"},
    {"Audio", "../icons/icon_volume.png"},
    {"Subtitles", "../icons/icon_subtitles.png"},
    {"Interface & Storage", "../icons/icon_palette.png"},
    {"System & Diagnostics", "../icons/icon_developer_tools.png"},
    {"Experimental", "../icons/icon_sparkles.png"}};

} // namespace

void SettingsScreen::set(const evo_rmlui_settings_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    std::string new_title = str(p.title);
    if (new_title != title_ && !title_.empty())
    {
        section_age_ = 0.0f;
        scroll_.snap(0.0f);
        row_cursor_snapped_ = false;
    }
    title_ = new_title;
    subtitle_ = str(p.subtitle);
    counter_ = str(p.counter);
    params_ = p;

    rows_.clear();
    for (int i = 0; i < p.row_count && i < EVO_RMLUI_SETTINGS_ROWS; ++i)
    {
        RowData r;
        r.title = str(p.rows[i].title);
        r.detail = str(p.rows[i].detail);
        r.icon_path = str(p.rows[i].icon_path);
        r.badge = str(p.rows[i].badge);
        r.has_chevron = p.rows[i].has_chevron;
        r.is_focused = p.rows[i].is_focused;
        r.kind = p.rows[i].kind;
        r.toggle_on = p.rows[i].toggle_on;
        r.is_disabled = p.rows[i].is_disabled;
        r.preview = p.rows[i].preview;
        r.preview_index = p.rows[i].preview_index;
        r.benefit = str(p.rows[i].benefit);
        for (int c = 0; c < p.rows[i].choice_count && c < EVO_RMLUI_PREVIEW_CHOICES; ++c)
        {
            r.choices.push_back(str(p.rows[i].choice[c]));
            r.choice_bg.push_back(p.rows[i].choice_bg[c]);
            r.choice_accent.push_back(p.rows[i].choice_accent[c]);
        }
        if (p.rows[i].qr_modules && p.rows[i].qr_size > 0)
        {
            r.qr_size = p.rows[i].qr_size;
            r.qr.assign(p.rows[i].qr_modules, p.rows[i].qr_modules + static_cast<size_t>(r.qr_size) * r.qr_size);
        }
        rows_.push_back(r);
    }
    detect_preview();
}

void SettingsScreen::detect_preview()
{
    preview_ = SettingsPreview{};
    int focus = -1;
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].is_focused)
            focus = static_cast<int>(i);
    if (focus < 0 || params_.sidebar_focused || rows_[focus].preview == 0)
        return;
    /* An OPTION belongs to the VALUE row above it: the card is titled after that setting. */
    int owner = focus;
    while (owner > 0 && rows_[owner].kind == EVO_RMLUI_ROW_OPTION)
        --owner;
    const RowData &f = rows_[focus];
    preview_.kind = f.preview;
    preview_.index = f.preview_index;
    preview_.toggle = rows_[owner].kind == EVO_RMLUI_ROW_TOGGLE;
    preview_.title = rows_[owner].title;
    preview_.icon = rows_[owner].icon_path;
    preview_.benefit = f.benefit;
    preview_.detail = rows_[owner].detail;
    preview_.choices = f.choices;
    preview_.choice_bg = f.choice_bg;
    preview_.choice_accent = f.choice_accent;
    preview_.qr_size = f.qr_size;
    preview_.qr = f.qr;
}

void SettingsScreen::enter()
{
    age_ = 0.0f;
    section_age_ = 0.0f;
    scroll_.snap(0.0f);
    sidebar_cursor_snapped_ = false;
    row_cursor_snapped_ = false;
}

/* The card spans y 130..950; its header takes the first 160. */
constexpr float kRowsTop = 290.0f;
constexpr float kRowsBottom = 930.0f;

float SettingsScreen::row_top(int index) const
{
    float start_y = kRowsTop;
    float y_pos = start_y;
    for (int i = 0; i < index; ++i)
    {
        y_pos += (rows_[i].kind == EVO_RMLUI_ROW_OPTION ? 56.0f : 72.0f) + 8.0f;
    }
    return y_pos;
}

void SettingsScreen::update(float dt)
{
    age_ += dt;
    section_age_ += dt;

    for (int i = 0; i < EVO_RMLUI_SETTINGS_ROWS; ++i)
    {
        if (i < static_cast<int>(rows_.size()))
            toggles_[i].target = rows_[i].toggle_on ? 1.0f : 0.0f;
        toggles_[i].update(dt, 18.0f);
    }

    int sec_active = std::clamp(params_.section_active, 0, 5);
    Rect sidebar_target{kLeft, 210.0f + sec_active * (64.0f + 8.0f), 440.0f, 64.0f};
    if (!sidebar_cursor_snapped_)
    {
        sidebar_cursor_.snap(sidebar_target);
        sidebar_cursor_snapped_ = true;
    }
    sidebar_cursor_.target(sidebar_target);
    sidebar_cursor_.update(dt, 20.0f);

    Rect row_target;
    int focus_idx = -1;
    for (size_t i = 0; i < rows_.size(); ++i)
    {
        if (rows_[i].is_focused)
        {
            focus_idx = static_cast<int>(i);
            break;
        }
    }

    float max_scroll = 0.0f;
    if (!rows_.empty())
    {
        const float content_bottom = row_top(static_cast<int>(rows_.size()) - 1) +
                                     ((rows_.back().kind == EVO_RMLUI_ROW_OPTION) ? 56.0f : 72.0f);
        max_scroll = std::max(0.0f, content_bottom - kRowsBottom);
    }

    if (focus_idx >= 0)
    {
        float h = (rows_[focus_idx].kind == EVO_RMLUI_ROW_OPTION) ? 56.0f : 72.0f;
        /* Scroll the least that keeps the focused row inside the card. */
        const float top = row_top(focus_idx);
        float want = scroll_.target;
        if (top - want < kRowsTop)
            want = top - kRowsTop;
        /* An expanded value row lists its options right below it; bring the
         * whole list into view, but never scroll the focused row itself out
         * of the top of the card. */
        float bottom = top + h;
        for (size_t j = static_cast<size_t>(focus_idx) + 1;
             j < rows_.size() && rows_[j].kind == EVO_RMLUI_ROW_OPTION; ++j)
            bottom = row_top(static_cast<int>(j)) + 56.0f;
        if (bottom - want > kRowsBottom)
            want = std::min(bottom - kRowsBottom, top - kRowsTop);
        scroll_.target = std::clamp(want, 0.0f, max_scroll);
        if (!row_cursor_snapped_)
            scroll_.snap(scroll_.target);
        row_target = {kLeft + 480.0f + 24.0f, top - scroll_.target, 1824.0f - (kLeft + 480.0f) - 48.0f - (preview_.kind ? kPreviewW + 24.0f : 0.0f), h};
    }
    else
    {
        scroll_.target = std::clamp(scroll_.target, 0.0f, max_scroll);
    }

    if (!row_cursor_snapped_ && row_target.w > 0)
    {
        row_cursor_.snap(row_target);
        row_cursor_snapped_ = true;
    }
    if (row_target.w > 0)
        row_cursor_.target(row_target);
    row_cursor_.update(dt, 20.0f);
    scroll_.update(dt, 16.0f);
}

void SettingsScreen::draw_row(DrawList &list, const Context &ctx, const RowData &row, const Rect &r, int index) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    float x = r.x + 24.0f; // pad inside focus rect

    if (row.kind == EVO_RMLUI_ROW_OPTION)
    {
        x += 40.0f; // indented
        list.circle(x + 10, r.cy(), 10, kWhite.with_alpha(0.16f));
        if (row.toggle_on)
        {
            list.circle(x + 10, r.cy(), 10, p.accent);
            list.circle(x + 10, r.cy(), 4, kInk);
        }
        ui::text(list, fonts.regular, fit(fonts.regular, row.title, 24, std::max(0.0f, r.w - 120.0f)), x + 34, r.cy() + 7, 24, kWhite);
        return;
    }

    if (!row.icon_path.empty())
    {
        uint32_t icon = ctx.textures.icon(row.icon_path);
        if (icon)
        {
            list.rounded_rect({x, r.cy() - 20, 40, 40}, 12, kWhite.with_alpha(0.08f));
            list.image(icon, {x + 4, r.cy() - 16, 32, 32}, ::hui::gfx::kFullUv, kWhite);
        }
        x += 56.0f;
    }

    float text_y = row.detail.empty() ? r.cy() + 8 : r.cy() - 2;
    /* Text stops short of the badge / switch on the right, whatever the row width. */
    float reserve = 24.0f + (row.has_chevron ? 36.0f : 0.0f) + 16.0f;
    if (row.kind == EVO_RMLUI_ROW_TOGGLE)
        reserve += 64.0f;
    else if (row.kind == EVO_RMLUI_ROW_VALUE && !row.badge.empty())
        reserve += fonts.semibold.measure(fit(fonts.semibold, row.badge, 20, kBadgeMax), 20) + 24.0f;
    const float text_w = std::max(0.0f, r.x + r.w - reserve - x);
    ui::text(list, fonts.regular, fit(fonts.regular, row.title, 24, text_w), x, text_y, 24, kWhite);
    if (!row.detail.empty())
    {
        ui::text(list, fonts.regular, fit(fonts.regular, row.detail, 18, text_w), x, r.cy() + 20, 18, p.text_faint);
    }

    float right = r.x + r.w - 24.0f;

    if (row.has_chevron)
    {
        uint32_t chevron = ctx.textures.icon("../icons/icon_chevron.png");
        if (chevron)
        {
            list.image(chevron, {right - 24, r.cy() - 12, 24, 24}, ::hui::gfx::kFullUv, p.text_muted);
        }
        right -= 36.0f;
    }

    if (row.kind == EVO_RMLUI_ROW_TOGGLE)
    {
        float on = toggles_[index].value;
        Rect pill{right - 64, r.cy() - 17, 64, 34};
        const ui::Theme theme = p.kit_theme();
        ui::Painter paint(list, fonts, theme);
        paint.toggle(pill, on, {});
    }
    else if (row.kind == EVO_RMLUI_ROW_VALUE && !row.badge.empty())
    {
        const std::string badge = fit(fonts.semibold, row.badge, 20, kBadgeMax);
        float bw = fonts.semibold.measure(badge, 20) + 24.0f;
        Rect pill{right - bw, r.cy() - 16, bw, 32};
        list.rounded_rect(pill, 16, kWhite.with_alpha(0.12f));
        ui::text(list, fonts.semibold, badge, pill.cx(), pill.cy() + 7, 20, kWhite, Align::center);
    }
}

void SettingsScreen::draw(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    draw_backdrop(list, ctx, 0, 0.0f);

    list.push_opacity(tween::stagger(age_, 0, 0.05f, 0.5f));
    draw_page_title(list, ctx, "Settings", "Preferences", kLeft, 0.0f, 440.0f);
    list.pop_opacity();

    Rect sc = sidebar_cursor_.value();
    if (params_.sidebar_focused)
    {
        list.glow(sc, page::kRadiusTile, 18, p.accent.with_alpha(0.45f));
        list.rounded_rect(sc, page::kRadiusTile, kWhite);
    }

    for (int i = 0; i < 6; ++i)
    {
        float y = 210.0f + static_cast<float>(i) * 72.0f;
        Rect r{kLeft, y, 440.0f, 64.0f};

        bool active = (i == params_.section_active);
        bool focused = params_.sidebar_focused && active;

        if (!params_.sidebar_focused && active)
        {
            list.rounded_rect(sc, page::kRadiusTile, kWhite.with_alpha(0.10f));
            list.rounded_rect({sc.x + 8, sc.y + 16, 4, 32}, 2, p.accent);
        }

        Color ink = focused ? kInk : (active ? kWhite : kWhite.with_alpha(0.62f));
        uint32_t icon = ctx.textures.icon(kSections[i].icon);
        if (icon)
        {
            list.image(icon, {r.x + 24, r.y + 16, 32, 32}, ::hui::gfx::kFullUv, ink);
        }
        ui::text(list, focused ? fonts.semibold : fonts.regular,
                 fit(focused ? fonts.semibold : fonts.regular, kSections[i].label, 26, r.w - 72.0f - 24.0f),
                 r.x + 72, r.cy() + 9, 26, ink);
    }

    Rect pane{kLeft + 480.0f, 130.0f, 1824.0f - (kLeft + 480.0f), 820.0f};
    list.shadow(pane, page::kRadiusCard, 40, Color::rgb(0x000000, 0.4f));
    list.rounded_rect(pane, page::kRadiusCard, p.surface);
    list.bordered_rect(pane, page::kRadiusCard, Color::rgb(0x000000, 0.0f), 1.0f, p.border);

    list.push_clip(pane);

    float px = pane.x + 48.0f;
    float py = pane.y + 56.0f;

    /* The section's own heading: title, then the line under it, as on every page. */
    const float counter_w = counter_.empty() ? 0.0f : (fonts.regular.measure(counter_, 22) + 24.0f);
    const float title_w = std::max(0.0f, pane.w - 96.0f - counter_w);
    ui::text(list, fonts.display, fit(fonts.display, title_, 40, title_w), px - 2, py + 36, 40, kWhite);
    if (!counter_.empty())
    {
        ui::text(list, fonts.regular, counter_, pane.x + pane.w - 48.0f, py + 36, 22, p.text_faint, Align::right);
    }
    ui::text(list, fonts.semibold, fit(fonts.semibold, ui::upper(subtitle_), 18, pane.w - 96.0f, 3.0f), px, py + 70, 18,
             p.text_muted, Align::left, 3.0f);

    if (!params_.sidebar_focused && row_cursor_snapped_)
    {
        Rect rc = row_cursor_.value();
        list.glow(rc, page::kRadiusTile, 20, p.accent.with_alpha(0.4f));
    }

    list.push_clip({pane.x, kRowsTop - 12.0f, pane.w, kRowsBottom - kRowsTop + 24.0f});
    for (size_t i = 0; i < rows_.size(); ++i)
    {
        float row_in = tween::stagger(section_age_, static_cast<int>(i), 0.05f, 0.4f);
        list.push_opacity(row_in * (rows_[i].is_disabled ? 0.4f : 1.0f));

        float h = (rows_[i].kind == EVO_RMLUI_ROW_OPTION) ? 56.0f : 72.0f;
        Rect r{pane.x + 24.0f, row_y(static_cast<int>(i)), pane.w - 48.0f - (preview_.kind ? kPreviewW + 24.0f : 0.0f), h};

        if (rows_[i].is_focused && !params_.sidebar_focused)
        {
            list.rounded_rect(r, page::kRadiusTile, kWhite.with_alpha(0.14f));
        }

        draw_row(list, ctx, rows_[i], r, static_cast<int>(i));
        list.pop_opacity();
    }

    if (!params_.sidebar_focused && row_cursor_snapped_)
    {
        Rect rc = row_cursor_.value();
        list.bordered_rect(rc, page::kRadiusTile, Color::rgb(0x000000, 0.0f), 3.0f, kWhite);
    }

    list.pop_clip();
    if (preview_.kind)
        draw_settings_preview(list, ctx, {pane.x + pane.w - kPreviewW - 24.0f, kRowsTop - 12.0f, kPreviewW, kRowsBottom - kRowsTop + 24.0f}, preview_);
    list.pop_clip();

    const ui::Hint hints[] = {
        {ui::Button::cross, "Select"},
        {ui::Button::dpad, "Navigate"},
        {ui::Button::circle, "Back"}};
    draw_page_hints(list, fonts, hints, 3);
}

bool SettingsScreen::focus(FocusInfo *out) const
{
    if (params_.sidebar_focused)
    {
        if (params_.section_active >= 0 && params_.section_active < 6)
        {
            if (out)
            {
                out->id = "section-" + std::to_string(params_.section_active);
                out->text = kSections[params_.section_active].label;
                out->index = params_.section_active + 1;
                out->total = 6;
                out->rect = {kLeft, 210.0f + static_cast<float>(params_.section_active) * 72.0f, 440.0f, 64.0f};
                for (int i = 0; i < 6; ++i)
                {
                    out->focusable.push_back({"section-" + std::to_string(i), kSections[i].label});
                }
            }
            return true;
        }
    }
    else
    {
        int focus_idx = -1;
        for (size_t i = 0; i < rows_.size(); ++i)
        {
            if (rows_[i].is_focused)
            {
                focus_idx = static_cast<int>(i);
                break;
            }
        }
        if (focus_idx >= 0)
        {
            if (out)
            {
                out->id = "row-" + std::to_string(focus_idx);
                std::string t = rows_[focus_idx].title;
                if (rows_[focus_idx].kind == EVO_RMLUI_ROW_VALUE)
                {
                    t += " = " + rows_[focus_idx].badge;
                }
                else if (rows_[focus_idx].kind == EVO_RMLUI_ROW_TOGGLE)
                {
                    t += rows_[focus_idx].toggle_on ? " = ON" : " = OFF";
                }
                out->text = t;
                out->index = focus_idx + 1;
                out->total = static_cast<int>(rows_.size());
                out->rect = row_cursor_.value();
                for (size_t i = 0; i < rows_.size(); ++i)
                {
                    out->focusable.push_back({"row-" + std::to_string(i), rows_[i].title});
                }
            }
            return true;
        }
    }
    return false;
}

} // namespace evo::kit
