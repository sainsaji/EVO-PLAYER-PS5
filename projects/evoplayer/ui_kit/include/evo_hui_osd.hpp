/*
 * evo_hui_osd.hpp - the player's on-screen display (playback.rml) on
 * ps5-homebrew-ui: drawn over the video in the same AGC frame.
 *
 * Title and format badges at the top, the timeline and controls at the
 * bottom, a centre badge for paused / loading / seeking, captions, the music
 * view for audio-only files, the "stats for nerds" HUD and the FPS pill. The
 * caller's `alpha` fades the chrome; `chrome_hidden` leaves captions only.
 *
 * can_draw() is false when a caption holds characters the kit's baked fonts
 * do not have: the caller then keeps RmlUi's OSD (Noto) for that frame, so
 * subtitles in any script still show until the kit gets a dynamic font.
 */
#ifndef EVO_HUI_OSD_HPP
#define EVO_HUI_OSD_HPP

#include "evo_hui_screens.hpp"

namespace evo::kit
{

class OsdScreen
{
  public:
    void set(const evo_playback_osd_params_t &params);
    void set_hud(const evo_perf_hud_t &hud);
    void enter();
    void update(float dt);
    bool can_draw(const hui::ui::Fonts &fonts) const;
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    void draw_top(hui::gfx::DrawList &list, const Context &ctx) const;
    void draw_bottom(hui::gfx::DrawList &list, const Context &ctx) const;
    void draw_centre(hui::gfx::DrawList &list, const Context &ctx) const;
    void draw_captions(hui::gfx::DrawList &list, const Context &ctx, float chrome) const;
    void draw_music(hui::gfx::DrawList &list, const Context &ctx) const;
    void draw_stats(hui::gfx::DrawList &list, const Context &ctx) const;

    evo_playback_osd_params_t raw_{}; /* numbers only: its strings dangle */
    std::string title_, meta_;
    std::string badges_[9]; /* res hdr codec fps audio decoder upscale smooth deepblack */
    std::string audio_track_, sub_track_, music_codec_;
    std::vector<std::string> sub_lines_, sub2_lines_;

    bool hud_on_ = false;
    std::string hud_lines_[6];
    std::vector<float> gpu_hist_, ram_hist_, cpu_hist_;
    float gpu_pct_ = 0, ram_mb_ = 0, ram_total_ = 0, cpu_pct_ = 0;

    float time_ = 0.0f;
    hui::tween::Spring chrome_;
    hui::tween::Spring paused_;
    hui::tween::Spring loading_;
    hui::tween::Spring scrub_;
    hui::tween::Spring progress_;
    bool progress_snapped_ = false;
};

} // namespace evo::kit

#endif /* EVO_HUI_OSD_HPP */
