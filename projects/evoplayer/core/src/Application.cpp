#include "evo/Application.hpp"
#include "evo/screens/LaunchScreen.hpp"
#include "evo/screens/BrowserScreen.hpp"
#include "evo/screens/PlayerScreen.hpp"
#include "evo/screens/SettingsScreen.hpp"
#include "evo/screens/SubtitlePickerScreen.hpp"
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
#include "evo_boot_trace.h"
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
#include "evo_gl_context.h"
#include "evo_rmlui_bridge.h"
#include "evo_perf_monitor.h"
#include "evo_vdec.h"

#if defined(EVO_AGC_DEVICE)
#include "evo_agc_runtime.h"
#endif

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
#if defined(EVO_AGC_DEVICE)
    evo_bt("AGC: bare-metal AGC device context");
    int agcOk = evo_agc_runtime_init(DisplayWidth, DisplayHeight, 0);
    if (agcOk == 0) {
        evo_agc_runtime_frame_begin();
        evo_agc_runtime_present();
    }
#else
    evo_bt("GL-3: device GL context");
    int glOk = evo_gl_context_create(DisplayWidth, DisplayHeight);
    if (glOk) {
        evo_gl_frame_begin();
        evo_gl_context_present();
        evo_gl_warm();
    }
#endif
    evo_keyboard_ime_probe();
    evo_boot_log_flush();
#endif

    evo_jailbreak_self();
    evo_boot_log_flush();

    av_force_cpu_flags(0);
    evo_direct_mem_init(EVO_DIRECT_MEM_POOL_BYTES);
    EnsureDataDirectories();

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

    m_glScratch = static_cast<uint32_t*>(calloc(static_cast<size_t>(DisplayWidth) * DisplayHeight, 4));
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

#if defined(EVO_AGC_DEVICE)
    evo_agc_runtime_shutdown();
#else
    evo_gl_context_destroy();
#endif

    if (m_glScratch) {
        free(m_glScratch);
        m_glScratch = nullptr;
    }

    m_appFsm.postEvent(ApplicationEvent::ShutdownComplete);
}

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

        // 2. Update animation engine and screen logic
        evo::animation::AnimationManager::getInstance().update(16.667);
        m_screenManager->update(16.667);

        // 3. Determine if graphics needs to render/present
        bool isPlayer = (m_screenManager->getCurrentScreenId() == ScreenId::Player);
        bool hasAnim = evo::animation::AnimationManager::getInstance().hasActiveAnimations();
        int glActive = (frame < 10) || isPlayer || hasInput || hasAnim || evo_rmlui_gl_needs_frame() || (jb_repaint > 0);
        if (jb_repaint > 0) jb_repaint--;
        evo_rmlui_gl_set_active(glActive);

        if (frame < 5) {
            evo_bt("frame %d: glActive=%d isPlayer=%d hasInput=%d", frame, glActive, isPlayer ? 1 : 0, hasInput ? 1 : 0);
            evo_boot_log_flush();
        }

        if (glActive) {
            if (frame < 5) {
                evo_bt("frame %d: frame_begin", frame);
                evo_boot_log_flush();
            }
#if defined(EVO_AGC_DEVICE)
            evo_agc_runtime_frame_begin();
#else
            evo_gl_frame_begin();
#endif
            if (frame < 5) {
                evo_bt("frame %d: frame_begin done", frame);
                evo_boot_log_flush();
            }
            if (m_glScratch && !isPlayer) {
                std::memset(m_glScratch, 0, static_cast<size_t>(DisplayWidth) * DisplayHeight * 4u);
            }
        }

        // 4. Video Quad blit & Screen rendering
        if (isPlayer) {
            pp_gl_nv12_frame f;
            std::memset(&f, 0, sizeof(f));
            int have = (pp_playback_get_nv12(&g_pp_pb, &f) && f.ready);
            if (have) {
                int view_mode = 0;
                if (m_playbackController) {
                    view_mode = static_cast<int>(m_playbackController->getViewMode());
                }
#if defined(EVO_AGC_DEVICE)
                int is_direct = (evo_pb_active_backend() == EVO_VDEC_BACKEND_NATIVE && !f.held && f.uv != nullptr) ? 1 : 0;
                evo_agc_blit_yuv(f.y, f.y_pitch, f.uv, f.uv_pitch,
                                 f.u, f.u_pitch, f.v, f.v_pitch,
                                 static_cast<int>(f.coded_w), static_cast<int>(f.coded_h),
                                 static_cast<int>(f.disp_w), static_cast<int>(f.disp_h),
                                 view_mode, f.ten_bit, f.color_trc,
                                 is_direct);
#else
                evo_gl_blit_yuv(f.y, f.y_pitch, f.uv, f.uv_pitch,
                                f.u, f.u_pitch, f.v, f.v_pitch,
                                static_cast<int>(f.coded_w), static_cast<int>(f.coded_h),
                                static_cast<int>(f.disp_w), static_cast<int>(f.disp_h),
                                view_mode, f.ten_bit, f.color_trc);
#endif
            }
        }

        if (m_glScratch) {
            if (frame < 5) {
                evo_bt("frame %d: screenManager render begin", frame);
                evo_boot_log_flush();
            }
            m_screenManager->render(m_glScratch, DisplayWidth, DisplayHeight);
            if (frame < 5) {
                evo_bt("frame %d: screenManager render done", frame);
                evo_boot_log_flush();
            }
        }

        // 5. Present if glActive
        if (glActive && m_glScratch) {
            bool swap = true;
            if (isPlayer) {
#if !defined(EVO_AGC_DEVICE)
                if (evo_rmlui_gl_blit_mode()) {
                    evo_gl_blit_bgra(m_glScratch, DisplayWidth, DisplayHeight);
                } else {
                    evo_gl_composite_bgra(m_glScratch, DisplayWidth, DisplayHeight, 1);
                }
#endif
            } else {
                g_ps5_video_out_hdr = 0;
                if (evo_rmlui_gl_blit_mode()) {
                    if (frame < 5) {
                        evo_bt("frame %d: evo_gl_blit_bgra begin", frame);
                        evo_boot_log_flush();
                    }
                    evo_gl_blit_bgra(m_glScratch, DisplayWidth, DisplayHeight);
                    if (frame < 5) {
                        evo_bt("frame %d: evo_gl_blit_bgra done", frame);
                        evo_boot_log_flush();
                    }
                } else {
                    swap = evo_rmlui_gl_consume_drew();
                }
            }

            if (swap) {
                if (frame < 5) {
                    evo_bt("frame %d: present begin", frame);
                    evo_boot_log_flush();
                }
#if defined(EVO_AGC_DEVICE)
                evo_agc_runtime_present();
                evo_rmlui_gl_end_frame();
#else
                evo_gl_context_present();
                evo_rmlui_gl_end_frame();
#endif
                if (frame < 5) {
                    evo_bt("frame %d: present done", frame);
                    evo_boot_log_flush();
                }
            }
        }

        evo_perf_monitor_tick(0.0);
        usleep(isPlayer ? 3000 : 2000);
    }

    shutdown();
    return 0;
}

} // namespace evo
