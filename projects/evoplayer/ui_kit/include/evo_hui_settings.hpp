/*
 * evo_hui_settings.hpp - the settings screen (settings.rml) on ps5-homebrew-ui:
 * six sections on the left, the section's rows in a card on the right. The
 * card scrolls to keep the focused row in view (a VALUE row expanded into its
 * OPTIONs can be twenty rows long).
 */
#ifndef EVO_HUI_SETTINGS_HPP
#define EVO_HUI_SETTINGS_HPP

#include "evo_hui_screens.hpp"

#include <string>
#include <vector>

namespace evo::kit
{

class SettingsScreen
{
  public:
    void set(const evo_rmlui_settings_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    struct RowData
    {
        std::string title;
        std::string detail;
        std::string icon_path;
        std::string badge;
        int has_chevron = 0;
        int is_focused = 0;
        int kind = 0;
        int toggle_on = 0;
        int is_disabled = 0;
    };

    /* Top of a row inside the card, before scrolling. */
    float row_top(int index) const;
    float row_y(int index) const { return row_top(index) - scroll_.value; }
    void draw_row(hui::gfx::DrawList &list, const Context &ctx, const RowData &row, const hui::gfx::Rect &r, int index) const;

    evo_rmlui_settings_params_t params_{};
    std::string title_;
    std::string subtitle_;
    std::string counter_;
    std::vector<RowData> rows_;

    float section_age_ = 0.0f;
    float age_ = 0.0f;

    hui::ui::SpringRect sidebar_cursor_;
    bool sidebar_cursor_snapped_ = false;

    hui::ui::SpringRect row_cursor_;
    bool row_cursor_snapped_ = false;

    hui::tween::Spring toggles_[EVO_RMLUI_SETTINGS_ROWS];
    hui::tween::Spring scroll_;
};

} // namespace evo::kit

#endif /* EVO_HUI_SETTINGS_HPP */
