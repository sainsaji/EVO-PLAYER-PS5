/*
 * evo_hui_extra.hpp - Extra EVO screens on ps5-homebrew-ui.
 */
#ifndef EVO_HUI_EXTRA_HPP
#define EVO_HUI_EXTRA_HPP

#include "evo_rmlui_bridge.h"
#include "evo_hui_screens.hpp"

#include "core/tween.hpp"
#include "gfx/draw_list.hpp"
#include "ui/components/keyboard.hpp"
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
    void sync_keys(const evo_keyboard_params_t &p);
    void draw_pad(hui::gfx::DrawList &list, const Context &ctx) const;
    hui::ui::Keyboard keys_;
    bool layouts_built_ = false;
    bool keys_ready_ = false;
    bool visible_ = false;
    bool native_only_ = false;
    bool clear_visible_ = false;
    std::string title_;
    std::string text_;
    std::string mode_label_;
    const evo_kb_key_t *key_rows_[EVO_KB_LAYERS][EVO_KB_ROWS] = {};
    int key_row_len_[EVO_KB_LAYERS][EVO_KB_ROWS] = {};
    int layer_ = 0;
    int len_ = 0;
    int max_len_ = 0;
    int focus_row_ = 0;
    int focus_col_ = 0;
    bool show_caret_ = false;
    int cursor_ = 0;
    int qr_size_ = 0;
    std::vector<unsigned char> qr_;
    int built_columns_ = 0;   /* the columns the key layouts were built for */
    int key_columns_ = 11;   /* fewer than 11 = a pad (digits only): a narrower board */
    std::string phone_title_;
    std::string phone_notice_;
    std::string phone_action_;
    int phone_kind_ = 0;

    hui::tween::Spring enter_scale_;
    hui::tween::Spring enter_fade_;
    hui::tween::Spring clear_focus_; /* 0..1: the clear button's focus fill */
};

} // namespace evo::kit

#endif /* EVO_HUI_EXTRA_HPP */
