/*
 * evo_hui_modals.cpp - the player's pop-ups on ps5-homebrew-ui. See the header.
 */
#include "evo_hui_modals.hpp"

#include "ui/glyphs.hpp"

#include <algorithm>
#include <cmath>

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
    layout.size = 30;
    layout.text_size = 20;
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
    sync_ = str(p.sync_str);
    sync2_ = str(p.sync2_str);
    preview_ = str(p.preview_text);
    preview_face_ = p.preview_face;
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
    list.shadow({sheet.x - 10, sheet.y + 20, sheet.w, sheet.h}, 36, 60, kBlack.with_alpha(0.6f));
    list.rounded_rect(sheet, 36, p.bg_top.with_alpha(0.96f));
    list.bordered_rect(sheet, 36, kClear, 1.5f, kWhite.with_alpha(0.12f));

    ui::text(list, fonts.semibold, ui::upper(eyebrow_.empty() ? "Subtitles" : eyebrow_), sheet.x + 36,
             sheet.y + 70, 16, p.accent, Align::left, 3.0f);
    ui::text(list, fonts.display, fit(fonts.display, title_.empty() ? "Subtitle track" : title_, 36, sheet.w - 72),
             sheet.x + 34, sheet.y + 118, 36, kWhite);
    /* Size and sync, as pills under the title. */
    float px = sheet.x + 36;
    auto pill = [&](const std::string &label, const std::string &value) {
        if (value.empty())
            return;
        const std::string t = label + "  " + value;
        const float w = fonts.semibold.measure(t, 16, 1.0f) + 28;
        list.rounded_rect({px, sheet.y + 140, w, 34}, 17, kWhite.with_alpha(0.08f));
        ui::text(list, fonts.semibold, t, px + w * 0.5f, sheet.y + 163, 16, p.text_muted, Align::center, 1.0f);
        px += w + 10;
    };
    pill("SIZE", size_);
    pill("SYNC", sync_);
    pill("SYNC 2", sync2_);

    list.rounded_rect(ring_.value(), 20, kWhite);
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
                              {ui::Button::square, "Size"},
                              {ui::Button::dpad, "Sync"},
                              {ui::Button::circle, "Back"}};
    hint_row(list, fonts, hints, 4, sheet.x + sheet.w - 36, sheet.y + sheet.h - 44);
    list.pop_opacity();
    list.pop_transform();

    /* The preview caption, in the size that is selected, where it will show. */
    if (!preview_.empty() && o > 0.01f)
    {
        const float size = preview_face_ == 0 ? 40.0f : (preview_face_ == 2 ? 60.0f : 50.0f);
        const float cx = kSheetX * 0.5f;
        list.push_opacity(o);
        ui::text(list, fonts.semibold, "PREVIEW", cx, 700, 14, p.text_faint, Align::center, 3.0f);
        const float w = std::min(fonts.semibold.measure(preview_, size), kSheetX - 120);
        list.rounded_rect({cx - w * 0.5f - 18, 800 - size * 0.95f, w + 36, size * 1.25f}, 10, kBlack.with_alpha(0.55f));
        ui::text(list, fonts.semibold, fit(fonts.semibold, preview_, size, kSheetX - 120), cx, 800, size, kWhite,
                 Align::center);
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
    const Rect card{160, 110, 1600, 820};
    list.shadow({card.x, card.y + 30, card.w, card.h}, 40, 60, kBlack.with_alpha(0.6f));
    list.rounded_rect(card, 40, p.bg_top.with_alpha(0.96f));
    list.bordered_rect(card, 40, kClear, 1.5f, kWhite.with_alpha(0.12f));

    ui::text(list, fonts.semibold, "MEDIA INFO", card.x + 56, card.y + 76, 16, p.accent, Align::left, 3.0f);
    ui::text(list, fonts.display, fit(fonts.display, title_, 44, card.w - 112), card.x + 54, card.y + 132, 44, kWhite);
    ui::text(list, fonts.regular, fit(fonts.regular, path_, 20, card.w - 112), card.x + 56, card.y + 170, 20,
             p.text_faint);
    float bx = card.x + 56;
    for (const std::string &b : badges_)
    {
        const float w = fonts.semibold.measure(b, 16, 1.5f) + 26;
        list.bordered_rect({bx, card.y + 196, w, 34}, 9, kClear, 1.5f, kWhite.with_alpha(0.45f));
        ui::text(list, fonts.semibold, b, bx + w * 0.5f, card.y + 219, 16, kWhite, Align::center, 1.5f);
        bx += w + 10;
    }

    /* Four columns of label / value. */
    const float colw = (card.w - 112 - 3 * 32) / 4.0f;
    for (std::size_t c = 0; c < sections_.size(); ++c)
    {
        const float x = card.x + 56 + static_cast<float>(c) * (colw + 32);
        const float top = card.y + 280;
        list.rounded_rect({x, top, colw, card.h - 320}, 24, kWhite.with_alpha(0.04f));
        ui::text(list, fonts.semibold, sections_[c].first, x + 24, top + 44, 15, p.accent, Align::left, 3.0f);
        float y = top + 88;
        for (const auto &row : sections_[c].second)
        {
            ui::text(list, fonts.regular, row.first, x + 24, y, 16, p.text_faint);
            ui::text(list, fonts.semibold, fit(fonts.semibold, row.second.empty() ? "-" : row.second, 21, colw - 48),
                     x + 24, y + 27, 21, kWhite);
            y += 66;
        }
    }
    const ui::Hint hints[] = {{ui::Button::circle, "Close"}};
    hint_row(list, fonts, hints, 1, card.x + card.w - 56);
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
