/*
 * evo_hui_misc.hpp - EVO's miscellaneous screens on ps5-homebrew-ui.
 */
#ifndef EVO_HUI_MISC_HPP
#define EVO_HUI_MISC_HPP

#include "evo_rmlui_bridge.h"
#include "evo_hui_screens.hpp"

#include "core/tween.hpp"
#include "gfx/draw_list.hpp"
#include "ui/fonts.hpp"
#include "ui/motion.hpp"

#include <string>
#include <vector>

namespace evo::kit
{

class DialogOverlay
{
  public:
    void set(const evo_rmlui_dialog_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    struct Action
    {
        std::string icon_path;
        std::string label;
        bool is_primary = false;
    };

    std::string eyebrow_;
    std::string title_;
    std::string detail_;
    double progress_pct_ = -1.0;
    int focused_action_ = -1;
    std::vector<Action> actions_;

    hui::tween::Spring enter_scale_;
    hui::tween::Spring enter_fade_;
};

class ToastOverlay
{
  public:
    void set(const evo_rmlui_toast_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    std::string title_;
    std::string message_;
    int kind_ = 0;
    bool visible_ = false;
    int alpha_ = 0;
    int slide_ = 0;
};

class AboutScreen
{
  public:
    void set(const evo_rmlui_about_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    std::string app_name_;
    std::string version_;
    std::string build_tag_;
    std::string tagline_;
    std::string themes_info_;
    bool action_focused_ = false;
};

class ChangelogScreen
{
  public:
    void set(const evo_rmlui_changelog_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    struct Release
    {
        std::string version;
        std::string tagline;
        std::string date;
        bool focused = false;
    };

    struct Item
    {
        std::string kind;
        std::string text;
    };

    std::string title_;
    std::string subtitle_;
    std::vector<Release> releases_;
    std::string detail_version_;
    std::string detail_tagline_;
    std::vector<Item> items_;
    int item_total_ = 0;

    hui::ui::SpringRect ring_;
    bool ring_snapped_ = false;
};

class ClosedScreen
{
  public:
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;
};

} // namespace evo::kit

#endif /* EVO_HUI_MISC_HPP */
