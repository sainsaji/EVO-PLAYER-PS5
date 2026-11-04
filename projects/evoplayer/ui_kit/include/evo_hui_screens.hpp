/*
 * evo_hui_screens.hpp - EVO's screens built on ps5-homebrew-ui.
 *
 * Each screen turns the params struct its core/ screen already fills for the
 * RmlUi bridge (evo_rmlui_bridge.h) into a hui::gfx::DrawList. Nothing here
 * touches the GPU: the console draws the lists through sceAgc
 * (hui_agc_batch.cpp, evo_hui_app.cpp), the host preview through the kit's
 * own OpenGL backend (tools/hui_preview). Navigation and focus stay where they
 * are, in core/: a screen only animates towards the state it is given.
 *
 * Coordinates are the kit's 1920 x 1080 virtual canvas.
 */
#ifndef EVO_HUI_SCREENS_HPP
#define EVO_HUI_SCREENS_HPP

#include "evo_rmlui_bridge.h"
#include "evo_hui.h" /* evo_hui_webpage_params_t */

#include "core/tween.hpp"
#include "gfx/draw_list.hpp"
#include "ui/fonts.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"
#include "ui/theme.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace evo::kit
{

/* What the screens need from the backend that draws them. */
class TextureHost
{
  public:
    virtual ~TextureHost() = default;
    /* RGBA8 straight alpha, top row first. 0 on failure. */
    virtual std::uint32_t create_texture(int width, int height, const std::uint8_t *rgba) = 0;
    virtual void release_texture(std::uint32_t handle) = 0;
    /* Reads an asset by its bundle path ("../icons/icon_home.png"). */
    virtual bool read_asset(const std::string &path, std::string *bytes) = 0;
};

/* A provider's logo (icons/brand_*.png): drawn in its own colours on a neutral
 * well, never tinted like the one-colour UI icons. */
inline bool is_brand_icon(const std::string &path)
{
    return path.find("/brand_") != std::string::npos;
}

/* Textures for pixels the app owns (cover art, thumbnails, icons). A cover is
 * keyed by its pixel pointer, size and a caller tag, uploaded once, and
 * dropped after it has gone unused for a while. */
class TextureCache
{
  public:
    explicit TextureCache(TextureHost &host) : host_(host) {}
    ~TextureCache();
    /* EVO pixels: 0xAABBGGRR words, i.e. R,G,B,A bytes. */
    std::uint32_t pixels(const std::uint32_t *data, int width, int height, std::uint64_t tag = 0);
    /* A PNG from the asset bundle; 0 when it cannot be read. */
    std::uint32_t icon(const std::string &path);
    /* Call once per frame. */
    void tick();
    void clear();

  private:
    struct Entry
    {
        std::uint32_t handle = 0;
        int idle_frames = 0;
    };
    TextureHost &host_;
    std::map<std::string, Entry> entries_;
};

/* EVO's theme (evo_rmlui_theme_t) as kit colours. */
struct Palette
{
    hui::gfx::Color bg_top = hui::gfx::Color::rgb(0x0a0d1c);
    hui::gfx::Color bg_bottom = hui::gfx::Color::rgb(0x04050b);
    hui::gfx::Color surface = hui::gfx::Color::rgb(0xffffff, 0.06f);
    hui::gfx::Color surface_sel = hui::gfx::Color::rgb(0xffffff, 0.14f);
    hui::gfx::Color border = hui::gfx::Color::rgb(0xffffff, 0.12f);
    hui::gfx::Color accent = hui::gfx::Color::rgb(0x4f8cff);
    hui::gfx::Color accent_soft = hui::gfx::Color::rgb(0x2a4c99);
    hui::gfx::Color accent_alt = hui::gfx::Color::rgb(0xb06cff);
    hui::gfx::Color text = hui::gfx::Color::rgb(0xffffff);
    hui::gfx::Color text_muted = hui::gfx::Color::rgb(0xffffff, 0.66f);
    hui::gfx::Color text_faint = hui::gfx::Color::rgb(0xffffff, 0.58f);

    static Palette from(const evo_rmlui_theme_t &theme);

    /* The kit components' theme in EVO's colours (#157). */
    hui::ui::Theme kit_theme() const;
};

struct Context
{
    const hui::ui::Fonts &fonts;
    TextureCache &textures;
    const Palette &palette;
    float time = 0.0f; /* free-running seconds, for idle motion */
};

/* What has the cursor, for the dev remote's `ui` report (#115): the same
 * fields evo_rmlui_devstate.cpp reports for an RmlUi element. */
struct FocusInfo
{
    std::string id;   /* stable, e.g. "recent-2" */
    std::string text; /* what the item says */
    int index = 0;    /* 1-based within its row or list */
    int total = 0;
    hui::gfx::Rect rect;
    std::vector<std::pair<std::string, std::string>> focusable; /* id, text */
};

/* The navigation rail on the left of every menu screen. */
class NavRail
{
  public:
    void set(const evo_rmlui_nav_params_t &params) { params_ = params; }
    const evo_rmlui_nav_params_t &params() const { return params_; }
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    /* False unless the rail has the cursor. */
    bool focus(FocusInfo *out) const;
    /* Width the content must leave free (the collapsed rail). */
    static constexpr float kWidth = 128.0f;

  private:
    evo_rmlui_nav_params_t params_{0, 0, 0, 1, 0, 0, 0};
    hui::tween::Spring expand_;
    hui::ui::SpringRect cursor_;
    bool cursor_snapped_ = false;
};

/* Shared backdrop: the theme's gradient, slow aurora glows and an optional
 * picture washed in behind them. */
void draw_backdrop(hui::gfx::DrawList &list, const Context &ctx, std::uint32_t picture = 0,
                   float picture_alpha = 0.0f);

/* What every full-screen page shares, so they all read the same: the title,
 * the line under it, the count at the top right, the hint row and the corner
 * radii. tools/ui_consistency.py checks the rendered screens against these. */
namespace page
{
constexpr float kLeft = NavRail::kWidth + 64.0f; /* content's left edge */
constexpr float kRight = 1824.0f;                /* content's right edge */
constexpr float kTitleY = 140.0f;                /* title baseline */
constexpr float kTitleSize = 52.0f;
constexpr float kSubtitleY = 182.0f; /* the line under the title */
constexpr float kHintsY = 1046.0f;   /* centre line of the hint row */
/* Corner radii: chips and badges, tiles / posters / rows, cards / panels / sheets. */
constexpr float kRadiusChip = 16.0f;
constexpr float kRadiusTile = 20.0f;
constexpr float kRadiusCard = 32.0f;
} // namespace page

/* The page title (as given: Title Case) and the line under it (drawn in spaced
 * capitals). Returns the title's width. */
float draw_page_title(hui::gfx::DrawList &list, const Context &ctx, const std::string &title,
                      const std::string &subtitle, float x = page::kLeft, float dy = 0.0f,
                      float max_w = 1100.0f);
/* "3 OF 23" / "23 ITEMS" at the top right. */
void draw_page_counter(hui::gfx::DrawList &list, const Context &ctx, const std::string &text, float dy = 0.0f);
/* The hint row at the foot of a page or full-screen overlay. Cross comes
 * first and Circle (Back) last, whatever order the caller lists them in. */
void draw_page_hints(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const hui::ui::Hint *hints, int n,
                     float right = page::kRight);

/* Home (launch.rml). */
class HomeScreen
{
  public:
    void set(const evo_rmlui_launch_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    struct Tile
    {
        std::string title;
        std::string detail;
        std::string icon_path;
        int progress = -1;
        const std::uint32_t *art = nullptr;
        int art_w = 0;
        int art_h = 0;
        bool focused = false;
    };
    struct Hero
    {
        std::string eyebrow;
        std::string title;
        std::string detail;
        std::string action;
        int progress = -1;
        const std::uint32_t *art = nullptr;
        int art_w = 0;
        int art_h = 0;
    };

    hui::gfx::Rect focus_rect() const;
    hui::gfx::Rect recent_rect(int index) const;
    hui::gfx::Rect library_rect(int index) const;
    void draw_hero(hui::gfx::DrawList &list, const Context &ctx, const Hero &hero, float alpha,
                   float slide) const;
    std::uint32_t art_texture(const Context &ctx, const std::uint32_t *art, int w, int h,
                              const std::string &tag) const;

    std::string app_name_;
    std::string version_;
    std::string clock_;
    Hero hero_;
    Hero previous_hero_;
    bool hero_focused_ = false;
    int recent_total_ = 0;
    int recent_cursor_ = -1;
    std::vector<Tile> recent_;
    std::vector<Tile> library_;

    float age_ = 0.0f;
    hui::tween::Timer hero_fade_;
    hui::ui::SpringRect ring_;
    bool ring_snapped_ = false;
    hui::tween::Spring row_glow_;
    hui::ui::Scroller recent_scroll_;
};

/* Recent, Favorites and the provider lists (list.rml): one scrolling list. */
class ListScreen
{
  public:
    void set(const evo_rmlui_list_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    struct Row
    {
        std::string title, detail, icon, badge;
        std::vector<std::string> pills;
        int progress = -1;
        bool chevron = false;
        bool focused = false;
    };
    struct MenuItem
    {
        std::string label, desc, icon;
        bool danger = false;
    };
    hui::gfx::Rect row_rect(int index) const;
    hui::gfx::Rect menu_item_rect(int index) const;

    std::string title_, subtitle_;
    int total_ = 0;
    int cursor_ = -1;
    std::vector<Row> rows_;
    bool empty_ = false;
    std::string empty_title_, empty_hint_, empty_icon_;
    std::vector<std::pair<int, std::string>> hints_; /* ui::Button, label */
    std::vector<MenuItem> menu_;
    int menu_focus_ = 0;
    std::string menu_eyebrow_, menu_title_, menu_sub_, menu_icon_;
    std::string panel_eyebrow_, panel_title_;
    bool loading_ = false;
    std::string loading_text_;
    float loading_age_ = 0.0f;
    std::vector<std::pair<std::string, std::string>> panel_; /* label, value */

    float age_ = 0.0f;
    hui::ui::SpringRect ring_;
    bool ring_snapped_ = false;
    hui::tween::Spring has_focus_;
    hui::tween::Spring menu_open_;
    hui::ui::SpringRect menu_ring_;
    bool menu_ring_snapped_ = false;
};

/* The browser section's page view (#237 Phase 2). */
class WebPage
{
  public:
    void set(const evo_hui_webpage_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;
    void set_page(std::uint32_t tex, float u1, float v1);

  private:
    std::string url_;
    std::string title_;
    std::string message_;
    int loading_ = 0;
    int can_back_ = 0;
    int can_forward_ = 0;
    int state_ = 0;
    int pointer_visible_ = 0;
    int pointer_x_ = 0;
    int pointer_y_ = 0;
    int pointer_hand_ = 0;
    int hints_visible_ = 0;

    std::uint32_t tex_ = 0;
    float u1_ = 1.0f;
    float v1_ = 1.0f;
    float age_ = 0.0f;
    float loading_age_ = 0.0f;
};

} // namespace evo::kit

#endif /* EVO_HUI_SCREENS_HPP */
