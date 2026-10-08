/*
 * evo_hui_browser.hpp - the storage browser (browser.rml) on ps5-homebrew-ui.
 *
 * Sources down the left, a 4 x 2 grid of 16:9 cards (BrowserScreen pages it:
 * 8 visible, 4 columns), the focused file's details in a strip underneath,
 * and the file-operations menu and transfer progress as overlays.
 */
#ifndef EVO_HUI_BROWSER_HPP
#define EVO_HUI_BROWSER_HPP

#include "evo_hui_screens.hpp"

namespace evo::kit
{

class BrowserScreen
{
  public:
    void set(const evo_rmlui_browser_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    struct Card
    {
        std::string name, detail, icon, badge, duration;
        int progress = -1;
        bool favorite = false;
        bool focused = false;
        const std::uint32_t *art = nullptr;
        int art_w = 0, art_h = 0;
    };

    hui::gfx::Rect card_rect(int index) const;
    hui::gfx::Rect source_rect(int index) const;
    hui::gfx::Rect chip_rect(int index, const Context *ctx) const;
    hui::gfx::Rect action_rect(int index) const;
    float chips_x(int index) const;

    evo_rmlui_browser_params_t raw_{}; /* ints only; strings live below */
    std::string path_, title_;
    std::vector<std::string> chips_;
    std::vector<Card> cards_;
    std::string empty_title_, empty_hint_;
    std::string ins_name_, ins_kind_;
    std::vector<std::pair<std::string, std::string>> props_;
    std::string status_[5];
    std::string act_target_, act_sub_, act_clip_;
    std::string tr_title_, tr_item_, tr_speed_, tr_bytes_, tr_eta_, tr_pct_;
    double tr_progress_ = 0.0;

    float age_ = 0.0f;
    float page_age_ = 1.0f;
    int last_first_ = -1;
    hui::ui::SpringRect ring_;
    bool ring_snapped_ = false;
    hui::tween::Spring ring_alpha_;
    hui::ui::SpringRect source_ring_;
    hui::tween::Spring menu_;
    hui::ui::SpringRect action_ring_;
    bool action_snapped_ = false;
    hui::tween::Spring transfer_;
    hui::tween::Spring transfer_bar_;
    std::vector<float> chip_w_;
};

} // namespace evo::kit

#endif /* EVO_HUI_BROWSER_HPP */
