#ifndef EVO_SETTINGS_SERVICE_HPP
#define EVO_SETTINGS_SERVICE_HPP

#include "evo/interfaces/ISettingsService.hpp"
#include <string>

namespace evo {

class SettingsService : public ISettingsService {
public:
    SettingsService();
    ~SettingsService() override = default;

    bool loadSettings() override;
    bool saveSettings() override;

    bool isResumePlaybackEnabled() const override { return m_resumePlaybackEnabled; }
    void setResumePlaybackEnabled(bool enabled) override { m_resumePlaybackEnabled = enabled; }

    ViewMode getDefaultViewMode() const override { return m_defaultViewMode; }
    void setDefaultViewMode(ViewMode mode) override { m_defaultViewMode = mode; }
    const char* getViewModeName(ViewMode mode) const override;

    bool isAutoSubtitlesEnabled() const override { return m_autoSubtitlesEnabled; }
    void setAutoSubtitlesEnabled(bool enabled) override { m_autoSubtitlesEnabled = enabled; }

    bool isSortFoldersFirst() const override { return m_sortFoldersFirst; }
    void setSortFoldersFirst(bool enabled) override { m_sortFoldersFirst = enabled; }

    std::string getThemeName() const override { return m_themeName; }
    void setThemeName(const std::string& themeName) override;

    bool isSoundFeedbackEnabled() const override { return m_soundFeedbackEnabled; }
    void setSoundFeedbackEnabled(bool enabled) override;

    bool isLightbarFeedbackEnabled() const override { return m_lightbarFeedbackEnabled; }
    void setLightbarFeedbackEnabled(bool enabled) override;

    int getSubtitleFontFace() const override { return m_subtitleFontFace; }
    void setSubtitleFontFace(int face) override { m_subtitleFontFace = face; }

    int getKeyboardType() const override { return m_keyboardType; }
    void setKeyboardType(int type) override;

    DecoderPreference getVideoDecoderPreference() const override { return m_decoderPreference; }
    void setVideoDecoderPreference(DecoderPreference preference) override { m_decoderPreference = preference; }
    const char* getDecoderPreferenceBadge(DecoderPreference preference) const override;

    bool isDebugOverlayEnabled() const override { return m_debugOverlayEnabled; }
    void setDebugOverlayEnabled(bool enabled) override { m_debugOverlayEnabled = enabled; }

    void syncThemeToRmlUi() override;

private:
    bool m_resumePlaybackEnabled = true;
    ViewMode m_defaultViewMode = ViewMode::Fit;
    bool m_autoSubtitlesEnabled = true;
    bool m_sortFoldersFirst = true;
    std::string m_themeName = "EVO Dark";
    bool m_soundFeedbackEnabled = true;
    bool m_lightbarFeedbackEnabled = true;
    int m_subtitleFontFace = 1; // Medium
    int m_keyboardType = 0;     // Native IME
    DecoderPreference m_decoderPreference = DecoderPreference::Auto;
    bool m_debugOverlayEnabled = false;
};

} // namespace evo

#endif // EVO_SETTINGS_SERVICE_HPP
