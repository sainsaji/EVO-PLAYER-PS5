/*
 * evo_hui_modals.cpp - the player's pop-ups on ps5-homebrew-ui. See the header.
 */
#include "evo_hui_modals.hpp"

#include "ui/glyphs.hpp"

#include <algorithm>
#include <cmath>
#include <string>
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
const Color kBlack = Color::rgb(0x000000);
const Color kClear = Color::rgb(0x000000, 0.0f);

constexpr float kSheetX = 1120.0f;
constexpr float kSheetW = 1920.0f - kSheetX - 48.0f;
constexpr float kTrackTop = 250.0f;
constexpr float kTrackH = 74.0f;

std::string fit(const ui::FontRef &font, const std::string &text, float size, float width)
{
    return font.font->fit(text, size, width);
}

void hint_row(DrawList &list, const ui::Fonts &fonts, const ui::Hint *hints, int n, float right, float cy = 1030.0f)
{
    ui::HintLayout layout;
    layout.size = 28;
    layout.text_size = 18;
    layout.item_gap = 26.0f;
    layout.cy = cy;
    ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), hints, n, right, true, layout);
}

} // namespace

/* ------------------------------------------------------- subtitle picker */

void SubtitlePicker::set(const evo_rmlui_subtitles_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    eyebrow_ = str(p.eyebrow);
    title_ = str(p.title);
    size_ = str(p.size_str);
    /* The values carry their own "SYNC: " / "2ND: " prefix for the RmlUi
     * screen; the pills here already print a label, so drop it. */
    auto unprefix = [](std::string s, const char *prefix) {
        const std::size_t n = std::char_traits<char>::length(prefix);
        return s.compare(0, n, prefix) == 0 ? s.substr(n) : s;
    };
    sync_ = unprefix(str(p.sync_str), "SYNC: ");
    sync2_ = unprefix(str(p.sync2_str), "2ND: ");
    preview_ = str(p.preview_text);
    preview2_ = str(p.preview_text2);
    preview_face_ = p.preview_face;
    has_secondary_ = p.has_secondary != 0;
    secondary_position_ = p.secondary_position;
    secondary_color_ = p.secondary_color;
    primary_color_ = p.primary_color;
    tracks_.clear();
    for (int i = 0; i < p.track_count && i < 8; ++i)
    {
        Track t;
        t.label = str(p.tracks[i].label);
        t.detail = str(p.tracks[i].detail);
        t.tag = str(p.tracks[i].tag);
        t.current = p.tracks[i].is_current != 0;
        t.focused = p.tracks[i].is_focused != 0;
        t.action = p.tracks[i].is_action != 0;
        t.disabled = p.tracks[i].is_disabled != 0;
        tracks_.push_back(t);
    }
}

void SubtitlePicker::enter()
{
    open_.snap(0.0f);
    ring_snapped_ = false;
}

Rect SubtitlePicker::track_rect(int i) const
{
    return {kSheetX + 28, kTrackTop + static_cast<float>(i) * (kTrackH + 8.0f), kSheetW - 56, kTrackH};
}

void SubtitlePicker::update(float dt)
{
    open_.target = 1.0f;
    open_.update(dt, 16.0f);
    for (std::size_t i = 0; i < tracks_.size(); ++i)
    {
        if (!tracks_[i].focused)
            continue;
        const Rect t = track_rect(static_cast<int>(i));
        if (!ring_snapped_)
        {
            ring_.snap(t);
            ring_snapped_ = true;
        }
        ring_.target(t);
    }
    ring_.update(dt, 22.0f);
}

void SubtitlePicker::draw(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const float o = open_.value;
    list.rounded_rect({0, 0, 1920, 1080}, 0, kBlack.with_alpha(0.62f * o));

    /* The sheet slides in from the right. */
    const float dx = 80.0f * (1.0f - tween::cubic_out(o));
    list.push_transform(1.0f, 0, 0, dx, 0);
    list.push_opacity(o);
    const Rect sheet{kSheetX, 48, kSheetW, 1080 - 96};
    list.shadow({sheet.x - 10, sheet.y + 20, sheet.w, sheet.h}, page::kRadiusCard, 60, kBlack.with_alpha(0.6f));
    list.rounded_rect(sheet, page::kRadiusCard, p.bg_top.with_alpha(0.96f));
    list.bordered_rect(sheet, page::kRadiusCard, kClear, 1.5f, kWhite.with_alpha(0.12f));

    ui::text(list, fonts.semibold, ui::upper(eyebrow_.empty() ? "Subtitles" : eyebrow_), sheet.x + 36,
             sheet.y + 70, 16, p.accent, Align::left, 3.0f);
    ui::text(list, fonts.display, fit(fonts.display, title_.empty() ? "Subtitle track" : title_, 36, sheet.w - 72),
             sheet.x + 34, sheet.y + 118, 36, kWhite);
    /* Size and sync, as pills under the title. */
    float px = sheet.x + 36;
    auto pill = [&](const std::string &label, const std::string &value, ui::Button btn = ui::Button::none) {
        if (value.empty())
            return;
        const std::string t = label + "  " + value;
        const float text_w = fonts.semibold.measure(t, 16, 1.0f);
        const float btn_w = (btn != ui::Button::none) ? (ui::button_width(btn, 18.0f) + 8.0f) : 0.0f;
        const float w = text_w + btn_w + 28.0f;
        list.rounded_rect({px, sheet.y + 140, w, 34}, 17, kWhite.with_alpha(0.08f));
        float tx = px + 14.0f;
        if (btn != ui::Button::none) {
            ui::draw_button(list, fonts, ui::GlyphStyle::dark(), btn, tx, sheet.y + 157.0f, 18.0f);
            tx += btn_w;
        }
        ui::text(list, fonts.semibold, t, tx, sheet.y + 163, 16, p.text_muted, Align::left, 1.0f);
        px += w + 10;
    };
    pill("SIZE", size_, ui::Button::triangle);
    pill("SYNC", sync_);
    pill("SYNC 2", sync2_);

    list.rounded_rect(ring_.value(), page::kRadiusTile, kWhite);
    for (std::size_t i = 0; i < tracks_.size(); ++i)
    {
        const Track &t = tracks_[i];
        const Rect r = track_rect(static_cast<int>(i));
        list.push_opacity(t.disabled ? 0.38f : 1.0f);
        const Color ink = t.focused ? kInk : kWhite;
        if (!t.action)
        {
            /* Radio mark: filled when this is the track playing. */
            const float cx = r.x + 30, cy = r.cy();
            list.ring(cx, cy, 11, 2.0f, t.focused ? kInk.with_alpha(0.5f) : kWhite.with_alpha(0.4f));
            if (t.current)
                list.circle(cx, cy, 6, t.focused ? kInk : p.accent);
        }
        const float tx = r.x + 60;
        float right = r.x + r.w - 20;
        if (!t.tag.empty())
        {
            const float w = fonts.semibold.measure(t.tag, 14, 1.5f) + 22;
            const Rect tag{right - w, r.cy() - 14, w, 28};
            list.rounded_rect(tag, 14, t.focused ? kInk.with_alpha(0.12f) : p.accent.with_alpha(0.2f));
            ui::text(list, fonts.semibold, t.tag, tag.cx(), tag.y + 19, 14, t.focused ? kInk : p.accent,
                     Align::center, 1.5f);
            right = tag.x - 14;
        }
        ui::text(list, fonts.semibold, fit(fonts.semibold, t.label, 22, right - tx), tx, t.detail.empty() ? r.cy() + 8 : r.y + 33, 22, ink);
        if (!t.detail.empty())
            ui::text(list, fonts.regular, fit(fonts.regular, t.detail, 16, right - tx), tx, r.y + 57, 16,
                     t.focused ? kInk.with_alpha(0.6f) : p.text_faint);
        list.pop_opacity();
    }

    const ui::Hint hints[] = {{ui::Button::cross, "Select"},
                              {ui::Button::square, "2nd track"},
                              {ui::Button::triangle, "Size"},
                              {ui::Button::dpad, "Sync"},
                              {ui::Button::circle, "Back"}};
    hint_row(list, fonts, hints, 5, sheet.x + sheet.w - 36, sheet.y + sheet.h - 44);
    list.pop_opacity();
    list.pop_transform();

    /* The preview caption, in the size that is selected, where it will show. */
    if ((!preview_.empty() || !preview2_.empty()) && o > 0.01f)
    {
        static const Color kSub1[4] = {
            Color::rgb(0xffffff), Color::rgb(0xffe14d), Color::rgb(0x66e3ff), Color::rgb(0x7dff8a)
        };
        static const Color kSub2[3] = {
            Color::rgb(0xffe14d), Color::rgb(0x66e3ff), Color::rgb(0xffffff)
        };

        const float size = preview_face_ == 0 ? 40.0f : (preview_face_ == 2 ? 60.0f : (preview_face_ == 3 ? 72.0f : 50.0f));
        const float cx = kSheetX * 0.5f;
        const float max_w = kSheetX - 120;
        list.push_opacity(o);

        auto wrap_lines = [&](const std::string &text, float sz) -> std::vector<std::string> {
            if (text.empty()) return {};
            std::vector<std::string> lines = fonts.semibold.font->wrap(text, sz, max_w);
            if (lines.size() > 1)
            {
                const float even = fonts.semibold.measure(text, sz) / static_cast<float>(lines.size()) * 1.15f;
                std::vector<std::string> balanced = fonts.semibold.font->wrap(text, sz, std::min(even, max_w));
                if (balanced.size() == lines.size())
                    lines = std::move(balanced);
            }
            if (lines.size() > 2)
                lines.resize(2);
            return lines;
        };

        auto draw_block = [&](const std::vector<std::string> &lines, float top_y, float sz, Color ink) {
            const float line_h = sz * 1.25f;
            float y = top_y;
            for (const std::string &line : lines)
            {
                const float w = std::min(fonts.semibold.measure(line, sz), max_w);
                list.rounded_rect({cx - w * 0.5f - 18, y - sz * 0.95f, w + 36, line_h}, 10, kBlack.with_alpha(0.55f));
                ui::text(list, fonts.semibold, fit(fonts.semibold, line, sz, max_w), cx, y, sz, ink,
                         Align::center);
                y += line_h;
            }
        };

        const Color col1 = kSub1[std::clamp(primary_color_, 0, 3)];
        const Color col2 = kSub2[std::clamp(secondary_color_, 0, 2)];
        const float sz2 = size * 0.85f;

        std::vector<std::string> lines1 = wrap_lines(preview_, size);
        std::vector<std::string> lines2 = has_secondary_ ? wrap_lines(preview2_, sz2) : std::vector<std::string>();

        if (has_secondary_ && secondary_position_ == 1)
        {
            // Secondary at TOP
            if (!lines2.empty())
            {
                ui::text(list, fonts.semibold, "SECONDARY (TOP)", cx, 140, 13, p.text_faint, Align::center, 2.5f);
                draw_block(lines2, 175, sz2, col2);
            }
            if (!lines1.empty())
            {
                ui::text(list, fonts.semibold, "PREVIEW (PRIMARY)", cx, 730, 14, p.text_faint, Align::center, 3.0f);
                draw_block(lines1, 770, size, col1);
            }
        }
        else if (has_secondary_)
        {
            // Secondary STACKED directly above primary
            const float h1 = static_cast<float>(lines1.size()) * size * 1.25f;
            const float h2 = static_cast<float>(lines2.size()) * sz2 * 1.25f;
            const float primary_top = 840.0f - h1;
            const float sec_top = primary_top - h2 - 16.0f;

            ui::text(list, fonts.semibold, "PREVIEW (DUAL SUBTITLES)", cx, sec_top - sz2 * 0.95f - 24.0f, 14, p.text_faint, Align::center, 3.0f);
            if (!lines2.empty())
                draw_block(lines2, sec_top, sz2, col2);
            if (!lines1.empty())
                draw_block(lines1, primary_top, size, col1);
        }
        else
        {
            // Single primary preview
            if (!lines1.empty())
            {
                ui::text(list, fonts.semibold, "PREVIEW", cx, 730, 14, p.text_faint, Align::center, 3.0f);
                draw_block(lines1, 770, size, col1);
            }
        }

        list.pop_opacity();
    }
}

bool SubtitlePicker::focus(FocusInfo *out) const
{
    out->focusable.clear();
    for (std::size_t i = 0; i < tracks_.size(); ++i)
        out->focusable.emplace_back("track-" + std::to_string(i), tracks_[i].label);
    for (std::size_t i = 0; i < tracks_.size(); ++i)
    {
        if (!tracks_[i].focused)
            continue;
        out->id = "track-" + std::to_string(i);
        out->text = tracks_[i].label + (tracks_[i].current ? " (current)" : "");
        out->index = static_cast<int>(i) + 1;
        out->total = static_cast<int>(tracks_.size());
        out->rect = track_rect(static_cast<int>(i));
        return true;
    }
    out->focusable.clear();
    return false;
}

/* ------------------------------------------------------------ media info */

void MediaInfoCard::set(const evo_rmlui_mediainfo_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    title_ = str(p.title);
    path_ = str(p.path);
    badges_.clear();
    for (const char *b : {p.res_badge, p.hdr_badge, p.codec_badge, p.fps_badge})
        if (b && *b)
            badges_.push_back(b);
    sections_.clear();
    sections_.push_back({"FORMAT",
                         {{"Container", str(p.container)}, {"File size", str(p.file_size)},
                          {"Duration", str(p.duration)}}});
    sections_.push_back({"VIDEO",
                         {{"Codec", str(p.video_codec)}, {"Resolution", str(p.resolution)},
                          {"Colour / HDR", str(p.color_hdr)}}});
    sections_.push_back({"AUDIO",
                         {{"Codec", str(p.audio_codec)}, {"Channels", str(p.channels)},
                          {"Sample rate", str(p.sample_rate)}}});
    sections_.push_back({"PLAYBACK",
                         {{"Subtitles", str(p.subtitles)}, {"Output", str(p.output)},
                          {"Renderer", str(p.renderer)}, {"Decoder", str(p.decoder)},
                          {"Upscaler", str(p.upscaler)}, {"Motion smoothing", str(p.motion_smoothing)}}});
}

void MediaInfoCard::enter()
{
    open_.snap(0.0f);
}

void MediaInfoCard::update(float dt)
{
    open_.target = 1.0f;
    open_.update(dt, 16.0f);
}

void MediaInfoCard::draw(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const float o = open_.value;
    list.rounded_rect({0, 0, 1920, 1080}, 0, kBlack.with_alpha(0.62f * o));
    list.push_transform(0.96f + 0.04f * o, 960, 540, 0, 0);
    list.push_opacity(o);
    /* The card is as tall as its longest column, so no column sits half empty. */
    std::size_t rows = 1;
    for (const auto &s : sections_)
        rows = std::max(rows, s.second.size());
    float pitch = 72.0f;
    const float max_h = 900.0f;
    if (280.0f + 92.0f + static_cast<float>(rows) * pitch + 48.0f > max_h)
        pitch = std::max(56.0f, (max_h - 280.0f - 92.0f - 48.0f) / static_cast<float>(rows));
    const float col_h = 92.0f + static_cast<float>(rows) * pitch;
    const float card_h = 280.0f + col_h + 48.0f;
    const Rect card{160, std::max(60.0f, (1080.0f - card_h) * 0.5f - 20.0f), 1600, card_h};
    list.shadow({card.x, card.y + 30, card.w, card.h}, page::kRadiusCard, 60, kBlack.with_alpha(0.6f));
    list.rounded_rect(card, page::kRadiusCard, p.bg_top.with_alpha(0.96f));
    list.bordered_rect(card, page::kRadiusCard, kClear, 1.5f, kWhite.with_alpha(0.12f));

    ui::text(list, fonts.display, fit(fonts.display, title_, 44, card.w - 112), card.x + 54, card.y + 96, 44, kWhite);
    ui::text(list, fonts.semibold, "MEDIA INFO", card.x + 56, card.y + 134, 18, p.text_muted, Align::left, 3.0f);
    ui::text(list, fonts.regular, fit(fonts.regular, path_, 20, card.w - 112), card.x + 56, card.y + 172, 20,
             p.text_muted);
    float bx = card.x + 56;
    for (const std::string &b : badges_)
    {
        const float w = fonts.semibold.measure(b, 18, 1.5f) + 28;
        list.bordered_rect({bx, card.y + 196, w, 38}, 10, kClear, 1.5f, kWhite.with_alpha(0.45f));
        ui::text(list, fonts.semibold, b, bx + w * 0.5f, card.y + 222, 18, kWhite, Align::center, 1.5f);
        bx += w + 10;
    }

    /* Four columns of label / value. */
    const float colw = (card.w - 112 - 3 * 32) / 4.0f;
    for (std::size_t c = 0; c < sections_.size(); ++c)
    {
        const float x = card.x + 56 + static_cast<float>(c) * (colw + 32);
        const float top = card.y + 280;
        list.rounded_rect({x, top, colw, col_h}, page::kRadiusTile, kWhite.with_alpha(0.04f));
        ui::text(list, fonts.semibold, sections_[c].first, x + 24, top + 46, 16, p.accent, Align::left, 3.0f);
        float y = top + 92;
        for (const auto &row : sections_[c].second)
        {
            ui::text(list, fonts.regular, row.first, x + 24, y, 18, p.text_muted);
            ui::text(list, fonts.semibold, fit(fonts.semibold, row.second.empty() ? "-" : row.second, 22, colw - 48),
                     x + 24, y + 30, 22, kWhite);
            y += pitch;
        }
    }
    const ui::Hint hints[] = {{ui::Button::circle, "Back"}};
    draw_page_hints(list, fonts, hints, 1);
    list.pop_opacity();
    list.pop_transform();
}

bool MediaInfoCard::focus(FocusInfo *out) const
{
    out->focusable.clear();
    out->id = "mediainfo";
    out->text = title_;
    out->index = out->total = 1;
    out->rect = {160, 110, 1600, 820};
    return true;
}

} // namespace evo::kit
