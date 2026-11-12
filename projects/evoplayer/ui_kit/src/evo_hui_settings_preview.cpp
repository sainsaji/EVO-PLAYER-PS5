/*
 * evo_hui_settings_preview.cpp - the Settings preview card (#158). See the header.
 *
 * Shared pieces first (card frame, header, tag, chips, mock still), then one
 * function per preview kind, then the dispatcher at the bottom. A kind with no
 * drawing yet falls back to the INFO card.
 */
#include "evo_hui_settings_preview.hpp"
#include "ui/glyphs.hpp"
#include "ui/widgets.hpp"

#include "evo_rmlui_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace evo::kit
{

using ::hui::gfx::Align;
using ::hui::gfx::Color;
using ::hui::gfx::DrawList;
using ::hui::gfx::Rect;
namespace ui = ::hui::ui;

namespace
{

const Color kWhite = Color::rgb(0xffffff);
const Color kInk = Color::rgb(0x0b0d16);

/* The card is 600 x ~640 (see kPreviewW in evo_hui_settings.cpp); content sits in a 24 px inset. */
struct Area
{
    Rect box;
    float x, w;  /* content column */
    float top;   /* first free y below the header */
};

Area begin_card(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    list.rounded_rect(box, 24, kWhite.with_alpha(0.05f));
    list.bordered_rect(box, 24, Color::rgb(0x000000, 0.0f), 1.0f, p.border);
    Area a{box, box.x + 24.0f, box.w - 48.0f, box.y + 112.0f};
    ui::text(list, fonts.semibold, "PREVIEW", a.x, box.y + 40, 16, p.text_muted, Align::left, 2.0f);
    /* A long title ("SECONDARY SUBTITLE POSITION") steps down instead of being cut. */
    float ts = 32.0f;
    while (ts > 22.0f && fonts.display.measure(pv.title, ts) > a.w)
        ts -= 1.0f;
    ui::text(list, fonts.display, fonts.display.font->fit(pv.title, ts, a.w), a.x, box.y + 84, ts, kWhite);
    return a;
}

/* A small pill over a frame: dark for the baseline frame, accent for the result. */
void tag(DrawList &list, const Context &ctx, const Rect &frame, const std::string &label, bool accent)
{
    const ui::Fonts &fonts = ctx.fonts;
    const float lw = fonts.semibold.measure(label, 16) + 24.0f;
    Rect t{frame.x + 10, frame.y + 10, lw, 28};
    list.rounded_rect(t, 14, accent ? ctx.palette.accent : Color::rgb(0x000000, 0.6f));
    ui::text(list, fonts.semibold, label, t.cx(), t.cy() + 6, 16, accent ? kInk : kWhite, Align::center);
}

/* A tag pill on the right side of a frame. */
void tag_right(DrawList &list, const Context &ctx, const Rect &frame, const std::string &label, bool accent)
{
    const ui::Fonts &fonts = ctx.fonts;
    const float lw = fonts.semibold.measure(label, 16) + 24.0f;
    Rect t{frame.x + frame.w - lw - 10.0f, frame.y + 10.0f, lw, 28.0f};
    list.rounded_rect(t, 14.0f, accent ? ctx.palette.accent : Color::rgb(0x000000, 0.6f));
    ui::text(list, fonts.semibold, label, t.cx(), t.cy() + 6.0f, 16, accent ? kInk : kWhite, Align::center);
}

/* Small photo credit line above the DETAILS block. */
void credit_line(DrawList &list, const Context &ctx, const Rect &box, const Area &a)
{
    ui::text(list, ctx.fonts.regular, "Photos: NASA (public domain)", a.x, box.y + box.h - 146.0f, 12, ctx.palette.text_faint);
}

/* A frame border: thin for the baseline, accent for the result. */
void frame_border(DrawList &list, const Context &ctx, const Rect &r, bool accent)
{
    list.bordered_rect(r, 12, Color::rgb(0x000000, 0.0f), accent ? 2.0f : 1.0f, accent ? ctx.palette.accent : ctx.palette.border);
}

/* Compare wipe on a 16:9 image: sweeps left to right and back with ctx.time (~4 s period). */
void draw_photo_wipe(DrawList &list, const Context &ctx, const Rect &rect,
                     std::uint32_t tex_left, std::uint32_t tex_right,
                     const std::string &left_label, const std::string &right_label,
                     bool is_active)
{
    list.rounded_rect(rect, 12.0f, kWhite.with_alpha(0.04f));

    if (!is_active || tex_right == 0 || tex_left == 0)
    {
        const std::uint32_t tex = (tex_left != 0) ? tex_left : tex_right;
        if (tex != 0)
            list.image(tex, rect, ::hui::gfx::kFullUv, kWhite, 12.0f);
        frame_border(list, ctx, rect, false);
        tag(list, ctx, rect, left_label, false);
        return;
    }

    const float sweep = 0.5f + 0.38f * std::sin((ctx.time + 0.5f) * (2.0f * 3.14159265f / 4.0f));
    const float split_w = std::clamp(std::round(rect.w * sweep), 1.0f, rect.w - 1.0f);
    const float div_x = rect.x + split_w;

    const Rect left_clip{rect.x, rect.y, split_w, rect.h};
    const Rect right_clip{div_x, rect.y, (rect.x + rect.w) - div_x, rect.h};

    list.push_clip(left_clip);
    list.image(tex_left, rect, ::hui::gfx::kFullUv, kWhite, 12.0f);
    list.pop_clip();

    list.push_clip(right_clip);
    list.image(tex_right, rect, ::hui::gfx::kFullUv, kWhite, 12.0f);
    list.pop_clip();

    list.line(div_x, rect.y + 1.0f, div_x, rect.y + rect.h - 1.0f, 2.0f, kWhite);
    frame_border(list, ctx, rect, true);

    tag(list, ctx, rect, left_label, false);
    tag_right(list, ctx, rect, right_label, true);
}

/* Choice chips, the highlighted one filled; wraps to a second row. Returns the y below them. */
float chips(DrawList &list, const Context &ctx, const Area &a, float y, const std::vector<std::string> &labels, int selected)
{
    const ui::Fonts &fonts = ctx.fonts;
    ui::Painter paint(list, fonts, ctx.palette.kit_theme());
    float cx = a.x;
    for (size_t i = 0; i < labels.size(); ++i)
    {
        const float cw = fonts.semibold.measure(labels[i], 18) + 36.0f;
        if (cx + cw > a.x + a.w && cx > a.x)
        {
            cx = a.x;
            y += 46.0f;
        }
        paint.chip({cx, y, cw, 36}, labels[i], static_cast<int>(i) == selected ? 1.0f : 0.0f, {});
        cx += cw + 10.0f;
    }
    return y + 36.0f;
}

/* Wrapped benefit text under the content, down to the card's foot. */
void benefit(DrawList &list, const Context &ctx, const Area &a, float y, const std::string &text)
{
    const float limit = a.box.y + a.box.h - 24.0f;
    const int lines = static_cast<int>((limit - y) / 28.0f) + 1;
    if (lines > 0)
        ui::paragraph(list, ctx.fonts.regular, text, a.x, y, 20, a.w, 28, ctx.palette.text_faint, std::min(lines, 12));
}

/* The labels the chips show: the setting's own choices, or OFF / ON for a toggle. */
std::vector<std::string> chip_labels(const SettingsPreview &pv)
{
    if (!pv.choices.empty())
    {
        std::vector<std::string> out;
        for (const std::string &c : pv.choices)
            out.push_back(ui::upper(c));
        return out;
    }
    return {"OFF", "ON"};
}

/* Mock still: sky, moon, three ridges, foreground. `dark` 0..1 pulls the lifted
 * blacks toward true black; `coarse` overlays a pixel grid for the low-res look. */
void mock_scene(DrawList &list, const Rect &r, float dark, bool coarse, bool hdr = false)
{
    auto mix = [&](uint32_t lifted, uint32_t deep) {
        Color a = Color::rgb(lifted), b = Color::rgb(deep);
        return Color{a.r + (b.r - a.r) * dark, a.g + (b.g - a.g) * dark, a.b + (b.b - a.b) * dark, 1.0f};
    };
    list.push_clip(r);
    if (hdr)
        list.gradient_rect(r, 0, Color::rgb(0x384a75), Color::rgb(0xe8995a));
    else
        list.gradient_rect(r, 0, mix(0x2c3550, 0x0a1226), mix(0xc9884f, 0xb8793f));
    const float mx = r.x + r.w * 0.72f, my = r.y + r.h * 0.30f, mr = r.h * 0.09f;
    if (hdr)
        list.glow({mx - mr, my - mr, mr * 2.0f, mr * 2.0f}, mr, 18.0f, Color::rgb(0xffffff, 0.85f));
    list.circle(mx, my, mr, hdr ? Color::rgb(0xffffff) : Color::rgb(0xf6e3b0));
    const uint32_t lift[3] = {0x4a4e5c, 0x3e424f, 0x353946}, deep[3] = {0x1b1722, 0x120f17, 0x09070c};
    const float cx[3] = {0.18f, 0.48f, 0.80f}, top[3] = {0.40f, 0.30f, 0.46f}, wd[3] = {0.6f, 0.5f, 0.55f};
    for (int i = 0; i < 3; ++i)
    {
        float xy[6] = {r.x + r.w * (cx[i] - wd[i] * 0.5f), r.y + r.h, r.x + r.w * cx[i], r.y + r.h * (0.30f + top[i] * 0.4f),
                       r.x + r.w * (cx[i] + wd[i] * 0.5f), r.y + r.h};
        list.polygon(xy, 3, mix(lift[i], deep[i]));
    }
    list.rounded_rect({r.x, r.y + r.h * 0.86f, r.w, r.h * 0.14f}, 0, mix(0x42434d, 0x050407));
    if (coarse)
        for (float gy = r.y; gy < r.y + r.h; gy += 9.0f)
            for (float gx = r.x + (static_cast<int>((gy - r.y) / 9.0f) & 1) * 9.0f; gx < r.x + r.w; gx += 18.0f)
                list.rounded_rect({gx, gy, 9.0f, 9.0f}, 0, Color::rgb(0x808080, 0.10f));
    list.pop_clip();
}

/* Two 16:9 frames side by side starting at a.top. Returns them and the y below. */
float frame_pair(const Area &a, Rect &left, Rect &right)
{
    const float sw = (a.w - 16.0f) * 0.5f, sh = sw * 9.0f / 16.0f;
    left = {a.x, a.top, sw, sh};
    right = {a.x + sw + 16.0f, a.top, sw, sh};
    return a.top + sh;
}

/* ---- INFO: icon and the benefit in plain words. Also the fallback. ---- */
void draw_info(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    Rect tile{a.x, a.top, 96, 96};
    list.rounded_rect(tile, 24, kWhite.with_alpha(0.08f));
    if (!pv.icon.empty())
        if (uint32_t icon = ctx.textures.icon(pv.icon))
            list.image(icon, {tile.x + 20, tile.y + 20, 56, 56}, ::hui::gfx::kFullUv, kWhite);
    float text_y = a.top + 148.0f;
    if (!pv.choices.empty() || pv.toggle)
        text_y = chips(list, ctx, a, a.top + 124.0f, chip_labels(pv), pv.index) + 28.0f;
    benefit(list, ctx, a, text_y, pv.benefit);
}

/* ---- DEEP BLACKS: OFF next to the highlighted choice. ---- */
void draw_deep_blacks(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    Rect l, r;
    const float bottom = frame_pair(a, l, r);
    const int lvl = std::min(std::max(pv.index, 0), 2);
    mock_scene(list, l, 0.0f, false);
    mock_scene(list, r, 0.5f * lvl, false);
    frame_border(list, ctx, l, false);
    frame_border(list, ctx, r, true);
    const auto labels = chip_labels(pv);
    tag(list, ctx, l, "OFF", false);
    tag(list, ctx, r, labels[std::min<size_t>(lvl, labels.size() - 1)], true);
    const float y = chips(list, ctx, a, bottom + 28.0f, labels, pv.index);
    benefit(list, ctx, a, y + 40.0f, pv.benefit);
}

/* ---- UPSCALING: 16:9 comparison wipe with real photos and three thumbnails. ---- */
void draw_upscaling(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);

    static const char *const kUpImages[3] = {
        "../icons/preview_up_off.png",
        "../icons/preview_up_lanczos.png",
        "../icons/preview_up_ai.png",
    };

    std::vector<std::string> labels = pv.choices;
    if (labels.empty())
        labels = {"OFF", "SHARP", "AI"};
    for (std::string &l : labels)
        l = ui::upper(l);

    const int sel = std::clamp(pv.index, 0, 2);
    const std::string choice_name = (sel < static_cast<int>(labels.size())) ? labels[sel] : (sel == 1 ? "SHARP" : "AI");
    const bool is_off = (sel == 0);

    const float img_w = a.w;
    const float img_h = std::round(img_w * 9.0f / 16.0f);
    const Rect img_rect{a.x, a.top, img_w, img_h};

    const std::uint32_t tex_off = ctx.textures.icon(kUpImages[0]);
    const std::uint32_t tex_sel = ctx.textures.icon(kUpImages[sel]);

    draw_photo_wipe(list, ctx, img_rect, tex_off, tex_sel, "OFF", choice_name, !is_off);

    const int count = 3;
    const float gap = 12.0f;
    const float thumb_w = std::floor((a.w - static_cast<float>(count - 1) * gap) / static_cast<float>(count));
    const float thumb_h = std::round(thumb_w * 9.0f / 16.0f);
    const float thumb_y = img_rect.y + img_rect.h + 12.0f;

    for (int i = 0; i < count; ++i)
    {
        const Rect thumb_r{a.x + static_cast<float>(i) * (thumb_w + gap), thumb_y, thumb_w, thumb_h};
        const bool sel_thumb = (i == sel);
        list.rounded_rect(thumb_r, 6.0f, kWhite.with_alpha(0.04f));
        if (const std::uint32_t t = ctx.textures.icon(kUpImages[i]))
            list.image(t, thumb_r, ::hui::gfx::kFullUv, kWhite.with_alpha(sel_thumb ? 1.0f : 0.75f), 6.0f);
        list.bordered_rect(thumb_r, 6.0f, Color::rgb(0x000000, 0.0f), sel_thumb ? 2.0f : 1.0f,
                           sel_thumb ? ctx.palette.accent : ctx.palette.border);
        if (sel_thumb)
            list.glow(thumb_r, 6.0f, 6.0f, ctx.palette.accent.with_alpha(0.35f));

        const std::string &lbl = (i < static_cast<int>(labels.size())) ? labels[i] : (i == 0 ? "OFF" : (i == 1 ? "SHARP" : "AI"));
        ui::text(list, ctx.fonts.semibold, lbl, thumb_r.cx(), thumb_y + thumb_h + 18.0f, 15,
                 sel_thumb ? ctx.palette.accent : ctx.palette.text_muted, Align::center);
    }

    /* The photo credit sits on the picture: under the labels there is no room. */
    {
        const char *credit = "Photos: NASA (public domain)";
        const float cw = ctx.fonts.regular.measure(credit, 12) + 16.0f;
        const Rect cr{img_rect.x + img_rect.w - cw - 8.0f, img_rect.y + img_rect.h - 30.0f, cw, 22.0f};
        list.rounded_rect(cr, 11.0f, Color::rgb(0x000000, 0.55f));
        ui::text(list, ctx.fonts.regular, credit, cr.cx(), cr.y + 16.0f, 12, kWhite.with_alpha(0.8f), Align::center);
    }
}

/* ---- 1. AI NETWORK: four rising bars for DETAIL and thinner GPU COST. ---- */
void draw_ai_network(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    ui::text(list, fonts.semibold, "DETAIL", a.x, a.top + 16.0f, 16, p.text_muted, Align::left, 2.0f);
    const float base1 = a.top + 105.0f;
    list.line(a.x, base1 + 1.0f, a.x + a.w, base1 + 1.0f, 1.0f, kWhite.with_alpha(0.08f));
    const float h1[4] = {25.0f, 45.0f, 65.0f, 85.0f};

    ui::text(list, fonts.semibold, "GPU COST", a.x, a.top + 135.0f, 16, p.text_muted, Align::left, 2.0f);
    const float base2 = a.top + 215.0f;
    list.line(a.x, base2 + 1.0f, a.x + a.w, base2 + 1.0f, 1.0f, kWhite.with_alpha(0.08f));
    const float h2[4] = {14.0f, 30.0f, 55.0f, 85.0f};

    const float col_w = a.w / 4.0f;
    for (int i = 0; i < 4; ++i)
    {
        const bool sel = (i == pv.index);
        const Color col = sel ? p.accent : kWhite.with_alpha(0.18f);
        const float cx = a.x + static_cast<float>(i) * col_w + col_w * 0.5f;

        Rect r1{cx - 24.0f, base1 - h1[i], 48.0f, h1[i]};
        list.rounded_rect(r1, 6, col);
        if (sel)
            list.glow(r1, 6, 10.0f, p.accent.with_alpha(0.4f));

        Rect r2{cx - 12.0f, base2 - h2[i], 24.0f, h2[i]};
        list.rounded_rect(r2, 4, col);
        if (sel)
            list.glow(r2, 4, 8.0f, p.accent.with_alpha(0.4f));
    }

    const auto labels = chip_labels(pv);
    const float y = chips(list, ctx, a, base2 + 28.0f, labels, pv.index);
    benefit(list, ctx, a, y + 40.0f, pv.benefit);
}

/* ---- 2. HDR OUTPUT: auto wipe between SDR and HDR photos. ---- */
void draw_hdr(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);

    static const char *const kHdrSdr = "../icons/preview_hdr_sdr.png";
    static const char *const kHdrHdr = "../icons/preview_hdr_hdr.png";

    const auto raw_labels = chip_labels(pv);
    const bool is_hdr = pv.index >= 0 && pv.index < static_cast<int>(raw_labels.size())
                            ? raw_labels[pv.index].find("OFF") == std::string::npos
                            : pv.index == 1;

    const float img_w = a.w;
    const float img_h = std::round(img_w * 9.0f / 16.0f);
    const Rect img_rect{a.x, a.top, img_w, img_h};

    const std::uint32_t tex_sdr = ctx.textures.icon(kHdrSdr);
    const std::uint32_t tex_hdr = ctx.textures.icon(kHdrHdr);

    draw_photo_wipe(list, ctx, img_rect, tex_sdr, tex_hdr, "SDR", "HDR", is_hdr);

    const float l1_y = img_rect.y + img_rect.h + 34.0f;
    ui::text(list, ctx.fonts.regular, "Bright areas keep their detail", a.x, l1_y, 16, kWhite);
    const float l2_y = l1_y + 22.0f;
    ui::text(list, ctx.fonts.regular, "Needs an HDR10 TV", a.x, l2_y, 15, ctx.palette.text_faint);

    std::vector<std::string> clean_labels;
    for (const std::string &l : raw_labels)
    {
        if (l.find("OFF") != std::string::npos)
            clean_labels.push_back("OFF");
        else if (l.find("AUTO") != std::string::npos || l.find("MATCH") != std::string::npos)
            clean_labels.push_back("AUTO");
        else
            clean_labels.push_back(l);
    }
    if (clean_labels.empty())
        clean_labels = {"OFF", "AUTO"};

    chips(list, ctx, a, l2_y + 14.0f, clean_labels, pv.index);

    credit_line(list, ctx, box, a);
}

/* ---- 3. MOTION SMOOTHING: 24 fps stepped motion vs smooth continuous glide. ---- */
void draw_motion(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    const bool on = (pv.index > 0);

    /* 16:9 cinema/video screen */
    const float sw = 352.0f, sh = 198.0f;
    Rect screen{a.x + (a.w - sw) * 0.5f, a.top, sw, sh};

    list.gradient_rect(screen, 12.0f, Color::rgb(0x0e111a), Color::rgb(0x06080d));
    frame_border(list, ctx, screen, on);
    if (on)
        list.glow(screen, 12.0f, 10.0f, p.accent.with_alpha(0.18f));

    tag(list, ctx, screen, on ? "SMOOTHING ON" : "SMOOTHING OFF", on);

    /* Ball motion track inside the screen */
    list.push_clip(screen);

    /* Subtle cinematic pan background: horizon line and distant vertical beacons */
    const float ground_y = screen.y + sh * 0.78f;
    list.line(screen.x, ground_y, screen.x + sw, ground_y, 1.0f, kWhite.with_alpha(0.08f));
    for (int i = 0; i < 5; ++i)
    {
        const float bx_bg = screen.x + 36.0f + static_cast<float>(i) * 70.0f;
        list.line(bx_bg, ground_y - 20.0f, bx_bg, ground_y, 1.0f, kWhite.with_alpha(0.04f));
    }

    const float track_x0 = screen.x + 32.0f;
    const float track_x1 = screen.x + sw - 32.0f;
    const float track_w = track_x1 - track_x0;
    const float track_y = screen.y + sh * 0.44f;

    /* Guide rail */
    list.line(track_x0, track_y, track_x1, track_y, 2.0f, kWhite.with_alpha(0.10f));
    list.circle(track_x0, track_y, 3.0f, kWhite.with_alpha(0.20f));
    list.circle(track_x1, track_y, 3.0f, kWhite.with_alpha(0.20f));

    /* 2.4 second loop */
    const float loop_dur = 2.4f;
    const float norm_t = std::fmod(ctx.time, loop_dur) / loop_dur;
    const float ball_r = 10.0f;

    if (!on)
    {
        /* OFF: visible steps (about 6 steps per sec = 14 steps over 2.4s) with faint ghost circles left at each step */
        const int total_steps = 14;
        int cur_step = static_cast<int>(norm_t * static_cast<float>(total_steps));
        if (cur_step >= total_steps)
            cur_step = total_steps - 1;

        /* Faint ghost circles left behind at earlier steps */
        for (int s = 0; s < cur_step; ++s)
        {
            const float gx = track_x0 + track_w * (static_cast<float>(s) / static_cast<float>(total_steps - 1));
            const float age = static_cast<float>(cur_step - s);
            const float alpha = std::max(0.05f, 0.24f - age * 0.015f);
            list.circle(gx, track_y, ball_r - 1.0f, kWhite.with_alpha(alpha));
            list.ring(gx, track_y, ball_r - 1.0f, 1.0f, kWhite.with_alpha(alpha * 1.3f));
        }

        /* Active ball at cur_step */
        const float cur_x = track_x0 + track_w * (static_cast<float>(cur_step) / static_cast<float>(total_steps - 1));
        list.glow({cur_x - ball_r, track_y - ball_r, ball_r * 2.0f, ball_r * 2.0f}, ball_r, 6.0f, kWhite.with_alpha(0.30f));
        list.circle(cur_x, track_y, ball_r, kWhite);
        list.circle(cur_x - 3.0f, track_y - 3.0f, 3.0f, Color::rgb(0xffffff, 0.85f));
    }
    else
    {
        /* ON: ball glides continuously with a soft fading trail */
        const float cur_x = track_x0 + track_w * norm_t;

        /* Soft fading trail */
        const int trail_samples = 14;
        for (int k = trail_samples; k >= 1; --k)
        {
            const float lag = static_cast<float>(k) * 0.022f;
            const float tx = cur_x - track_w * lag;
            if (tx >= track_x0)
            {
                const float fade = 1.0f - (static_cast<float>(k) / static_cast<float>(trail_samples));
                const float alpha = 0.38f * fade;
                const float tr = ball_r * (0.50f + 0.50f * fade);
                list.circle(tx, track_y, tr, p.accent.with_alpha(alpha));
            }
        }

        /* Glowing continuous ball */
        list.glow({cur_x - ball_r - 2.0f, track_y - ball_r - 2.0f, (ball_r + 2.0f) * 2.0f, (ball_r + 2.0f) * 2.0f},
                  ball_r + 2.0f, 10.0f, p.accent.with_alpha(0.65f));
        list.circle(cur_x, track_y, ball_r, kWhite);
        list.circle(cur_x - 3.0f, track_y - 3.0f, 3.0f, Color::rgb(0xffffff, 0.9f));
    }

    list.pop_clip();

    /* Captions below the screen */
    const float cap_y = screen.y + screen.h + 20.0f;
    if (!on)
    {
        ui::text(list, fonts.semibold, "Normal film motion", a.x, cap_y, 16, kWhite, Align::left);
    }
    else
    {
        ui::text(list, fonts.semibold, "Extra smooth, like a video call or sports", a.x, cap_y, 16, p.accent, Align::left);
        ui::text(list, fonts.regular, "Films can look too smooth", a.x, cap_y + 22.0f, 14, p.text_muted, Align::left);
    }

    const auto labels = chip_labels(pv);
    const float chips_y = screen.y + screen.h + (on ? 56.0f : 36.0f);
    const float y = chips(list, ctx, a, chips_y, labels, pv.index);
    benefit(list, ctx, a, y + 24.0f, pv.benefit);
}

/* ---- 4. REFRESH RATE (120 Hz output): 60 Hz uneven stutter vs 120 Hz even lanes. ---- */
void draw_refresh(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    /* pv.index == 0 is OFF (60 Hz); > 0 is 120 Hz (Always or PlaybackOnly) */
    const bool is_120 = (pv.index > 0);

    const float lane_w = a.w;
    const float lane_h = 76.0f;
    Rect lane1{a.x, a.top, lane_w, lane_h};
    Rect lane2{a.x, a.top + lane_h + 12.0f, lane_w, lane_h};

    /* Common timing for both lanes */
    const float loop_dur = 2.4f;
    const float t = std::fmod(ctx.time, loop_dur);
    const float ball_r = 9.0f;

    auto draw_lane = [&](const Rect &lane, const std::string &badge_text, const std::string &caption,
                         bool active, bool is_top_60hz) {
        /* Base container */
        const Color bg = active ? Color::rgb(0x0e1322) : Color::rgb(0x07090f, 0.6f);
        list.rounded_rect(lane, 12.0f, bg);
        frame_border(list, ctx, lane, active);
        if (active)
            list.glow(lane, 12.0f, 8.0f, p.accent.with_alpha(0.20f));

        /* Badge on left */
        const float badge_w = 76.0f;
        Rect badge_r{lane.x + 14.0f, lane.y + 12.0f, badge_w, 24.0f};
        list.rounded_rect(badge_r, 12.0f, active ? p.accent : kWhite.with_alpha(0.08f));
        ui::text(list, fonts.semibold, badge_text, badge_r.cx(), badge_r.cy() + 5.0f, 13,
                 active ? kInk : p.text_muted, Align::center);

        /* Caption below badge */
        ui::text(list, fonts.regular, caption, lane.x + 14.0f, lane.y + 54.0f, 13,
                 active ? kWhite : p.text_faint, Align::left);

        /* Inset track screen on the right */
        const float track_screen_x = lane.x + 184.0f;
        const float track_screen_w = lane.w - 198.0f;
        Rect track_screen{track_screen_x, lane.y + 10.0f, track_screen_w, lane.h - 20.0f};

        list.rounded_rect(track_screen, 8.0f, Color::rgb(0x05070c));
        list.bordered_rect(track_screen, 8.0f, Color::rgb(0, 0), 1.0f,
                           active ? p.accent.with_alpha(0.35f) : kWhite.with_alpha(0.06f));

        const float bx0 = track_screen.x + 18.0f;
        const float bx1 = track_screen.x + track_screen_w - 18.0f;
        const float bw = bx1 - bx0;
        const float track_y = track_screen.cy();

        /* Track line */
        list.line(bx0, track_y, bx1, track_y, 1.5f, kWhite.with_alpha(active ? 0.14f : 0.05f));

        /* Discrete step positions */
        const int total_steps = 10;
        for (int i = 0; i < total_steps; ++i)
        {
            const float sx = bx0 + bw * (static_cast<float>(i) / static_cast<float>(total_steps - 1));
            list.circle(sx, track_y, 2.0f, kWhite.with_alpha(active ? 0.18f : 0.07f));
        }

        /* Calculate ball position */
        float ball_x = bx0;
        if (is_top_60hz)
        {
            /* 60 Hz: uneven timing (alternating step weights 3 and 2: lingering every other step) */
            const float weight_t = (t / loop_dur) * 25.0f;
            float cum = 0.0f;
            int step_60 = 0;
            for (int i = 0; i < total_steps; ++i)
            {
                const float w = (i % 2 == 0) ? 3.0f : 2.0f;
                if (weight_t < cum + w || i == total_steps - 1)
                {
                    step_60 = i;
                    break;
                }
                cum += w;
            }
            ball_x = bx0 + bw * (static_cast<float>(step_60) / static_cast<float>(total_steps - 1));
        }
        else
        {
            /* 120 Hz: perfectly even steps */
            int step_120 = static_cast<int>((t / loop_dur) * static_cast<float>(total_steps));
            if (step_120 >= total_steps)
                step_120 = total_steps - 1;
            ball_x = bx0 + bw * (static_cast<float>(step_120) / static_cast<float>(total_steps - 1));
        }

        /* Draw ball */
        if (active)
        {
            list.glow({ball_x - ball_r, track_y - ball_r, ball_r * 2.0f, ball_r * 2.0f},
                      ball_r, 6.0f, p.accent.with_alpha(0.60f));
            list.circle(ball_x, track_y, ball_r, kWhite);
            list.circle(ball_x - 2.5f, track_y - 2.5f, 2.5f, Color::rgb(0xffffff, 0.9f));
        }
        else
        {
            list.circle(ball_x, track_y, ball_r, kWhite.with_alpha(0.35f));
        }
    };

    /* Top lane: 60 Hz */
    draw_lane(lane1, "60 HZ", "Slight stutter in films", !is_120, true);

    /* Bottom lane: 120 Hz */
    draw_lane(lane2, "120 HZ", "Even and smooth", is_120, false);

    const auto labels = chip_labels(pv);
    const float chips_y = lane2.y + lane2.h + 24.0f;
    const float y = chips(list, ctx, a, chips_y, labels, pv.index);
    benefit(list, ctx, a, y + 24.0f, pv.benefit);
}

/* ---- 5. MATCH VIDEO RATE: mock TV with mode badge and ball lane. ---- */
void draw_match_rate(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    const bool on = (pv.index == 1);

    /* Mock TV bezel and screen */
    const float tv_w = 416.0f, tv_h = 152.0f;
    Rect tv{a.x + (a.w - tv_w) * 0.5f, a.top, tv_w, tv_h};

    /* TV Stand: base neck and wide stand foot */

    /* TV outer frame */
    list.rounded_rect(tv, 12.0f, Color::rgb(0x10131c));
    frame_border(list, ctx, tv, on);
    if (on)
        list.glow(tv, 12.0f, 8.0f, p.accent.with_alpha(0.18f));

    /* Inner display screen */
    Rect screen{tv.x + 8.0f, tv.y + 8.0f, tv.w - 16.0f, tv.h - 16.0f};
    list.rounded_rect(screen, 8.0f, Color::rgb(0x06080e));
    list.bordered_rect(screen, 8.0f, Color::rgb(0, 0), 1.0f, kWhite.with_alpha(0.06f));

    list.push_clip(screen);

    /* Mode badge animation in ON mode:
     * Cycle: 4.0s
     * 0.0s - 0.9s: 60 Hz initial
     * 0.9s - 1.2s: brief switch blink
     * 1.2s - 4.0s: switched to "24 Hz film mode" / "TV matches the film"
     * At ctx.time = 1.5s, it is comfortably in the matched film state!
     */
    const float cycle_dur = 4.0f;
    const float phase = std::fmod(ctx.time, cycle_dur);
    const bool in_switched_state = on && (phase >= 1.2f || ctx.time <= 0.01f);
    const bool is_blinking = on && (phase >= 0.9f && phase < 1.2f);

    /* TV top bar: left status indicator */
    list.circle(screen.x + 16.0f, screen.y + 20.0f, 3.5f,
                on ? (in_switched_state ? p.accent : Color::rgb(0xffa500)) : p.text_muted);
    ui::text(list, fonts.semibold, on ? "HDMI 1 (24P FILM)" : "HDMI 1",
             screen.x + 26.0f, screen.y + 24.0f, 11, p.text_muted, Align::left, 1.5f);

    /* Mode Badge on the top right */
    std::string badge_label;
    bool badge_accent = false;
    if (!on)
    {
        badge_label = "TV stays at 60 Hz";
        badge_accent = false;
    }
    else
    {
        if (in_switched_state)
        {
            badge_label = "TV matches the film";
            badge_accent = true;
        }
        else
        {
            badge_label = "TV stays at 60 Hz";
            badge_accent = false;
        }
    }

    const float badge_w = fonts.semibold.measure(badge_label, 13) + 24.0f;
    Rect badge_r{screen.x + screen.w - badge_w - 12.0f, screen.y + 10.0f, badge_w, 24.0f};
    list.rounded_rect(badge_r, 12.0f, badge_accent ? p.accent : Color::rgb(0x000000, 0.65f));
    if (!badge_accent)
        list.bordered_rect(badge_r, 12.0f, Color::rgb(0, 0), 1.0f, p.border.with_alpha(0.5f));
    ui::text(list, fonts.semibold, badge_label, badge_r.cx(), badge_r.cy() + 5.0f, 13,
             badge_accent ? kInk : kWhite, Align::center);

    /* Ball lane inside TV */
    Rect tv_lane{screen.x + 14.0f, screen.y + 44.0f, screen.w - 28.0f, 80.0f};
    list.rounded_rect(tv_lane, 8.0f, Color::rgb(0x0a0d16));
    list.bordered_rect(tv_lane, 8.0f, Color::rgb(0, 0), 1.0f,
                       on ? p.accent.with_alpha(0.25f) : kWhite.with_alpha(0.06f));

    const float bx0 = tv_lane.x + 20.0f;
    const float bx1 = tv_lane.x + tv_lane.w - 20.0f;
    const float bw = bx1 - bx0;
    const float track_y = tv_lane.cy();

    /* Track line */
    list.line(bx0, track_y, bx1, track_y, 1.5f, kWhite.with_alpha(on ? 0.14f : 0.06f));

    const int total_steps = 10;
    for (int i = 0; i < total_steps; ++i)
    {
        const float sx = bx0 + bw * (static_cast<float>(i) / static_cast<float>(total_steps - 1));
        list.circle(sx, track_y, 2.0f, kWhite.with_alpha(on ? 0.18f : 0.08f));
    }

    /* Ball motion:
     * OFF: ball with uneven steps (judder: alternating weights 3 and 2)
     * ON: ball with perfectly even steps
     */
    const float loop_dur = 2.4f;
    const float t = std::fmod(ctx.time, loop_dur);
    const float ball_r = 9.0f;
    float ball_x = bx0;

    if (!on || !in_switched_state)
    {
        /* Uneven steps (lingering alternating frames) */
        const float weight_t = (t / loop_dur) * 25.0f;
        float cum = 0.0f;
        int step_uneven = 0;
        for (int i = 0; i < total_steps; ++i)
        {
            const float w = (i % 2 == 0) ? 3.0f : 2.0f;
            if (weight_t < cum + w || i == total_steps - 1)
            {
                step_uneven = i;
                break;
            }
            cum += w;
        }
        ball_x = bx0 + bw * (static_cast<float>(step_uneven) / static_cast<float>(total_steps - 1));

        list.circle(ball_x, track_y, ball_r, kWhite.with_alpha(0.60f));
    }
    else
    {
        /* Perfectly even steps */
        int step_even = static_cast<int>((t / loop_dur) * static_cast<float>(total_steps));
        if (step_even >= total_steps)
            step_even = total_steps - 1;
        ball_x = bx0 + bw * (static_cast<float>(step_even) / static_cast<float>(total_steps - 1));

        list.glow({ball_x - ball_r, track_y - ball_r, ball_r * 2.0f, ball_r * 2.0f},
                  ball_r, 6.0f, p.accent.with_alpha(0.60f));
        list.circle(ball_x, track_y, ball_r, kWhite);
        list.circle(ball_x - 2.5f, track_y - 2.5f, 2.5f, Color::rgb(0xffffff, 0.9f));
    }

    /* Screen switch blink: brief black veil when mode switches */
    if (is_blinking)
    {
        const float blink_prog = (phase - 0.9f) / 0.3f;
        const float blink_alpha = 0.92f * std::sin(blink_prog * 3.14159f);
        list.rounded_rect(screen, 8.0f, Color::rgb(0x000000, blink_alpha));
    }

    list.pop_clip();

    /* Primary caption below TV */
    const float note_y = tv.y + tv.h + 20.0f;
    if (!on)
        ui::text(list, fonts.semibold, "Films can judder slightly", a.x, note_y, 16, kWhite, Align::left);
    else
        ui::text(list, fonts.semibold, "Perfectly even motion", a.x, note_y, 16, p.accent, Align::left);

    /* Plain note */
    ui::text(list, fonts.regular, "The screen blinks briefly when it switches", a.x, note_y + 22.0f, 14,
             p.text_muted, Align::left);

    const auto labels = chip_labels(pv);
    const float chips_y = note_y + 50.0f;
    const float y = chips(list, ctx, a, chips_y, labels, pv.index);
    benefit(list, ctx, a, y + 24.0f, pv.benefit);
}

/* ---- 6. ASPECT RATIO: 16:9 screen with fit, fill or stretch picture. ---- */
void draw_aspect(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const float sw = 384.0f, sh = 216.0f;
    Rect screen{a.x + (a.w - sw) * 0.5f, a.top, sw, sh};
    list.rounded_rect(screen, 12, Color::rgb(0x050508));
    frame_border(list, ctx, screen, true);

    int mode = pv.index;
    if (pv.index < static_cast<int>(pv.choices.size()))
    {
        const std::string u = ui::upper(pv.choices[pv.index]);
        if (u.find("FIT") != std::string::npos)
            mode = 0;
        else if (u.find("FILL") != std::string::npos)
            mode = 1;
        else if (u.find("STRETCH") != std::string::npos)
            mode = 2;
    }

    list.push_clip(screen);
    Rect pic;
    float rx = 0.0f, ry = 0.0f;
    if (mode == 0) // FIT
    {
        const float pw = sh * (4.0f / 3.0f);
        pic = {screen.x + (sw - pw) * 0.5f, screen.y, pw, sh};
        rx = ry = sh * 0.22f;
    }
    else if (mode == 1) // FILL
    {
        const float ph = sw * (3.0f / 4.0f);
        pic = {screen.x, screen.y + (sh - ph) * 0.5f, sw, ph};
        rx = ry = ph * 0.22f;
    }
    else // STRETCH
    {
        pic = screen;
        ry = sh * 0.22f;
        rx = ry * (16.0f / 9.0f) / (4.0f / 3.0f);
    }

    const float hhalf = pic.h * 0.5f;
    list.gradient_rect({pic.x, pic.y, pic.w, hhalf}, 0, Color::rgb(0x1e3c72), Color::rgb(0x2a5298));
    list.gradient_rect({pic.x, pic.y + hhalf, pic.w, hhalf}, 0, Color::rgb(0x2a5298), Color::rgb(0x0f2027));

    float xy[64];
    for (int i = 0; i < 32; ++i)
    {
        const float th = static_cast<float>(i) * (6.2831853f / 32.0f);
        xy[i * 2] = pic.cx() + rx * std::cos(th);
        xy[i * 2 + 1] = pic.cy() + ry * std::sin(th);
    }
    list.polygon(xy, 32, Color::rgb(0xffe066));
    list.pop_clip();

    const auto labels = chip_labels(pv);
    const std::string tag_lbl = (pv.index < static_cast<int>(labels.size())) ? labels[pv.index] : "FIT";
    tag(list, ctx, screen, tag_lbl, true);

    const float y = chips(list, ctx, a, screen.y + screen.h + 28.0f, labels, pv.index);
    benefit(list, ctx, a, y + 40.0f, pv.benefit);
}

/* ---- 7. AUTO SUBTITLES: sample line shown vs empty scene note. ---- */
void draw_sub_auto(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const float sw = 400.0f, sh = 225.0f;
    Rect frame{a.x + (a.w - sw) * 0.5f, a.top, sw, sh};
    mock_scene(list, frame, 0.0f, false);
    const bool on = (pv.index == 1);
    frame_border(list, ctx, frame, on);
    tag(list, ctx, frame, on ? "ON" : "OFF", on);

    if (on)
    {
        const char *line = "You were right about the lighthouse.";
        const float tx = frame.cx(), ty = frame.y + frame.h - 24.0f;
        ui::text(list, fonts.semibold, line, tx + 1.5f, ty + 1.5f, 22, Color::rgb(0x000000, 0.9f), Align::center);
        ui::text(list, fonts.semibold, line, tx - 1.0f, ty - 1.0f, 22, Color::rgb(0x000000, 0.9f), Align::center);
        ui::text(list, fonts.semibold, line, tx, ty, 22, kWhite, Align::center);
    }
    else
    {
        ui::text(list, fonts.semibold, "NO SUBTITLES", frame.cx(), frame.y + frame.h - 24.0f, 16, p.text_muted, Align::center);
    }

    const auto labels = chip_labels(pv);
    const float y = chips(list, ctx, a, frame.y + frame.h + 28.0f, labels, pv.index);
    benefit(list, ctx, a, y + 40.0f, pv.benefit);
}

/* ---- 8. SUBTITLE FONT: sample line in the chosen typeface. ---- */
void draw_sub_font(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const float sw = 400.0f, sh = 225.0f;
    Rect frame{a.x + (a.w - sw) * 0.5f, a.top, sw, sh};
    mock_scene(list, frame, 0.0f, false);
    frame_border(list, ctx, frame, true);

    const auto labels = chip_labels(pv);
    const std::string tag_lbl = (pv.index < static_cast<int>(labels.size())) ? labels[pv.index] : "STANDARD";
    tag(list, ctx, frame, tag_lbl, true);

    const char *line = "You were right about the lighthouse.";
    const float tx = frame.cx();
    const ui::FontRef *face = &fonts.regular;
    float fsize = 26.0f;
    float tracking = 0.0f;
    if (pv.index == 1)
        face = &fonts.display;
    else if (pv.index == 2)
        face = &fonts.semibold;
    else if (pv.index == 3)
    {
        face = &fonts.regular;
        fsize = 22.0f;
        tracking = -0.5f;
    }

    /* Wrap the sample line so it fits cleanly inside the 400 px frame at every choice. */
    const float max_text_w = frame.w - 36.0f;
    std::vector<std::string> lines = face->font->wrap(line, fsize, max_text_w);
    if (lines.empty())
        lines.push_back(line);

    const float line_h = fsize * 1.25f;
    const float last_baseline = frame.y + frame.h - 22.0f;
    float y_base = last_baseline - static_cast<float>(lines.size() - 1) * line_h;
    for (const std::string &l : lines)
    {
        const std::string fitted = face->font->fit(l, fsize, max_text_w);
        ui::text(list, *face, fitted, tx + 1.5f, y_base + 1.5f, fsize, Color::rgb(0x000000, 0.9f), Align::center, tracking);
        ui::text(list, *face, fitted, tx, y_base, fsize, kWhite, Align::center, tracking);
        y_base += line_h;
    }

    const float y = chips(list, ctx, a, frame.y + frame.h + 24.0f, labels, pv.index);
    benefit(list, ctx, a, y + 24.0f, pv.benefit);
}

/* ---- #231: SUBTITLE SIZE / COLOUR / POSITION / BACKGROUND. One sample line
 * on the mock scene; only the row's own choice changes, the rest is the
 * default look. ---- */
void draw_sub_look(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const float sw = 400.0f, sh = 225.0f;
    Rect frame{a.x + (a.w - sw) * 0.5f, a.top, sw, sh};
    mock_scene(list, frame, 0.0f, false);
    frame_border(list, ctx, frame, true);

    const auto labels = chip_labels(pv);
    const std::string tag_lbl = (pv.index < static_cast<int>(labels.size())) ? labels[pv.index] : "";
    tag(list, ctx, frame, tag_lbl, true);

    /* The player's sizes (40/50/60/72 px on 1080) scaled to the 400 px frame. */
    static const float kSize[4] = {17.0f, 21.0f, 25.0f, 30.0f};
    static const unsigned kInk[4] = {0xffffff, 0xffe14d, 0x66e3ff, 0x7dff8a};
    static const float kLift[3] = {0.0f, 17.0f, 34.0f};
    const int i = std::max(0, pv.index);
    float fsize = kSize[1];
    Color ink = kWhite;
    float lift = 0.0f;
    int bg = 0;
    if (pv.kind == EVO_RMLUI_PREVIEW_SUB_SIZE) fsize = kSize[std::min(i, 3)];
    else if (pv.kind == EVO_RMLUI_PREVIEW_SUB_TEXT_COLOUR) ink = Color::rgb(kInk[std::min(i, 3)]);
    else if (pv.kind == EVO_RMLUI_PREVIEW_SUB_LIFT) lift = kLift[std::min(i, 2)];
    else bg = std::min(i, 3);

    const ui::FontRef &face = fonts.semibold;
    const char *line = "You were right about the lighthouse.";
    const float max_w = frame.w - 36.0f;
    std::vector<std::string> lines = face.font->wrap(line, fsize, max_w);
    if (lines.empty())
        lines.push_back(line);
    const float line_h = fsize * 1.25f;
    const float cx = frame.cx();
    float y = frame.y + frame.h - 20.0f - lift - static_cast<float>(lines.size() - 1) * line_h;
    for (const std::string &l : lines)
    {
        const float w = face.measure(l, fsize);
        if (bg == 0)
            list.rounded_rect({cx - w * 0.5f - 8, y - fsize * 0.95f, w + 16, fsize * 1.25f}, 5,
                              Color::rgb(0x000000, 0.55f));
        else if (bg == 1)
            ui::text(list, face, l, cx + 1.5f, y + 1.5f, fsize, Color::rgb(0x000000, 0.85f), Align::center);
        else if (bg == 2)
        {
            static const float kDir[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1},
                                             {-0.7f, -0.7f}, {0.7f, -0.7f}, {-0.7f, 0.7f}, {0.7f, 0.7f}};
            for (const auto &d : kDir)
                ui::text(list, face, l, cx + d[0], y + d[1], fsize, Color::rgb(0x000000, 0.9f), Align::center);
        }
        ui::text(list, face, l, cx, y, fsize, ink, Align::center);
        y += line_h;
    }

    const float yc = chips(list, ctx, a, frame.y + frame.h + 24.0f, labels, pv.index);
    benefit(list, ctx, a, yc + 24.0f, pv.benefit);
}

/* ---- 9. SECONDARY POSITION: stacked above primary vs top of screen. ---- */
void draw_sub_pos(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const float sw = 400.0f, sh = 225.0f;
    Rect frame{a.x + (a.w - sw) * 0.5f, a.top, sw, sh};
    mock_scene(list, frame, 0.0f, false);
    frame_border(list, ctx, frame, true);

    const auto labels = chip_labels(pv);
    const std::string tag_lbl = (pv.index < static_cast<int>(labels.size())) ? labels[pv.index] : "POSITION";
    tag(list, ctx, frame, tag_lbl, true);

    const char *line1 = "You were right about the lighthouse.";
    const char *line2 = "Tu avais raison pour le phare.";
    const float cx = frame.cx();
    const float l1_y = frame.y + frame.h - 22.0f;
    const float l2_y = (pv.index == 1) ? frame.y + 66.0f : frame.y + frame.h - 52.0f;

    ui::text(list, fonts.semibold, line2, cx + 1.5f, l2_y + 1.5f, 18, Color::rgb(0x000000, 0.9f), Align::center);
    ui::text(list, fonts.semibold, line2, cx, l2_y, 18, p.accent, Align::center);

    ui::text(list, fonts.semibold, line1, cx + 1.5f, l1_y + 1.5f, 22, Color::rgb(0x000000, 0.9f), Align::center);
    ui::text(list, fonts.semibold, line1, cx, l1_y, 22, kWhite, Align::center);

    const float y = chips(list, ctx, a, frame.y + frame.h + 28.0f, labels, pv.index);
    benefit(list, ctx, a, y + 40.0f, pv.benefit);
}

/* ---- 10. SECONDARY COLOUR: yellow, cyan, or white second line. ---- */
void draw_sub_colour(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const float sw = 400.0f, sh = 225.0f;
    Rect frame{a.x + (a.w - sw) * 0.5f, a.top, sw, sh};
    mock_scene(list, frame, 0.0f, false);
    frame_border(list, ctx, frame, true);

    const auto labels = chip_labels(pv);
    const std::string tag_lbl = (pv.index < static_cast<int>(labels.size())) ? labels[pv.index] : "COLOUR";
    tag(list, ctx, frame, tag_lbl, true);

    Color sec_color = Color::rgb(0xffd84a); // index 0 yellow
    if (pv.index == 1)
        sec_color = Color::rgb(0x4adfff); // index 1 cyan
    else if (pv.index == 2)
        sec_color = Color::rgb(0xffffff); // index 2 white

    const char *line1 = "You were right about the lighthouse.";
    const char *line2 = "Tu avais raison pour le phare.";
    const float cx = frame.cx();
    const float l2_y = frame.y + frame.h - 52.0f;
    const float l1_y = frame.y + frame.h - 22.0f;

    ui::text(list, fonts.semibold, line2, cx + 1.5f, l2_y + 1.5f, 18, Color::rgb(0x000000, 0.9f), Align::center);
    ui::text(list, fonts.semibold, line2, cx, l2_y, 18, sec_color, Align::center);

    ui::text(list, fonts.semibold, line1, cx + 1.5f, l1_y + 1.5f, 22, Color::rgb(0x000000, 0.9f), Align::center);
    ui::text(list, fonts.semibold, line1, cx, l1_y, 22, kWhite, Align::center);

    const float y = chips(list, ctx, a, frame.y + frame.h + 28.0f, labels, pv.index);
    benefit(list, ctx, a, y + 40.0f, pv.benefit);
}

/* ---- 11. THEME: mini screen mock + palette swatch strip. ---- */
void draw_theme(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const Palette &p = ctx.palette;

    const size_t idx = std::min<size_t>(std::max(pv.index, 0), pv.choice_bg.empty() ? 0 : pv.choice_bg.size() - 1);
    const uint32_t bg_hex = (!pv.choice_bg.empty()) ? pv.choice_bg[idx] : 0x080d16;
    const uint32_t acc_hex = (!pv.choice_accent.empty()) ? pv.choice_accent[idx] : 0xffcd00;
    const Color t_bg = Color::rgb(bg_hex);
    const Color t_acc = Color::rgb(acc_hex);

    Rect mock{a.x + (a.w - 380.0f) * 0.5f, a.top, 380.0f, 160.0f};
    list.rounded_rect(mock, 14, t_bg);
    list.bordered_rect(mock, 14, Color::rgb(0,0), 1.5f, t_acc.with_alpha(0.5f));

    list.rounded_rect({mock.x + 18, mock.y + 14, 80, 8}, 4, kWhite.with_alpha(0.7f));
    list.line(mock.x + 18, mock.y + 30, mock.x + mock.w - 18, mock.y + 30, 1.0f, kWhite.with_alpha(0.1f));

    const float ry0 = mock.y + 38.0f;
    for (int r = 0; r < 3; ++r)
    {
        Rect row_r{mock.x + 18, ry0 + static_cast<float>(r) * 38.0f, mock.w - 36, 32};
        const bool row_sel = (r == 1);
        list.rounded_rect(row_r, 6, row_sel ? t_acc.with_alpha(0.20f) : kWhite.with_alpha(0.06f));
        if (row_sel)
        {
            list.bordered_rect(row_r, 6, Color::rgb(0,0), 1.5f, t_acc);
            Rect tog{row_r.x + row_r.w - 38, row_r.y + 7, 28, 18};
            list.rounded_rect(tog, 9, t_acc);
            list.circle(tog.x + 19, tog.cy(), 6, t_bg);
        }
        list.rounded_rect({row_r.x + 12, row_r.y + 11, row_sel ? 80.0f : 50.0f, 10}, 5, kWhite.with_alpha(row_sel ? 0.9f : 0.4f));
    }

    const float swatch_y = mock.y + mock.h + 20.0f;
    size_t n_choices = std::max<size_t>(pv.choices.size(), pv.choice_bg.size());
    if (n_choices == 0)
        n_choices = 4;
    const float step = std::min(52.0f, a.w / static_cast<float>(n_choices));
    const float strip_w = step * static_cast<float>(n_choices);
    const float sx0 = a.x + (a.w - strip_w) * 0.5f;
    for (size_t c = 0; c < n_choices; ++c)
    {
        const float cx = sx0 + static_cast<float>(c) * step + step * 0.5f;
        const uint32_t c_bg_hex = (c < pv.choice_bg.size()) ? pv.choice_bg[c] : 0x1a1a1a;
        const uint32_t c_acc_hex = (c < pv.choice_accent.size()) ? pv.choice_accent[c] : 0xffcd00;
        list.circle(cx - 7.0f, swatch_y + 12.0f, 7.0f, Color::rgb(c_bg_hex));
        list.circle(cx + 7.0f, swatch_y + 12.0f, 7.0f, Color::rgb(c_acc_hex));
        if (static_cast<int>(c) == pv.index)
            list.ring(cx, swatch_y + 12.0f, 18.0f, 2.0f, p.accent);
    }

    const auto labels = chip_labels(pv);
    const float y = chips(list, ctx, a, swatch_y + 36.0f, labels, pv.index);
    benefit(list, ctx, a, y + 40.0f, pv.benefit);
}

/* ---- 12. CONTROLLER LIGHTBAR: DualSense top-down outline. ---- */
void draw_lightbar(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const Palette &p = ctx.palette;

    /* preview_controller.png is 720x456 (aspect 1.5789). Draw centred, 360 px wide. */
    const float cw = 360.0f;
    const float ch = std::round(cw * 456.0f / 720.0f); // 228 px
    const Rect c_rect{a.x + (a.w - cw) * 0.5f, a.top, cw, ch};

    if (const std::uint32_t tex = ctx.textures.icon("../icons/preview_controller.png"))
        list.image(tex, c_rect, ::hui::gfx::kFullUv, kWhite);

    const bool on = (pv.index == 1);
    if (on)
    {
        /* In the 600x380 design space, the light bars are:
         * Left:  from (210, 82) curving to (212, 158)
         * Right: from (390, 82) curving to (388, 158)
         * Scale from 600x380 space to drawn rectangle is cw / 600.0f = 0.60f.
         */
        const float s = cw / 600.0f;
        const float breathe = 0.5f + 0.5f * std::sin(ctx.time * 2.2f);
        const float glow_spread = 8.0f + 6.0f * breathe;
        const float glow_alpha = 0.40f + 0.35f * breathe;

        // Glow around both strips
        const Rect left_glow{c_rect.x + 198.0f * s, c_rect.y + 78.0f * s, 18.0f * s, 84.0f * s};
        const Rect right_glow{c_rect.x + 384.0f * s, c_rect.y + 78.0f * s, 18.0f * s, 84.0f * s};
        list.glow(left_glow, 6.0f, glow_spread, p.accent.with_alpha(glow_alpha));
        list.glow(right_glow, 6.0f, glow_spread, p.accent.with_alpha(glow_alpha));

        // Draw curved strips (about 6 px wide at drawn scale) with capsule-capped line segments
        constexpr int kSteps = 8;
        float prev_lx = 0.0f, prev_ly = 0.0f;
        float prev_rx = 0.0f, prev_ry = 0.0f;
        for (int i = 0; i <= kSteps; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(kSteps);
            const float yd = 82.0f + 76.0f * t;
            const float bow = 4.0f * t * (1.0f - t);
            const float xld = 210.0f + 2.0f * t - 9.0f * bow;
            const float xrd = 390.0f - 2.0f * t + 9.0f * bow;

            const float lx = c_rect.x + xld * s;
            const float ly = c_rect.y + yd * s;
            const float rx = c_rect.x + xrd * s;
            const float ry = c_rect.y + yd * s;

            if (i > 0)
            {
                list.line(prev_lx, prev_ly, lx, ly, 6.0f, p.accent);
                list.line(prev_rx, prev_ry, rx, ry, 6.0f, p.accent);
            }
            prev_lx = lx;
            prev_ly = ly;
            prev_rx = rx;
            prev_ry = ry;
        }
    }

    tag(list, ctx, c_rect, on ? "ON" : "OFF", on);

    const auto labels = chip_labels(pv);
    const float y = chips(list, ctx, a, c_rect.y + c_rect.h + 20.0f, labels, pv.index);
    benefit(list, ctx, a, y + 24.0f, pv.benefit);
}

/* Top-down living room view for audio channel layout and surround sound test. */
float draw_living_room_audio(DrawList &list, const Context &ctx, const Area &a, int layout)
{
    const Palette &p = ctx.palette;
    const ui::Fonts &fonts = ctx.fonts;

    const float cx = a.x + a.w * 0.5f;
    const float room_w = a.w, room_h = 244.0f;
    const Rect room{a.x, a.top, room_w, room_h};

    // Living room floor boundary
    list.rounded_rect(room, 16.0f, kWhite.with_alpha(0.03f));
    list.bordered_rect(room, 16.0f, Color::rgb(0, 0), 1.0f, kWhite.with_alpha(0.08f));

    // TV on the front wall: a thin screen with a stand
    const Rect tv_rect{cx - 100.0f, room.y + 12.0f, 200.0f, 18.0f};
    list.rounded_rect(tv_rect, 5.0f, Color::rgb(0x2a3242));
    list.bordered_rect(tv_rect, 5.0f, Color::rgb(0, 0), 1.0f, Color::rgb(0x4a5772));
    ui::text(list, fonts.semibold, "TV", cx, tv_rect.cy() + 5.0f, 13, kWhite.with_alpha(0.9f), Align::center);

    // Sofa facing the TV, back of the sofa at the bottom
    const Rect sofa{cx - 80.0f, room.y + 150.0f, 160.0f, 64.0f};
    list.rounded_rect(sofa, 14.0f, Color::rgb(0x252c3b));
    list.bordered_rect(sofa, 14.0f, Color::rgb(0, 0), 1.0f, Color::rgb(0x3b4660));
    list.rounded_rect({sofa.x + 6.0f, sofa.y + sofa.h - 18.0f, sofa.w - 12.0f, 12.0f}, 6.0f, Color::rgb(0x303a4e));
    list.rounded_rect({sofa.x + 5.0f, sofa.y + 6.0f, 14.0f, sofa.h - 22.0f}, 6.0f, Color::rgb(0x303a4e));
    list.rounded_rect({sofa.x + sofa.w - 19.0f, sofa.y + 6.0f, 14.0f, sofa.h - 22.0f}, 6.0f, Color::rgb(0x303a4e));

    // Viewer seen from above: shoulders first, head on top, facing the TV
    const float px = cx, py = sofa.y + 36.0f;
    list.rounded_rect({px - 25.0f, py - 4.0f, 50.0f, 22.0f}, 11.0f, Color::rgb(0x6f7d99));
    list.circle(px, py - 6.0f, 12.0f, Color::rgb(0xe3e9f5));
    list.circle(px, py - 6.0f, 12.0f, Color::rgb(0xe3e9f5));
    list.rounded_rect({px - 4.0f, py - 20.0f, 8.0f, 5.0f}, 2.5f, Color::rgb(0xc7d0e2));  // nose: shows which way they face

    const float lx = px, ly = py - 6.0f;

    struct RoomSpeaker
    {
        const char *label;
        float x;
        float y;
        int min_layout; // 0 stereo, 1 5.1, 2 7.1
        bool is_sub;
        float label_dx;
        float label_dy;
        Align align;
    };

    const RoomSpeaker speakers[8] = {
        {"FL",  cx - 195.0f, room.y + 46.0f,  0, false,   0.0f,  30.0f, Align::center},
        {"FR",  cx + 195.0f, room.y + 46.0f,  0, false,   0.0f,  30.0f, Align::center},
        {"C",   cx,          room.y + 62.0f,  1, false,   0.0f,  30.0f, Align::center},
        {"SUB", cx - 105.0f, room.y + 64.0f,  1, true,    0.0f,  32.0f, Align::center},
        {"SL",  cx - 232.0f, room.y + 134.0f, 1, false,   0.0f,  30.0f, Align::center},
        {"SR",  cx + 232.0f, room.y + 134.0f, 1, false,   0.0f,  30.0f, Align::center},
        {"RL",  cx - 135.0f, room.y + 206.0f, 2, false,   0.0f, -20.0f, Align::center},
        {"RR",  cx + 135.0f, room.y + 206.0f, 2, false,   0.0f, -20.0f, Align::center},
    };

    for (const auto &spk : speakers)
    {
        /* layout 3 = 4.0 upmix: front pair plus the surround/rear set; no
         * centre or subwoofer, those get nothing from a stereo source. */
        const bool lit = layout == 3 ? (spk.label[0] != 'C' && !spk.is_sub) : (layout >= spk.min_layout);
        const float bw = spk.is_sub ? 34.0f : 28.0f;
        const float bh = spk.is_sub ? 34.0f : 28.0f;
        const Rect box_r{spk.x - bw * 0.5f, spk.y - bh * 0.5f, bw, bh};

        if (lit)
        {
            list.glow(box_r, 4.0f, 8.0f, p.accent.with_alpha(0.35f));

            for (int w = 0; w < 2; ++w)
            {
                const float pulse = std::fmod(ctx.time * 2.0f + static_cast<float>(w) * 0.5f, 1.0f);
                const float r = (spk.is_sub ? 16.0f : 14.0f) + pulse * 12.0f;
                const float alpha = (1.0f - pulse) * 0.75f + 0.15f;
                const Color wave_col = p.accent.with_alpha(alpha);

                if (spk.is_sub)
                {
                    list.ring(spk.x, spk.y, r, 1.5f, wave_col);
                }
                else
                {
                    const float ang = std::atan2(ly - spk.y, lx - spk.x);
                    float px0 = 0.0f, py0 = 0.0f;
                    for (int step = -3; step <= 3; ++step)
                    {
                        const float a = ang + static_cast<float>(step) * (3.14159265f / 14.0f);
                        const float ptx = spk.x + r * std::cos(a);
                        const float pty = spk.y + r * std::sin(a);
                        if (step > -3)
                            list.line(px0, py0, ptx, pty, 2.0f, wave_col);
                        px0 = ptx;
                        py0 = pty;
                    }
                }
            }
        }

        // Speaker box (rounded rect with cone circle)
        list.rounded_rect(box_r, 4.0f, lit ? p.accent.with_alpha(0.25f) : kWhite.with_alpha(0.05f));
        list.bordered_rect(box_r, 4.0f, Color::rgb(0, 0), 1.0f, lit ? p.accent : kWhite.with_alpha(0.18f));

        const float cone_r = spk.is_sub ? 11.0f : 8.5f;
        list.circle(spk.x, spk.y, cone_r, lit ? p.accent : kWhite.with_alpha(0.18f));
        list.circle(spk.x, spk.y, spk.is_sub ? 4.0f : 3.0f, lit ? kInk : Color::rgb(0x141822));

        ui::text(list, fonts.semibold, spk.label, spk.x + spk.label_dx, spk.y + spk.label_dy, 13,
                 lit ? p.accent : p.text_faint.with_alpha(0.55f), spk.align);
    }

    return room.y + room.h;
}

/* ---- 13. OUTPUT CHANNELS: listener and speaker layout. ---- */
void draw_channels(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);

    int layout = 1; // 0 stereo, 1 5.1, 2 7.1; AUTO plays what the file has
    if (!pv.choices.empty() && pv.index < static_cast<int>(pv.choices.size()))
    {
        const std::string u = ui::upper(pv.choices[pv.index]);
        if (u.find("7.1") != std::string::npos)
            layout = 2;
        else if (u.find("5.1") != std::string::npos)
            layout = 1;
        else if (u.find("STEREO") != std::string::npos || u.find("2.0") != std::string::npos)
            layout = 0;
    }

    const float room_bottom = draw_living_room_audio(list, ctx, a, layout);
    const auto labels = chip_labels(pv);
    const float y = chips(list, ctx, a, room_bottom + 20.0f, labels, pv.index);
    benefit(list, ctx, a, y + 24.0f, pv.benefit);
}

/* ---- 14. AUDIO PASSTHROUGH: decoded PCM vs bitstream flow diagram. ---- */
void draw_passthrough(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const float rw = a.w, rh = 68.0f;
    Rect row1{a.x, a.top, rw, rh};
    Rect row2{a.x, a.top + rh + 16.0f, rw, rh};
    const bool on = (pv.index == 1);

    // Row 1: OFF
    list.rounded_rect(row1, 12, kWhite.with_alpha(0.04f));
    frame_border(list, ctx, row1, !on);
    const Color col1 = (!on) ? p.accent : p.text_muted;
    const Color ink1 = (!on) ? kWhite : p.text_faint;

    Rect b1{row1.x + 16.0f, row1.cy() - 20.0f, 68.0f, 40.0f};
    list.rounded_rect(b1, 8, col1.with_alpha(!on ? 0.25f : 0.08f));
    list.bordered_rect(b1, 8, Color::rgb(0,0), 1.0f, col1);
    ui::text(list, fonts.semibold, "EVO", b1.cx(), b1.cy() + 6.0f, 16, ink1, Align::center);

    const float a1_start = b1.x + b1.w + 10.0f;
    const float a1_end = a1_start + 70.0f;
    list.line(a1_start, row1.cy(), a1_end, row1.cy(), 2.0f, col1);
    list.triangle({a1_end - 6.0f, row1.cy() - 6.0f, 8.0f, 12.0f}, col1, 0.0f, 1.5708f);
    ui::text(list, fonts.semibold, "DECODES", (a1_start + a1_end) * 0.5f, row1.cy() - 8.0f, 12, col1, Align::center);

    Rect b2{a1_end + 12.0f, row1.cy() - 20.0f, 95.0f, 40.0f};
    list.rounded_rect(b2, 8, col1.with_alpha(!on ? 0.25f : 0.08f));
    list.bordered_rect(b2, 8, Color::rgb(0,0), 1.0f, col1);
    ui::text(list, fonts.semibold, "PCM", b2.cx(), b2.cy() + 6.0f, 16, ink1, Align::center);

    const float a2_start = b2.x + b2.w + 10.0f;
    const float a2_end = a2_start + 50.0f;
    list.line(a2_start, row1.cy(), a2_end, row1.cy(), 2.0f, col1);
    list.triangle({a2_end - 6.0f, row1.cy() - 6.0f, 8.0f, 12.0f}, col1, 0.0f, 1.5708f);

    Rect b3{a2_end + 12.0f, row1.cy() - 20.0f, row1.x + row1.w - 16.0f - (a2_end + 12.0f), 40.0f};
    list.rounded_rect(b3, 8, col1.with_alpha(!on ? 0.25f : 0.08f));
    list.bordered_rect(b3, 8, Color::rgb(0,0), 1.0f, col1);
    ui::text(list, fonts.semibold, "RECEIVER", b3.cx(), b3.cy() + 6.0f, 16, ink1, Align::center);

    // Row 2: ON
    list.rounded_rect(row2, 12, kWhite.with_alpha(0.04f));
    frame_border(list, ctx, row2, on);
    const Color col2 = on ? p.accent : p.text_muted;
    const Color ink2 = on ? kWhite : p.text_faint;

    Rect r2_b1{row2.x + 16.0f, row2.cy() - 20.0f, 68.0f, 40.0f};
    list.rounded_rect(r2_b1, 8, col2.with_alpha(on ? 0.25f : 0.08f));
    list.bordered_rect(r2_b1, 8, Color::rgb(0,0), 1.0f, col2);
    ui::text(list, fonts.semibold, "EVO", r2_b1.cx(), r2_b1.cy() + 6.0f, 16, ink2, Align::center);

    const float r2_a_start = r2_b1.x + r2_b1.w + 14.0f;
    const float r2_a_end = row2.x + row2.w - 180.0f - 14.0f;
    list.line(r2_a_start, row2.cy(), r2_a_end, row2.cy(), 2.0f, col2);
    list.triangle({r2_a_end - 6.0f, row2.cy() - 6.0f, 8.0f, 12.0f}, col2, 0.0f, 1.5708f);
    ui::text(list, fonts.semibold, "BITSTREAM", (r2_a_start + r2_a_end) * 0.5f, row2.cy() - 8.0f, 12, col2, Align::center);

    Rect r2_b2{r2_a_end + 14.0f, row2.cy() - 20.0f, 166.0f, 40.0f};
    list.rounded_rect(r2_b2, 8, col2.with_alpha(on ? 0.25f : 0.08f));
    list.bordered_rect(r2_b2, 8, Color::rgb(0,0), 1.0f, col2);
    ui::text(list, fonts.semibold, "DOLBY / DTS", r2_b2.cx(), r2_b2.cy() + 6.0f, 16, ink2, Align::center);

    const auto labels = chip_labels(pv);
    const float y = chips(list, ctx, a, row2.y + row2.h + 28.0f, labels, pv.index);
    benefit(list, ctx, a, y + 40.0f, pv.benefit);
}

/* ---- 15. SURROUND TEST: 7.1 layout all lit with test action hint. ---- */
void draw_surround(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    // Surround test lights all 8 channels (7.1 layout)
    const float room_bottom = draw_living_room_audio(list, ctx, a, 2);

    const float line_y = room_bottom + 16.0f;
    ui::text(list, fonts.semibold, "Press X to play the test tones.", a.x + a.w * 0.5f, line_y, 18, p.text_muted, Align::center);
    benefit(list, ctx, a, line_y + 36.0f, pv.benefit);
}

/* ---- 16. RESUME PLAYBACK: 16:9 mock player frame with progress bar ---- */
void draw_resume(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const bool on = (pv.index == 1);
    const Palette &p = ctx.palette;

    const float fw = std::round(a.w * 0.72f);
    const float fh = std::round(fw * 9.0f / 16.0f);
    const float fx = a.x + (a.w - fw) * 0.5f;
    const Rect frame{fx, a.top, fw, fh};

    list.gradient_rect(frame, 0, Color::rgb(0x1e2536), Color::rgb(0x0c0e16));
    frame_border(list, ctx, frame, on);
    tag(list, ctx, frame, on ? "RESUME ON" : "FROM START", on);

    // Center play glyph: translucent circle with a right-pointing play triangle inside
    const float cx = frame.cx(), cy = frame.cy();
    list.circle(cx, cy, 28.0f, kWhite.with_alpha(0.12f));
    list.triangle({cx + 2.0f - 14.0f, cy - 12.0f, 28.0f, 24.0f}, on ? p.accent : kWhite, 0.0f, 1.5708f);

    // Progress bar under frame
    const float bar_y = frame.y + frame.h + 20.0f;
    const float bar_h = 8.0f;
    const Rect bar_bg{fx, bar_y, fw, bar_h};
    list.rounded_rect(bar_bg, 4.0f, kWhite.with_alpha(0.12f));

    if (on)
    {
        const float fill_w = fw * 0.42f;
        list.rounded_rect({fx, bar_y, fill_w, bar_h}, 4.0f, p.accent);
        const float marker_x = fx + fill_w;
        const float pulse = 6.0f + 1.0f * std::sin(ctx.time * 4.0f);
        list.glow({marker_x - 8.0f, bar_y + bar_h * 0.5f - 8.0f, 16.0f, 16.0f}, 8.0f, 6.0f, p.accent.with_alpha(0.5f));
        list.circle(marker_x, bar_y + bar_h * 0.5f, pulse, kWhite);
    }
    else
    {
        list.circle(fx + 4.0f, bar_y + bar_h * 0.5f, 5.0f, kWhite.with_alpha(0.4f));
    }

    // Caption
    const float cap_y = bar_y + bar_h + 20.0f;
    ui::text(list, ctx.fonts.semibold, on ? "Continues from 42:10" : "Starts from 0:00", fx, cap_y, 18, on ? p.accent : kWhite);

    const float y = chips(list, ctx, a, cap_y + 24.0f, chip_labels(pv), pv.index);
    benefit(list, ctx, a, y + 24.0f, pv.benefit);
}

/* ---- 17. VIDEO DECODER: CPU use and Heat meter bars + plain verdict ---- */
void draw_decoder(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const Palette &p = ctx.palette;

    // 0: Auto (Hardware preferred), 1: Hardware, 2: Software
    const int sel = std::clamp(pv.index, 0, 2);
    const bool is_sw = (sel == 2);

    // Animated meters: jitter slightly with time
    const float jitter = 0.015f * std::sin(ctx.time * 3.0f);
    const float cpu_pct = is_sw ? std::clamp(0.85f + jitter, 0.70f, 0.95f)
                                : std::clamp(0.16f + jitter, 0.10f, 0.25f);
    const float heat_pct = is_sw ? std::clamp(0.80f + 0.01f * std::cos(ctx.time * 2.0f), 0.70f, 0.90f)
                                 : std::clamp(0.20f + 0.01f * std::cos(ctx.time * 2.0f), 0.12f, 0.28f);

    const Rect panel{a.x, a.top, a.w, 136.0f};
    list.rounded_rect(panel, 12.0f, kWhite.with_alpha(0.04f));
    frame_border(list, ctx, panel, !is_sw);

    const Color good_col = p.accent;
    const Color heavy_col = Color::rgb(0xf87171); // warm red
    const Color bar_col = is_sw ? heavy_col : good_col;

    // Row 1: CPU use
    const float r1_y = panel.y + 16.0f;
    ui::text(list, ctx.fonts.semibold, "TYPICAL CPU USE", panel.x + 16.0f, r1_y + 12.0f, 14, p.text_muted, Align::left, 1.5f);
    char cpu_str[16];
    std::snprintf(cpu_str, sizeof(cpu_str), "%d %%", static_cast<int>(cpu_pct * 100.0f));
    ui::text(list, ctx.fonts.semibold, cpu_str, panel.x + panel.w - 16.0f, r1_y + 12.0f, 15, bar_col, Align::right);

    const float bw = panel.w - 32.0f;
    const Rect cpu_bg{panel.x + 16.0f, r1_y + 20.0f, bw, 8.0f};
    list.rounded_rect(cpu_bg, 4.0f, kWhite.with_alpha(0.10f));
    list.rounded_rect({cpu_bg.x, cpu_bg.y, bw * cpu_pct, 8.0f}, 4.0f, bar_col);

    // Row 2: Heat
    const float r2_y = r1_y + 44.0f;
    ui::text(list, ctx.fonts.semibold, "TYPICAL HEAT", panel.x + 16.0f, r2_y + 12.0f, 14, p.text_muted, Align::left, 1.5f);
    ui::text(list, ctx.fonts.semibold, is_sw ? "HIGH" : "LOW", panel.x + panel.w - 16.0f, r2_y + 12.0f, 15, bar_col, Align::right);

    const Rect heat_bg{panel.x + 16.0f, r2_y + 20.0f, bw, 8.0f};
    list.rounded_rect(heat_bg, 4.0f, kWhite.with_alpha(0.10f));
    list.rounded_rect({heat_bg.x, heat_bg.y, bw * heat_pct, 8.0f}, 4.0f, bar_col);

    // One-line plain verdict
    const float v_y = panel.y + panel.h + 16.0f;
    const Rect v_pill{a.x, v_y, a.w, 36.0f};
    list.rounded_rect(v_pill, 10.0f, bar_col.with_alpha(0.12f));
    list.bordered_rect(v_pill, 10.0f, Color::rgb(0, 0), 1.0f, bar_col);
    const char *verdict = is_sw ? "Uses the CPU. Only for files hardware can't play"
                          : (sel == 0) ? "Hardware when it can, software when it must"
                                       : "Uses the video chip. Light on the CPU";
    ui::text(list, ctx.fonts.semibold, ctx.fonts.semibold.font->fit(verdict, 16, v_pill.w - 24.0f), v_pill.cx(),
             v_pill.cy() + 6.0f, 16, bar_col, Align::center);
    ui::text(list, ctx.fonts.regular, "Meters show typical values, not live ones", a.x, v_pill.y + v_pill.h + 18.0f, 13,
             p.text_faint);

    // Clean choice chips: short names so chips never wrap or overflow
    static const std::vector<std::string> kDecoderLabels = {"AUTO", "HARDWARE", "SOFTWARE"};
    const float y = chips(list, ctx, a, v_y + 76.0f, kDecoderLabels, sel);
    benefit(list, ctx, a, y + 24.0f, pv.benefit);
}

/* ---- 18. NAVIGATION SOUNDS: speaker box+cone and pulsing D-pad ---- */
void draw_nav_sounds(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const bool on = (pv.index == 1);
    const Palette &p = ctx.palette;

    const Rect panel{a.x, a.top, a.w, 144.0f};
    list.rounded_rect(panel, 12.0f, kWhite.with_alpha(0.04f));
    frame_border(list, ctx, panel, on);

    // Speaker on the left: box + cone drawn with shapes
    const float spk_cx = panel.x + 110.0f, spk_cy = panel.cy();
    const Color spk_col = on ? p.accent : p.text_muted;

    // Speaker box
    list.rounded_rect({spk_cx - 28.0f, spk_cy - 12.0f, 16.0f, 24.0f}, 3.0f, spk_col);
    // Speaker cone: two triangles forming the expanding trapezoid
    const float cone_t1[6] = {
        spk_cx - 12.0f, spk_cy - 12.0f,
        spk_cx + 10.0f, spk_cy - 22.0f,
        spk_cx + 10.0f, spk_cy + 22.0f
    };
    const float cone_t2[6] = {
        spk_cx - 12.0f, spk_cy - 12.0f,
        spk_cx + 10.0f, spk_cy + 22.0f,
        spk_cx - 12.0f, spk_cy + 12.0f
    };
    list.polygon(cone_t1, 3, spk_col);
    list.polygon(cone_t2, 3, spk_col);

    if (on)
    {
        // Sound-wave arcs pulse beside the speaker
        for (int w = 0; w < 3; ++w)
        {
            const float r = 16.0f + static_cast<float>(w) * 11.0f;
            const float pulse = std::fmod(ctx.time * 2.5f + static_cast<float>(w) * 0.33f, 1.0f);
            const float alpha = 0.25f + 0.65f * (1.0f - pulse);
            const Color wave_col = p.accent.with_alpha(alpha);
            float px0 = 0.0f, py0 = 0.0f;
            for (int step = -4; step <= 4; ++step)
            {
                const float angle = static_cast<float>(step) * (3.14159265f / 16.0f);
                const float px = spk_cx + 14.0f + r * std::cos(angle);
                const float py = spk_cy + r * std::sin(angle);
                if (step > -4)
                    list.line(px0, py0, px, py, 2.5f, wave_col);
                px0 = px;
                py0 = py;
            }
        }
    }
    else
    {
        // Red slash across speaker
        list.line(spk_cx - 36.0f, spk_cy + 26.0f, spk_cx + 20.0f, spk_cy - 26.0f, 3.0f, Color::rgb(0xf87171));
    }

    // Mini D-pad on the right
    const float dpad_cx = panel.x + panel.w - 110.0f, dpad_cy = panel.cy();
    list.rounded_rect({dpad_cx - 9.0f, dpad_cy - 9.0f, 18.0f, 18.0f}, 4.0f, Color::rgb(0x222632));

    const Rect arms[4] = {
        {dpad_cx - 8.0f, dpad_cy - 28.0f, 16.0f, 19.0f}, // 0: Up
        {dpad_cx + 9.0f, dpad_cy - 8.0f, 19.0f, 16.0f},  // 1: Right
        {dpad_cx - 8.0f, dpad_cy + 9.0f, 16.0f, 19.0f},  // 2: Down
        {dpad_cx - 28.0f, dpad_cy - 8.0f, 19.0f, 16.0f}  // 3: Left
    };

    const int active_arm = on ? (static_cast<int>(ctx.time * 3.0f) % 4) : -1;
    for (int i = 0; i < 4; ++i)
    {
        const bool lit = (i == active_arm);
        const Color arm_col = lit ? p.accent : Color::rgb(0x222632);
        list.rounded_rect(arms[i], 3.0f, arm_col);
        if (lit)
            list.glow(arms[i], 3.0f, 8.0f, p.accent.with_alpha(0.5f));
    }

    // Caption
    const float cap_y = panel.y + panel.h + 18.0f;
    ui::text(list, ctx.fonts.semibold, on ? "Click when you move" : "Silent", a.x, cap_y, 18, on ? p.accent : p.text_muted);

    const float y = chips(list, ctx, a, cap_y + 24.0f, chip_labels(pv), pv.index);
    benefit(list, ctx, a, y + 24.0f, pv.benefit);
}

/* ---- 19. FOLDERS FIRST: 6-row mock USB browser list ---- */
void draw_folders_first(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const bool on = (pv.index == 1);
    const Palette &p = ctx.palette;

    struct Item { const char *name; bool is_folder; const char *tag; };

    // ON: folders first, then files
    static const Item kItemsOn[6] = {
        {"Action Movies", true, "DIR"},
        {"Cartoons", true, "DIR"},
        {"Subtitles", true, "DIR"},
        {"Holiday.mp4", false, "MP4"},
        {"Sample.mkv", false, "MKV"},
        {"Trailer.mp4", false, "MP4"}
    };

    // OFF: mixed alphabetical order
    static const Item kItemsOff[6] = {
        {"Action Movies", true, "DIR"},
        {"Cartoons", true, "DIR"},
        {"Holiday.mp4", false, "MP4"},
        {"Sample.mkv", false, "MKV"},
        {"Subtitles", true, "DIR"},
        {"Trailer.mp4", false, "MP4"}
    };

    const Item *items = on ? kItemsOn : kItemsOff;

    const float row_h = 28.0f;
    const Rect panel{a.x, a.top, a.w, 6.0f * row_h + 16.0f};
    list.rounded_rect(panel, 12.0f, kWhite.with_alpha(0.04f));
    frame_border(list, ctx, panel, on);

    for (int i = 0; i < 6; ++i)
    {
        const float ry = panel.y + 8.0f + static_cast<float>(i) * row_h;
        const float ix = panel.x + 14.0f, iy = ry + 6.0f;

        if (items[i].is_folder)
        {
            // Folder icon (simple shapes): amber tab + body
            list.rounded_rect({ix, iy, 6.0f, 3.0f}, 1.0f, Color::rgb(0xf59e0b));
            list.rounded_rect({ix, iy + 2.0f, 16.0f, 11.0f}, 2.0f, Color::rgb(0xf59e0b));
            ui::text(list, ctx.fonts.semibold, items[i].name, ix + 24.0f, ry + 18.0f, 14, kWhite);
            ui::text(list, ctx.fonts.regular, items[i].tag, panel.x + panel.w - 14.0f, ry + 18.0f, 11, p.accent, Align::right);
        }
        else
        {
            // File icon (simple shapes): slate paper rect with folded corner
            list.rounded_rect({ix + 1.0f, iy, 13.0f, 14.0f}, 1.5f, p.text_faint);
            list.triangle({ix + 9.0f, iy, 5.0f, 5.0f}, Color::rgb(0x131722));
            ui::text(list, ctx.fonts.regular, items[i].name, ix + 24.0f, ry + 18.0f, 14, p.text_muted);
            ui::text(list, ctx.fonts.regular, items[i].tag, panel.x + panel.w - 14.0f, ry + 18.0f, 11, p.text_faint, Align::right);
        }

        // Faint row separator
        if (i < 5)
            list.line(panel.x + 12.0f, ry + row_h, panel.x + panel.w - 12.0f, ry + row_h, 1.0f, kWhite.with_alpha(0.04f));
    }

    const float y = chips(list, ctx, a, panel.y + panel.h + 20.0f, chip_labels(pv), pv.index);
    benefit(list, ctx, a, y + 24.0f, pv.benefit);
}

/* ---- 20. KEYBOARD INPUT: EVO virtual keyboard vs native PS5 IME ---- */
void draw_keyboard(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const Palette &p = ctx.palette;
    const int sel = (pv.index == 1) ? 1 : 0;

    const float gap = 14.0f;
    const float pw = (a.w - gap) * 0.5f;
    const float ph = 168.0f;
    const Rect p_left{a.x, a.top, pw, ph};
    const Rect p_right{a.x + pw + gap, a.top, pw, ph};

    // --- Left Panel: EVO Keyboard ---
    const bool sel_l = (sel == 0);
    list.rounded_rect(p_left, 12.0f, kWhite.with_alpha(sel_l ? 0.06f : 0.02f));
    frame_border(list, ctx, p_left, sel_l);
    if (sel_l)
        list.glow(p_left, 12.0f, 6.0f, p.accent.with_alpha(0.35f));

    ui::text(list, ctx.fonts.semibold, "EVO KEYBOARD", p_left.cx(), p_left.y + 24.0f, 14,
             sel_l ? p.accent : p.text_faint, Align::center);

    // 3 rows of drawn key caps
    const float kw = (pw - 36.0f) / 6.0f;
    const float kh = 20.0f;
    for (int r = 0; r < 3; ++r)
    {
        const float ky = p_left.y + 44.0f + static_cast<float>(r) * 26.0f;
        const int cols = (r == 2) ? 5 : 6;
        const float rx = p_left.x + (pw - (static_cast<float>(cols) * (kw + 3.0f) - 3.0f)) * 0.5f;
        for (int c = 0; c < cols; ++c)
        {
            const Rect kr{rx + static_cast<float>(c) * (kw + 3.0f), ky, kw, kh};
            const bool highlight_key = sel_l && (r == 1 && c == 2);
            list.rounded_rect(kr, 4.0f, highlight_key ? p.accent : Color::rgb(0x282f3d, sel_l ? 1.0f : 0.4f));
        }
    }
    // Spacebar at bottom
    const Rect space_r{p_left.cx() - 40.0f, p_left.y + 128.0f, 80.0f, 16.0f};
    list.rounded_rect(space_r, 4.0f, Color::rgb(0x282f3d, sel_l ? 1.0f : 0.4f));

    if (!sel_l)
        list.rounded_rect(p_left, 12.0f, Color::rgb(0x000000, 0.45f));

    // --- Right Panel: PS5 System Keyboard ---
    const bool sel_r = (sel == 1);
    list.rounded_rect(p_right, 12.0f, kWhite.with_alpha(sel_r ? 0.06f : 0.02f));
    frame_border(list, ctx, p_right, sel_r);
    if (sel_r)
        list.glow(p_right, 12.0f, 6.0f, p.accent.with_alpha(0.35f));

    ui::text(list, ctx.fonts.semibold, "PS5 KEYBOARD", p_right.cx(), p_right.y + 24.0f, 14,
             sel_r ? p.accent : p.text_faint, Align::center);

    // Text field at upper area
    const Rect tf{p_right.x + 16.0f, p_right.y + 40.0f, pw - 32.0f, 24.0f};
    list.rounded_rect(tf, 4.0f, Color::rgb(0x181e2a, sel_r ? 1.0f : 0.4f));
    list.bordered_rect(tf, 4.0f, Color::rgb(0, 0), 1.0f, sel_r ? p.accent.with_alpha(0.6f) : p.border);
    ui::text(list, ctx.fonts.regular, "Search...", tf.x + 8.0f, tf.y + 16.0f, 12, sel_r ? p.text_muted : p.text_faint);

    // System keyboard bar at the bottom with a row of keys
    const Rect bar{p_right.x + 12.0f, p_right.y + 80.0f, pw - 24.0f, 72.0f};
    list.rounded_rect(bar, 6.0f, Color::rgb(0x161922, sel_r ? 1.0f : 0.4f));
    list.bordered_rect(bar, 6.0f, Color::rgb(0, 0), 1.0f, Color::rgb(0xffffff, sel_r ? 0.12f : 0.04f));

    // Keys in the PS5 bar
    const float pkw = (bar.w - 24.0f) / 6.0f;
    for (int r = 0; r < 2; ++r)
    {
        const float bky = bar.y + 10.0f + static_cast<float>(r) * 26.0f;
        for (int c = 0; c < 6; ++c)
        {
            const Rect bkr{bar.x + 12.0f + static_cast<float>(c) * pkw, bky, pkw - 3.0f, 20.0f};
            list.rounded_rect(bkr, 3.0f, Color::rgb(0x232734, sel_r ? 1.0f : 0.4f));
        }
    }

    if (!sel_r)
        list.rounded_rect(p_right, 12.0f, Color::rgb(0x000000, 0.45f));

    // Choice chips
    static const std::vector<std::string> kKbLabels = {"VIRTUAL KEYBOARD", "NATIVE PS5 IME"};
    const float y = chips(list, ctx, a, a.top + ph + 24.0f, kKbLabels, sel);
    benefit(list, ctx, a, y + 24.0f, pv.benefit);
}

void draw_qr_tile(DrawList &list, float x0, float y0, float qs, const SettingsPreview &pv);

/* ---- PHONE & LOG PORT: the phone link as it will read, and whether the port took ----
 * pv.index = 0 free, 1 busy (old port kept), 2 fallback in use.
 * pv.choices[0] = the link, pv.choices[1] = the status line. */
void draw_port(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const Palette &p = ctx.palette;
    const std::string link = pv.choices.size() > 0 ? pv.choices[0] : "";
    const std::string status = pv.choices.size() > 1 ? pv.choices[1] : "";
    const Color ok = Color::rgb(0x3ddc84);
    const Color warn = Color::rgb(0xffb02e);
    const Color bad = Color::rgb(0xff5a5f);
    const Color tone = (pv.index == 0) ? ok : (pv.index == 3) ? bad : warn;

    // The same button glyphs the footer uses, not words.
    auto hint_row = [&](float cy) {
        const ui::Hint hints[] = {{ui::Button::cross, "Change"}, {ui::Button::triangle, "Reset to 9780"}};
        ui::HintLayout layout;
        layout.size = 30;
        layout.text_size = 20;
        layout.cy = cy;
        ui::draw_hints(list, ctx.fonts, ui::GlyphStyle::dark(), hints, 2, a.x, false, layout);
    };

    if (pv.index == 3)
    {
        // Every port in the range is taken: no QR, and a guided way out.
        list.rounded_rect({a.x, a.top, 12.0f, 12.0f}, 6.0f, bad);
        ui::text(list, ctx.fonts.semibold, "NO FREE PORT", a.x + 24.0f, a.top + 13.0f, 20, bad, Align::left, 2.0f);
        ui::paragraph(list, ctx.fonts.regular, status, a.x, a.top + 52.0f, 20, a.w, 28, kWhite, 3);
        static const char *const kSteps[] = {"Select this row", "Type a port from 1024 to 65535",
                                             "EVO checks it, then the phone link is back"};
        float sy = a.top + 150.0f;
        for (int i = 0; i < 3; ++i)
        {
            list.rounded_rect({a.x, sy - 20.0f, 30.0f, 30.0f}, 15.0f, p.accent);
            ui::text(list, ctx.fonts.semibold, std::to_string(i + 1), a.x + 15.0f, sy + 1.0f, 18, Color::rgb(0x10131a), Align::center);
            ui::text(list, ctx.fonts.regular, kSteps[i], a.x + 46.0f, sy + 1.0f, 20, kWhite);
            sy += 46.0f;
        }
        hint_row(sy + 40.0f);
        return;
    }

    const float qs = 176.0f;
    draw_qr_tile(list, a.x, a.top, qs, pv);

    // To the right: what to do, the link, then the status of the port.
    const float tx = a.x + qs + 24.0f;
    const float tw = a.w - qs - 24.0f;
    ui::text(list, ctx.fonts.semibold, "SCAN WITH YOUR PHONE", tx, a.top + 22.0f, 14, p.text_faint, Align::left, 2.0f);
    ui::text(list, ctx.fonts.semibold, ctx.fonts.semibold.font->fit(link, 18, tw), tx, a.top + 62.0f, 18, kWhite);
    list.rounded_rect({tx, a.top + 108.0f, 12.0f, 12.0f}, 6.0f, tone);
    ui::paragraph(list, ctx.fonts.regular, status, tx + 22.0f, a.top + 120.0f, 18, tw - 22.0f, 24, tone, 2);

    hint_row(a.top + qs + 36.0f);
    benefit(list, ctx, a, a.top + qs + 96.0f, pv.benefit);
}

/* ---- REPORT A PROBLEM (#229): the report page's QR, what to do, and Cross to save ----
 * pv.index = 0 the phone page is up, 1 no link (no port or no network).
 * pv.choices[0] = the link, pv.choices[1] = the status line. */
void draw_report(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const Palette &p = ctx.palette;
    const std::string link = pv.choices.size() > 0 ? pv.choices[0] : "";
    const std::string status = pv.choices.size() > 1 ? pv.choices[1] : "";
    const Color tone = pv.index == 0 ? Color::rgb(0x3ddc84) : Color::rgb(0xffb02e);

    const float qs = 176.0f;
    float ty = a.top;
    if (pv.index == 0)
    {
        draw_qr_tile(list, a.x, a.top, qs, pv);
        const float tx = a.x + qs + 24.0f;
        const float tw = a.w - qs - 24.0f;
        ui::text(list, ctx.fonts.semibold, "SCAN WITH YOUR PHONE", tx, a.top + 22.0f, 14, p.text_faint, Align::left, 2.0f);
        ui::text(list, ctx.fonts.semibold, ctx.fonts.semibold.font->fit(link, 18, tw), tx, a.top + 62.0f, 18, kWhite);
        ui::paragraph(list, ctx.fonts.regular, "Opens a page to download or share EVO's log. No USB stick needed.",
                      tx, a.top + 104.0f, 18, tw, 24, p.text_faint, 3);
        ty = a.top + qs + 20.0f;
    }
    list.rounded_rect({a.x, ty + 4.0f, 12.0f, 12.0f}, 6.0f, tone);
    ui::paragraph(list, ctx.fonts.regular, status, a.x + 22.0f, ty + 16.0f, 18, a.w - 22.0f, 24, tone, 2);

    const ui::Hint hints[] = {{ui::Button::cross, "Save report to USB"}};
    ui::HintLayout layout;
    layout.size = 30;
    layout.text_size = 20;
    layout.cy = ty + 76.0f;
    ui::draw_hints(list, ctx.fonts, ui::GlyphStyle::dark(), hints, 1, a.x, false, layout);
    benefit(list, ctx, a, ty + 136.0f, pv.benefit);
}

/* A QR tile: the real code when pv carries one, otherwise a QR-shaped stand-in
 * (three finder squares and a fixed scatter) for the host preview. */
void draw_qr_tile(DrawList &list, float x0, float y0, float qs, const SettingsPreview &pv)
{
    const Rect qr{x0, y0, qs, qs};
    list.rounded_rect(qr, 12.0f, Color::rgb(0xffffff));
    const bool real = pv.qr_size > 0;
    const int n = real ? pv.qr_size : 25;
    const float m = (qs - 24.0f) / static_cast<float>(n);
    std::uint32_t seed = 9780u;
    for (int y = 0; y < n; ++y)
    {
        for (int x = 0; x < n; ++x)
        {
            seed = seed * 1664525u + 1013904223u;
            const bool left = x < 7, right = x >= n - 7, top = y < 7, bottom = y >= n - 7;
            bool on = ((seed >> 16) & 3u) != 0u ? ((seed >> 20) & 1u) != 0u : false;
            if (real)
                on = pv.qr[static_cast<size_t>(y) * n + x] != 0;
            else if ((left && top) || (right && top) || (left && bottom))
            {
                const int fx = left ? x : x - (n - 7);
                const int fy = top ? y : y - (n - 7);
                const bool ring = fx == 0 || fx == 6 || fy == 0 || fy == 6;
                const bool core = fx >= 2 && fx <= 4 && fy >= 2 && fy <= 4;
                on = ring || core;
            }
            else if ((left && y == 7) || (top && x == 7) || (right && y == 7) || (bottom && x == 7)
                     || (x == n - 8 && top) || (y == n - 8 && left))
            {
                on = false;
            }
            if (on)
                list.rounded_rect({qr.x + 12.0f + static_cast<float>(x) * m, qr.y + 12.0f + static_cast<float>(y) * m, m, m},
                                  0.0f, Color::rgb(0x10131a));
        }
    }
}

/* ---- 21. ASK WHICH LIVE STREAM: channel tile to quality dialog vs direct play ---- */
void draw_ask_stream(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const bool on = (pv.index == 1);
    const Palette &p = ctx.palette;

    // Channel tile on the left
    const float tile_w = 130.0f;
    const float tile_h = 130.0f;
    const Rect tile{a.x, a.top + 10.0f, tile_w, tile_h};
    list.rounded_rect(tile, 14.0f, Color::rgb(0x181c28));
    list.bordered_rect(tile, 14.0f, Color::rgb(0, 0), 1.0f, p.border);

    // Red "LIVE" pill
    const Rect live_pill{tile.x + 10.0f, tile.y + 10.0f, 40.0f, 18.0f};
    list.rounded_rect(live_pill, 4.0f, Color::rgb(0xef4444));
    ui::text(list, ctx.fonts.semibold, "LIVE", live_pill.cx(), live_pill.cy() + 4.5f, 10, kWhite, Align::center);

    // Play glyph
    const float tcx = tile.cx(), tcy = tile.cy() + 4.0f;
    list.circle(tcx, tcy, 18.0f, kWhite.with_alpha(0.12f));
    list.triangle({tcx - 8.0f, tcy - 8.0f, 20.0f, 16.0f}, p.accent, 0.0f, 1.5708f);
    ui::text(list, ctx.fonts.semibold, "Sports 1", tcx, tile.y + tile_h - 14.0f, 12, p.text_muted, Align::center);

    // Arrow to target
    const float arrow_x1 = tile.x + tile_w + 10.0f;
    const float arrow_x2 = a.x + 190.0f;
    const float arrow_y = tile.cy();
    list.line(arrow_x1, arrow_y, arrow_x2, arrow_y, 2.0f, on ? p.accent : p.text_muted);
    list.triangle({arrow_x2 - 8.0f, arrow_y - 6.0f, 8.0f, 12.0f}, on ? p.accent : p.text_muted, 0.0f, 1.5708f);

    const float target_x = arrow_x2 + 8.0f;
    const float target_w = a.x + a.w - target_x;

    if (on)
    {
        // Dialog mock "Choose quality" with 3 rows
        const Rect dialog{target_x, a.top, target_w, 150.0f};
        list.rounded_rect(dialog, 12.0f, Color::rgb(0x1a1e2a));
        frame_border(list, ctx, dialog, true);

        ui::text(list, ctx.fonts.semibold, "Choose quality", dialog.x + 14.0f, dialog.y + 22.0f, 15, kWhite);

        static const char *const kQualities[3] = {"Auto", "1080p", "720p"};
        for (int r = 0; r < 3; ++r)
        {
            const float qy = dialog.y + 36.0f + static_cast<float>(r) * 34.0f;
            const Rect qr{dialog.x + 10.0f, qy, dialog.w - 20.0f, 28.0f};
            const bool sel_q = (r == 0);
            list.rounded_rect(qr, 6.0f, sel_q ? p.accent.with_alpha(0.20f) : kWhite.with_alpha(0.03f));
            if (sel_q)
                list.bordered_rect(qr, 6.0f, Color::rgb(0, 0), 1.0f, p.accent);
            ui::text(list, ctx.fonts.semibold, kQualities[r], qr.x + 12.0f, qr.y + 18.0f, 13,
                     sel_q ? p.accent : p.text_muted);
        }
    }
    else
    {
        // Mock player frame labelled "Plays right away"
        const Rect player{target_x, a.top + 10.0f, target_w, 130.0f};
        list.gradient_rect(player, 0, Color::rgb(0x161a26), Color::rgb(0x0a0c12));
        frame_border(list, ctx, player, false);

        list.circle(player.cx(), player.cy() - 10.0f, 20.0f, kWhite.with_alpha(0.12f));
        list.triangle({player.cx() - 6.0f, player.cy() - 18.0f, 16.0f, 16.0f}, kWhite, 0.0f, 1.5708f);
        ui::text(list, ctx.fonts.semibold, "Plays right away", player.cx(), player.y + player.h - 16.0f, 15,
                 p.accent, Align::center);
    }

    const float y = chips(list, ctx, a, a.top + 170.0f, chip_labels(pv), pv.index);
    benefit(list, ctx, a, y + 24.0f, pv.benefit);
}

/* ---- 22. DEBUG OVERLAY: mock video frame with stats HUD in top-left corner ---- */
void draw_debug_overlay(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const bool on = (pv.index == 1);
    const Palette &p = ctx.palette;

    const float fw = a.w;
    const float fh = std::round(fw * 9.0f / 16.0f);
    const Rect frame{a.x, a.top, fw, fh};

    mock_scene(list, frame, 0.2f, false);
    frame_border(list, ctx, frame, on);
    tag_right(list, ctx, frame, on ? "HUD ON" : "HUD OFF", on);

    if (on)
    {
        // Translucent dark stats box in the top-left corner
        const Rect stats{frame.x + 12.0f, frame.y + 12.0f, 136.0f, 96.0f};
        list.rounded_rect(stats, 6.0f, Color::rgb(0x000000, 0.82f));
        list.bordered_rect(stats, 6.0f, Color::rgb(0, 0), 1.0f, Color::rgb(0xffffff, 0.18f));

        char buf_fps[32], buf_buf[32], buf_mem[32];
        const float fps = 24.0f + 0.2f * std::sin(ctx.time * 4.0f);
        const float buf_sec = 1.2f + 0.1f * std::cos(ctx.time * 2.5f);
        const int mem = 61 + static_cast<int>(std::sin(ctx.time * 1.5f) * 1.5f);
        std::snprintf(buf_fps, sizeof(buf_fps), "FPS %.1f", fps);
        std::snprintf(buf_buf, sizeof(buf_buf), "BUFFER %.1f s", buf_sec);
        std::snprintf(buf_mem, sizeof(buf_mem), "MEMORY %d %%", mem);

        const float sy = stats.y + 18.0f;
        ui::text(list, ctx.fonts.semibold, buf_fps, stats.x + 10.0f, sy, 13, p.accent);
        ui::text(list, ctx.fonts.regular, "DECODE HW", stats.x + 10.0f, sy + 20.0f, 13, kWhite);
        ui::text(list, ctx.fonts.regular, buf_buf, stats.x + 10.0f, sy + 40.0f, 13, kWhite);
        ui::text(list, ctx.fonts.regular, buf_mem, stats.x + 10.0f, sy + 60.0f, 13, kWhite);
    }

    const float y = chips(list, ctx, a, frame.y + frame.h + 20.0f, chip_labels(pv), pv.index);
    benefit(list, ctx, a, y + 24.0f, pv.benefit);
}

} // namespace

/* Same as benefit(); kept so the newer drawers read the same. */
void benefit_fit(DrawList &list, const Context &ctx, const Area &a, float y, const std::string &text)
{
    benefit(list, ctx, a, y, text);
}

/* A one-line caption centred under a picture. */
void caption(DrawList &list, const Context &ctx, const Area &a, float y, const std::string &text, bool accent)
{
    ui::text(list, ctx.fonts.semibold, ctx.fonts.semibold.font->fit(text, 18, a.w), a.x + a.w * 0.5f, y, 18,
             accent ? ctx.palette.accent : ctx.palette.text_muted, Align::center);
}

/* A small labelled box for flow pictures; lit = the active path. */
Rect flow_box(DrawList &list, const Context &ctx, const Rect &r, const std::string &label, bool lit, int size = 16)
{
    const Color col = lit ? ctx.palette.accent : ctx.palette.text_muted;
    list.rounded_rect(r, 10, col.with_alpha(lit ? 0.22f : 0.06f));
    list.bordered_rect(r, 10, Color::rgb(0, 0), lit ? 2.0f : 1.0f, lit ? col : kWhite.with_alpha(0.16f));
    ui::text(list, ctx.fonts.semibold, ctx.fonts.semibold.font->fit(label, static_cast<float>(size), r.w - 12.0f), r.cx(),
             r.cy() + size * 0.36f, static_cast<float>(size), lit ? kWhite : ctx.palette.text_faint, Align::center);
    return r;
}

/* A horizontal arrow from x0 to x1; dashes travel along it when lit. */
void flow_arrow(DrawList &list, const Context &ctx, float x0, float x1, float y, bool lit)
{
    const Color col = lit ? ctx.palette.accent : kWhite.with_alpha(0.18f);
    list.line(x0, y, x1 - 6.0f, y, 2.0f, col);
    list.triangle({x1 - 8.0f, y - 6.0f, 8.0f, 12.0f}, col, 0.0f, 1.5708f);
    if (lit)
    {
        const float t = std::fmod(ctx.time * 0.8f, 1.0f);
        list.circle(x0 + (x1 - x0 - 10.0f) * t, y, 4.0f, kWhite);
    }
}

/* ---- UPMIX STEREO TO 4.0: the room with the front pair, then the rear lit. ---- */
void draw_upmix(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const bool on = pv.index == 1;
    const float room_bottom = draw_living_room_audio(list, ctx, a, on ? 3 : 0);
    caption(list, ctx, a, room_bottom + 30.0f,
            on ? "4.0: the ambience fills the rear, voices stay in front" : "Stereo: only the front pair plays", on);
    const float y = chips(list, ctx, a, room_bottom + 54.0f, chip_labels(pv), pv.index);
    benefit_fit(list, ctx, a, y + 24.0f, pv.benefit);
}

/* A DualSense seen from above: body, grips, touchpad, the speaker grille in the middle. */
void dualsense(DrawList &list, const Context &ctx, float cx, float cy, bool lit)
{
    const Color body = Color::rgb(0xe9edf5), dark = Color::rgb(0x1b2130);
    /* Sound fans out of the top, behind the body (arc angle 0 = straight up). */
    if (lit)
        for (int w = 0; w < 3; ++w)
        {
            const float pulse = std::fmod(ctx.time * 1.2f + w / 3.0f, 1.0f);
            list.arc(cx, cy + 19.0f, 60.0f + pulse * 48.0f, 3.0f, -0.75f, 1.5f, ctx.palette.accent.with_alpha(1.0f - pulse));
        }
    list.rounded_rect({cx - 92.0f, cy - 34.0f, 184.0f, 64.0f}, 30.0f, body);
    list.rounded_rect({cx - 96.0f, cy - 10.0f, 56.0f, 70.0f}, 28.0f, body);
    list.rounded_rect({cx + 40.0f, cy - 10.0f, 56.0f, 70.0f}, 28.0f, body);
    list.rounded_rect({cx - 44.0f, cy - 32.0f, 88.0f, 38.0f}, 10.0f, Color::rgb(0xd6dbe6));
    list.circle(cx - 60.0f, cy + 4.0f, 9.0f, dark);
    list.circle(cx + 60.0f, cy + 4.0f, 9.0f, dark);
    list.circle(cx - 30.0f, cy + 26.0f, 11.0f, dark);
    list.circle(cx + 30.0f, cy + 26.0f, 11.0f, dark);
    for (int gy = 0; gy < 2; ++gy)
        for (int gx = 0; gx < 5; ++gx)
            list.circle(cx - 12.0f + gx * 6.0f, cy + 16.0f + gy * 6.0f, 1.8f, lit ? ctx.palette.accent : Color::rgb(0x9aa3b5));
}

/* ---- CONTROLLER PHONE AUDIO: a film's phone line plays on the controller. ---- */
void draw_phone_audio(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const bool on = pv.index == 1;
    const ui::Fonts &fonts = ctx.fonts;
    const float fw = a.w, fh = fw * 9.0f / 16.0f * 0.52f;
    Rect frame{a.x, a.top, fw, fh};
    mock_scene(list, frame, 0.6f, false);
    frame_border(list, ctx, frame, false);
    /* The SDH subtitle that marks the call. */
    const std::string line = "[ON PHONE] Where are you right now?";
    const float lw = fonts.semibold.measure(line, 20) + 28.0f;
    Rect sub{frame.cx() - lw * 0.5f, frame.y + frame.h - 50.0f, lw, 36.0f};
    list.rounded_rect(sub, 8, Color::rgb(0x000000, 0.72f));
    ui::text(list, fonts.semibold, line, sub.cx(), sub.cy() + 7.0f, 20, kWhite, Align::center);
    tag(list, ctx, frame, "SUBTITLES ON", false);

    const float cy = frame.y + frame.h + 100.0f;
    dualsense(list, ctx, a.x + a.w * 0.5f, cy, on);
    caption(list, ctx, a, cy + 84.0f, on ? "The caller's voice plays on the controller" : "Off: the call plays on the TV with the rest", on);
    const float y = chips(list, ctx, a, cy + 100.0f, chip_labels(pv), pv.index);
    benefit_fit(list, ctx, a, y + 24.0f, pv.benefit);
}

/* A phone outline with a little video tile and the cast list over it. */
void phone_with_cast_list(DrawList &list, const Context &ctx, const Rect &ph, bool found)
{
    const ui::Fonts &fonts = ctx.fonts;
    list.rounded_rect(ph, 22, Color::rgb(0x11151f));
    list.bordered_rect(ph, 22, Color::rgb(0, 0), 2.0f, kWhite.with_alpha(0.35f));
    Rect video{ph.x + 10.0f, ph.y + 26.0f, ph.w - 20.0f, (ph.w - 20.0f) * 9.0f / 16.0f};
    mock_scene(list, video, 0.3f, false);
    Rect sheet{ph.x + 8.0f, video.y + video.h + 16.0f, ph.w - 16.0f, ph.y + ph.h - 10.0f - (video.y + video.h + 16.0f)};
    list.rounded_rect(sheet, 12, Color::rgb(0x232a3a));
    ui::text(list, fonts.semibold, "Cast to", sheet.x + 12.0f, sheet.y + 26.0f, 14, ctx.palette.text_muted);
    Rect item{sheet.x + 8.0f, sheet.y + 38.0f, sheet.w - 16.0f, 34.0f};
    if (found)
    {
        list.rounded_rect(item, 8, ctx.palette.accent.with_alpha(0.25f));
        list.bordered_rect(item, 8, Color::rgb(0, 0), 1.5f, ctx.palette.accent);
        ui::text(list, fonts.semibold, "EVO Player", item.x + 12.0f, item.cy() + 6.0f, 16, kWhite);
    }
    else
        ui::text(list, fonts.regular, "No devices", item.x + 12.0f, item.cy() + 6.0f, 15, ctx.palette.text_faint);
}

/* A TV with EVO's mark on the screen (or dark when nothing is coming). */
void tv_with_evo(DrawList &list, const Context &ctx, const Rect &tv, bool lit, const std::string &label)
{
    list.rounded_rect(tv, 10, Color::rgb(0x0d1018));
    list.bordered_rect(tv, 10, Color::rgb(0, 0), 2.0f, lit ? ctx.palette.accent : kWhite.with_alpha(0.25f));
    list.rounded_rect({tv.cx() - 30.0f, tv.y + tv.h, 60.0f, 8.0f}, 3, Color::rgb(0x2a3140));
    if (lit)
    {
        list.circle(tv.cx(), tv.cy() - 10.0f, 22.0f, ctx.palette.accent.with_alpha(0.25f));
        list.triangle({tv.cx() - 7.0f, tv.cy() - 22.0f, 20.0f, 24.0f}, ctx.palette.accent, 0.0f, 1.5708f);
    }
    ui::text(list, ctx.fonts.semibold, label, tv.cx(), tv.y + tv.h - 16.0f, 15, lit ? kWhite : ctx.palette.text_faint, Align::center);
}

/* ---- PHONE CASTING (DIAL): the YouTube app's cast list finds EVO. ---- */
void draw_phone_cast(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const bool on = pv.index == 1;
    Rect ph{a.x + 10.0f, a.top, 150.0f, 268.0f};
    phone_with_cast_list(list, ctx, ph, on);
    Rect tv{a.x + a.w - 250.0f, a.top + 70.0f, 250.0f, 150.0f};
    tv_with_evo(list, ctx, tv, on, on ? "EVO PLAYER" : "NOT LISTED");
    /* the cast hop: dotted arc from the phone's list to the TV */
    const float x0 = ph.x + ph.w + 14.0f, x1 = tv.x - 14.0f, y = tv.cy();
    for (float x = x0; x < x1; x += 14.0f)
    {
        const float t = (x - x0) / (x1 - x0);
        const float yy = y - std::sin(t * 3.14159265f) * 40.0f;
        const bool dot_lit = on && std::fmod(ctx.time * 0.9f, 1.0f) >= t - 0.12f && std::fmod(ctx.time * 0.9f, 1.0f) <= t + 0.12f;
        list.circle(x, yy, dot_lit ? 4.5f : 3.0f, on ? (dot_lit ? kWhite : ctx.palette.accent) : kWhite.with_alpha(0.15f));
    }
    caption(list, ctx, a, ph.y + ph.h + 34.0f, on ? "EVO shows up in the YouTube app's cast list" : "Off: phones on your network cannot see EVO", on);
    const float yc = chips(list, ctx, a, ph.y + ph.h + 58.0f, chip_labels(pv), pv.index);
    benefit_fit(list, ctx, a, yc + 24.0f, pv.benefit);
}

/* ---- CAST TO EVO PLAYER: EVO's own player vs the system browser. ---- */
void draw_cast_player(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const bool on = pv.index == 1;
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    struct Lane { bool lit; const char *dest; const char *what; float fill; const char *len; };
    const Lane lanes[2] = {{on, "EVO PLAYER", "1080p60  ·  whole video", 1.0f, "10:34"},
                           {!on, "WEB BROWSER", "720p  ·  YouTube TV page", 1.0f, "10:34"}};
    float y = a.top;
    for (const Lane &l : lanes)
    {
        Rect row{a.x, y, a.w, 104.0f};
        list.rounded_rect(row, 12, kWhite.with_alpha(0.04f));
        frame_border(list, ctx, row, l.lit);
        Rect phone{row.x + 16.0f, row.cy() - 30.0f, 36.0f, 60.0f};
        list.rounded_rect(phone, 8, Color::rgb(0x11151f));
        list.bordered_rect(phone, 8, Color::rgb(0, 0), 1.5f, l.lit ? p.accent : kWhite.with_alpha(0.25f));
        flow_arrow(list, ctx, phone.x + phone.w + 10.0f, phone.x + phone.w + 64.0f, row.cy(), l.lit);
        Rect dest{phone.x + phone.w + 74.0f, row.y + 18.0f, row.x + row.w - 16.0f - (phone.x + phone.w + 74.0f), 68.0f};
        list.rounded_rect(dest, 10, l.lit ? p.accent.with_alpha(0.16f) : kWhite.with_alpha(0.03f));
        ui::text(list, fonts.semibold, l.dest, dest.x + 14.0f, dest.y + 26.0f, 18, l.lit ? kWhite : p.text_faint);
        ui::text(list, fonts.regular, l.what, dest.x + 14.0f, dest.y + 50.0f, 15, l.lit ? p.text_muted : p.text_faint);
        y += 104.0f + 14.0f;
    }
    caption(list, ctx, a, y + 16.0f, on ? "On: casts play in EVO, controls as usual" : "Off: casts open in the PS5 browser", true);
    const float yc = chips(list, ctx, a, y + 40.0f, chip_labels(pv), pv.index);
    benefit_fit(list, ctx, a, yc + 24.0f, pv.benefit);
}

/* ---- PLAY A YOUTUBE LINK: a pasted link becomes the whole video in EVO. ---- */
void draw_yt_link(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    /* the link field, with a blinking caret */
    Rect field{a.x, a.top, a.w, 52.0f};
    list.rounded_rect(field, 10, kWhite.with_alpha(0.06f));
    list.bordered_rect(field, 10, Color::rgb(0, 0), 1.5f, p.accent);
    const std::string url = "https://youtu.be/aqz-KE-bpKQ";
    ui::text(list, fonts.mono, url, field.x + 16.0f, field.cy() + 7.0f, 19, kWhite);
    if (std::fmod(ctx.time, 1.0f) < 0.55f)
    {
        const float cx = field.x + 16.0f + fonts.mono.measure(url, 19) + 3.0f;
        list.rounded_rect({cx, field.cy() - 12.0f, 2.0f, 24.0f}, 1, kWhite);
    }
    /* down to the player */
    const float ax = a.x + a.w * 0.5f, ay0 = field.y + field.h + 8.0f, ay1 = ay0 + 34.0f;
    list.line(ax, ay0, ax, ay1 - 6.0f, 2.0f, p.accent);
    list.triangle({ax - 6.0f, ay1 - 8.0f, 12.0f, 8.0f}, p.accent, 0.0f, 3.14159265f);
    /* the player frame: playing the whole video */
    const float fw = a.w, fh = fw * 9.0f / 16.0f * 0.78f;
    Rect frame{a.x, ay1 + 6.0f, fw, fh};
    mock_scene(list, frame, 0.4f, false);
    frame_border(list, ctx, frame, true);
    tag(list, ctx, frame, "1080p60", true);
    tag_right(list, ctx, frame, "WHOLE VIDEO", false);
    const float t = 0.15f + 0.8f * std::fmod(ctx.time * 0.06f, 1.0f);
    Rect bar{frame.x + 16.0f, frame.y + frame.h - 34.0f, frame.w - 32.0f, 6.0f};
    list.rounded_rect(bar, 3, kWhite.with_alpha(0.3f));
    list.rounded_rect({bar.x, bar.y, bar.w * t, bar.h}, 3, p.accent);
    ui::text(list, fonts.mono, "10:34", bar.x + bar.w, bar.y - 8.0f, 15, kWhite, Align::right);
    caption(list, ctx, a, frame.y + frame.h + 32.0f, "Or paste it on your phone: <ps5>:9780/input", false);
}

/* ---- SKIP INTROS & CREDITS: 16:9 mock scene, progress bar, intro stretch, skip jump / button ---- */
static void draw_skip_segments(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const int sel = std::clamp(pv.index, 0, 2); // 0: OFF, 1: ASK, 2: AUTO

    const float fw = 360.0f, fh = fw * 9.0f / 16.0f * 0.76f;
    const float fx = a.x + (a.w - fw) * 0.5f;
    Rect frame{fx, a.top, fw, fh};
    mock_scene(list, frame, 0.4f, false);
    frame_border(list, ctx, frame, sel > 0);
    tag(list, ctx, frame, sel == 2 ? "AUTO SKIP" : (sel == 1 ? "ASK TO SKIP" : "NO SKIP"), sel > 0);

    /* ASK: "Skip intro" pill button in the bottom right of the frame */
    if (sel == 1)
    {
        const std::string btn = "Skip intro";
        const float bw = fonts.semibold.measure(btn, 14) + 24.0f;
        Rect pill{frame.x + frame.w - bw - 10.0f, frame.y + frame.h - 38.0f, bw, 28.0f};
        list.rounded_rect(pill, 14.0f, p.accent);
        ui::text(list, fonts.semibold, btn, pill.cx(), pill.cy() + 5.0f, 14, kInk, Align::center);
    }

    /* Progress bar with intro stretch marked in accent */
    const float bar_y = frame.y + frame.h + 16.0f, bar_h = 8.0f;
    Rect bar_bg{fx, bar_y, fw, bar_h};
    list.rounded_rect(bar_bg, 4.0f, kWhite.with_alpha(0.12f));

    const float intro_x0 = fx + fw * 0.20f;
    const float intro_x1 = fx + fw * 0.50f;
    list.rounded_rect({intro_x0, bar_y, intro_x1 - intro_x0, bar_h}, 4.0f, p.accent.with_alpha(0.70f));
    ui::text(list, fonts.semibold, "INTRO", (intro_x0 + intro_x1) * 0.5f, bar_y - 6.0f, 11, p.accent, Align::center);

    /* AUTO: arrow jumping over the intro stretch */
    if (sel == 2)
    {
        const float ax0 = intro_x0 - 4.0f, ax1 = intro_x1 + 14.0f;
        flow_arrow(list, ctx, ax0, ax1, bar_y + bar_h + 16.0f, true);
        list.circle(ax1, bar_y + bar_h * 0.5f, 5.0f, kWhite);
    }
    else
    {
        list.circle(fx + fw * 0.28f, bar_y + bar_h * 0.5f, 5.0f, kWhite);
    }

    const char *cap = (sel == 2) ? "Auto: jumps over intros without asking"
                    : (sel == 1) ? "Ask: button appears during intro, Triangle skips"
                                 : "Off: intros play straight through";
    const float cap_y = bar_y + bar_h + (sel == 2 ? 32.0f : 24.0f);
    caption(list, ctx, a, cap_y, cap, sel > 0);
    const float y = chips(list, ctx, a, cap_y + 24.0f, chip_labels(pv), pv.index);
    benefit_fit(list, ctx, a, y + 20.0f, pv.benefit);
}

/* ---- HDR PEAK: vertical brightness scale beside bright HDR frame ---- */
static void draw_hdr_peak(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const int sel = std::clamp(pv.index, 0, 5); // 0: Off, 1: 800, 2: 1000, 3: 1500, 4: 2000, 5: 3000
    const bool off = (sel == 0);

    /* Bright HDR frame on the left */
    const float fw = 280.0f, fh = 150.0f;
    Rect frame{a.x, a.top, fw, fh};
    mock_scene(list, frame, 0.0f, false, true);
    frame_border(list, ctx, frame, !off);
    tag(list, ctx, frame, off ? "PASSTHROUGH" : "TONE-MAPPED", !off);

    /* Vertical brightness scale (0 to 3000 nits) on the right */
    const float sx = a.x + 360.0f, sy_top = a.top + 10.0f, sy_bot = a.top + 140.0f;
    list.line(sx, sy_top, sx, sy_bot, 2.0f, kWhite.with_alpha(0.25f));

    struct Tick { int nits; float frac; const char *label; };
    const Tick ticks[6] = {
        {3000, 0.00f, "3000 nits"},
        {2000, 0.22f, "2000"},
        {1500, 0.40f, "1500"},
        {1000, 0.60f, "1000"},
        {800,  0.74f, "800"},
        {0,    1.00f, "0"}
    };

    float marker_y = sy_bot;
    for (const Tick &t : ticks)
    {
        const float ty = sy_top + (sy_bot - sy_top) * t.frac;
        list.line(sx - 4.0f, ty, sx + 4.0f, ty, 1.0f, kWhite.with_alpha(0.35f));
        ui::text(list, fonts.mono, t.label, sx + 10.0f, ty + 4.0f, 12, p.text_muted);
    }

    if (!off)
    {
        const float fracs[6] = {1.0f, 0.74f, 0.60f, 0.40f, 0.22f, 0.00f};
        marker_y = sy_top + (sy_bot - sy_top) * fracs[sel];
        list.triangle({sx - 14.0f, marker_y - 5.0f, 10.0f, 10.0f}, p.accent, 0.0f, 1.5708f);
        list.circle(sx, marker_y, 4.0f, kWhite);
    }
    else
    {
        list.circle(sx, sy_bot, 4.0f, p.text_faint);
    }

    const char *cap = off ? "Your TV picks the brightness" : "HDR fitted to target peak brightness";
    const float cap_y = frame.y + frame.h + 24.0f;
    caption(list, ctx, a, cap_y, cap, !off);
    const float y = chips(list, ctx, a, cap_y + 24.0f, chip_labels(pv), pv.index);
    benefit_fit(list, ctx, a, y + 20.0f, pv.benefit);
}

/* ---- AUDIO LANGUAGE: track list card with default track highlighted ---- */
static void draw_audio_language(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    const float cw = 380.0f, ch = 142.0f;
    const float cx = a.x + (a.w - cw) * 0.5f;
    Rect card{cx, a.top, cw, ch};
    list.rounded_rect(card, 12.0f, kWhite.with_alpha(0.04f));
    frame_border(list, ctx, card, false);

    ui::text(list, fonts.semibold, "AUDIO TRACKS", card.x + 16.0f, card.y + 22.0f, 13, p.text_muted, Align::left, 1.5f);

    /* Determine which row to highlight based on chosen language (AUTO highlights the first) */
    int highlight = 0;
    std::string sel = (!pv.choices.empty() && pv.index >= 0 && pv.index < static_cast<int>(pv.choices.size()))
                          ? ui::upper(pv.choices[pv.index]) : "";
    if (sel.find("JAP") != std::string::npos) highlight = 1;
    else if (sel.find("FRAN") != std::string::npos || sel.find("FREN") != std::string::npos) highlight = 2;
    else highlight = 0;

    const char *rows[3] = {"English 5.1 (Surround)", "Japanese 2.0 (Stereo)", "French 2.0 (Stereo)"};
    for (int i = 0; i < 3; ++i)
    {
        const bool lit = (i == highlight);
        Rect r{card.x + 12.0f, card.y + 34.0f + i * 34.0f, card.w - 24.0f, 28.0f};
        list.rounded_rect(r, 6.0f, lit ? p.accent.with_alpha(0.20f) : kWhite.with_alpha(0.02f));
        if (lit) list.bordered_rect(r, 6.0f, Color::rgb(0, 0), 1.0f, p.accent);
        ui::text(list, fonts.semibold, rows[i], r.x + 12.0f, r.cy() + 5.0f, 14, lit ? kWhite : p.text_faint);
        if (lit)
            ui::text(list, fonts.semibold, "START", r.x + r.w - 12.0f, r.cy() + 5.0f, 12, p.accent, Align::right);
    }

    const float cap_y = card.y + card.h + 24.0f;
    caption(list, ctx, a, cap_y, "A film starts with this track", true);
    const float y = chips(list, ctx, a, cap_y + 24.0f, chip_labels(pv), pv.index);
    benefit_fit(list, ctx, a, y + 20.0f, pv.benefit);
}

/* ---- SUBTITLE LANGUAGE: track list card + ASCII subtitle sample ---- */
static void draw_sub_language(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    const float cw = 380.0f, ch = 138.0f;
    const float cx = a.x + (a.w - cw) * 0.5f;
    Rect card{cx, a.top, cw, ch};
    list.rounded_rect(card, 12.0f, kWhite.with_alpha(0.04f));
    frame_border(list, ctx, card, false);

    ui::text(list, fonts.semibold, "SUBTITLES", card.x + 16.0f, card.y + 22.0f, 13, p.text_muted, Align::left, 1.5f);

    int highlight = 0;
    std::string sel = (!pv.choices.empty() && pv.index >= 0 && pv.index < static_cast<int>(pv.choices.size()))
                          ? ui::upper(pv.choices[pv.index]) : "";
    if (sel.find("SPA") != std::string::npos) highlight = 2;
    else if (sel.find("SDH") != std::string::npos) highlight = 1;
    else highlight = 0;

    const char *rows[3] = {"English (Dialogue)", "English (SDH)", "Spanish (Dialogo)"};
    for (int i = 0; i < 3; ++i)
    {
        const bool lit = (i == highlight);
        Rect r{card.x + 12.0f, card.y + 34.0f + i * 32.0f, card.w - 24.0f, 26.0f};
        list.rounded_rect(r, 6.0f, lit ? p.accent.with_alpha(0.20f) : kWhite.with_alpha(0.02f));
        if (lit) list.bordered_rect(r, 6.0f, Color::rgb(0, 0), 1.0f, p.accent);
        ui::text(list, fonts.semibold, rows[i], r.x + 12.0f, r.cy() + 5.0f, 14, lit ? kWhite : p.text_faint);
        if (lit)
            ui::text(list, fonts.semibold, "ACTIVE", r.x + r.w - 12.0f, r.cy() + 5.0f, 12, p.accent, Align::right);
    }

    /* Subtitle line drawn under it in the chosen language (plain ASCII only) */
    const char *sample = (highlight == 2) ? "Buenos dias, amigo."
                       : (highlight == 1) ? "[ambient music playing softly]"
                                          : "Hello, how are you?";
    const float sub_y = card.y + card.h + 12.0f;
    Rect sub_box{cx + 20.0f, sub_y, cw - 40.0f, 30.0f};
    list.rounded_rect(sub_box, 6.0f, Color::rgb(0x000000, 0.70f));
    ui::text(list, fonts.semibold, sample, sub_box.cx(), sub_box.cy() + 5.0f, 14, kWhite, Align::center);

    const float cap_y = sub_box.y + sub_box.h + 20.0f;
    caption(list, ctx, a, cap_y, "Preferred subtitle track selected automatically", true);
    const float y = chips(list, ctx, a, cap_y + 24.0f, chip_labels(pv), pv.index);
    benefit_fit(list, ctx, a, y + 20.0f, pv.benefit);
}

/* ---- VOLUME BOOST: two waveform strips (quiet original vs boosted clipped) ---- */
static void draw_volume_boost(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const int sel = std::clamp(pv.index, 0, 4); // 0: OFF, 1: +50%, 2: +100%, 3: +200%, 4: +300%
    const float mults[5] = {1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
    const float mult = mults[sel];

    static const float kOriginalHeights[40] = {
        6, 10, 14, 8, 12, 16, 20, 14, 8, 12,
        15, 18, 10, 6, 14, 18, 20, 16, 12, 8,
        14, 18, 16, 12, 6, 10, 15, 20, 14, 8,
        12, 16, 18, 14, 10, 15, 12, 8, 14, 6
    };

    const float pw = 420.0f, px = a.x + (a.w - pw) * 0.5f;
    Rect row_orig{px, a.top, pw, 54.0f};
    list.rounded_rect(row_orig, 8.0f, kWhite.with_alpha(0.04f));
    ui::text(list, fonts.semibold, "ORIGINAL", row_orig.x + 10.0f, row_orig.y + 16.0f, 11, p.text_muted);

    const float bar_w = 4.0f, gap = 4.0f;
    const float start_x = row_orig.x + 80.0f;
    for (int i = 0; i < 40; ++i)
    {
        const float bh = kOriginalHeights[i];
        const float bx = start_x + i * (bar_w + gap);
        list.rounded_rect({bx, row_orig.cy() - bh * 0.5f, bar_w, bh}, 2.0f, p.text_muted);
    }

    Rect row_boost{px, a.top + 62.0f, pw, 54.0f};
    list.rounded_rect(row_boost, 8.0f, kWhite.with_alpha(0.04f));
    frame_border(list, ctx, row_boost, sel > 0);
    ui::text(list, fonts.semibold, "BOOSTED", row_boost.x + 10.0f, row_boost.y + 16.0f, 11, sel > 0 ? p.accent : p.text_muted);

    const float clip_limit = 44.0f;
    list.line(start_x, row_boost.cy() - clip_limit * 0.5f, start_x + 40 * (bar_w + gap), row_boost.cy() - clip_limit * 0.5f, 1.0f,
              Color::rgb(0xf87171, 0.60f));
    list.line(start_x, row_boost.cy() + clip_limit * 0.5f, start_x + 40 * (bar_w + gap), row_boost.cy() + clip_limit * 0.5f, 1.0f,
              Color::rgb(0xf87171, 0.60f));

    for (int i = 0; i < 40; ++i)
    {
        float bh = kOriginalHeights[i] * mult;
        if (bh > clip_limit) bh = clip_limit; // clipped at the limit, never wrap
        const float bx = start_x + i * (bar_w + gap);
        list.rounded_rect({bx, row_boost.cy() - bh * 0.5f, bar_w, bh}, 2.0f, sel > 0 ? p.accent : p.text_muted);
    }

    const char *cap_pcts[5] = {"Volume boost: Off", "Boosted by +50%", "Boosted by +100%", "Boosted by +200%", "Boosted by +300%"};
    const float cap_y = row_boost.y + row_boost.h + 22.0f;
    caption(list, ctx, a, cap_y, cap_pcts[sel], sel > 0);
    const float y = chips(list, ctx, a, cap_y + 24.0f, chip_labels(pv), pv.index);
    benefit_fit(list, ctx, a, y + 20.0f, pv.benefit);
}

/* ---- AUDIO DELAY: picture flash lane above shifted sound spike lane ---- */
static void draw_audio_delay(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    /* Parse or calculate delay in ms: range -500 to +500 ms */
    int ms = 0;
    if (!pv.choices.empty() && pv.index >= 0 && pv.index < static_cast<int>(pv.choices.size()))
        std::sscanf(pv.choices[pv.index].c_str(), "%d", &ms);
    else if (pv.index >= 0 && pv.index <= 20)
        ms = (pv.index - 10) * 50;

    const float lw = 420.0f, lx = a.x + (a.w - lw) * 0.5f;

    /* Picture lane with white clapperboard flash at centre */
    Rect r_pic{lx, a.top, lw, 52.0f};
    list.rounded_rect(r_pic, 8.0f, kWhite.with_alpha(0.04f));
    ui::text(list, fonts.semibold, "PICTURE", r_pic.x + 12.0f, r_pic.cy() + 5.0f, 13, p.text_muted);
    Rect flash{r_pic.cx() - 14.0f, r_pic.cy() - 14.0f, 28.0f, 28.0f};
    list.rounded_rect(flash, 4.0f, kWhite);
    list.glow(flash, 4.0f, 6.0f, kWhite.with_alpha(0.6f));
    ui::text(list, fonts.mono, "FLASH", flash.cx(), r_pic.y + r_pic.h - 6.0f, 10, p.text_faint, Align::center);

    /* Centre reference line across lanes */
    list.line(r_pic.cx(), r_pic.y + 4.0f, r_pic.cx(), a.top + 120.0f, 1.0f, kWhite.with_alpha(0.20f));

    /* Sound lane with waveform spike shifted proportional to delay */
    Rect r_snd{lx, a.top + 64.0f, lw, 52.0f};
    list.rounded_rect(r_snd, 8.0f, kWhite.with_alpha(0.04f));
    ui::text(list, fonts.semibold, "SOUND", r_snd.x + 12.0f, r_snd.cy() + 5.0f, 13, p.text_muted);

    const float shift_px = std::clamp((static_cast<float>(ms) / 500.0f) * 110.0f, -110.0f, 110.0f);
    const float spike_cx = r_snd.cx() + shift_px;

    const float spike_h[7] = {6.0f, 12.0f, 22.0f, 38.0f, 22.0f, 12.0f, 6.0f};
    for (int i = 0; i < 7; ++i)
    {
        const float bx = spike_cx + (i - 3) * 5.0f;
        const float sh = spike_h[i];
        list.rounded_rect({bx - 1.5f, r_snd.cy() - sh * 0.5f, 3.0f, sh}, 1.5f, ms != 0 ? p.accent : kWhite);
    }

    char cap[64];
    if (ms == 0) std::snprintf(cap, sizeof(cap), "Sound in sync with picture");
    else if (ms > 0) std::snprintf(cap, sizeof(cap), "Sound %d ms later", ms);
    else std::snprintf(cap, sizeof(cap), "Sound %d ms earlier", -ms);

    const float cap_y = r_snd.y + r_snd.h + 22.0f;
    caption(list, ctx, a, cap_y, cap, ms != 0);

    /* Show clean key choice chips so 21 items do not overflow */
    static const std::vector<std::string> kDelayChips = {"-200 MS", "-100 MS", "0 MS", "+100 MS", "+200 MS"};
    int delay_sel = 2; // default 0 ms
    if (ms <= -200) delay_sel = 0;
    else if (ms < 0) delay_sel = 1;
    else if (ms == 0) delay_sel = 2;
    else if (ms < 200) delay_sel = 3;
    else delay_sel = 4;

    const float y = chips(list, ctx, a, cap_y + 24.0f, kDelayChips, delay_sel);
    benefit_fit(list, ctx, a, y + 20.0f, pv.benefit);
}

/* ---- NETWORK BUFFER: timeline with played stretch, loaded ahead buffer and drop gap ---- */
static void draw_network_buffer(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const int sel = std::clamp(pv.index, 0, 3); // 0: 30 SEC, 1: 1 MIN, 2: 2 MIN, 3: 5 MIN

    const float pw = 420.0f, ph = 130.0f;
    const float px = a.x + (a.w - pw) * 0.5f;
    Rect panel{px, a.top, pw, ph};
    list.rounded_rect(panel, 12.0f, kWhite.with_alpha(0.04f));
    frame_border(list, ctx, panel, false);

    ui::text(list, fonts.semibold, "PLAYBACK TIMELINE", panel.x + 16.0f, panel.y + 22.0f, 13, p.text_muted, Align::left, 1.5f);

    const float bar_w = panel.w - 32.0f;
    const float bar_y = panel.y + 44.0f, bar_h = 14.0f;
    Rect bar_bg{panel.x + 16.0f, bar_y, bar_w, bar_h};
    list.rounded_rect(bar_bg, 7.0f, kWhite.with_alpha(0.12f));

    /* Played part in accent */
    const float played_w = bar_w * 0.22f;
    list.rounded_rect({bar_bg.x, bar_y, played_w, bar_h}, 7.0f, p.accent);
    list.circle(bar_bg.x + played_w, bar_bg.cy(), 8.0f, kWhite);

    /* Lighter loaded ahead stretch whose length grows with choice */
    const float ahead_fracs[4] = {0.15f, 0.32f, 0.52f, 0.72f};
    const float ahead_w = bar_w * ahead_fracs[sel];
    list.rounded_rect({bar_bg.x + played_w, bar_y, ahead_w, bar_h}, 7.0f, p.accent.with_alpha(0.38f));

    /* Wi-Fi drop gap drawn on the timeline */
    const float drop_x = bar_bg.x + played_w + bar_w * 0.35f;
    const float drop_w = 40.0f;
    Rect drop_box{drop_x, bar_y - 12.0f, drop_w, bar_h + 24.0f};
    list.rounded_rect(drop_box, 4.0f, Color::rgb(0xf87171, 0.18f));
    list.bordered_rect(drop_box, 4.0f, Color::rgb(0, 0), 1.0f, Color::rgb(0xf87171, 0.70f));
    ui::text(list, fonts.semibold, "DROP", drop_box.cx(), drop_box.y - 4.0f, 10, Color::rgb(0xf87171), Align::center);

    const bool covers = (bar_bg.x + played_w + ahead_w >= drop_x + drop_w);
    ui::text(list, fonts.semibold, covers ? "Buffer rides out the drop" : "Buffer runs out: playback stalls",
             panel.cx(), panel.y + panel.h - 14.0f, 13, covers ? p.accent : Color::rgb(0xf87171), Align::center);

    const char *lens[4] = {"30 sec", "1 min", "2 min", "5 min"};
    char cap[48];
    std::snprintf(cap, sizeof(cap), "Loads %s ahead", lens[sel]);
    const float cap_y = panel.y + panel.h + 22.0f;
    caption(list, ctx, a, cap_y, cap, true);
    const float y = chips(list, ctx, a, cap_y + 24.0f, chip_labels(pv), pv.index);
    benefit_fit(list, ctx, a, y + 20.0f, pv.benefit);
}

/* ---- DEVELOPER TOOLS: terminal-like panel with live stats lines ---- */
static void draw_dev_tools(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    const float pw = 420.0f, ph = 154.0f;
    const float px = a.x + (a.w - pw) * 0.5f;
    Rect term{px, a.top, pw, ph};
    list.rounded_rect(term, 10.0f, Color::rgb(0x0a0d14));
    frame_border(list, ctx, term, false);

    /* Terminal title bar with 3 dots */
    list.rounded_rect({term.x, term.y, term.w, 24.0f}, 10.0f, Color::rgb(0x141824));
    list.circle(term.x + 14.0f, term.y + 12.0f, 3.5f, Color::rgb(0xed6a5e));
    list.circle(term.x + 25.0f, term.y + 12.0f, 3.5f, Color::rgb(0xf4bf4f));
    list.circle(term.x + 36.0f, term.y + 12.0f, 3.5f, Color::rgb(0x61c554));
    ui::text(list, fonts.semibold, "DEVELOPER OVERLAY", term.cx(), term.y + 16.0f, 11, p.text_muted, Align::center);

    const char *lines[5] = {
        "decoder   hardware  4K HEVC",
        "fps       60",
        "memory    app 751 MB",
        "network   12.4 MB/s",
        "pipeline  sceAgc  present"
    };

    float ty = term.y + 44.0f;
    for (int i = 0; i < 5; ++i)
    {
        ui::text(list, fonts.mono, lines[i], term.x + 18.0f, ty, 13, (i < 2) ? p.accent : kWhite);
        ty += 20.0f;
    }

    const float cap_y = term.y + term.h + 24.0f;
    caption(list, ctx, a, cap_y, "Live numbers for testing", true);
    benefit_fit(list, ctx, a, cap_y + 36.0f, pv.benefit);
}

/* ---- CONSOLE MODEL: PS5 console silhouette with standard/pro badges ---- */
static void draw_console(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const Palette &p = ctx.palette;

    const float cx = a.x + a.w * 0.5f;
    const float cy = a.top + 72.0f;

    /* PS5 silhouette: dark core with white curved side panels */
    Rect core{cx - 16.0f, cy - 65.0f, 32.0f, 130.0f};
    list.rounded_rect(core, 6.0f, Color::rgb(0x0a0c12));
    list.line(core.x + 2.0f, core.y + 12.0f, core.x + 2.0f, core.y + core.h - 12.0f, 1.5f, p.accent);

    /* Left and right white side panels */
    Rect left_panel{core.x - 20.0f, core.y - 4.0f, 22.0f, 138.0f};
    Rect right_panel{core.x + core.w - 2.0f, core.y - 4.0f, 22.0f, 138.0f};
    list.rounded_rect(left_panel, 10.0f, Color::rgb(0xedf1fa));
    list.rounded_rect(right_panel, 10.0f, Color::rgb(0xedf1fa));

    /* Console stand / base */
    Rect base{cx - 42.0f, core.y + core.h - 2.0f, 84.0f, 8.0f};
    list.rounded_rect(base, 4.0f, Color::rgb(0x181c26));

    /* PS5 and PS5 PRO badges on sides */
    Rect b_std{cx - 150.0f, cy - 18.0f, 90.0f, 36.0f};
    Rect b_pro{cx + 60.0f, cy - 18.0f, 90.0f, 36.0f};
    flow_box(list, ctx, b_std, "PS5", false, 15);
    flow_box(list, ctx, b_pro, "PS5 PRO", false, 15);

    const float cap_y = base.y + 36.0f;
    caption(list, ctx, a, cap_y, "The model EVO detected", false);
    benefit_fit(list, ctx, a, cap_y + 36.0f, pv.benefit);
}

/* ---- MEMORY MODE: horizontal bar split into Video buffers & App ---- */
static void draw_memory(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    const float pw = 420.0f, ph = 130.0f;
    const float px = a.x + (a.w - pw) * 0.5f;
    Rect panel{px, a.top, pw, ph};
    list.rounded_rect(panel, 12.0f, kWhite.with_alpha(0.04f));
    frame_border(list, ctx, panel, false);

    ui::text(list, fonts.semibold, "DIRECT MEMORY POOL", panel.x + 16.0f, panel.y + 22.0f, 13, p.text_muted, Align::left, 1.5f);
    ui::text(list, fonts.mono, "12.9 GIB", panel.x + panel.w - 16.0f, panel.y + 22.0f, 13, kWhite, Align::right);

    const float bar_w = panel.w - 32.0f;
    const float bar_y = panel.y + 44.0f, bar_h = 24.0f;
    const float vid_w = bar_w * 0.68f;
    const float app_w = bar_w - vid_w;

    Rect r_vid{panel.x + 16.0f, bar_y, vid_w, bar_h};
    Rect r_app{panel.x + 16.0f + vid_w + 3.0f, bar_y, app_w - 3.0f, bar_h};
    list.rounded_rect(r_vid, 6.0f, p.accent);
    list.rounded_rect(r_app, 6.0f, p.text_muted.with_alpha(0.35f));

    ui::text(list, fonts.semibold, "VIDEO BUFFERS (8.8 GiB)", r_vid.x + 10.0f, r_vid.cy() + 5.0f, 12, kInk);
    ui::text(list, fonts.semibold, "APP (4.1 GiB)", r_app.cx(), r_app.cy() + 5.0f, 12, kWhite, Align::center);

    ui::text(list, fonts.regular, "4K video queues, AI upscaler networks & stream cache",
             panel.cx(), panel.y + panel.h - 16.0f, 13, p.text_faint, Align::center);

    const float cap_y = panel.y + panel.h + 24.0f;
    caption(list, ctx, a, cap_y, "More memory for 4K and big buffers on PS5 Pro", true);
    benefit_fit(list, ctx, a, cap_y + 36.0f, pv.benefit);
}

/* ---- SOFTWARE UPDATE: version progression with download progress bar ---- */
static void draw_update(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    const float cw = 420.0f, ch = 140.0f;
    const float cx = a.x + (a.w - cw) * 0.5f;
    Rect card{cx, a.top, cw, ch};
    list.rounded_rect(card, 12.0f, kWhite.with_alpha(0.04f));
    frame_border(list, ctx, card, true);

    /* "v0.13.6" -> "newer version" */
    Rect b_cur{card.x + 18.0f, card.y + 18.0f, 110.0f, 36.0f};
    flow_box(list, ctx, b_cur, "v0.13.6", false, 15);

    flow_arrow(list, ctx, b_cur.x + b_cur.w + 12.0f, card.x + card.w - 150.0f, b_cur.cy(), true);

    Rect b_new{card.x + card.w - 138.0f, card.y + 18.0f, 120.0f, 36.0f};
    flow_box(list, ctx, b_new, "v0.14.0", true, 15);

    /* Progress bar animating with ctx.time */
    const float t = std::fmod(ctx.time * 0.20f, 1.0f);
    const float pct = 0.15f + 0.85f * t;
    const float bar_y = card.y + 76.0f, bar_h = 8.0f;
    const float bar_w = card.w - 36.0f;
    Rect bar_bg{card.x + 18.0f, bar_y, bar_w, bar_h};
    list.rounded_rect(bar_bg, 4.0f, kWhite.with_alpha(0.12f));
    list.rounded_rect({bar_bg.x, bar_bg.y, bar_w * pct, bar_h}, 4.0f, p.accent);

    char pct_str[32];
    std::snprintf(pct_str, sizeof(pct_str), "Downloading update: %d %%", static_cast<int>(pct * 100.0f));
    ui::text(list, fonts.semibold, pct_str, bar_bg.x, card.y + card.h - 16.0f, 13, p.accent);
    ui::text(list, fonts.mono, "SHA-256 ok", card.x + card.w - 18.0f, card.y + card.h - 16.0f, 12, p.text_muted, Align::right);

    const float cap_y = card.y + card.h + 24.0f;
    caption(list, ctx, a, cap_y, "Downloads, checks and installs the update", true);
    benefit_fit(list, ctx, a, cap_y + 36.0f, pv.benefit);
}

/* ---- QUIT EVO: 3 flow boxes STOP VIDEO -> RELEASE GPU -> CLOSE IN SWITCHER ---- */
static void draw_quit(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);

    const float bw1 = 120.0f, bw2 = 130.0f, bw3 = 160.0f;
    const float bh = 56.0f;
    const float arr_w = 26.0f;
    const float total_w = bw1 + bw2 + bw3 + 2.0f * arr_w;
    const float x0 = a.x + (a.w - total_w) * 0.5f;
    const float y = a.top + 32.0f;

    Rect b1{x0, y, bw1, bh};
    flow_box(list, ctx, b1, "STOP VIDEO", true, 13);

    flow_arrow(list, ctx, b1.x + b1.w + 4.0f, b1.x + b1.w + arr_w - 4.0f, b1.cy(), true);

    Rect b2{b1.x + b1.w + arr_w, y, bw2, bh};
    flow_box(list, ctx, b2, "RELEASE GPU", true, 13);

    flow_arrow(list, ctx, b2.x + b2.w + 4.0f, b2.x + b2.w + arr_w - 4.0f, b2.cy(), true);

    Rect b3{b2.x + b2.w + arr_w, y, bw3, bh};
    flow_box(list, ctx, b3, "CLOSE IN SWITCHER", true, 12);

    const float cap_y = y + bh + 36.0f;
    caption(list, ctx, a, cap_y, "Closes EVO safely", true);
    benefit_fit(list, ctx, a, cap_y + 36.0f, pv.benefit);
}

/* ---- WEB BROWSER: browser window mock inside a frame ---- */
static void draw_web(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    Area a = begin_card(list, ctx, box, pv);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const bool on = (pv.index == 1);

    const float fw = 380.0f, fh = 140.0f;
    const float fx = a.x + (a.w - fw) * 0.5f;
    Rect frame{fx, a.top, fw, fh};
    list.rounded_rect(frame, 12.0f, on ? Color::rgb(0x101522) : Color::rgb(0x0e0e12));
    frame_border(list, ctx, frame, on);

    /* Address bar pill with https:// */
    Rect bar{frame.x, frame.y, frame.w, 32.0f};
    list.rounded_rect({bar.x, bar.y, bar.w, 32.0f}, 12.0f, on ? Color::rgb(0x1a2130) : Color::rgb(0x14161c));
    list.circle(bar.x + 14.0f, bar.cy(), 3.5f, on ? Color::rgb(0xed6a5e) : Color::rgb(0x404450));
    list.circle(bar.x + 25.0f, bar.cy(), 3.5f, on ? Color::rgb(0xf4bf4f) : Color::rgb(0x404450));
    list.circle(bar.x + 36.0f, bar.cy(), 3.5f, on ? Color::rgb(0x61c554) : Color::rgb(0x404450));

    Rect addr{bar.x + 50.0f, bar.cy() - 10.0f, bar.w - 60.0f, 20.0f};
    list.rounded_rect(addr, 10.0f, on ? Color::rgb(0x242d3e) : Color::rgb(0x1a1c22));
    list.bordered_rect(addr, 10.0f, Color::rgb(0, 0), 1.0f, on ? p.accent : kWhite.with_alpha(0.12f));
    ui::text(list, fonts.mono, "https://prospero.local", addr.x + 10.0f, addr.cy() + 4.0f, 11, on ? kWhite : p.text_faint);

    /* A few grey content blocks inside frame */
    const float cy = bar.y + bar.h + 10.0f;
    Rect hero{frame.x + 14.0f, cy, frame.w - 28.0f, 32.0f};
    list.rounded_rect(hero, 6.0f, on ? p.accent.with_alpha(0.18f) : kWhite.with_alpha(0.04f));

    const float col_w = (hero.w - 16.0f) / 3.0f;
    const float cy2 = hero.y + hero.h + 8.0f;
    for (int i = 0; i < 3; ++i)
    {
        Rect col{hero.x + i * (col_w + 8.0f), cy2, col_w, 24.0f};
        list.rounded_rect(col, 4.0f, on ? kWhite.with_alpha(0.08f) : kWhite.with_alpha(0.03f));
    }

    const float cap_y = frame.y + frame.h + 24.0f;
    caption(list, ctx, a, cap_y, on ? "Web section on the rail" : "Web section hidden", on);
    const float y = chips(list, ctx, a, cap_y + 24.0f, chip_labels(pv), pv.index);
    benefit_fit(list, ctx, a, y + 20.0f, pv.benefit);
}

void draw_settings_preview(DrawList &list, const Context &ctx, const Rect &box, const SettingsPreview &pv)
{
    switch (pv.kind)
    {
    case EVO_RMLUI_PREVIEW_DEEP_BLACKS:
        draw_deep_blacks(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_UPSCALING:
        draw_upscaling(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_AI_NETWORK:
        draw_ai_network(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_HDR:
        draw_hdr(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_MOTION:
        draw_motion(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_REFRESH:
        draw_refresh(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_MATCH_RATE:
        draw_match_rate(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_ASPECT:
        draw_aspect(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_SUB_AUTO:
        draw_sub_auto(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_SUB_FONT:
        draw_sub_font(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_SUB_POS:
        draw_sub_pos(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_SUB_COLOUR:
        draw_sub_colour(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_SUB_SIZE:
    case EVO_RMLUI_PREVIEW_SUB_TEXT_COLOUR:
    case EVO_RMLUI_PREVIEW_SUB_LIFT:
    case EVO_RMLUI_PREVIEW_SUB_BACKGROUND:
        draw_sub_look(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_THEME:
        draw_theme(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_LIGHTBAR:
        draw_lightbar(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_CHANNELS:
        draw_channels(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_PASSTHROUGH:
        draw_passthrough(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_SURROUND:
        draw_surround(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_RESUME:
        draw_resume(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_DECODER:
        draw_decoder(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_NAV_SOUNDS:
        draw_nav_sounds(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_FOLDERS_FIRST:
        draw_folders_first(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_KEYBOARD:
        draw_keyboard(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_ASK_STREAM:
        draw_ask_stream(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_DEBUG_OVERLAY:
        draw_debug_overlay(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_PORT:
        draw_port(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_REPORT:
        draw_report(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_UPMIX:
        draw_upmix(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_PHONE_AUDIO:
        draw_phone_audio(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_PHONE_CAST:
        draw_phone_cast(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_CAST_PLAYER:
        draw_cast_player(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_YT_LINK:
        draw_yt_link(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_SKIP_SEGMENTS:
        draw_skip_segments(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_HDR_PEAK:
        draw_hdr_peak(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_AUDIO_LANGUAGE:
        draw_audio_language(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_SUB_LANGUAGE:
        draw_sub_language(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_VOLUME_BOOST:
        draw_volume_boost(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_AUDIO_DELAY:
        draw_audio_delay(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_NETWORK_BUFFER:
        draw_network_buffer(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_DEV_TOOLS:
        draw_dev_tools(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_CONSOLE:
        draw_console(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_MEMORY:
        draw_memory(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_UPDATE:
        draw_update(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_QUIT:
        draw_quit(list, ctx, box, pv);
        break;
    case EVO_RMLUI_PREVIEW_WEB:
        draw_web(list, ctx, box, pv);
        break;
    default: /* INFO and every kind that has no drawing yet */
        draw_info(list, ctx, box, pv);
        break;
    }

    /* No DETAILS block: it repeated the row's subtext, which the row now shows in
     * full (the section list folds to icons), and it took the room the
     * explanation above needs to be shown without being cut. */
}

} // namespace evo::kit
