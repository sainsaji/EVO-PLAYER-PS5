/*
 * evo_hui_settings_preview.hpp - the preview card beside the Settings rows (#158).
 * One drawing function per EVO_RMLUI_PREVIEW_* kind; every setting gets at least
 * the INFO card (icon + plain-words benefit).
 */
#ifndef EVO_HUI_SETTINGS_PREVIEW_HPP
#define EVO_HUI_SETTINGS_PREVIEW_HPP

#include "evo_hui_screens.hpp"

#include <string>
#include <vector>

namespace evo::kit
{

struct SettingsPreview
{
    int kind = 0;   /* EVO_RMLUI_PREVIEW_*; 0 = no card */
    int index = 0;  /* the highlighted choice (toggle: 0 = off, 1 = on) */
    bool toggle = false;
    std::string title;
    std::string benefit;
    std::string detail;                /* the row's subtext in full (the row itself cuts it short) */
    std::string icon;                  /* the setting's icon path */
    std::vector<std::string> choices;  /* empty for a toggle: OFF / ON */
    std::vector<unsigned int> choice_bg;      /* THEME: page background per choice, 0xRRGGBB */
    int qr_size = 0;                          /* PORT: the real phone-link QR, 0 = none */
    std::vector<unsigned char> qr;
    std::vector<unsigned int> choice_accent;  /* THEME: accent per choice, 0xRRGGBB */
};

void draw_settings_preview(hui::gfx::DrawList &list, const Context &ctx, const hui::gfx::Rect &box,
                           const SettingsPreview &pv);

} // namespace evo::kit

#endif /* EVO_HUI_SETTINGS_PREVIEW_HPP */
