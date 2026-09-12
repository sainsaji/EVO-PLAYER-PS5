#ifndef EVO_I_SETTINGS_SERVICE_HPP
#define EVO_I_SETTINGS_SERVICE_HPP

#include "evo/Common.hpp"
#include <string>

namespace evo {

/**
 * @brief Interface for application configuration and user preferences.
 * 
 * Provides abstraction for persistent settings serialization, theme synchronization,
 * and user-defined runtime preferences.
 */
class ISettingsService {
public:
    virtual ~ISettingsService() = default;

    virtual bool loadSettings() = 0;
    virtual bool saveSettings() = 0;

    virtual PlaybackProfile getProfile() const = 0;
    virtual void setProfile(PlaybackProfile profile) = 0;
    virtual const char* getProfileName(PlaybackProfile profile) const = 0;

    virtual bool isResumePlaybackEnabled() const = 0;
    virtual void setResumePlaybackEnabled(bool enabled) = 0;

    virtual ViewMode getDefaultViewMode() const = 0;
    virtual void setDefaultViewMode(ViewMode mode) = 0;
    virtual const char* getViewModeName(ViewMode mode) const = 0;

    virtual bool isAutoSubtitlesEnabled() const = 0;
    virtual void setAutoSubtitlesEnabled(bool enabled) = 0;

    virtual bool isSortFoldersFirst() const = 0;
    virtual void setSortFoldersFirst(bool enabled) = 0;

    virtual std::string getThemeName() const = 0;
    virtual void setThemeName(const std::string& themeName) = 0;

    virtual bool isSoundFeedbackEnabled() const = 0;
    virtual void setSoundFeedbackEnabled(bool enabled) = 0;

    virtual bool isLightbarFeedbackEnabled() const = 0;
    virtual void setLightbarFeedbackEnabled(bool enabled) = 0;

    virtual int getSubtitleFontFace() const = 0;
    virtual void setSubtitleFontFace(int face) = 0;

    virtual int getKeyboardType() const = 0;
    virtual void setKeyboardType(int type) = 0;

    virtual DecoderPreference getVideoDecoderPreference() const = 0;
    virtual void setVideoDecoderPreference(DecoderPreference preference) = 0;
    virtual const char* getDecoderPreferenceBadge(DecoderPreference preference) const = 0;

    virtual bool isDebugOverlayEnabled() const = 0;
    virtual void setDebugOverlayEnabled(bool enabled) = 0;

    virtual void syncThemeToRmlUi() = 0;
};

} // namespace evo

#endif // EVO_I_SETTINGS_SERVICE_HPP
