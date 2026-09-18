#include "evo/screens/SettingsScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_toast.h"
#include "evo_theme.h"
#include "evo_boot_log.h"

#include <cstdio>
#include <cstring>

#ifndef EVO_PLAYER_VERSION
#define EVO_PLAYER_VERSION "0.7.6"
#endif

namespace evo {

namespace {

constexpr int kSectionCount = 4;
constexpr int kMaxDefs      = 6;
constexpr int kMaxOptions   = EVO_THEME_MAX;   /* themes are the longest list */

enum SettingAction { ACT_NONE = 0, ACT_SURROUND, ACT_COMPAT_REPORT, ACT_DEVTOOLS, ACT_QUIT };

/*
 * One description of a setting, independent of how it is drawn.
 *
 * A VALUE setting carries every choice it has, not just the current one, so the
 * row can expand in place and show the user what actually exists - picking a
 * theme used to mean cycling a badge blind, with no way to see the list.
 */
struct SettingDef {
    const char* title;
    const char* detail;
    const char* icon;
    int         kind;
    bool        toggle_on;
    const char* badge;
    int         action;
    int         opt_count;
    int         opt_current;
    const char* opt_label[kMaxOptions];
};

struct SectionHeader { const char* title; const char* subtitle; };
const SectionHeader kHeaders[kSectionCount] = {
    {"PLAYBACK & VIDEO",     "ASPECT RATIO, RESUME & DECODER"},
    {"SUBTITLES",            "PREFERENCES & APPEARANCE"},
    {"INTERFACE & CONTROLS", "THEMES, SOUNDS & CONTROLS"},
    {"SYSTEM & DIAGNOSTICS", "DIAGNOSTICS & SYSTEM MANAGEMENT"},
};

int buildSectionDefs(int section, SettingDef* d) {
    auto settings = Application::getInstance().getSettingsService();
    if (!settings) return 0;
    for (int i = 0; i < kMaxDefs; ++i) d[i] = SettingDef{};

    int n = 0;
    switch (section) {
    case 0:
        d[0] = {"DEFAULT ASPECT RATIO", "HOW VIDEO FILLS THE SCREEN",
                "../icons/icon_aspect.png", EVO_RMLUI_ROW_VALUE, false, "", ACT_NONE, 3,
                static_cast<int>(settings->getDefaultViewMode()), {}};
        for (int i = 0; i < 3; ++i)
            d[0].opt_label[i] = settings->getViewModeName(static_cast<ViewMode>(i));

        d[1] = {"RESUME PLAYBACK", "REMEMBER PLAYBACK POSITION",
                "../icons/icon_resume.png", EVO_RMLUI_ROW_TOGGLE,
                settings->isResumePlaybackEnabled(), "", ACT_NONE, 0, 0, {}};

        d[2] = {"SURROUND SOUND TEST", "5.1 & 7.1 SPEAKER CHANNEL VERIFICATION",
                "../icons/icon_resume.png", EVO_RMLUI_ROW_ACTION, false, "OPEN",
                ACT_SURROUND, 0, 0, {}};

        d[3] = {"VIDEO DECODER", "WHICH BACKEND DECODES VIDEO",
                "../icons/icon_developer_tools.png", EVO_RMLUI_ROW_VALUE, false, "",
                ACT_NONE, 3, static_cast<int>(settings->getVideoDecoderPreference()), {}};
        for (int i = 0; i < 3; ++i)
            d[3].opt_label[i] = settings->getDecoderPreferenceBadge(
                                    static_cast<DecoderPreference>(i));
        n = 4;
        break;

    case 1: {
        static const char* kFaces[] = {"STANDARD", "ROUNDED", "BOLD", "CONDENSED"};
        d[0] = {"AUTO SUBTITLES", "AUTOMATICALLY LOAD SUBTITLES ON PLAYBACK",
                "../icons/icon_subtitles.png", EVO_RMLUI_ROW_TOGGLE,
                settings->isAutoSubtitlesEnabled(), "", ACT_NONE, 0, 0, {}};

        int face = settings->getSubtitleFontFace();
        if (face < 0 || face >= 4) face = 0;
        d[1] = {"DEFAULT FONT STYLE", "ON-SCREEN TEXT TYPEFACE",
                "../icons/icon_subtitles.png", EVO_RMLUI_ROW_VALUE, false, "",
                ACT_NONE, 4, face, {}};
        for (int i = 0; i < 4; ++i) d[1].opt_label[i] = kFaces[i];
        n = 2;
        break;
    }

    case 2: {
        int themes = evo_theme_count();
        if (themes > kMaxOptions) themes = kMaxOptions;
        d[0] = {"THEME", "COLOR PALETTE & ACCENTS",
                "../icons/icon_palette.png", EVO_RMLUI_ROW_VALUE, false, "",
                ACT_NONE, themes, evo_theme_index(), {}};
        for (int i = 0; i < themes; ++i) {
            const char* nm = evo_theme_name(i);
            d[0].opt_label[i] = nm ? nm : "THEME";
        }

        d[1] = {"NAVIGATION SOUNDS", "AUDIO FEEDBACK ON D-PAD & BUTTONS",
                "../icons/icon_subtitles.png", EVO_RMLUI_ROW_TOGGLE,
                settings->isSoundFeedbackEnabled(), "", ACT_NONE, 0, 0, {}};

        d[2] = {"CONTROLLER LIGHTBAR", "DUALSENSE LIGHT FOLLOWS THE THEME ACCENT",
                "../icons/icon_palette.png", EVO_RMLUI_ROW_TOGGLE,
                settings->isLightbarFeedbackEnabled(), "", ACT_NONE, 0, 0, {}};

        d[3] = {"FOLDERS FIRST", "USB FILE BROWSER SORTING",
                "../icons/icon_folder.png", EVO_RMLUI_ROW_TOGGLE,
                settings->isSortFoldersFirst(), "", ACT_NONE, 0, 0, {}};

        static const char* kKeyboards[] = {"VIRTUAL KEYBOARD", "NATIVE PS5 IME"};
        int kb = (settings->getKeyboardType() == 1) ? 1 : 0;
        d[4] = {"KEYBOARD INPUT", "TEXT ENTRY METHOD",
                "../icons/icon_settings.png", EVO_RMLUI_ROW_VALUE, false, "",
                ACT_NONE, 2, kb, {}};
        for (int i = 0; i < 2; ++i) d[4].opt_label[i] = kKeyboards[i];
        n = 5;
        break;
    }

    case 3:
        d[0] = {"COMPATIBILITY REPORT", "WRITES A CODEC REPORT TO USB0",
                "../icons/icon_developer_tools.png", EVO_RMLUI_ROW_ACTION, false, "RUN",
                ACT_COMPAT_REPORT, 0, 0, {}};

        d[1] = {"DEBUG OVERLAY", "ON-SCREEN HARDWARE PERFORMANCE METRICS",
                "../icons/icon_aspect.png", EVO_RMLUI_ROW_TOGGLE,
                settings->isDebugOverlayEnabled(), "", ACT_NONE, 0, 0, {}};

        d[2] = {"DEVELOPER TOOLS", "SYSTEM DIAGNOSTICS & PERFORMANCE STATS",
                "../icons/icon_settings.png", EVO_RMLUI_ROW_ACTION, false, "OPEN",
                ACT_DEVTOOLS, 0, 0, {}};

        d[3] = {"QUIT EVO", "RELEASE EVERYTHING, THEN CLOSE FROM THE SWITCHER",
                "../icons/icon_settings.png", EVO_RMLUI_ROW_ACTION, false, "QUIT",
                ACT_QUIT, 0, 0, {}};
        n = 4;
        break;

    default:
        break;
    }

    /* A collapsed VALUE row shows its current choice as the badge. */
    for (int i = 0; i < n; ++i) {
        if (d[i].kind == EVO_RMLUI_ROW_VALUE) {
            const int c = d[i].opt_current;
            d[i].badge = (c >= 0 && c < d[i].opt_count && d[i].opt_label[c])
                           ? d[i].opt_label[c] : "";
        }
    }
    return n;
}

/* One visible line: a setting, or a choice under an expanded setting. */
struct DisplaySlot { int def; int opt; };   /* opt < 0 = the setting row itself */

int buildSlots(const SettingDef* d, int n, int expanded, DisplaySlot* out) {
    int m = 0;
    for (int i = 0; i < n && m < EVO_RMLUI_SETTINGS_ROWS; ++i) {
        out[m].def = i; out[m].opt = -1; ++m;
        if (i == expanded && d[i].kind == EVO_RMLUI_ROW_VALUE) {
            for (int o = 0; o < d[i].opt_count && m < EVO_RMLUI_SETTINGS_ROWS; ++o) {
                out[m].def = i; out[m].opt = o; ++m;
            }
        }
    }
    return m;
}

int sectionRowCount(int section, int expanded) {
    SettingDef d[kMaxDefs];
    DisplaySlot slots[EVO_RMLUI_SETTINGS_ROWS];
    const int n = buildSectionDefs(section, d);
    return buildSlots(d, n, expanded, slots);
}

bool slotAt(int section, int expanded, int cursor, int& defOut, int& optOut) {
    SettingDef d[kMaxDefs];
    DisplaySlot slots[EVO_RMLUI_SETTINGS_ROWS];
    const int n = buildSectionDefs(section, d);
    const int m = buildSlots(d, n, expanded, slots);
    if (cursor < 0 || cursor >= m) return false;
    defOut = slots[cursor].def;
    optOut = slots[cursor].opt;
    return true;
}

int settingKind(int section, int def) {
    SettingDef d[kMaxDefs];
    const int n = buildSectionDefs(section, d);
    return (def >= 0 && def < n) ? d[def].kind : EVO_RMLUI_ROW_ACTION;
}

/* Fills the params for one section. cursor < 0 = the sidebar owns the cursor,
 * so nothing in the detail pane is highlighted. */
int fillSection(int section, int expanded, int cursor, evo_rmlui_settings_params_t& p) {
    SettingDef d[kMaxDefs];
    DisplaySlot slots[EVO_RMLUI_SETTINGS_ROWS];
    const int n = buildSectionDefs(section, d);
    const int m = buildSlots(d, n, expanded, slots);

    if (section >= 0 && section < kSectionCount) {
        p.title = kHeaders[section].title;
        p.subtitle = kHeaders[section].subtitle;
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

void toggleSetting(int section, int def) {
    auto st = Application::getInstance().getSettingsService();
    if (!st) return;
    if (section == 0 && def == 1) st->setResumePlaybackEnabled(!st->isResumePlaybackEnabled());
    else if (section == 1 && def == 0) st->setAutoSubtitlesEnabled(!st->isAutoSubtitlesEnabled());
    else if (section == 2 && def == 1) st->setSoundFeedbackEnabled(!st->isSoundFeedbackEnabled());
    else if (section == 2 && def == 2) st->setLightbarFeedbackEnabled(!st->isLightbarFeedbackEnabled());
    else if (section == 2 && def == 3) st->setSortFoldersFirst(!st->isSortFoldersFirst());
    else if (section == 3 && def == 1) st->setDebugOverlayEnabled(!st->isDebugOverlayEnabled());
    else return;
    evo_feedback(EVO_FB_TOGGLE);
}

void applyOption(int section, int def, int opt) {
    auto st = Application::getInstance().getSettingsService();
    if (!st || opt < 0) return;
    if (section == 0 && def == 0) st->setDefaultViewMode(static_cast<ViewMode>(opt));
    else if (section == 0 && def == 3) st->setVideoDecoderPreference(static_cast<DecoderPreference>(opt));
    else if (section == 1 && def == 1) st->setSubtitleFontFace(opt);
    else if (section == 2 && def == 0) {
        if (evo_theme_set(opt) == 0) {
            const char* nm = evo_theme_name(opt);
            if (nm) st->setThemeName(nm);
        }
    }
    else if (section == 2 && def == 4) st->setKeyboardType(opt);
    else return;
    evo_feedback(EVO_FB_TOGGLE);
}

bool runAction(int section, int def) {
    SettingDef d[kMaxDefs];
    const int n = buildSectionDefs(section, d);
    if (def < 0 || def >= n) return false;

    switch (d[def].action) {
    case ACT_SURROUND:
        if (auto sm = Application::getInstance().getScreenManager()) {
            evo_feedback(EVO_FB_OPEN);
            sm->navigateTo(ScreenId::SurroundTest);
            return true;
        }
        return false;
    case ACT_DEVTOOLS:
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
    case ACT_QUIT:
        evo_feedback(EVO_FB_OPEN);
        evo_boot_log("settings: QUIT EVO selected - soft close");
        evo_boot_log_flush();
        Application::getInstance().requestSoftClose();
        return true;
    case ACT_COMPAT_REPORT: {
        FILE* fp = std::fopen("/mnt/usb0/evo_compatibility_report.txt", "w");
        if (!fp) fp = std::fopen("evo_compatibility_report.txt", "w");
        if (fp) {
            std::fprintf(fp, "=== EVO Player Compatibility Report ===\n");
            std::fprintf(fp, "Version: %s\n", EVO_PLAYER_VERSION);
            std::fprintf(fp, "Hardware: PlayStation 5\n");
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

ScreenId sectionScreenId(int section) {
    switch (section) {
        case 0: return ScreenId::SettingsPlayback;
        case 1: return ScreenId::SettingsSubtitles;
        case 2: return ScreenId::SettingsInterface;
        default: return ScreenId::SettingsSystem;
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
    if (auto settings = Application::getInstance().getSettingsService()) {
        settings->saveSettings();
    }
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
    screenMgr->navigateTo(sectionScreenId(m_selectedIndex));
}

void SettingsScreen::adjustValue(int delta) {
    (void)delta;
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
// SettingsPlaybackScreen
// =============================================================================

SettingsPlaybackScreen::SettingsPlaybackScreen()
    : StatefulScreen("SettingsPlaybackScreen") {
}

void SettingsPlaybackScreen::onExit() {
    StatefulScreen::onExit();
    if (auto settings = Application::getInstance().getSettingsService()) {
        settings->saveSettings();
    }
}

void SettingsPlaybackScreen::onEnter() {
    StatefulScreen::onEnter();
    m_selectedIndex = 0;
    m_expandedIndex = -1;
}

void SettingsPlaybackScreen::navigate(int delta) {
    const int total = sectionRowCount(0, m_expandedIndex);
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

void SettingsPlaybackScreen::activateSelection() {
    int def = -1, opt = -1;
    if (!slotAt(0, m_expandedIndex, m_selectedIndex, def, opt)) return;

    if (opt >= 0) {
        /* A choice under an expanded setting: apply it and collapse, leaving
         * the cursor on the setting itself. */
        applyOption(0, def, opt);
        m_expandedIndex = -1;
        m_selectedIndex = def;
        return;
    }

    switch (settingKind(0, def)) {
    case EVO_RMLUI_ROW_TOGGLE:
        toggleSetting(0, def);
        break;
    case EVO_RMLUI_ROW_VALUE:
        /* Expand in place so every choice is visible, collapse if already open. */
        m_expandedIndex = (m_expandedIndex == def) ? -1 : def;
        m_selectedIndex = def;
        evo_feedback(EVO_FB_CONFIRM);
        break;
    default:
        runAction(0, def);
        break;
    }
}

bool SettingsPlaybackScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
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
        int def = -1, opt = -1;
        if (slotAt(0, m_expandedIndex, m_selectedIndex, def, opt) && opt < 0 &&
            settingKind(0, def) == EVO_RMLUI_ROW_VALUE && m_expandedIndex != def) {
            m_expandedIndex = def;
            evo_feedback(EVO_FB_CONFIRM);
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

void SettingsPlaybackScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_settings_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.rail_active_idx = 5;
    params.rail_focused = 0;
    params.section_active = 0;
    params.sidebar_focused = 0;

    fillSection(0, m_expandedIndex, m_selectedIndex, params);

    evo_rmlui_update_settings(&params);
    evo_rmlui_render_settings(framebuffer, width, height);
}

void SettingsPlaybackScreen::update(double deltaMs) {
    (void)deltaMs;
}

// =============================================================================
// SettingsSubtitlesScreen
// =============================================================================

SettingsSubtitlesScreen::SettingsSubtitlesScreen()
    : StatefulScreen("SettingsSubtitlesScreen") {
}

void SettingsSubtitlesScreen::onExit() {
    StatefulScreen::onExit();
    if (auto settings = Application::getInstance().getSettingsService()) {
        settings->saveSettings();
    }
}

void SettingsSubtitlesScreen::onEnter() {
    StatefulScreen::onEnter();
    m_selectedIndex = 0;
    m_expandedIndex = -1;
}

void SettingsSubtitlesScreen::navigate(int delta) {
    const int total = sectionRowCount(1, m_expandedIndex);
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

void SettingsSubtitlesScreen::activateSelection() {
    int def = -1, opt = -1;
    if (!slotAt(1, m_expandedIndex, m_selectedIndex, def, opt)) return;

    if (opt >= 0) {
        /* A choice under an expanded setting: apply it and collapse, leaving
         * the cursor on the setting itself. */
        applyOption(1, def, opt);
        m_expandedIndex = -1;
        m_selectedIndex = def;
        return;
    }

    switch (settingKind(1, def)) {
    case EVO_RMLUI_ROW_TOGGLE:
        toggleSetting(1, def);
        break;
    case EVO_RMLUI_ROW_VALUE:
        /* Expand in place so every choice is visible, collapse if already open. */
        m_expandedIndex = (m_expandedIndex == def) ? -1 : def;
        m_selectedIndex = def;
        evo_feedback(EVO_FB_CONFIRM);
        break;
    default:
        runAction(1, def);
        break;
    }
}

bool SettingsSubtitlesScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
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
        int def = -1, opt = -1;
        if (slotAt(1, m_expandedIndex, m_selectedIndex, def, opt) && opt < 0 &&
            settingKind(1, def) == EVO_RMLUI_ROW_VALUE && m_expandedIndex != def) {
            m_expandedIndex = def;
            evo_feedback(EVO_FB_CONFIRM);
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

void SettingsSubtitlesScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_settings_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.rail_active_idx = 5;
    params.rail_focused = 0;
    params.section_active = 1;
    params.sidebar_focused = 0;

    fillSection(1, m_expandedIndex, m_selectedIndex, params);

    evo_rmlui_update_settings(&params);
    evo_rmlui_render_settings(framebuffer, width, height);
}

void SettingsSubtitlesScreen::update(double deltaMs) {
    (void)deltaMs;
}

// =============================================================================
// SettingsInterfaceScreen
// =============================================================================

SettingsInterfaceScreen::SettingsInterfaceScreen()
    : StatefulScreen("SettingsInterfaceScreen") {
}

void SettingsInterfaceScreen::onExit() {
    StatefulScreen::onExit();
    if (auto settings = Application::getInstance().getSettingsService()) {
        settings->saveSettings();
    }
}

void SettingsInterfaceScreen::onEnter() {
    StatefulScreen::onEnter();
    m_selectedIndex = 0;
    m_expandedIndex = -1;
}

void SettingsInterfaceScreen::navigate(int delta) {
    const int total = sectionRowCount(2, m_expandedIndex);
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

void SettingsInterfaceScreen::activateSelection() {
    int def = -1, opt = -1;
    if (!slotAt(2, m_expandedIndex, m_selectedIndex, def, opt)) return;

    if (opt >= 0) {
        /* A choice under an expanded setting: apply it and collapse, leaving
         * the cursor on the setting itself. */
        applyOption(2, def, opt);
        m_expandedIndex = -1;
        m_selectedIndex = def;
        return;
    }

    switch (settingKind(2, def)) {
    case EVO_RMLUI_ROW_TOGGLE:
        toggleSetting(2, def);
        break;
    case EVO_RMLUI_ROW_VALUE:
        /* Expand in place so every choice is visible, collapse if already open. */
        m_expandedIndex = (m_expandedIndex == def) ? -1 : def;
        m_selectedIndex = def;
        evo_feedback(EVO_FB_CONFIRM);
        break;
    default:
        runAction(2, def);
        break;
    }
}

bool SettingsInterfaceScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
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
        int def = -1, opt = -1;
        if (slotAt(2, m_expandedIndex, m_selectedIndex, def, opt) && opt < 0 &&
            settingKind(2, def) == EVO_RMLUI_ROW_VALUE && m_expandedIndex != def) {
            m_expandedIndex = def;
            evo_feedback(EVO_FB_CONFIRM);
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

void SettingsInterfaceScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_settings_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.rail_active_idx = 5;
    params.rail_focused = 0;
    params.section_active = 2;
    params.sidebar_focused = 0;

    fillSection(2, m_expandedIndex, m_selectedIndex, params);

    evo_rmlui_update_settings(&params);
    evo_rmlui_render_settings(framebuffer, width, height);
}

void SettingsInterfaceScreen::update(double deltaMs) {
    (void)deltaMs;
}

// =============================================================================
// SettingsSystemScreen
// =============================================================================

SettingsSystemScreen::SettingsSystemScreen()
    : StatefulScreen("SettingsSystemScreen") {
}

void SettingsSystemScreen::onExit() {
    StatefulScreen::onExit();
    if (auto settings = Application::getInstance().getSettingsService()) {
        settings->saveSettings();
    }
}

void SettingsSystemScreen::onEnter() {
    StatefulScreen::onEnter();
    m_selectedIndex = 0;
    m_expandedIndex = -1;
}

void SettingsSystemScreen::navigate(int delta) {
    const int total = sectionRowCount(3, m_expandedIndex);
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

void SettingsSystemScreen::activateSelection() {
    int def = -1, opt = -1;
    if (!slotAt(3, m_expandedIndex, m_selectedIndex, def, opt)) return;

    if (opt >= 0) {
        /* A choice under an expanded setting: apply it and collapse, leaving
         * the cursor on the setting itself. */
        applyOption(3, def, opt);
        m_expandedIndex = -1;
        m_selectedIndex = def;
        return;
    }

    switch (settingKind(3, def)) {
    case EVO_RMLUI_ROW_TOGGLE:
        toggleSetting(3, def);
        break;
    case EVO_RMLUI_ROW_VALUE:
        /* Expand in place so every choice is visible, collapse if already open. */
        m_expandedIndex = (m_expandedIndex == def) ? -1 : def;
        m_selectedIndex = def;
        evo_feedback(EVO_FB_CONFIRM);
        break;
    default:
        runAction(3, def);
        break;
    }
}

bool SettingsSystemScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
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
        int def = -1, opt = -1;
        if (slotAt(3, m_expandedIndex, m_selectedIndex, def, opt) && opt < 0 &&
            settingKind(3, def) == EVO_RMLUI_ROW_VALUE && m_expandedIndex != def) {
            m_expandedIndex = def;
            evo_feedback(EVO_FB_CONFIRM);
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

void SettingsSystemScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_settings_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.rail_active_idx = 5;
    params.rail_focused = 0;
    params.section_active = 3;
    params.sidebar_focused = 0;

    fillSection(3, m_expandedIndex, m_selectedIndex, params);

    evo_rmlui_update_settings(&params);
    evo_rmlui_render_settings(framebuffer, width, height);
}

void SettingsSystemScreen::update(double deltaMs) {
    (void)deltaMs;
}

} // namespace evo
