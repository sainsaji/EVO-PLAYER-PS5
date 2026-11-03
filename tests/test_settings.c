/*
 * tests/test_settings.c — Settings persistence save/load round-trip and backward compatibility host tests.
 *
 * Exercises evo::SettingsService (projects/evoplayer/core/src/services/SettingsService.cpp)
 * to verify that:
 * 1. Every enum/int/bool setting (and theme) set to a non-default value survives saveSettings()
 *    followed by loadSettings() into a fresh instance.
 * 2. An older settings file lacking the newest line (line 28: audioDelayMs) loads successfully,
 *    leaves the omitted setting at its default value (0 ms), and correctly populates all earlier settings.
 */

#include "evo/services/SettingsService.hpp"
#include "evo/interfaces/ISettingsService.hpp"
#include "evo/Common.hpp"
#include "evo_boot_log.h"
#include "evo_data_path.h"
#include "evo_theme.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>

extern "C" {
    extern int g_tests_run;
    extern int g_tests_passed;
    extern int g_tests_failed;
}

#define TEST_START(name) do { \
    g_tests_run++; \
    printf("  RUN  %-50s", name); \
    fflush(stdout); \
} while(0)

#define TEST_PASS() do { \
    g_tests_passed++; \
    printf(" [ PASS ]\n"); \
} while(0)

#define TEST_FAIL(reason) do { \
    g_tests_failed++; \
    printf(" [ FAIL ] (%s at line %d)\n", reason, __LINE__); \
} while(0)

#define TEST_ASSERT(cond, reason) do { \
    if (!(cond)) { \
        TEST_FAIL(reason); \
        return; \
    } \
} while(0)

/* ---------------------------------------------------------------------------
 * Minimal stubs for subsystem hooks triggered by SettingsService
 * ------------------------------------------------------------------------ */
extern "C" {
    int prospero_secondary_position = 0;
    int prospero_secondary_color = 0;

    void evo_feedback_set_sound(int enabled) { (void)enabled; }
    void evo_feedback_set_lightbar(int enabled) { (void)enabled; }
    void evo_feedback_refresh_lightbar(void) {}
    void evo_keyboard_set_type(int type) { (void)type; }
    void evo_rmlui_set_theme(const void *theme) { (void)theme; }

    static int s_host_port = EVO_LOG_SERVER_PORT;
    int evo_log_server_port(void) { return s_host_port; }
    int evo_log_server_saved_port(void) { return s_host_port; }
    int evo_log_server_state(void) { return EVO_LOGSRV_SAVED; }
    void evo_log_server_set_port(int port) { if (port >= 1024 && port <= 65535) s_host_port = port; }
    int evo_log_server_probe_port(int port) { return (port < 1024 || port > 65535) ? 22 : 0; }
    int evo_log_server_try_port(int port) { if (port < 1024 || port > 65535) return 22; s_host_port = port; return 0; }
}

/* ---------------------------------------------------------------------------
 * Test 1: Full Save / Load Round-Trip for Every Setting
 * ------------------------------------------------------------------------ */
extern "C" void test_settings_save_load_roundtrip(void)
{
    TEST_START("Settings: Save/Load Round-Trip (All Settings)");

    const char *cfg_path = evo_data_path("evo_player_settings.cfg");
    std::remove(cfg_path);

    /* 1. Verify default values on a fresh instance */
    evo::SettingsService svc1;

    TEST_ASSERT(svc1.isResumePlaybackEnabled() == true, "Initial ResumePlaybackEnabled default mismatch");
    TEST_ASSERT(svc1.getDefaultViewMode() == evo::ViewMode::Fit, "Initial DefaultViewMode default mismatch");
    TEST_ASSERT(svc1.isAutoSubtitlesEnabled() == true, "Initial AutoSubtitlesEnabled default mismatch");
    TEST_ASSERT(svc1.isDebugOverlayEnabled() == false, "Initial DebugOverlayEnabled default mismatch");
    TEST_ASSERT(svc1.isSortFoldersFirst() == true, "Initial SortFoldersFirst default mismatch");
    TEST_ASSERT(svc1.isSoundFeedbackEnabled() == true, "Initial SoundFeedbackEnabled default mismatch");
    TEST_ASSERT(svc1.isLightbarFeedbackEnabled() == true, "Initial LightbarFeedbackEnabled default mismatch");
    TEST_ASSERT(svc1.getSubtitleFontFace() == 1, "Initial SubtitleFontFace default mismatch");
    TEST_ASSERT(svc1.getKeyboardType() == 1, "Initial KeyboardType default mismatch");
    TEST_ASSERT(svc1.getVideoDecoderPreference() == evo::DecoderPreference::Auto, "Initial VideoDecoderPreference default mismatch");
    TEST_ASSERT(svc1.getUpscaler() == evo::Upscaler::Off, "Initial Upscaler default mismatch");
    TEST_ASSERT(svc1.getAiNetwork() == evo::AiNetwork::Auto, "Initial AiNetwork default mismatch");
    TEST_ASSERT(svc1.getRefreshRateMode() == evo::RefreshRateMode::Off, "Initial RefreshRateMode default mismatch");
    TEST_ASSERT(svc1.getHdrOutputMode() == evo::HdrOutputMode::Auto, "Initial HdrOutputMode default mismatch");
    TEST_ASSERT(svc1.getSecondarySubtitlePosition() == 0, "Initial SecondarySubtitlePosition default mismatch");
    TEST_ASSERT(svc1.getSecondarySubtitleColor() == 0, "Initial SecondarySubtitleColor default mismatch");
    TEST_ASSERT(svc1.isAskStreamEnabled() == true, "Initial AskStreamEnabled default mismatch");
    TEST_ASSERT(svc1.getAudioOutputChannels() == evo::AudioOutputChannels::Auto, "Initial AudioOutputChannels default mismatch");
    TEST_ASSERT(svc1.getMotionSmoothing() == evo::MotionSmoothing::Off, "Initial MotionSmoothing default mismatch");
    TEST_ASSERT(svc1.getDeepBlacks() == evo::DeepBlacks::Off, "Initial DeepBlacks default mismatch");
    TEST_ASSERT(svc1.isAudioPassthroughEnabled() == false, "Initial AudioPassthroughEnabled default mismatch");
    TEST_ASSERT(svc1.isMatchVideoRateEnabled() == false, "Initial MatchVideoRateEnabled default mismatch");
    TEST_ASSERT(svc1.getVolumeBoost() == evo::VolumeBoost::Off, "Initial VolumeBoost default mismatch");
    TEST_ASSERT(svc1.getNetworkBuffer() == evo::NetworkBuffer::Sec30, "Initial NetworkBuffer default mismatch");
    TEST_ASSERT(svc1.getAudioDelayMs() == 0, "Initial AudioDelayMs default mismatch");
    TEST_ASSERT(svc1.getThemeName() == "EVO Dark", "Initial ThemeName default mismatch");
    TEST_ASSERT(evo_log_server_saved_port() == EVO_LOG_SERVER_PORT, "Initial LogServerPort default mismatch");

    /* 2. Mutate every enum/int/bool setting and theme to a non-default value */
    svc1.setResumePlaybackEnabled(false);
    svc1.setDefaultViewMode(evo::ViewMode::Fill);
    svc1.setAutoSubtitlesEnabled(false);
    svc1.setDebugOverlayEnabled(true);
    svc1.setSortFoldersFirst(false);
    svc1.setSoundFeedbackEnabled(false);
    svc1.setLightbarFeedbackEnabled(false);
    svc1.setSubtitleFontFace(2);
    svc1.setKeyboardType(0);
    svc1.setVideoDecoderPreference(evo::DecoderPreference::NativeHardware);
    svc1.setUpscaler(evo::Upscaler::Sharp);
    svc1.setAiNetwork(evo::AiNetwork::Standard);
    svc1.setRefreshRateMode(evo::RefreshRateMode::Always);
    svc1.setHdrOutputMode(evo::HdrOutputMode::Off);
    svc1.setSecondarySubtitlePosition(1);
    svc1.setSecondarySubtitleColor(1);
    svc1.setAskStreamEnabled(false);
    svc1.setAudioOutputChannels(evo::AudioOutputChannels::Stereo);
    svc1.setMotionSmoothing(evo::MotionSmoothing::Low);
    svc1.setDeepBlacks(evo::DeepBlacks::Low);
    svc1.setAudioPassthroughEnabled(true);
    svc1.setMatchVideoRateEnabled(true);
    svc1.setVolumeBoost(evo::VolumeBoost::Plus100);
    svc1.setNetworkBuffer(evo::NetworkBuffer::Min1);
    svc1.setAudioDelayMs(150);
    svc1.setThemeName("CARBON");
    evo_log_server_set_port(9812);

    /* 3. Persist to disk */
    bool save_ok = svc1.saveSettings();
    TEST_ASSERT(save_ok, "saveSettings failed");

    /* Reset port to default before loading into fresh instance */
    evo_log_server_set_port(EVO_LOG_SERVER_PORT);

    /* 4. Load into a completely fresh instance */
    evo::SettingsService svc2;
    bool load_ok = svc2.loadSettings();
    TEST_ASSERT(load_ok, "loadSettings failed on saved configuration");

    /* 5. Verify every non-default value survived serialization & deserialization */
    TEST_ASSERT(svc2.isResumePlaybackEnabled() == false, "ResumePlaybackEnabled round-trip mismatch");
    TEST_ASSERT(svc2.getDefaultViewMode() == evo::ViewMode::Fill, "DefaultViewMode round-trip mismatch");
    TEST_ASSERT(svc2.isAutoSubtitlesEnabled() == false, "AutoSubtitlesEnabled round-trip mismatch");
    TEST_ASSERT(svc2.isDebugOverlayEnabled() == true, "DebugOverlayEnabled round-trip mismatch");
    TEST_ASSERT(svc2.isSortFoldersFirst() == false, "SortFoldersFirst round-trip mismatch");
    TEST_ASSERT(svc2.isSoundFeedbackEnabled() == false, "SoundFeedbackEnabled round-trip mismatch");
    TEST_ASSERT(svc2.isLightbarFeedbackEnabled() == false, "LightbarFeedbackEnabled round-trip mismatch");
    TEST_ASSERT(svc2.getSubtitleFontFace() == 2, "SubtitleFontFace round-trip mismatch");
    TEST_ASSERT(svc2.getKeyboardType() == 0, "KeyboardType round-trip mismatch");
    TEST_ASSERT(svc2.getVideoDecoderPreference() == evo::DecoderPreference::NativeHardware, "VideoDecoderPreference round-trip mismatch");
    TEST_ASSERT(svc2.getUpscaler() == evo::Upscaler::Sharp, "Upscaler round-trip mismatch");
    TEST_ASSERT(svc2.getAiNetwork() == evo::AiNetwork::Standard, "AiNetwork round-trip mismatch");
    TEST_ASSERT(svc2.getRefreshRateMode() == evo::RefreshRateMode::Always, "RefreshRateMode round-trip mismatch");
    TEST_ASSERT(svc2.getHdrOutputMode() == evo::HdrOutputMode::Off, "HdrOutputMode round-trip mismatch");
    TEST_ASSERT(svc2.getSecondarySubtitlePosition() == 1, "SecondarySubtitlePosition round-trip mismatch");
    TEST_ASSERT(svc2.getSecondarySubtitleColor() == 1, "SecondarySubtitleColor round-trip mismatch");
    TEST_ASSERT(svc2.isAskStreamEnabled() == false, "AskStreamEnabled round-trip mismatch");
    TEST_ASSERT(svc2.getAudioOutputChannels() == evo::AudioOutputChannels::Stereo, "AudioOutputChannels round-trip mismatch");
    TEST_ASSERT(svc2.getMotionSmoothing() == evo::MotionSmoothing::Low, "MotionSmoothing round-trip mismatch");
    TEST_ASSERT(svc2.getDeepBlacks() == evo::DeepBlacks::Low, "DeepBlacks round-trip mismatch");
    TEST_ASSERT(svc2.isAudioPassthroughEnabled() == true, "AudioPassthroughEnabled round-trip mismatch");
    TEST_ASSERT(svc2.isMatchVideoRateEnabled() == true, "MatchVideoRateEnabled round-trip mismatch");
    TEST_ASSERT(svc2.getVolumeBoost() == evo::VolumeBoost::Plus100, "VolumeBoost round-trip mismatch");
    TEST_ASSERT(svc2.getNetworkBuffer() == evo::NetworkBuffer::Min1, "NetworkBuffer round-trip mismatch");
    TEST_ASSERT(svc2.getAudioDelayMs() == 150, "AudioDelayMs round-trip mismatch");
    TEST_ASSERT(svc2.getThemeName() == "CARBON", "ThemeName round-trip mismatch");
    TEST_ASSERT(evo_log_server_saved_port() == 9812, "Log server saved port round-trip mismatch");

    TEST_PASS();
}

/* ---------------------------------------------------------------------------
 * Test 2: Backward Compatibility with Older Settings File Lacking Line 29
 * ------------------------------------------------------------------------ */
extern "C" void test_settings_legacy_compat(void)
{
    TEST_START("Settings: Backward Compatibility with Truncated File");

    const char *cfg_path = evo_data_path("evo_player_settings.cfg");
    FILE *f = std::fopen(cfg_path, "w");
    TEST_ASSERT(f != nullptr, "Failed to create legacy cfg file");

    /*
     * Write exactly 28 lines (pre-#227 format), deliberately omitting
     * line 29 (phone & log port). All values are non-default.
     */
    std::fprintf(f,
        "0\n"       /* 1: legacy dummy profile */
        "0\n"       /* 2: resumePlaybackEnabled = false */
        "1\n"       /* 3: defaultViewMode = Fill (1) */
        "0\n"       /* 4: autoSubtitlesEnabled = false */
        "1\n"       /* 5: debugOverlayEnabled = true */
        "0\n"       /* 6: sortFoldersFirst = false */
        "CARBON\n"  /* 7: activeTheme = CARBON */
        "0\n"       /* 8: soundFeedbackEnabled = false */
        "0\n"       /* 9: lightbarFeedbackEnabled = false */
        "2\n"       /* 10: subtitleFontFace = 2 */
        "0\n"       /* 11: keyboardType = 0 */
        "1\n"       /* 12: decoderPreference = NativeHardware (1) */
        "2\n"       /* 13: marker = 2 */
        "1\n"       /* 14: upscaler = Sharp (1) */
        "1\n"       /* 15: aiNetwork = Standard (1) */
        "1\n"       /* 16: refreshRateMode = Always (1) */
        "1\n"       /* 17: hdrOutputMode = Off (1) */
        "1\n"       /* 18: secondarySubtitlePosition = 1 */
        "1\n"       /* 19: secondarySubtitleColor = 1 */
        "0\n"       /* 20: askStream = false */
        "1\n"       /* 21: audioOutputChannels = Stereo (1) */
        "1\n"       /* 22: motionSmoothing = Low (1) */
        "1\n"       /* 23: deepBlacks = Low (1) */
        "1\n"       /* 24: audioPassthroughEnabled = true */
        "1\n"       /* 25: matchVideoRate = true */
        "2\n"       /* 26: volumeBoost = Plus100 (2) */
        "1\n"       /* 27: networkBuffer = Min1 (1) */
        "150\n"     /* 28: audioDelayMs = 150 (#170) */
        /* Line 29 (phone & log port, #227) intentionally omitted */
    );
    std::fclose(f);

    evo_log_server_set_port(EVO_LOG_SERVER_PORT);
    TEST_ASSERT(evo_log_server_saved_port() == EVO_LOG_SERVER_PORT, "Default LogServerPort must be 9780");

    evo::SettingsService svc;

    /* Verify fresh instance starts at default AudioDelayMs = 0 */
    TEST_ASSERT(svc.getAudioDelayMs() == 0, "Default AudioDelayMs must be 0 ms");

    /* Loading truncated file must succeed */
    bool load_ok = svc.loadSettings();
    TEST_ASSERT(load_ok, "loadSettings failed on 28-line legacy configuration");

    /* Missing line 29 setting must retain its default value */
    TEST_ASSERT(evo_log_server_saved_port() == EVO_LOG_SERVER_PORT, "Missing log port must retain default (9780)");
    TEST_ASSERT(svc.getAudioDelayMs() == 150, "Legacy audioDelayMs mismatch");

    /* All 26 preceding settings must be restored correctly */
    TEST_ASSERT(svc.isResumePlaybackEnabled() == false, "Legacy resumePlaybackEnabled mismatch");
    TEST_ASSERT(svc.getDefaultViewMode() == evo::ViewMode::Fill, "Legacy defaultViewMode mismatch");
    TEST_ASSERT(svc.isAutoSubtitlesEnabled() == false, "Legacy autoSubtitlesEnabled mismatch");
    TEST_ASSERT(svc.isDebugOverlayEnabled() == true, "Legacy debugOverlayEnabled mismatch");
    TEST_ASSERT(svc.isSortFoldersFirst() == false, "Legacy sortFoldersFirst mismatch");
    TEST_ASSERT(svc.isSoundFeedbackEnabled() == false, "Legacy soundFeedbackEnabled mismatch");
    TEST_ASSERT(svc.isLightbarFeedbackEnabled() == false, "Legacy lightbarFeedbackEnabled mismatch");
    TEST_ASSERT(svc.getSubtitleFontFace() == 2, "Legacy subtitleFontFace mismatch");
    TEST_ASSERT(svc.getKeyboardType() == 0, "Legacy keyboardType mismatch");
    TEST_ASSERT(svc.getVideoDecoderPreference() == evo::DecoderPreference::NativeHardware, "Legacy decoderPreference mismatch");
    TEST_ASSERT(svc.getUpscaler() == evo::Upscaler::Sharp, "Legacy upscaler mismatch");
    TEST_ASSERT(svc.getAiNetwork() == evo::AiNetwork::Standard, "Legacy aiNetwork mismatch");
    TEST_ASSERT(svc.getRefreshRateMode() == evo::RefreshRateMode::Always, "Legacy refreshRateMode mismatch");
    TEST_ASSERT(svc.getHdrOutputMode() == evo::HdrOutputMode::Off, "Legacy hdrOutputMode mismatch");
    TEST_ASSERT(svc.getSecondarySubtitlePosition() == 1, "Legacy secondarySubtitlePosition mismatch");
    TEST_ASSERT(svc.getSecondarySubtitleColor() == 1, "Legacy secondarySubtitleColor mismatch");
    TEST_ASSERT(svc.isAskStreamEnabled() == false, "Legacy askStream mismatch");
    TEST_ASSERT(svc.getAudioOutputChannels() == evo::AudioOutputChannels::Stereo, "Legacy audioOutputChannels mismatch");
    /* #233: a file from before the Experimental section resets those once. */
    TEST_ASSERT(svc.getMotionSmoothing() == evo::MotionSmoothing::Off, "Legacy motionSmoothing must reset to Off");
    TEST_ASSERT(svc.getDeepBlacks() == evo::DeepBlacks::Off, "Legacy deepBlacks must reset to Off");
    TEST_ASSERT(svc.isAudioPassthroughEnabled() == true, "Legacy audioPassthroughEnabled mismatch");
    TEST_ASSERT(svc.isMatchVideoRateEnabled() == false, "Legacy matchVideoRate must reset to Off");
    TEST_ASSERT(svc.getVolumeBoost() == evo::VolumeBoost::Plus100, "Legacy volumeBoost mismatch");
    TEST_ASSERT(svc.getNetworkBuffer() == evo::NetworkBuffer::Min1, "Legacy networkBuffer mismatch");
    TEST_ASSERT(svc.getThemeName() == "CARBON", "Legacy themeName mismatch");

    /* ...once: the user's choice after that sticks across a reload. */
    svc.setDeepBlacks(evo::DeepBlacks::High);
    TEST_ASSERT(svc.saveSettings(), "save after the reset failed");
    evo::SettingsService again;
    TEST_ASSERT(again.loadSettings(), "reload failed");
    TEST_ASSERT(again.getDeepBlacks() == evo::DeepBlacks::High, "deepBlacks chosen after the reset must stick");
    TEST_ASSERT(again.getVolumeBoost() == evo::VolumeBoost::Plus100, "other settings survive the reset");

    TEST_PASS();
}
