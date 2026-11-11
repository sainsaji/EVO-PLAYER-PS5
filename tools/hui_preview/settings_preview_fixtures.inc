#ifndef SETTINGS_PREVIEW_FIXTURES_INC
#define SETTINGS_PREVIEW_FIXTURES_INC

#include "evo_rmlui_bridge.h"

#include <string>
#include <vector>

/*
 * settings_preview_fixtures.inc - Host-preview fixtures for #158 preview cards.
 *
 * Each fixture is a real section page with the previewed setting focused,
 * built from the setting's real makeDef titles, icons, and option labels,
 * and kPreviews benefit text.
 */

namespace
{

struct PvOptDef
{
    const char *label;
};

struct PvRowDef
{
    int kind; /* EVO_RMLUI_ROW_* */
    const char *title;
    const char *detail;
    const char *icon_path;
    const char *badge;
    int toggle_on;
    int preview;
    const char *benefit;
    std::vector<const char *> options;
    int current_opt;
    int focused_opt; /* -1 if the row itself is focused */
    bool is_theme;
};

static void build_preview_section(evo_rmlui_settings_params_t &p, int section,
                                  const char *title, const char *subtitle,
                                  const std::vector<PvRowDef> &rows)
{
    p = {};
    p.sidebar_focused = 0;
    p.section_active = section;
    p.title = title;
    p.subtitle = subtitle;

    const unsigned int theme_bgs[4] = {0x080d16, 0x0f0f11, 0x140c08, 0x051212};
    const unsigned int theme_accents[4] = {0xffcd00, 0xf2f2f5, 0xffa528, 0x3df5c0};

    int out_idx = 0;
    for (const auto &r : rows)
    {
        if (out_idx >= EVO_RMLUI_SETTINGS_ROWS)
            break;

        evo_rmlui_settings_row_t &row = p.rows[out_idx++];
        row.kind = r.kind;
        row.title = r.title;
        row.detail = r.detail ? r.detail : "";
        row.icon_path = r.icon_path ? r.icon_path : "";
        row.badge = r.badge ? r.badge : "";
        row.has_chevron = (r.kind != EVO_RMLUI_ROW_TOGGLE) ? 1 : 0;
        row.toggle_on = r.toggle_on;
        row.is_disabled = 0;
        row.is_focused = (r.focused_opt < 0 && r.preview != 0) ? 1 : 0;
        row.preview = r.preview;
        row.benefit = r.benefit ? r.benefit : "";
        row.choice_count = static_cast<int>(r.options.size());
        for (int c = 0; c < row.choice_count && c < EVO_RMLUI_PREVIEW_CHOICES; ++c)
            row.choice[c] = r.options[static_cast<size_t>(c)];

        if (r.is_theme)
        {
            for (int c = 0; c < row.choice_count && c < 4; ++c)
            {
                row.choice_bg[c] = theme_bgs[c];
                row.choice_accent[c] = theme_accents[c];
            }
        }

        if (r.kind == EVO_RMLUI_ROW_TOGGLE)
            row.preview_index = r.toggle_on ? 1 : 0;
        else if (r.kind == EVO_RMLUI_ROW_VALUE)
            row.preview_index = r.current_opt;
        else
            row.preview_index = 0;

        // If expanded VALUE row, append its options
        if (r.kind == EVO_RMLUI_ROW_VALUE && !r.options.empty() && r.focused_opt >= 0)
        {
            row.badge = r.options[static_cast<size_t>(r.current_opt)];
            for (int opt = 0; opt < static_cast<int>(r.options.size()) && out_idx < EVO_RMLUI_SETTINGS_ROWS; ++opt)
            {
                evo_rmlui_settings_row_t &opt_row = p.rows[out_idx++];
                opt_row.kind = EVO_RMLUI_ROW_OPTION;
                opt_row.title = r.options[static_cast<size_t>(opt)];
                opt_row.detail = "";
                opt_row.icon_path = "";
                opt_row.badge = "";
                opt_row.has_chevron = 0;
                opt_row.toggle_on = (opt == r.current_opt) ? 1 : 0;
                opt_row.is_disabled = 0;
                opt_row.is_focused = (opt == r.focused_opt) ? 1 : 0;
                opt_row.preview = r.preview;
                opt_row.preview_index = opt;
                opt_row.benefit = r.benefit ? r.benefit : "";
                opt_row.choice_count = static_cast<int>(r.options.size());
                for (int c = 0; c < opt_row.choice_count && c < EVO_RMLUI_PREVIEW_CHOICES; ++c)
                    opt_row.choice[c] = r.options[static_cast<size_t>(c)];
                if (r.is_theme)
                {
                    for (int c = 0; c < opt_row.choice_count && c < 4; ++c)
                    {
                        opt_row.choice_bg[c] = theme_bgs[c];
                        opt_row.choice_accent[c] = theme_accents[c];
                    }
                }
            }
        }
    }

    p.row_count = out_idx;
    static char s_counter[32];
    snprintf(s_counter, sizeof(s_counter), "%zu SETTINGS", rows.size());
    p.counter = s_counter;
}

} // namespace

static void fill_settings_pv_fixture(evo_rmlui_settings_params_t &p, const std::string &kind)
{
    if (kind == "ai_network")
    {
        build_preview_section(p, 0, "Video & Display", "DECODER, UPSCALING, 120 HZ & HDR", {
            {EVO_RMLUI_ROW_VALUE, "UPSCALING", "SHARPEN VIDEO SMALLER THAN THE SCREEN", "../icons/icon_sparkles.png", "AI", 0, 0, "", {"OFF", "SHARP", "AI"}, 2, -1, false},
            {EVO_RMLUI_ROW_VALUE, "AI NETWORK", "BIGGER IS SHARPER BUT HEAVIER - MAXIMUM IS FOR PS5 PRO", "../icons/icon_brain.png", "", 0, EVO_RMLUI_PREVIEW_AI_NETWORK, "A bigger network gives a sharper picture but is heavier. Maximum is for PS5 Pro.", {"AUTO", "STANDARD", "LARGE", "MAXIMUM (PRO)"}, 2, 2, false},
            {EVO_RMLUI_ROW_VALUE, "120 HZ OUTPUT", "SMOOTHER 24 FPS - THE TV GOES BLACK BRIEFLY ON EACH SWITCH", "../icons/icon_gauge.png", "OFF", 0, 0, "", {"OFF", "AUTO (ALWAYS 120 HZ)", "PLAYBACK ONLY"}, 0, -1, false}
        });
    }
    else if (kind == "hdr")
    {
        build_preview_section(p, 0, "Video & Display", "DECODER, UPSCALING, 120 HZ & HDR", {
            {EVO_RMLUI_ROW_VALUE, "120 HZ OUTPUT", "SMOOTHER 24 FPS - THE TV GOES BLACK BRIEFLY ON EACH SWITCH", "../icons/icon_gauge.png", "OFF", 0, 0, "", {"OFF", "AUTO (ALWAYS 120 HZ)", "PLAYBACK ONLY"}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "HDR OUTPUT", "HDR10 FOR HDR VIDEO - THE TV GOES BLACK BRIEFLY AT START & END", "../icons/icon_sun.png", "", 0, EVO_RMLUI_PREVIEW_HDR, "Highlights like the sun and lamps get brighter without clipping. Needs an HDR10 TV.", {"OFF (TONE-MAP TO SDR)", "AUTO (MATCH CONTENT)"}, 1, 1, false},
            {EVO_RMLUI_ROW_TOGGLE, "RESUME PLAYBACK", "REMEMBER PLAYBACK POSITION", "../icons/icon_resume.png", "", 1, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "motion")
    {
        build_preview_section(p, 5, "Experimental", "UNFINISHED WORK - EXPECT ARTEFACTS", {
            {EVO_RMLUI_ROW_TOGGLE, "MATCH VIDEO FRAME RATE", "24P / 50 HZ OUTPUT DURING PLAYBACK - FALLS BACK TO 120 HZ OR 60 HZ", "../icons/icon_gauge.png", "", 0, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "MOTION SMOOTHING", "24 -> 60 FPS ON THE GPU - SOFT EDGES AND HALOES AROUND FAST MOTION", "../icons/icon_activity.png", "", 0, EVO_RMLUI_PREVIEW_MOTION, "Pans and sports look fluid. Films can look too smooth, and fast motion can show haloes.", {"OFF", "LOW", "HIGH"}, 2, 2, false},
            {EVO_RMLUI_ROW_VALUE, "DEEP BLACKS", "PULLS ELEVATED SHADOWS TO TRUE OLED BLACK", "../icons/icon_sparkles.png", "OFF", 0, 0, "", {"OFF", "LOW", "HIGH"}, 0, -1, false}
        });
    }
    else if (kind == "refresh")
    {
        build_preview_section(p, 0, "Video & Display", "DECODER, UPSCALING, 120 HZ & HDR", {
            {EVO_RMLUI_ROW_VALUE, "AI NETWORK", "BIGGER IS SHARPER BUT HEAVIER - MAXIMUM IS FOR PS5 PRO", "../icons/icon_brain.png", "LARGE", 0, 0, "", {"AUTO", "STANDARD", "LARGE", "MAXIMUM (PRO)"}, 2, -1, false},
            {EVO_RMLUI_ROW_VALUE, "120 HZ OUTPUT", "SMOOTHER 24 FPS - THE TV GOES BLACK BRIEFLY ON EACH SWITCH", "../icons/icon_gauge.png", "", 0, EVO_RMLUI_PREVIEW_REFRESH, "24 fps film plays more evenly on a 120 Hz output. The TV goes black briefly on each switch.", {"OFF", "AUTO (ALWAYS 120 HZ)", "PLAYBACK ONLY"}, 1, 1, false},
            {EVO_RMLUI_ROW_VALUE, "HDR OUTPUT", "HDR10 FOR HDR VIDEO - THE TV GOES BLACK BRIEFLY AT START & END", "../icons/icon_sun.png", "AUTO", 0, 0, "", {"OFF (TONE-MAP TO SDR)", "AUTO (MATCH CONTENT)"}, 1, -1, false}
        });
    }
    else if (kind == "match_rate")
    {
        build_preview_section(p, 5, "Experimental", "UNFINISHED WORK - EXPECT ARTEFACTS", {
            {EVO_RMLUI_ROW_VALUE, "DEEP BLACKS", "PULLS ELEVATED SHADOWS TO TRUE OLED BLACK", "../icons/icon_sparkles.png", "OFF", 0, 0, "", {"OFF", "LOW", "HIGH"}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "MATCH VIDEO FRAME RATE", "24P / 50 HZ OUTPUT DURING PLAYBACK - FALLS BACK TO 120 HZ OR 60 HZ", "../icons/icon_gauge.png", "", 1, EVO_RMLUI_PREVIEW_MATCH_RATE, "Outputs the video's own rate (24p or 50 Hz) so motion has no judder.", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "MOTION SMOOTHING", "24 -> 60 FPS ON THE GPU - SOFT EDGES AND HALOES AROUND FAST MOTION", "../icons/icon_activity.png", "OFF", 0, 0, "", {"OFF", "LOW", "HIGH"}, 0, -1, false}
        });
    }
    else if (kind == "aspect")
    {
        build_preview_section(p, 0, "Video & Display", "DECODER, UPSCALING, 120 HZ & HDR", {
            {EVO_RMLUI_ROW_VALUE, "VIDEO DECODER", "WHICH BACKEND DECODES VIDEO", "../icons/icon_cpu.png", "AUTO", 0, 0, "", {"AUTO", "HARDWARE", "SOFTWARE"}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "DEFAULT ASPECT RATIO", "HOW VIDEO FILLS THE SCREEN", "../icons/icon_aspect.png", "", 0, EVO_RMLUI_PREVIEW_ASPECT, "Sets how video fills the screen: fit keeps its shape, fill crops the edges, stretch fills the screen and distorts.", {"FIT", "FILL", "STRETCH"}, 0, 0, false},
            {EVO_RMLUI_ROW_TOGGLE, "RESUME PLAYBACK", "REMEMBER PLAYBACK POSITION", "../icons/icon_resume.png", "", 1, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "sub_auto")
    {
        build_preview_section(p, 2, "Subtitles", "PREFERENCES & APPEARANCE", {
            {EVO_RMLUI_ROW_TOGGLE, "AUTO SUBTITLES", "AUTOMATICALLY LOAD SUBTITLES ON PLAYBACK", "../icons/icon_subtitles.png", "", 1, EVO_RMLUI_PREVIEW_SUB_AUTO, "Loads subtitles on its own when a file has them.", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "DEFAULT FONT STYLE", "ON-SCREEN TEXT TYPEFACE", "../icons/icon_type.png", "ROUNDED", 0, 0, "", {"STANDARD", "ROUNDED", "BOLD", "CONDENSED"}, 1, -1, false},
            {EVO_RMLUI_ROW_VALUE, "SECONDARY SUBTITLE POSITION", "WHERE THE SECOND LINE OF DIALOGUE GOES", "../icons/icon_subtitles.png", "STACKED ABOVE", 0, 0, "", {"STACKED ABOVE", "TOP OF SCREEN"}, 0, -1, false}
        });
    }
    else if (kind == "sub_font")
    {
        build_preview_section(p, 2, "Subtitles", "PREFERENCES & APPEARANCE", {
            {EVO_RMLUI_ROW_TOGGLE, "AUTO SUBTITLES", "AUTOMATICALLY LOAD SUBTITLES ON PLAYBACK", "../icons/icon_subtitles.png", "", 1, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "DEFAULT FONT STYLE", "ON-SCREEN TEXT TYPEFACE", "../icons/icon_type.png", "", 0, EVO_RMLUI_PREVIEW_SUB_FONT, "Picks the typeface for on-screen subtitles.", {"STANDARD", "ROUNDED", "BOLD", "CONDENSED"}, 1, 1, false},
            {EVO_RMLUI_ROW_VALUE, "SECONDARY SUBTITLE POSITION", "WHERE THE SECOND LINE OF DIALOGUE GOES", "../icons/icon_subtitles.png", "STACKED ABOVE", 0, 0, "", {"STACKED ABOVE", "TOP OF SCREEN"}, 0, -1, false}
        });
    }
    else if (kind == "sub_pos")
    {
        build_preview_section(p, 2, "Subtitles", "PREFERENCES & APPEARANCE", {
            {EVO_RMLUI_ROW_VALUE, "DEFAULT FONT STYLE", "ON-SCREEN TEXT TYPEFACE", "../icons/icon_type.png", "ROUNDED", 0, 0, "", {"STANDARD", "ROUNDED", "BOLD", "CONDENSED"}, 1, -1, false},
            {EVO_RMLUI_ROW_VALUE, "SECONDARY SUBTITLE POSITION", "WHERE THE SECOND LINE OF DIALOGUE GOES", "../icons/icon_subtitles.png", "", 0, EVO_RMLUI_PREVIEW_SUB_POS, "Where the second line of dialogue goes when two tracks are shown.", {"STACKED ABOVE", "TOP OF SCREEN"}, 1, 1, false},
            {EVO_RMLUI_ROW_VALUE, "SECONDARY SUBTITLE COLOUR", "KEEPS THE TWO LINES APART", "../icons/icon_palette.png", "CYAN", 0, 0, "", {"YELLOW", "CYAN", "WHITE"}, 1, -1, false}
        });
    }
    else if (kind == "sub_colour")
    {
        build_preview_section(p, 2, "Subtitles", "PREFERENCES & APPEARANCE", {
            {EVO_RMLUI_ROW_VALUE, "SECONDARY SUBTITLE POSITION", "WHERE THE SECOND LINE OF DIALOGUE GOES", "../icons/icon_subtitles.png", "STACKED ABOVE", 0, 0, "", {"STACKED ABOVE", "TOP OF SCREEN"}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "SECONDARY SUBTITLE COLOUR", "KEEPS THE TWO LINES APART", "../icons/icon_palette.png", "", 0, EVO_RMLUI_PREVIEW_SUB_COLOUR, "A different colour keeps the two lines apart.", {"YELLOW", "CYAN", "WHITE"}, 1, 1, false},
            {EVO_RMLUI_ROW_TOGGLE, "AUTO SUBTITLES", "AUTOMATICALLY LOAD SUBTITLES ON PLAYBACK", "../icons/icon_subtitles.png", "", 1, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "sub_size")
    {
        build_preview_section(p, 2, "Subtitles", "PREFERENCES & APPEARANCE", {
            {EVO_RMLUI_ROW_VALUE, "DEFAULT FONT STYLE", "ON-SCREEN TEXT TYPEFACE", "../icons/icon_type.png", "STANDARD", 0, 0, "", {"STANDARD", "ROUNDED", "BOLD", "CONDENSED"}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "SUBTITLE SIZE", "HOW BIG SUBTITLES ARE", "../icons/icon_type.png", "", 0, EVO_RMLUI_PREVIEW_SUB_SIZE, "How big subtitles are.", {"SMALL", "MEDIUM", "LARGE", "EXTRA LARGE"}, 3, 1, false},
            {EVO_RMLUI_ROW_TOGGLE, "AUTO SUBTITLES", "AUTOMATICALLY LOAD SUBTITLES ON PLAYBACK", "../icons/icon_subtitles.png", "", 1, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "sub_text_colour")
    {
        build_preview_section(p, 2, "Subtitles", "PREFERENCES & APPEARANCE", {
            {EVO_RMLUI_ROW_VALUE, "DEFAULT FONT STYLE", "ON-SCREEN TEXT TYPEFACE", "../icons/icon_type.png", "STANDARD", 0, 0, "", {"STANDARD", "ROUNDED", "BOLD", "CONDENSED"}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "SUBTITLE COLOUR", "THE MAIN LINE OF DIALOGUE", "../icons/icon_palette.png", "", 0, EVO_RMLUI_PREVIEW_SUB_TEXT_COLOUR, "The colour of the main subtitle line.", {"WHITE", "YELLOW", "CYAN", "GREEN"}, 1, 1, false},
            {EVO_RMLUI_ROW_TOGGLE, "AUTO SUBTITLES", "AUTOMATICALLY LOAD SUBTITLES ON PLAYBACK", "../icons/icon_subtitles.png", "", 1, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "sub_lift")
    {
        build_preview_section(p, 2, "Subtitles", "PREFERENCES & APPEARANCE", {
            {EVO_RMLUI_ROW_VALUE, "DEFAULT FONT STYLE", "ON-SCREEN TEXT TYPEFACE", "../icons/icon_type.png", "STANDARD", 0, 0, "", {"STANDARD", "ROUNDED", "BOLD", "CONDENSED"}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "SUBTITLE POSITION", "RAISE SUBTITLES FROM THE BOTTOM", "../icons/icon_subtitles.png", "", 0, EVO_RMLUI_PREVIEW_SUB_LIFT, "Raises subtitles from the bottom.", {"BOTTOM", "RAISED", "HIGHER"}, 2, 1, false},
            {EVO_RMLUI_ROW_TOGGLE, "AUTO SUBTITLES", "AUTOMATICALLY LOAD SUBTITLES ON PLAYBACK", "../icons/icon_subtitles.png", "", 1, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "sub_background")
    {
        build_preview_section(p, 2, "Subtitles", "PREFERENCES & APPEARANCE", {
            {EVO_RMLUI_ROW_VALUE, "DEFAULT FONT STYLE", "ON-SCREEN TEXT TYPEFACE", "../icons/icon_type.png", "STANDARD", 0, 0, "", {"STANDARD", "ROUNDED", "BOLD", "CONDENSED"}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "SUBTITLE BACKGROUND", "KEEPS TEXT READABLE ON BRIGHT SCENES", "../icons/icon_subtitles.png", "", 0, EVO_RMLUI_PREVIEW_SUB_BACKGROUND, "What sits behind the text so it stays readable on bright scenes.", {"BOX", "SHADOW", "OUTLINE", "NONE"}, 2, 1, false},
            {EVO_RMLUI_ROW_TOGGLE, "AUTO SUBTITLES", "AUTOMATICALLY LOAD SUBTITLES ON PLAYBACK", "../icons/icon_subtitles.png", "", 1, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "theme")
    {
        build_preview_section(p, 3, "Interface & Storage", "THEMES, CONTROLS & BROWSING", {
            {EVO_RMLUI_ROW_TOGGLE, "CONTROLLER LIGHTBAR", "DUALSENSE LIGHT FOLLOWS THE THEME ACCENT", "../icons/icon_gamepad.png", "", 1, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "THEME", "COLOR PALETTE & ACCENTS", "../icons/icon_palette.png", "", 0, EVO_RMLUI_PREVIEW_THEME, "Changes the colours of the whole app.", {"MIDNIGHT", "CARBON", "EMBER", "AURORA"}, 0, 0, true},
            {EVO_RMLUI_ROW_TOGGLE, "FOLDERS FIRST", "USB FILE BROWSER SORTING", "../icons/icon_folder.png", "", 1, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "lightbar")
    {
        build_preview_section(p, 3, "Interface & Storage", "THEMES, CONTROLS & BROWSING", {
            {EVO_RMLUI_ROW_VALUE, "THEME", "COLOR PALETTE & ACCENTS", "../icons/icon_palette.png", "MIDNIGHT", 0, 0, "", {"MIDNIGHT", "CARBON", "EMBER", "AURORA"}, 0, -1, true},
            {EVO_RMLUI_ROW_TOGGLE, "CONTROLLER LIGHTBAR", "DUALSENSE LIGHT FOLLOWS THE THEME ACCENT", "../icons/icon_gamepad.png", "", 1, EVO_RMLUI_PREVIEW_LIGHTBAR, "The DualSense light follows the theme accent.", {}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "FOLDERS FIRST", "USB FILE BROWSER SORTING", "../icons/icon_folder.png", "", 1, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "channels")
    {
        build_preview_section(p, 1, "Audio", "OUTPUT CHANNELS, SPEAKERS & SOUNDS", {
            {EVO_RMLUI_ROW_TOGGLE, "AUDIO PASSTHROUGH", "BITSTREAM DOLBY & DTS TO RECEIVER OVER HDMI", "../icons/icon_volume.png", "", 0, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "OUTPUT CHANNELS", "STEREO DOWNMIXES 5.1 & 7.1 TO 2.0", "../icons/icon_volume.png", "", 0, EVO_RMLUI_PREVIEW_CHANNELS, "Stereo mixes 5.1 and 7.1 down to two speakers or headphones.", {"AUTO", "STEREO (2.0)"}, 0, 0, false},
            {EVO_RMLUI_ROW_ACTION, "SURROUND SOUND TEST", "5.1 & 7.1 SPEAKER CHANNEL VERIFICATION", "../icons/icon_speaker.png", "OPEN", 0, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "passthrough")
    {
        build_preview_section(p, 1, "Audio", "OUTPUT CHANNELS, SPEAKERS & SOUNDS", {
            {EVO_RMLUI_ROW_VALUE, "OUTPUT CHANNELS", "STEREO DOWNMIXES 5.1 & 7.1 TO 2.0", "../icons/icon_volume.png", "AUTO", 0, 0, "", {"AUTO", "STEREO (2.0)"}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "AUDIO PASSTHROUGH", "BITSTREAM DOLBY & DTS TO RECEIVER OVER HDMI", "../icons/icon_volume.png", "", 1, EVO_RMLUI_PREVIEW_PASSTHROUGH, "Sends Dolby and DTS untouched to your receiver over HDMI.", {}, 0, -1, false},
            {EVO_RMLUI_ROW_ACTION, "SURROUND SOUND TEST", "5.1 & 7.1 SPEAKER CHANNEL VERIFICATION", "../icons/icon_speaker.png", "OPEN", 0, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "surround")
    {
        build_preview_section(p, 1, "Audio", "OUTPUT CHANNELS, SPEAKERS & SOUNDS", {
            {EVO_RMLUI_ROW_TOGGLE, "AUDIO PASSTHROUGH", "BITSTREAM DOLBY & DTS TO RECEIVER OVER HDMI", "../icons/icon_volume.png", "", 1, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_ACTION, "SURROUND SOUND TEST", "5.1 & 7.1 SPEAKER CHANNEL VERIFICATION", "../icons/icon_speaker.png", "OPEN", 0, EVO_RMLUI_PREVIEW_SURROUND, "Plays a tone through each speaker so you can check your layout.", {}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "NAVIGATION SOUNDS", "AUDIO FEEDBACK ON D-PAD & BUTTONS", "../icons/icon_listener.png", "", 1, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "deep_blacks")
    {
        build_preview_section(p, 5, "Experimental", "UNFINISHED WORK - EXPECT ARTEFACTS", {
            {EVO_RMLUI_ROW_VALUE, "MOTION SMOOTHING", "24 -> 60 FPS ON THE GPU - SOFT EDGES AND HALOES AROUND FAST MOTION", "../icons/icon_activity.png", "OFF", 0, 0, "", {"OFF", "LOW", "HIGH"}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "DEEP BLACKS", "PULLS ELEVATED SHADOWS TO TRUE OLED BLACK", "../icons/icon_sparkles.png", "", 0, EVO_RMLUI_PREVIEW_DEEP_BLACKS, "Dark scenes look deeper instead of washed out. Best on OLED and HDR screens.", {"OFF", "LOW", "HIGH"}, 2, 2, false},
            {EVO_RMLUI_ROW_TOGGLE, "MATCH VIDEO FRAME RATE", "24P / 50 HZ OUTPUT DURING PLAYBACK - FALLS BACK TO 120 HZ OR 60 HZ", "../icons/icon_gauge.png", "", 0, 0, "", {}, 0, -1, false}
        });
    }
    else if (kind == "upscaling")
    {
        build_preview_section(p, 0, "Video & Display", "DECODER, UPSCALING, 120 HZ & HDR", {
            {EVO_RMLUI_ROW_VALUE, "VIDEO DECODER", "WHICH BACKEND DECODES VIDEO", "../icons/icon_cpu.png", "AUTO", 0, 0, "", {"AUTO", "HARDWARE", "SOFTWARE"}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "UPSCALING", "SHARPEN VIDEO SMALLER THAN THE SCREEN", "../icons/icon_sparkles.png", "", 0, EVO_RMLUI_PREVIEW_UPSCALING, "Soft video below your screen's resolution looks crisper. Higher modes cost more GPU.", {"OFF", "SHARP", "AI"}, 2, 2, false},
            {EVO_RMLUI_ROW_VALUE, "AI NETWORK", "BIGGER IS SHARPER BUT HEAVIER - MAXIMUM IS FOR PS5 PRO", "../icons/icon_brain.png", "LARGE", 0, 0, "", {"AUTO", "STANDARD", "LARGE", "MAXIMUM (PRO)"}, 2, -1, false}
        });
    }
    else if (kind == "info_resume")
    {
        build_preview_section(p, 0, "Video & Display", "DECODER, UPSCALING, 120 HZ & HDR", {
            {EVO_RMLUI_ROW_VALUE, "VIDEO DECODER", "WHICH BACKEND DECODES VIDEO", "../icons/icon_cpu.png", "AUTO", 0, 0, "", {"AUTO", "HARDWARE", "SOFTWARE"}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "RESUME PLAYBACK", "REMEMBER PLAYBACK POSITION", "../icons/icon_resume.png", "", 1, EVO_RMLUI_PREVIEW_INFO, "Picks a film up where you stopped instead of from the start.", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "DEFAULT ASPECT RATIO", "HOW VIDEO FILLS THE SCREEN", "../icons/icon_aspect.png", "FIT", 0, 0, "", {"FIT", "FILL", "STRETCH"}, 0, -1, false}
        });
    }
    /* ---- 2026-11 design round: full real sections (layout trial) and the new previews ---- */
    else if (kind.rfind("audio_full", 0) == 0 || kind.rfind("upmix_", 0) == 0 || kind.rfind("phone_audio_", 0) == 0)
    {
        const bool up_focus = kind == "audio_full" || kind.rfind("upmix_", 0) == 0;
        const bool on = kind.find("_off") == std::string::npos;
        const int up_pv = up_focus ? EVO_RMLUI_PREVIEW_UPMIX : 0;
        const int ph_pv = up_focus ? 0 : EVO_RMLUI_PREVIEW_PHONE_AUDIO;
        build_preview_section(p, 1, "Audio", "OUTPUT CHANNELS, SPEAKERS & SOUNDS", {
            {EVO_RMLUI_ROW_VALUE, "VOLUME BOOST", "RAISE QUIET FILES. LOUD PASSAGES CLIP, NEVER WRAP", "../icons/icon_volume.png", "OFF", 0, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "AUDIO DELAY", "SHIFTS SOUND AGAINST PICTURE TO FIX LIP-SYNC", "../icons/icon_volume.png", "0 ms", 0, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "OUTPUT CHANNELS", "STEREO DOWNMIXES 5.1 & 7.1 TO 2.0", "../icons/icon_volume.png", "AUTO", 0, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "UPMIX STEREO TO 4.0", "EXPANDS 2.0 STEREO TO 4.0 QUADRAPHONIC SURROUND", "../icons/icon_speaker.png", "", up_focus ? on : 1, up_pv,
             "Extracts ambient sound and expands stereo tracks across your surround speakers (quadraphonic 4.0). Requires Output Channels set to Auto.", {}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "AUDIO PASSTHROUGH", "BITSTREAM DOLBY & DTS TO RECEIVER OVER HDMI", "../icons/icon_volume.png", "", 0, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "CONTROLLER PHONE AUDIO", "ROUTES IN-MOVIE PHONE CALLS TO THE DUALSENSE SPEAKER", "../icons/icon_volume.png", "", up_focus ? 1 : on, ph_pv,
             "Plays in-movie phone conversations and telephone voices through your DualSense controller speaker, just like in games.", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "AUDIO LANGUAGE", "THE TRACK A FILM STARTS WITH", "../icons/icon_volume.png", "AUTO", 0, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_ACTION, "SURROUND SOUND TEST", "5.1 & 7.1 SPEAKER CHANNEL VERIFICATION", "../icons/icon_speaker.png", "", 0, 0, "", {}, 0, -1, false},
        });
    }
    else if (kind.rfind("iface_", 0) == 0)
    {
        /* iface_keyboard: the row the screenshots showed cut; iface_cast_on/off, iface_phonecast_on/off, iface_ytlink */
        const bool on = kind.find("_off") == std::string::npos;
        int kb = 0, pc = 0, cp = 0, yt = 0;
        if (kind == "iface_keyboard") kb = EVO_RMLUI_PREVIEW_KEYBOARD;
        else if (kind.rfind("iface_phonecast", 0) == 0) pc = EVO_RMLUI_PREVIEW_PHONE_CAST;
        else if (kind.rfind("iface_cast", 0) == 0) cp = EVO_RMLUI_PREVIEW_CAST_PLAYER;
        else yt = EVO_RMLUI_PREVIEW_YT_LINK;
        build_preview_section(p, 3, "Interface & Storage", "THEMES, CONTROLS & BROWSING", {
            {EVO_RMLUI_ROW_VALUE, "THEME", "COLOR PALETTE & ACCENTS", "../icons/icon_palette.png", "MIDNIGHT", 0, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "CONTROLLER LIGHTBAR", "DUALSENSE LIGHT FOLLOWS THE THEME ACCENT", "../icons/icon_gamepad.png", "", 0, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "FOLDERS FIRST", "USB FILE BROWSER SORTING", "../icons/icon_folder.png", "", 1, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_VALUE, "KEYBOARD INPUT", "TEXT ENTRY METHOD", "../icons/icon_keyboard.png", "VIRTUAL KEYBOARD", 0, kb,
             "The virtual keyboard is drawn by EVO. The native one is the PS5 system keyboard.", {"VIRTUAL KEYBOARD", "NATIVE PS5 IME"}, 0, -1, false},
            {EVO_RMLUI_ROW_ACTION, "PHONE & LOG PORT", "PHONE KEYBOARD LINK AND LIVE LOG - CROSS TO CHANGE", "../icons/icon_keyboard.png", "9780", 0, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "PHONE CASTING (DIAL)", "CAST YOUTUBE FROM YOUR PHONE (PORT 8765)", "../icons/icon_tv.png", "", pc ? on : 1, pc,
             "Allows casting YouTube from your phone directly to EVO Player (DIAL receiver on port 8765).", {}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "CAST TO EVO PLAYER", "PLAY CAST IN NATIVE PLAYER (UP TO 1080P)", "../icons/icon_tv.png", "", cp ? on : 1, cp,
             "Plays YouTube casts in EVO Player, full length up to 1080p60, instead of the 720p WebKit browser.", {}, 0, -1, false},
            {EVO_RMLUI_ROW_ACTION, "PLAY A YOUTUBE LINK", "PASTE OR TYPE A LINK - PLAYS FULL LENGTH (UP TO 1080P60)", "../icons/icon_tv.png", "", 0, yt,
             "Paste or type a YouTube link (or use the phone page) and EVO plays the whole video, up to 1080p60.", {}, 0, -1, false},
            {EVO_RMLUI_ROW_TOGGLE, "ASK WHICH LIVE STREAM", "PICK THE FORMAT AND QUALITY WHEN YOU OPEN A CHANNEL", "../icons/icon_tv.png", "", 1, 0, "", {}, 0, -1, false},
        });
    }
    else if (kind == "info_quit")
    {
        build_preview_section(p, 4, "System & Diagnostics", "DIAGNOSTICS & SYSTEM MANAGEMENT", {
            {EVO_RMLUI_ROW_ACTION, "DEVELOPER TOOLS", "SYSTEM DIAGNOSTICS & PERFORMANCE STATS", "../icons/icon_developer_tools.png", "OPEN", 0, 0, "", {}, 0, -1, false},
            {EVO_RMLUI_ROW_ACTION, "QUIT EVO", "RELEASE EVERYTHING, THEN CLOSE FROM THE SWITCHER", "../icons/icon_power.png", "QUIT", 0, EVO_RMLUI_PREVIEW_INFO, "Releases everything, then close EVO from the switcher.", {}, 0, -1, false},
            {EVO_RMLUI_ROW_ACTION, "CONSOLE", "DETECTED HARDWARE MODEL", "../icons/icon_tv.png", "PS5", 0, 0, "", {}, 0, -1, false}
        });
    }
}

#endif /* SETTINGS_PREVIEW_FIXTURES_INC */
