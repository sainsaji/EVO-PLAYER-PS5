/*
 * evo_hui_osd.cpp - the player OSD on ps5-homebrew-ui. See the header.
 */
#include "evo_hui_osd.hpp"
#include "evo_hui_cover.hpp"

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
const Color kCyan = Color::rgb(0x4dd8ff);
const Color kAmber = Color::rgb(0xffb547);
const Color kGreen = Color::rgb(0x5fe39a);
const Color kSub2[3] = {Color::rgb(0xffe14d), Color::rgb(0x66e3ff), Color::rgb(0xffffff)};
/* #231: the primary caption's colour, PROSPERO_SUBTITLE_COLORS order. */
const Color kSub1[4] = {Color::rgb(0xffffff), Color::rgb(0xffe14d), Color::rgb(0x66e3ff), Color::rgb(0x7dff8a)};

constexpr float kMargin = 96.0f;

std::string fit(const ui::FontRef &font, const std::string &text, float size, float width)
{
    return font.font->fit(text, size, width);
}

std::string clock_text(double seconds)
{
    if (!(seconds >= 0.0))
        seconds = 0.0;
    const long s = static_cast<long>(seconds);
    char b[32];
    if (s >= 3600)
        std::snprintf(b, sizeof b, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    else
        std::snprintf(b, sizeof b, "%ld:%02ld", s / 60, s % 60);
    return b;
}

std::vector<std::string> split_lines(const char *text)
{
    std::vector<std::string> out;
    if (!text || !*text)
        return out;
    std::string cur;
    for (const char *c = text; *c; ++c)
    {
        if (*c == '\n')
        {
            out.push_back(cur);
            cur.clear();
        }
        else if (*c != '\r')
        {
            cur += *c;
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

/* Every code point of `s` in `font`? Bad UTF-8 counts as missing. */
bool font_covers(const ::hui::gfx::Font &font, const std::string &s)
{
    for (std::size_t i = 0; i < s.size();)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp;
        int n;
        if (c < 0x80)
        {
            cp = c;
            n = 1;
        }
        else if ((c & 0xe0) == 0xc0)
        {
            cp = c & 0x1f;
            n = 2;
        }
        else if ((c & 0xf0) == 0xe0)
        {
            cp = c & 0x0f;
            n = 3;
        }
        else if ((c & 0xf8) == 0xf0)
        {
            cp = c & 0x07;
            n = 4;
        }
        else
        {
            return false;
        }
        if (i + static_cast<std::size_t>(n) > s.size())
            return false;
        for (int k = 1; k < n; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]) & 0x3f);
        i += static_cast<std::size_t>(n);
        if (cp == ' ')
            continue;
        if (!font.has_glyph(cp))
            return false;
    }
    return true;
}

float caption_size(int face)
{
    switch (face)
    {
    case 1:
        return 40.0f;
    case 3:
        return 60.0f;
    case 4: /* #231: extra large */
        return 72.0f;
    default:
        return 50.0f;
    }
}

/* Settings -> DEFAULT FONT STYLE: 0 standard (Inter SemiBold), 1 light (Inter
 * Regular), 2 rounded (Montserrat), 3 bold (SemiBold drawn heavier, see
 * draw_captions - the kit bakes no bold face). */
const ui::FontRef &caption_font(const ui::Fonts &fonts, int style)
{
    switch (style)
    {
    case 1:
        return fonts.regular;
    case 2:
        return fonts.display;
    default:
        return fonts.semibold;
    }
}

} // namespace

void OsdScreen::set(const evo_playback_osd_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    raw_ = p;
    title_ = str(p.title);
    meta_ = str(p.metadata);
    badges_[0] = str(p.res_badge);
    badges_[1] = str(p.hdr_badge);
    badges_[2] = str(p.codec_badge);
    badges_[3] = str(p.fps_badge);
    badges_[4] = str(p.audio_badge);
    badges_[5] = str(p.decoder_badge);
    badges_[6] = str(p.upscale_badge);
    badges_[7] = str(p.smooth_badge);
    badges_[8] = str(p.deepblack_badge);
    audio_track_ = str(p.audio_track);
    sub_track_ = str(p.sub_track);
    music_codec_ = str(p.music_codec);
    chapter_title_ = str(p.chapter_title);
    scrub_chapter_title_ = str(p.scrub_chapter_title);
    ends_at_ = str(p.ends_at);
    remaining_ = str(p.remaining);
    chapter_starts_.clear();
    for (int i = 0; p.chapter_starts && i < p.chapter_count && i < 128; ++i)
        chapter_starts_.push_back(p.chapter_starts[i]);
    raw_.chapter_starts = nullptr;
    skip_on_ = p.skip_label && *p.skip_label;
    if (skip_on_)
        skip_label_ = p.skip_label;
    raw_.skip_label = nullptr;
    sub_lines_ = split_lines(p.subtitle_text);
    sub2_lines_ = split_lines(p.subtitle_text2);
    if (p.music_cover_gen != cover_gen_)
    {
        cover_gen_ = p.music_cover_gen;
        cover_.clear();
        cover_w_ = cover_h_ = 0;
        if (p.music_cover && p.music_cover_w > 0 && p.music_cover_h > 0)
        {
            cover_.assign(p.music_cover,
                          p.music_cover + static_cast<std::size_t>(p.music_cover_w) * p.music_cover_h);
            cover_w_ = p.music_cover_w;
            cover_h_ = p.music_cover_h;
        }
    }
    raw_.music_cover = nullptr;
    raw_.title = raw_.metadata = nullptr; /* never read the copied pointers */
    hud_on_ = p.show_stats != 0;
}

void OsdScreen::set_hud(const evo_perf_hud_t &h)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    hud_lines_[0] = str(h.line_video);
    hud_lines_[1] = str(h.line_audio);
    hud_lines_[2] = str(h.line_subs);
    hud_lines_[3] = str(h.line_source);
    hud_lines_[4] = str(h.line_perf);
    hud_lines_[5] = str(h.line_pipeline);
    hud_lines_[6] = str(h.line_queues);
    hud_lines_[7] = str(h.line_clocks);
    auto hist = [&](const float *src, std::vector<float> &dst) {
        dst.clear();
        for (int i = 0; src && i < h.hist_len; ++i)
            dst.push_back(src[i]);
    };
    hist(h.gpu_hist, gpu_hist_);
    hist(h.ram_hist, ram_hist_);
    hist(h.cpu_hist, cpu_hist_);
    gpu_pct_ = h.gpu_pct;
    ram_mb_ = h.ram_mb;
    ram_total_ = h.ram_total_mb;
    cpu_pct_ = h.cpu_pct;
}

void OsdScreen::enter()
{
    progress_snapped_ = false;
    chrome_.snap(0.0f);
}

void OsdScreen::update(float dt)
{
    time_ += dt;
    /* The caller already fades `alpha`; the spring only smooths its steps. */
    chrome_.target = raw_.chrome_hidden ? 0.0f : static_cast<float>(raw_.alpha) / 255.0f;
    chrome_.update(dt, 20.0f);
    paused_.target = (raw_.paused && !raw_.scrub_active && !raw_.loading) ? 1.0f : 0.0f;
    paused_.update(dt, 16.0f);
    loading_.target = raw_.loading ? 1.0f : 0.0f;
    loading_.update(dt, 14.0f);
    scrub_.target = raw_.scrub_active ? 1.0f : 0.0f;
    scrub_.update(dt, 18.0f);
    skip_.target = skip_on_ ? 1.0f : 0.0f;
    skip_.update(dt, 14.0f);
    const float pct = static_cast<float>(std::clamp(raw_.percentage > 1.0 ? raw_.percentage / 100.0
                                                                           : raw_.percentage,
                                                    0.0, 1.0));
    if (!progress_snapped_)
    {
        progress_.snap(pct);
        progress_snapped_ = true;
    }
    progress_.target = pct;
    progress_.update(dt, 14.0f);

    /* The kit's ProgressBar draws the track and fill; the spring above stays
     * the one source of motion so the thumb and the fill never disagree. */
    bar_.style.height = 8.0f;
    bar_.style.radius_source = ui::RadiusSource::pill;
    bar_.style.track = ui::TrackStyle::flat;
    bar_.style.placement = ui::LabelPlacement::none;
    bar_.style.percent = false;
    bar_.style.finish_flash = false;
    bar_.set_bounds({kMargin, 920, 1920 - 2 * kMargin, 8});
    bar_.set_value(std::max(progress_.value, 8.0f / (1920 - 2 * kMargin)), true);
    bar_.update(dt);
}

bool OsdScreen::can_draw(const ui::Fonts &fonts) const
{
    const ui::FontRef &font = caption_font(fonts, raw_.subtitle_style);
    for (const std::string &l : sub_lines_)
        if (!font_covers(*font.font, l))
            return false;
    for (const std::string &l : sub2_lines_)
        if (!font_covers(*font.font, l))
            return false;
    /* The title is not checked: a missing glyph there costs one blank, and
     * dropping the whole kit OSD for it would cost far more. */
    return true;
}

void OsdScreen::draw_top(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    list.gradient_rect({0, 0, 1920, 260}, 0, kBlack.with_alpha(0.75f), kBlack.with_alpha(0.0f));
    ui::text(list, fonts.display, fit(fonts.display, title_, 44, 1300), kMargin, 110, 44, kWhite);
    if (!meta_.empty())
        ui::text(list, fonts.regular, fit(fonts.regular, meta_, 22, 1300), kMargin, 148, 22,
                 kWhite.with_alpha(0.7f));

    /* Badges, right-aligned on the title line, as many as clear the title. */
    const float limit = kMargin + std::min(1300.0f, fonts.display.measure(title_, 44)) + 48.0f;
    float x = 1920.0f - kMargin;
    for (int i = 8; i >= 0; --i)
    {
        const std::string &b = badges_[i];
        if (b.empty())
            continue;
        const bool accent = (i == 6 && raw_.upscale_active) || (i == 7 && raw_.smooth_active) || i == 8 || i == 1;
        const float w = fonts.semibold.measure(b, 16, 1.5f) + 24.0f;
        if (x - w < limit)
            break;
        const Rect r{x - w, 82, w, 32};
        if (accent)
            list.rounded_rect(r, 8, p.accent);
        else
            list.bordered_rect(r, 8, kBlack.with_alpha(0.35f), 1.5f, kWhite.with_alpha(0.45f));
        ui::text(list, fonts.semibold, b, r.cx(), r.y + 22, 16, accent ? kInk : kWhite, Align::center, 1.5f);
        x -= w + 10.0f;
    }
}

void OsdScreen::draw_bottom(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    list.gradient_rect({0, 760, 1920, 320}, 0, kBlack.with_alpha(0.0f), kBlack.with_alpha(0.82f));

    const Rect track{kMargin, 920, 1920 - 2 * kMargin, 8};
    if (raw_.is_live)
    {
        const Rect pill{kMargin, 900, 96, 36};
        list.rounded_rect(pill, 18, Color::rgb(0xe5484d));
        list.circle(pill.x + 22, pill.cy(), 5, kWhite.with_alpha(0.6f + 0.4f * ui::breathe(time_, 1.6f)));
        ui::text(list, fonts.semibold, "LIVE", pill.x + 36, pill.y + 25, 18, kWhite, Align::left, 2.0f);
    }
    else
    {
        const float f = progress_.value;
        bar_.style.color = p.accent;
        bar_.style.theme.surface_high = kWhite.with_alpha(0.22f);
        ui::Canvas canvas{list, fonts, 0, time_};
        bar_.draw(canvas);
        const float tx = track.x + track.w * f;
        list.glow({tx - 9, track.cy() - 9, 18, 18}, 9, 10, p.accent.with_alpha(0.5f));
        list.circle(tx, track.cy(), 9, kWhite);
        if (scrub_.value > 0.01f && raw_.duration_sec > 0.0)
        {
            /* Where the seek will land, ahead of (or behind) the playhead. */
            const float t = static_cast<float>(std::clamp(raw_.scrub_target / raw_.duration_sec, 0.0, 1.0));
            const float sx = track.x + track.w * t;
            list.push_opacity(scrub_.value);
            list.rounded_rect({sx - 2, track.y - 14, 4, track.h + 28}, 2, kWhite);
            list.pop_opacity();
        }
        /* #150: a tick at each chapter start, skipping any closer than 6 px. */
        if (raw_.duration_sec > 0.0)
        {
            float last = -100.0f;
            for (const double cs : chapter_starts_)
            {
                if (cs <= 1.0 || cs >= raw_.duration_sec - 1.0)
                    continue;
                const float cx = track.x + track.w * static_cast<float>(cs / raw_.duration_sec);
                if (cx - last < 6.0f)
                    continue;
                last = cx;
                list.rounded_rect({cx - 1.5f, track.y - 5, 3, track.h + 10}, 1.5f, kWhite.with_alpha(0.85f));
            }
        }
        const std::string cur = clock_text(raw_.position_sec);
        ui::text(list, fonts.mono, cur, track.x, 970, 24, kWhite);
        /* The chapter name beside the time; under the cursor while scrubbing. */
        const std::string &ch = (scrub_.value > 0.5f && !scrub_chapter_title_.empty()) ? scrub_chapter_title_
                                                                                     : chapter_title_;
        if (!ch.empty())
            ui::text(list, fonts.semibold, fit(fonts.semibold, ch, 20, 800),
                     track.x + fonts.mono.measure(cur, 24) + 28, 968, 20, p.accent);
        ui::text(list, fonts.mono, clock_text(raw_.duration_sec), track.x + track.w, 970, 24,
                 kWhite.with_alpha(0.7f), Align::right);
        /* #151: time left and the clock time it ends, centred. */
        if (!remaining_.empty())
        {
            const std::string mid = ends_at_.empty() ? remaining_ : remaining_ + "   -   ends at " + ends_at_;
            ui::text(list, fonts.regular, mid, 960, 968, 20, kWhite.with_alpha(0.7f), Align::center);
        }
    }

    /* Controls, one row of hints. */
    char aspect[32];
    std::snprintf(aspect, sizeof aspect, "Aspect: %s",
                  raw_.view_mode == 0 ? "Fit" : (raw_.view_mode == 1 ? "Fill" : "Stretch"));
    const std::string audio = "Audio: " + (audio_track_.empty() ? std::string("Default") : audio_track_);
    const std::string subs = "Subs: " + (sub_track_.empty() ? std::string("Off") : sub_track_);
    ui::Hint hints[10];
    int n = 0;
    hints[n++] = {ui::Button::cross, raw_.paused ? "Play" : "Pause"};
    if (!raw_.is_live)
        hints[n++] = {ui::Button::dpad, "Seek 10s"};
    if (!raw_.is_live && raw_.chapter_count > 0)
    {
        hints[n++] = {ui::Button::l1, "Prev chapter"};
        hints[n++] = {ui::Button::r1, "Next chapter"};
        hints[n++] = {ui::Button::right_stick, "Chapter list"};
    }
    hints[n++] = {ui::Button::r2, audio.c_str()};
    hints[n++] = {ui::Button::square, "Info"};
    hints[n++] = {ui::Button::triangle, aspect};
    hints[n++] = {ui::Button::circle, "Back"};
    ui::HintLayout layout;
    layout.size = 30;
    layout.text_size = 20;
    layout.cy = 1030;
    ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), hints, n, 1920 - kMargin, true, layout);
    ui::text(list, fonts.regular, fit(fonts.regular, subs, 20, 520), kMargin, 1037, 20, kWhite.with_alpha(0.75f));
    if (raw_.sub_delay_ms != 0)
    {
        char sync[40];
        std::snprintf(sync, sizeof sync, "Sync %+d ms", raw_.sub_delay_ms);
        ui::text(list, fonts.mono, sync, kMargin + 540, 1037, 18, p.accent);
    }
}

void OsdScreen::draw_centre(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    if (loading_.value > 0.01f)
    {
        list.push_opacity(loading_.value);
        list.circle(960, 520, 64, kBlack.with_alpha(0.55f));
        list.arc(960, 520, 44, 6, 0.0f, 6.2831853f, kWhite.with_alpha(0.15f));
        list.arc(960, 520, 44, 6, std::fmod(time_ * 5.5f, 6.2831853f), 1.6f, p.accent);
        ui::text(list, fonts.semibold, "LOADING", 960, 630, 18, kWhite.with_alpha(0.85f), Align::center, 3.0f);
        list.pop_opacity();
    }
    if (paused_.value > 0.01f)
    {
        const float s = 0.85f + 0.15f * paused_.value;
        list.push_transform(s, 960, 520, 0, 0);
        list.push_opacity(paused_.value);
        list.circle(960, 520, 70, kBlack.with_alpha(0.55f));
        list.rounded_rect({960 - 22, 520 - 28, 14, 56}, 4, kWhite);
        list.rounded_rect({960 + 8, 520 - 28, 14, 56}, 4, kWhite);
        ui::text(list, fonts.semibold, "PAUSED", 960, 636, 18, kWhite.with_alpha(0.85f), Align::center, 3.0f);
        list.pop_opacity();
        list.pop_transform();
    }
    if (scrub_.value > 0.01f)
    {
        list.push_opacity(scrub_.value);
        const Rect cap{960 - 170, 470, 340, scrub_chapter_title_.empty() ? 110.0f : 150.0f};
        /* The frame at the cursor, above the time card. The tag is the worker's
         * serial: each new frame is a new texture. */
        if (raw_.scrub_thumb && raw_.scrub_thumb_w > 0 && raw_.scrub_thumb_h > 0)
        {
            const std::uint32_t tex = ctx.textures.pixels(raw_.scrub_thumb, raw_.scrub_thumb_w, raw_.scrub_thumb_h,
                                                          0x5300000000ull ^ raw_.scrub_thumb_serial);
            if (tex)
            {
                const Rect frame{960 - 160.0f, cap.y - 196.0f, 320, 180};
                list.rounded_rect(frame.inset(-4), 14, kBlack.with_alpha(0.7f));
                list.image(tex, frame, ::hui::gfx::kFullUv, kWhite);
            }
        }
        list.rounded_rect(cap, page::kRadiusCard, kBlack.with_alpha(0.6f));
        ui::text(list, fonts.semibold, "SEEK TO", 960, cap.y + 38, 16, p.accent, Align::center, 3.0f);
        ui::text(list, fonts.mono, clock_text(raw_.scrub_target), 960, cap.y + 86, 40, kWhite, Align::center);
        if (!scrub_chapter_title_.empty())
            ui::text(list, fonts.regular, fit(fonts.regular, scrub_chapter_title_, 18, 320), 960, cap.y + 134, 18,
                     kWhite.with_alpha(0.8f), Align::center);
        list.pop_opacity();
    }
}

void OsdScreen::draw_captions(DrawList &list, const Context &ctx, float chrome) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const float size = caption_size(raw_.subtitle_face);
    const float line_h = size * 1.25f;
    /* #231: Settings -> SUBTITLE POSITION raises the text captions; the chrome
     * lift still applies on top, minus what the setting already gave. */
    static const float kLift[3] = {0.0f, 80.0f, 160.0f};
    const float lift = kLift[std::clamp(raw_.subtitle_lift, 0, 2)];
    /* Lifted above the timeline while the chrome is up. */
    const float bottom =
        1000.0f - lift - (raw_.subtitle_raised ? std::max(0.0f, 150.0f - lift) * chrome : 0.0f);

    const ui::FontRef &font = caption_font(fonts, raw_.subtitle_style);
    const bool heavy = raw_.subtitle_style == 3;
    auto block = [&](const std::vector<std::string> &lines, float last_baseline, float sz, Color ink, int background_mode) {
        const int bg = std::clamp(background_mode, 0, 3); /* 0 box, 1 shadow, 2 outline, 3 none */
        float y = last_baseline - static_cast<float>(lines.size() - 1) * (sz * 1.25f);
        for (const std::string &l : lines)
        {
            const float w = font.measure(l, sz);
            if (bg == 0)
                list.rounded_rect({960 - w * 0.5f - 18, y - sz * 0.95f, w + 36, sz * 1.25f}, 10,
                                  kBlack.with_alpha(0.55f));
            else if (bg == 1) /* drop shadow, scaled with the text */
            {
                const float o = std::max(2.0f, sz * 0.05f);
                ui::text(list, font, l, 960 + o, y + o, sz, kBlack.with_alpha(0.85f), Align::center);
            }
            else if (bg == 2) /* outline: the line in black, offset all round */
            {
                const float o = std::max(2.0f, sz * 0.045f);
                static const float kDir[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1},
                                                 {-0.7f, -0.7f}, {0.7f, -0.7f}, {-0.7f, 0.7f}, {0.7f, 0.7f}};
                for (const auto &d : kDir)
                    ui::text(list, font, l, 960 + d[0] * o, y + d[1] * o, sz, kBlack.with_alpha(0.9f),
                             Align::center);
            }
            if (heavy) /* thicken the strokes: the same line 1 px either side */
            {
                ui::text(list, font, l, 959, y, sz, ink, Align::center);
                ui::text(list, font, l, 961, y, sz, ink, Align::center);
            }
            ui::text(list, font, l, 960, y, sz, ink, Align::center);
            y += sz * 1.25f;
        }
        return last_baseline - static_cast<float>(lines.size()) * (sz * 1.25f);
    };

    float top = bottom;
    if (raw_.subtitle_image && raw_.subtitle_image_w > 0 && raw_.subtitle_image_h > 0)
    {
        const std::uint32_t tex =
            ctx.textures.pixels(raw_.subtitle_image, raw_.subtitle_image_w, raw_.subtitle_image_h, 0x5047u);
        if (tex)
        {
            const float sx = 1920.0f / static_cast<float>(std::max(raw_.subtitle_image_canvas_w, 1));
            const float sy = 1080.0f / static_cast<float>(std::max(raw_.subtitle_image_canvas_h, 1));
            Rect r{static_cast<float>(raw_.subtitle_image_x) * sx, static_cast<float>(raw_.subtitle_image_y) * sy,
                   static_cast<float>(raw_.subtitle_image_w) * sx, static_cast<float>(raw_.subtitle_image_h) * sy};
            /* Lifted clear of the timeline while the chrome is up. */
            if (raw_.subtitle_raised && r.y + r.h > 850.0f)
                r.y -= 150.0f * chrome;
            list.image(tex, r, ::hui::gfx::kFullUv, kWhite);
            top = r.y;
        }
    }
    if (!sub_lines_.empty())
        top = block(sub_lines_, bottom, size, kSub1[std::clamp(raw_.subtitle_color, 0, 3)], raw_.subtitle_background);
    if (!sub2_lines_.empty())
    {
        const Color ink = kSub2[std::clamp(raw_.subtitle2_color, 0, 2)];
        const float sz2_base = caption_size(raw_.subtitle2_face > 0 ? raw_.subtitle2_face : raw_.subtitle_face);
        const float sz2 = sz2_base * 0.85f;
        if (raw_.subtitle2_position == 1)
            block(sub2_lines_, 120.0f + static_cast<float>(sub2_lines_.size()) * sz2 * 1.25f, sz2, ink, raw_.subtitle2_background);
        else
            block(sub2_lines_, top - 12.0f, sz2, ink, raw_.subtitle2_background);
    }
    (void)line_h;
}

/* #219: "[triangle] Skip intro", bottom right above the timeline. Drawn on its
 * own opacity so it shows with the controls hidden too. */
void OsdScreen::draw_skip(DrawList &list, const Context &ctx) const
{
    if (skip_.value <= 0.01f || skip_label_.empty())
        return;
    const ui::Fonts &fonts = ctx.fonts;
    ui::Hint hint{ui::Button::triangle, skip_label_.c_str()};
    ui::HintLayout layout;
    layout.size = 36;
    layout.text_size = 26;
    layout.font = &fonts.semibold;
    const float hw = ui::measure_hints(fonts, &hint, 1, layout);
    const float w = hw + 56.0f, h = 64.0f;
    const float s = 0.92f + 0.08f * skip_.value;
    const Rect r{1920 - kMargin - w, 800, w, h};
    layout.cy = r.cy();
    list.push_transform(s, r.x + r.w, r.cy(), 0, 0);
    list.push_opacity(skip_.value);
    list.shadow({r.x, r.y + 8, r.w, r.h}, h * 0.5f, 24, kBlack.with_alpha(0.5f));
    list.rounded_rect(r, h * 0.5f, kBlack.with_alpha(0.72f));
    list.bordered_rect(r, h * 0.5f, kClear, 2.0f, kWhite.with_alpha(0.75f));
    ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), &hint, 1, r.x + r.w - 28.0f, true, layout);
    list.pop_opacity();
    list.pop_transform();
}

void OsdScreen::draw_music(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    draw_backdrop(list, ctx);
    const Rect card{960 - 520, 300, 1040, 380};
    list.shadow({card.x, card.y + 30, card.w, card.h}, page::kRadiusCard, 60, kBlack.with_alpha(0.6f));
    list.rounded_rect(card, page::kRadiusCard, kWhite.with_alpha(0.06f));
    list.bordered_rect(card, page::kRadiusCard, kClear, 1.5f, kWhite.with_alpha(0.12f));
    const Rect art{card.x + 50, card.y + 50, 280, 280};
    const std::uint32_t cover =
        cover_.empty() ? 0 : ctx.textures.pixels(cover_.data(), cover_w_, cover_h_, cover_gen_);
    if (cover)
    {
        list.image(cover, art, cover_uv(cover_w_, cover_h_, art.w, art.h), kWhite, page::kRadiusTile);
    }
    else
    {
        list.gradient_rect(art, page::kRadiusTile, p.accent, p.accent_alt);
        const std::uint32_t icon = ctx.textures.icon("../icons/icon_volume.png");
        if (icon)
            list.image(icon, art.inset(90), ::hui::gfx::kFullUv, kWhite);
    }
    const float x = art.x + art.w + 50;
    ui::text(list, fonts.semibold, raw_.paused ? "PAUSED" : "NOW PLAYING", x, card.y + 92, 18, p.accent,
             Align::left, 3.0f);
    ui::text(list, fonts.display, fit(fonts.display, title_, 44, card.x + card.w - 50 - x), x, card.y + 150, 44,
             kWhite);
    ui::text(list, fonts.regular, music_codec_.empty() ? "AUDIO" : music_codec_, x, card.y + 190, 22,
             p.text_muted);
    /* A visualiser that breathes with time (no spectrum is fed to the UI). */
    for (int i = 0; i < 24; ++i)
    {
        const float ph = time_ * (raw_.paused ? 0.0f : 3.0f) + static_cast<float>(i) * 0.7f;
        const float h = 12.0f + 70.0f * (0.5f + 0.5f * std::sin(ph) * std::sin(ph * 0.37f + 1.3f));
        list.rounded_rect({x + static_cast<float>(i) * 22.0f, card.y + 320 - h, 12, h}, 6,
                          p.accent.with_alpha(0.85f));
    }
}

void OsdScreen::draw_stats(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Rect panel{kMargin, 200, 760, 560};
    list.rounded_rect(panel, page::kRadiusCard, kBlack.with_alpha(0.72f));
    list.bordered_rect(panel, page::kRadiusCard, kClear, 1.0f, kWhite.with_alpha(0.12f));
    ui::text(list, fonts.semibold, "PLAYBACK DIAGNOSTICS", panel.x + 28, panel.y + 44, 16, ctx.palette.accent,
             Align::left, 3.0f);
    float y = panel.y + 84;
    for (const std::string &l : hud_lines_)
    {
        if (l.empty())
            continue;
        ui::text(list, fonts.mono, fit(fonts.mono, l, 17, panel.w - 56), panel.x + 28, y, 17, kWhite.with_alpha(0.9f));
        y += 30;
    }
    struct Graph
    {
        const char *label;
        const std::vector<float> *hist;
        Color color;
        char value[32];
    };
    Graph graphs[3] = {{"GPU", &gpu_hist_, kCyan, {}}, {"RAM", &ram_hist_, kAmber, {}}, {"CPU", &cpu_hist_, kGreen, {}}};
    std::snprintf(graphs[0].value, sizeof graphs[0].value, "%.0f%%", static_cast<double>(gpu_pct_));
    std::snprintf(graphs[1].value, sizeof graphs[1].value, "%.0f MB", static_cast<double>(ram_mb_));
    std::snprintf(graphs[2].value, sizeof graphs[2].value, "%.0f%%", static_cast<double>(cpu_pct_));
    float gx = panel.x + 28;
    const float gy = panel.y + panel.h - 130;
    for (const Graph &g : graphs)
    {
        const Rect box{gx, gy, 220, 100};
        list.rounded_rect(box, 12, kWhite.with_alpha(0.05f));
        ui::text(list, fonts.semibold, g.label, box.x + 12, box.y + 24, 14, g.color, Align::left, 2.0f);
        ui::text(list, fonts.mono, g.value, box.x + box.w - 12, box.y + 24, 14, kWhite, Align::right);
        const std::vector<float> &h = *g.hist;
        if (h.size() > 1)
        {
            const float step = (box.w - 24) / static_cast<float>(h.size() - 1);
            for (std::size_t i = 1; i < h.size(); ++i)
            {
                const float x0 = box.x + 12 + step * static_cast<float>(i - 1);
                const float y0 = box.y + box.h - 10 - 56 * std::clamp(h[i - 1], 0.0f, 1.0f);
                const float x1 = box.x + 12 + step * static_cast<float>(i);
                const float y1 = box.y + box.h - 10 - 56 * std::clamp(h[i], 0.0f, 1.0f);
                list.line(x0, y0, x1, y1, 2.5f, g.color);
            }
        }
        gx += 236;
    }
}

void OsdScreen::draw(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    if (raw_.music_mode)
        draw_music(list, ctx);
    const float chrome = chrome_.value;
    if (chrome > 0.01f)
    {
        list.push_opacity(chrome);
        if (!raw_.music_mode)
            draw_top(list, ctx);
        draw_bottom(list, ctx);
        list.pop_opacity();
    }
    if (!raw_.music_mode)
    {
        draw_centre(list, ctx);
        draw_captions(list, ctx, chrome);
        draw_skip(list, ctx);
    }
    if (hud_on_)
        draw_stats(list, ctx);
    if (raw_.debug_overlay)
    {
        char fps[24];
        std::snprintf(fps, sizeof fps, "%d FPS", raw_.fps);
        const Rect chip{1920 - 180, 40, 140, 40};
        list.rounded_rect(chip, 20, kBlack.with_alpha(0.6f));
        ui::text(list, fonts.mono, fps, chip.cx(), chip.y + 28, 20, ctx.palette.accent, Align::center);
    }
}

bool OsdScreen::focus(FocusInfo *out) const
{
    /* The player has no cursor; report the state the remote tests look for. */
    out->focusable.clear();
    out->id = raw_.scrub_active ? "osd-scrub" : (raw_.paused ? "osd-paused" : "osd-playing");
    out->text = title_;
    out->index = out->total = 1;
    out->rect = {0, 0, 1920, 1080};
    return chrome_.value > 0.01f || raw_.scrub_active || raw_.paused;
}

} // namespace evo::kit
