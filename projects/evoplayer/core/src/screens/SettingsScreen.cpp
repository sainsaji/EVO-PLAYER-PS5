#include "evo/screens/SettingsScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_toast.h"
#include "evo_theme.h"

#include <cstdio>
#include <cstring>

#ifndef EVO_PLAYER_VERSION
#define EVO_PLAYER_VERSION "0.7.6"
#endif

namespace evo {

// =============================================================================
// SettingsScreen (Top-level Category Selector)
// =============================================================================

SettingsScreen::SettingsScreen()
    : StatefulScreen("SettingsScreen") {
}

void SettingsScreen::onEnter() {
    StatefulScreen::onEnter();
    m_selectedIndex = 0;
}

void SettingsScreen::onExit() {
    StatefulScreen::onExit();
    if (auto settings = Application::getInstance().getSettingsService()) {
        settings->saveSettings();
    }
}

void SettingsScreen::navigate(int delta) {
    constexpr int totalRows = 4;
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
    switch (m_selectedIndex) {
        case 0: screenMgr->navigateTo(ScreenId::SettingsPlayback); break;
        case 1: screenMgr->navigateTo(ScreenId::SettingsSubtitles); break;
        case 2: screenMgr->navigateTo(ScreenId::SettingsInterface); break;
        case 3: screenMgr->navigateTo(ScreenId::SettingsSystem); break;
        default: break;
    }
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
    if (pressed & PadButtons::Cross) {
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

    params.title = "SETTINGS";
    params.subtitle = "SYSTEM CONFIGURATION";
    params.counter = "4 CATEGORIES";
    params.rail_active_idx = 5;
    params.rail_focused = railFocused ? 1 : 0;
    params.row_count = 4;

    const char* titles[] = {"Playback", "Subtitles", "Interface", "System"};
    const char* details[] = {
        "Hardware decoder, audio output format, resume positions",
        "Preferred language, embedded and external SRT subtitles",
        "Themes, lightbar feedback, UI sound effects",
        "Developer tools, performance overlay, storage inspection"
    };
    const char* icons[] = {
        "../icons/icon_settings.png",
        "../icons/icon_subtitles.png",
        "../icons/icon_palette.png",
        "../icons/icon_developer_tools.png"
    };

    for (int i = 0; i < 4; ++i) {
        params.rows[i].title = titles[i];
        params.rows[i].detail = details[i];
        params.rows[i].icon_path = icons[i];
        params.rows[i].has_chevron = 1;
        params.rows[i].is_focused = (m_selectedIndex == i);
    }

    evo_rmlui_update_settings(&params);
    evo_rmlui_render_settings(framebuffer, width, height);
}

// =============================================================================
// SettingsPlaybackScreen
// =============================================================================

SettingsPlaybackScreen::SettingsPlaybackScreen()
    : StatefulScreen("SettingsPlaybackScreen") {
}

void SettingsPlaybackScreen::onEnter() {
    StatefulScreen::onEnter();
    m_selectedIndex = 0;
}

void SettingsPlaybackScreen::onExit() {
    StatefulScreen::onExit();
    if (auto settings = Application::getInstance().getSettingsService()) {
        settings->saveSettings();
    }
}

void SettingsPlaybackScreen::navigate(int delta) {
    constexpr int totalRows = 5;
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

void SettingsPlaybackScreen::adjustValue(int delta) {
    auto settings = Application::getInstance().getSettingsService();
    if (!settings) return;

    switch (m_selectedIndex) {
        case 0: { // Playback Profile
            int p = static_cast<int>(settings->getProfile());
            p = (p + delta + 4) % 4;
            settings->setProfile(static_cast<PlaybackProfile>(p));
            evo_feedback(EVO_FB_TOGGLE);
            break;
        }
        case 1: { // Aspect Ratio
            int v = static_cast<int>(settings->getDefaultViewMode());
            v = (v + delta + 3) % 3;
            settings->setDefaultViewMode(static_cast<ViewMode>(v));
            evo_feedback(EVO_FB_TOGGLE);
            break;
        }
        case 2: { // Resume Playback
            settings->setResumePlaybackEnabled(!settings->isResumePlaybackEnabled());
            evo_feedback(EVO_FB_TOGGLE);
            break;
        }
        case 3: { // Surround Sound Test
            // Handled via activateSelection
            break;
        }
        case 4: { // Video Decoder
            int d = static_cast<int>(settings->getVideoDecoderPreference());
            d = (d + delta + 3) % 3;
            settings->setVideoDecoderPreference(static_cast<DecoderPreference>(d));
            evo_feedback(EVO_FB_TOGGLE);
            break;
        }
        default: break;
    }
}

void SettingsPlaybackScreen::activateSelection() {
    if (m_selectedIndex == 3) {
        if (auto screenMgr = Application::getInstance().getScreenManager()) {
            evo_feedback(EVO_FB_OPEN);
            screenMgr->navigateTo(ScreenId::SurroundTest);
        }
    } else {
        adjustValue(1);
    }
}

bool SettingsPlaybackScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
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
        adjustValue(-1);
        return true;
    }
    if (pressed & PadButtons::Right) {
        adjustValue(1);
        return true;
    }
    if (pressed & PadButtons::Cross) {
        activateSelection();
        return true;
    }
    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto screenMgr = Application::getInstance().getScreenManager()) {
            screenMgr->navigateTo(ScreenId::Settings);
        }
        return true;
    }

    return false;
}

void SettingsPlaybackScreen::update(double deltaMs) {
    (void)deltaMs;
}

void SettingsPlaybackScreen::render(uint32_t* framebuffer, int width, int height) {
    auto settings = Application::getInstance().getSettingsService();
    if (!settings) return;

    evo_rmlui_settings_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.title = "PLAYBACK & VIDEO";
    params.subtitle = "SETTINGS  -  PROFILES, ASPECT RATIO & RESUME";
    params.counter = "5 SETTINGS";
    params.rail_active_idx = 5;
    params.rail_focused = 0;
    params.row_count = 5;

    params.rows[0].title = "PLAYBACK PROFILE";
    params.rows[0].detail = "HOW AGGRESSIVELY THE DECODER IS TUNED";
    params.rows[0].icon_path = "../icons/icon_settings.png";
    params.rows[0].badge = settings->getProfileName(settings->getProfile());
    params.rows[0].has_chevron = 1;
    params.rows[0].is_focused = (m_selectedIndex == 0);

    params.rows[1].title = "DEFAULT ASPECT RATIO";
    params.rows[1].detail = "FIT, FILL OR STRETCH";
    params.rows[1].icon_path = "../icons/icon_aspect.png";
    params.rows[1].badge = settings->getViewModeName(settings->getDefaultViewMode());
    params.rows[1].has_chevron = 1;
    params.rows[1].is_focused = (m_selectedIndex == 1);

    params.rows[2].title = "RESUME PLAYBACK";
    params.rows[2].detail = "REMEMBER PLAYBACK POSITION";
    params.rows[2].icon_path = "../icons/icon_resume.png";
    params.rows[2].badge = settings->isResumePlaybackEnabled() ? "ON" : "OFF";
    params.rows[2].has_chevron = 1;
    params.rows[2].is_focused = (m_selectedIndex == 2);

    params.rows[3].title = "SURROUND SOUND TEST";
    params.rows[3].detail = "5.1 & 7.1 SPEAKER CHANNEL VERIFICATION";
    params.rows[3].icon_path = "../icons/icon_resume.png";
    params.rows[3].badge = "OPEN";
    params.rows[3].has_chevron = 1;
    params.rows[3].is_focused = (m_selectedIndex == 3);

    params.rows[4].title = "VIDEO DECODER";
    params.rows[4].detail = "AUTO, SOFTWARE OR HARDWARE DECODE";
    params.rows[4].icon_path = "../icons/icon_developer_tools.png";
    params.rows[4].badge = settings->getDecoderPreferenceBadge(settings->getVideoDecoderPreference());
    params.rows[4].has_chevron = 1;
    params.rows[4].is_focused = (m_selectedIndex == 4);

    evo_rmlui_update_settings(&params);
    evo_rmlui_render_settings(framebuffer, width, height);
}

// =============================================================================
// SettingsSubtitlesScreen
// =============================================================================

SettingsSubtitlesScreen::SettingsSubtitlesScreen()
    : StatefulScreen("SettingsSubtitlesScreen") {
}

void SettingsSubtitlesScreen::onEnter() {
    StatefulScreen::onEnter();
    m_selectedIndex = 0;
}

void SettingsSubtitlesScreen::onExit() {
    StatefulScreen::onExit();
    if (auto settings = Application::getInstance().getSettingsService()) {
        settings->saveSettings();
    }
}

void SettingsSubtitlesScreen::navigate(int delta) {
    constexpr int totalRows = 2;
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

void SettingsSubtitlesScreen::adjustValue(int delta) {
    auto settings = Application::getInstance().getSettingsService();
    if (!settings) return;

    switch (m_selectedIndex) {
        case 0: { // Auto Subtitles
            settings->setAutoSubtitlesEnabled(!settings->isAutoSubtitlesEnabled());
            evo_feedback(EVO_FB_TOGGLE);
            break;
        }
        case 1: { // Subtitle Font Style
            int face = settings->getSubtitleFontFace();
            face = (face + delta + 4) % 4;
            settings->setSubtitleFontFace(face);
            evo_feedback(EVO_FB_TOGGLE);
            break;
        }
        default: break;
    }
}

void SettingsSubtitlesScreen::activateSelection() {
    adjustValue(1);
}

bool SettingsSubtitlesScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
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
        adjustValue(-1);
        return true;
    }
    if (pressed & PadButtons::Right) {
        adjustValue(1);
        return true;
    }
    if (pressed & PadButtons::Cross) {
        activateSelection();
        return true;
    }
    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto screenMgr = Application::getInstance().getScreenManager()) {
            screenMgr->navigateTo(ScreenId::Settings);
        }
        return true;
    }

    return false;
}

void SettingsSubtitlesScreen::update(double deltaMs) {
    (void)deltaMs;
}

void SettingsSubtitlesScreen::render(uint32_t* framebuffer, int width, int height) {
    auto settings = Application::getInstance().getSettingsService();
    if (!settings) return;

    evo_rmlui_settings_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.title = "SUBTITLES";
    params.subtitle = "SETTINGS  -  PREFERENCES & APPEARANCE";
    params.counter = "2 SETTINGS";
    params.rail_active_idx = 5;
    params.rail_focused = 0;
    params.row_count = 2;

    static const char* s_faceNames[] = {"STANDARD", "ROUNDED", "BOLD", "CONDENSED"};
    int face = settings->getSubtitleFontFace();
    if (face < 0 || face >= 4) face = 0;

    params.rows[0].title = "AUTO SUBTITLES";
    params.rows[0].detail = "AUTOMATICALLY LOAD SUBTITLES ON PLAYBACK";
    params.rows[0].icon_path = "../icons/icon_subtitles.png";
    params.rows[0].badge = settings->isAutoSubtitlesEnabled() ? "ON" : "OFF";
    params.rows[0].has_chevron = 1;
    params.rows[0].is_focused = (m_selectedIndex == 0);

    params.rows[1].title = "DEFAULT FONT STYLE";
    params.rows[1].detail = "ON-SCREEN TEXT TYPEFACE";
    params.rows[1].icon_path = "../icons/icon_subtitles.png";
    params.rows[1].badge = s_faceNames[face];
    params.rows[1].has_chevron = 1;
    params.rows[1].is_focused = (m_selectedIndex == 1);

    evo_rmlui_update_settings(&params);
    evo_rmlui_render_settings(framebuffer, width, height);
}

// =============================================================================
// SettingsInterfaceScreen
// =============================================================================

SettingsInterfaceScreen::SettingsInterfaceScreen()
    : StatefulScreen("SettingsInterfaceScreen") {
}

void SettingsInterfaceScreen::onEnter() {
    StatefulScreen::onEnter();
    m_selectedIndex = 0;
}

void SettingsInterfaceScreen::onExit() {
    StatefulScreen::onExit();
    if (auto settings = Application::getInstance().getSettingsService()) {
        settings->saveSettings();
    }
}

void SettingsInterfaceScreen::navigate(int delta) {
    constexpr int totalRows = 5;
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

void SettingsInterfaceScreen::adjustValue(int delta) {
    auto settings = Application::getInstance().getSettingsService();
    if (!settings) return;

    switch (m_selectedIndex) {
        case 0: { // Color Theme
            int count = evo_theme_count();
            if (count > 0) {
                int cur = evo_theme_index();
                int next = (cur + delta + count) % count;
                evo_theme_set(next);
                settings->setThemeName(evo_theme_name(next));
                evo_feedback(EVO_FB_TOGGLE);
            }
            break;
        }
        case 1: { // Navigation Sounds
            settings->setSoundFeedbackEnabled(!settings->isSoundFeedbackEnabled());
            evo_feedback(EVO_FB_TOGGLE);
            break;
        }
        case 2: { // Controller Lightbar
            settings->setLightbarFeedbackEnabled(!settings->isLightbarFeedbackEnabled());
            evo_feedback(EVO_FB_TOGGLE);
            break;
        }
        case 3: { // Folders First
            settings->setSortFoldersFirst(!settings->isSortFoldersFirst());
            evo_feedback(EVO_FB_TOGGLE);
            break;
        }
        case 4: { // Keyboard Input
            int k = settings->getKeyboardType();
            settings->setKeyboardType(k == 0 ? 1 : 0);
            evo_feedback(EVO_FB_TOGGLE);
            break;
        }
        default: break;
    }
}

void SettingsInterfaceScreen::activateSelection() {
    adjustValue(1);
}

bool SettingsInterfaceScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
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
        adjustValue(-1);
        return true;
    }
    if (pressed & PadButtons::Right) {
        adjustValue(1);
        return true;
    }
    if (pressed & PadButtons::Cross) {
        activateSelection();
        return true;
    }
    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto screenMgr = Application::getInstance().getScreenManager()) {
            screenMgr->navigateTo(ScreenId::Settings);
        }
        return true;
    }

    return false;
}

void SettingsInterfaceScreen::update(double deltaMs) {
    (void)deltaMs;
}

void SettingsInterfaceScreen::render(uint32_t* framebuffer, int width, int height) {
    auto settings = Application::getInstance().getSettingsService();
    if (!settings) return;

    evo_rmlui_settings_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.title = "INTERFACE & CONTROLS";
    params.subtitle = "SETTINGS  -  THEMES, SOUNDS & CONTROLS";
    params.counter = "5 SETTINGS";
    params.rail_active_idx = 5;
    params.rail_focused = 0;
    params.row_count = 5;

    const char* activeTheme = evo_theme_name(evo_theme_index());

    params.rows[0].title = "THEME";
    params.rows[0].detail = "COLOR PALETTE & ACCENTS";
    params.rows[0].icon_path = "../icons/icon_palette.png";
    params.rows[0].badge = activeTheme ? activeTheme : "DEFAULT";
    params.rows[0].has_chevron = 1;
    params.rows[0].is_focused = (m_selectedIndex == 0);

    params.rows[1].title = "NAVIGATION SOUNDS";
    params.rows[1].detail = "AUDIO FEEDBACK ON D-PAD & BUTTONS";
    params.rows[1].icon_path = "../icons/icon_subtitles.png";
    params.rows[1].badge = settings->isSoundFeedbackEnabled() ? "ON" : "OFF";
    params.rows[1].has_chevron = 1;
    params.rows[1].is_focused = (m_selectedIndex == 1);

    params.rows[2].title = "CONTROLLER LIGHTBAR";
    params.rows[2].detail = "DUALSENSE LIGHT COLOR";
    params.rows[2].icon_path = "../icons/icon_palette.png";
    params.rows[2].badge = settings->isLightbarFeedbackEnabled() ? "THEME ACCENT" : "OFF";
    params.rows[2].has_chevron = 1;
    params.rows[2].is_focused = (m_selectedIndex == 2);

    params.rows[3].title = "FOLDERS FIRST";
    params.rows[3].detail = "USB FILE BROWSER SORTING";
    params.rows[3].icon_path = "../icons/icon_folder.png";
    params.rows[3].badge = settings->isSortFoldersFirst() ? "ON" : "OFF";
    params.rows[3].has_chevron = 1;
    params.rows[3].is_focused = (m_selectedIndex == 3);

    params.rows[4].title = "KEYBOARD INPUT";
    params.rows[4].detail = "TEXT ENTRY METHOD";
    params.rows[4].icon_path = "../icons/icon_settings.png";
    params.rows[4].badge = (settings->getKeyboardType() == 1) ? "NATIVE PS5 IME" : "VIRTUAL KEYBOARD";
    params.rows[4].has_chevron = 1;
    params.rows[4].is_focused = (m_selectedIndex == 4);

    evo_rmlui_update_settings(&params);
    evo_rmlui_render_settings(framebuffer, width, height);
}

// =============================================================================
// SettingsSystemScreen
// =============================================================================

SettingsSystemScreen::SettingsSystemScreen()
    : StatefulScreen("SettingsSystemScreen") {
}

void SettingsSystemScreen::onEnter() {
    StatefulScreen::onEnter();
    m_selectedIndex = 0;
}

void SettingsSystemScreen::onExit() {
    StatefulScreen::onExit();
    if (auto settings = Application::getInstance().getSettingsService()) {
        settings->saveSettings();
    }
}

void SettingsSystemScreen::navigate(int delta) {
    constexpr int totalRows = 3;
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

void SettingsSystemScreen::adjustValue(int delta) {
    auto settings = Application::getInstance().getSettingsService();
    if (!settings) return;

    if (m_selectedIndex == 1) { // Debug Overlay
        settings->setDebugOverlayEnabled(!settings->isDebugOverlayEnabled());
        evo_feedback(EVO_FB_TOGGLE);
    } else {
        (void)delta;
    }
}

void SettingsSystemScreen::activateSelection() {
    if (m_selectedIndex == 0) {
        // Compatibility report
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
    } else if (m_selectedIndex == 1) {
        adjustValue(1);
    } else if (m_selectedIndex == 2) {
        if (auto screenMgr = Application::getInstance().getScreenManager()) {
            evo_feedback(EVO_FB_OPEN);
            screenMgr->navigateTo(ScreenId::DeveloperTools);
        }
    }
}

bool SettingsSystemScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
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
        adjustValue(-1);
        return true;
    }
    if (pressed & PadButtons::Right) {
        adjustValue(1);
        return true;
    }
    if (pressed & PadButtons::Cross) {
        activateSelection();
        return true;
    }
    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto screenMgr = Application::getInstance().getScreenManager()) {
            screenMgr->navigateTo(ScreenId::Settings);
        }
        return true;
    }

    return false;
}

void SettingsSystemScreen::update(double deltaMs) {
    (void)deltaMs;
}

void SettingsSystemScreen::render(uint32_t* framebuffer, int width, int height) {
    auto settings = Application::getInstance().getSettingsService();
    if (!settings) return;

    evo_rmlui_settings_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.title = "SYSTEM & DIAGNOSTICS";
    params.subtitle = "SETTINGS  -  DIAGNOSTICS & SYSTEM MANAGEMENT";
    params.counter = "3 SETTINGS";
    params.rail_active_idx = 5;
    params.rail_focused = 0;
    params.row_count = 3;

    params.rows[0].title = "COMPATIBILITY REPORT";
    params.rows[0].detail = "WRITES A CODEC REPORT TO USB0";
    params.rows[0].icon_path = "../icons/icon_developer_tools.png";
    params.rows[0].badge = "RUN";
    params.rows[0].has_chevron = 1;
    params.rows[0].is_focused = (m_selectedIndex == 0);

    params.rows[1].title = "DEBUG OVERLAY";
    params.rows[1].detail = "ON-SCREEN HARDWARE PERFORMANCE METRICS";
    params.rows[1].icon_path = "../icons/icon_aspect.png";
    params.rows[1].badge = settings->isDebugOverlayEnabled() ? "ON" : "OFF";
    params.rows[1].has_chevron = 1;
    params.rows[1].is_focused = (m_selectedIndex == 1);

    params.rows[2].title = "DEVELOPER TOOLS";
    params.rows[2].detail = "SYSTEM DIAGNOSTICS & PERFORMANCE STATS";
    params.rows[2].icon_path = "../icons/icon_settings.png";
    params.rows[2].badge = "OPEN";
    params.rows[2].has_chevron = 1;
    params.rows[2].is_focused = (m_selectedIndex == 2);

    evo_rmlui_update_settings(&params);
    evo_rmlui_render_settings(framebuffer, width, height);
}

} // namespace evo
