#include "evo/screens/SettingsScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_toast.h"
#include "evo_theme.h"
#include "evo_boot_log.h"
#include "evo_hw.h"
#include "evo_agc_runtime.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

#ifndef EVO_PLAYER_VERSION
#define EVO_PLAYER_VERSION "v0.11.0-final"
#endif

namespace evo {

namespace {

constexpr int kMaxDefs    = 16;
constexpr int kMaxOptions = EVO_THEME_MAX;   /* themes are the longest list */

/*
 * Every setting the screen can show, by name rather than by position (#118).
 * Toggling, picking a choice and running an action all dispatch on this key,
 * so a section's rows can be added to or reordered without touching any of
 * that code. Sections are just lists of keys - see kSections below.
 */
enum class SettingKey : int {
    /* VIDEO & DISPLAY */
    DefaultAspect,
    ResumePlayback,
    VideoDecoder,
    Upscaling,
    AiNetwork,
    RefreshRate,
    HdrOutput,
    /* AUDIO */
    OutputChannels,
    SurroundTest,
    NavigationSounds,
    /* SUBTITLES */
    AutoSubtitles,
    SubtitleFont,
    SecondaryPosition,
    SecondaryColour,
    /* INTERFACE & STORAGE */
    Theme,
    Lightbar,
    FoldersFirst,
    KeyboardInput,
    AskStream,
    /* SYSTEM & DIAGNOSTICS */
    CompatReport,
    DebugOverlay,
    DeveloperTools,
    ConsoleModel,
    QuitEvo,
    /* EXPERIMENTAL */
    MotionSmoothing,
};

/*
 * One description of a setting, independent of how it is drawn.
 *
 * A VALUE setting carries every choice it has, not just the current one, so the
 * row can expand in place and show the user what actually exists - picking a
 * theme used to mean cycling a badge blind, with no way to see the list.
 */
struct SettingDef {
    SettingKey  key;
    const char* title;
    const char* detail;
    const char* icon;
    int         kind;
    bool        toggle_on;
    const char* badge;
    int         opt_count;
    int         opt_current;
    const char* opt_label[kMaxOptions];
    bool        disabled;   /* depends on another setting: dimmed, Cross does nothing */
};

const SettingKey kVideoKeys[] = {
    SettingKey::DefaultAspect, SettingKey::ResumePlayback, SettingKey::VideoDecoder,
    SettingKey::Upscaling, SettingKey::AiNetwork, SettingKey::RefreshRate,
    SettingKey::HdrOutput,
};
const SettingKey kAudioKeys[] = {
    SettingKey::OutputChannels, SettingKey::SurroundTest, SettingKey::NavigationSounds,
};
const SettingKey kSubtitleKeys[] = {
    SettingKey::AutoSubtitles, SettingKey::SubtitleFont,
    SettingKey::SecondaryPosition, SettingKey::SecondaryColour,
};
const SettingKey kInterfaceKeys[] = {
    SettingKey::Theme, SettingKey::Lightbar, SettingKey::FoldersFirst,
    SettingKey::KeyboardInput, SettingKey::AskStream,
};
const SettingKey kSystemKeys[] = {
    SettingKey::CompatReport, SettingKey::DebugOverlay, SettingKey::DeveloperTools,
    SettingKey::ConsoleModel, SettingKey::QuitEvo,
};
/*
 * Things that work but are not finished. They stay out of the other sections so
 * nobody turns one on expecting a shipped feature, and they default to off.
 */
const SettingKey kExperimentalKeys[] = {
    SettingKey::MotionSmoothing,
};

struct SectionSpec {
    const char*       title;
    const char*       subtitle;
    const char*       name;     /* screen name for the lifecycle FSM */
    ScreenId          screen;
    const SettingKey* keys;
    int               count;
};

template <std::size_t N>
constexpr SectionSpec makeSection(const char* title, const char* subtitle, const char* name,
                                  ScreenId screen, const SettingKey (&keys)[N]) {
    static_assert(N <= kMaxDefs, "settings section holds more than kMaxDefs settings");
    return {title, subtitle, name, screen, keys, static_cast<int>(N)};
}

/* Order here is the sidebar order in settings.rml (sb-0 .. sb-4). */
const SectionSpec kSections[] = {
    makeSection("VIDEO & DISPLAY", "DECODER, UPSCALING, 120 HZ & HDR",
                "SettingsPlaybackScreen", ScreenId::SettingsPlayback, kVideoKeys),
    makeSection("AUDIO", "OUTPUT CHANNELS, SPEAKERS & SOUNDS",
                "SettingsAudioScreen", ScreenId::SettingsAudio, kAudioKeys),
    makeSection("SUBTITLES", "PREFERENCES & APPEARANCE",
                "SettingsSubtitlesScreen", ScreenId::SettingsSubtitles, kSubtitleKeys),
    makeSection("INTERFACE & STORAGE", "THEMES, CONTROLS & BROWSING",
                "SettingsInterfaceScreen", ScreenId::SettingsInterface, kInterfaceKeys),
    makeSection("SYSTEM & DIAGNOSTICS", "DIAGNOSTICS & SYSTEM MANAGEMENT",
                "SettingsSystemScreen", ScreenId::SettingsSystem, kSystemKeys),
    makeSection("EXPERIMENTAL", "UNFINISHED WORK - EXPECT ARTEFACTS",
                "SettingsExperimentalScreen", ScreenId::SettingsExperimental,
                kExperimentalKeys),
};
constexpr int kSectionCount = static_cast<int>(sizeof kSections / sizeof kSections[0]);

int sectionForScreen(ScreenId id) {
    for (int s = 0; s < kSectionCount; ++s)
        if (kSections[s].screen == id) return s;
    return 0;
}

SettingDef toggleDef(SettingKey key, const char* title, const char* detail,
                     const char* icon, bool on) {
    SettingDef d{};
    d.key = key; d.title = title; d.detail = detail; d.icon = icon;
    d.kind = EVO_RMLUI_ROW_TOGGLE; d.toggle_on = on; d.badge = "";
    return d;
}

SettingDef actionDef(SettingKey key, const char* title, const char* detail,
                     const char* icon, const char* badge) {
    SettingDef d{};
    d.key = key; d.title = title; d.detail = detail; d.icon = icon;
    d.kind = EVO_RMLUI_ROW_ACTION; d.badge = badge;
    return d;
}

/* The caller fills opt_label[0 .. count-1]. */
SettingDef valueDef(SettingKey key, const char* title, const char* detail,
                    const char* icon, int count, int current) {
    SettingDef d{};
    d.key = key; d.title = title; d.detail = detail; d.icon = icon;
    d.kind = EVO_RMLUI_ROW_VALUE; d.badge = "";
    d.opt_count = count; d.opt_current = current;
    return d;
}

SettingDef makeDef(SettingKey key, ISettingsService* settings) {
    SettingDef d{};
    switch (key) {
    case SettingKey::DefaultAspect:
        d = valueDef(key, "DEFAULT ASPECT RATIO", "HOW VIDEO FILLS THE SCREEN",
                     "../icons/icon_aspect.png", 3,
                     static_cast<int>(settings->getDefaultViewMode()));
        for (int i = 0; i < 3; ++i)
            d.opt_label[i] = settings->getViewModeName(static_cast<ViewMode>(i));
        break;

    case SettingKey::ResumePlayback:
        d = toggleDef(key, "RESUME PLAYBACK", "REMEMBER PLAYBACK POSITION",
                      "../icons/icon_resume.png", settings->isResumePlaybackEnabled());
        break;

    case SettingKey::VideoDecoder:
        d = valueDef(key, "VIDEO DECODER", "WHICH BACKEND DECODES VIDEO",
                     "../icons/icon_cpu.png", 3,
                     static_cast<int>(settings->getVideoDecoderPreference()));
        for (int i = 0; i < 3; ++i)
            d.opt_label[i] = settings->getDecoderPreferenceBadge(static_cast<DecoderPreference>(i));
        break;

    case SettingKey::Upscaling:
        d = valueDef(key, "UPSCALING", "SHARPEN VIDEO SMALLER THAN THE SCREEN",
                     "../icons/icon_sparkles.png", 3, static_cast<int>(settings->getUpscaler()));
        for (int i = 0; i < 3; ++i)
            d.opt_label[i] = settings->getUpscalerName(static_cast<Upscaler>(i));
        break;

    case SettingKey::AiNetwork:
        d = valueDef(key, "AI NETWORK", "BIGGER IS SHARPER BUT HEAVIER - MAXIMUM IS FOR PS5 PRO",
                     "../icons/icon_brain.png", 4, static_cast<int>(settings->getAiNetwork()));
        for (int i = 0; i < 4; ++i)
            d.opt_label[i] = settings->getAiNetworkName(static_cast<AiNetwork>(i));
        /* Only AI runs a network; Sharp (FSR 1) and Off have nothing to pick. */
        if (settings->getUpscaler() != Upscaler::AI) {
            d.disabled = true;
            d.detail = "ONLY USED WHEN UPSCALING IS SET TO AI";
        }
        break;

    case SettingKey::MotionSmoothing:
        d = valueDef(key, "MOTION SMOOTHING",
                     "24 -> 60 FPS ON THE GPU - SOFT EDGES AND HALOES AROUND FAST MOTION",
                     "../icons/icon_activity.png", 3, static_cast<int>(settings->getMotionSmoothing()));
        for (int i = 0; i < 3; ++i)
            d.opt_label[i] = settings->getMotionSmoothingName(static_cast<MotionSmoothing>(i));
        /*
         * Interpolation invents frames from the picture it is handed, so it
         * also interpolates whatever the AI upscaler invented - the two stack
         * and the haloes get much worse. Say so on the row rather than leaving
         * it to be discovered.
         */
        if (settings->getUpscaler() == Upscaler::AI)
            d.detail = "AI UPSCALING IS ON - THE TWO STACK, EXPECT HEAVY ARTEFACTS";
        break;

    case SettingKey::RefreshRate:
        d = valueDef(key, "120 HZ OUTPUT", "SMOOTHER 24 FPS - THE TV GOES BLACK BRIEFLY ON EACH SWITCH",
                     "../icons/icon_gauge.png", 3, static_cast<int>(settings->getRefreshRateMode()));
        for (int i = 0; i < 3; ++i)
            d.opt_label[i] = settings->getRefreshRateModeName(static_cast<RefreshRateMode>(i));
        if (!evo_agc_runtime_supports_120hz()) {
            d.disabled = true;
            d.detail = "DISPLAY OR HDMI SINK DOES NOT SUPPORT 120 HZ";
        }
        break;

    case SettingKey::HdrOutput:
        d = valueDef(key, "HDR OUTPUT", "HDR10 FOR HDR VIDEO - THE TV GOES BLACK BRIEFLY AT START & END",
                     "../icons/icon_sun.png", 2, static_cast<int>(settings->getHdrOutputMode()));
        for (int i = 0; i < 2; ++i)
            d.opt_label[i] = settings->getHdrOutputModeName(static_cast<HdrOutputMode>(i));
        break;

    /* #117: read when a file opens, so it applies from the next file. */
    case SettingKey::OutputChannels:
        d = valueDef(key, "OUTPUT CHANNELS", "STEREO DOWNMIXES 5.1 & 7.1 TO 2.0",
                     "../icons/icon_volume.png", 2,
                     static_cast<int>(settings->getAudioOutputChannels()));
        for (int i = 0; i < 2; ++i)
            d.opt_label[i] = settings->getAudioOutputChannelsName(static_cast<AudioOutputChannels>(i));
        break;

    case SettingKey::SurroundTest:
        d = actionDef(key, "SURROUND SOUND TEST", "5.1 & 7.1 SPEAKER CHANNEL VERIFICATION",
                      "../icons/icon_speaker.png", "OPEN");
        break;

    case SettingKey::NavigationSounds:
        d = toggleDef(key, "NAVIGATION SOUNDS", "AUDIO FEEDBACK ON D-PAD & BUTTONS",
                      "../icons/icon_listener.png", settings->isSoundFeedbackEnabled());
        break;

    case SettingKey::AutoSubtitles:
        d = toggleDef(key, "AUTO SUBTITLES", "AUTOMATICALLY LOAD SUBTITLES ON PLAYBACK",
                      "../icons/icon_subtitles.png", settings->isAutoSubtitlesEnabled());
        break;

    case SettingKey::SubtitleFont: {
        static const char* kFaces[] = {"STANDARD", "ROUNDED", "BOLD", "CONDENSED"};
        int face = settings->getSubtitleFontFace();
        if (face < 0 || face >= 4) face = 0;
        d = valueDef(key, "DEFAULT FONT STYLE", "ON-SCREEN TEXT TYPEFACE",
                     "../icons/icon_type.png", 4, face);
        for (int i = 0; i < 4; ++i) d.opt_label[i] = kFaces[i];
        break;
    }

    /* #110: the second line of dialogue, drawn with the first when a
     * secondary track is chosen in the subtitle picker. */
    case SettingKey::SecondaryPosition: {
        static const char* kSecondaryPos[] = {"STACKED ABOVE", "TOP OF SCREEN"};
        int secPos = settings->getSecondarySubtitlePosition();
        if (secPos < 0 || secPos >= 2) secPos = 0;
        d = valueDef(key, "SECONDARY SUBTITLE POSITION", "WHERE THE SECOND LINE OF DIALOGUE GOES",
                     "../icons/icon_subtitles.png", 2, secPos);
        for (int i = 0; i < 2; ++i) d.opt_label[i] = kSecondaryPos[i];
        break;
    }

    case SettingKey::SecondaryColour: {
        static const char* kSecondaryColor[] = {"YELLOW", "CYAN", "WHITE"};
        int secColor = settings->getSecondarySubtitleColor();
        if (secColor < 0 || secColor >= 3) secColor = 0;
        d = valueDef(key, "SECONDARY SUBTITLE COLOUR", "KEEPS THE TWO LINES APART",
                     "../icons/icon_palette.png", 3, secColor);
        for (int i = 0; i < 3; ++i) d.opt_label[i] = kSecondaryColor[i];
        break;
    }

    case SettingKey::Theme: {
        int themes = evo_theme_count();
        if (themes > kMaxOptions) themes = kMaxOptions;
        d = valueDef(key, "THEME", "COLOR PALETTE & ACCENTS",
                     "../icons/icon_palette.png", themes, evo_theme_index());
        for (int i = 0; i < themes; ++i) {
            const char* nm = evo_theme_name(i);
            d.opt_label[i] = nm ? nm : "THEME";
        }
        break;
    }

    case SettingKey::Lightbar:
        d = toggleDef(key, "CONTROLLER LIGHTBAR", "DUALSENSE LIGHT FOLLOWS THE THEME ACCENT",
                      "../icons/icon_gamepad.png", settings->isLightbarFeedbackEnabled());
        break;

    case SettingKey::FoldersFirst:
        d = toggleDef(key, "FOLDERS FIRST", "USB FILE BROWSER SORTING",
                      "../icons/icon_folder.png", settings->isSortFoldersFirst());
        break;

    case SettingKey::KeyboardInput: {
        static const char* kKeyboards[] = {"VIRTUAL KEYBOARD", "NATIVE PS5 IME"};
        d = valueDef(key, "KEYBOARD INPUT", "TEXT ENTRY METHOD",
                     "../icons/icon_keyboard.png", 2, settings->getKeyboardType() == 1 ? 1 : 0);
        for (int i = 0; i < 2; ++i) d.opt_label[i] = kKeyboards[i];
        break;
    }

    case SettingKey::AskStream:
        d = toggleDef(key, "ASK WHICH LIVE STREAM", "PICK THE FORMAT AND QUALITY WHEN YOU OPEN A CHANNEL",
                      "../icons/icon_tv.png", settings->isAskStreamEnabled());
        break;

    case SettingKey::CompatReport:
        d = actionDef(key, "COMPATIBILITY REPORT", "WRITES A CODEC REPORT TO USB0",
                      "../icons/icon_report.png", "RUN");
        break;

    case SettingKey::DebugOverlay:
        d = toggleDef(key, "DEBUG OVERLAY", "ON-SCREEN HARDWARE PERFORMANCE METRICS",
                      "../icons/icon_activity.png", settings->isDebugOverlayEnabled());
        break;

    case SettingKey::DeveloperTools:
        d = actionDef(key, "DEVELOPER TOOLS", "SYSTEM DIAGNOSTICS & PERFORMANCE STATS",
                      "../icons/icon_developer_tools.png", "OPEN");
        break;

    /* #103: read-only - the badge is what evo_hw_probe() found at boot. */
    case SettingKey::ConsoleModel:
        d = actionDef(key, "CONSOLE", "DETECTED HARDWARE MODEL", "../icons/icon_tv.png",
                      evo_hw_is_ps5_pro() ? "PS5 PRO"
                          : evo_hw_model_known() ? "PS5" : "NOT DETECTED");
        break;

    case SettingKey::QuitEvo:
        d = actionDef(key, "QUIT EVO", "RELEASE EVERYTHING, THEN CLOSE FROM THE SWITCHER",
                      "../icons/icon_power.png", "QUIT");
        break;
    }

    /* A collapsed VALUE row shows its current choice as the badge. */
    if (d.kind == EVO_RMLUI_ROW_VALUE) {
        const int c = d.opt_current;
        d.badge = (c >= 0 && c < d.opt_count && d.opt_label[c]) ? d.opt_label[c] : "";
    }
    return d;
}

int buildSectionDefs(int section, SettingDef* d) {
    ISettingsService* settings = Application::getInstance().getSettingsService();
    if (!settings || section < 0 || section >= kSectionCount) return 0;
    const SectionSpec& spec = kSections[section];
    for (int i = 0; i < spec.count; ++i) d[i] = makeDef(spec.keys[i], settings);
    return spec.count;
}

/* One visible line: a setting, or a choice under an expanded setting. */
struct DisplaySlot { int def; int opt; };   /* opt < 0 = the setting row itself */

int buildSlots(const SettingDef* d, int n, int expanded, DisplaySlot* out) {
    int m = 0;
    for (int i = 0; i < n && m < EVO_RMLUI_SETTINGS_ROWS; ++i) {
        out[m].def = i; out[m].opt = -1; ++m;
        if (i == expanded && d[i].kind == EVO_RMLUI_ROW_VALUE && !d[i].disabled) {
            for (int o = 0; o < d[i].opt_count && m < EVO_RMLUI_SETTINGS_ROWS; ++o) {
                out[m].def = i; out[m].opt = o; ++m;
            }
        }
    }
    return m;
}

/* Fills the params for one section. cursor < 0 = the sidebar owns the cursor,
 * so nothing in the detail pane is highlighted. */
int fillSection(int section, int expanded, int cursor, evo_rmlui_settings_params_t& p) {
    SettingDef d[kMaxDefs];
    DisplaySlot slots[EVO_RMLUI_SETTINGS_ROWS];
    const int n = buildSectionDefs(section, d);
    const int m = buildSlots(d, n, expanded, slots);

    if (section >= 0 && section < kSectionCount) {
        p.title = kSections[section].title;
        p.subtitle = kSections[section].subtitle;
    }

    for (int i = 0; i < m; ++i) {
        const SettingDef& def = d[slots[i].def];
        const int opt = slots[i].opt;
        if (opt < 0) {
            p.rows[i].title = def.title;
            p.rows[i].detail = def.detail;
            p.rows[i].icon_path = def.icon;
            p.rows[i].badge = def.badge;
            p.rows[i].kind = def.kind;
            p.rows[i].toggle_on = def.toggle_on ? 1 : 0;
            p.rows[i].has_chevron = (def.kind != EVO_RMLUI_ROW_TOGGLE) ? 1 : 0;
            p.rows[i].is_disabled = def.disabled ? 1 : 0;
        } else {
            p.rows[i].title = def.opt_label[opt];
            p.rows[i].detail = "";
            p.rows[i].icon_path = "";
            p.rows[i].badge = "";
            p.rows[i].kind = EVO_RMLUI_ROW_OPTION;
            p.rows[i].toggle_on = (opt == def.opt_current) ? 1 : 0;
            p.rows[i].has_chevron = 0;
        }
        p.rows[i].is_focused = (i == cursor) ? 1 : 0;
    }

    static char s_counter[32];
    std::snprintf(s_counter, sizeof s_counter, "%d SETTING%s", n, n == 1 ? "" : "S");
    p.counter = s_counter;
    p.row_count = m;
    return m;
}

void toggleSetting(SettingKey key) {
    ISettingsService* st = Application::getInstance().getSettingsService();
    if (!st) return;
    switch (key) {
    case SettingKey::ResumePlayback:   st->setResumePlaybackEnabled(!st->isResumePlaybackEnabled()); break;
    case SettingKey::AskStream:        st->setAskStreamEnabled(!st->isAskStreamEnabled()); break;
    case SettingKey::AutoSubtitles:    st->setAutoSubtitlesEnabled(!st->isAutoSubtitlesEnabled()); break;
    case SettingKey::NavigationSounds: st->setSoundFeedbackEnabled(!st->isSoundFeedbackEnabled()); break;
    case SettingKey::Lightbar:         st->setLightbarFeedbackEnabled(!st->isLightbarFeedbackEnabled()); break;
    case SettingKey::FoldersFirst:     st->setSortFoldersFirst(!st->isSortFoldersFirst()); break;
    case SettingKey::DebugOverlay:     st->setDebugOverlayEnabled(!st->isDebugOverlayEnabled()); break;
    default: return;
    }
    evo_feedback(EVO_FB_TOGGLE);
}

void applyOption(SettingKey key, int opt) {
    ISettingsService* st = Application::getInstance().getSettingsService();
    if (!st || opt < 0) return;
    switch (key) {
    case SettingKey::DefaultAspect:  st->setDefaultViewMode(static_cast<ViewMode>(opt)); break;
    case SettingKey::VideoDecoder:   st->setVideoDecoderPreference(static_cast<DecoderPreference>(opt)); break;
    case SettingKey::Upscaling:      st->setUpscaler(static_cast<Upscaler>(opt)); break;
    case SettingKey::AiNetwork:      st->setAiNetwork(static_cast<AiNetwork>(opt)); break;
    case SettingKey::MotionSmoothing: st->setMotionSmoothing(static_cast<MotionSmoothing>(opt)); break;
    case SettingKey::HdrOutput:      st->setHdrOutputMode(static_cast<HdrOutputMode>(opt)); break;
    case SettingKey::OutputChannels: st->setAudioOutputChannels(static_cast<AudioOutputChannels>(opt)); break;
    case SettingKey::SubtitleFont:   st->setSubtitleFontFace(opt); break;
    case SettingKey::SecondaryPosition: st->setSecondarySubtitlePosition(opt); break;
    case SettingKey::SecondaryColour:   st->setSecondarySubtitleColor(opt); break;
    case SettingKey::KeyboardInput:  st->setKeyboardType(opt); break;
    case SettingKey::RefreshRate:
        st->setRefreshRateMode(static_cast<RefreshRateMode>(opt));
        /* Off or PlaybackOnly run 60 Hz here: SettingsScreen is not player mode. */
        if (evo_agc_runtime_supports_120hz())
            evo_agc_runtime_set_120hz(opt == static_cast<int>(RefreshRateMode::Always) ? 1 : 0);
        break;
    case SettingKey::Theme: {
        /*
         * evo_theme_set() returns the index it applied, not a status - see the
         * contract in evo_theme.h. Testing it for 0 meant the body only ran for
         * the first theme in the list, so every other one changed the C-side
         * active index and stopped there: setThemeName() never ran, so
         * syncThemeToRmlUi() never pushed the colours into RmlUi and the choice
         * was never persisted. Picking any theme but the first one appeared to
         * do nothing at all.
         *
         * Use the returned index rather than opt, because the setter wraps.
         */
        const int applied = evo_theme_set(opt);
        if (const char* nm = evo_theme_name(applied))
            st->setThemeName(nm);
        break;
    }
    default: return;
    }
    evo_feedback(EVO_FB_TOGGLE);
}

bool runAction(const SettingDef& def) {
    if (def.disabled) {
        evo_feedback(EVO_FB_BOUNDARY);
        return false;
    }

    switch (def.key) {
    case SettingKey::SurroundTest:
        if (auto sm = Application::getInstance().getScreenManager()) {
            evo_feedback(EVO_FB_OPEN);
            sm->navigateTo(ScreenId::SurroundTest);
            return true;
        }
        return false;
    case SettingKey::DeveloperTools:
        if (auto sm = Application::getInstance().getScreenManager()) {
            evo_feedback(EVO_FB_OPEN);
            sm->navigateTo(ScreenId::DeveloperTools);
            return true;
        }
        return false;
    /*
     * Soft close, not exit.
     *
     * Closing from the PS button kills the process outright, so the frame loop
     * never returns and Application::shutdown() never runs - meaning the GPU is
     * never drained and the scanout registration and direct-memory pool are
     * left for the kernel to reclaim underneath a GPU that may still be
     * mid-submit. requestExit() just clears m_running, so the loop finishes the
     * frame it is on and exits through the ordinary shutdown path like any
     * other return from run().
     *
     * This used to call requestExit(), which left through shutdown() and
     * returned from main(). The teardown itself worked - the breadcrumbs ran
     * all the way to "shutdown: complete" - but the libc exit path after it
     * crashed every time, and a PS5 app is not really expected to terminate
     * itself anyway: no retail game ships a quit menu. So it now releases
     * everything and parks instead, and the user closes it from the switcher.
     * See Application::requestSoftClose().
     */
    case SettingKey::QuitEvo:
        evo_feedback(EVO_FB_OPEN);
        evo_boot_log("settings: QUIT EVO selected - soft close");
        evo_boot_log_flush();
        Application::getInstance().requestSoftClose();
        return true;
    case SettingKey::CompatReport: {
        FILE* fp = std::fopen("/mnt/usb0/evo_compatibility_report.txt", "w");
        if (!fp) fp = std::fopen("evo_compatibility_report.txt", "w");
        if (fp) {
            std::fprintf(fp, "=== EVO Player Compatibility Report ===\n");
            std::fprintf(fp, "Version: %s\n", EVO_PLAYER_VERSION);
            std::fprintf(fp, "Hardware: %s\n", evo_hw_model_name());
            std::fprintf(fp, "Hardware Acceleration: sceAgc Bare-Metal Driver\n");
            std::fprintf(fp, "Report generated successfully.\n");
            std::fclose(fp);
            toast("REPORT", "Report saved to USB0");
            evo_feedback(EVO_FB_CONFIRM);
        } else {
            toast("REPORT", "Save failed");
            evo_feedback(EVO_FB_BOUNDARY);
        }
        return true;
    }
    default:
        return false;
    }
}

void saveSettings() {
    if (auto settings = Application::getInstance().getSettingsService()) {
        settings->saveSettings();
    }
}

} // namespace

// =============================================================================
// SettingsScreen (Top-level Category Selector)
// =============================================================================

SettingsScreen::SettingsScreen()
    : StatefulScreen("SettingsScreen") {
}

void SettingsScreen::onEnter() {
    StatefulScreen::onEnter();
    /* Deliberately keeps m_selectedIndex: Circle out of a section returns the
     * cursor to that section, the way a sidebar should behave. */
}

void SettingsScreen::onExit() {
    StatefulScreen::onExit();
    saveSettings();
}

void SettingsScreen::navigate(int delta) {
    constexpr int totalRows = kSectionCount;
    m_selectedIndex += delta;
    if (m_selectedIndex < 0) {
        m_selectedIndex = 0;
        evo_feedback(EVO_FB_BOUNDARY);
    } else if (m_selectedIndex >= totalRows) {
        m_selectedIndex = totalRows - 1;
        evo_feedback(EVO_FB_BOUNDARY);
    } else {
        evo_feedback(EVO_FB_MOVE);
    }
}

void SettingsScreen::activateSelection() {
    auto screenMgr = Application::getInstance().getScreenManager();
    if (!screenMgr) return;

    evo_feedback(EVO_FB_CONFIRM);
    screenMgr->navigateTo(kSections[m_selectedIndex].screen);
}

bool SettingsScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    if (pressed & PadButtons::Up) {
        navigate(-1);
        return true;
    }
    if (pressed & PadButtons::Down) {
        navigate(1);
        return true;
    }
    if (pressed & PadButtons::Left) {
        if (auto screenMgr = Application::getInstance().getScreenManager()) {
            screenMgr->setRailFocused(true);
            return true;
        }
    }
    if ((pressed & PadButtons::Right) || (pressed & PadButtons::Cross)) {
        /* Right steps from the sidebar into the detail pane, same as Cross. */
        activateSelection();
        return true;
    }
    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto screenMgr = Application::getInstance().getScreenManager()) {
            screenMgr->navigateTo(ScreenId::MainMenu);
        }
        return true;
    }

    return false;
}

void SettingsScreen::update(double deltaMs) {
    (void)deltaMs;
}

void SettingsScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_settings_params_t params;
    std::memset(&params, 0, sizeof(params));

    bool railFocused = false;
    if (auto screenMgr = Application::getInstance().getScreenManager()) {
        railFocused = screenMgr->isRailFocused();
    }

    params.rail_active_idx = 5;
    params.rail_focused = railFocused ? 1 : 0;
    params.section_active = m_selectedIndex;
    params.sidebar_focused = railFocused ? 0 : 1;

    /* The sidebar is the page here, so the detail pane previews the highlighted
     * section's real options rather than repeating the section list. Nothing is
     * expanded and no row is focused - the cursor lives in the sidebar. */
    fillSection(m_selectedIndex, -1, -1, params);

    evo_rmlui_update_settings(&params);
    evo_rmlui_render_settings(framebuffer, width, height);
}

// =============================================================================
// SettingsSectionScreen (one per section in kSections)
// =============================================================================

SettingsSectionScreen::SettingsSectionScreen(ScreenId id)
    : StatefulScreen(kSections[sectionForScreen(id)].name),
      m_id(id),
      m_section(sectionForScreen(id)) {
}

void SettingsSectionScreen::onExit() {
    StatefulScreen::onExit();
    saveSettings();
}

void SettingsSectionScreen::onEnter() {
    StatefulScreen::onEnter();
    m_selectedIndex = 0;
    m_expandedIndex = -1;
}

void SettingsSectionScreen::navigate(int delta) {
    SettingDef d[kMaxDefs];
    DisplaySlot slots[EVO_RMLUI_SETTINGS_ROWS];
    const int total = buildSlots(d, buildSectionDefs(m_section, d), m_expandedIndex, slots);
    if (total <= 0) return;
    m_selectedIndex += delta;
    if (m_selectedIndex < 0) {
        m_selectedIndex = 0;
        evo_feedback(EVO_FB_BOUNDARY);
    } else if (m_selectedIndex >= total) {
        m_selectedIndex = total - 1;
        evo_feedback(EVO_FB_BOUNDARY);
    } else {
        evo_feedback(EVO_FB_MOVE);
    }
}

void SettingsSectionScreen::activateSelection() {
    SettingDef d[kMaxDefs];
    DisplaySlot slots[EVO_RMLUI_SETTINGS_ROWS];
    const int m = buildSlots(d, buildSectionDefs(m_section, d), m_expandedIndex, slots);
    if (m_selectedIndex < 0 || m_selectedIndex >= m) return;
    const int def = slots[m_selectedIndex].def;
    const int opt = slots[m_selectedIndex].opt;

    if (opt >= 0) {
        /* A choice under an expanded setting: apply it and collapse, leaving
         * the cursor on the setting itself. */
        applyOption(d[def].key, opt);
        m_expandedIndex = -1;
        m_selectedIndex = def;
        return;
    }

    /* A disabled row does nothing but the boundary buzz (runAction). */
    switch (d[def].disabled ? EVO_RMLUI_ROW_ACTION : d[def].kind) {
    case EVO_RMLUI_ROW_TOGGLE:
        toggleSetting(d[def].key);
        break;
    case EVO_RMLUI_ROW_VALUE:
        /* Expand in place so every choice is visible, collapse if already open. */
        m_expandedIndex = (m_expandedIndex == def) ? -1 : def;
        m_selectedIndex = def;
        evo_feedback(EVO_FB_CONFIRM);
        break;
    default:
        runAction(d[def]);
        break;
    }
}

bool SettingsSectionScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    /* The D-pad only ever moves the cursor. Values change on Cross alone -
     * Left/Right used to edit the highlighted setting, which meant walking the
     * menu silently rewrote your settings. */
    if (pressed & PadButtons::Up) {
        navigate(-1);
        return true;
    }
    if (pressed & PadButtons::Down) {
        navigate(1);
        return true;
    }
    if (pressed & PadButtons::Left) {
        if (m_expandedIndex >= 0) {
            m_expandedIndex = -1;
            evo_feedback(EVO_FB_CANCEL);
            return true;
        }
        if (auto screenMgr = Application::getInstance().getScreenManager()) {
            evo_feedback(EVO_FB_CANCEL);
            screenMgr->navigateTo(ScreenId::Settings);
            return true;
        }
        return true;
    }
    if (pressed & PadButtons::Right) {
        SettingDef d[kMaxDefs];
        DisplaySlot slots[EVO_RMLUI_SETTINGS_ROWS];
        const int m = buildSlots(d, buildSectionDefs(m_section, d), m_expandedIndex, slots);
        if (m_selectedIndex >= 0 && m_selectedIndex < m && slots[m_selectedIndex].opt < 0) {
            const int def = slots[m_selectedIndex].def;
            if (d[def].kind == EVO_RMLUI_ROW_VALUE && !d[def].disabled && m_expandedIndex != def) {
                m_expandedIndex = def;
                evo_feedback(EVO_FB_CONFIRM);
            }
        }
        return true;
    }
    if (pressed & PadButtons::Cross) {
        activateSelection();
        return true;
    }
    if (pressed & PadButtons::Circle) {
        if (m_expandedIndex >= 0) {
            m_expandedIndex = -1;
            evo_feedback(EVO_FB_CANCEL);
            return true;
        }
        evo_feedback(EVO_FB_CANCEL);
        if (auto screenMgr = Application::getInstance().getScreenManager()) {
            screenMgr->navigateTo(ScreenId::Settings);
        }
        return true;
    }

    return false;
}

void SettingsSectionScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_settings_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.rail_active_idx = 5;
    params.rail_focused = 0;
    params.section_active = m_section;
    params.sidebar_focused = 0;

    fillSection(m_section, m_expandedIndex, m_selectedIndex, params);

    evo_rmlui_update_settings(&params);
    evo_rmlui_render_settings(framebuffer, width, height);
}

void SettingsSectionScreen::update(double deltaMs) {
    (void)deltaMs;
}

} // namespace evo
