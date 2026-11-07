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
    void setSubtitleFontFace(int face) override;

    bool isAskStreamEnabled() const override { return m_askStream; }
    void setAskStreamEnabled(bool enabled) override { m_askStream = enabled; }
    bool isAddonAutoPickEnabled() const override { return m_addonAutoPick; }
    void setAddonAutoPickEnabled(bool enabled) override { m_addonAutoPick = enabled; }

    int getSecondarySubtitlePosition() const override { return m_secondarySubtitlePosition; }
    void setSecondarySubtitlePosition(int position) override;
    int getSecondarySubtitleColor() const override { return m_secondarySubtitleColor; }
    void setSecondarySubtitleColor(int color) override;
    int getSubtitleSize() const override { return m_subtitleSize; }
    void setSubtitleSize(int size) override;
    int getSubtitleColor() const override { return m_subtitleColor; }
    void setSubtitleColor(int color) override;
    int getSubtitleLift() const override { return m_subtitleLift; }
    void setSubtitleLift(int lift) override;
    int getSubtitleBackground() const override { return m_subtitleBackground; }
    void setSubtitleBackground(int background) override;

    int getKeyboardType() const override { return m_keyboardType; }
    void setKeyboardType(int type) override;

    DecoderPreference getVideoDecoderPreference() const override { return m_decoderPreference; }
    void setVideoDecoderPreference(DecoderPreference preference) override { m_decoderPreference = preference; }
    const char* getDecoderPreferenceBadge(DecoderPreference preference) const override;

    Upscaler getUpscaler() const override { return m_upscaler; }
    void setUpscaler(Upscaler upscaler) override { m_upscaler = upscaler; }
    const char* getUpscalerName(Upscaler upscaler) const override;

    AiNetwork getAiNetwork() const override { return m_aiNetwork; }
    void setAiNetwork(AiNetwork network) override { m_aiNetwork = network; }
    const char* getAiNetworkName(AiNetwork network) const override;

    MotionSmoothing getMotionSmoothing() const override { return m_motionSmoothing; }
    void setMotionSmoothing(MotionSmoothing smoothing) override { m_motionSmoothing = smoothing; }
    const char* getMotionSmoothingName(MotionSmoothing smoothing) const override;

    DeepBlacks getDeepBlacks() const override { return m_deepBlacks; }
    void setDeepBlacks(DeepBlacks mode) override { m_deepBlacks = mode; }
    const char* getDeepBlacksName(DeepBlacks mode) const override;

    bool isDebugOverlayEnabled() const override { return m_debugOverlayEnabled; }
    void setDebugOverlayEnabled(bool enabled) override { m_debugOverlayEnabled = enabled; }

    RefreshRateMode getRefreshRateMode() const override { return m_refreshRateMode; }
    void setRefreshRateMode(RefreshRateMode mode) override { m_refreshRateMode = mode; }
    const char* getRefreshRateModeName(RefreshRateMode mode) const override;

    HdrOutputMode getHdrOutputMode() const override { return m_hdrOutputMode; }
    void setHdrOutputMode(HdrOutputMode mode) override { m_hdrOutputMode = mode; }
    const char* getHdrOutputModeName(HdrOutputMode mode) const override;

    AudioOutputChannels getAudioOutputChannels() const override { return m_audioOutputChannels; }
    void setAudioOutputChannels(AudioOutputChannels channels) override { m_audioOutputChannels = channels; }
    const char* getAudioOutputChannelsName(AudioOutputChannels channels) const override;

    NetworkBuffer getNetworkBuffer() const override { return m_networkBuffer; }
    void setNetworkBuffer(NetworkBuffer buffer) override { m_networkBuffer = buffer; }
    const char* getNetworkBufferName(NetworkBuffer buffer) const override;

    SkipSegments getSkipSegments() const override { return m_skipSegments; }
    void setSkipSegments(SkipSegments mode) override { m_skipSegments = mode; }
    const char* getSkipSegmentsName(SkipSegments mode) const override;

    VolumeBoost getVolumeBoost() const override { return m_volumeBoost; }
    void setVolumeBoost(VolumeBoost boost) override { m_volumeBoost = boost; }
    const char* getVolumeBoostName(VolumeBoost boost) const override;

    int getAudioDelayMs() const override { return m_audioDelayMs; }
    void setAudioDelayMs(int delayMs) override;

    bool isAudioPassthroughEnabled() const override { return m_audioPassthroughEnabled; }
    void setAudioPassthroughEnabled(bool enabled) override { m_audioPassthroughEnabled = enabled; }
    bool isMatchVideoRateEnabled() const override { return m_matchVideoRate; }
    void setMatchVideoRateEnabled(bool enabled) override { m_matchVideoRate = enabled; }

    HdrPeak getHdrPeak() const override { return m_hdrPeak; }
    void setHdrPeak(HdrPeak peak) override { m_hdrPeak = peak; }
    const char* getHdrPeakName(HdrPeak peak) const override;

    PreferredLanguage getAudioLanguage() const override { return m_audioLanguage; }
    void setAudioLanguage(PreferredLanguage lang) override;
    PreferredLanguage getSubtitleLanguage() const override { return m_subtitleLanguage; }
    void setSubtitleLanguage(PreferredLanguage lang) override;
    const char* getPreferredLanguageName(PreferredLanguage lang) const override;

    void syncThemeToRmlUi() override;

private:
    bool m_resumePlaybackEnabled = true;
    ViewMode m_defaultViewMode = ViewMode::Fit;
    bool m_autoSubtitlesEnabled = true;
    bool m_sortFoldersFirst = true;
    std::string m_themeName = "EVO Dark";
    bool m_soundFeedbackEnabled = true;
    bool m_lightbarFeedbackEnabled = true;
    int m_subtitleFontFace = 0; // STANDARD (prospero_subtitle_style)
    int m_subtitleSize = 1;       // #231: MEDIUM (prospero_subtitle_face - 1)
    int m_subtitleColor = 0;      // #231: WHITE
    int m_subtitleLift = 0;       // #231: BOTTOM
    int m_subtitleBackground = 0; // #231: BOX
    bool m_askStream = true;    // Live TV: pick the stream yourself
    int m_secondarySubtitlePosition = 0; // #110: stacked above the primary
    int m_secondarySubtitleColor = 0;    // #110: yellow
    /* EVO_KEYBOARD_TYPE_NATIVE (1) - the enum has VIRTUAL = 0, and this
     * used to be 0 under a "Native IME" comment, so a fresh install saved
     * (and kept) the virtual keyboard nobody picked. */
    int m_keyboardType = 1;
    DecoderPreference m_decoderPreference = DecoderPreference::Auto;
    // Off until the upscaler is hardware-verified (#103), then Sharp.
    Upscaler m_upscaler = Upscaler::Off;
    AiNetwork m_aiNetwork = AiNetwork::Auto;
    MotionSmoothing m_motionSmoothing = MotionSmoothing::Off;
    DeepBlacks m_deepBlacks = DeepBlacks::Off;
    RefreshRateMode m_refreshRateMode = RefreshRateMode::Off;
    HdrOutputMode m_hdrOutputMode = HdrOutputMode::Auto;
    AudioOutputChannels m_audioOutputChannels = AudioOutputChannels::Auto;
    VolumeBoost m_volumeBoost = VolumeBoost::Off;
    int m_audioDelayMs = 0;
    NetworkBuffer m_networkBuffer = NetworkBuffer::Sec30;
    bool m_audioPassthroughEnabled = false;
    bool m_matchVideoRate = false;
    bool m_debugOverlayEnabled = false;
    HdrPeak m_hdrPeak = HdrPeak::Off;
    bool m_addonAutoPick = true;   /* addons: rank the sources, play the best at once */
    SkipSegments m_skipSegments = SkipSegments::Ask;
    PreferredLanguage m_audioLanguage = PreferredLanguage::Auto;
    PreferredLanguage m_subtitleLanguage = PreferredLanguage::Auto;
};


} // namespace evo

#endif // EVO_SETTINGS_SERVICE_HPP
