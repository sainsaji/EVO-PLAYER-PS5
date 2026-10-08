/*
 * evo_hui_extra.hpp - Extra EVO screens on ps5-homebrew-ui.
 */
#ifndef EVO_HUI_EXTRA_HPP
#define EVO_HUI_EXTRA_HPP

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

class ReaderScreen
{
  public:
    void set(const evo_rmlui_reader_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    std::string title_;
    std::string subtitle_;
    std::string badge_;
    std::vector<std::string> lines_;
    int face_ = 0;
    double progress_ = 0.0;
    double visible_frac_ = 1.0;
    std::string notice_;
    std::string footnote_;
};

class ImageViewer
{
  public:
    void set(const evo_rmlui_image_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    std::string title_;
    const std::uint32_t *pixels_ = nullptr;
    int w_ = 0;
    int h_ = 0;
    bool loaded_ = false;

    float fade_age_ = 0.0f; /* seconds since the title changed */
};

class KeyboardOverlay
{
  public:
    void set(const evo_keyboard_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    bool visible_ = false;
    bool native_only_ = false;
    std::string title_;
    std::string text_;
    std::string mode_label_;
    std::vector<std::string> rows_[4];
    std::vector<std::string> action_labels_;
    int len_ = 0;
    int max_len_ = 0;
    int focus_row_ = 0;
    int focus_col_ = 0;
    bool show_caret_ = false;

    hui::tween::Spring enter_scale_;
    hui::tween::Spring enter_fade_;
    hui::ui::SpringRect ring_;
    bool ring_snapped_ = false;
};

} // namespace evo::kit

#endif /* EVO_HUI_EXTRA_HPP */
