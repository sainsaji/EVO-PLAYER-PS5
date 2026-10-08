/*
 * evo_hui_provider.cpp - the provider screens on ps5-homebrew-ui.
 *
 * Content follows assets/rml/{mediaserver,iptv,xtream,provider_fallback}.rml:
 * header, a 4x2 card page, a detail pane (poster, or channel + guide), the
 * setup cards of an empty source, the options side panel, and the status strip
 * with the button hints. The host keeps the data and the focus (RmlUi's own
 * navigation); this only draws what it is given.
 */
#include "evo_hui_provider.hpp"
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
const Color kBlack = Color::rgb(0x000000);
const Color kRed = Color::rgb(0xff6b6b);
const Color kLive = Color::rgb(0xff4d4d);
const Color kAmber = Color::rgb(0xffb020);

constexpr int kCols = 4;
constexpr float kGridX = NavRail::kWidth + 64.0f;
constexpr float kGridTop = 214.0f;
constexpr float kCardW = 232.0f;
constexpr float kGap = 24.0f;
constexpr Rect kDetail{1288.0f, 214.0f, 536.0f, 700.0f};

std::string str(const char *s) { return std::string(s ? s : ""); }

std::uint64_t hash_of(const std::string &s)
{
    std::uint64_t h = 1469598103934665603ull;
    for (char c : s)
        h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    return h;
}

/* "/assets/icons/x.png" (an RmlUi path) as the kit's bundle path. */
std::string icon_path(const std::string &p)
{
    static const std::string prefix = "/assets/icons/";
    if (p.compare(0, prefix.size(), prefix) == 0)
        return "../icons/" + p.substr(prefix.size());
    return p;
}

/* A cover-fit crop of a w x h picture into a box of the given aspect. */
Rect cover_uv(int w, int h, float box_w, float box_h)
{
    if (w <= 0 || h <= 0)
        return ::hui::gfx::kFullUv;
    const float img = static_cast<float>(w) / static_cast<float>(h);
    const float box = box_w / box_h;
    if (img > box)
    {
        const float f = box / img;
        return {(1.0f - f) * 0.5f, 0.0f, f, 1.0f};
    }
    const float f = img / box;
    return {0.0f, (1.0f - f) * 0.5f, 1.0f, f};
}

/* The largest rect of the picture's aspect that fits inside `box`. */
Rect contain_rect(int w, int h, const Rect &box)
{
    if (w <= 0 || h <= 0)
        return box;
    const float img = static_cast<float>(w) / static_cast<float>(h);
    const float b = box.w / box.h;
    if (img > b)
    {
        const float hh = box.w / img;
        return {box.x, box.y + (box.h - hh) * 0.5f, box.w, hh};
    }
    const float ww = box.h * img;
    return {box.x + (box.w - ww) * 0.5f, box.y, ww, box.h};
}

const char *kind_word(int variant) { return variant == EVO_HUI_PROVIDER_XTREAM ? "CATEGORY" : "GROUP"; }

struct SetupCard
{
    const char *icon, *title, *desc, *action;
};
} // namespace

void ProviderGridScreen::set(const evo_hui_provider_params_t &p)
{
    variant_ = p.variant;
    name_ = str(p.provider_name);
    crumb_ = str(p.breadcrumb);
    status_ = str(p.status);
    query_ = str(p.query);
    page_info_ = str(p.page_info);
    loading_ = p.loading != 0;
    tuning_ = p.tuning != 0;
    has_error_ = p.has_error != 0;
    empty_ = p.empty != 0;
    multi_page_ = p.has_multiple_pages != 0;
    folder_level_ = p.is_folder_level != 0;
    in_folder_ = p.in_folder != 0;
    count_ = p.count;
    focus_ = p.focus;
    rows_.clear();
    for (int i = 0; i < p.row_count && i < EVO_HUI_PROVIDER_ROWS; ++i)
    {
        const evo_hui_provider_row_t &r = p.rows[i];
        Row row;
        row.title = str(r.title);
        row.subtitle = str(r.subtitle);
        row.initial = str(r.initial);
        row.art_key = str(r.art_key);
        row.now = str(r.now);
        row.next = str(r.next);
        row.art = r.art;
        row.art_w = r.art_w;
        row.art_h = r.art_h;
        row.is_folder = r.is_folder != 0;
        row.played = r.played != 0;
        row.is_live = r.is_live != 0;
        row.progress = r.progress;
        rows_.push_back(std::move(row));
    }
    has_selected_ = p.has_selected != 0;
    sel_folder_ = p.selected_is_folder != 0;
    sel_played_ = p.selected_played != 0;
    sel_progress_ = p.selected_progress;
    sel_title_ = str(p.selected_title);
    sel_sub_ = str(p.selected_subtitle);
    sel_initial_ = str(p.selected_initial);
    sel_overview_ = str(p.selected_overview);
    sel_resume_ = str(p.selected_resume);
    sel_art_key_ = str(p.selected_art_key);
    sel_num_ = str(p.selected_num);
    sel_tech_ = str(p.selected_tech);
    sel_now_ = str(p.selected_now);
    sel_next_ = str(p.selected_next);
    epg_status_ = str(p.epg_status);
    epg_setup_ = p.epg_setup != 0;
    sel_art_ = p.selected_art;
    sel_art_w_ = p.selected_art_w;
    sel_art_h_ = p.selected_art_h;

    setup_configured_ = p.setup_configured != 0;
    setup_account_ = str(p.setup_account);
    setup_focus_ = p.setup_focus;

    const bool was_open = panel_open_;
    panel_open_ = p.panel_open != 0;
    if (panel_open_ && !was_open)
        panel_age_ = 0.0f;
    panel_crumb_ = str(p.panel_crumb);
    /* The kit's fonts have no single right-angle quote. */
    for (std::size_t at; (at = panel_crumb_.find("\xE2\x80\xBA")) != std::string::npos;)
        panel_crumb_.replace(at, 3, ">");
    panel_eyebrow_ = str(p.panel_eyebrow);
    panel_title_ = str(p.panel_title);
    panel_sub_ = str(p.panel_sub);
    panel_note_b_ = str(p.panel_note_b);
    panel_note_ = str(p.panel_note);
    panel_accept_ = str(p.panel_accept);
    panel_back_ = str(p.panel_back);
    panel_rows_.clear();
    for (int i = 0; i < p.panel_row_count && i < EVO_HUI_PROVIDER_PANEL_ROWS; ++i)
    {
        const evo_hui_panel_row_t &r = p.panel_rows[i];
        PanelRow row;
        row.title = str(r.title);
        row.detail = str(r.detail);
        row.badge = str(r.badge);
        row.icon = icon_path(str(r.icon));
        row.radio = r.radio != 0;
        row.on = r.on != 0;
        row.warn = r.warn != 0;
        row.chevron = r.chevron != 0;
        row.badge_live = r.badge_live != 0;
        row.focused = r.focused != 0;
        panel_rows_.push_back(std::move(row));
    }
}

void ProviderGridScreen::enter()
{
    age_ = 0.0f;
    ring_snapped_ = false;
}

bool ProviderGridScreen::setup_shown() const
{
    if (!empty_ || loading_ || !query_.empty())
        return false;
    if (variant_ == EVO_HUI_PROVIDER_LIVETV)
        return !in_folder_;
    return variant_ == EVO_HUI_PROVIDER_XTREAM;
}

Rect ProviderGridScreen::card_rect(int index) const
{
    const int col = index % kCols;
    const int row = index / kCols;
    const float h = channels() ? 300.0f : 334.0f;
    return {kGridX + col * (kCardW + kGap), kGridTop + row * (h + kGap), kCardW, h};
}

void ProviderGridScreen::update(float dt)
{
    age_ += dt;
    if (panel_open_)
        panel_age_ += dt;
    Rect target{};
    if (setup_shown() && setup_focus_ >= 0)
    {
        const int n = (variant_ == EVO_HUI_PROVIDER_XTREAM && setup_configured_) ? 3 : 2;
        const float w = 380.0f, gap = 28.0f;
        const float total = n * w + (n - 1) * gap;
        const float x0 = (kGridX + 1824.0f - total) * 0.5f;
        target = {x0 + setup_focus_ * (w + gap), 400.0f, w, 330.0f};
    }
    else if (focus_ >= 0 && focus_ < static_cast<int>(rows_.size()))
    {
        target = card_rect(focus_);
    }
    if (target.w <= 0.0f)
        return;
    if (!ring_snapped_)
    {
        ring_.snap(target);
        ring_snapped_ = true;
    }
    else
    {
        ring_.target(target);
    }
    ring_.update(dt, 22.0f);
}

bool ProviderGridScreen::focus(FocusInfo *out) const
{
    if (panel_open_)
    {
        for (std::size_t i = 0; i < panel_rows_.size(); ++i)
        {
            out->focusable.push_back({"panel-" + std::to_string(i), panel_rows_[i].title});
            if (panel_rows_[i].focused)
            {
                out->id = "panel-" + std::to_string(i);
                out->text = panel_rows_[i].title;
                out->index = static_cast<int>(i) + 1;
                out->total = static_cast<int>(panel_rows_.size());
            }
        }
        return !out->id.empty();
    }
    if (setup_shown() && setup_focus_ >= 0)
    {
        static const char *kIds[3] = {"tv-card-setup-url", "tv-card-setup-usb", "tv-card-setup-signout"};
        out->id = kIds[std::min(2, setup_focus_)];
        out->text = out->id;
        out->index = setup_focus_ + 1;
        out->total = (variant_ == EVO_HUI_PROVIDER_XTREAM && setup_configured_) ? 3 : 2;
        return true;
    }
    if (focus_ < 0 || focus_ >= static_cast<int>(rows_.size()))
        return false;
    out->id = "row-" + std::to_string(focus_);
    out->text = rows_[focus_].title;
    out->index = focus_ + 1;
    out->total = static_cast<int>(rows_.size());
    out->rect = card_rect(focus_);
    for (std::size_t i = 0; i < rows_.size(); ++i)
        out->focusable.push_back({"row-" + std::to_string(i), rows_[i].title});
    return true;
}

void ProviderGridScreen::draw_art(DrawList &list, const Context &ctx, const Rect &r, float radius,
                                  const std::uint32_t *art, int w, int h, const std::string &key, bool folder,
                                  const std::string &initial, float initial_size, bool contain) const
{
    const Palette &pal = ctx.palette;
    const ui::Fonts &fonts = ctx.fonts;
    const std::uint32_t tex = art ? ctx.textures.pixels(art, w, h, hash_of(key)) : 0;
    if (tex && contain)
    {
        /* a channel logo: whole, on a plate */
        list.gradient_rect(r, radius, kWhite.with_alpha(0.10f), kWhite.with_alpha(0.04f));
        list.image(tex, contain_rect(w, h, r.inset(18)), ::hui::gfx::kFullUv, kWhite);
        return;
    }
    if (tex)
    {
        list.image(tex, r, cover_uv(w, h, r.w, r.h), kWhite, radius);
        return;
    }
    list.gradient_rect(r, radius, pal.accent_soft, pal.bg_bottom);
    if (folder)
    {
        const std::uint32_t icon = ctx.textures.icon("../icons/icon_folder.png");
        if (icon)
            list.image(icon, {r.cx() - 36, r.cy() - 36, 72, 72}, ::hui::gfx::kFullUv, kWhite.with_alpha(0.85f));
    }
    else if (!initial.empty())
    {
        ui::text(list, fonts.display, initial, r.cx(), r.cy() + initial_size * 0.35f, initial_size,
                 kWhite.with_alpha(0.7f), Align::center);
    }
}

void ProviderGridScreen::draw_card(DrawList &list, const Context &ctx, const Row &row, const Rect &c) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &pal = ctx.palette;

    if (!channels())
    {
        /* poster wall card */
        const Rect poster{c.x, c.y, c.w, 268.0f};
        list.shadow(poster, 16, 18, kBlack.with_alpha(0.35f));
        draw_art(list, ctx, poster, 16, row.art, row.art_w, row.art_h, row.art_key, row.is_folder, row.initial, 80.0f,
                 false);
        if (row.played)
        {
            const float w2 = fonts.semibold.measure("WATCHED", 13, 1.2f) + 18;
            Rect pill{poster.x + 10, poster.y + 10, w2, 24};
            list.rounded_rect(pill, 12, kBlack.with_alpha(0.62f));
            ui::text(list, fonts.semibold, "WATCHED", pill.cx(), pill.y + 17, 13, kWhite, Align::center, 1.2f);
        }
        if (row.progress > 0)
        {
            Rect bar{poster.x + 10, poster.y + poster.h - 14, poster.w - 20, 5};
            list.rounded_rect(bar, 2.5f, kBlack.with_alpha(0.55f));
            list.rounded_rect({bar.x, bar.y, bar.w * std::min(100, row.progress) / 100.0f, bar.h}, 2.5f, pal.accent);
        }
        ui::text(list, fonts.semibold, fonts.semibold.font->fit(row.title, 19, c.w), c.x + 2, c.y + 296, 19,
                 kWhite.with_alpha(0.92f));
        ui::text(list, fonts.regular, fonts.regular.font->fit(row.subtitle, 15, c.w), c.x + 2, c.y + 320, 15,
                 pal.text_faint);
        return;
    }

    /* channel / group card */
    list.shadow(c, 18, 18, kBlack.with_alpha(0.3f));
    list.rounded_rect(c, 18, pal.surface);
    const Rect preview{c.x + 10, c.y + 10, c.w - 20, 132};
    draw_art(list, ctx, preview, 12, row.art, row.art_w, row.art_h, row.art_key, row.is_folder, row.initial, 64.0f,
             true);
    if (row.is_folder)
    {
        const float w2 = fonts.semibold.measure(kind_word(variant_), 12, 1.2f) + 16;
        Rect pill{preview.x + 8, preview.y + 8, w2, 22};
        list.rounded_rect(pill, 11, kBlack.with_alpha(0.6f));
        ui::text(list, fonts.semibold, kind_word(variant_), pill.cx(), pill.y + 16, 12, kWhite, Align::center, 1.2f);
    }
    else if (row.is_live)
    {
        Rect pill{preview.x + 8, preview.y + 8, 48, 22};
        list.rounded_rect(pill, 11, kLive);
        ui::text(list, fonts.semibold, "LIVE", pill.cx(), pill.y + 16, 12, kWhite, Align::center, 1.2f);
    }
    float y = c.y + 168;
    ui::text(list, fonts.semibold, fonts.semibold.font->fit(row.title, 18, c.w - 24), c.x + 14, y, 18, kWhite);
    y += 24;
    if (row.is_folder || row.now.empty())
    {
        ui::text(list, fonts.regular, fonts.regular.font->fit(row.subtitle, 14, c.w - 24), c.x + 14, y, 14,
                 pal.text_faint);
    }
    else
    {
        ui::text(list, fonts.semibold, "NOW", c.x + 14, y, 11, pal.accent, Align::left, 1.2f);
        ui::text(list, fonts.regular, fonts.regular.font->fit(row.now, 14, c.w - 70), c.x + 52, y, 14, pal.text_muted);
        if (!row.next.empty())
        {
            y += 20;
            ui::text(list, fonts.semibold, "NEXT", c.x + 14, y, 11, pal.text_faint, Align::left, 1.2f);
            ui::text(list, fonts.regular, fonts.regular.font->fit(row.next, 14, c.w - 70), c.x + 52, y, 14,
                     pal.text_faint);
        }
    }
    /* footer */
    const char *kind = row.is_folder ? kind_word(variant_)
                       : (variant_ == EVO_HUI_PROVIDER_XTREAM && !row.is_live) ? "VOD" : (row.is_live || variant_ == EVO_HUI_PROVIDER_LIVETV) ? "CHANNEL" : "VOD";
    const char *tag = row.is_folder ? "OPEN" : ((variant_ == EVO_HUI_PROVIDER_XTREAM && !row.is_live) ? "PLAY" : "WATCH");
    list.line(c.x + 14, c.y + c.h - 36, c.x + c.w - 14, c.y + c.h - 36, 1.0f, kWhite.with_alpha(0.08f));
    ui::text(list, fonts.semibold, kind, c.x + 14, c.y + c.h - 14, 12, pal.text_faint, Align::left, 1.4f);
    ui::text(list, fonts.semibold, tag, c.x + c.w - 14, c.y + c.h - 14, 12, pal.accent, Align::right, 1.4f);
}

void ProviderGridScreen::draw_detail(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &pal = ctx.palette;
    list.push_opacity(tween::stagger(age_, 3, 0.08f, 0.5f));
    list.rounded_rect(kDetail, 28, pal.surface);
    const float x = kDetail.x + 32;
    const float w = kDetail.w - 64;

    if (!channels())
    {
        ui::text(list, fonts.semibold, sel_folder_ ? "OPEN" : "PLAY", x, kDetail.y + 48, 16, pal.accent,
                 Align::left, 3.0f);
        Rect poster{x, kDetail.y + 70, 168, 236};
        draw_art(list, ctx, poster, 14, sel_art_, sel_art_w_, sel_art_h_, sel_art_key_, sel_folder_, sel_initial_, 64.0f,
                 false);
        const float tx = poster.x + poster.w + 24;
        const float tw = x + w - tx;
        float y = poster.y + 34;
        y = ui::paragraph(list, fonts.display, sel_title_, tx, y, 28, tw, 34, kWhite, 3);
        if (!sel_sub_.empty())
            ui::text(list, fonts.regular, fonts.regular.font->fit(sel_sub_, 18, tw), tx, y + 4, 18, pal.text_muted);
        y += 34;
        if (sel_played_)
            ui::text(list, fonts.semibold, "WATCHED", tx, y, 14, pal.accent, Align::left, 1.6f);
        if (!sel_resume_.empty())
        {
            ui::text(list, fonts.regular, sel_resume_, tx, poster.y + poster.h - 26, 16, pal.text_muted);
            Rect bar{tx, poster.y + poster.h - 14, tw, 6};
            list.rounded_rect(bar, 3, kWhite.with_alpha(0.14f));
            list.rounded_rect({bar.x, bar.y, bar.w * std::min(100, sel_progress_) / 100.0f, bar.h}, 3, pal.accent);
        }
        if (!sel_overview_.empty())
            ui::paragraph(list, fonts.regular, sel_overview_, x, poster.y + poster.h + 44, 18, w, 27, pal.text_muted, 9);
        const char *verb = sel_folder_ ? "OPEN" : (sel_resume_.empty() ? "PLAY" : "RESUME");
        Rect act{x, kDetail.y + kDetail.h - 84, w, 56};
        list.rounded_rect(act, 28, pal.accent.with_alpha(0.28f));
        ui::text(list, fonts.semibold, verb, act.cx(), act.cy() + 7, 20, kWhite, Align::center, 2.0f);
        list.pop_opacity();
        return;
    }

    const bool xt = variant_ == EVO_HUI_PROVIDER_XTREAM;
    ui::text(list, fonts.semibold,
             sel_folder_ ? (xt ? "SELECTED CATEGORY" : "SELECTED GROUP") : (xt ? "SELECTED ITEM" : "SELECTED CHANNEL"), x,
             kDetail.y + 46, 15, pal.accent, Align::left, 3.0f);
    Rect plate{x, kDetail.y + 66, w, 150};
    draw_art(list, ctx, plate, 16, sel_art_, sel_art_w_, sel_art_h_, sel_art_key_, sel_folder_, sel_initial_, 72.0f, true);
    if (sel_folder_)
    {
        const char *k = kind_word(variant_);
        const float w2 = fonts.semibold.measure(k, 12, 1.2f) + 18;
        Rect pill{plate.x + 10, plate.y + 10, w2, 24};
        list.rounded_rect(pill, 12, kBlack.with_alpha(0.6f));
        ui::text(list, fonts.semibold, k, pill.cx(), pill.y + 17, 12, kWhite, Align::center, 1.2f);
    }
    else
    {
        Rect pill{plate.x + 10, plate.y + 10, 74, 24};
        list.rounded_rect(pill, 12, kLive);
        ui::text(list, fonts.semibold, "\xE2\x97\x8F LIVE", pill.cx(), pill.y + 17, 12, kWhite, Align::center, 1.0f);
    }
    float y = plate.y + plate.h + 36;
    float tx = x;
    if (!sel_folder_ && !sel_num_.empty())
        tx += ui::text(list, fonts.semibold, sel_num_, x, y, 14, pal.accent, Align::left, 1.2f) + 14;
    ui::text(list, fonts.regular, fonts.regular.font->fit(sel_tech_, 14, x + w - tx), tx, y, 14, pal.text_faint);
    y += 38;
    y = ui::paragraph(list, fonts.display, sel_title_, x, y, 28, w, 34, kWhite, 2);
    if (!sel_sub_.empty())
        ui::text(list, fonts.regular, fonts.regular.font->fit(sel_sub_, 17, w), x, y + 2, 17, pal.text_muted);

    if (!sel_folder_)
    {
        Rect box{x, kDetail.y + 410, w, 168};
        list.rounded_rect(box, 16, kBlack.with_alpha(0.28f));
        ui::text(list, fonts.semibold, "CHANNEL GUIDE", box.x + 18, box.y + 30, 12, pal.text_faint, Align::left, 1.8f);
        if (!sel_now_.empty())
            ui::text(list, fonts.semibold, "\xE2\x97\x8F ON AIR", box.x + box.w - 18, box.y + 30, 12, kLive, Align::right,
                     1.2f);
        float ry = box.y + 62;
        if (!sel_now_.empty())
        {
            ui::text(list, fonts.semibold, "NOW", box.x + 18, ry, 12, pal.accent, Align::left, 1.2f);
            ui::text(list, fonts.regular, fonts.regular.font->fit(sel_now_, 17, box.w - 100), box.x + 72, ry, 17, kWhite);
            ry += 32;
        }
        if (!sel_next_.empty())
        {
            ui::text(list, fonts.semibold, "NEXT", box.x + 18, ry, 12, pal.text_faint, Align::left, 1.2f);
            ui::text(list, fonts.regular, fonts.regular.font->fit(sel_next_, 17, box.w - 100), box.x + 72, ry, 17,
                     pal.text_muted);
            ry += 32;
        }
        if (sel_now_.empty() && sel_next_.empty())
        {
            ui::text(list, fonts.semibold, "GUIDE", box.x + 18, ry, 12, pal.accent, Align::left, 1.2f);
            const std::string msg =
                xt ? "Live broadcast \xE2\x80\x94 guide loading / standby" : (epg_status_.empty() ? "Live Broadcast" : epg_status_);
            ui::paragraph(list, fonts.regular, msg, box.x + 78, ry, 16, box.w - 96, 22, pal.text_muted, 3);
            if (epg_setup_ && !xt)
            {
                ui::text(list, fonts.semibold, "OPTIONS", box.x + 18, box.y + box.h - 22, 12, pal.accent, Align::left, 1.2f);
                ui::text(list, fonts.semibold, "SET UP A GUIDE", box.x + 100, box.y + box.h - 22, 12, pal.text_muted,
                         Align::left, 1.2f);
            }
        }
    }

    const char *verb = sel_folder_ ? (xt ? "OPEN CATEGORY" : "OPEN GROUP") : (xt ? "OPEN STREAM" : "WATCH CHANNEL");
    Rect act{x, kDetail.y + kDetail.h - 84, w, 56};
    list.rounded_rect(act, 28, pal.accent.with_alpha(0.28f));
    ui::text(list, fonts.semibold, verb, act.cx(), act.cy() + 7, 20, kWhite, Align::center, 2.0f);
    if (multi_page_)
        ui::text(list, fonts.regular, "Use L1 / R1 or D-pad up/down to flip pages", kDetail.cx(), kDetail.y + kDetail.h - 96,
                 13, pal.text_faint, Align::center);
    list.pop_opacity();
}

void ProviderGridScreen::draw_setup(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &pal = ctx.palette;
    const bool xt = variant_ == EVO_HUI_PROVIDER_XTREAM;
    const float cx = (kGridX + 1824.0f) * 0.5f;

    ui::text(list, fonts.display, xt ? "SETUP XTREAM CODES ACCOUNT" : "SELECT PLAYLIST SOURCE", cx, 270, 38, kWhite,
             Align::center);
    ui::text(list, fonts.regular,
             xt ? "Connect to your Xtream Codes subscription for Live TV, Movies & Series"
                : "Choose how you would like to load your television channels",
             cx, 312, 20, pal.text_muted, Align::center);
    if (!setup_account_.empty())
        ui::text(list, fonts.semibold, setup_account_, cx, 346, 17, pal.accent, Align::center, 0.8f);

    std::vector<SetupCard> cards;
    if (xt)
    {
        cards.push_back({"../icons/icon_settings.png", "ENTER SERVER & CREDENTIALS",
                         "Type http://user:pass@host:port or get.php URL", "SETUP ACCOUNT"});
        cards.push_back({"../icons/icon_browse_usb.png", "LOAD FROM USB", "Open an M3U exported from your Xtream account",
                         "READ USB"});
        if (setup_configured_)
            cards.push_back({"../icons/icon_power.png", "SIGN OUT", "Remove this account from EVO", "SIGN OUT"});
    }
    else
    {
        cards.push_back({"../icons/icon_settings.png", "ENTER URL MANUALLY",
                         "Type an HTTP or HTTPS stream address using the on-screen keyboard", "ENTER ADDRESS"});
        cards.push_back({"../icons/icon_browse_usb.png", "BROWSE FROM USB",
                         "Scan connected USB storage (/mnt/usb0, /mnt/usb1) for M3U playlists", "BROWSE USB"});
    }
    const int n = static_cast<int>(cards.size());
    const float w = 380.0f, gap = 28.0f;
    const float x0 = cx - (n * w + (n - 1) * gap) * 0.5f;
    for (int i = 0; i < n; ++i)
    {
        list.push_opacity(tween::stagger(age_, i, 0.08f, 0.5f));
        Rect c{x0 + i * (w + gap), 400, w, 330};
        list.shadow(c, 24, 20, kBlack.with_alpha(0.3f));
        list.rounded_rect(c, 24, pal.surface);
        list.circle(c.cx(), c.y + 84, 44, pal.accent.with_alpha(0.2f));
        const std::uint32_t icon = ctx.textures.icon(cards[i].icon);
        if (icon)
            list.image(icon, {c.cx() - 24, c.y + 60, 48, 48}, ::hui::gfx::kFullUv, kWhite);
        ui::text(list, fonts.semibold, cards[i].title, c.cx(), c.y + 176, 20, kWhite, Align::center, 0.6f);
        ui::paragraph(list, fonts.regular, cards[i].desc, c.x + 28, c.y + 208, 16, c.w - 56, 23, pal.text_muted, 3,
                      Align::left);
        Rect act{c.x + 28, c.y + c.h - 76, c.w - 56, 48};
        list.rounded_rect(act, 24, pal.accent.with_alpha(0.25f));
        ui::text(list, fonts.semibold, cards[i].action, act.cx(), act.cy() + 6, 16, kWhite, Align::center, 1.6f);
        list.pop_opacity();
    }
    if (setup_focus_ >= 0)
    {
        const Rect ring = ring_.value();
        list.glow(ring, 24, 22, pal.accent.with_alpha(0.3f + 0.1f * ui::breathe(ctx.time)));
        list.bordered_rect(ring.inset(-4), 28, Color::rgb(0x000000, 0.0f), 4, kWhite);
    }
}

void ProviderGridScreen::draw_panel(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &pal = ctx.palette;
    const float a = std::min(1.0f, panel_age_ * 6.0f);
    list.push_opacity(a);
    list.rounded_rect({0, 0, 1920, 1080}, 0, kBlack.with_alpha(0.55f));
    const float slide = (1.0f - a) * 40.0f;
    Rect p{1920.0f - 600.0f + slide, 0, 600, 1080};
    list.rounded_rect(p, 0, Color::rgb(0x0b0d16, 1.0f));
    list.line(p.x, 0, p.x, 1080, 1.0f, kWhite.with_alpha(0.1f));
    const float x = p.x + 44, w = p.w - 88;
    float y = 120;
    float cx = x;
    if (!panel_crumb_.empty())
        cx += ui::text(list, fonts.semibold, panel_crumb_, x, y, 14, pal.text_faint, Align::left, 2.0f) + 2;
    ui::text(list, fonts.semibold, panel_eyebrow_, cx, y, 14, pal.accent, Align::left, 2.0f);
    y += 46;
    ui::text(list, fonts.display, fonts.display.font->fit(panel_title_, 36, w), x, y, 36, kWhite);
    y += 32;
    if (!panel_sub_.empty())
    {
        y = ui::paragraph(list, fonts.regular, panel_sub_, x, y, 17, w, 24, pal.text_muted, 2);
    }
    y += 24;

    const float rh = 76.0f;
    for (std::size_t i = 0; i < panel_rows_.size(); ++i)
    {
        const PanelRow &r = panel_rows_[i];
        Rect row{x - 12, y + i * (rh + 6), w + 24, rh};
        if (row.y + rh > 940)
            break;
        if (r.focused)
        {
            list.rounded_rect(row, 18, pal.surface_sel);
            list.bordered_rect(row, 18, Color::rgb(0x000000, 0.0f), 2, kWhite.with_alpha(0.9f));
        }
        else
        {
            list.rounded_rect(row, 18, pal.surface);
        }
        const float lx = row.x + 20;
        if (r.radio)
        {
            list.ring(lx + 14, row.cy(), 12, 2.5f, r.on ? pal.accent : kWhite.with_alpha(0.35f));
            if (r.on)
                list.circle(lx + 14, row.cy(), 6, pal.accent);
        }
        else
        {
            list.rounded_rect({lx, row.cy() - 22, 44, 44}, 12, kWhite.with_alpha(0.08f));
            const std::uint32_t icon = r.icon.empty() ? 0 : ctx.textures.icon(r.icon);
            if (icon)
                list.image(icon, {lx + 10, row.cy() - 12, 24, 24}, ::hui::gfx::kFullUv, kWhite);
        }
        const float tx = lx + 62;
        float right = row.x + row.w - 20;
        if (r.chevron)
        {
            const std::uint32_t chev = ctx.textures.icon("../icons/icon_chevron.png");
            if (chev)
                list.image(chev, {right - 20, row.cy() - 10, 20, 20}, ::hui::gfx::kFullUv, kWhite.with_alpha(0.7f));
            right -= 30;
        }
        if (!r.badge.empty())
        {
            const float bw = fonts.semibold.measure(r.badge, 12, 1.2f) + 20;
            Rect bp{right - bw, row.cy() - 13, bw, 26};
            list.rounded_rect(bp, 13, r.badge_live ? kLive.with_alpha(0.3f) : kWhite.with_alpha(0.1f));
            ui::text(list, fonts.semibold, r.badge, bp.cx(), bp.y + 18, 12, r.badge_live ? kWhite : pal.text_muted,
                     Align::center, 1.2f);
            right -= bw + 10;
        }
        const float tw = right - tx;
        ui::text(list, fonts.semibold, fonts.semibold.font->fit(r.title, 19, tw), tx, row.cy() - 4, 19, kWhite);
        ui::text(list, fonts.regular, fonts.regular.font->fit(r.detail, 15, tw), tx, row.cy() + 20, 15,
                 r.warn ? kAmber : pal.text_faint);
    }

    if (!panel_note_b_.empty())
    {
        const float ny = 960;
        const float bw = ui::text(list, fonts.semibold, panel_note_b_, x, ny, 14, pal.accent);
        ui::paragraph(list, fonts.regular, panel_note_, x + bw + 8, ny, 14, w - bw - 8, 20, pal.text_faint, 2);
    }
    const ui::Hint hints[] = {{ui::Button::cross, panel_accept_.c_str()}, {ui::Button::circle, panel_back_.c_str()}};
    ui::HintLayout layout;
    layout.size = 30;
    layout.text_size = 20;
    layout.cy = 1030.0f;
    ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), hints, 2, 1876.0f, true, layout);
    list.pop_opacity();
}

void ProviderGridScreen::draw(DrawList &list, const Context &ctx) const
{
    draw_backdrop(list, ctx);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &pal = ctx.palette;
    const bool xt = variant_ == EVO_HUI_PROVIDER_XTREAM;
    const bool live = variant_ == EVO_HUI_PROVIDER_LIVETV;

    /* header */
    {
        list.push_opacity(tween::stagger(age_, 0, 0.08f, 0.5f));
        const std::uint32_t badge =
            ctx.textures.icon(channels() ? "../icons/icon_tv.png" : "../icons/icon_emby.png");
        Rect box{kGridX, 96, 56, 56};
        list.rounded_rect(box, 16, pal.accent.with_alpha(0.22f));
        if (badge)
            list.image(badge, box.inset(12), ::hui::gfx::kFullUv, kWhite);
        const std::string title = live ? "LIVE TV" : (xt ? "XTREAM CODES" : ui::upper(name_));
        const float tw = ui::text(list, fonts.display, title, kGridX + 76, 134, 40, kWhite);
        if (channels())
            list.circle(kGridX + 76 + tw + 18, 118, 6, kLive.with_alpha(0.6f + 0.4f * ui::breathe(ctx.time)));
        ui::text(list, fonts.regular, crumb_, kGridX + 76, 168, 20, pal.text_muted);
        if (!empty_)
        {
            float right = 1824.0f;
            std::string counter;
            if (live)
                counter = std::to_string(count_) + (folder_level_ ? " GROUPS" : " CHANNELS");
            else if (xt)
                counter = std::to_string(count_) + (folder_level_ ? " CATEGORIES" : " ITEMS");
            else
                counter = std::to_string(count_) + (count_ == 1 ? " ITEM" : " ITEMS");
            const float cw = fonts.semibold.measure(counter, 15, 1.4f) + 24;
            Rect cp{right - cw, 108, cw, 32};
            list.rounded_rect(cp, 16, kWhite.with_alpha(0.08f));
            ui::text(list, fonts.semibold, counter, cp.cx(), cp.y + 22, 15, pal.text_muted, Align::center, 1.4f);
            right -= cw + 12;
            if (multi_page_)
            {
                const float pw = fonts.semibold.measure(page_info_, 15, 1.4f) + 24;
                Rect pp{right - pw, 108, pw, 32};
                list.rounded_rect(pp, 16, pal.accent.with_alpha(0.25f));
                ui::text(list, fonts.semibold, page_info_, pp.cx(), pp.y + 22, 15, kWhite, Align::center, 1.4f);
            }
        }
        list.pop_opacity();
    }

    /* notice */
    if (!status_.empty())
        ui::text(list, fonts.semibold, status_, kGridX + 2, 200, 17, has_error_ ? kRed : pal.accent, Align::left, 0.6f);

    if (setup_shown())
    {
        draw_setup(list, ctx);
    }
    else if (empty_ && !loading_)
    {
        const float cx = (kGridX + kDetail.x + kDetail.w) * 0.5f;
        const bool searching = !query_.empty();
        const std::uint32_t icon = ctx.textures.icon(channels() ? "../icons/icon_tv.png" : "../icons/icon_emby.png");
        Rect box{cx - 48, 340, 96, 96};
        list.rounded_rect(box, 28, kWhite.with_alpha(0.06f));
        if (icon)
            list.image(icon, box.inset(24), ::hui::gfx::kFullUv, kWhite.with_alpha(0.6f));
        std::string title, hint, sub;
        if (searching)
        {
            title = live ? "NO CHANNELS FOUND" : (xt ? "NO ITEMS FOUND" : "NO RESULTS");
            hint = (live ? "No channels match '" : (xt ? "No items match '" : "Nothing matches '")) + query_ + "'";
            sub = "Press Square to search again, or Circle to clear";
        }
        else if (live)
        {
            title = "NO CHANNELS HERE";
            hint = "This group is empty";
            sub = "Press Circle to go back";
        }
        else
        {
            title = "NOTHING HERE YET";
            hint = "This list is empty";
            sub = "Press Circle to go back";
        }
        ui::text(list, fonts.display, title, cx, 500, 36, kWhite, Align::center);
        ui::text(list, fonts.regular, hint, cx, 540, 22, pal.text_muted, Align::center);
        ui::text(list, fonts.regular, sub, cx, 576, 18, pal.text_faint, Align::center);
    }
    else
    {
        for (std::size_t i = 0; i < rows_.size(); ++i)
        {
            const float rise = tween::stagger(age_, static_cast<int>(i), 0.04f, 0.4f);
            list.push_opacity(rise);
            Rect c = card_rect(static_cast<int>(i));
            c.y += (1.0f - rise) * 16.0f;
            draw_card(list, ctx, rows_[i], c);
            list.pop_opacity();
        }
        if (focus_ >= 0 && focus_ < static_cast<int>(rows_.size()))
        {
            Rect ring = ring_.value();
            if (!channels())
                ring.h = 268.0f;
            list.glow(ring, 18, 20, pal.accent.with_alpha(0.32f + 0.1f * ui::breathe(ctx.time)));
            list.bordered_rect(ring.inset(-5), 22, Color::rgb(0x000000, 0.0f), 4, kWhite);
        }
        if (has_selected_)
            draw_detail(list, ctx);
    }

    /* tuning */
    if (tuning_)
    {
        list.rounded_rect({0, 0, 1920, 1080}, 0, kBlack.with_alpha(0.55f));
        Rect card{760, 440, 400, 200};
        list.rounded_rect(card, 28, pal.surface_sel);
        const float a = ctx.time * 6.0f;
        for (int i = 0; i < 8; ++i)
        {
            const float ang = a + i * 0.785398f;
            list.circle(card.cx() + std::cos(ang) * 26, card.y + 66 + std::sin(ang) * 26, 4.0f,
                        kWhite.with_alpha(0.15f + 0.85f * (i / 8.0f)));
        }
        ui::text(list, fonts.display, live ? "TUNING CHANNEL" : (xt ? "OPENING STREAM" : "STARTING"), card.cx(),
                 card.y + 136, 26, kWhite, Align::center);
        ui::text(list, fonts.regular,
                 live ? "Connecting to live stream..." : (xt ? "Connecting to Xtream server..." : "Opening the stream..."),
                 card.cx(), card.y + 168, 17, pal.text_muted, Align::center);
    }

    /* status strip */
    {
        float x = 192;
        x += ui::text(list, fonts.semibold, ui::upper(name_), x, 1030, 16, pal.text_faint, Align::left, 1.6f) + 16;
        if (channels())
        {
            const char *pill = live ? (folder_level_ ? "GROUPS" : "CHANNEL GUIDE") : (folder_level_ ? "CATEGORIES" : "CHANNEL GUIDE");
            x += ui::text(list, fonts.semibold, pill, x, 1030, 13, pal.text_faint, Align::left, 1.4f) + 16;
        }
        if (multi_page_)
            x += ui::text(list, fonts.semibold, page_info_, x, 1030, 13, pal.text_faint, Align::left, 1.4f) + 16;
        if (loading_ || tuning_)
            ui::text(list, fonts.semibold, tuning_ ? (live || xt ? "TUNING..." : "STARTING...") : "LOADING...", x, 1030, 14,
                     pal.accent, Align::left, 1.6f);

        std::vector<ui::Hint> hints;
        if (channels())
        {
            const char *go = (empty_ && !loading_) ? "Select" : (folder_level_ ? "Open" : "Watch");
            hints.push_back({ui::Button::cross, go});
            hints.push_back({ui::Button::circle, "Back"});
            if (!empty_)
                hints.push_back({ui::Button::square, "Search"});
            if (!empty_ && !folder_level_)
                hints.push_back({ui::Button::triangle, "Favorite"});
            if (multi_page_)
                hints.push_back({ui::Button::l1, "Page", ui::Button::r1});
            hints.push_back({ui::Button::dpad, "Navigate"});
            if (!empty_)
                hints.push_back({ui::Button::options, xt ? "Account" : "Playlist & guide"});
        }
        else
        {
            hints.push_back({ui::Button::cross, folder_level_ ? "Open" : "Play"});
            hints.push_back({ui::Button::circle, "Back"});
            hints.push_back({ui::Button::square, "Search"});
            if (multi_page_)
                hints.push_back({ui::Button::l1, "Page", ui::Button::r1});
            hints.push_back({ui::Button::options, "Servers"});
        }
        ui::HintLayout layout;
        layout.size = 30;
        layout.text_size = 20;
        layout.cy = 1030.0f;
        ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), hints.data(), static_cast<int>(hints.size()), 1824.0f, true,
                       layout);
    }

    if (panel_open_)
        draw_panel(list, ctx);
}

} // namespace evo::kit
