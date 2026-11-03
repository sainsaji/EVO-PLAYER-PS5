#include "evo_rmlui_bridge.h"

static void fill_settings_fixture(evo_rmlui_settings_params_t &p, int which)
{
    p = {};
    if (which == 0) {
        p.sidebar_focused = 1;
        p.section_active = 0;
        p.title = "Video & Display";
        p.subtitle = "DECODER, UPSCALING, 120 HZ & HDR";
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
    } else if (which >= 2) {
        /* #158 preview mock: which 2 = Deep Blacks, 3 = Upscaling */
        p.sidebar_focused = 0; p.section_active = 0;
        p.title = "Video & Display"; p.subtitle = "DECODER, UPSCALING, 120 HZ & HDR"; p.counter = "8 ITEMS"; p.row_count = 8;
        p.rows[0].kind = EVO_RMLUI_ROW_TOGGLE; p.rows[0].title = "HDR Output"; p.rows[0].detail = "High dynamic range colors"; p.rows[0].toggle_on = 1;
        p.rows[1].kind = EVO_RMLUI_ROW_VALUE; p.rows[1].has_chevron = 1;
        if (which == 2) {
            p.rows[1].title = "Deep Blacks"; p.rows[1].detail = "Pulls grey shadows to true black"; p.rows[1].badge = "LOW";
            p.rows[2].kind = EVO_RMLUI_ROW_OPTION; p.rows[2].title = "Off";
            p.rows[3].kind = EVO_RMLUI_ROW_OPTION; p.rows[3].title = "Low"; p.rows[3].toggle_on = 1;
            p.rows[4].kind = EVO_RMLUI_ROW_OPTION; p.rows[4].title = "High"; p.rows[4].is_focused = 1;
        } else {
            p.rows[1].title = "Upscaling"; p.rows[1].detail = "Sharpens video below screen resolution"; p.rows[1].badge = "ANIME4K";
            p.rows[2].kind = EVO_RMLUI_ROW_OPTION; p.rows[2].title = "Off";
            p.rows[3].kind = EVO_RMLUI_ROW_OPTION; p.rows[3].title = "FSR1"; p.rows[3].is_focused = 1;
            p.rows[4].kind = EVO_RMLUI_ROW_OPTION; p.rows[4].title = "Anime4K"; p.rows[4].toggle_on = 1;
        }
        for (int i = 1; i <= 4; ++i) {   /* #158: the setting's preview data rides on its value and option rows */
            const bool db = (which == 2);
            p.rows[i].preview = db ? EVO_RMLUI_PREVIEW_DEEP_BLACKS : EVO_RMLUI_PREVIEW_UPSCALING;
            p.rows[i].benefit = db ? "Dark scenes look deeper instead of washed out. Best on OLED and HDR screens."
                                   : "Soft video below your screen's resolution looks crisper. Higher modes cost more GPU.";
            p.rows[i].choice_count = (i == 1) ? 3 : 0;
            p.rows[i].choice[0] = "Off"; p.rows[i].choice[1] = db ? "Low" : "FSR1"; p.rows[i].choice[2] = db ? "High" : "Anime4K";
            p.rows[i].preview_index = (i == 1) ? 1 : i - 2;
        }
        for (int i = 2; i <= 4; ++i) { p.rows[i].choice_count = 3; p.rows[i].choice[0] = "Off"; p.rows[i].choice[1] = which == 2 ? "Low" : "FSR1"; p.rows[i].choice[2] = which == 2 ? "High" : "Anime4K"; }
        p.rows[5].kind = EVO_RMLUI_ROW_VALUE; p.rows[5].title = "Motion Smoothing"; p.rows[5].detail = "Frame interpolation"; p.rows[5].badge = "OFF"; p.rows[5].has_chevron = 1;
        p.rows[6].kind = EVO_RMLUI_ROW_ACTION; p.rows[6].title = "Resume Playback"; p.rows[6].has_chevron = 1;
        p.rows[7].kind = EVO_RMLUI_ROW_ACTION; p.rows[7].title = "Refresh Rate"; p.rows[7].has_chevron = 1;
    } else {
        p.sidebar_focused = 0;
        p.section_active = 0;
        p.title = "Video & Display";
        p.subtitle = "DECODER, UPSCALING, 120 HZ & HDR";
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
