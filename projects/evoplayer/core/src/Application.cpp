#include "evo/Application.hpp"
#include "evo/screens/LaunchScreen.hpp"
#include "evo/screens/BrowserScreen.hpp"
#include "evo/screens/PlayerScreen.hpp"
#include "evo/screens/SettingsScreen.hpp"
#include "evo/screens/SubtitlePickerScreen.hpp"
#include "evo/screens/AudioTrackPickerScreen.hpp"
#include "evo/screens/TextReaderScreen.hpp"
#include "evo/screens/MediaInfoScreen.hpp"
#include "evo/screens/SurroundTestScreen.hpp"
#include "evo/screens/DeveloperToolsScreen.hpp"
#include "evo/screens/EmbyScreen.hpp"
#include "evo/screens/ModalDialogScreen.hpp"
#include "evo/screens/RecentFilesScreen.hpp"
#include "evo/screens/FavoritesScreen.hpp"
#include "evo/screens/AboutSupportScreen.hpp"
#include "evo/screens/ImageViewerScreen.hpp"
#include "evo/screens/ChangelogScreen.hpp"
#include "evo/animation/AnimationManager.hpp"

#include "evo_boot_log.h"

#include <ctime>
#include <sys/time.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "evo_agc_runtime.h"
#include "evo_toast.h"

/* Defined in Bridge.cpp and read by every FPS readout in the app, but
 * nothing ever assigned it - so the player pill and the rail pill both
 * showed "0 FPS". The render loop is the only place that can measure it. */
extern "C" int perf_render_fps;
#include "evo_boot_trace.h"
#include "evo_crash_note.h"
#include "evo_jailbreak.h"
#include "evo_vdec.h"
#include "evo_direct_mem.h"
#include "evo_data_path.h"
#include "evo_toast.h"
#include "evo_recent.h"
#include "evo_favorites.h"
#include "evo_theme.h"
#include "evo_keyboard.h"
#include "evo_feedback.h"
#include "evo_input.h"
#include "evo_net.h"
#include "evo_usb_remote.h"
#include "addon_emby.h"
#include "pp_playback.h"
#include "evo_playback.h"
#include "evo_adec.h"
#include "evo_rmlui_bridge.h"
#include "evo_perf_monitor.h"
#include "evo_vdec.h"

#include "evo_agc_runtime.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/cpu.h>
#include <sys/stat.h>
#include <unistd.h>

int sceUserServiceInitialize(void *);
int sceUserServiceGetLoginUserIdList(int userId[4]);
int scePadInit(void);
int scePadOpen(int, int, int, void *);

typedef struct PS5_PadData {
    uint32_t buttons;
    struct { uint8_t x; uint8_t y; } leftStick;
    struct { uint8_t x; uint8_t y; } rightStick;
    struct { uint8_t l2; uint8_t r2; } analogButtons;
    uint8_t rest[128];
} PS5_PadData;

int scePadReadState(int, PS5_PadData *);

extern pp_playback g_pp_pb;
extern int g_ps5_user_id;
extern int g_ps5_video_out_hdr;
extern int screen;
long long now_ms(void);
}

#ifndef EVO_PLAYER_VERSION
#define EVO_PLAYER_VERSION "0.7.6"
#endif

#ifndef EVO_DIRECT_MEM_POOL_BYTES
#define EVO_DIRECT_MEM_POOL_BYTES (128 * 1024 * 1024)
#endif

namespace evo {

static evo_input evo_pad_state;

static void SoundEffectCallback(int sfxKind) {
    if (auto sfx = Application::getInstance().getSoundEffectEngine()) {
        sfx->playSound(static_cast<SoundEffect>(sfxKind));
    }
}

static void EnsureDataDirectories() {
    mkdir("/data", 0777);
    mkdir(evo_data_dir(), 0777);
}

Application& Application::getInstance() {
    static Application s_instance;
    return s_instance;
}

Application::Application()
    : m_appFsm(ApplicationState::Uninitialized, "ApplicationFSM")
{
    initStateMachine();
}

Application::~Application() {
    shutdown();
}

void Application::initStateMachine() {
    m_appFsm
        .addState(ApplicationState::Uninitialized, "Uninitialized")
        .addState(ApplicationState::InitializingHardware, "InitializingHardware")
        .addState(ApplicationState::InitializingServices, "InitializingServices")
        .addState(ApplicationState::InitializingScreens, "InitializingScreens")
        .addState(ApplicationState::Running, "Running")
        .addState(ApplicationState::Exiting, "Exiting")
        .addState(ApplicationState::Terminated, "Terminated");

    m_appFsm
        .addTransition(ApplicationState::Uninitialized, ApplicationEvent::Boot, ApplicationState::InitializingHardware)
        .addTransition(ApplicationState::InitializingHardware, ApplicationEvent::HardwareReady, ApplicationState::InitializingServices)
        .addTransition(ApplicationState::InitializingServices, ApplicationEvent::ServicesReady, ApplicationState::InitializingScreens)
        .addTransition(ApplicationState::InitializingScreens, ApplicationEvent::ScreensReady, ApplicationState::Running)
        .addTransition(ApplicationState::Running, ApplicationEvent::RequestExit, ApplicationState::Exiting)
        .addTransition(ApplicationState::Exiting, ApplicationEvent::ShutdownComplete, ApplicationState::Terminated)
        .addTransition(ApplicationState::Running, ApplicationEvent::ShutdownComplete, ApplicationState::Terminated);
}

void Application::requestExit() {
    m_running = false;
    m_appFsm.postEvent(ApplicationEvent::RequestExit);
}

bool Application::initialize(int argc, char** argv) {
    (void)argc;
    (void)argv;

    evo_bt("Application::initialize entry");
    m_appFsm.postEvent(ApplicationEvent::Boot);
    evo_vdec_probe();
    /* Same pre-unjail constraint as the video decoder: after
     * evo_jailbreak_self() the credential swap breaks libSceAudiodec too. */
    evo_adec_native_probe();

    if (!initHardware()) {
        return false;
    }
    m_appFsm.postEvent(ApplicationEvent::HardwareReady);

    if (!initServices()) {
        return false;
    }
    m_appFsm.postEvent(ApplicationEvent::ServicesReady);

    if (!initScreens()) {
        return false;
    }
    m_appFsm.postEvent(ApplicationEvent::ScreensReady);

    m_running = true;
    evo_bt("Application::initialize complete");
    return true;
}

bool Application::initHardware() {
#if defined(EVO_APP_MODULE)
    evo_bt("AGC: bare-metal AGC device context");
    int agcOk = evo_agc_runtime_init(DisplayWidth, DisplayHeight, 0);
    if (agcOk == 0) {
        /*
         * The runtime resolves what the panel is actually running at while it
         * brings VideoOut up, and may have settled on something other than the
         * 1080p default. Everything built below - the RmlUi contexts, the
         * video output rect, the UI scratch surface - has to use that size.
         */
        evo_agc_runtime_get_size(&DisplayWidth, &DisplayHeight);
        evo_bt("display: rendering at %dx%d", DisplayWidth, DisplayHeight);
        evo_boot_log_flush();
        evo_agc_runtime_frame_begin();
        evo_agc_runtime_present();
    }
    evo_keyboard_ime_probe();
    evo_boot_log_flush();
#endif

    evo_jailbreak_self();
    evo_boot_log_flush();

    av_force_cpu_flags(0);
    evo_direct_mem_init(EVO_DIRECT_MEM_POOL_BYTES);
    EnsureDataDirectories();
    evo_crash_note_init();

    sceUserServiceInitialize(nullptr);
    scePadInit();

    int users[4] = {0};
    sceUserServiceGetLoginUserIdList(users);
    g_ps5_user_id = users[0];

    m_padHandle = scePadOpen(users[0], 0, 0, nullptr);
    if (m_padHandle < 0) {
        toast("PAD OPEN FAIL", "Could not open controller");
    } else {
        toast("PAD OPEN OK", "Controller connected");
    }

    evo_rmlui_init(DisplayWidth, DisplayHeight);

    evo_input_reset(&evo_pad_state);
    evo_feedback_init(m_padHandle, SoundEffectCallback);

    evo_net_init();
    emby_init();
    avformat_network_init();

    pp_playback_init(&g_pp_pb);
    pp_playback_set_output(&g_pp_pb, DisplayWidth, DisplayHeight, PP_ASPECT_FIT);
    evo_vdec_prefer_nv12(1);

    m_uiScratch = static_cast<uint32_t*>(calloc(static_cast<size_t>(DisplayWidth) * DisplayHeight, 4));
    return true;
}

bool Application::initServices() {
    m_settingsService = std::make_unique<SettingsService>();
    m_mediaMetadataService = std::make_unique<MediaMetadataService>();
    m_coverArtService = std::make_unique<CoverArtService>();
    m_soundEffectEngine = std::make_unique<SoundEffectEngine>();
    m_surroundTestService = std::make_unique<SurroundTestService>();
    m_fileSystemBrowser = std::make_unique<FileSystemBrowser>();
    m_playbackController = std::make_unique<PlaybackController>();
    m_screenManager = std::make_unique<ScreenManager>();

    m_soundEffectEngine->initialize();
    m_settingsService->loadSettings();
    recent_load();
    favorites_load();
    m_fileSystemBrowser->loadLastFolder();

    toast("EVO Player", "Version " EVO_PLAYER_VERSION);
    return true;
}

bool Application::initScreens() {
    m_screenManager->registerScreen(std::make_unique<LaunchScreen>());
    m_screenManager->registerScreen(std::make_unique<BrowserScreen>());
    m_screenManager->registerScreen(std::make_unique<PlayerScreen>());
    m_screenManager->registerScreen(std::make_unique<SettingsScreen>());
    m_screenManager->registerScreen(std::make_unique<SettingsPlaybackScreen>());
    m_screenManager->registerScreen(std::make_unique<SettingsSubtitlesScreen>());
    m_screenManager->registerScreen(std::make_unique<SettingsInterfaceScreen>());
    m_screenManager->registerScreen(std::make_unique<SettingsSystemScreen>());
    m_screenManager->registerScreen(std::make_unique<SubtitlePickerScreen>());
    m_screenManager->registerScreen(std::make_unique<AudioTrackPickerScreen>());
    m_screenManager->registerScreen(std::make_unique<TextReaderScreen>());
    m_screenManager->registerScreen(std::make_unique<MediaInfoScreen>());
    m_screenManager->registerScreen(std::make_unique<SurroundTestScreen>());
    m_screenManager->registerScreen(std::make_unique<DeveloperToolsScreen>());
    m_screenManager->registerScreen(std::make_unique<EmbyScreen>());
    m_screenManager->registerScreen(std::make_unique<RecentFilesScreen>());
    m_screenManager->registerScreen(std::make_unique<FavoritesScreen>());
    m_screenManager->registerScreen(std::make_unique<AboutSupportScreen>());
    m_screenManager->registerScreen(std::make_unique<ChangelogScreen>());
    m_screenManager->registerScreen(std::make_unique<ImageViewerScreen>());
    m_screenManager->registerScreen(std::make_unique<ModalDialogScreen>(ModalType::ExitConfirm));
    m_screenManager->registerScreen(std::make_unique<ModalDialogScreen>(ModalType::ResumePrompt));

    m_screenManager->navigateTo(ScreenId::MainMenu);
    return true;
}

void Application::shutdown() {
    m_running = false;

    if (m_playbackController) {
        m_playbackController->stopPlayback();
    }
    if (m_surroundTestService) {
        m_surroundTestService->stop();
    }
    if (m_soundEffectEngine) {
        m_soundEffectEngine->shutdown();
    }

    pp_playback_shutdown(&g_pp_pb);

    evo_agc_runtime_shutdown();

    if (m_uiScratch) {
        free(m_uiScratch);
        m_uiScratch = nullptr;
    }

    m_appFsm.postEvent(ApplicationEvent::ShutdownComplete);
}

namespace {

/*
 * L3 screenshot.
 *
 * The feature survived only as a changelog line: nothing handled PadButtons::L3,
 * there was no BMP writer, and evo_agc_runtime_read_scanout() - which exists
 * precisely for this - had no callers. Captures the FRONT buffer (what is on
 * the panel right now), so it works on menus as well as during playback.
 *
 * 24-bit bottom-up BMP: the simplest format every viewer reads, and the same
 * one tools/shot.sh already expects.
 */
bool evo_capture_screenshot(std::string& outPath) {
    int w = 0, h = 0;
    evo_agc_runtime_get_size(&w, &h);
    if (w <= 0 || h <= 0) {
        evo_bt("screenshot: no render size (%dx%d)", w, h);
        evo_boot_log_flush();
        return false;
    }

    std::vector<uint32_t> bgra(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
    evo_agc_runtime_read_scanout(bgra.data(), w, h);

    /* Pick the next free slot so captures accumulate instead of overwriting. */
    char path[256];
    int slot = 0;
    for (; slot < 1000; ++slot) {
        std::snprintf(path, sizeof(path), "/mnt/usb0/evo_shot_%03d.bmp", slot);
        FILE* probe = std::fopen(path, "rb");
        if (!probe) break;
        std::fclose(probe);
    }
    if (slot >= 1000) return false;

    FILE* fp = std::fopen(path, "wb");
    if (!fp) {
        /* USB is the nice place for these (the user can pull them off without
         * FTP), but it is not always writable. Fall back to the data root
         * rather than just failing. */
        std::snprintf(path, sizeof(path), "%s/evo_shot_%03d.bmp", evo_data_dir(), slot);
        fp = std::fopen(path, "wb");
    }
    if (!fp) {
        evo_bt("screenshot: cannot open %s for writing", path);
        evo_boot_log_flush();
        return false;
    }

    const int rowBytes = w * 3;
    const int pad = (4 - (rowBytes % 4)) % 4;
    const uint32_t pixelBytes = static_cast<uint32_t>((rowBytes + pad) * h);
    const uint32_t fileSize = 54u + pixelBytes;

    unsigned char hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (unsigned char)(fileSize);       hdr[3] = (unsigned char)(fileSize >> 8);
    hdr[4] = (unsigned char)(fileSize >> 16); hdr[5] = (unsigned char)(fileSize >> 24);
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = (unsigned char)(w);        hdr[19] = (unsigned char)(w >> 8);
    hdr[20] = (unsigned char)(w >> 16);  hdr[21] = (unsigned char)(w >> 24);
    hdr[22] = (unsigned char)(h);        hdr[23] = (unsigned char)(h >> 8);
    hdr[24] = (unsigned char)(h >> 16);  hdr[25] = (unsigned char)(h >> 24);
    hdr[26] = 1;
    hdr[28] = 24;
    hdr[34] = (unsigned char)(pixelBytes);       hdr[35] = (unsigned char)(pixelBytes >> 8);
    hdr[36] = (unsigned char)(pixelBytes >> 16); hdr[37] = (unsigned char)(pixelBytes >> 24);
    std::fwrite(hdr, 1, sizeof(hdr), fp);

    std::vector<unsigned char> row(static_cast<size_t>(rowBytes + pad), 0u);
    for (int y = h - 1; y >= 0; --y) {           /* BMP rows run bottom-up */
        const uint32_t* src = bgra.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
        for (int x = 0; x < w; ++x) {
            /*
             * The scanout is 0xAABBGGRR - the format MakeColorBgra() packs and
             * the one frame_begin clears with (0xff100d0d for r,g,b = 0d,0d,10).
             * So the low byte is RED, and a 24-bit BMP wants B,G,R. Reading the
             * low byte as blue, as this did, swapped red and blue in every
             * capture.
             */
            const uint32_t px = src[x];
            row[x * 3 + 0] = (unsigned char)((px >> 16) & 0xFF);   /* B */
            row[x * 3 + 1] = (unsigned char)((px >> 8) & 0xFF);    /* G */
            row[x * 3 + 2] = (unsigned char)(px & 0xFF);           /* R */
        }
        std::fwrite(row.data(), 1, row.size(), fp);
    }
    std::fclose(fp);
    outPath = path;
    return true;
}

} // namespace

int Application::run() {
    PS5_PadData padData;
    uint32_t lastButtons = 0;

    m_appFsm.postEvent(ApplicationEvent::StartLoop);
    evo_bt("Application: entering frame loop");
    evo_boot_log_flush();

    for (int frame = 0; m_running; ++frame) {
        if (frame < 5) {
            evo_bt("frame %d: start poll", frame);
            evo_boot_log_flush();
        }

        /* Render FPS over a ~500 ms window: long enough to be steady, short
         * enough to react. Sampled here rather than per-screen so the menus and
         * the player report the same number. */
        {
            static struct timespec s_fps_mark = {0, 0};
            static int s_fps_frames = 0;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (s_fps_mark.tv_sec == 0 && s_fps_mark.tv_nsec == 0) {
                s_fps_mark = now;
            } else {
                ++s_fps_frames;
                const double elapsed =
                    static_cast<double>(now.tv_sec - s_fps_mark.tv_sec) +
                    static_cast<double>(now.tv_nsec - s_fps_mark.tv_nsec) / 1e9;
                if (elapsed >= 0.5) {
                    perf_render_fps =
                        static_cast<int>(static_cast<double>(s_fps_frames) / elapsed + 0.5);
                    s_fps_frames = 0;
                    s_fps_mark = now;
                }
            }
        }

        evo_net_poll();
        evo_usb_remote_poll();

        if ((frame & 63) == 0) {
            evo_boot_log_flush();
        }

        static int jb_repaint = 0;
        if (evo_jailbreak_poll()) {
#ifdef EVO_APP_MODULE
            evo_data_path_rebind();
            EnsureDataDirectories();
            /* The quarantine only counts once it lives on /data - see
             * evo_crash_note_init(). */
            evo_crash_note_init();
            recent_load();
            favorites_load();
            if (m_settingsService) {
                m_settingsService->loadSettings();
            }
            emby_init();
            evo_bt("persistence: rebound to %s after late unjail", evo_data_dir());
            evo_boot_log_flush();
#endif
            toast("STORAGE", "READY");
            jb_repaint = 8;
        }

        // 1. Controller input & auto-repeat
        std::memset(&padData, 0, sizeof(padData));
        uint32_t pressed = 0;
        uint32_t released = 0;
        uint32_t held = 0;
        bool hasInput = false;

        if (m_padHandle >= 0 && scePadReadState(m_padHandle, &padData) == 0) {
            pressed = padData.buttons & ~lastButtons;
            released = ~padData.buttons & lastButtons;
            held = padData.buttons;

            evo_input_update(&evo_pad_state, padData.buttons, static_cast<uint64_t>(now_ms()));

            // Synthesize directional repeat events into pressed mask
            if (evo_input_fired(&evo_pad_state, EVO_ACT_UP))    pressed |= PadButtons::Up;
            if (evo_input_fired(&evo_pad_state, EVO_ACT_DOWN))  pressed |= PadButtons::Down;
            if (evo_input_fired(&evo_pad_state, EVO_ACT_LEFT))  pressed |= PadButtons::Left;
            if (evo_input_fired(&evo_pad_state, EVO_ACT_RIGHT)) pressed |= PadButtons::Right;

            hasInput = (pressed != 0 || released != 0 || evo_input_any(&evo_pad_state));
            if (hasInput) {
                evo::animation::AnimationManager::getInstance().triggerTransition(350.0);
            }

            /*
             * L3 never reached the handler below on hardware - not one
             * `screenshot: L3 pressed` line in a whole session - so print the
             * raw mask for the first presses of a run and find out what the
             * stick click actually reports as, rather than assuming 0x0002.
             */
            if (pressed) {
                static int s_padlog = 24;
                if (s_padlog > 0) {
                    s_padlog--;
                    evo_bt("pad: pressed=%#010x held=%#010x", pressed, held);
                    evo_boot_log_flush();
                }
            }

            /*
             * Either stick click captures the screen, and is swallowed so no
             * screen sees it as a normal press.
             *
             * It was bound to L3 alone and never once fired: across a logged
             * session the pad reported 0x0004 (R3) for the stick click and
             * 0x0002 (L3) not at all. Accepting both means it works whichever
             * bit this pad and firmware decide to send.
             */
            if (pressed & (PadButtons::L3 | PadButtons::R3)) {
                evo_bt("screenshot: stick click pressed=%#010x", pressed);
                evo_boot_log_flush();
                std::string shotPath;
                if (evo_capture_screenshot(shotPath)) {
                    const char* name = std::strrchr(shotPath.c_str(), '/');
                    toast("SCREENSHOT", name ? name + 1 : shotPath.c_str());
                    evo_bt("screenshot: wrote %s", shotPath.c_str());
                    evo_boot_log_flush();
                    evo_feedback(EVO_FB_CONFIRM);
                } else {
                    toast("SCREENSHOT", "Capture failed");
                    evo_feedback(EVO_FB_BOUNDARY);
                }
                pressed &= ~(PadButtons::L3 | PadButtons::R3);
            }

            if (evo_keyboard_is_open()) {
                evo_keyboard_update();
                if (pressed) {
                    evo_keyboard_handle_input(pressed);
                }
            } else {
                if (hasInput) {
                    m_screenManager->handleInput(pressed, held, released);
                }
            }

            lastButtons = padData.buttons;
        }

        /*
         * 2. Update animation engine and screen logic.
         *
         * Measured, not assumed: a hardcoded 16.667 ran every animation in
         * slow motion whenever the loop dropped below 60 (a seek settle sits
         * nearer 27). Clamped so a long stall - a file open, a thumbnail
         * decode - cannot teleport an animation to its end.
         */
        double frameDeltaMs = 16.667;
        {
            static uint64_t s_lastTickUs = 0;
            struct timeval tvNow;
            gettimeofday(&tvNow, nullptr);
            uint64_t nowUs = static_cast<uint64_t>(tvNow.tv_sec) * 1000000ULL +
                             static_cast<uint64_t>(tvNow.tv_usec);
            if (s_lastTickUs != 0) {
                frameDeltaMs = static_cast<double>(nowUs - s_lastTickUs) / 1000.0;
                if (frameDeltaMs < 1.0)   frameDeltaMs = 1.0;
                if (frameDeltaMs > 100.0) frameDeltaMs = 100.0;
            }
            s_lastTickUs = nowUs;
        }
        evo::animation::AnimationManager::getInstance().update(frameDeltaMs);
        m_screenManager->update(frameDeltaMs);

        // 3. Determine if graphics needs to render/present
        bool isPlayer = (m_screenManager->getCurrentScreenId() == ScreenId::Player);
        static bool s_was_player = false;
        if (isPlayer != s_was_player) {
            evo_agc_runtime_set_player_mode(isPlayer ? 1 : 0);
            s_was_player = isPlayer;
        }
        bool hasAnim = evo::animation::AnimationManager::getInstance().hasActiveAnimations();
        int uiActive = (frame < 10) || isPlayer || hasInput || hasAnim || evo_rmlui_needs_frame() || (jb_repaint > 0);
        if (jb_repaint > 0) jb_repaint--;
        evo_rmlui_set_active(uiActive);

        if (frame < 5) {
            evo_bt("frame %d: uiActive=%d isPlayer=%d hasInput=%d", frame, uiActive, isPlayer ? 1 : 0, hasInput ? 1 : 0);
            evo_boot_log_flush();
        }

        if (uiActive && !isPlayer) {
            if (frame < 5) {
                evo_bt("frame %d: frame_begin", frame);
                evo_boot_log_flush();
            }
            evo_agc_runtime_frame_begin();
            if (frame < 5) {
                evo_bt("frame %d: frame_begin done", frame);
                evo_boot_log_flush();
            }
            /*
             * Only the CPU rasteriser reads this buffer back. On the console
             * RmlUi renders through the AGC interface and RenderCachedScreen
             * does `(void)framebuffer`, so the 8.3 MB clear was pure cost on
             * every menu frame. The allocation stays - the render entry points
             * still reject a null pointer.
             */
            if (m_uiScratch && evo_rmlui_blit_mode()) {
                std::memset(m_uiScratch, 0, static_cast<size_t>(DisplayWidth) * DisplayHeight * 4u);
            }
        }

        // 4. Video Quad blit & Screen rendering
        bool swap = false;
        static int64_t s_last_pts = -1;
        if (!isPlayer) {
            s_last_pts = -1;
        }

        if (isPlayer) {
            pp_video_frame f;
            std::memset(&f, 0, sizeof(f));
            int have = (pp_playback_get_video_frame(&g_pp_pb, &f) && f.ready);
            int64_t current_pts = g_pp_pb.display_pts_us;
            bool new_frame = (current_pts != s_last_pts);

            /* isPlayer came from getCurrentScreenId(), so the type is already
             * proved - no need to pay for RTTI once a frame. */
            auto playerScreen = static_cast<PlayerScreen*>(m_screenManager->getCurrentScreen());
            bool overlay_active = playerScreen && playerScreen->hasActiveOverlay();
            bool is_paused = m_playbackController && m_playbackController->isPaused();
            bool is_scrubbing = m_playbackController && m_playbackController->isScrubbing();
            bool toast_visible = evo_toast_visible();

            /*
             * Redraw the quad whenever the buffer we are about to draw into
             * does not already hold this frame. Two scanout buffers and a
             * player-mode frame_begin that skips the clear mean a present that
             * skipped the blit shows the picture from two presents ago, so any
             * source slower than the panel - 24/25/30 fps, a paused picture,
             * the settle after a seek - flicked between the current frame and
             * the previous one. See evo_agc_runtime_video_slot_stale.
             */
            bool buffer_stale = have && evo_agc_runtime_video_slot_stale(current_pts);

            bool should_render = (have && new_frame) || buffer_stale || overlay_active || is_paused || is_scrubbing || toast_visible || g_pp_pb.seek_discarding;

            static int s_player_render_log = 10;
            if (s_player_render_log > 0 && new_frame && have) {
                s_player_render_log--;
                evo_boot_log("app video blit: have=%d ready=%d new_frame=%d pts=%lld y=%p uv=%p %ux%u (coded %ux%u)",
                             have, f.ready, new_frame, (long long)current_pts,
                             (void*)f.y, (void*)f.uv, f.disp_w, f.disp_h, f.coded_w, f.coded_h);
                evo_boot_log_flush();
            }

            if (should_render && have) {
                s_last_pts = current_pts;
                int view_mode = 0;
                if (m_playbackController) {
                    view_mode = static_cast<int>(m_playbackController->getViewMode());
                }
                int is_direct = (evo_pb_active_backend() == EVO_VDEC_BACKEND_NATIVE && !f.held && f.uv != nullptr) ? 1 : 0;
                evo_agc_blit_yuv(f.y, f.y_pitch, f.uv, f.uv_pitch,
                                 f.u, f.u_pitch, f.v, f.v_pitch,
                                 static_cast<int>(f.coded_w), static_cast<int>(f.coded_h),
                                 static_cast<int>(f.disp_w), static_cast<int>(f.disp_h),
                                 view_mode, f.ten_bit, f.color_trc,
                                 is_direct, current_pts);
                swap = true;
            }

            if (m_uiScratch && should_render) {
                m_screenManager->render(m_uiScratch, DisplayWidth, DisplayHeight);
                /*
                 * The video quad only repaints the image. Any OSD, scrub bar or
                 * subtitle drawn on top of it - or over a letterbox bar - has to
                 * be erased before this buffer is used again, or the next frame
                 * draws on top of it: subtitles stacked line on line and the OSD
                 * stayed on screen after it faded out.
                 */
                if (overlay_active || is_paused || is_scrubbing || toast_visible) {
                    evo_agc_runtime_note_ui_drawn();
                }
                swap = true;
            }
        } else {
            if (m_uiScratch) {
                if (frame < 5) {
                    evo_bt("frame %d: screenManager render begin", frame);
                    evo_boot_log_flush();
                }
                m_screenManager->render(m_uiScratch, DisplayWidth, DisplayHeight);
                if (frame < 5) {
                    evo_bt("frame %d: screenManager render done", frame);
                    evo_boot_log_flush();
                }
            }

            // 5. Present if uiActive
            if (uiActive && m_uiScratch) {
                g_ps5_video_out_hdr = 0;
                if (evo_rmlui_blit_mode()) {
                    if (frame < 5) {
                        evo_bt("frame %d: agc_composite_bgra begin", frame);
                        evo_boot_log_flush();
                    }
                    evo_agc_composite_bgra(m_uiScratch, DisplayWidth, DisplayHeight, 1);
                    if (frame < 5) {
                        evo_bt("frame %d: agc_composite_bgra done", frame);
                        evo_boot_log_flush();
                    }
                    swap = true;
                } else {
                    swap = evo_rmlui_consume_drew();
                }
            }
        }

        // 5. Present if swap requested
        if (swap && uiActive) {
            if (frame < 5) {
                evo_bt("frame %d: present begin", frame);
                evo_boot_log_flush();
            }
            static int s_present_player_log = 10;
            bool log_this_present = false;
            if (isPlayer && s_present_player_log > 0) {
                s_present_player_log--;
                log_this_present = true;
                evo_boot_log("app present begin isPlayer=1");
                evo_boot_log_flush();
            }
            evo_agc_runtime_present();
            evo_rmlui_end_frame();
            if (log_this_present) {
                evo_boot_log("app present done isPlayer=1");
                evo_boot_log_flush();
            }
            if (frame < 5) {
                evo_bt("frame %d: present done", frame);
                evo_boot_log_flush();
            }
        }

        evo_perf_monitor_tick(0.0);
        if (!swap) {
            /*
             * Nothing was presented. A 1 ms spin here meant an idle menu ran
             * this loop ~1000 times a second, and every pass rebuilt each
             * screen's RmlUi parameter block - strings and all - for a frame
             * RenderCachedScreen then discarded because the UI was inactive.
             * Idle backs off to display cadence; an active frame that simply
             * had nothing to draw still retries promptly.
             */
            usleep(uiActive ? 1000 : 8000);
        }
    }

    shutdown();
    return 0;
}

} // namespace evo
