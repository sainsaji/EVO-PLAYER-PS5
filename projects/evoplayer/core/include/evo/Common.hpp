#ifndef EVO_COMMON_HPP
#define EVO_COMMON_HPP

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace evo {

/*
 * The size EVO actually renders at. Resolved once at startup from
 * sceVideoOutGetResolutionStatus (see evo_agc_runtime_init) and fixed for the
 * session - not a constant, because a 1440p panel should be driven at 1440p
 * rather than upscaled from 1080p.
 *
 * This is NOT the UI's unit system. Stylesheets stay authored against the
 * EVO_UI_DESIGN_W x _H canvas in dp (ui/include/evo_metrics.h); the RmlUi
 * contexts get a density-independent pixel ratio of DisplayWidth /
 * EVO_UI_DESIGN_W, so layout and glyph rasterisation happen at the real
 * pixel size while the source geometry never changes.
 */
extern int DisplayWidth;
extern int DisplayHeight;

// Screen Identifiers (compatible with legacy UI router)
enum class ScreenId : int {
    MainMenu = 0,
    UsbBrowser = 1,
    Player = 2,
    ResumePrompt = 3,
    Settings = 10,
    ProfileSelect = 11,
    RecentFiles = 12,
    Favorites = 13,
    AboutSupport = 14,
    DeveloperTools = 15,
    MediaInfo = 16,
    PlaybackFinished = 17,
    SubtitlePicker = 18,
    Changelog = 19,
    ExitConfirm = 20,
    TextReader = 21,
    EmbySetup = 22,
    EmbyBrowse = 23,
    SettingsPlayback = 24,
    SettingsSubtitles = 25,
    SettingsInterface = 26,
    SettingsSystem = 27,
    SurroundTest = 28,
    ThemeSelect = 29,
    ImageViewer = 30,
    AudioTrackPicker = 31,
    SafeToClose = 32,    /* the frame EVO parks on after QUIT (SafeToCloseScreen) */
    SettingsAudio = 33,  /* #118 */
    SettingsExperimental = 34, /* #105 */
    ChapterPicker = 35,        /* #150 */
    SubtitleFile = 36,         /* pick a subtitle file anywhere on the console */
    WebHome = 37,              /* #237: the Web section (Prospero Browser's UI, browser/) */
    WebPage = 38               /* #237: no longer used (was the page view); kept so the ids stay */
};

// Playback Quality Profile
enum class PlaybackProfile : int {
    Balanced = 0,
    Performance = 1,
    Compatibility = 2,
    Debug = 3
};

// Video Scaling / Aspect Ratio View Mode
enum class ViewMode : int {
    Fit = 0,
    Fill = 1,
    Stretch = 2
};

// Video Decoder Backend Preference
enum class DecoderPreference : int {
    Auto = 0,
    NativeHardware = 1,
    FFmpegSoftware = 2
};

// Video upscaler for sources smaller than the panel (#103). Values match
// EVO_AGC_UPSCALE_* in evo_agc_runtime.h.
enum class Upscaler : int {
    Off = 0,
    Sharp = 1,   // FSR1 EASU + RCAS
    AI = 2       // Anime4K CNN x2 in shaders (larger network on PS5 Pro)
};

// Which network AI upscaling runs (#103). Values match EVO_AGC_UPNET_*.
// Auto follows PS5 Pro detection; the others override it, because the probe
// cannot identify every Pro. Large falls back to Standard on its own.
enum class AiNetwork : int {
    Auto = 0,
    Standard = 1,   // Anime4K S
    Large = 2,      // Anime4K M
    Maximum = 3     // Anime4K UL - sized for a PS5 Pro
};

// Frame interpolation / motion smoothing (24 -> 60 fps) (#105). Values match
// EVO_AGC_MOTION_SMOOTH_* in evo_agc_runtime.h.
enum class MotionSmoothing : int {
    Off = 0,
    Low = 1,    // whole-pixel search, short half-strength warp
    High = 2    // quarter-pel search, full-strength warp
};

// Deep Blacks (#119): pulls elevated shadows down to true OLED black in the
// video shader. Values match EVO_AGC_DEEP_BLACK_*.
enum class DeepBlacks : int {
    Off = 0,
    Low = 1,    // subtle toe, ~1-2 % near-black pull-down
    High = 2    // stronger, for washed-out masters
};

// Refresh rate output mode (60 Hz vs 120 Hz)
enum class RefreshRateMode : int {
    Off = 0,
    Always = 1,
    PlaybackOnly = 2
};

// HDR10 output: Auto switches the TV into HDR10 while an HDR10 / HLG video is
// on screen (and back to SDR otherwise); Off keeps SDR and tone-maps HDR.
enum class HdrOutputMode : int {
    Auto = 0,
    Off = 1
};

// Audio output channels (#117): Auto opens an 8-channel port for surround
// sources; Stereo always opens 2.0 and libswresample downmixes surround to it.
enum class AudioOutputChannels : int {
    Auto = 0,
    Stereo = 1
};

// HDR peak mapping: the panel's peak brightness. Off leaves tone mapping to
// the TV (HDR10 passthrough, as before); a value fits each Dolby Vision shot
// (from its L1 metadata) or an HDR10 film (from MaxCLL) to that peak.
enum class HdrPeak : int {
    Off = 0,
    Nits800 = 1,
    Nits1000 = 2,
    Nits1500 = 3,
    Nits2000 = 4,
    Nits3000 = 5
};

inline int getHdrPeakNits(HdrPeak peak) {
    switch (peak) {
        case HdrPeak::Nits800:  return 800;
        case HdrPeak::Nits1000: return 1000;
        case HdrPeak::Nits1500: return 1500;
        case HdrPeak::Nits2000: return 2000;
        case HdrPeak::Nits3000: return 3000;
        case HdrPeak::Off:
        default:                return 0;
    }
}

// Preferred audio / subtitle language. Auto keeps the old choice (first
// decodable audio track, English subtitles).
enum class PreferredLanguage : int {
    Auto = 0,
    Italian = 1,
    English = 2,
    Spanish = 3,
    French = 4,
    German = 5,
    Portuguese = 6,
    Japanese = 7
};
constexpr int kPreferredLanguageCount = 8;

inline const char* getPreferredLanguageCode(PreferredLanguage lang) {
    switch (lang) {
        case PreferredLanguage::Italian:    return "ita";
        case PreferredLanguage::English:    return "eng";
        case PreferredLanguage::Spanish:    return "spa";
        case PreferredLanguage::French:     return "fra";
        case PreferredLanguage::German:     return "deu";
        case PreferredLanguage::Portuguese: return "por";
        case PreferredLanguage::Japanese:   return "jpn";
        case PreferredLanguage::Auto:
        default:                            return "";
    }
}

// Audio volume boost (#166): Off, +50%, +100%, +200%, +300%.
// Multiplies decoded PCM before sceAudioOutOutput with clean int16 clipping.
enum class VolumeBoost : int {
    Off = 0,
    Plus50 = 1,
    Plus100 = 2,
    Plus200 = 3,
    Plus300 = 4
};

// Network buffer (#199): how far ahead a network stream reads. Memory caps the
// real value for very high bitrates.
enum class NetworkBuffer : int {
    Sec30 = 0,
    Min1 = 1,
    Min2 = 2,
    Min5 = 3
};

// Skip intros & credits: Off never offers a skip; Ask shows a button; Auto skips once.
enum class SkipSegments : int {
    Off = 0,
    Ask = 1,
    Auto = 2
};

inline int getNetworkBufferSeconds(NetworkBuffer buffer) {
    switch (buffer) {
        case NetworkBuffer::Min1: return 60;
        case NetworkBuffer::Min2: return 120;
        case NetworkBuffer::Min5: return 300;
        case NetworkBuffer::Sec30:
        default:                  return 30;
    }
}

inline int getVolumeBoostPercent(VolumeBoost boost) {
    switch (boost) {
        case VolumeBoost::Plus50:  return 50;
        case VolumeBoost::Plus100: return 100;
        case VolumeBoost::Plus200: return 200;
        case VolumeBoost::Plus300: return 300;
        case VolumeBoost::Off:
        default:                   return 0;
    }
}


// File Classification for Storage Browser
enum class FileCategory : int {
    Unknown = 0,
    Video,
    Audio,
    Image,
    Document,
    DiscImage,
    Homebrew,
    Folder
};

// Synthesized UI Sound Effects
enum class SoundEffect : int {
    None = 0,
    Move = 1,
    Confirm = 2,
    Back = 3,
    Toggle = 4
};

// Controller Button Masks (Standard SCE Layout)
namespace PadButtons {
    constexpr uint32_t Options  = 0x0008;
    constexpr uint32_t Up       = 0x0010;
    constexpr uint32_t Right    = 0x0020;
    constexpr uint32_t Down     = 0x0040;
    constexpr uint32_t Left     = 0x0080;
    constexpr uint32_t L2       = 0x0100;
    constexpr uint32_t R2       = 0x0200;
    constexpr uint32_t L1       = 0x0400;
    constexpr uint32_t R1       = 0x0800;
    constexpr uint32_t Triangle = 0x1000;
    constexpr uint32_t Circle   = 0x2000;
    constexpr uint32_t Cross    = 0x4000;
    constexpr uint32_t Square   = 0x8000;
    constexpr uint32_t L3            = 0x0002;
    constexpr uint32_t R3            = 0x0004;
    constexpr uint32_t TouchPad      = 0x00100000; // Physical touchpad click
    constexpr uint32_t TouchPadLeft  = 0x00200000; // Touchpad click/swipe on left half
    constexpr uint32_t TouchPadRight = 0x00400000; // Touchpad click/swipe on right half
}

// Color packing helper for 0xAABBGGRR (BGRA in little-endian uint32_t)
inline constexpr uint32_t MakeColorBgra(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(g) << 8)  |
           (static_cast<uint32_t>(r));
}

} // namespace evo

#endif // EVO_COMMON_HPP
