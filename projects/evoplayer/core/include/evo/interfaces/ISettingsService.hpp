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

    /* Live TV: open a channel on a picker of the streams behind it (the URL as
     * listed, each HLS quality variant, the extension guess) instead of letting
     * EVO choose and fall back on its own. */
    virtual bool isAskStreamEnabled() const = 0;
    virtual void setAskStreamEnabled(bool enabled) = 0;
    /* Stremio addons: play the best-ranked source at once (Remux, Dolby
     * Vision/HDR, cached on the debrid service), the full list one press
     * away in the title's OPTIONS. Off: always open the list. */
    virtual bool isAddonAutoPickEnabled() const = 0;
    virtual void setAddonAutoPickEnabled(bool enabled) = 0;

    /* #110: where the second line of dialogue goes (0 stacked above the
     * primary, 1 top of the screen) and its colour (0 yellow, 1 cyan, 2 white). */
    virtual int getSecondarySubtitlePosition() const = 0;
    virtual void setSecondarySubtitlePosition(int position) = 0;
    virtual int getSecondarySubtitleColor() const = 0;
    virtual void setSecondarySubtitleColor(int color) = 0;
    virtual int getSecondarySubtitleSize() const = 0;
    virtual void setSecondarySubtitleSize(int size) = 0;
    virtual int getSecondarySubtitleBackground() const = 0;
    virtual void setSecondarySubtitleBackground(int background) = 0;
    // #231: the primary caption's look (indexes, see evo_subtitle.h)
    virtual int getSubtitleSize() const = 0;          // 0 small .. 3 extra large
    virtual void setSubtitleSize(int size) = 0;
    virtual int getSubtitleColor() const = 0;
    virtual void setSubtitleColor(int color) = 0;
    virtual int getSubtitleLift() const = 0;
    virtual void setSubtitleLift(int lift) = 0;
    virtual int getSubtitleBackground() const = 0;
    virtual void setSubtitleBackground(int background) = 0;

    virtual int getKeyboardType() const = 0;
    virtual void setKeyboardType(int type) = 0;

    virtual DecoderPreference getVideoDecoderPreference() const = 0;
    virtual void setVideoDecoderPreference(DecoderPreference preference) = 0;
    virtual const char* getDecoderPreferenceBadge(DecoderPreference preference) const = 0;

    virtual Upscaler getUpscaler() const = 0;
    virtual void setUpscaler(Upscaler upscaler) = 0;
    virtual const char* getUpscalerName(Upscaler upscaler) const = 0;

    virtual AiNetwork getAiNetwork() const = 0;
    virtual void setAiNetwork(AiNetwork network) = 0;
    virtual const char* getAiNetworkName(AiNetwork network) const = 0;

    virtual MotionSmoothing getMotionSmoothing() const = 0;
    virtual void setMotionSmoothing(MotionSmoothing smoothing) = 0;
    virtual const char* getMotionSmoothingName(MotionSmoothing smoothing) const = 0;

    virtual DeepBlacks getDeepBlacks() const = 0;
    virtual void setDeepBlacks(DeepBlacks mode) = 0;
    virtual const char* getDeepBlacksName(DeepBlacks mode) const = 0;

    virtual bool isDebugOverlayEnabled() const = 0;
    virtual void setDebugOverlayEnabled(bool enabled) = 0;

    virtual RefreshRateMode getRefreshRateMode() const = 0;
    virtual void setRefreshRateMode(RefreshRateMode mode) = 0;
    virtual const char* getRefreshRateModeName(RefreshRateMode mode) const = 0;

    virtual HdrOutputMode getHdrOutputMode() const = 0;
    virtual void setHdrOutputMode(HdrOutputMode mode) = 0;
    virtual const char* getHdrOutputModeName(HdrOutputMode mode) const = 0;

    virtual AudioOutputChannels getAudioOutputChannels() const = 0;
    virtual void setAudioOutputChannels(AudioOutputChannels channels) = 0;
    virtual const char* getAudioOutputChannelsName(AudioOutputChannels channels) const = 0;

    virtual NetworkBuffer getNetworkBuffer() const = 0;
    virtual void setNetworkBuffer(NetworkBuffer buffer) = 0;
    virtual const char* getNetworkBufferName(NetworkBuffer buffer) const = 0;

    virtual SkipSegments getSkipSegments() const = 0;
    virtual void setSkipSegments(SkipSegments mode) = 0;
    virtual const char* getSkipSegmentsName(SkipSegments mode) const = 0;

    virtual VolumeBoost getVolumeBoost() const = 0;
    virtual void setVolumeBoost(VolumeBoost boost) = 0;
    virtual const char* getVolumeBoostName(VolumeBoost boost) const = 0;

    virtual int getAudioDelayMs() const = 0;
    virtual void setAudioDelayMs(int delayMs) = 0;

    virtual bool isAudioPassthroughEnabled() const = 0;
    virtual void setAudioPassthroughEnabled(bool enabled) = 0;
    virtual bool isStereoUpmixEnabled() const = 0;
    virtual void setStereoUpmixEnabled(bool enabled) = 0;
    virtual bool isPhoneSpeakerEnabled() const = 0;
    virtual void setPhoneSpeakerEnabled(bool enabled) = 0;
    // Experimental: drive the panel at the video's own rate during playback
    virtual bool isMatchVideoRateEnabled() const = 0;
    virtual void setMatchVideoRateEnabled(bool enabled) = 0;

    // Experimental: built-in web browser
    virtual bool isWebBrowserEnabled() const = 0;
    virtual void setWebBrowserEnabled(bool enabled) = 0;

    // Phone casting (DIAL) receiver
    virtual bool isCastReceiverEnabled() const = 0;
    virtual void setCastReceiverEnabled(bool enabled) = 0;
    virtual int getCastPort() const = 0;
    virtual void setCastPort(int port) = 0;
    virtual std::string getCastDeviceName() const = 0;
    virtual void setCastDeviceName(const std::string& name) = 0;
    virtual bool isCastNativePlayerEnabled() const = 0;
    virtual void setCastNativePlayerEnabled(bool enabled) = 0;

    // HDR peak mapping (panel peak, Off = the TV tone-maps)
    virtual HdrPeak getHdrPeak() const = 0;
    virtual void setHdrPeak(HdrPeak peak) = 0;
    virtual const char* getHdrPeakName(HdrPeak peak) const = 0;

    // Preferred audio / subtitle language (track choice when a file opens)
    virtual PreferredLanguage getAudioLanguage() const = 0;
    virtual void setAudioLanguage(PreferredLanguage lang) = 0;
    virtual PreferredLanguage getSubtitleLanguage() const = 0;
    virtual void setSubtitleLanguage(PreferredLanguage lang) = 0;
    virtual const char* getPreferredLanguageName(PreferredLanguage lang) const = 0;

    virtual void syncThemeToRmlUi() = 0;

};

} // namespace evo

#endif // EVO_I_SETTINGS_SERVICE_HPP
