#include "evo/screens/SettingsScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_toast.h"
#include "evo_theme.h"
#include "evo_boot_log.h"
#include "evo_prev_run.h"
#include "evo_keyboard.h"
#include "qrcodegen.h"
#include "evo_hw.h"
#include "evo_agc_runtime.h"
#include "evo_direct_mem.h"
#include "evo_updater.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef EVO_PLAYER_VERSION
#define EVO_PLAYER_VERSION "v0.13.6"
#endif

namespace evo {

namespace {

constexpr int kMaxDefs    = 16;
constexpr int kMaxOptions = 21 > EVO_THEME_MAX ? 21 : EVO_THEME_MAX;
constexpr int kMaxSlots   = kMaxDefs + kMaxOptions;

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
    SkipSegments,
    VideoDecoder,
    Upscaling,
    AiNetwork,
    RefreshRate,
    HdrOutput,
    HdrPeak,
    /* AUDIO */
    VolumeBoost,
    AudioDelay,
    NetworkBuffer,
    OutputChannels,
    StereoUpmix,
    AudioPassthrough,
    PhoneSpeaker,
    AudioLanguage,
    SurroundTest,
    NavigationSounds,
    /* SUBTITLES */
    AutoSubtitles,
    SubtitleLanguage,
    SubtitleFont,
    SubtitleSize,
    SubtitleColour,
    SubtitlePosition,
    SubtitleBackground,
    SecondaryPosition,
    SecondaryColour,
    /* INTERFACE & STORAGE */
    Theme,
    Lightbar,
    FoldersFirst,
    KeyboardInput,
    PhonePort,
    PhoneCast,
    CastNativePlayer,
    AskStream,
    AddonAutoPick,
    /* SYSTEM & DIAGNOSTICS */
    ReportProblem,
    ReportLastRun,
    DebugOverlay,
    DeveloperTools,
    ConsoleModel,
    MemoryMode,
    SoftwareUpdate,
    QuitEvo,
    /* EXPERIMENTAL */
    MotionSmoothing,
    MatchVideoRate,
    DeepBlacks,
    WebBrowser,
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
    int         preview;    /* EVO_RMLUI_PREVIEW_* (#158) */
    const char* benefit;    /* plain words, shown in the preview card */
};

struct PreviewSpec { SettingKey key; int kind; const char* benefit; };
const PreviewSpec kPreviews[] = {
    {SettingKey::DefaultAspect, EVO_RMLUI_PREVIEW_ASPECT,
     "Sets how video fills the screen: fit keeps its shape, fill crops the edges, stretch fills the screen and distorts."},
    {SettingKey::ResumePlayback, EVO_RMLUI_PREVIEW_RESUME, "Picks a film up where you stopped instead of from the start."},
    {SettingKey::SkipSegments, EVO_RMLUI_PREVIEW_INFO,
     "Off never offers a skip. Ask shows a Skip button during an intro or credits and Triangle presses it. Auto skips by itself once and says so."},
    {SettingKey::VideoDecoder, EVO_RMLUI_PREVIEW_DECODER,
     "Hardware decoding is smoother and cooler. Software is for odd formats."},
    {SettingKey::Upscaling, EVO_RMLUI_PREVIEW_UPSCALING,
     "Soft video below your screen's resolution looks crisper. Higher modes cost more GPU."},
    {SettingKey::DeepBlacks, EVO_RMLUI_PREVIEW_DEEP_BLACKS,
     "Dark scenes look deeper instead of washed out. Best on OLED and HDR screens."},
    {SettingKey::AiNetwork, EVO_RMLUI_PREVIEW_AI_NETWORK,
     "A bigger network gives a sharper picture but is heavier. Maximum is for PS5 Pro."},
    {SettingKey::MotionSmoothing, EVO_RMLUI_PREVIEW_MOTION,
     "Pans and sports look fluid. Films can look too smooth, and fast motion can show haloes."},
    {SettingKey::RefreshRate, EVO_RMLUI_PREVIEW_REFRESH,
     "24 fps film plays more evenly on a 120 Hz output. The TV goes black briefly on each switch."},
    {SettingKey::MatchVideoRate, EVO_RMLUI_PREVIEW_MATCH_RATE,
     "Outputs the video's own rate (24p or 50 Hz) so motion has no judder."},
    {SettingKey::HdrOutput, EVO_RMLUI_PREVIEW_HDR,
     "Highlights like the sun and lamps get brighter without clipping. Needs an HDR10 TV."},
    {SettingKey::HdrPeak, EVO_RMLUI_PREVIEW_INFO,
     "Fits bright HDR films to your TV's real peak, shot by shot for Dolby Vision, instead of leaving it to the TV. Set your TV's peak (about 1500 nits for an LG G5) and turn its dynamic tone mapping off."},
    {SettingKey::AudioLanguage, EVO_RMLUI_PREVIEW_INFO,
     "The audio track a film starts with when it has several languages. Auto takes the first one."},
    {SettingKey::SubtitleLanguage, EVO_RMLUI_PREVIEW_INFO,
     "The subtitles chosen when a film opens: embedded tracks first, then online subtitles from your Stremio addons. Forced (signs only) tracks are only picked when the audio is already in your language."},
    {SettingKey::VolumeBoost, EVO_RMLUI_PREVIEW_INFO,
     "Makes quiet files louder. Very loud passages are cut off cleanly instead of wrapping into noise. Has no effect on Dolby/DTS passthrough."},
    {SettingKey::AudioDelay, EVO_RMLUI_PREVIEW_INFO,
     "Shifts sound against picture to fix lip-sync on a TV, soundbar or Bluetooth speaker. Positive = sound later."},
    {SettingKey::NetworkBuffer, EVO_RMLUI_PREVIEW_INFO,
     "How far ahead network streams load. A bigger buffer rides out Wi-Fi drops but uses more memory, and very high bitrate files get less than you pick. Applies from the next file."},
    {SettingKey::OutputChannels, EVO_RMLUI_PREVIEW_CHANNELS,
     "Stereo mixes 5.1 and 7.1 down to two speakers or headphones."},
    {SettingKey::StereoUpmix, EVO_RMLUI_PREVIEW_INFO,
     "Extracts ambient sound and expands stereo tracks across your surround speakers (quadraphonic 4.0). Requires Output Channels set to Auto."},
    {SettingKey::AudioPassthrough, EVO_RMLUI_PREVIEW_PASSTHROUGH,
     "Sends Dolby and DTS untouched to your receiver over HDMI."},
    {SettingKey::PhoneSpeaker, EVO_RMLUI_PREVIEW_INFO,
     "Plays in-movie phone conversations and telephone voices through your DualSense controller speaker, just like in games."},
    {SettingKey::SurroundTest, EVO_RMLUI_PREVIEW_SURROUND,
     "Plays a tone through each speaker so you can check your layout."},
    {SettingKey::NavigationSounds, EVO_RMLUI_PREVIEW_NAV_SOUNDS, "A soft click when you move and press buttons."},
    {SettingKey::AutoSubtitles, EVO_RMLUI_PREVIEW_SUB_AUTO, "Loads subtitles on its own when a file has them."},
    {SettingKey::SubtitleFont, EVO_RMLUI_PREVIEW_SUB_FONT, "Picks the typeface for on-screen subtitles."},
    {SettingKey::SubtitleSize, EVO_RMLUI_PREVIEW_SUB_SIZE,
     "How big subtitles are. The size button in the player's subtitle menu changes it for one session."},
    {SettingKey::SubtitleColour, EVO_RMLUI_PREVIEW_SUB_TEXT_COLOUR,
     "The colour of the main subtitle line. Picture subtitles (Blu-ray, DVD) keep their own colours."},
    {SettingKey::SubtitlePosition, EVO_RMLUI_PREVIEW_SUB_LIFT,
     "Raises subtitles from the bottom, for TVs that cut the edge or films with text low in the frame."},
    {SettingKey::SubtitleBackground, EVO_RMLUI_PREVIEW_SUB_BACKGROUND,
     "What sits behind the text so it stays readable on bright scenes. Subtitles burned into a server transcode are part of the picture and cannot change."},
    {SettingKey::SecondaryPosition, EVO_RMLUI_PREVIEW_SUB_POS,
     "Where the second line of dialogue goes when two tracks are shown."},
    {SettingKey::SecondaryColour, EVO_RMLUI_PREVIEW_SUB_COLOUR, "A different colour keeps the two lines apart."},
    {SettingKey::Theme, EVO_RMLUI_PREVIEW_THEME, "Changes the colours of the whole app."},
    {SettingKey::Lightbar, EVO_RMLUI_PREVIEW_LIGHTBAR, "The DualSense light follows the theme accent."},
    {SettingKey::FoldersFirst, EVO_RMLUI_PREVIEW_FOLDERS_FIRST, "Lists folders above files in the USB browser."},
    {SettingKey::KeyboardInput, EVO_RMLUI_PREVIEW_KEYBOARD,
     "The virtual keyboard is drawn by EVO. The native one is the PS5 system keyboard."},
    {SettingKey::PhonePort, EVO_RMLUI_PREVIEW_PORT,
     "EVO checks the port is free before it saves it. If it is busy at start-up, EVO tries the next nine and shows the one it got."},
    {SettingKey::PhoneCast, EVO_RMLUI_PREVIEW_INFO,
     "Allows casting YouTube from your phone directly to EVO Player (DIAL receiver on port 8765)."},
    {SettingKey::CastNativePlayer, EVO_RMLUI_PREVIEW_INFO,
     "Plays YouTube casts in EVO Player (up to 4K) using hardware decoding, instead of the 720p WebKit browser."},
    {SettingKey::AskStream, EVO_RMLUI_PREVIEW_ASK_STREAM,
     "Lets you choose the format and quality each time you open a live channel."},
    {SettingKey::ReportProblem, EVO_RMLUI_PREVIEW_REPORT,
     "The report has EVO's log, version and settings. Passwords and tokens are taken out."},
    {SettingKey::ReportLastRun, EVO_RMLUI_PREVIEW_REPORT,
     "The log of the run before this one. Use it when EVO crashed or froze and you reopened it."},
    {SettingKey::AddonAutoPick, EVO_RMLUI_PREVIEW_ASK_STREAM,
     "Plays the best source at once: Remux and high bitrate, Dolby Vision or HDR, already cached on your debrid service. The full list is in OPTIONS on the title."},
    {SettingKey::DebugOverlay, EVO_RMLUI_PREVIEW_DEBUG_OVERLAY, "Shows live performance numbers on screen."},
    {SettingKey::DeveloperTools, EVO_RMLUI_PREVIEW_INFO, "System diagnostics and performance stats."},
    {SettingKey::ConsoleModel, EVO_RMLUI_PREVIEW_INFO,
     "The console EVO detected. Some features, like Maximum AI upscaling, need a PS5 Pro."},
    {SettingKey::MemoryMode, EVO_RMLUI_PREVIEW_INFO,
     "How much GPU memory EVO was given. A PS5 Pro gives about 1.2 GiB more to the Pro memory package. Standard mode is the normal amount."},
    {SettingKey::SoftwareUpdate, EVO_RMLUI_PREVIEW_INFO,
     "Checks GitHub for a newer EVO. It downloads only after you say so, checks the file against GitHub's checksum, and replaces the installed image just once, when you confirm. EVO then closes; open it again from the Games row."},
    {SettingKey::QuitEvo, EVO_RMLUI_PREVIEW_INFO, "Releases everything, then close EVO from the switcher."},
    {SettingKey::WebBrowser, EVO_RMLUI_PREVIEW_INFO,
     "Enables the built-in Prospero Web browser section on the navigation rail. Unfinished experimental feature."},
};

const SettingKey kVideoKeys[] = {
    SettingKey::DefaultAspect, SettingKey::ResumePlayback, SettingKey::SkipSegments, SettingKey::VideoDecoder,
    SettingKey::Upscaling, SettingKey::AiNetwork, SettingKey::RefreshRate,
    SettingKey::HdrOutput, SettingKey::HdrPeak, SettingKey::NetworkBuffer,
};
const SettingKey kAudioKeys[] = {
    SettingKey::VolumeBoost, SettingKey::AudioDelay,
    SettingKey::OutputChannels, SettingKey::StereoUpmix, SettingKey::AudioPassthrough,
    SettingKey::PhoneSpeaker, SettingKey::AudioLanguage,
    SettingKey::SurroundTest, SettingKey::NavigationSounds,
};
const SettingKey kSubtitleKeys[] = {
    SettingKey::AutoSubtitles, SettingKey::SubtitleLanguage, SettingKey::SubtitleFont,
    SettingKey::SubtitleSize, SettingKey::SubtitleColour, SettingKey::SubtitlePosition,
    SettingKey::SubtitleBackground, SettingKey::SecondaryPosition, SettingKey::SecondaryColour,
};
const SettingKey kInterfaceKeys[] = {
    SettingKey::Theme, SettingKey::Lightbar, SettingKey::FoldersFirst,
    SettingKey::KeyboardInput, SettingKey::PhonePort, SettingKey::PhoneCast, SettingKey::CastNativePlayer, SettingKey::AskStream, SettingKey::AddonAutoPick,
};
const SettingKey kSystemKeys[] = {
    SettingKey::ReportProblem, SettingKey::ReportLastRun, SettingKey::DebugOverlay, SettingKey::DeveloperTools,
    SettingKey::ConsoleModel, SettingKey::MemoryMode, SettingKey::SoftwareUpdate, SettingKey::QuitEvo,
};
/*
 * Things that work but are not finished. They stay out of the other sections so
 * nobody turns one on expecting a shipped feature, and they default to off.
 */
const SettingKey kExperimentalKeys[] = {
    SettingKey::MotionSmoothing, SettingKey::MatchVideoRate, SettingKey::DeepBlacks,
    SettingKey::WebBrowser,
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
    makeSection("Video & Display", "DECODER, UPSCALING, 120 HZ & HDR",
                "SettingsPlaybackScreen", ScreenId::SettingsPlayback, kVideoKeys),
    makeSection("Audio", "OUTPUT CHANNELS, SPEAKERS & SOUNDS",
                "SettingsAudioScreen", ScreenId::SettingsAudio, kAudioKeys),
    makeSection("Subtitles", "PREFERENCES & APPEARANCE",
                "SettingsSubtitlesScreen", ScreenId::SettingsSubtitles, kSubtitleKeys),
    makeSection("Interface & Storage", "THEMES, CONTROLS & BROWSING",
                "SettingsInterfaceScreen", ScreenId::SettingsInterface, kInterfaceKeys),
    makeSection("System & Diagnostics", "DIAGNOSTICS & SYSTEM MANAGEMENT",
                "SettingsSystemScreen", ScreenId::SettingsSystem, kSystemKeys),
    makeSection("Experimental", "UNFINISHED WORK - EXPECT ARTEFACTS",
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

void fillPortPreview(evo_rmlui_settings_row_t& row);
void fillReportPreview(evo_rmlui_settings_row_t& row, bool lastRun);

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

    case SettingKey::SkipSegments:
        d = valueDef(key, "SKIP INTROS & CREDITS", "OFF NEVER OFFERS A SKIP; ASK SHOWS A BUTTON; AUTO SKIPS ONCE",
                     "../icons/icon_resume.png", 3,
                     static_cast<int>(settings->getSkipSegments()));
        for (int i = 0; i < 3; ++i)
            d.opt_label[i] = settings->getSkipSegmentsName(static_cast<SkipSegments>(i));
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

    case SettingKey::DeepBlacks:
        d = valueDef(key, "DEEP BLACKS", "PULLS ELEVATED SHADOWS TO TRUE OLED BLACK",
                     "../icons/icon_sparkles.png", 3, static_cast<int>(settings->getDeepBlacks()));
        for (int i = 0; i < 3; ++i)
            d.opt_label[i] = settings->getDeepBlacksName(static_cast<DeepBlacks>(i));
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

    case SettingKey::MatchVideoRate:
        d = toggleDef(key, "MATCH VIDEO FRAME RATE",
                      "24P / 50 HZ OUTPUT DURING PLAYBACK - FALLS BACK TO 120 HZ OR 60 HZ",
                      "../icons/icon_gauge.png", settings->isMatchVideoRateEnabled());
        break;

    case SettingKey::WebBrowser:
        d = toggleDef(key, "WEB BROWSER", "BUILT-IN PROSPERO BROWSER ON THE NAVIGATION RAIL",
                      "../icons/icon_globe.png", settings->isWebBrowserEnabled());
        break;

    case SettingKey::HdrOutput:
        d = valueDef(key, "HDR OUTPUT", "HDR10 FOR HDR VIDEO - THE TV GOES BLACK BRIEFLY AT START & END",
                     "../icons/icon_sun.png", 2, static_cast<int>(settings->getHdrOutputMode()));
        for (int i = 0; i < 2; ++i)
            d.opt_label[i] = settings->getHdrOutputModeName(static_cast<HdrOutputMode>(i));
        break;

    case SettingKey::HdrPeak:
        d = valueDef(key, "HDR PEAK", "FIT HDR & DOLBY VISION TO YOUR TV'S PEAK - OFF LEAVES IT TO THE TV",
                     "../icons/icon_sun.png", 6, static_cast<int>(settings->getHdrPeak()));
        for (int i = 0; i < 6; ++i)
            d.opt_label[i] = settings->getHdrPeakName(static_cast<HdrPeak>(i));
        break;

    /* #199: sized when a network file opens, so it applies from the next file. */
    case SettingKey::NetworkBuffer:
        d = valueDef(key, "NETWORK BUFFER", "HOW FAR AHEAD STREAMS LOAD. HIGH BITRATE FILES GET LESS",
                     "../icons/icon_gauge.png", 4,
                     static_cast<int>(settings->getNetworkBuffer()));
        for (int i = 0; i < 4; ++i)
            d.opt_label[i] = settings->getNetworkBufferName(static_cast<NetworkBuffer>(i));
        break;

    case SettingKey::VolumeBoost:
        d = valueDef(key, "VOLUME BOOST", "RAISE QUIET FILES. LOUD PASSAGES CLIP, NEVER WRAP",
                     "../icons/icon_volume.png", 5,
                     static_cast<int>(settings->getVolumeBoost()));
        for (int i = 0; i < 5; ++i)
            d.opt_label[i] = settings->getVolumeBoostName(static_cast<VolumeBoost>(i));
        break;

    case SettingKey::AudioDelay: {
        static const char* const kLabels[21] = {
            "-500 ms", "-450 ms", "-400 ms", "-350 ms", "-300 ms",
            "-250 ms", "-200 ms", "-150 ms", "-100 ms", "-50 ms",
            "0 ms",
            "+50 ms", "+100 ms", "+150 ms", "+200 ms", "+250 ms",
            "+300 ms", "+350 ms", "+400 ms", "+450 ms", "+500 ms"
        };
        int delay = settings->getAudioDelayMs();
        int opt = (delay / 50) + 10;
        if (opt < 0) opt = 0;
        if (opt > 20) opt = 20;
        d = valueDef(key, "AUDIO DELAY", "SHIFTS SOUND AGAINST PICTURE TO FIX LIP-SYNC",
                     "../icons/icon_volume.png", 21, opt);
        for (int i = 0; i < 21; ++i)
            d.opt_label[i] = kLabels[i];
        break;
    }

    /* #117: read when a file opens, so it applies from the next file. */
    case SettingKey::OutputChannels:
        d = valueDef(key, "OUTPUT CHANNELS", "STEREO DOWNMIXES 5.1 & 7.1 TO 2.0",
                     "../icons/icon_volume.png", 2,
                     static_cast<int>(settings->getAudioOutputChannels()));
        for (int i = 0; i < 2; ++i)
            d.opt_label[i] = settings->getAudioOutputChannelsName(static_cast<AudioOutputChannels>(i));
        break;

    case SettingKey::StereoUpmix:
        d = toggleDef(key, "UPMIX STEREO TO 4.0",
                      "EXPANDS 2.0 STEREO TO 4.0 QUADRAPHONIC SURROUND",
                      "../icons/icon_speaker.png", settings->isStereoUpmixEnabled());
        if (settings->getAudioOutputChannels() != AudioOutputChannels::Auto) {
            d.disabled = true;
            d.detail = "ONLY AVAILABLE WHEN OUTPUT CHANNELS IS SET TO AUTO";
        }
        break;

    case SettingKey::AudioLanguage:
        d = valueDef(key, "AUDIO LANGUAGE", "THE TRACK A FILM STARTS WITH",
                     "../icons/icon_volume.png", kPreferredLanguageCount,
                     static_cast<int>(settings->getAudioLanguage()));
        for (int i = 0; i < kPreferredLanguageCount; ++i)
            d.opt_label[i] = settings->getPreferredLanguageName(static_cast<PreferredLanguage>(i));
        break;

    case SettingKey::SubtitleLanguage:
        d = valueDef(key, "SUBTITLE LANGUAGE", "EMBEDDED FIRST, THEN ONLINE FROM YOUR ADDONS",
                     "../icons/icon_subtitles.png", kPreferredLanguageCount,
                     static_cast<int>(settings->getSubtitleLanguage()));
        for (int i = 0; i < kPreferredLanguageCount; ++i)
            d.opt_label[i] = settings->getPreferredLanguageName(static_cast<PreferredLanguage>(i));
        break;

    case SettingKey::AudioPassthrough:
        d = toggleDef(key, "AUDIO PASSTHROUGH", "BITSTREAM DOLBY & DTS TO RECEIVER OVER HDMI",
                      "../icons/icon_volume.png", settings->isAudioPassthroughEnabled());
        break;

    case SettingKey::PhoneSpeaker:
        d = toggleDef(key, "CONTROLLER PHONE AUDIO",
                      "ROUTES IN-MOVIE PHONE CALLS TO THE DUALSENSE SPEAKER",
                      "../icons/icon_volume.png", settings->isPhoneSpeakerEnabled());
        if (settings->isAudioPassthroughEnabled()) {
            d.disabled = true;
            d.detail = "ONLY AVAILABLE WHEN AUDIO PASSTHROUGH IS DISABLED";
        }
        break;

    case SettingKey::SurroundTest:
        d = actionDef(key, "SURROUND SOUND TEST", "5.1 & 7.1 SPEAKER CHANNEL VERIFICATION",
                      "../icons/icon_speaker.png", "OPEN");
        break;

    case SettingKey::NavigationSounds:
        d = toggleDef(key, "NAVIGATION SOUNDS", "AUDIO FEEDBACK ON D-PAD & BUTTONS",
                      "../icons/icon_speaker.png", settings->isSoundFeedbackEnabled());
        break;

    case SettingKey::AutoSubtitles:
        d = toggleDef(key, "AUTO SUBTITLES", "AUTOMATICALLY LOAD SUBTITLES ON PLAYBACK",
                      "../icons/icon_subtitles.png", settings->isAutoSubtitlesEnabled());
        break;

    case SettingKey::SubtitleFont: {
        /* Order is prospero_subtitle_style; evo_hui_osd.cpp caption_font draws them. */
        static const char* kFaces[] = {"STANDARD", "LIGHT", "ROUNDED", "BOLD"};
        int face = settings->getSubtitleFontFace();
        if (face < 0 || face >= 4) face = 0;
        d = valueDef(key, "DEFAULT FONT STYLE", "SUBTITLE TYPEFACE",
                     "../icons/icon_type.png", 4, face);
        for (int i = 0; i < 4; ++i) d.opt_label[i] = kFaces[i];
        break;
    }

    /* #231: the main caption's look. */
    case SettingKey::SubtitleSize: {
        static const char* kSizes[] = {"SMALL", "MEDIUM", "LARGE", "EXTRA LARGE"};
        int v = settings->getSubtitleSize();
        if (v < 0 || v >= 4) v = 1;
        d = valueDef(key, "SUBTITLE SIZE", "HOW BIG SUBTITLES ARE",
                     "../icons/icon_type.png", 4, v);
        for (int i = 0; i < 4; ++i) d.opt_label[i] = kSizes[i];
        break;
    }

    case SettingKey::SubtitleColour: {
        static const char* kColours[] = {"WHITE", "YELLOW", "CYAN", "GREEN"};
        int v = settings->getSubtitleColor();
        if (v < 0 || v >= 4) v = 0;
        d = valueDef(key, "SUBTITLE COLOUR", "THE MAIN LINE OF DIALOGUE",
                     "../icons/icon_palette.png", 4, v);
        for (int i = 0; i < 4; ++i) d.opt_label[i] = kColours[i];
        break;
    }

    case SettingKey::SubtitlePosition: {
        static const char* kLifts[] = {"BOTTOM", "RAISED", "HIGHER"};
        int v = settings->getSubtitleLift();
        if (v < 0 || v >= 3) v = 0;
        d = valueDef(key, "SUBTITLE POSITION", "RAISE SUBTITLES FROM THE BOTTOM",
                     "../icons/icon_subtitles.png", 3, v);
        for (int i = 0; i < 3; ++i) d.opt_label[i] = kLifts[i];
        break;
    }

    case SettingKey::SubtitleBackground: {
        static const char* kBackgrounds[] = {"BOX", "SHADOW", "OUTLINE", "NONE"};
        int v = settings->getSubtitleBackground();
        if (v < 0 || v >= 4) v = 0;
        d = valueDef(key, "SUBTITLE BACKGROUND", "KEEPS TEXT READABLE ON BRIGHT SCENES",
                     "../icons/icon_subtitles.png", 4, v);
        for (int i = 0; i < 4; ++i) d.opt_label[i] = kBackgrounds[i];
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

    case SettingKey::PhonePort: {
        static char s_port_badge[16];
        const int actual = evo_log_server_port();
        if (actual > 0) std::snprintf(s_port_badge, sizeof s_port_badge, "%d", actual);
        else std::snprintf(s_port_badge, sizeof s_port_badge, "NONE");
        d = actionDef(key, "PHONE & LOG PORT",
                      actual > 0 ? "PHONE KEYBOARD LINK AND LIVE LOG - CROSS TO CHANGE"
                                 : "NO FREE PORT - CROSS TO SET A NEW ONE",
                      "../icons/icon_keyboard.png", s_port_badge);
        break;
    }

    case SettingKey::PhoneCast:
        d = toggleDef(key, "PHONE CASTING (DIAL)", "CAST YOUTUBE FROM YOUR PHONE (PORT 8765)",
                      "../icons/icon_tv.png", settings->isCastReceiverEnabled());
        break;

    case SettingKey::CastNativePlayer:
        d = toggleDef(key, "CAST TO EVO PLAYER", "PLAY CAST IN NATIVE PLAYER (UP TO 4K)",
                      "../icons/icon_play.png", settings->isCastNativePlayerEnabled());
        break;

    case SettingKey::AskStream:
        d = toggleDef(key, "ASK WHICH LIVE STREAM", "PICK THE FORMAT AND QUALITY WHEN YOU OPEN A CHANNEL",
                      "../icons/icon_tv.png", settings->isAskStreamEnabled());
        break;

    case SettingKey::AddonAutoPick:
        d = toggleDef(key, "PLAY BEST ADDON STREAM", "REMUX, DOLBY VISION / HDR, CACHED FIRST - LIST IN OPTIONS",
                      "../icons/icon_tv.png", settings->isAddonAutoPickEnabled());
        break;

    case SettingKey::ReportProblem:
        d = actionDef(key, "REPORT A PROBLEM", "SEND EVO'S LOG FROM YOUR PHONE, OR SAVE IT TO USB",
                      "../icons/icon_report.png", "SAVE");
        break;

    case SettingKey::ReportLastRun: {
        /* #229: after a crash the report to send is the previous run's. */
        const evo_prev_run_t prev = evo_prev_run_state();
        d = actionDef(key, "REPORT THE LAST RUN",
                      prev == EVO_PREV_RUN_CRASHED ? "THE LAST RUN CRASHED - SEND ITS LOG"
                      : prev == EVO_PREV_RUN_ENDED ? "THE LAST RUN ENDED WITHOUT CLOSING"
                      : prev == EVO_PREV_RUN_CLOSED ? "THE LAST RUN CLOSED NORMALLY"
                                                     : "NO EARLIER RUN SAVED",
                      "../icons/icon_report.png", prev == EVO_PREV_RUN_NONE ? "NONE" : "SAVE");
        break;
    }

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

    /* #143: read-only row showing GPU memory budget & mode. */
    case SettingKey::MemoryMode: {
        /*
         * Direct memory mode threshold (#143):
         * Measured on the PS5 Pro (2026-10-22) from boot line "mem budget [boot] direct_total=...":
         *   standard build  direct_total = 11712 MB
         *   Pro memory mode direct_total = 12928 MB   (attribute3 bit 22 set)
         * Pool > 12288 MB (12 GiB) indicates Pro memory mode.
         */
        constexpr long long kProMemoryThresholdMb = 12288;
        static char s_mem_badge[32];
        const long long pool = evo_direct_mem_kernel_total_mb();
        if (pool > kProMemoryThresholdMb) {
            std::snprintf(s_mem_badge, sizeof s_mem_badge, "%.1f GIB - PRO MODE",
                          static_cast<double>(pool) / 1024.0);
        } else if (pool > 0) {
            std::snprintf(s_mem_badge, sizeof s_mem_badge, "%.1f GIB - STANDARD",
                          static_cast<double>(pool) / 1024.0);
        } else {
            std::snprintf(s_mem_badge, sizeof s_mem_badge, "UNKNOWN");
        }
        /* The kit draws an action row as a chevron with no badge, so the value
         * goes in the subtitle line, where it is visible. On a Pro in standard
         * mode the line also points at the Pro build (EVO cannot switch itself). */
        static char s_mem_sub[96];
        /* Pro detection misses on the Pro (see Console row), so the hint cannot
         * wait for evo_hw_is_ps5_pro(); it is worded for a Pro owner instead. */
        if (pool > 0 && pool <= kProMemoryThresholdMb)
            std::snprintf(s_mem_sub, sizeof s_mem_sub,
                          "%s - PS5 PRO: GET THE PRO BUILD FOR +1.2 GIB", s_mem_badge);
        else
            std::snprintf(s_mem_sub, sizeof s_mem_sub, "%s", s_mem_badge);
        d = actionDef(key, "MEMORY", s_mem_sub, "../icons/icon_activity.png",
                      s_mem_badge);
        break;
    }

    /* #120: one row walks check -> download -> install; the state is in the subtitle. */
    case SettingKey::SoftwareUpdate: {
        static char s_upd_sub[200];
        evo_updater_status_t us;
        evo_updater_get(&us);
        const char* ver = (EVO_PLAYER_VERSION[0] == 'v' || EVO_PLAYER_VERSION[0] == 'V')
                              ? EVO_PLAYER_VERSION : "v" EVO_PLAYER_VERSION;
        switch (us.state) {
        case EVO_UPD_CHECKING:
            std::snprintf(s_upd_sub, sizeof s_upd_sub, "CHECKING GITHUB...");
            break;
        case EVO_UPD_UP_TO_DATE:
            std::snprintf(s_upd_sub, sizeof s_upd_sub, "RUNNING %s - UP TO DATE. CROSS TO CHECK AGAIN", ver);
            break;
        case EVO_UPD_AVAILABLE:
            std::snprintf(s_upd_sub, sizeof s_upd_sub, "%s AVAILABLE (%llu MB) - CROSS TO DOWNLOAD",
                          us.latest, static_cast<unsigned long long>(us.total >> 20));
            break;
        case EVO_UPD_DOWNLOADING:
            std::snprintf(s_upd_sub, sizeof s_upd_sub, "DOWNLOADING %s: %d%% - CROSS TO CANCEL", us.latest,
                          us.total ? static_cast<int>(us.done * 100 / us.total) : 0);
            break;
        case EVO_UPD_READY:
            std::snprintf(s_upd_sub, sizeof s_upd_sub, "%s VERIFIED - CROSS TO INSTALL AND CLOSE EVO", us.latest);
            break;
        case EVO_UPD_APPLIED:
            std::snprintf(s_upd_sub, sizeof s_upd_sub, "INSTALLED - OPEN EVO AGAIN FROM THE GAMES ROW");
            break;
        case EVO_UPD_FAILED:
            std::snprintf(s_upd_sub, sizeof s_upd_sub, "%s - CROSS TO TRY AGAIN", us.error);
            break;
        default:
            std::snprintf(s_upd_sub, sizeof s_upd_sub, "RUNNING %s - CROSS TO CHECK FOR UPDATES", ver);
            break;
        }
        for (char* c = s_upd_sub; *c; ++c)
            if (*c >= 'a' && *c <= 'z') *c = static_cast<char>(*c - 'a' + 'A');
        d = actionDef(key, "SOFTWARE UPDATE", s_upd_sub, "../icons/icon_activity.png", "UPDATE");
        break;
    }

    case SettingKey::QuitEvo:
        d = actionDef(key, "QUIT EVO", "RELEASE EVERYTHING, THEN CLOSE FROM THE SWITCHER",
                      "../icons/icon_power.png", "QUIT");
        break;
    }

    for (const PreviewSpec& ps : kPreviews)
        if (ps.key == key) { d.preview = ps.kind; d.benefit = ps.benefit; break; }

    /* #120: once a newer release is known, the preview pane says what is new in it. */
    if (key == SettingKey::SoftwareUpdate) {
        evo_updater_status_t us;
        evo_updater_get(&us);
        static char s_upd_new[640];
        if (us.notes[0]) {
            char plain[560];
            evo_updater_plain_notes(us.notes, plain, sizeof plain);
            /* One feature per line, and no feature cut off in the middle. */
            if (std::strlen(plain) > 500) {
                if (char* cut = std::strrchr(plain, '-')) { if (cut > plain) cut[-1] = '\0'; }
            }
            std::string lines;
            for (const char* q = plain; *q; ++q) {
                if (q[0] == ' ' && q[1] == '-' && q[2] == ' ') { lines += "\n- "; q += 2; }
                else lines += *q;
            }
            std::snprintf(s_upd_new, sizeof s_upd_new, "WHAT'S NEW IN %s\n%s", us.latest, lines.c_str());
            d.benefit = s_upd_new;
        }
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
    for (int i = 0; i < n && m < kMaxSlots; ++i) {
        out[m].def = i; out[m].opt = -1; ++m;
        if (i == expanded && d[i].kind == EVO_RMLUI_ROW_VALUE && !d[i].disabled) {
            for (int o = 0; o < d[i].opt_count && m < kMaxSlots; ++o) {
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
    DisplaySlot all_slots[kMaxSlots];
    const int n = buildSectionDefs(section, d);
    const int total = buildSlots(d, n, expanded, all_slots);

    if (section >= 0 && section < kSectionCount) {
        p.title = kSections[section].title;
        p.subtitle = kSections[section].subtitle;
    }

    int window_start = 0;
    if (total > EVO_RMLUI_SETTINGS_ROWS) {
        if (cursor >= 0) {
            window_start = cursor - (EVO_RMLUI_SETTINGS_ROWS / 2);
            if (window_start + EVO_RMLUI_SETTINGS_ROWS > total)
                window_start = total - EVO_RMLUI_SETTINGS_ROWS;
            if (window_start < 0)
                window_start = 0;
        }
    }
    const int visible_count = (total < EVO_RMLUI_SETTINGS_ROWS) ? total : EVO_RMLUI_SETTINGS_ROWS;

    for (int i = 0; i < visible_count; ++i) {
        const int slot_idx = window_start + i;
        const SettingDef& def = d[all_slots[slot_idx].def];
        const int opt = all_slots[slot_idx].opt;
        evo_rmlui_settings_row_t& row = p.rows[i];
        row.preview = def.preview;
        row.benefit = def.benefit ? def.benefit : "";
        row.choice_count = (def.kind == EVO_RMLUI_ROW_VALUE) ? def.opt_count : 0;
        if (row.choice_count > EVO_RMLUI_PREVIEW_CHOICES) row.choice_count = EVO_RMLUI_PREVIEW_CHOICES;
        for (int c = 0; c < row.choice_count; ++c) row.choice[c] = def.opt_label[c];
        if (def.key == SettingKey::Theme) {
            for (int c = 0; c < row.choice_count; ++c) {
                const evo_theme* t = evo_theme_at(c);
                if (!t) continue;
                auto rgb = [](uint32_t v) { return (v & 0xffu) << 16 | (v & 0xff00u) | ((v >> 16) & 0xffu); };
                row.choice_bg[c] = rgb(t->bg_top);
                row.choice_accent[c] = rgb(t->accent);
            }
        }
        row.preview_index = (def.kind == EVO_RMLUI_ROW_TOGGLE) ? (def.toggle_on ? 1 : 0)
                          : (opt >= 0) ? opt : def.opt_current;
        if (def.key == SettingKey::PhonePort) fillPortPreview(row);
        if (def.key == SettingKey::ReportProblem) fillReportPreview(row, false);
        if (def.key == SettingKey::ReportLastRun) fillReportPreview(row, true);
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
        p.rows[i].is_focused = (slot_idx == cursor) ? 1 : 0;
    }

    static char s_counter[32];
    std::snprintf(s_counter, sizeof s_counter, "%d SETTING%s", n, n == 1 ? "" : "S");
    p.counter = s_counter;
    p.row_count = visible_count;
    return visible_count;
}

void toggleSetting(SettingKey key) {
    ISettingsService* st = Application::getInstance().getSettingsService();
    if (!st) return;
    switch (key) {
    case SettingKey::ResumePlayback:   st->setResumePlaybackEnabled(!st->isResumePlaybackEnabled()); break;
    case SettingKey::AudioPassthrough: st->setAudioPassthroughEnabled(!st->isAudioPassthroughEnabled()); break;
    case SettingKey::StereoUpmix:      st->setStereoUpmixEnabled(!st->isStereoUpmixEnabled()); break;
    case SettingKey::PhoneSpeaker:     st->setPhoneSpeakerEnabled(!st->isPhoneSpeakerEnabled()); break;
    case SettingKey::MatchVideoRate:   st->setMatchVideoRateEnabled(!st->isMatchVideoRateEnabled()); break;
    case SettingKey::WebBrowser:
        st->setWebBrowserEnabled(!st->isWebBrowserEnabled());
        if (auto sm = Application::getInstance().getScreenManager()) {
            if (!st->isWebBrowserEnabled() &&
                (sm->getCurrentScreenId() == ScreenId::WebHome ||
                 sm->getCurrentScreenId() == ScreenId::WebPage)) {
                sm->navigateTo(ScreenId::MainMenu);
            }
            sm->syncNavRail();
        }
        break;
    case SettingKey::PhoneCast:
        st->setCastReceiverEnabled(!st->isCastReceiverEnabled());
        toast("PHONE CASTING", st->isCastReceiverEnabled() ? "Receiver started (port 8765)" : "Receiver stopped");
        break;
    case SettingKey::CastNativePlayer:
        st->setCastNativePlayerEnabled(!st->isCastNativePlayerEnabled());
        toast("CAST PLAYER", st->isCastNativePlayerEnabled() ? "EVO Player (up to 4K)" : "System WebKit (720p)");
        break;
    case SettingKey::AskStream:        st->setAskStreamEnabled(!st->isAskStreamEnabled()); break;
    case SettingKey::AddonAutoPick:    st->setAddonAutoPickEnabled(!st->isAddonAutoPickEnabled()); break;
    case SettingKey::AutoSubtitles:    st->setAutoSubtitlesEnabled(!st->isAutoSubtitlesEnabled()); break;
    case SettingKey::NavigationSounds: st->setSoundFeedbackEnabled(!st->isSoundFeedbackEnabled()); break;
    case SettingKey::Lightbar:         st->setLightbarFeedbackEnabled(!st->isLightbarFeedbackEnabled()); break;
    case SettingKey::FoldersFirst:
        st->setSortFoldersFirst(!st->isSortFoldersFirst());
        if (auto fs = Application::getInstance().getFileSystemBrowser())
            fs->setSortFoldersFirst(st->isSortFoldersFirst());
        break;
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
    case SettingKey::SkipSegments:   st->setSkipSegments(static_cast<SkipSegments>(opt)); break;
    case SettingKey::VideoDecoder:   st->setVideoDecoderPreference(static_cast<DecoderPreference>(opt)); break;
    case SettingKey::Upscaling:      st->setUpscaler(static_cast<Upscaler>(opt)); break;
    case SettingKey::AiNetwork:      st->setAiNetwork(static_cast<AiNetwork>(opt)); break;
    case SettingKey::DeepBlacks:     st->setDeepBlacks(static_cast<DeepBlacks>(opt)); break;
    case SettingKey::MotionSmoothing: st->setMotionSmoothing(static_cast<MotionSmoothing>(opt)); break;
    case SettingKey::HdrOutput:      st->setHdrOutputMode(static_cast<HdrOutputMode>(opt)); break;
    case SettingKey::HdrPeak:        st->setHdrPeak(static_cast<HdrPeak>(opt)); break;
    case SettingKey::AudioLanguage:  st->setAudioLanguage(static_cast<PreferredLanguage>(opt)); break;
    case SettingKey::SubtitleLanguage: st->setSubtitleLanguage(static_cast<PreferredLanguage>(opt)); break;
    case SettingKey::NetworkBuffer:  st->setNetworkBuffer(static_cast<NetworkBuffer>(opt)); break;
    case SettingKey::VolumeBoost:    st->setVolumeBoost(static_cast<VolumeBoost>(opt)); break;
    case SettingKey::AudioDelay:     st->setAudioDelayMs((opt - 10) * 50); break;
    case SettingKey::OutputChannels: st->setAudioOutputChannels(static_cast<AudioOutputChannels>(opt)); break;
    case SettingKey::SubtitleFont:   st->setSubtitleFontFace(opt); break;
    case SettingKey::SubtitleSize:   st->setSubtitleSize(opt); break;
    case SettingKey::SubtitleColour: st->setSubtitleColor(opt); break;
    case SettingKey::SubtitlePosition: st->setSubtitleLift(opt); break;
    case SettingKey::SubtitleBackground: st->setSubtitleBackground(opt); break;
    case SettingKey::SecondaryPosition: st->setSecondarySubtitlePosition(opt); break;
    case SettingKey::SecondaryColour:   st->setSecondarySubtitleColor(opt); break;
    case SettingKey::KeyboardInput:  st->setKeyboardType(opt); break;
    case SettingKey::RefreshRate:
        st->setRefreshRateMode(static_cast<RefreshRateMode>(opt));
        /* Off or PlaybackOnly run the default rate here: SettingsScreen is
         * not player mode. */
        if (opt == static_cast<int>(RefreshRateMode::Always))
            evo_agc_runtime_set_120hz(1);
        else
            evo_agc_runtime_set_output_rate(EVO_VO_RATE_DEFAULT);
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

void saveSettings();
/* The port a typed number was refused for (it was busy); 0 = nothing refused. */
int s_portRefused = 0;

/* The preview card of PHONE & LOG PORT: the link, and one line on how the port went. */
void fillPortPreview(evo_rmlui_settings_row_t& row) {
    static char s_link[96], s_status[200];
    const int state = evo_log_server_state();
    const int actual = evo_log_server_port();
    const int saved = evo_log_server_saved_port();
    char ip[32];
    evo_keyboard_local_ip(ip, sizeof ip);
    if (actual > 0 && ip[0]) std::snprintf(s_link, sizeof s_link, "http://%s:%d/input", ip, actual);
    else s_link[0] = '\0';

    row.preview_index = 0;
    if (state == EVO_LOGSRV_NONE) {
        row.preview_index = 3;
        std::snprintf(s_status, sizeof s_status,
                      "Ports %d to %d are all taken, so the phone keyboard and live log are off. Please set a new port.",
                      saved, saved + 9);
    } else if (s_portRefused > 0) {
        row.preview_index = 1;
        std::snprintf(s_status, sizeof s_status, "%d is in use, kept %d", s_portRefused, saved);
    } else if (state == EVO_LOGSRV_FALLBACK) {
        row.preview_index = 2;
        std::snprintf(s_status, sizeof s_status, "%d is busy, using %d", saved, actual);
    } else if (state == EVO_LOGSRV_SAVED) {
        std::snprintf(s_status, sizeof s_status, "Listening on %d", actual);
    } else {
        std::snprintf(s_status, sizeof s_status, "Starting...");
    }
    /* The real QR of the link the card shows, rebuilt only when the link changes. */
    static char s_qr_for[96];
    static unsigned char s_qr_mod[(4 * 5 + 17) * (4 * 5 + 17)];
    static int s_qr_n;
    if (std::strcmp(s_qr_for, s_link) != 0) {
        std::snprintf(s_qr_for, sizeof s_qr_for, "%s", s_link);
        s_qr_n = 0;
        if (s_link[0]) {
            static uint8_t s_qr[qrcodegen_BUFFER_LEN_FOR_VERSION(5)];
            uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(5)];
            if (qrcodegen_encodeText(s_link, tmp, s_qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, 5,
                                     qrcodegen_Mask_AUTO, true)) {
                s_qr_n = qrcodegen_getSize(s_qr);
                for (int y = 0; y < s_qr_n; y++)
                    for (int x = 0; x < s_qr_n; x++)
                        s_qr_mod[y * s_qr_n + x] = qrcodegen_getModule(s_qr, x, y) ? 1 : 0;
            }
        }
    }
    row.qr_size = s_qr_n;
    row.qr_modules = s_qr_n ? s_qr_mod : nullptr;
    row.choice_count = 2;
    row.choice[0] = s_link;
    row.choice[1] = s_status;
}

/* The preview card of REPORT A PROBLEM (#229): the phone link to /report and
 * where Cross saves the file. */
void fillReportPreview(evo_rmlui_settings_row_t& row, bool lastRun) {
    static char s_link[96], s_status[160];
    const int actual = evo_log_server_port();
    char ip[32];
    evo_keyboard_local_ip(ip, sizeof ip);
    if (actual > 0 && ip[0]) {
        std::snprintf(s_link, sizeof s_link, "http://%s:%d/report%s", ip, actual, lastRun ? "?run=last" : "");
        std::snprintf(s_status, sizeof s_status,
                      "The phone must be on the same network as the PS5. EVO has to stay open.");
        row.preview_index = 0;
    } else {
        s_link[0] = '\0';
        std::snprintf(s_status, sizeof s_status, "%s",
                      actual > 0 ? "No network: the phone link is off. Save the report to USB instead."
                                 : "The phone link is off (no free port, see PHONE & LOG PORT). Save the report to USB instead.");
        row.preview_index = 1;
    }
    static char s_qr_for[96];
    static unsigned char s_qr_mod[(4 * 5 + 17) * (4 * 5 + 17)];
    static int s_qr_n;
    if (std::strcmp(s_qr_for, s_link) != 0) {
        std::snprintf(s_qr_for, sizeof s_qr_for, "%s", s_link);
        s_qr_n = 0;
        if (s_link[0]) {
            static uint8_t s_qr[qrcodegen_BUFFER_LEN_FOR_VERSION(5)];
            uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(5)];
            if (qrcodegen_encodeText(s_link, tmp, s_qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, 5,
                                     qrcodegen_Mask_AUTO, true)) {
                s_qr_n = qrcodegen_getSize(s_qr);
                for (int y = 0; y < s_qr_n; y++)
                    for (int x = 0; x < s_qr_n; x++)
                        s_qr_mod[y * s_qr_n + x] = qrcodegen_getModule(s_qr, x, y) ? 1 : 0;
            }
        }
    }
    row.qr_size = s_qr_n;
    row.qr_modules = s_qr_n ? s_qr_mod : nullptr;
    row.choice_count = 2;
    row.choice[0] = s_link;
    row.choice[1] = s_status;
}

void onPortEntered(const char* text, void*) {
    const int port = std::atoi(text);
    const int rc = evo_log_server_try_port(port);
    if (rc == 0) {
        s_portRefused = 0;
        saveSettings();
        toast("PHONE & LOG PORT", "Saved");
    } else {
        s_portRefused = port;
        toast("PHONE & LOG PORT", rc == EADDRINUSE ? "That port is in use" : "Not a valid port");
    }
}

void resetPort() {
    s_portRefused = 0;
    evo_log_server_set_port(EVO_LOG_SERVER_PORT);
    saveSettings();
    evo_feedback(EVO_FB_CONFIRM);
    toast("PHONE & LOG PORT", "Reset to 9780");
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
    case SettingKey::PhonePort: {
        char init[8];
        std::snprintf(init, sizeof init, "%d", evo_log_server_saved_port());
        evo_feedback(EVO_FB_OPEN);
        evo_keyboard_open_number("PHONE & LOG PORT (1024-65535)", init, 5, onPortEntered, nullptr);
        return true;
    }
    case SettingKey::SoftwareUpdate: {
        evo_updater_status_t us;
        evo_updater_get(&us);
        evo_feedback(EVO_FB_CONFIRM);
        switch (us.state) {
        case EVO_UPD_AVAILABLE:
            evo_updater_download();
            break;
        case EVO_UPD_DOWNLOADING:
            evo_updater_cancel();
            break;
        case EVO_UPD_READY:
            if (evo_updater_apply()) {
                toast("EVO UPDATED", "Closing. Open EVO again from the Games row");
                evo_boot_log("settings: update installed - closing");
                evo_boot_log_flush();
                Application::getInstance().requestCloseAndExit();
            }
            break;
        case EVO_UPD_CHECKING:
        case EVO_UPD_APPLIED:
            break;
        default:
            evo_updater_check(evo_direct_mem_kernel_total_mb() > 12288);
            break;
        }
        return true;
    }
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
        Application::getInstance().requestCloseAndExit();
        return true;
    case SettingKey::ReportProblem: {
        /* #229: the same report the phone page gives, as a file. A stick if
         * there is one; otherwise EVO's data folder, readable over FTP. */
        char path[160];
        if (evo_report_save(path, sizeof path)) {
            toast("REPORT SAVED", std::strncmp(path, "/mnt/usb0/", 10) == 0
                                      ? "evo_report.txt on the USB stick" : path);
            evo_feedback(EVO_FB_CONFIRM);
        } else {
            toast("REPORT", "Could not save. Use the phone link instead");
            evo_feedback(EVO_FB_BOUNDARY);
        }
        return true;
    }
    case SettingKey::ReportLastRun: {
        if (evo_prev_run_state() == EVO_PREV_RUN_NONE) {
            toast("REPORT", "No earlier run is saved");
            evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        char path[160];
        if (evo_report_save_run(path, sizeof path, 1)) {
            toast("LAST RUN SAVED", std::strncmp(path, "/mnt/usb0/", 10) == 0
                                        ? "evo_report_last_run.txt on the USB stick" : path);
            evo_feedback(EVO_FB_CONFIRM);
        } else {
            toast("REPORT", "Could not save. Use the phone link instead");
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

/* #201: `focus <row>` asks the open section page to put its cursor on this key. */
int s_pendingFocus = -1;

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

static bool s_focusUpdateRow = false;
void settingsFocusUpdateRow() { s_focusUpdateRow = true; }

void SettingsSectionScreen::onEnter() {
    StatefulScreen::onEnter();
    m_selectedIndex = 0;
    m_expandedIndex = -1;
    if (s_focusUpdateRow && m_id == ScreenId::SettingsSystem) {
        for (int i = 0; i < kSections[m_section].count; ++i)
            if (kSections[m_section].keys[i] == SettingKey::SoftwareUpdate) m_selectedIndex = i;
    }
    s_focusUpdateRow = false;
}

void SettingsSectionScreen::navigate(int delta) {
    SettingDef d[kMaxDefs];
    DisplaySlot slots[kMaxSlots];
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
    DisplaySlot slots[kMaxSlots];
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
            /* The cursor may sit on one of the choices; those slots vanish on
             * collapse, so return it to the setting that was open. */
            m_selectedIndex = m_expandedIndex;
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
        DisplaySlot slots[kMaxSlots];
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
    if (pressed & PadButtons::Triangle) {
        SettingDef d[kMaxDefs];
        DisplaySlot slots[kMaxSlots];
        const int m = buildSlots(d, buildSectionDefs(m_section, d), m_expandedIndex, slots);
        if (m_selectedIndex >= 0 && m_selectedIndex < m && slots[m_selectedIndex].opt < 0 &&
            d[slots[m_selectedIndex].def].key == SettingKey::PhonePort) {
            resetPort();
            return true;
        }
    }
    if (pressed & PadButtons::Circle) {
        if (m_expandedIndex >= 0) {
            /* The cursor may sit on one of the choices; those slots vanish on
             * collapse, so return it to the setting that was open. */
            m_selectedIndex = m_expandedIndex;
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
    if (s_pendingFocus < 0) return;
    SettingDef d[kMaxDefs];
    const int n = buildSectionDefs(m_section, d);
    for (int i = 0; i < n; ++i) {
        if (static_cast<int>(d[i].key) == s_pendingFocus) {
            m_selectedIndex = i;
            m_expandedIndex = -1;
            s_pendingFocus = -1;
            return;
        }
    }
}

#if defined(EVO_USB_REMOTE)
// =============================================================================
// Dev remote (#201): `set <setting> <value>`, `set --list`, `focus <row>`.
// A setting is named by its row title in lower case with dashes, so the name is
// whatever the screen shows ("AUDIO DELAY" -> audio-delay). The answer goes to
// /mnt/usb0/evo_set.txt, which evo-remote.sh reads back.
// =============================================================================

namespace {

constexpr const char* kSetResultPath = "/mnt/usb0/evo_set.txt";

/* "AUDIO DELAY" -> "audio-delay"; any run of non-alphanumerics becomes one dash. */
void slugOf(const char* title, char* out, std::size_t n) {
    std::size_t o = 0;
    bool dash = false;
    for (const char* p = title; *p && o + 2 < n; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (dash && o > 0) out[o++] = '-';
            dash = false;
            out[o++] = c;
        } else {
            dash = true;
        }
    }
    out[o] = 0;
}

/* Lower case, no spaces, no trailing "ms": "+100 ms" and "+100" compare equal. */
void normValue(const char* s, char* out, std::size_t n) {
    std::size_t o = 0;
    for (; *s && o + 1 < n; ++s) {
        char c = *s;
        if (c == ' ') continue;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        out[o++] = c;
    }
    out[o] = 0;
    if (o >= 3 && out[o - 2] == 'm' && out[o - 1] == 's' && out[o - 3] >= '0' && out[o - 3] <= '9')
        out[o - 2] = 0;
}

void writeSetResult(const char* text) {
    if (FILE* fp = std::fopen(kSetResultPath, "w")) {
        std::fputs(text, fp);
        std::fputc('\n', fp);
        std::fclose(fp);
    } else if (FILE* fp2 = std::fopen("/data/evoplayer/evo_set.txt", "w")) {
        std::fputs(text, fp2);
        std::fputc('\n', fp2);
        std::fclose(fp2);
    }
    evo_boot_log("remote set: %s", text);
}

/* Finds a settable (toggle or value) row by name. */
bool findSetting(const char* name, ISettingsService* st, SettingDef& def) {
    for (int s = 0; s < kSectionCount; ++s) {
        for (int i = 0; i < kSections[s].count; ++i) {
            SettingDef d = makeDef(kSections[s].keys[i], st);
            if (d.kind != EVO_RMLUI_ROW_TOGGLE && d.kind != EVO_RMLUI_ROW_VALUE) continue;
            char slug[64];
            slugOf(d.title, slug, sizeof slug);
            if (std::strcmp(slug, name) == 0 ||
                (d.key == SettingKey::SkipSegments && std::strcmp(name, "skip-segments") == 0)) {
                def = d;
                return true;
            }
        }
    }
    return false;
}

/* The row's current value as the screen shows it. */
const char* currentLabel(const SettingDef& d) {
    if (d.kind == EVO_RMLUI_ROW_TOGGLE) return d.toggle_on ? "on" : "off";
    return (d.opt_current >= 0 && d.opt_current < d.opt_count) ? d.opt_label[d.opt_current] : "?";
}

void listSettings(ISettingsService* st) {
    static char list[4096];
    std::size_t len = 0;
    list[0] = 0;
    auto put = [&](const char* fmt, const char* a, const char* b) {
        if (len < sizeof list)
            len += std::snprintf(list + len, sizeof list - len, fmt, a, b);
    };
    for (int s = 0; s < kSectionCount; ++s) {
        for (int i = 0; i < kSections[s].count; ++i) {
            const SettingDef d = makeDef(kSections[s].keys[i], st);
            if (d.kind != EVO_RMLUI_ROW_TOGGLE && d.kind != EVO_RMLUI_ROW_VALUE) continue;
            char slug[64];
            if (d.key == SettingKey::SkipSegments) {
                std::snprintf(slug, sizeof slug, "skip-segments");
            } else {
                slugOf(d.title, slug, sizeof slug);
            }
            put("%s: %s", slug, "");
            if (d.kind == EVO_RMLUI_ROW_TOGGLE) {
                put("%s%s", "on|off", "");
            } else if (d.key == SettingKey::AudioDelay) {
                put("%s%s", "-500..+500 (step 50)", "");
            } else {
                for (int o = 0; o < d.opt_count; ++o)
                    put("%s%s", o ? "|" : "", d.opt_label[o]);
            }
            put("%s%s", "\n", "");
        }
    }
    writeSetResult(list);
}

} // namespace

extern "C" void evo_remote_set(const char* args) {
    ISettingsService* st = Application::getInstance().getSettingsService();
    if (!st) { writeSetResult("ERR no settings service"); return; }

    char name[64] = {0}, value[64] = {0};
    std::sscanf(args ? args : "", "%63s %63[^\n]", name, value);

    if (std::strcmp(name, "--list") == 0) {
        listSettings(st);
        return;
    }

    SettingDef def{};
    if (!name[0] || !findSetting(name, st, def)) {
        writeSetResult("ERR unknown setting (set --list shows every name)");
        return;
    }
    char reply[128];
    if (!value[0]) {
        std::snprintf(reply, sizeof reply, "%s=%s", name, currentLabel(def));
        writeSetResult(reply);
        return;
    }

    char want[64];
    normValue(value, want, sizeof want);

    if (def.kind == EVO_RMLUI_ROW_TOGGLE) {
        bool on;
        if (!std::strcmp(want, "on") || !std::strcmp(want, "1")) on = true;
        else if (!std::strcmp(want, "off") || !std::strcmp(want, "0")) on = false;
        else { writeSetResult("ERR value must be on or off"); return; }
        if (on != def.toggle_on) toggleSetting(def.key);
    } else {
        int opt = -1;
        if (def.key == SettingKey::AudioDelay) {
            const int ms = std::atoi(want);
            if (ms < -500 || ms > 500 || ms % 50 != 0) {
                writeSetResult("ERR audio-delay is -500..+500 in steps of 50");
                return;
            }
            opt = ms / 50 + 10;
        } else {
            for (int o = 0; o < def.opt_count && opt < 0; ++o) {
                char lab[64];
                normValue(def.opt_label[o], lab, sizeof lab);
                if (!std::strcmp(lab, want)) opt = o;
            }
        }
        if (opt < 0) { writeSetResult("ERR no such value (set --list shows them)"); return; }
        applyOption(def.key, opt);
    }
    saveSettings();

    /* Print back what the settings service now holds, not what was asked. */
    if (findSetting(name, st, def)) {
        std::snprintf(reply, sizeof reply, "%s=%s", name, currentLabel(def));
        writeSetResult(reply);
    }
}

extern "C" void evo_remote_focus(const char* row) {
    ISettingsService* st = Application::getInstance().getSettingsService();
    auto sm = Application::getInstance().getScreenManager();
    if (!st || !sm || !row) return;
    for (int s = 0; s < kSectionCount; ++s) {
        for (int i = 0; i < kSections[s].count; ++i) {
            const SettingDef d = makeDef(kSections[s].keys[i], st);
            char slug[64];
            slugOf(d.title, slug, sizeof slug);
            if (std::strcmp(slug, row) != 0 &&
                !(d.key == SettingKey::SkipSegments && std::strcmp(row, "skip-segments") == 0)) continue;
            s_pendingFocus = static_cast<int>(kSections[s].keys[i]);
            if (sm->getCurrentScreenId() != kSections[s].screen)
                sm->navigateTo(kSections[s].screen);
            writeSetResult("focus ok");
            return;
        }
    }
    writeSetResult("ERR unknown row");
}
#endif // EVO_USB_REMOTE

} // namespace evo
