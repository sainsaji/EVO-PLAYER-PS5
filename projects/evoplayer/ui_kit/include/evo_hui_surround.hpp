/*
 * evo_hui_surround.hpp - Surround Sound Studio (#106) on ps5-homebrew-ui.
 */
#ifndef EVO_HUI_SURROUND_HPP
#define EVO_HUI_SURROUND_HPP

#include "evo_rmlui_bridge.h"
#include "evo_hui_screens.hpp"

#include "gfx/draw_list.hpp"
#include "ui/fonts.hpp"

#include <string>

namespace evo::kit
{

class SurroundScreen
{
  public:
    void set(const evo_rmlui_surround_params_t &params);
    void set_display_120(bool on) { is_120hz_ = on; }
    void enter() {}
    void update(float) {}
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    void draw_stage(hui::gfx::DrawList &list, const Context &ctx) const;
    void draw_left(hui::gfx::DrawList &list, const Context &ctx) const;
    void draw_right(hui::gfx::DrawList &list, const Context &ctx) const;
    void draw_calibration(hui::gfx::DrawList &list, const Context &ctx) const;
    void draw_telemetry(hui::gfx::DrawList &list, const Context &ctx) const;

    std::string label_of(int ch) const;
    std::string name_of(int ch) const;

    evo_rmlui_surround_params_t p_{};
    std::string names_[EVO_RMLUI_SURROUND_SPEAKERS];
    std::string labels_[EVO_RMLUI_SURROUND_SPEAKERS];
    std::string cal_message_;
    bool is_120hz_ = false;
};

} // namespace evo::kit

#endif /* EVO_HUI_SURROUND_HPP */
