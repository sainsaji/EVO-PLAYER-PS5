/*
 * evo_hui_webpage.cpp - Browser section's page view on ps5-homebrew-ui (#237 Phase 2).
 */
#include "evo_rmlui_bridge.h"
#include "evo_hui.h"
#include "evo_hui_screens.hpp"

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
namespace ui = ::hui::ui;

namespace
{

const Color kWhite = Color::rgb(0xffffff);
const Color kDark = Color::rgb(0x0a0c14);

/* Pointer size: the shapes below are drawn at 1x and scaled, so the pointer
 * reads from a sofa (about 45 px tall). */
constexpr float kPointerScale = 1.5f;

void scale_points(float *pts, int n, float px, float py)
{
    for (int i = 0; i < n; ++i)
    {
        pts[i * 2] = px + (pts[i * 2] - px) * kPointerScale;
        pts[i * 2 + 1] = py + (pts[i * 2 + 1] - py) * kPointerScale;
    }
}

void draw_arrow_pointer(DrawList &list, float px, float py)
{
    // Arrow pointer pointing up-left, with a dark outline
    float pts[] = {
        px, py,
        px, py + 24.0f,
        px + 6.0f, py + 19.0f,
        px + 11.0f, py + 29.0f,
        px + 15.0f, py + 27.0f,
        px + 10.0f, py + 17.0f,
        px + 17.0f, py + 17.0f
    };
    scale_points(pts, 7, px, py);
    for (int i = 0; i < 7; ++i)
    {
        int next = (i + 1) % 7;
        list.line(pts[i * 2], pts[i * 2 + 1], pts[next * 2], pts[next * 2 + 1], 2.5f, kDark);
    }
    list.polygon(pts, 7, kWhite);
}

void draw_hand_pointer(DrawList &list, float px, float py)
{
    // Hand pointer pointing up with index finger, with a dark outline
    float pts[] = {
        px, py,
        px + 4.0f, py,
        px + 4.0f, py + 8.0f,
        px + 8.0f, py + 8.0f,
        px + 8.0f, py + 10.0f,
        px + 12.0f, py + 10.0f,
        px + 12.0f, py + 12.0f,
        px + 16.0f, py + 12.0f,
        px + 16.0f, py + 24.0f,
        px + 6.0f, py + 26.0f,
        px - 4.0f, py + 18.0f,
        px - 6.0f, py + 12.0f,
        px - 2.0f, py + 10.0f,
        px, py + 8.0f
    };
    scale_points(pts, 14, px, py);
    for (int i = 0; i < 14; ++i)
    {
        int next = (i + 1) % 14;
        list.line(pts[i * 2], pts[i * 2 + 1], pts[next * 2], pts[next * 2 + 1], 2.5f, kDark);
    }
    list.polygon(pts, 14, kWhite);
    const float s = kPointerScale;
    list.line(px + 4.0f * s, py + 8.0f * s, px + 4.0f * s, py + 18.0f * s, 1.5f, kDark);
    list.line(px + 8.0f * s, py + 10.0f * s, px + 8.0f * s, py + 18.0f * s, 1.5f, kDark);
    list.line(px + 12.0f * s, py + 12.0f * s, px + 12.0f * s, py + 18.0f * s, 1.5f, kDark);
}

} // namespace

void WebPage::set(const evo_hui_webpage_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    url_ = str(p.url);
    title_ = str(p.title);
    message_ = str(p.message);
    loading_ = p.loading;
    can_back_ = p.can_back;
    can_forward_ = p.can_forward;
    state_ = p.state;
    pointer_visible_ = p.pointer_visible;
    pointer_x_ = p.pointer_x;
    pointer_y_ = p.pointer_y;
    pointer_hand_ = p.pointer_hand;
    hints_visible_ = p.hints_visible;
}

void WebPage::enter()
{
    age_ = 0.0f;
    loading_age_ = 0.0f;
}

void WebPage::update(float dt)
{
    age_ += dt;
    if (loading_)
        loading_age_ += dt;
    else
        loading_age_ = 0.0f;
}

void WebPage::set_page(std::uint32_t tex, float u1, float v1)
{
    tex_ = tex;
    u1_ = u1;
    v1_ = v1;
}

bool WebPage::focus(FocusInfo *out) const
{
    if (!out)
        return false;
    out->id = "webpage";
    out->text = url_;
    out->rect = {120.0f, 18.0f, 1680.0f, 60.0f};
    out->index = 1;
    out->total = 1;
    out->focusable.clear();
    out->focusable.emplace_back(out->id, out->text);
    return true;
}

void WebPage::draw(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    // 1. Background: the theme background, like the other pages.
    draw_backdrop(list, ctx);

    // 2. Address bar: y 0..96
    // Back and forward arrow glyphs left of the field (x: 0..120, cy: 48)
    const float arrow_cy = 48.0f;
    const Color back_c = can_back_ ? p.text : p.text.with_alpha(0.25f);
    const Color fwd_c = can_forward_ ? p.text : p.text.with_alpha(0.25f);

    // Back arrow (pointing left) at cx = 48
    const float back_cx = 48.0f;
    list.line(back_cx + 7.0f, arrow_cy, back_cx - 7.0f, arrow_cy, 2.5f, back_c);
    list.line(back_cx - 7.0f, arrow_cy, back_cx - 1.0f, arrow_cy - 6.0f, 2.5f, back_c);
    list.line(back_cx - 7.0f, arrow_cy, back_cx - 1.0f, arrow_cy + 6.0f, 2.5f, back_c);

    // Forward arrow (pointing right) at cx = 88
    const float fwd_cx = 88.0f;
    list.line(fwd_cx - 7.0f, arrow_cy, fwd_cx + 7.0f, arrow_cy, 2.5f, fwd_c);
    list.line(fwd_cx + 7.0f, arrow_cy, fwd_cx + 1.0f, arrow_cy - 6.0f, 2.5f, fwd_c);
    list.line(fwd_cx + 7.0f, arrow_cy, fwd_cx + 1.0f, arrow_cy + 6.0f, 2.5f, fwd_c);

    // Rounded field (kRadiusTile) from x 120 to x 1800, y 18..78
    const Rect field{120.0f, 18.0f, 1680.0f, 60.0f};
    list.rounded_rect(field, page::kRadiusTile, p.surface);
    list.bordered_rect(field, page::kRadiusTile, Color::rgb(0, 0), 1.0f, p.border);

    // Inside field: URL text and right-aligned muted page title
    const float text_baseline = 55.0f;
    const float pad_x = 24.0f;
    const float inner_left = field.x + pad_x;
    const float inner_right = field.x + field.w - pad_x;
    const float inner_w = inner_right - inner_left;

    float title_w = 0.0f;
    bool show_title = false;
    if (!title_.empty())
    {
        title_w = fonts.regular.font->measure(title_, 20.0f);
        // Room for title if at least 250 px remains for the URL plus 40 px gap
        if (title_w > 0.0f && (title_w + 290.0f) <= inner_w)
            show_title = true;
    }

    if (show_title)
    {
        ui::text(list, fonts.regular, title_, inner_right, text_baseline, 20.0f, p.text_muted, Align::right);
    }

    const float max_url_w = show_title ? (inner_w - title_w - 40.0f) : inner_w;
    const std::string fitted_url = fonts.regular.font->fit(url_, 20.0f, max_url_w);
    ui::text(list, fonts.regular, fitted_url, inner_left, text_baseline, 20.0f, p.text);

    // While loading: a thin (4 px) accent bar under the field that moves left to right and loops
    if (loading_)
    {
        const float bar_w = 300.0f;
        const float period = 1.4f;
        const float t = std::fmod(loading_age_, period) / period;
        const float cur_x = (field.x - bar_w) + t * (field.w + bar_w);
        list.push_clip({field.x, 78.0f, field.w, 4.0f});
        list.rounded_rect({cur_x, 78.0f, bar_w, 4.0f}, 2.0f, p.accent);
        list.pop_clip();
    }

    // 3. Page area: x 0, y 96, w 1920, h 984
    const Rect page_rect{0.0f, 96.0f, 1920.0f, 984.0f};
    if (tex_ != 0 && state_ == 2)
    {
        list.image(tex_, page_rect, Rect{0.0f, 0.0f, u1_, v1_}, kWhite);
    }
    else
    {
        list.rounded_rect(page_rect, 0.0f, p.surface);
        if (!message_.empty())
        {
            ui::text(list, fonts.display, message_, page_rect.cx(), 560.0f, page::kTitleSize,
                     p.text_muted, Align::center);
        }
        if (state_ == 1) // STARTING
        {
            // Small spinner or pulsing dot under the message
            const float pulse = 0.5f + 0.5f * std::sin(age_ * 5.0f);
            const float dot_r = 7.0f + 3.0f * pulse;
            const float dot_cy = 620.0f;
            list.circle(page_rect.cx(), dot_cy, dot_r, p.accent.with_alpha(0.7f + 0.3f * pulse));
            list.glow({page_rect.cx() - dot_r, dot_cy - dot_r, 2.0f * dot_r, 2.0f * dot_r},
                      dot_r, 12.0f, p.accent.with_alpha(0.5f * pulse));
        }
    }

    // 4. Pointer: when pointer_visible, about 32 px tall with dark outline
    if (pointer_visible_)
    {
        const float px = static_cast<float>(pointer_x_);
        const float py = static_cast<float>(pointer_y_);
        if (pointer_hand_)
            draw_hand_pointer(list, px, py);
        else
            draw_arrow_pointer(list, px, py);
    }

    // 5. Hints (only if hints_visible)
    if (hints_visible_)
    {
        list.rounded_rect({0.0f, 1012.0f, 1920.0f, 68.0f}, 0.0f, Color::rgb(0x000000, 0.65f));

        std::vector<ui::Hint> hints;
        hints.push_back({ui::Button::cross, "Click"});
        hints.push_back({ui::Button::square, "Address"});
        /* L1 also goes back, but Circle does the same and is the hint every
         * page shows; only Forward gets its own. */
        if (can_forward_)
            hints.push_back({ui::Button::r1, "Forward"});
        hints.push_back({ui::Button::triangle, "Reload"});
        hints.push_back({ui::Button::circle, "Back"});

        draw_page_hints(list, fonts, hints.data(), static_cast<int>(hints.size()));
    }
}

} // namespace evo::kit
