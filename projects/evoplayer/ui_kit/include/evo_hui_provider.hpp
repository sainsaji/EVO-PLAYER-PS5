/*
 * evo_hui_provider.hpp - the provider screens on ps5-homebrew-ui: Emby /
 * Jellyfin / addon poster walls, Live TV, Xtream Codes and the generic
 * fallback, with their setup cards and the options side panel.
 */
#ifndef EVO_HUI_PROVIDER_HPP
#define EVO_HUI_PROVIDER_HPP

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

class ProviderGridScreen
{
  public:
    void set(const evo_hui_provider_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    struct Row
    {
        std::string title, subtitle, initial, art_key, now, next;
        const std::uint32_t *art = nullptr;
        int art_w = 0, art_h = 0;
        bool is_folder = false, played = false, is_live = false;
        int progress = 0;
    };
    struct PanelRow
    {
        std::string title, detail, badge, icon;
        bool radio = false, on = false, warn = false, chevron = false, badge_live = false, focused = false;
    };

    bool channels() const { return variant_ == EVO_HUI_PROVIDER_LIVETV || variant_ == EVO_HUI_PROVIDER_XTREAM; }
    bool setup_shown() const;
    hui::gfx::Rect card_rect(int index) const;
    void draw_setup(hui::gfx::DrawList &list, const Context &ctx) const;
    void draw_card(hui::gfx::DrawList &list, const Context &ctx, const Row &row, const hui::gfx::Rect &c) const;
    void draw_detail(hui::gfx::DrawList &list, const Context &ctx) const;
    void draw_panel(hui::gfx::DrawList &list, const Context &ctx) const;
    void draw_art(hui::gfx::DrawList &list, const Context &ctx, const hui::gfx::Rect &r, float radius,
                  const std::uint32_t *art, int w, int h, const std::string &key, bool folder,
                  const std::string &initial, float initial_size, bool contain) const;

    int variant_ = 0;
    std::string name_, crumb_, status_, query_, page_info_;
    bool loading_ = false, tuning_ = false, has_error_ = false, empty_ = false, multi_page_ = false;
    bool folder_level_ = false, in_folder_ = false;
    int count_ = 0;
    std::vector<Row> rows_;
    int focus_ = -1;

    bool has_selected_ = false, sel_folder_ = false, sel_played_ = false;
    int sel_progress_ = 0;
    std::string sel_title_, sel_sub_, sel_initial_, sel_overview_, sel_resume_, sel_art_key_;
    std::string sel_num_, sel_tech_, sel_now_, sel_next_, epg_status_;
    bool epg_setup_ = false;
    const std::uint32_t *sel_art_ = nullptr;
    int sel_art_w_ = 0, sel_art_h_ = 0;

    bool setup_configured_ = false;
    std::string setup_account_;
    int setup_focus_ = -1;

    bool panel_open_ = false;
    std::string panel_crumb_, panel_eyebrow_, panel_title_, panel_sub_, panel_note_b_, panel_note_;
    std::string panel_accept_, panel_back_;
    std::vector<PanelRow> panel_rows_;

    float age_ = 0.0f;
    float panel_age_ = 0.0f;
    hui::ui::SpringRect ring_;
    bool ring_snapped_ = false;
};

} // namespace evo::kit

#endif /* EVO_HUI_PROVIDER_HPP */
