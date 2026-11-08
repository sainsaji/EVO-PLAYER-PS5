/*
 * evo_hui_modals.hpp - the player's pop-ups on ps5-homebrew-ui, drawn over the
 * video: the subtitle track picker (subtitles.rml) as a side sheet with a live
 * caption preview, and Media Info (mediainfo.rml) as a centred card.
 */
#ifndef EVO_HUI_MODALS_HPP
#define EVO_HUI_MODALS_HPP

#include "evo_hui_screens.hpp"

namespace evo::kit
{

class SubtitlePicker
{
  public:
    void set(const evo_rmlui_subtitles_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    struct Track
    {
        std::string label, detail, tag;
        bool current = false, focused = false, action = false, disabled = false;
    };
    hui::gfx::Rect track_rect(int index) const;

    std::string eyebrow_, title_, size_, sync_, sync2_, preview_, preview2_;
    int preview_face_ = 1;
    bool has_secondary_ = false;
    int secondary_position_ = 0;
    int secondary_color_ = 0;
    int primary_color_ = 0;
    std::vector<Track> tracks_;
    hui::tween::Spring open_;
    hui::ui::SpringRect ring_;
    bool ring_snapped_ = false;
};

class MediaInfoCard
{
  public:
    void set(const evo_rmlui_mediainfo_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    std::string title_, path_;
    std::vector<std::string> badges_;
    /* section title, then (label, value) rows */
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> sections_;
    hui::tween::Spring open_;
};

} // namespace evo::kit

#endif /* EVO_HUI_MODALS_HPP */
