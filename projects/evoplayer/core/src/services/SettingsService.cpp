#include "evo/services/SettingsService.hpp"
#include "evo_data_path.h"
#include "evo_theme.h"
#include "evo_toast.h"
#include "evo_feedback.h"
#include "evo_keyboard.h"
#include "evo_rmlui_bridge.h"
#include "evo_vdec.h"

#include <cstdio>
#include <cstring>

namespace evo {

SettingsService::SettingsService() {
    m_themeName = "EVO Dark";
}

const char* SettingsService::getProfileName(PlaybackProfile profile) const {
    switch (profile) {
        case PlaybackProfile::Balanced:      return "Balanced";
        case PlaybackProfile::Performance:   return "Performance";
        case PlaybackProfile::Compatibility: return "Compatibility";
        case PlaybackProfile::Debug:         return "Debug";
        default:                             return "Balanced";
    }
}

const char* SettingsService::getViewModeName(ViewMode mode) const {
    switch (mode) {
        case ViewMode::Fit:     return "FIT";
        case ViewMode::Fill:    return "FILL";
        case ViewMode::Stretch: return "STRETCH";
        default:                return "FIT";
    }
}

const char* SettingsService::getDecoderPreferenceBadge(DecoderPreference preference) const {
    switch (preference) {
        case DecoderPreference::Auto:           return "AUTO";
        case DecoderPreference::NativeHardware: return "HARDWARE (sceVideodec2)";
        case DecoderPreference::FFmpegSoftware: return "SOFTWARE (FFmpeg)";
        default:                                return "AUTO";
    }
}

void SettingsService::setThemeName(const std::string& themeName) {
    m_themeName = themeName;
    evo_theme_set_by_name(m_themeName.c_str());
    syncThemeToRmlUi();
    evo_feedback_refresh_lightbar();
}

void SettingsService::setSoundFeedbackEnabled(bool enabled) {
    m_soundFeedbackEnabled = enabled;
    evo_feedback_set_sound(enabled ? 1 : 0);
}

void SettingsService::setLightbarFeedbackEnabled(bool enabled) {
    m_lightbarFeedbackEnabled = enabled;
    evo_feedback_set_lightbar(enabled ? 1 : 0);
}

void SettingsService::setKeyboardType(int type) {
    m_keyboardType = type;
    evo_keyboard_set_type(type);
}

bool SettingsService::saveSettings() {
    const char* filePath = evo_data_path("evo_player_settings.cfg");
    FILE* file = std::fopen(filePath, "w");
    if (!file) {
        toast("SETTINGS", "SAVE FAILED");
        return false;
    }

    const char* activeTheme = evo_theme_name(evo_theme_index());
    if (!activeTheme || !activeTheme[0]) {
        activeTheme = m_themeName.c_str();
    }

    std::fprintf(file,
        "%d\n%d\n%d\n%d\n%d\n%d\n%s\n%d\n%d\n%d\n%d\n%d\n%d\n",
        static_cast<int>(m_profile),
        m_resumePlaybackEnabled ? 1 : 0,
        static_cast<int>(m_defaultViewMode),
        m_autoSubtitlesEnabled ? 1 : 0,
        m_debugOverlayEnabled ? 1 : 0,
        m_sortFoldersFirst ? 1 : 0,
        activeTheme,
        m_soundFeedbackEnabled ? 1 : 0,
        m_lightbarFeedbackEnabled ? 1 : 0,
        m_subtitleFontFace,
        m_keyboardType,
        static_cast<int>(m_decoderPreference),
        1 // marker that keyboard preference was explicitly saved
    );

    std::fclose(file);
    return true;
}

bool SettingsService::loadSettings() {
    const char* filePath = evo_data_path("evo_player_settings.cfg");
    FILE* file = std::fopen(filePath, "r");
    if (!file) {
        return false;
    }

    int rawProfile = 0;
    int rawResume = 1;
    int rawViewMode = 0;
    int rawAutoSubs = 1;
    int rawDebugOverlay = 0;
    int rawSortFolders = 1;
    char themeBuffer[128] = {0};
    int rawSoundFeedback = 1;
    int rawLightbarFeedback = 1;
    int rawSubtitleFace = 1;
    int rawKeyboardType = 0;
    int rawDecoderPref = EVO_VDEC_PREF_AUTO;
    int explicitKeyboardMarker = 0;

    int readCount = std::fscanf(file,
        "%d\n%d\n%d\n%d\n%d\n%d\n%127[^\n]\n%d\n%d\n%d\n%d\n%d\n%d",
        &rawProfile,
        &rawResume,
        &rawViewMode,
        &rawAutoSubs,
        &rawDebugOverlay,
        &rawSortFolders,
        themeBuffer,
        &rawSoundFeedback,
        &rawLightbarFeedback,
        &rawSubtitleFace,
        &rawKeyboardType,
        &rawDecoderPref,
        &explicitKeyboardMarker
    );

    std::fclose(file);

    if (readCount >= 1) {
        if (rawProfile >= 0 && rawProfile <= 3) {
            m_profile = static_cast<PlaybackProfile>(rawProfile);
        }
    }
    if (readCount >= 2) m_resumePlaybackEnabled = (rawResume != 0);
    if (readCount >= 3) {
        if (rawViewMode >= 0 && rawViewMode <= 2) {
            m_defaultViewMode = static_cast<ViewMode>(rawViewMode);
        }
    }
    if (readCount >= 4) m_autoSubtitlesEnabled = (rawAutoSubs != 0);
    if (readCount >= 5) m_debugOverlayEnabled = (rawDebugOverlay != 0);
    if (readCount >= 6) m_sortFoldersFirst = (rawSortFolders != 0);
    if (readCount >= 7 && themeBuffer[0] != '\0') {
        m_themeName = themeBuffer;
        evo_theme_set_by_name(themeBuffer);
    }
    if (readCount >= 8) {
        m_soundFeedbackEnabled = (rawSoundFeedback != 0);
        evo_feedback_set_sound(rawSoundFeedback);
    }
    if (readCount >= 9) {
        m_lightbarFeedbackEnabled = (rawLightbarFeedback != 0);
        evo_feedback_set_lightbar(rawLightbarFeedback);
    }
    if (readCount >= 10) {
        m_subtitleFontFace = rawSubtitleFace;
    }
    if (readCount >= 11) {
        if (readCount >= 13 && explicitKeyboardMarker == 1) {
            m_keyboardType = rawKeyboardType;
        } else {
            m_keyboardType = EVO_KEYBOARD_TYPE_NATIVE;
        }
        evo_keyboard_set_type(m_keyboardType);
    }
    if (readCount >= 12) {
        m_decoderPreference = static_cast<DecoderPreference>(rawDecoderPref);
    }

    syncThemeToRmlUi();
    evo_feedback_refresh_lightbar();
    return true;
}

void SettingsService::syncThemeToRmlUi() {
    const evo_theme* t = evo_theme_current();
    if (!t) return;

    evo_rmlui_theme_t rmlUiTheme;
    rmlUiTheme.name = t->name;
    rmlUiTheme.bg_top = t->bg_top;
    rmlUiTheme.bg_bottom = t->bg_bottom;
    rmlUiTheme.surface = t->surface;
    rmlUiTheme.surface_sel = t->surface_sel;
    rmlUiTheme.border = t->border;
    rmlUiTheme.border_sel = t->border_sel;
    rmlUiTheme.accent = t->accent;
    rmlUiTheme.accent_soft = t->accent_soft;
    rmlUiTheme.accent_alt = t->accent_alt;
    rmlUiTheme.text_primary = t->text_primary;
    rmlUiTheme.text_secondary = t->text_secondary;
    rmlUiTheme.text_muted = t->text_muted;

    evo_rmlui_set_theme(&rmlUiTheme);
}

} // namespace evo
