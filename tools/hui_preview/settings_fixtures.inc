#include "evo_rmlui_bridge.h"

static void fill_settings_fixture(evo_rmlui_settings_params_t &p, int which)
{
    p = {};
    if (which == 0) {
        p.sidebar_focused = 1;
        p.section_active = 0;
        p.title = "Video & Display";
        p.subtitle = "PREFERENCES";
        p.counter = "6 ITEMS";
        p.row_count = 6;
        
        p.rows[0].kind = EVO_RMLUI_ROW_ACTION;
        p.rows[0].title = "Resume Playback";
        p.rows[0].detail = "Continue from where you left off";
        p.rows[0].icon_path = "../icons/icon_resume.png";
        p.rows[0].has_chevron = 1;

        p.rows[1].kind = EVO_RMLUI_ROW_VALUE;
        p.rows[1].title = "Decoder";
        p.rows[1].detail = "Hardware decoding preferred";
        p.rows[1].badge = "AUTO";
        p.rows[1].has_chevron = 1;

        p.rows[2].kind = EVO_RMLUI_ROW_VALUE;
        p.rows[2].title = "Upscaling";
        p.rows[2].detail = "Enhance lower resolution video";
        p.rows[2].badge = "AI";
        p.rows[2].has_chevron = 1;

        p.rows[3].kind = EVO_RMLUI_ROW_TOGGLE;
        p.rows[3].title = "120 Hz Output";
        p.rows[3].detail = "Smoother interface on supported displays";
        p.rows[3].toggle_on = 1;

        p.rows[4].kind = EVO_RMLUI_ROW_TOGGLE;
        p.rows[4].title = "HDR Output";
        p.rows[4].detail = "High dynamic range colors";
        p.rows[4].toggle_on = 0;

        p.rows[5].kind = EVO_RMLUI_ROW_VALUE;
        p.rows[5].title = "Motion Smoothing";
        p.rows[5].detail = "Frame interpolation";
        p.rows[5].badge = "OFF";
        p.rows[5].has_chevron = 1;
    } else {
        p.sidebar_focused = 0;
        p.section_active = 0;
        p.title = "Video & Display";
        p.subtitle = "PREFERENCES";
        p.counter = "8 ITEMS";
        p.row_count = 8;
        
        p.rows[0].kind = EVO_RMLUI_ROW_TOGGLE;
        p.rows[0].title = "120 Hz Output";
        p.rows[0].toggle_on = 1;

        p.rows[1].kind = EVO_RMLUI_ROW_VALUE;
        p.rows[1].title = "Upscaling";
        p.rows[1].badge = "AI";
        p.rows[1].has_chevron = 1;

        p.rows[2].kind = EVO_RMLUI_ROW_OPTION;
        p.rows[2].title = "Off";
        p.rows[2].toggle_on = 0;
        p.rows[2].is_focused = 1;

        p.rows[3].kind = EVO_RMLUI_ROW_OPTION;
        p.rows[3].title = "Bilinear";
        p.rows[3].toggle_on = 0;

        p.rows[4].kind = EVO_RMLUI_ROW_OPTION;
        p.rows[4].title = "Lanczos";
        p.rows[4].toggle_on = 0;

        p.rows[5].kind = EVO_RMLUI_ROW_OPTION;
        p.rows[5].title = "AI (Neural Network)";
        p.rows[5].toggle_on = 1;

        p.rows[6].kind = EVO_RMLUI_ROW_TOGGLE;
        p.rows[6].title = "HDR Output";
        p.rows[6].toggle_on = 0;
        p.rows[6].is_disabled = 1;

        p.rows[7].kind = EVO_RMLUI_ROW_ACTION;
        p.rows[7].title = "Resume Playback";
        p.rows[7].has_chevron = 1;
    }
}
