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
#include "ui/components/skeleton.hpp"
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
        std::string title, detail, badge, icon, initial, art_key;
        const std::uint32_t *art = nullptr;
        int art_w = 0, art_h = 0;
        bool radio = false, on = false, warn = false, chevron = false, badge_live = false, focused = false;
    };

    struct Shelf
    {
        std::string title;
        int abs_index = 0, total = 0, first_card = 0, focus_card = -1, scroll_card = 0;
        std::vector<Row> cards;
        std::vector<char> see_all;
    };
    void draw_shelves(hui::gfx::DrawList &list, const Context &ctx) const;
    hui::gfx::Rect shelf_card_rect(const Shelf &s, int card_index) const;

    bool channels() const { return variant_ == EVO_HUI_PROVIDER_LIVETV || variant_ == EVO_HUI_PROVIDER_XTREAM; }
    bool setup_shown() const;
    hui::gfx::Rect card_rect(int index) const;
    void draw_setup(hui::gfx::DrawList &list, const Context &ctx) const;
    void draw_poster_card(hui::gfx::DrawList &list, const Context &ctx, const Row &row, bool see_all, float cx, float y) const;
    void draw_selected_line(hui::gfx::DrawList &list, const Context &ctx) const;
    void draw_panel(hui::gfx::DrawList &list, const Context &ctx) const;
    void draw_art(hui::gfx::DrawList &list, const Context &ctx, const hui::gfx::Rect &r, float radius,
                  const std::uint32_t *art, int w, int h, const std::string &key, bool folder,
                  const std::string &initial, float initial_size, bool contain) const;

    int variant_ = 0;
    std::string name_, icon_, options_hint_, crumb_, status_, query_, page_info_;
    bool loading_ = false, tuning_ = false, has_error_ = false, empty_ = false, multi_page_ = false;
    mutable hui::ui::Skeleton skel_[6]; /* placeholder cards while a first load runs (#157) */
    mutable float skel_time_ = 0.0f;
    bool folder_level_ = false, in_folder_ = false;
    int count_ = 0;
    std::vector<Row> rows_;
    int focus_ = -1;

    bool has_selected_ = false, sel_folder_ = false, sel_played_ = false;
    int sel_progress_ = 0;
    std::string sel_title_, sel_sub_, sel_initial_, sel_overview_, sel_resume_, sel_art_key_;
    std::string sel_num_, sel_tech_, sel_now_, sel_next_, epg_status_;
    std::string sel_rating_, sel_cert_, sel_cast_;
    /* The shelves with a Details block (title, pills, description, cast) above the focused row. */
    bool details_mode() const;
    void draw_details(hui::gfx::DrawList &list, const Context &ctx) const;
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

    bool shelves_active_ = false, item_menu_ = false;
    /* The focused card's wide image and logo, behind and over the shelves. */
    const std::uint32_t *bd_ = nullptr, *logo_ = nullptr;
    int bd_w_ = 0, bd_h_ = 0, logo_w_ = 0, logo_h_ = 0;
    std::string bd_key_, logo_key_;
    float bd_alpha_ = 0.0f;                 /* fades in when a new image arrives */
    int shelf_total_ = 0, shelf_focus_abs_ = 0;
    std::vector<Shelf> shelves_;
    float scroll_y_ = 0.0f;                 /* in shelves */
    float scroll_x_[32] = {};               /* per shelf, in cards */
    bool scroll_snapped_ = false;

    /* The movie / show detail page (#221): poster, PLAY / RESTART, and the tabs. */
    struct DetailPerson
    {
        std::string name, role, initial, art_key;
        const std::uint32_t *art = nullptr;
        int art_w = 0, art_h = 0;
    };
    struct DetailVersion
    {
        std::string label, facts;
    };
    struct Detail
    {
        bool open = false, is_show = false, has_restart = false;
        std::string title, meta, rating, cert, overview, resume, poster_key;
        int zone = 0, button = 0, tab = 0;
        const std::uint32_t *poster = nullptr;
        int poster_w = 0, poster_h = 0;
        std::vector<DetailPerson> cast;
        std::vector<DetailVersion> versions;
        int cast_state = 0, versions_state = 0;
        /* the stream picker (pick = true) */
        bool pick = false;
        std::string pick_note, fact_title;
        struct StreamRow
        {
            std::string label, chips, badge;
        };
        std::vector<StreamRow> streams;
        int stream_total = 0, stream_first = 0, stream_cursor = -1;
        std::vector<DetailVersion> facts;     /* label, value */
    } detail_;
    float detail_age_ = 0.0f;
    hui::gfx::Rect detail_focus_rect() const;
    void draw_detail_page(hui::gfx::DrawList &list, const Context &ctx) const;
    void draw_stream_picker(hui::gfx::DrawList &list, const Context &ctx) const;

    float age_ = 0.0f;
    float panel_age_ = 0.0f;
    hui::ui::SpringRect ring_;
    bool ring_snapped_ = false;
};

} // namespace evo::kit

#endif /* EVO_HUI_PROVIDER_HPP */
