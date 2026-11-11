/*
 * evo_hui_screens.cpp - EVO's screens on ps5-homebrew-ui. See the header.
 *
 * The look follows the kit's Aurora Shelf design (third_party/ps5-homebrew-ui
 * src/concepts/aurora.cpp): a hero block for the title you were watching, 16:9
 * shelves under it, one focus ring that glides between whatever has the
 * cursor, and entrances that stagger in rather than pop.
 */
#include "evo_hui_screens.hpp"
#include "evo_hui_cover.hpp"

#include "evo_features.h"
#include "evo_browser_engine.h"

#include "ui/glyphs.hpp"
#include "ui/components/component.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../../stb_image.h"
#pragma clang diagnostic pop

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

/* EVO theme words are 0xAABBGGRR. */
Color from_evo(std::uint32_t c, bool use_alpha)
{
    const float a = use_alpha ? static_cast<float>((c >> 24) & 0xff) / 255.0f : 1.0f;
    return {static_cast<float>(c & 0xff) / 255.0f, static_cast<float>((c >> 8) & 0xff) / 255.0f,
            static_cast<float>((c >> 16) & 0xff) / 255.0f, a};
}

/* Relative luminance and WCAG contrast of opaque colours. */
float rel_lum(const Color &c)
{
    auto lin = [](float v) { return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
}
float contrast(const Color &a, const Color &b)
{
    const float la = rel_lum(a), lb = rel_lum(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}
Color over(const Color &fg, const Color &bg)
{
    return {fg.r * fg.a + bg.r * (1 - fg.a), fg.g * fg.a + bg.g * (1 - fg.a), fg.b * fg.a + bg.b * (1 - fg.a), 1.0f};
}
/* Sub text must read on the brightest background it sits on (a selected row
 * or tile), so a theme's dim "muted" grey is lifted toward its secondary text
 * colour until it reaches 4.5:1 there (#137). */
Color readable(const Color &dim, const Color &strong, const Color &bg, const Color &surface_sel,
               const Color &surface)
{
    const Color lit[3] = {over(surface_sel, bg), over(Color::rgb(0xffffff, 0.16f), over(surface, bg)), bg};
    for (int step = 0; step <= 10; ++step)
    {
        const float t = static_cast<float>(step) / 10.0f;
        const Color c{dim.r + (strong.r - dim.r) * t, dim.g + (strong.g - dim.g) * t,
                      dim.b + (strong.b - dim.b) * t, 1.0f};
        bool ok = true;
        for (const Color &l : lit)
            ok = ok && contrast(c, l) >= 4.5f;
        if (ok)
            return c;
    }
    return strong;
}

/* A one-line string cut to width with an ellipsis. */
std::string fit(const ui::FontRef &font, const std::string &text, float size, float width)
{
    return font.font->fit(text, size, width);
}

} // namespace

/* ---------------------------------------------------------------- textures */

TextureCache::~TextureCache()
{
    clear();
}

void TextureCache::clear()
{
    for (auto &kv : entries_)
        host_.release_texture(kv.second.handle);
    entries_.clear();
}

std::uint32_t TextureCache::pixels(const std::uint32_t *data, int width, int height,
                                   std::uint64_t tag)
{
    if (!data || width <= 0 || height <= 0)
        return 0;
    /* The cover service decodes into its buffers after handing them out, so
     * the same pointer first holds black and later the picture. A sparse hash
     * of the pixels goes into the key: new content, new upload. */
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::uint64_t sample = 1469598103934665603ull;
    for (std::size_t i = 0; i < count; i += 61)
        sample = (sample ^ data[i]) * 1099511628211ull;
    char key[128];
    std::snprintf(key, sizeof(key), "px:%p:%dx%d:%llx:%llx", static_cast<const void *>(data), width,
                  height, static_cast<unsigned long long>(tag),
                  static_cast<unsigned long long>(sample));
    auto it = entries_.find(key);
    if (it != entries_.end())
    {
        it->second.idle_frames = 0;
        return it->second.handle;
    }
    Entry e;
    e.handle = host_.create_texture(width, height, reinterpret_cast<const std::uint8_t *>(data));
    entries_[key] = e;
    return e.handle;
}

std::uint32_t TextureCache::icon(const std::string &path)
{
    if (path.empty())
        return 0;
    const std::string key = "icon:" + path;
    auto it = entries_.find(key);
    if (it != entries_.end())
    {
        it->second.idle_frames = 0;
        return it->second.handle;
    }
    Entry e;
    std::string bytes;
    if (host_.read_asset(path, &bytes))
    {
        int w = 0, h = 0, n = 0;
        unsigned char *rgba =
            stbi_load_from_memory(reinterpret_cast<const unsigned char *>(bytes.data()),
                                  static_cast<int>(bytes.size()), &w, &h, &n, 4);
        if (rgba)
        {
            e.handle = host_.create_texture(w, h, rgba);
            stbi_image_free(rgba);
        }
    }
    /* A failed load is cached too (handle 0), so a missing icon is not
     * re-read every frame. */
    entries_[key] = e;
    return e.handle;
}

void TextureCache::tick()
{
    /* Icons stay; covers that left the screen go after about two seconds. */
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        if (it->first.compare(0, 3, "px:") == 0 && ++it->second.idle_frames > 120)
        {
            host_.release_texture(it->second.handle);
            it = entries_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

/* ----------------------------------------------------------------- palette */

ui::Theme Palette::kit_theme() const
{
    ui::Theme t = ui::default_theme();
    t.surface = surface;
    t.surface_high = Color::rgb(0xffffff, 0.16f);
    t.text = text;
    t.text_muted = text_muted;
    t.primary = Color::rgb(0xffffff);
    t.on_primary = Color::rgb(0x0b0d16);
    t.accent = accent;
    t.outline = border;
    return t;
}

Palette Palette::from(const evo_rmlui_theme_t &t)
{
    Palette p;
    p.bg_top = from_evo(t.bg_top, false);
    p.bg_bottom = from_evo(t.bg_bottom, false);
    p.surface = from_evo(t.surface, true);
    p.surface_sel = from_evo(t.surface_sel, true);
    p.border = from_evo(t.border, true);
    p.accent = from_evo(t.accent, false);
    p.accent_soft = from_evo(t.accent_soft, false);
    p.accent_alt = from_evo(t.accent_alt, false);
    p.text = from_evo(t.text_primary, false);
    p.text_muted = from_evo(t.text_secondary, false);
    p.text_faint = readable(from_evo(t.text_muted, false), p.text_muted, p.bg_top, p.surface_sel, p.surface);
    return p;
}

/* ---------------------------------------------------------------- backdrop */

void draw_backdrop(DrawList &list, const Context &ctx, std::uint32_t picture, float picture_alpha)
{
    const Palette &p = ctx.palette;
    const Rect screen{0, 0, ::hui::gfx::kVirtualWidth, ::hui::gfx::kVirtualHeight};
    list.gradient_rect(screen, 0, p.bg_top, p.bg_bottom);

    /* The picture, washed in from the right: the left side stays dark for
     * the text that sits on it. */
    if (picture && picture_alpha > 0.0f)
    {
        list.image(picture, screen, ::hui::gfx::kFullUv, kWhite.with_alpha(0.55f * picture_alpha));
        list.gradient_rect_h(screen, 0, p.bg_bottom.with_alpha(0.97f), p.bg_bottom.with_alpha(0.35f));
        list.gradient_rect({0, 540, screen.w, 540}, 0, p.bg_bottom.with_alpha(0.0f),
                           p.bg_bottom.with_alpha(0.92f));
    }

    /* Aurora: three large soft lights that drift on slow, unrelated periods. */
    const float t = ctx.time;
    struct Light
    {
        float x, y, r, period, phase;
        Color c;
    };
    const Light lights[] = {
        {1500, 160, 360, 23.0f, 0.0f, p.accent.with_alpha(0.16f)},
        {420, 900, 420, 31.0f, 2.1f, p.accent_alt.with_alpha(0.12f)},
        {1200, 980, 300, 19.0f, 4.2f, p.accent_soft.with_alpha(0.14f)},
    };
    for (const Light &l : lights)
    {
        const float a = 6.2831853f * t / l.period + l.phase;
        const float x = l.x + 120.0f * std::sin(a);
        const float y = l.y + 70.0f * std::cos(a * 0.7f);
        list.shadow({x - l.r, y - l.r, 2 * l.r, 2 * l.r}, l.r, l.r * 0.9f, l.c);
    }
}

/* ------------------------------------------------------------ page chrome */

float draw_page_title(DrawList &list, const Context &ctx, const std::string &title, const std::string &subtitle,
                      float x, float dy, float max_w)
{
    const ui::Fonts &fonts = ctx.fonts;
    const float w = ui::text(list, fonts.display, fit(fonts.display, title, page::kTitleSize, max_w), x - 2,
                             page::kTitleY + dy, page::kTitleSize, kWhite);
    if (!subtitle.empty())
        ui::text(list, fonts.semibold, fit(fonts.semibold, ui::upper(subtitle), 18, max_w), x,
                 page::kSubtitleY + dy, 18, ctx.palette.text_muted, Align::left, 3.0f);
    return w;
}

void draw_page_counter(DrawList &list, const Context &ctx, const std::string &text, float dy)
{
    ui::text(list, ctx.fonts.regular, text, page::kRight, page::kTitleY + dy, 20, ctx.palette.text_faint,
             Align::right, 2.0f);
}

void draw_page_hints(DrawList &list, const ui::Fonts &fonts, const ui::Hint *hints, int n, float right)
{
    ui::Hint sorted[12];
    int m = 0;
    const auto take = [&](bool want_cross, bool want_circle) {
        for (int i = 0; i < n && m < 12; ++i)
        {
            const bool cross = hints[i].button == ui::Button::cross;
            const bool circle = hints[i].button == ui::Button::circle;
            if (cross == want_cross && circle == want_circle)
                sorted[m++] = hints[i];
        }
    };
    take(true, false);
    take(false, false);
    take(false, true);
    ui::HintLayout layout;
    layout.size = 30;
    layout.text_size = 20;
    layout.cy = page::kHintsY;
    ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), sorted, m, right, true, layout);
}

/* ----------------------------------------------------------------- the rail */

namespace
{

struct RailItem
{
    const char *label;
    const char *icon;
};

/* ScreenManager's section order; Providers only when the build has them. */
int rail_items(RailItem *out)
{
    int n = 0;
    out[n++] = {"Home", "../icons/icon_home.png"};
    out[n++] = {"Browse", "../icons/icon_browse_usb.png"};
    if (EVO_ENABLE_EMBY)
        out[n++] = {"Providers", "../icons/icon_emby.png"};
    if (evo_settings_web_browser_enabled())
        out[n++] = {"Web", "../icons/icon_globe.png"}; /* #237 */
    out[n++] = {"Settings", "../icons/icon_settings.png"};
    out[n++] = {"About", "../icons/icon_about_support.png"};
    out[n++] = {"Quit EVO", "../icons/icon_power.png"};
    return n;
}

constexpr float kRailTop = 300.0f;
constexpr float kRailPitch = 88.0f;
constexpr float kRailExpanded = 360.0f;

float rail_item_y(int index, int count)
{
    /* Quit sits apart at the foot of the rail. */
    if (index == count - 1)
        return 1080.0f - 150.0f;
    return kRailTop + static_cast<float>(index) * kRailPitch;
}

} // namespace

void NavRail::update(float dt)
{
    expand_.target = params_.rail_focused ? 1.0f : 0.0f;
    expand_.update(dt, 16.0f);
    RailItem items[8];
    const int count = rail_items(items);
    const int at = params_.rail_focused ? params_.cursor_index : params_.active_section;
    const int index = std::clamp(at, 0, count - 1);
    const float width = tween::lerp(72.0f, kRailExpanded - 56.0f, expand_.value);
    const Rect target{28.0f, rail_item_y(index, count), width, 72.0f};
    if (!cursor_snapped_)
    {
        cursor_.snap(target);
        cursor_snapped_ = true;
    }
    cursor_.target(target);
    cursor_.update(dt, 22.0f);
}

void NavRail::draw(DrawList &list, const Context &ctx) const
{
    if (!params_.visible)
        return;
    const Palette &p = ctx.palette;
    const ui::Fonts &fonts = ctx.fonts;
    const float e = expand_.value;
    RailItem items[8];
    const int count = rail_items(items);

    /* Scrim over the content while the rail is open. */
    if (e > 0.01f)
        list.rounded_rect({0, 0, 1920, 1080}, 0, Color::rgb(0x000000, 0.5f * e));

    const float width = tween::lerp(kWidth, kRailExpanded, e);
    const Rect panel{0, 0, width, 1080};
    list.gradient_rect_h(panel, 0, p.bg_bottom.with_alpha(0.94f + 0.05f * e),
                         p.bg_bottom.with_alpha(0.7f + 0.27f * e));
    list.rounded_rect({width - 1.5f, 0, 1.5f, 1080}, 0, kWhite.with_alpha(0.07f));

    /* Logo. */
    const std::uint32_t logo = ctx.textures.icon("../icons/icon_logo.png");
    if (logo)
        list.image(logo, {36, 64, 56, 56}, ::hui::gfx::kFullUv, kWhite);
    if (e > 0.01f)
    {
        list.push_opacity(e);
        ui::text(list, fonts.display, "EVO", 112, 104, 34, kWhite);
        list.pop_opacity();
    }

    /* The highlight: a pill that glides to the cursor (rail open) or rests on
     * the active section (rail closed). */
    const Rect pill = cursor_.value();
    if (params_.rail_focused)
    {
        list.glow(pill, 36, 18, p.accent.with_alpha(0.45f));
        list.rounded_rect(pill, 36, p.accent);
    }
    else
    {
        list.rounded_rect(pill, 36, kWhite.with_alpha(0.10f));
    }

    for (int i = 0; i < count; ++i)
    {
        const float y = rail_item_y(i, count);
        const bool cursor = params_.rail_focused && i == params_.cursor_index;
        const bool active = i == params_.active_section;
        const Color ink = cursor ? kInk : (active ? kWhite : kWhite.with_alpha(0.62f));
        const std::uint32_t icon = ctx.textures.icon(items[i].icon);
        if (icon)
            list.image(icon, {48, y + 20, 32, 32}, ::hui::gfx::kFullUv, ink);
        if (!params_.rail_focused && active)
            list.rounded_rect({8, y + 22, 4, 28}, 2, p.accent);
        if (e > 0.01f)
        {
            list.push_opacity(tween::clamp01(e * 1.6f - 0.4f));
            ui::text(list, cursor ? fonts.semibold : fonts.regular, items[i].label, 104, y + 46, 26,
                     ink);
            list.pop_opacity();
        }
    }

    if (params_.show_fps)
    {
        char fps[24];
        std::snprintf(fps, sizeof(fps), "%d FPS", params_.fps);
        const Rect chip{1920 - 180, 1080 - 64, 140, 40};
        list.rounded_rect(chip, 20, Color::rgb(0x000000, 0.55f));
        ui::text(list, fonts.mono, fps, chip.cx(), chip.y + 28, 20, p.accent, Align::center);
    }
}

/* ------------------------------------------------------------------- home */

namespace
{

constexpr float kLeft = NavRail::kWidth + 64.0f; /* content's left edge */
constexpr float kCardW = 320.0f;
constexpr float kCardH = 180.0f; /* the cover service's 16:9 posters */
constexpr float kCardGap = 30.0f;
constexpr float kRecentY = 636.0f;
constexpr float kLibraryY = 920.0f;
constexpr float kLibW = 250.0f;
constexpr float kLibH = 96.0f;
constexpr float kLibGap = 22.0f;
constexpr float kShelfView = 1824.0f - kLeft; /* visible shelf width */
const Rect kHeroArt{1176, 164, 640, 360};
const Rect kHeroAction{kLeft, 452, 260, 64};

} // namespace

void HomeScreen::set(const evo_rmlui_launch_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    app_name_ = str(p.app_name);
    version_ = str(p.version);
    clock_ = str(p.clock);

    Hero next;
    next.eyebrow = str(p.hero_eyebrow);
    next.title = str(p.hero_title);
    next.detail = str(p.hero_detail);
    next.action = str(p.hero_action);
    next.progress = p.hero_progress;
    next.art = p.hero_art;
    next.art_w = p.hero_art_w;
    next.art_h = p.hero_art_h;
    if (next.title != hero_.title && !hero_.title.empty())
    {
        previous_hero_ = hero_;
        hero_fade_.start(0.45f);
    }
    hero_ = next;
    hero_focused_ = p.hero_focused != 0;

    recent_total_ = p.recent_total;
    recent_cursor_ = p.recent_cursor;
    auto tile = [&](const evo_rmlui_launch_tile_t &in) {
        Tile t;
        t.title = str(in.title);
        t.detail = str(in.detail);
        t.icon_path = str(in.icon_path);
        t.progress = in.progress;
        t.art = in.art;
        t.art_w = in.art_w;
        t.art_h = in.art_h;
        t.focused = in.is_focused != 0;
        return t;
    };
    recent_.clear();
    for (int i = 0; i < p.recent_visible && i < EVO_RMLUI_TILES; ++i)
        recent_.push_back(tile(p.recent[i]));
    library_.clear();
    for (int i = 0; i < p.library_visible && i < EVO_RMLUI_TILES; ++i)
        library_.push_back(tile(p.library[i]));
    tips_.clear();
    for (int i = 0; i < p.tip_count && i < EVO_RMLUI_HOME_TIPS; ++i)
        if (p.tips[i] && p.tips[i][0])
            tips_.push_back(p.tips[i]);
}

void HomeScreen::enter()
{
    age_ = 0.0f;
    ring_snapped_ = false;
}

Rect HomeScreen::recent_rect(int index) const
{
    return {kLeft + static_cast<float>(index) * (kCardW + kCardGap) - recent_scroll_.offset(), kRecentY,
            kCardW, kCardH};
}

Rect HomeScreen::library_rect(int index) const
{
    return {kLeft + static_cast<float>(index) * (kLibW + kLibGap), kLibraryY, kLibW, kLibH};
}

Rect HomeScreen::focus_rect() const
{
    if (hero_focused_)
        return kHeroAction;
    for (std::size_t i = 0; i < recent_.size(); ++i)
        if (recent_[i].focused)
        {
            /* Where the card lands once the shelf stops, not where it is: the
             * ring would otherwise trail the scroll. */
            Rect r = recent_rect(static_cast<int>(i));
            r.x += recent_scroll_.offset() - recent_scroll_.position.target;
            return {r.x - 10, r.y - 10, r.w + 20, r.h + 20};
        }
    for (std::size_t i = 0; i < library_.size(); ++i)
        if (library_[i].focused)
            return library_rect(static_cast<int>(i));
    return {};
}

void HomeScreen::update(float dt)
{
    age_ += dt;
    hero_fade_.update(dt);
    if (recent_cursor_ >= 0 && recent_cursor_ < static_cast<int>(recent_.size()))
    {
        const float start = static_cast<float>(recent_cursor_) * (kCardW + kCardGap);
        const float limit = static_cast<float>(recent_.size()) * (kCardW + kCardGap) - kCardGap;
        recent_scroll_.reveal(start, start + kCardW, kShelfView, kCardW * 0.6f, limit);
    }
    recent_scroll_.update(dt, 12.0f);
    const Rect target = focus_rect();
    if (!ring_snapped_ && target.w > 0.0f)
    {
        ring_.snap(target);
        recent_scroll_.position.snap(recent_scroll_.position.target);
        ring_snapped_ = true;
    }
    if (target.w > 0.0f)
        ring_.target(target);
    ring_.update(dt, 20.0f);
    row_glow_.target = target.w > 0.0f ? 1.0f : 0.0f;
    row_glow_.update(dt, 10.0f);
}

std::uint32_t HomeScreen::art_texture(const Context &ctx, const std::uint32_t *art, int w, int h,
                                      const std::string &tag) const
{
    if (!art)
        return 0;
    /* The hero buffer is reused for every title, so its key carries the title. */
    std::uint64_t hash = 1469598103934665603ull;
    for (char c : tag)
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    return ctx.textures.pixels(art, w, h, hash);
}

void HomeScreen::draw_hero(DrawList &list, const Context &ctx, const Hero &hero, float alpha,
                           float slide) const
{
    if (alpha <= 0.01f)
        return;
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    list.push_opacity(alpha);

    /* Artwork, floating, with a glow in the accent colour. */
    const float bob = std::sin(ctx.time * 0.8f) * 5.0f;
    const Rect art{kHeroArt.x + slide * 1.5f, kHeroArt.y + bob, kHeroArt.w, kHeroArt.h};
    list.glow(art.inset(24), 40, 80, p.accent.with_alpha(0.28f));
    list.shadow({art.x, art.y + 24, art.w, art.h}, 30, 44, Color::rgb(0x000000, 0.6f));
    const std::uint32_t tex = art_texture(ctx, hero.art, hero.art_w, hero.art_h, hero.title);
    if (tex)
    {
        draw_art_fit(list, tex, hero.art_w, hero.art_h, art, page::kRadiusCard, kWhite, p.surface_sel, p.surface);
    }
    else
    {
        list.gradient_rect(art, page::kRadiusCard, p.accent_soft, p.bg_bottom);
        const std::uint32_t logo = ctx.textures.icon("../icons/icon_logo.png");
        if (logo)
            list.image(logo, {art.cx() - 56, art.cy() - 56, 112, 112}, ::hui::gfx::kFullUv,
                       kWhite.with_alpha(0.85f));
    }
    list.bordered_rect(art, page::kRadiusCard, Color::rgb(0x000000, 0.0f), 1.5f, kWhite.with_alpha(0.16f));

    const float x = kLeft + slide;
    ui::text(list, fonts.semibold, ui::upper(hero.eyebrow), x, 196, 20, p.accent, Align::left, 4.0f);
    /* Show the whole title: shrink it to fit before resorting to an ellipsis. */
    float title_size = 76.0f;
    while (title_size > 44.0f && fonts.display.font->measure(hero.title, title_size) > 940.0f)
        title_size -= 4.0f;
    ui::text(list, fonts.display, fit(fonts.display, hero.title, title_size, 940), x - 3, 282, title_size, kWhite);
    ui::text(list, fonts.regular, fit(fonts.regular, hero.detail, 24, 940), x, 330, 24,
             p.text_faint);

    if (hero.progress >= 0)
    {
        const float t = tween::clamp01(static_cast<float>(hero.progress) / 1000.0f);
        const Rect bar{x, 382, 420, 8};
        list.rounded_rect(bar, 4, kWhite.with_alpha(0.16f));
        list.rounded_rect({bar.x, bar.y, std::max(8.0f, bar.w * t), bar.h}, 4, p.accent);
        char pct[32];
        std::snprintf(pct, sizeof(pct), "%d%% watched", static_cast<int>(t * 100.0f + 0.5f));
        ui::text(list, fonts.regular, pct, bar.x + bar.w + 24, 394, 22, p.text_muted);
    }

    if (!hero.action.empty())
    {
        const Rect play{x, kHeroAction.y, kHeroAction.w, kHeroAction.h};
        if (hero_focused_)
            list.glow(play, 32, 22, p.accent.with_alpha(0.45f));
        list.rounded_rect(play, 32, hero_focused_ ? kWhite : kWhite.with_alpha(0.14f));
        ui::draw_button(list, fonts, hero_focused_ ? ui::GlyphStyle::light() : ui::GlyphStyle::dark(),
                        ui::Button::cross, play.x + 20, play.cy(), 34);
        ui::text(list, fonts.semibold, ui::upper(hero.action), play.x + 68, play.cy() + 9, 24,
                 hero_focused_ ? kInk : kWhite, Align::left, 2.0f);
    }
    list.pop_opacity();
}

void HomeScreen::draw(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const std::uint32_t hero_tex = art_texture(ctx, hero_.art, hero_.art_w, hero_.art_h, hero_.title);
    draw_backdrop(list, ctx, hero_tex, 1.0f);

    /* Top right: clock and version. */
    {
        const float in = tween::stagger(age_, 0, 0.05f, 0.5f);
        list.push_opacity(in);
        if (!clock_.empty())
            ui::text(list, fonts.semibold, clock_, 1824, 92, 30, kWhite, Align::right);
        if (!version_.empty())
            ui::text(list, fonts.regular, version_, 1824, 124, 18, p.text_faint, Align::right, 2.0f);
        list.pop_opacity();
    }

    /* Hero, cross-fading when the title changes. */
    {
        const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
        list.push_opacity(in);
        if (hero_fade_.running)
        {
            const float t = hero_fade_.progress();
            draw_hero(list, ctx, previous_hero_, 1.0f - tween::smoothstep(t * 2.2f),
                      -36.0f * tween::cubic_in(tween::clamp01(t * 2.2f)));
            const float arrive = tween::clamp01((t - 0.25f) / 0.75f);
            draw_hero(list, ctx, hero_, tween::smoothstep(arrive),
                      44.0f * (1.0f - tween::quint_out(arrive)));
        }
        else
        {
            draw_hero(list, ctx, hero_, 1.0f, 30.0f * (1.0f - in));
        }
        list.pop_opacity();
    }

    /* The focus ring's light goes under the cards (a glow fills its inside). */
    if (row_glow_.value > 0.01f && !hero_focused_)
    {
        list.push_opacity(row_glow_.value);
        list.glow(ring_.value(), 24, 22, p.accent.with_alpha(0.35f + 0.12f * ui::breathe(ctx.time)));
        list.pop_opacity();
    }

    /* Jump back in. Clipped at the rail: the shelf scrolls under it. */
    if (!recent_.empty())
    {
        list.push_clip({NavRail::kWidth, 0, ::hui::gfx::kVirtualWidth - NavRail::kWidth,
                        ::hui::gfx::kVirtualHeight});
        const float in = tween::stagger(age_, 3, 0.09f, 0.6f);
        list.push_opacity(in);
        const float dy = 40.0f * (1.0f - in);
        ui::text(list, fonts.semibold, "JUMP BACK IN", kLeft, kRecentY - 30 + dy, 20,
                 recent_cursor_ >= 0 ? kWhite : p.text_muted, Align::left, 3.0f);
        if (recent_total_ > static_cast<int>(recent_.size()) || recent_cursor_ >= 0)
        {
            char counter[32];
            if (recent_cursor_ >= 0)
                std::snprintf(counter, sizeof(counter), "%d OF %d", recent_cursor_ + 1, recent_total_);
            else
                std::snprintf(counter, sizeof(counter), "%d ITEMS", recent_total_);
            ui::text(list, fonts.regular, counter, 1824, kRecentY - 30 + dy, 18, p.text_faint,
                     Align::right, 2.0f);
        }
        for (std::size_t i = 0; i < recent_.size(); ++i)
        {
            const Tile &t = recent_[i];
            Rect r = recent_rect(static_cast<int>(i));
            r.y += dy;
            if (t.focused)
                r = {r.x - 10, r.y - 10, r.w + 20, r.h + 20};
            list.shadow({r.x, r.y + 14, r.w, r.h}, 20, 24, Color::rgb(0x000000, 0.5f));
            const std::uint32_t tex = art_texture(ctx, t.art, t.art_w, t.art_h, std::string());
            if (tex)
            {
                draw_art_fit(list, tex, t.art_w, t.art_h, r, page::kRadiusTile, kWhite.with_alpha(t.focused ? 1.0f : 0.82f), p.surface_sel,
                             p.surface);
            }
            else
            {
                list.gradient_rect(r, page::kRadiusTile, p.surface_sel, p.surface);
                const std::uint32_t icon = ctx.textures.icon(t.icon_path);
                if (icon)
                    list.image(icon, {r.cx() - 28, r.cy() - 40, 56, 56}, ::hui::gfx::kFullUv,
                               kWhite.with_alpha(0.5f));
            }
            /* Title on a scrim at the foot of the card. */
            list.gradient_rect({r.x, r.y + r.h * 0.45f, r.w, r.h * 0.55f}, page::kRadiusTile,
                               Color::rgb(0x000000, 0.0f), Color::rgb(0x000000, 0.78f));
            ui::text(list, t.focused ? fonts.semibold : fonts.regular,
                     fit(fonts.regular, t.title, 22, r.w - 36), r.x + 18, r.y + r.h - 24, 22, kWhite);
            if (t.progress >= 0)
            {
                const float f = tween::clamp01(static_cast<float>(t.progress) / 1000.0f);
                const Rect bar{r.x + 18, r.y + r.h - 12, r.w - 36, 4};
                list.rounded_rect(bar, 2, kWhite.with_alpha(0.22f));
                list.rounded_rect({bar.x, bar.y, std::max(4.0f, bar.w * f), bar.h}, 2, p.accent);
            }
        }
        list.pop_opacity();
        list.pop_clip();
    }

    /* Library. */
    {
        const float in = tween::stagger(age_, 4, 0.09f, 0.6f);
        list.push_opacity(in);
        const float dy = 40.0f * (1.0f - in);
        bool any_focus = false;
        for (const Tile &t : library_)
            any_focus = any_focus || t.focused;
        ui::text(list, fonts.semibold, "LIBRARY", kLeft, kLibraryY - 26 + dy, 20,
                 any_focus ? kWhite : p.text_muted, Align::left, 3.0f);
        for (std::size_t i = 0; i < library_.size(); ++i)
        {
            const Tile &t = library_[i];
            Rect r = library_rect(static_cast<int>(i));
            r.y += dy;
            list.rounded_rect(r, page::kRadiusTile, t.focused ? kWhite.with_alpha(0.16f) : p.surface);
            list.bordered_rect(r, page::kRadiusTile, Color::rgb(0x000000, 0.0f), 1.0f, kWhite.with_alpha(0.08f));
            const Rect well{r.x + 16, r.y + 22, 52, 52};
            list.rounded_rect(well, page::kRadiusChip, t.focused ? p.accent : kWhite.with_alpha(0.08f));
            const std::uint32_t icon = ctx.textures.icon(t.icon_path);
            if (icon)
                list.image(icon, well.inset(13), ::hui::gfx::kFullUv, t.focused ? kInk : kWhite);
            ui::text(list, fonts.semibold, fit(fonts.semibold, t.title, 22, r.w - 98), r.x + 82,
                     r.y + 44, 22, kWhite);
            ui::text(list, fonts.regular, fit(fonts.regular, t.detail, 18, r.w - 98), r.x + 82,
                     r.y + 72, 18, p.text_muted);
        }
        list.pop_opacity();
    }

    /* The focus ring glides between hero button, covers and library tiles. */
    if (row_glow_.value > 0.01f && !hero_focused_)
    {
        const Rect ring = ring_.value();
        list.push_opacity(row_glow_.value);
        list.bordered_rect(ring.inset(-5), page::kRadiusTile + 5, Color::rgb(0x000000, 0.0f), 4, kWhite);
        list.pop_opacity();
    }

    /* Hints. */
    {
        list.push_opacity(tween::stagger(age_, 6, 0.08f, 0.5f));
        const ui::Hint hints[] = {{ui::Button::cross, "Select"}, {ui::Button::circle, "Back"}};
        draw_page_hints(list, fonts, hints, 2);
        list.pop_opacity();
    }

    /* "Did you know": one tip at a time on the hint line's left, 8 s each,
     * fading between them. Starts after the page's own entrance. */
    if (!tips_.empty())
    {
        constexpr float kPeriod = 8.0f, kFade = 0.6f;
        const float t = std::max(0.0f, age_ - 1.0f);
        const int idx = static_cast<int>(t / kPeriod) % static_cast<int>(tips_.size());
        const float in = std::fmod(t, kPeriod);
        const float a = std::min({1.0f, in / kFade, (kPeriod - in) / kFade}) * tween::stagger(age_, 7, 0.08f, 0.5f);
        list.push_opacity(std::max(0.0f, a));
        const float y = page::kHintsY;
        float x = kLeft;
        if (uint32_t bulb = ctx.textures.icon("../icons/icon_lightbulb.png"))
        {
            list.image(bulb, {x, y - 14.0f, 28, 28}, ::hui::gfx::kFullUv, p.accent);
            x += 40.0f;
        }
        ui::text(list, fonts.semibold, "DID YOU KNOW", x, y + 6.0f, 15, p.accent, Align::left, 2.0f);
        x += fonts.semibold.measure("DID YOU KNOW", 15, 2.0f) + 18.0f;
        /* Stop well short of the button hints on the right. */
        const float w = 1500.0f - x;
        ui::text(list, fonts.regular, fonts.regular.font->fit(tips_[idx], 21, w), x, y + 7.0f, 21, kWhite.with_alpha(0.85f));
        list.pop_opacity();
    }
}

/* -------------------------------------------------------------------- list */

namespace
{

constexpr float kListTop = 220.0f;
constexpr float kListRowH = 76.0f;
constexpr float kListGap = 8.0f;
constexpr float kListRight = 1824.0f;
constexpr float kPanelW = 600.0f; /* details card beside the rows, when there is one */
constexpr float kMenuW = 640.0f;
constexpr float kMenuItemH = 64.0f;
const Color kDanger = Color::rgb(0xff5a5f);

/* The RmlUi documents name a hint by its glyph file. */
ui::Button button_for(const std::string &glyph)
{
    struct Map
    {
        const char *name;
        ui::Button button;
    };
    static const Map map[] = {{"btn_cross", ui::Button::cross},     {"btn_circle", ui::Button::circle},
                              {"btn_square", ui::Button::square},   {"btn_triangle", ui::Button::triangle},
                              {"btn_dpad", ui::Button::dpad},       {"btn_lstick", ui::Button::left_stick},
                              {"btn_options", ui::Button::options}, {"icon_settings", ui::Button::options},
                              {"options", ui::Button::options},      {"btn_touchpad", ui::Button::touchpad}};
    for (const Map &m : map)
        if (glyph.find(m.name) != std::string::npos)
            return m.button;
    return ui::Button::none;
}

/* "SELECT" -> "Select": the kit's hints are sentence case. */
std::string title_case(const std::string &s)
{
    std::string out = s;
    bool start = true;
    for (char &c : out)
    {
        if (c >= 'A' && c <= 'Z' && !start)
            c = static_cast<char>(c - 'A' + 'a');
        start = (c == ' ');
    }
    return out;
}

} // namespace

void ListScreen::set(const evo_rmlui_list_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    if (str(p.title) != title_)
    {
        age_ = 0.0f; /* a different list: run the entrance again */
        ring_snapped_ = false;
    }
    title_ = str(p.title);
    subtitle_ = str(p.subtitle);
    total_ = p.total_count;
    cursor_ = p.cursor_index;
    rows_.clear();
    for (int i = 0; i < p.row_count && i < EVO_RMLUI_LIST_ROWS; ++i)
    {
        Row r;
        r.title = str(p.rows[i].title);
        r.detail = str(p.rows[i].detail);
        r.icon = str(p.rows[i].icon_path);
        r.badge = str(p.rows[i].badge);
        {
            const std::string all = str(p.rows[i].pills);
            for (std::size_t a = 0; a < all.size();)
            {
                std::size_t b = all.find('\t', a);
                if (b == std::string::npos)
                    b = all.size();
                if (b > a)
                    r.pills.push_back(all.substr(a, b - a));
                a = b + 1;
            }
        }
        r.progress = p.rows[i].progress;
        r.chevron = p.rows[i].has_chevron != 0;
        r.focused = p.rows[i].is_focused != 0;
        rows_.push_back(r);
    }
    empty_ = p.is_empty != 0;
    empty_title_ = str(p.empty_title);
    empty_hint_ = str(p.empty_hint);
    empty_icon_ = str(p.empty_icon);
    hints_.clear();
    for (int i = 0; i < p.hint_count && i < 4; ++i)
    {
        const ui::Button b = button_for(str(p.hints[i].glyph_path));
        if (b != ui::Button::none)
            hints_.emplace_back(static_cast<int>(b), title_case(str(p.hints[i].label)));
    }
    if (menu_.empty() && p.menu_count > 0)
        menu_ring_snapped_ = false;
    menu_.clear();
    for (int i = 0; i < p.menu_count && i < EVO_RMLUI_LIST_MENU_ROWS; ++i)
    {
        MenuItem m;
        m.label = str(p.menu[i].label);
        m.desc = str(p.menu[i].desc);
        m.icon = str(p.menu[i].icon_path);
        m.danger = p.menu[i].danger != 0;
        menu_.push_back(m);
    }
    menu_focus_ = p.menu_focus;
    menu_eyebrow_ = str(p.menu_eyebrow);
    menu_title_ = str(p.menu_title);
    menu_sub_ = str(p.menu_sub);
    menu_icon_ = str(p.menu_icon);
    if (p.loading && !loading_)
        loading_age_ = 0.0f;
    loading_ = p.loading != 0;
    loading_text_ = str(p.loading_text);
    panel_eyebrow_ = str(p.panel_eyebrow);
    panel_title_ = str(p.panel_title);
    panel_.clear();
    for (int i = 0; i < p.panel_count && i < 10; ++i)
        panel_.emplace_back(str(p.panel[i].label), str(p.panel[i].value));
}

void ListScreen::enter()
{
    age_ = 0.0f;
    ring_snapped_ = false;
    menu_open_.snap(menu_.empty() ? 0.0f : 1.0f);
}

Rect ListScreen::row_rect(int index) const
{
    const float right = panel_.empty() ? kListRight : kListRight - kPanelW - 28.0f;
    return {kLeft, kListTop + static_cast<float>(index) * (kListRowH + kListGap), right - kLeft,
            kListRowH};
}

Rect ListScreen::menu_item_rect(int index) const
{
    /* Card centred on the screen; items under its 180 px header. */
    const float h = 180.0f + static_cast<float>(menu_.size()) * (kMenuItemH + 6.0f) + 24.0f;
    const float top = 540.0f - h * 0.5f;
    return {960.0f - kMenuW * 0.5f + 24.0f, top + 180.0f + static_cast<float>(index) * (kMenuItemH + 6.0f),
            kMenuW - 48.0f, kMenuItemH};
}

void ListScreen::update(float dt)
{
    age_ += dt;
    loading_age_ += dt;
    int focused = -1;
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].focused)
            focused = static_cast<int>(i);
    if (focused >= 0)
    {
        const Rect target = row_rect(focused);
        if (!ring_snapped_)
        {
            ring_.snap(target);
            ring_snapped_ = true;
        }
        ring_.target(target);
    }
    ring_.update(dt, 22.0f);
    has_focus_.target = focused >= 0 ? 1.0f : 0.0f;
    has_focus_.update(dt, 12.0f);

    menu_open_.target = menu_.empty() ? 0.0f : 1.0f;
    menu_open_.update(dt, 18.0f);
    if (!menu_.empty())
    {
        const Rect target = menu_item_rect(std::clamp(menu_focus_, 0, static_cast<int>(menu_.size()) - 1));
        if (!menu_ring_snapped_)
        {
            menu_ring_.snap(target);
            menu_ring_snapped_ = true;
        }
        menu_ring_.target(target);
        menu_ring_.update(dt, 24.0f);
    }
}

void ListScreen::draw(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    draw_backdrop(list, ctx);

    /* Header. */
    {
        const float in = tween::stagger(age_, 0, 0.05f, 0.45f);
        list.push_opacity(in);
        const float dy = 16.0f * (1.0f - in);
        draw_page_title(list, ctx, title_, subtitle_, kLeft, dy);
        if (total_ > 0)
        {
            char counter[32];
            if (total_ > static_cast<int>(rows_.size()) && cursor_ >= 0)
                std::snprintf(counter, sizeof(counter), "%d OF %d", cursor_ + 1, total_);
            else
                std::snprintf(counter, sizeof(counter), "%d %s", total_, total_ == 1 ? "ITEM" : "ITEMS");
            draw_page_counter(list, ctx, counter, dy);
        }
        list.pop_opacity();
    }

    if (empty_)
    {
        const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
        list.push_opacity(in);
        const float cx = (kLeft + kListRight) * 0.5f;
        const std::uint32_t icon = ctx.textures.icon(empty_icon_);
        if (icon)
            list.image(icon, {cx - 48, 380, 96, 96}, ::hui::gfx::kFullUv, kWhite.with_alpha(0.4f));
        ui::text(list, fonts.display, empty_title_, cx, 560, 40, kWhite, Align::center);
        ui::paragraph(list, fonts.regular, empty_hint_, cx, 610, 24, 900, 34, p.text_muted, 3,
                      Align::center);
        list.pop_opacity();
    }
    else
    {
        /* The highlight's light goes under the rows. */
        if (has_focus_.value > 0.01f)
        {
            list.push_opacity(has_focus_.value);
            list.glow(ring_.value(), 20, 18, p.accent.with_alpha(0.3f));
            list.pop_opacity();
        }
        for (std::size_t i = 0; i < rows_.size(); ++i)
        {
            const Row &row = rows_[i];
            const float in = tween::stagger(age_, static_cast<int>(i) + 1, 0.03f, 0.4f);
            list.push_opacity(in);
            Rect r = row_rect(static_cast<int>(i));
            r.y += 24.0f * (1.0f - tween::cubic_out(in));
            list.rounded_rect(r, page::kRadiusTile, row.focused ? kWhite.with_alpha(0.14f) : p.surface);
            const Rect well{r.x + 12, r.y + 10, 56, 56};
            const bool brand = is_brand_icon(row.icon);
            list.rounded_rect(well, page::kRadiusChip, row.focused ? (brand ? kWhite.with_alpha(0.92f) : p.accent)
                                                    : kWhite.with_alpha(0.08f));
            const std::uint32_t icon = ctx.textures.icon(row.icon);
            if (icon)
                list.image(icon, well.inset(brand ? 10 : 14), ::hui::gfx::kFullUv,
                           (row.focused && !brand) ? kInk : kWhite);

            float right = r.x + r.w - 24;
            if (row.chevron)
            {
                const std::uint32_t chev = ctx.textures.icon("../icons/icon_chevron.png");
                if (chev)
                    list.image(chev, {right - 24, r.cy() - 12, 24, 24}, ::hui::gfx::kFullUv,
                               kWhite.with_alpha(row.focused ? 0.9f : 0.45f));
                right -= 44;
            }
            if (!row.badge.empty())
            {
                const float w = fonts.semibold.measure(row.badge, 16, 1.5f) + 28;
                const Rect pill{right - w, r.cy() - 16, w, 32};
                if (row.focused)
                    list.bordered_rect(pill, 16, Color::rgb(0x000000, 0.0f), 1.5f, p.accent);
                else
                    list.rounded_rect(pill, 16, kWhite.with_alpha(0.08f));
                ui::text(list, fonts.semibold, row.badge, pill.cx(), pill.y + 22, 16,
                         row.focused ? p.accent : p.text_muted, Align::center, 1.5f);
                right = pill.x - 20;
            }
            const float text_w = right - (r.x + 88);
            ui::text(list, row.focused ? fonts.semibold : fonts.regular,
                     fit(fonts.semibold, row.title, 24, text_w), r.x + 88, r.y + 35, 24, kWhite);
            if (!row.pills.empty())
            {
                /* Pills on the second line: one fact each, so a long stream or track
                 * reads as chips instead of one run-on string. '+' = stands out. */
                float px = r.x + 88;
                const float py = r.y + 40;
                for (const std::string &raw : row.pills)
                {
                    const bool strong = raw[0] == '+';
                    const std::string t = strong ? raw.substr(1) : raw;
                    const float w = fonts.semibold.measure(t, 15, 0.6f) + 22;
                    if (px + w > r.x + 88 + text_w)
                        break;
                    const Rect pill{px, py, w, 26};
                    if (strong)
                        list.bordered_rect(pill, 13, p.accent.with_alpha(row.focused ? 0.25f : 0.14f), 1.5f,
                                           p.accent);
                    else
                        list.rounded_rect(pill, 13, kWhite.with_alpha(row.focused ? 0.18f : 0.09f));
                    ui::text(list, fonts.semibold, t, pill.cx(), py + 18.5f, 15,
                             strong ? p.accent : (row.focused ? kWhite : p.text_muted), Align::center, 0.6f);
                    px += w + 8;
                }
            }
            else
                ui::text(list, fonts.regular, fit(fonts.regular, row.detail, 18, text_w), r.x + 88, r.y + 61,
                         18, p.text_faint);
            if (row.progress >= 0)
            {
                const float f = tween::clamp01(static_cast<float>(row.progress) / 1000.0f);
                const Rect bar{r.x + 88, r.y + r.h - 6, text_w, 3};
                list.rounded_rect(bar, 1.5f, kWhite.with_alpha(0.12f));
                list.rounded_rect({bar.x, bar.y, std::max(3.0f, bar.w * f), bar.h}, 1.5f, p.accent);
            }
            list.pop_opacity();
        }
        if (has_focus_.value > 0.01f && menu_open_.value < 0.99f)
        {
            list.push_opacity(has_focus_.value * (1.0f - menu_open_.value));
            list.bordered_rect(ring_.value().inset(-3), page::kRadiusTile + 3, Color::rgb(0x000000, 0.0f), 3, kWhite);
            list.pop_opacity();
        }

        /* Details card for the row under the cursor: every fact on its own line,
         * so nothing has to be squeezed into (and cut from) a row. */
        if (!panel_.empty())
        {
            const float in = tween::stagger(age_, 2, 0.05f, 0.45f);
            list.push_opacity(in);
            const float x = kListRight - kPanelW;
            const float h = 9.0f * (kListRowH + kListGap) - kListGap;
            const Rect card{x, kListTop, kPanelW, h};
            list.rounded_rect(card, page::kRadiusCard, p.surface);
            list.bordered_rect(card, page::kRadiusCard, Color::rgb(0x000000, 0.0f), 1.5f, kWhite.with_alpha(0.10f));
            const float tx = x + 32.0f;
            const float tw = kPanelW - 64.0f;
            if (!panel_eyebrow_.empty())
                ui::text(list, fonts.semibold, ui::upper(panel_eyebrow_), tx, card.y + 46, 16, p.accent,
                         Align::left, 3.0f);
            ui::paragraph(list, fonts.display, panel_title_, tx, card.y + 90, 32, tw, 38, kWhite, 2,
                          Align::left);
            float y = card.y + 152.0f;
            list.rounded_rect({tx, y - 16.0f, tw, 1.5f}, 0.75f, kWhite.with_alpha(0.10f));
            for (const auto &f : panel_)
            {
                if (y + 56.0f > card.y + card.h)
                    break;
                ui::text(list, fonts.semibold, ui::upper(f.first), tx, y + 14, 13, p.text_muted, Align::left,
                         2.0f);
                ui::paragraph(list, fonts.regular, f.second, tx, y + 42, 22, tw, 26, kWhite, 2, Align::left);
                y += 60.0f;
            }
            list.pop_opacity();
        }
    }

    /* Hints. */
    if (!hints_.empty())
    {
        ui::Hint hints[4];
        int n = 0;
        for (const auto &h : hints_)
            hints[n++] = {static_cast<ui::Button>(h.first), h.second.c_str()};
        list.push_opacity(tween::stagger(age_, 6, 0.06f, 0.4f));
        draw_page_hints(list, fonts, hints, n);
        list.pop_opacity();
    }

    /* Busy card: the screen's work is running off the UI thread, so say so and keep
     * something moving - a frozen picture reads as a hang. */
    if (loading_)
    {
        list.rounded_rect({0, 0, 1920, 1080}, 0, Color::rgb(0x000000, 0.55f));
        const Rect card{960.0f - 280.0f, 540.0f - 100.0f, 560.0f, 200.0f};
        list.shadow({card.x, card.y + 20, card.w, card.h}, page::kRadiusCard, 40, Color::rgb(0x000000, 0.55f));
        list.rounded_rect(card, page::kRadiusCard, p.bg_top.with_alpha(0.97f));
        list.bordered_rect(card, page::kRadiusCard, Color::rgb(0x000000, 0.0f), 1.5f, kWhite.with_alpha(0.14f));
        for (int i = 0; i < 3; ++i)
        {
            const float pulse = std::max(0.0f, std::sin(loading_age_ * 6.0f - static_cast<float>(i) * 0.9f));
            const float x = card.cx() + (static_cast<float>(i) - 1.0f) * 40.0f;
            list.rounded_rect({x - 9.0f, card.y + 56.0f, 18.0f, 18.0f}, 9,
                              p.accent.with_alpha(0.25f + 0.75f * pulse));
        }
        ui::text(list, fonts.semibold, loading_text_.empty() ? std::string("Working") : loading_text_,
                 card.cx(), card.y + 138.0f, 28, kWhite, Align::center);
    }

    /* OPTIONS menu. */
    const float m = menu_open_.value;
    if (m > 0.01f && !menu_.empty())
    {
        list.rounded_rect({0, 0, 1920, 1080}, 0, Color::rgb(0x000000, 0.45f * m));
        list.push_transform(0.96f + 0.04f * m, 960, 540, 0, 0);
        list.push_opacity(m);
        const Rect first = menu_item_rect(0);
        const float top = first.y - 180.0f;
        const float h = 180.0f + static_cast<float>(menu_.size()) * (kMenuItemH + 6.0f) + 24.0f;
        const Rect card{960.0f - kMenuW * 0.5f, top, kMenuW, h};
        list.shadow({card.x, card.y + 24, card.w, card.h}, 32, 48, Color::rgb(0x000000, 0.6f));
        list.rounded_rect(card, page::kRadiusCard, p.bg_top.with_alpha(0.96f));
        list.bordered_rect(card, page::kRadiusCard, Color::rgb(0x000000, 0.0f), 1.5f, kWhite.with_alpha(0.14f));
        const Rect well{card.x + 32, card.y + 36, 64, 64};
        const bool brand = is_brand_icon(menu_icon_);
        list.rounded_rect(well, page::kRadiusChip, brand ? kWhite.with_alpha(0.92f) : p.accent);
        const std::uint32_t icon = ctx.textures.icon(menu_icon_);
        if (icon)
            list.image(icon, well.inset(brand ? 12 : 16), ::hui::gfx::kFullUv, brand ? kWhite : kInk);
        const float tx = well.x + well.w + 20;
        ui::text(list, fonts.semibold, ui::upper(menu_eyebrow_), tx, card.y + 54, 16, p.accent, Align::left,
                 3.0f);
        ui::text(list, fonts.display, fit(fonts.display, menu_title_, 32, card.x + card.w - 32 - tx), tx,
                 card.y + 92, 32, kWhite);
        ui::text(list, fonts.regular, fit(fonts.regular, menu_sub_, 19, card.w - 64), card.x + 32,
                 card.y + 144, 19, p.text_muted);

        list.rounded_rect(menu_ring_.value(), 20, kWhite);
        for (std::size_t i = 0; i < menu_.size(); ++i)
        {
            const MenuItem &item = menu_[i];
            const Rect r = menu_item_rect(static_cast<int>(i));
            const bool focused = static_cast<int>(i) == menu_focus_;
            const Color ink = focused ? kInk : (item.danger ? kDanger : kWhite);
            const std::uint32_t ic = ctx.textures.icon(item.icon);
            if (ic)
                list.image(ic, {r.x + 18, r.cy() - 14, 28, 28}, ::hui::gfx::kFullUv, ink);
            ui::text(list, fonts.semibold, fit(fonts.semibold, item.label, 22, r.w - 80), r.x + 62, r.y + 30, 22,
                     ink);
            ui::text(list, fonts.regular, fit(fonts.regular, item.desc, 16, r.w - 80), r.x + 62, r.y + 52, 16,
                     focused ? kInk.with_alpha(0.7f) : p.text_faint);
        }
        list.pop_opacity();
        list.pop_transform();
    }
}

bool ListScreen::focus(FocusInfo *out) const
{
    out->focusable.clear();
    if (!menu_.empty())
    {
        for (std::size_t i = 0; i < menu_.size(); ++i)
            out->focusable.emplace_back("menu-" + std::to_string(i), menu_[i].label);
        const int at = std::clamp(menu_focus_, 0, static_cast<int>(menu_.size()) - 1);
        out->id = "menu-" + std::to_string(at);
        out->text = menu_[static_cast<std::size_t>(at)].label;
        out->index = at + 1;
        out->total = static_cast<int>(menu_.size());
        out->rect = menu_item_rect(at);
        return true;
    }
    for (std::size_t i = 0; i < rows_.size(); ++i)
        out->focusable.emplace_back("row-" + std::to_string(i), rows_[i].title);
    for (std::size_t i = 0; i < rows_.size(); ++i)
    {
        if (!rows_[i].focused)
            continue;
        out->id = "row-" + std::to_string(i);
        out->text = rows_[i].title;
        out->index = cursor_ >= 0 ? cursor_ + 1 : static_cast<int>(i) + 1;
        out->total = total_ > 0 ? total_ : static_cast<int>(rows_.size());
        out->rect = row_rect(static_cast<int>(i));
        return true;
    }
    out->focusable.clear();
    return false;
}

/* ------------------------------------------------------------ focus report */

bool NavRail::focus(FocusInfo *out) const
{
    if (!params_.visible || !params_.rail_focused)
        return false;
    RailItem items[8];
    const int count = rail_items(items);
    const int at = std::clamp(params_.cursor_index, 0, count - 1);
    out->id = "nav-" + std::to_string(at);
    out->text = items[at].label;
    out->index = at + 1;
    out->total = count;
    out->rect = {28.0f, rail_item_y(at, count), kRailExpanded - 56.0f, 72.0f};
    out->focusable.clear();
    for (int i = 0; i < count; ++i)
        out->focusable.emplace_back("nav-" + std::to_string(i), items[i].label);
    return true;
}

bool HomeScreen::focus(FocusInfo *out) const
{
    out->focusable.clear();
    if (hero_focused_)
    {
        out->id = "hero-action";
        out->text = hero_.action + " " + hero_.title;
        out->index = out->total = 1;
        out->rect = kHeroAction;
        out->focusable.emplace_back(out->id, out->text);
        return true;
    }
    auto row = [&](const std::vector<Tile> &tiles, const char *prefix, bool recent) {
        for (std::size_t i = 0; i < tiles.size(); ++i)
            out->focusable.emplace_back(std::string(prefix) + std::to_string(i), tiles[i].title);
        for (std::size_t i = 0; i < tiles.size(); ++i)
        {
            if (!tiles[i].focused)
                continue;
            out->id = std::string(prefix) + std::to_string(i);
            out->text = tiles[i].title;
            out->index = static_cast<int>(i) + 1;
            out->total = recent ? std::max(recent_total_, static_cast<int>(tiles.size()))
                                : static_cast<int>(tiles.size());
            out->rect = recent ? recent_rect(static_cast<int>(i)) : library_rect(static_cast<int>(i));
            return true;
        }
        out->focusable.clear();
        return false;
    };
    return row(recent_, "recent-", true) || row(library_, "library-", false);
}

} // namespace evo::kit
