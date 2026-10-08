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
#include "evo/screens/ProviderHostScreen.hpp"
#include "evo/screens/ModalDialogScreen.hpp"
#include "evo/screens/RecentFilesScreen.hpp"
#include "evo/screens/FavoritesScreen.hpp"
#include "evo/screens/AboutSupportScreen.hpp"
#include "evo/screens/SafeToCloseScreen.hpp"
#include "evo/screens/ImageViewerScreen.hpp"
#include "evo/screens/ChangelogScreen.hpp"
#include "evo/animation/AnimationManager.hpp"

#include "evo_boot_log.h"
#include "evo_stream_io.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <sys/time.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "evo_agc_runtime.h"
#include "evo_sweep.h"
#include "evo_toast.h"
#include <chrono>
#include <atomic>
#include <cctype>
#include <pthread.h>
extern "C" void evo_log_alloc_state(const char *when);  /* PlaybackController.cpp */

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
#include "evo_speaker_cal.h"
#include "evo_toast.h"
#include "evo_recent.h"
#include "evo_favorites.h"
#include "evo_theme.h"
#include "evo_keyboard.h"
#include "evo_feedback.h"
#include "evo_input.h"
#include "evo_net.h"
#include "evo_subtitle.h"
#include "evo_usb_remote.h"
#include "evo_webui.h"
/* #90: providers are reached through the vtable registry, never by name.
 * addon_emby.h is gone from here - provider_emby.c is the only thing that
 * includes it now. */
#include "evo_provider.h"
#include "evo_provider_bundle.h"
/* For the provider host's teardown ordering in shutdown(); the C entry points
 * live beside the class that implements them. */
#include "evo_rmlui_provider.h"
#include "pp_playback.h"
#include "evo_playback.h"
#include "evo_adec.h"
#include "evo_hw.h"
#include "evo_rmlui_bridge.h"
#include "evo_perf_monitor.h"
#include "evo_vdec.h"

#include "evo_agc_runtime.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/cpu.h>
#include <libavutil/log.h>
#include <sys/stat.h>
#include <unistd.h>

int sceUserServiceInitialize(void *);
int sceUserServiceGetLoginUserIdList(int userId[4]);
int scePadInit(void);
int scePadOpen(int, int, int, void *);

typedef struct PS5_PadTouch {
    uint16_t x;
    uint16_t y;
    uint8_t id;
    uint8_t reserve[3];
} PS5_PadTouch;

typedef struct PS5_PadTouchData {
    uint8_t touchNum;
    uint8_t reserve[3];
    uint32_t time_since_touch_held_down;
    PS5_PadTouch touch[2];
} PS5_PadTouchData;

typedef struct PS5_PadData {
    uint32_t buttons;
    struct { uint8_t x; uint8_t y; } leftStick;
    struct { uint8_t x; uint8_t y; } rightStick;
    struct { uint8_t l2; uint8_t r2; uint8_t padding[2]; } analogButtons;
    float orientation[4];
    float acceleration[3];
    float angularVelocity[3];
    PS5_PadTouchData touchData;
    uint8_t rest[72];
} PS5_PadData;

static_assert(offsetof(PS5_PadData, touchData) == 52, "touchData offset mismatch");
static_assert(offsetof(PS5_PadData, touchData.touch[0].x) == 60, "touch[0].x offset mismatch");
static_assert(offsetof(PS5_PadData, touchData.touch[1].x) == 68, "touch[1].x offset mismatch");

int scePadReadState(int, PS5_PadData *);

extern pp_playback g_pp_pb;
extern int g_ps5_user_id;
extern int g_ps5_video_out_hdr;
extern int screen;
long long now_ms(void);
}

#ifndef EVO_PLAYER_VERSION
#define EVO_PLAYER_VERSION "v0.11.0"
#endif

#ifndef EVO_DIRECT_MEM_POOL_BYTES
#define EVO_DIRECT_MEM_POOL_BYTES (128 * 1024 * 1024)
#endif

extern "C" { volatile sig_atomic_t g_evo_term_requested = 0; }

#if defined(EVO_APP_MODULE) || defined(EVO_TARGET_PS5)
/* libSceSystemService, linked from the SDK stub - the link line already passes
 * --as-needed over the whole stub directory, so no PRX .syms entry is needed
 * (and a .syms line nothing imports would brick the module load). */
extern "C" int sceSystemServiceHideSplashScreen(void);
#endif

namespace evo {

static evo_input evo_pad_state;

/*
 * Route FFmpeg's logging into evo.log, and away from stderr.
 *
 * THIS IS A CRASH FIX, not a diagnostic nicety.
 *
 * av_log's default callback ends in fputs(str, stderr) - libavutil/log.c,
 * colored_fputs(). Nothing in this binary has ever written to stderr: the boot
 * log opens /mnt/usb0/evo.log itself and klog goes through
 * sceKernelDebugOutText, so on the native-app CRT stderr had never once been
 * touched, and touching it dereferences null. SIGSEGV, addr=0, no diagnostic.
 *
 * That is why crashes only ever happened on files that make FFmpeg *log*
 * something. A clean MP4 or MKV probes silently and hundreds of them worked
 * fine; an MPEG-TS reliably logs "start time for stream N is not set in
 * estimate_timings_from_pts" and died every single time, on three builds, with
 * that string still sitting in the register dump. The same dumps from
 * EVO_TEST_hevc8_4k.mp4 held "Error parsing NAL unit #0." - also an av_log
 * message. Each of those was read as a clue to where the code had got to; they
 * were the fault itself.
 *
 * Second prize: FFmpeg's own diagnostics finally land somewhere readable.
 * Several comments in this tree complain that libavcodec "writes to its own
 * av_log, which goes nowhere here" - it went to a null stderr.
 *
 * WARNING and above only, so a stream with per-frame complaints cannot flood
 * the log or slow the decode thread down.
 */
static void EvoAvLogCallback(void* avcl, int level, const char* fmt, va_list vl) {
    (void)avcl;
    if (level > AV_LOG_WARNING)
        return;
    char line[512];
    int n = vsnprintf(line, sizeof line, fmt, vl);
    if (n <= 0)
        return;
    if (n >= (int)sizeof line)
        n = (int)sizeof line - 1;
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    if (n == 0)
        return;
    evo_bt("ffmpeg[%d]: %s", level, line);
}

static void evo_av_log_init() {
    av_log_set_callback(EvoAvLogCallback);
    av_log_set_level(AV_LOG_WARNING);
}

static void SoundEffectCallback(int sfxKind) {
    if (auto sfx = Application::getInstance().getSoundEffectEngine()) {
        sfx->playSound(static_cast<SoundEffect>(sfxKind));
    }
}

static void EnsureDataDirectories() {
    mkdir("/data", 0777);
    mkdir(evo_data_dir(), 0777);
}

/*
 * Deliberately never destroyed.
 *
 * A function-local static is destroyed at exit, and that is where QUIT EVO was
 * dying on 2026-09-18 - the breadcrumbs showed Application::shutdown() running
 * to "shutdown: complete", main() returning, and the process then faulting
 * while the static destructor chain unwound. ~Application() re-entered a
 * teardown that had already happened (its own shutdown() is guarded, but the
 * member unique_ptrs are not: ~PlaybackController ran stopPlayback() a second
 * time, which is the stats block that appears after "shutdown: complete"), and
 * the destruction order across these singletons is not something this process
 * needs to get right.
 *
 * By the time main() returns, shutdown() has already released everything that
 * actually matters - GPU, VideoOut, the direct-memory pool, decoders, audio -
 * in a verified order. What remains is C++ object destruction in a process
 * that is about to stop existing, so it buys nothing and costs a crash. The
 * heap "leak" is reclaimed with the process.
 *
 * This is the leaky-singleton form, not _exit(): main() still returns
 * normally, which the app module requires.
 */
Application& Application::getInstance() {
    static Application* s_instance = new Application();
    return *s_instance;
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

/* See the note on the declaration in Application.hpp. */
void Application::requestSoftClose() {
    if (m_softClose)
        return;
    m_softClose = true;
    m_softCloseFrames = 0;
    evo_boot_log("soft close: requested");
    evo_boot_log_flush();
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
    /* #103: PS5 Pro detection. Cached; the upscaler's default network size,
     * the PSML gate and the diagnostics screens all read it later. */
    evo_hw_probe();

    if (!initHardware()) {
        return false;
    }
    m_appFsm.postEvent(ApplicationEvent::HardwareReady);

    if (!initServices()) {
        return false;
    }
    m_appFsm.postEvent(ApplicationEvent::ServicesReady);

    if (m_settingsService && m_settingsService->getRefreshRateMode() == RefreshRateMode::Always) {
        if (evo_agc_runtime_supports_120hz()) {
            evo_bt("120Hz: enabling Always mode at boot");
            evo_agc_runtime_set_120hz(1);
        }
    }


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
    /* #101: the system browser (evo_webui.c). Same pre-unjail rule as the
     * IME - the dialog's sysmodule load fails after the credential swap. */
    evo_webui_preload();
    evo_boot_log_flush();
#endif

    evo_jailbreak_self();
    evo_boot_log_flush();

    evo_av_log_init();
    av_force_cpu_flags(0);
    evo_direct_mem_init(EVO_DIRECT_MEM_POOL_BYTES);
    /* The baseline, before anything has been decoded or drawn. Every later
     * figure is only meaningful against this one. */
    evo_mem_budget_log("boot");
    /* Follow-up to #94: how much of the ~11 GB of direct memory EVO can really
     * take, measured after the GPU runtime and resident decoders hold theirs.
     * The trigger file's content is the ceiling in MB (default 8192). */
    {
        FILE *pf = std::fopen("/mnt/usb0/evo_dm_probe", "r");
        if (pf) {
            long max_mb = 0;
            if (std::fscanf(pf, "%ld", &max_mb) != 1 || max_mb <= 0)
                max_mb = 8192;
            std::fclose(pf);
            evo_direct_mem_probe(static_cast<size_t>(256) << 20,
                                 static_cast<size_t>(max_mb) << 20);
            evo_boot_log_flush();
        }
    }
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
    /*
     * The three screen footers (list, browser, changelog) render m_version,
     * but only behind `if (!m_version.empty())` - and nothing in the app ever
     * called this, so m_version was always empty and all three fell through to
     * the literal baked into the .rml. They had been showing v0.7.1 ever since
     * that string was last hand-edited. Only the host tools set it, which is
     * why uiview looked right while the console did not.
     */
    const char* app_ver = (EVO_PLAYER_VERSION[0] == 'v' || EVO_PLAYER_VERSION[0] == 'V')
                          ? EVO_PLAYER_VERSION
                          : ("v" EVO_PLAYER_VERSION);
    evo_rmlui_set_version(app_ver);

    evo_input_reset(&evo_pad_state);
    evo_feedback_init(m_padHandle, SoundEffectCallback);

    evo_net_init();
    /*
     * #90: bring up the provider registry. It calls each provider's init(),
     * which loads persisted credentials and touches no network - this runs
     * during boot, before the self-unjail has necessarily opened /data, and a
     * DNS lookup here would stall the splash.
     *
     * emby_init() is now reached through the registry (provider_emby.c), so it
     * is no longer called by name. Nothing else in the tree knows the word
     * "emby" any more, which is the whole point of the seam.
     */
    evo_provider_mgr_init();
    /*
     * avformat_network_init() was already here and was a no-op for the whole
     * 0.6-0.10 era, because the production FFmpeg profile was built
     * --disable-network. It does something now (#90 scope 1).
     */
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
    m_fileTransferService = std::make_unique<FileTransferService>();
    m_screenManager = std::make_unique<ScreenManager>();

    m_soundEffectEngine->initialize();
    m_settingsService->loadSettings();
    evo_speaker_cal_load();
    recent_load();
    favorites_load();
    m_fileSystemBrowser->loadLastFolder();

    toast("EVO Player", (EVO_PLAYER_VERSION[0] == 'v' || EVO_PLAYER_VERSION[0] == 'V')
                         ? EVO_PLAYER_VERSION
                         : ("v" EVO_PLAYER_VERSION));
    return true;
}

bool Application::initScreens() {
    m_screenManager->registerScreen(std::make_unique<LaunchScreen>());
    m_screenManager->registerScreen(std::make_unique<BrowserScreen>());
    m_screenManager->registerScreen(std::make_unique<PlayerScreen>());
    m_screenManager->registerScreen(std::make_unique<SettingsScreen>());
    m_screenManager->registerScreen(std::make_unique<SettingsSectionScreen>(ScreenId::SettingsPlayback));
    m_screenManager->registerScreen(std::make_unique<SettingsSectionScreen>(ScreenId::SettingsAudio));
    m_screenManager->registerScreen(std::make_unique<SettingsSectionScreen>(ScreenId::SettingsSubtitles));
    m_screenManager->registerScreen(std::make_unique<SettingsSectionScreen>(ScreenId::SettingsInterface));
    m_screenManager->registerScreen(std::make_unique<SettingsSectionScreen>(ScreenId::SettingsSystem));
    m_screenManager->registerScreen(std::make_unique<SettingsSectionScreen>(ScreenId::SettingsExperimental));
    m_screenManager->registerScreen(std::make_unique<SubtitlePickerScreen>());
    m_screenManager->registerScreen(std::make_unique<AudioTrackPickerScreen>());
    m_screenManager->registerScreen(std::make_unique<TextReaderScreen>());
    m_screenManager->registerScreen(std::make_unique<MediaInfoScreen>());
    m_screenManager->registerScreen(std::make_unique<SurroundTestScreen>());
    m_screenManager->registerScreen(std::make_unique<DeveloperToolsScreen>());
    m_screenManager->registerScreen(std::make_unique<EmbyScreen>());
    /* #90: the generic provider host. Fills ScreenId::EmbyBrowse, which had an
     * id and a rail path but no class registered at all. */
    m_screenManager->registerScreen(std::make_unique<ProviderHostScreen>());
    m_screenManager->registerScreen(std::make_unique<RecentFilesScreen>());
    m_screenManager->registerScreen(std::make_unique<FavoritesScreen>());
    m_screenManager->registerScreen(std::make_unique<AboutSupportScreen>());
    m_screenManager->registerScreen(std::make_unique<SafeToCloseScreen>());
    m_screenManager->registerScreen(std::make_unique<ChangelogScreen>());
    m_screenManager->registerScreen(std::make_unique<ImageViewerScreen>());
    m_screenManager->registerScreen(std::make_unique<ModalDialogScreen>(ModalType::ExitConfirm));
    m_screenManager->registerScreen(std::make_unique<ModalDialogScreen>(ModalType::ResumePrompt));

    m_screenManager->navigateTo(ScreenId::MainMenu);
    return true;
}

/*
 * Runs at most once, and tears down in dependency order.
 *
 * Two things were wrong here, both found by the first QUIT EVO on hardware
 * (2026-09-18) crashing after the pp_playback stats and before any AGC line:
 *
 * 1. It ran more than once. ~Application() calls it too, and Application is a
 *    singleton, so the destructor fires during static destruction after main()
 *    has returned - by which point run() had already torn everything down. The
 *    log showed the pp_playback stats block three times over. Nothing here was
 *    written to be re-entrant, so it is now guarded rather than every step
 *    being made individually idempotent.
 *
 * 2. It released the GPU before the UI that was using it. evo_rmlui_shutdown()
 *    existed but had no caller anywhere in the tree, so RmlUi kept its render
 *    interface, its compiled geometry and its textures while
 *    evo_agc_runtime_shutdown() closed VideoOut and unmapped the pool - and
 *    then unwound at static-destruction time against a runtime that was gone.
 *    The UI now goes first, while the runtime that owns its allocator is still
 *    alive to service the frees.
 *
 * Breadcrumbs at every step and flushed as they go: this path had no
 * instrumentation, so its first failure could only be located by what was
 * missing from the log.
 */
void Application::shutdown() {
    if (m_shutdownDone)
        return;
    m_shutdownDone = true;

    m_running = false;

    evo_boot_log("shutdown: begin");
    evo_boot_log_flush();

    if (m_playbackController) {
        m_playbackController->stopPlayback();
    }
    if (m_surroundTestService) {
        m_surroundTestService->stop();
    }
    if (m_soundEffectEngine) {
        m_soundEffectEngine->shutdown();
    }
    evo_boot_log("shutdown: media stopped");
    evo_boot_log_flush();

    /* #101: the web UI's proxy threads and the browser subsystem. */
    evo_webui_shutdown();

    /*
     * #90: providers down before RmlUi.
     *
     * The provider host holds an Rml context and textures in the render
     * interface's registry, so it has to let go of them while Rml is still
     * alive - evo_rmlui_shutdown() below runs Rml::Shutdown(), after which
     * closing a context is a use-after-free. Same ordering constraint the
     * comment above evo_rmlui_shutdown() describes for the AGC runtime.
     */
    if (evo_rmlui_provider_is_open())
        evo_rmlui_provider_close();
    evo_provider_art_clear();
    evo_provider_mgr_shutdown();
    evo_boot_log("shutdown: providers down");
    evo_boot_log_flush();

    pp_playback_shutdown(&g_pp_pb);
    evo_boot_log("shutdown: pp_playback down");
    evo_boot_log_flush();

    /* Before the AGC runtime - see (2) above. */
    evo_rmlui_shutdown();
    evo_boot_log("shutdown: rmlui down");
    evo_boot_log_flush();

    evo_agc_runtime_shutdown();
    evo_boot_log("shutdown: agc runtime down");
    evo_boot_log_flush();

    if (m_uiScratch) {
        free(m_uiScratch);
        m_uiScratch = nullptr;
    }

    m_appFsm.postEvent(ApplicationEvent::ShutdownComplete);
    evo_boot_log("shutdown: complete");
    evo_boot_log_flush();
}

namespace {

#if defined(EVO_APP_MODULE)
/*
 * #101: a page in the system browser handed a stream over (evo_webui.c).
 * Play it the way ProviderHostScreen does - startPlaybackSource() blocks on
 * network I/O, so it runs on a worker and the Player screen follows once it
 * has started - then tell the probe when playback is over so it can reopen
 * the page.
 */
PlaybackSource g_web_src;
pthread_t g_web_thread;
std::atomic<bool> g_web_done{false};
std::atomic<bool> g_web_ok{false};

void* web_start_worker(void*) {
    IPlaybackController* pb = Application::getInstance().getPlaybackController();
    evo_stream_headers[0] = '\0';       /* a provider's headers must not follow us here */
    evo_stream_user_agent[0] = '\0';
    g_web_ok = pb && pb->startPlaybackSource(g_web_src, 0.0);
    g_web_done = true;
    return nullptr;
}

/*
 * Watched state and resume points back to Emby / Jellyfin. The page hands
 * over a server URL that already names the item (/Videos/<id>/...) and
 * carries the user's api_key, so EVO can make the same three session calls
 * the server's own player makes. The server decides "played" from the
 * position in the Stopped report, exactly as for its own player. A URL
 * without both (Nuvio, a plain stream) reports nothing.
 */
struct WebReport {
    bool on = false;
    std::string base;       /* scheme://host:port[/emby] */
    std::string item;
    std::string key;
    std::string mediaSource;
    std::string playSession;
    double lastPos = 0.0;
    int frames = 0;
};
WebReport g_web_rep;

std::string url_lower(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}

/* The value of query parameter `name` (matched case-insensitively). */
std::string url_param(const std::string& url, const char* name) {
    const std::string low = url_lower(url);
    const std::string want = url_lower(name) + "=";
    size_t q = low.find('?');
    while (q != std::string::npos) {
        size_t at = q + 1;
        if (low.compare(at, want.size(), want) == 0) {
            size_t v = at + want.size();
            size_t e = url.find_first_of("&#", v);
            return url.substr(v, e == std::string::npos ? std::string::npos : e - v);
        }
        q = low.find('&', at);
    }
    return std::string();
}

void web_report_begin(const std::string& url) {
    g_web_rep = WebReport();
    const std::string low = url_lower(url);
    size_t scheme = low.find("://");
    if (scheme == std::string::npos) return;
    size_t path = low.find('/', scheme + 3);
    if (path == std::string::npos) return;
    size_t v = low.find("/videos/", path);
    if (v == std::string::npos) v = low.find("/audio/", path);
    if (v == std::string::npos) return;
    size_t idStart = low.find('/', v + 1) + 1;
    size_t idEnd = url.find_first_of("/?#", idStart);
    std::string key = url_param(url, "api_key");
    if (key.empty()) key = url_param(url, "ApiKey");
    if (idEnd == std::string::npos || idEnd == idStart || key.empty()) return;

    g_web_rep.base = url.substr(0, path);
    if (low.compare(path, 6, "/emby/") == 0) g_web_rep.base += "/emby";
    g_web_rep.item = url.substr(idStart, idEnd - idStart);
    g_web_rep.key = key;
    g_web_rep.mediaSource = url_param(url, "MediaSourceId");
    if (g_web_rep.mediaSource.empty()) g_web_rep.mediaSource = g_web_rep.item;
    g_web_rep.playSession = url_param(url, "PlaySessionId");
    g_web_rep.on = true;
}

void web_report_cb(int success, int status, const char*, size_t, void* what) {
    evo_bt("web: report %s -> ok=%d http=%d", (const char*)what, success, status);
}

void web_report(const char* what, double pos) {
    if (!g_web_rep.on) return;
    char body[640];
    std::snprintf(body, sizeof body,
                  "{\"ItemId\":\"%s\",\"MediaSourceId\":\"%s\",\"PlaySessionId\":\"%s\","
                  "\"PositionTicks\":%lld,\"CanSeek\":true,\"PlayMethod\":\"DirectStream\"}",
                  g_web_rep.item.c_str(), g_web_rep.mediaSource.c_str(),
                  g_web_rep.playSession.c_str(), (long long)(pos * 10000000.0));
    std::string url = g_web_rep.base + "/Sessions/Playing" + what;
    std::string hdr = "X-Emby-Token: " + g_web_rep.key;
    const char* headers[1] = { hdr.c_str() };
    evo_net_request_async("POST", url.c_str(), body, headers, 1, web_report_cb,
                          (void*)(what[0] ? what : "/start"));
}

void webui_playback_pump() {
    static int phase = 0;           /* 0 idle, 1 starting, 2 playing */
    static int frames = 0;
    static bool seen_active = false;
    Application& app = Application::getInstance();

    if (phase == 0) {
        char url[2048], title[256];
        if (!evo_webui_take_play(url, sizeof url, title, sizeof title))
            return;
        g_web_src = PlaybackSource();
        g_web_src.url = url;
        g_web_src.title = title[0] ? title : "Web";
        g_web_src.provider = "web";
        g_web_done = false;
        g_web_ok = false;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
        int rc = pthread_create(&g_web_thread, &attr, web_start_worker, nullptr);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            evo_bt("web: playback worker pthread_create rc=%d", rc);
            evo_webui_playback_ended(0);
            return;
        }
        phase = 1;
    } else if (phase == 1) {
        if (!g_web_done)
            return;
        pthread_join(g_web_thread, nullptr);
        evo_bt("web: startPlaybackSource -> %s", g_web_ok ? "ok" : "failed");
        evo_boot_log_flush();
        if (!g_web_ok) {
            toast("STREAM", "The web page's stream would not open");
            evo_webui_playback_ended(0);
            phase = 0;
            return;
        }
        if (auto sm = app.getScreenManager())
            sm->navigateTo(ScreenId::Player);
        phase = 2;
        frames = 0;
        seen_active = false;
        web_report_begin(g_web_src.url);
        evo_bt("web: server reporting %s (item %s)", g_web_rep.on ? "on" : "off",
               g_web_rep.on ? g_web_rep.item.c_str() : "-");
        web_report("", 0.0);
    } else {
        IPlaybackController* pb = app.getPlaybackController();
        bool active = pb && pb->isActive();
        if (active) {
            seen_active = true;
            /* Kept while playing: the position is gone once playback stops,
             * and the Stopped report is what sets watched / resume. */
            double pos = pb->getPositionSeconds();
            if (pos > 0.0) g_web_rep.lastPos = pos;
            if (++g_web_rep.frames % 600 == 0)          /* ~10 s */
                web_report("/Progress", g_web_rep.lastPos);
        }
        if ((seen_active && !active) || (!seen_active && ++frames > 600)) {
            web_report("/Stopped", g_web_rep.lastPos);
            g_web_rep.on = false;
            evo_webui_playback_ended(seen_active ? 1 : 0);
            phase = 0;
        }
    }
}
#else
void webui_playback_pump() {}
#endif

#ifdef EVO_APP_MODULE
/*
 * A system notification, which is the only on-screen feedback available here.
 *
 * The app's own toast goes through a second Rml::Context and is disabled
 * (#75: it SIGSEGVs the plain .ffpfsc build on frame 1), so toast() draws
 * nothing and a screenshot gave no sign it had worked. This is the same
 * notification path --breadcrumbs uses for the boot trace; it is the
 * platform's, not RmlUi's, so it is unaffected.
 */
struct evo_sys_note { char pad[45]; char msg[3075]; };
extern "C" int sceKernelSendNotificationRequest(int, void *, unsigned long, int);

static void notifyOnScreen(const char* text) {
    evo_sys_note n;
    std::memset(&n, 0, sizeof n);
    std::snprintf(n.msg, sizeof n.msg, "%s", text);
    sceKernelSendNotificationRequest(0, &n, sizeof n, 0);
}
#else
static void notifyOnScreen(const char*) {}
#endif

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
static bool write_scanout_bmp(FILE* fp);

/*
 * #103 upscaler comparison (dev remote `upcompare`).
 *
 * Hand-taken screenshots can never land on the same frame, so this pauses and
 * redraws ONE held frame three times - upscaler Off, Sharp, AI - with the OSD
 * suppressed, capturing the scanout after each. The redraw is forced every
 * frame while it runs (the normal loop only redraws a buffer that does not
 * already hold this PTS, and a mode change does not change the PTS).
 *
 * Runs on the render thread: the remote only raises the request.
 */
namespace {
struct UpscaleCompare {
    int  state = 0;        /* 0 idle, -1 requested, 1 running */
    int  step = 0;         /* index into kModes */
    int  frames = 0;       /* presents since this step's mode was set */
    bool wasPaused = false;
    int  kind = 0;         /* 0 upscaler (#103), 1 Deep Blacks (#119) */
};
UpscaleCompare s_upcmp;
struct UpcmpMode { int mode; int net; };
constexpr UpcmpMode kUpcmpModes[] = {
    { EVO_AGC_UPSCALE_OFF,   EVO_AGC_UPNET_AUTO },
    { EVO_AGC_UPSCALE_SHARP, EVO_AGC_UPNET_AUTO },
    { EVO_AGC_UPSCALE_AI,    EVO_AGC_UPNET_STANDARD },
    { EVO_AGC_UPSCALE_AI,    EVO_AGC_UPNET_LARGE },
    { EVO_AGC_UPSCALE_AI,    EVO_AGC_UPNET_MAXIMUM },
};
constexpr const char* kUpcmpNames[] = { "off", "sharp", "ai", "ai_large", "ai_max" };
/* #119 `dbcompare`: the same held frame with Deep Blacks Off / Low / High. */
constexpr const char* kDbcmpNames[] = { "off", "low", "high" };
inline int upcmpSteps() { return s_upcmp.kind ? 3 : static_cast<int>(sizeof kUpcmpModes / sizeof kUpcmpModes[0]); }
/* The first step also waits out the pause settling; later ones only need both
 * scanout buffers redrawn in the new mode and flipped. */
constexpr int kUpcmpSettleFirst = 30;
constexpr int kUpcmpSettle = 8;
} // namespace

/* Leaves the file's anonymous namespace and evo for C linkage, then reopens
 * both. */
} // namespace
} // namespace evo

extern "C" void evo_remote_upscale_compare(void) {
    if (evo::s_upcmp.state == 0) {
        evo::s_upcmp.kind = 0;
        evo::s_upcmp.state = -1;
    }
}

extern "C" void evo_remote_deepblack_compare(void) {
    if (evo::s_upcmp.state == 0) {
        evo::s_upcmp.kind = 1;
        evo::s_upcmp.state = -1;
    }
}

extern "C" void evo_remote_soft_close(void) {
    evo::Application::getInstance().requestSoftClose();
}

namespace evo {
namespace {

bool evo_capture_screenshot(std::string& outPath) {
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
    if (!write_scanout_bmp(fp))
        return false;
    outPath = path;
    return true;
}

/* The front buffer as a 24-bit bottom-up BMP. Closes fp. */
static bool write_scanout_bmp(FILE* fp) {
    int w = 0, h = 0;
    evo_agc_runtime_get_size(&w, &h);
    if (w <= 0 || h <= 0) {
        evo_bt("screenshot: no render size (%dx%d)", w, h);
        evo_boot_log_flush();
        std::fclose(fp);
        return false;
    }

    std::vector<uint32_t> bgra(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
    evo_agc_runtime_read_scanout(bgra.data(), w, h);

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
             * Low byte is BLUE here, so it goes straight into the BMP's blue
             * slot.
             *
             * The UI composes colours as 0xAABBGGRR (MakeColorBgra), but the
             * scanout is registered COMP_SWAP=ALT - see the note on
             * EVO_AGC_LAYER_BYTES: "only the scanout uses COMP_SWAP=ALT for
             * BGRA" - so what lands in scanout memory is byte order B,G,R,A,
             * which read as a little-endian word is 0xAARRGGBB. I "corrected"
             * this to swap R and B and it was already right: every capture
             * after that came back with navy UI rendered brown.
             */
            const uint32_t px = src[x];
            row[x * 3 + 0] = (unsigned char)(px & 0xFF);           /* B */
            row[x * 3 + 1] = (unsigned char)((px >> 8) & 0xFF);    /* G */
            row[x * 3 + 2] = (unsigned char)((px >> 16) & 0xFF);   /* R */
        }
        std::fwrite(row.data(), 1, row.size(), fp);
    }
    std::fclose(fp);
    return true;
}

/* True when `hz` shows every frame of `fps` video for the same number of
 * refreshes (59.94 fps on 119.88 Hz: 2 each). */
static bool fits_rate(double fps, double hz)
{
    const double k = hz / fps;
    const double kr = static_cast<double>(static_cast<int>(k + 0.5));
    return kr >= 1.0 && std::fabs(k - kr) < 0.02 * kr;
}

/*
 * RefreshRateMode::MatchVideo: the output rate for `fps` video. 23.976 and 24
 * are told apart (0.024 fps apart; streams report them to well under 0.005),
 * since 24 fps on 23.976 Hz still drops a frame every 42 s. Film the display
 * cannot take natively goes to 119.88 Hz when it has it. Everything else
 * (29.97, 30, 59.94, 60) stays on the default rate.
 */
static evo_vo_rate match_video_rate(double fps, bool can120)
{
    evo_vo_rate want = EVO_VO_RATE_DEFAULT;
    if (std::fabs(fps - 24000.0 / 1001.0) < 0.008)
        want = EVO_VO_RATE_23_976;
    else if (std::fabs(fps - 24.0) < 0.008)
        want = EVO_VO_RATE_24;
    else if (fits_rate(fps, 50.0))
        want = EVO_VO_RATE_50;
    if (want != EVO_VO_RATE_DEFAULT && !evo_agc_runtime_supports_output_rate(want))
        want = (want != EVO_VO_RATE_50 && can120) ? EVO_VO_RATE_119_88 : EVO_VO_RATE_DEFAULT;
    return want;
}

/*
 * Playback frame-pacing trace - diagnostics only, no effect on playback.
 *
 * Every 5 s of playback it logs one "pace:" line: how many presents and new
 * video frames there were, how evenly the presents were spaced, what each
 * step of a player frame cost (video blit, UI, present incl. the flip wait),
 * and - the one that shows judder - for how many presents each video frame
 * stayed on screen. 59.94 fps on a 119.88 Hz output should be all "2"; a
 * "3" is a frame held one refresh longer than its neighbours.
 */
struct PaceTrace {
    using clk = std::chrono::steady_clock;
    clk::time_point window{}, last_present{};
    int presents = 0, frames = 0, since_frame = -1;
    int held[5] = {0};                     /* 1, 2, 3, 4, 5+ presents */
    int gap[4] = {0};                      /* <=12, 12-20, 20-30, >30 ms */
    double gap_sum = 0, gap_max = 0;
    double blit_sum = 0, blit_max = 0, ui_sum = 0, ui_max = 0, pres_sum = 0, pres_max = 0;
    int blits = 0, uis = 0;

    /* Where a whole loop iteration goes, phase by phase - so a stall outside
     * blit / ui / present can be pinned on its phase. */
    static constexpr int kPh = 8;
    clk::time_point ph_t{};
    double ph_cur[kPh] = {0}, ph_max[kPh] = {0};
    int slow_by[kPh] = {0};
    int iters = 0;

    void mark(int i) {
        const clk::time_point n = clk::now();
        if (ph_t != clk::time_point{}) ph_cur[i] += ms(ph_t, n);
        ph_t = n;
    }
    /* Loop top: close the previous iteration (the tail since present is
     * phase 7) and charge it to its slowest phase if it overran 30 ms. */
    void iter_begin() {
        mark(7);
        if (window == clk::time_point{}) {
            for (double& v : ph_cur) v = 0;
            return;
        }
        double total = 0;
        int worst = 0;
        for (int i = 0; i < kPh; ++i) {
            total += ph_cur[i];
            if (ph_cur[i] > ph_max[i]) ph_max[i] = ph_cur[i];
            if (ph_cur[i] > ph_cur[worst]) worst = i;
            ph_cur[i] = 0;
        }
        iters++;
        if (total > 30.0) slow_by[worst]++;
    }

    static double ms(clk::time_point a, clk::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    }
    static void add(double& sum, double& mx, double v) { sum += v; if (v > mx) mx = v; }

    void clear(clk::time_point now) {
        const clk::time_point lp = last_present, pt = ph_t;
        const int sf = since_frame;
        double cur[kPh];
        for (int i = 0; i < kPh; ++i) cur[i] = ph_cur[i];
        *this = PaceTrace();
        window = now;
        last_present = lp;
        since_frame = sf;
        ph_t = pt;
        for (int i = 0; i < kPh; ++i) ph_cur[i] = cur[i];
    }
    void stop() { *this = PaceTrace(); }
    void new_frame() {
        if (since_frame > 0) held[since_frame >= 5 ? 4 : since_frame - 1]++;
        since_frame = 0;
        frames++;
    }
    void blit(double d) { add(blit_sum, blit_max, d); blits++; }
    void ui(double d)   { add(ui_sum, ui_max, d); uis++; }
    void present(clk::time_point t0, clk::time_point t1) {
        if (window == clk::time_point{}) window = t0;
        presents++;
        if (since_frame >= 0) since_frame++;
        add(pres_sum, pres_max, ms(t0, t1));
        if (last_present != clk::time_point{}) {
            const double g = ms(last_present, t1);
            add(gap_sum, gap_max, g);
            gap[g <= 12.0 ? 0 : g <= 20.0 ? 1 : g <= 30.0 ? 2 : 3]++;
        }
        last_present = t1;
        const double span = ms(window, t1);
        if (span >= 5000.0 && presents > 1) {
            evo_boot_log("pace: %.1fs presents=%d (%.1f/s) video_frames=%d (%.2f/s) | gap avg %.2f max %.2f ms"
                         " [<=12:%d 12-20:%d 20-30:%d >30:%d] | blit avg %.2f max %.2f | ui avg %.2f max %.2f"
                         " | present avg %.2f max %.2f | held 1:%d 2:%d 3:%d 4:%d 5+:%d",
                         span / 1000.0, presents, presents * 1000.0 / span,
                         frames, frames * 1000.0 / span,
                         gap_sum / (presents - 1 > 0 ? presents - 1 : 1), gap_max,
                         gap[0], gap[1], gap[2], gap[3],
                         blits ? blit_sum / blits : 0.0, blit_max,
                         uis ? ui_sum / uis : 0.0, ui_max,
                         pres_sum / presents, pres_max,
                         held[0], held[1], held[2], held[3], held[4]);
            evo_boot_log("pace:   loop max ms pumps %.1f input %.1f update %.1f decide %.1f fetch %.1f"
                         " draw %.1f present %.1f tail %.1f | slow(>30ms) iters=%d by phase:"
                         " pumps %d input %d update %d decide %d fetch %d draw %d present %d tail %d",
                         ph_max[0], ph_max[1], ph_max[2], ph_max[3], ph_max[4], ph_max[5],
                         ph_max[6], ph_max[7],
                         slow_by[0] + slow_by[1] + slow_by[2] + slow_by[3] + slow_by[4] +
                         slow_by[5] + slow_by[6] + slow_by[7],
                         slow_by[0], slow_by[1], slow_by[2], slow_by[3], slow_by[4],
                         slow_by[5], slow_by[6], slow_by[7]);
            clear(t1);
        }
    }
};
PaceTrace g_pace;

} // namespace

int Application::run() {
    PS5_PadData padData;
    uint32_t lastButtons = 0;

    m_appFsm.postEvent(ApplicationEvent::StartLoop);
    evo_bt("Application: entering frame loop");
    evo_boot_log_flush();

    for (int frame = 0; m_running && !g_evo_term_requested; ++frame) {
        /*
         * Soft close, in three stages across successive iterations.
         *
         * The wait matters: the message has to be composited and presented
         * before submission stops, because what freezes on screen is whatever
         * the scanout last latched. Stop too early and the user is left
         * staring at a blank panel wondering whether it crashed.
         */
        if (m_softClosed) {
            usleep(200000);        /* parked: no input, no render, no present */
            continue;
        }
        if (m_softClose) {
            if (m_softCloseFrames == 0) {
                if (m_playbackController)  m_playbackController->stopPlayback();
                if (m_surroundTestService) m_surroundTestService->stop();
                if (m_soundEffectEngine)   m_soundEffectEngine->shutdown();
                if (m_fileTransferService) m_fileTransferService->cancelTransfer();
                pp_playback_shutdown(&g_pp_pb);
                if (evo_agc_runtime_is_120hz()) {
                    evo_agc_runtime_set_120hz(0);
                }
                evo_boot_log("soft close: media released");

                evo_boot_log_flush();
                /* The safe-to-close screen, not a toast: it is what the
                 * parked loop leaves latched on the panel, so it has to say
                 * everything on its own. */
                if (m_screenManager)
                    m_screenManager->navigateTo(ScreenId::SafeToClose);
            }
            if (++m_softCloseFrames > 150) {     /* ~2.5 s of presented frames */
                evo_agc_runtime_wait_idle(500);
                m_softClosed = true;
                evo_boot_log("soft close: parked - GPU idle, display latched, "
                             "safe to close from the switcher");
                evo_boot_log_flush();
                evo_usb_remote_mark_parked();
                continue;
            }
        }

        static uint64_t s_sw_p0 = 0, s_sw_p1 = 0;
        g_pace.iter_begin();
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
        /* #90: completed provider UI bundle refreshes. Next to evo_net_poll
         * because a refresh finishes inside one of its callbacks and this is
         * what turns that into a main-thread callback for the screen. */
        evo_bundle_poll();
        evo_usb_remote_poll();
        evo_webui_pump();   /* #101: the system browser, when a web UI is open */
        webui_playback_pump();

        /* Non-blocking: wakes the log writer thread. This was a blocking
         * fflush+fsync to the USB stick the movie streams from, once a
         * second, and it stalled 4K playback 33-83 ms at a time. */
        if ((frame & 63) == 0) {
            evo_boot_log_kick();
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
            evo_speaker_cal_load();
            /* #90: every provider re-reads its config from the real data
             * root - same reason recent_load() and the settings service are
             * re-run here. */
            evo_provider_mgr_rebind();
            evo_bt("persistence: rebound to %s after late unjail", evo_data_dir());
            evo_boot_log_flush();
#endif
            toast("STORAGE", "READY");
            jb_repaint = 8;
        }

        /*
         * Log once when it is clear the promotion is not coming.
         *
         * The user-facing half of this is #nav-privilege-banner, driven from
         * evo_jailbreak_is_open() in Bridge.cpp, because a shut sandbox is a
         * condition that persists rather than an event - a toast that fades
         * after a few seconds leaves the app looking simply empty for as long
         * as it stays unusable.
         *
         * Still logged, and still late: evo_jailbreak_self() gets one short
         * attempt at boot because the daemon may not be polling yet, and
         * evo_jailbreak_poll() keeps re-dropping the request, so a promotion
         * landing a second or two in is the healthy case.
         */
        static bool s_jb_warned = false;
        if (!s_jb_warned && frame > 300 && !evo_jailbreak_is_open()) {
            s_jb_warned = true;
            evo_boot_log("jailbreak: sandbox still shut after %d frames - "
                         "no daemon answered the promotion request", frame);
            evo_boot_log_flush();
        }

        g_pace.mark(0);   /* pumps: net, bundles, usb remote, web UI, jailbreak */
        // 1. Controller input & auto-repeat
        std::memset(&padData, 0, sizeof(padData));
        uint32_t pressed = 0;
        uint32_t released = 0;
        uint32_t held = 0;
        bool hasInput = false;

        if (m_padHandle >= 0 && scePadReadState(m_padHandle, &padData) == 0) {
            /*
             * Synthetic presses from the dev remote (`key <button>`), OR'd in
             * before anything reads the mask so they are indistinguishable
             * from a real tap: one frame set, cleared the next, which gives
             * the press edge here and the release on the following frame.
             * Compiles to nothing without --usb-remote.
             */
            padData.buttons |= evo_usb_remote_take_buttons();

            pressed = padData.buttons & ~lastButtons;
            released = ~padData.buttons & lastButtons;
            held = padData.buttons;
            m_padButtons = padData.buttons;

            /* Absolute first-contact position for screens that drag with the
             * touchpad (the Surround Studio orb, #106). The pad reports
             * 1920 x 1080 - the click handler below splits it at x = 960. */
            m_touchActive = (padData.touchData.touchNum > 0);
            if (m_touchActive) {
                m_touchX = std::min(1.0f, padData.touchData.touch[0].x / 1919.0f);
                m_touchY = std::min(1.0f, padData.touchData.touch[0].y / 1079.0f);
            }

            // DualSense Left Analog Stick: normalize and apply deadzone
            float rawLx = (static_cast<float>(padData.leftStick.x) - 128.0f) / 128.0f;
            float rawLy = (static_cast<float>(padData.leftStick.y) - 128.0f) / 128.0f;
            const float kStickDeadzone = 0.18f;
            m_leftStickX = (std::abs(rawLx) > kStickDeadzone) ? rawLx : 0.0f;
            m_leftStickY = (std::abs(rawLy) > kStickDeadzone) ? rawLy : 0.0f;

            // Synthesize stick tilt events into directional pad masks
            static bool s_stickLeft = false, s_stickRight = false, s_stickUp = false, s_stickDown = false;
            bool curStickLeft  = (rawLx < -0.55f);
            bool curStickRight = (rawLx >  0.55f);
            bool curStickUp    = (rawLy < -0.55f);
            bool curStickDown  = (rawLy >  0.55f);

            if (curStickLeft && !s_stickLeft)   pressed |= PadButtons::Left;
            if (curStickRight && !s_stickRight) pressed |= PadButtons::Right;
            if (curStickUp && !s_stickUp)       pressed |= PadButtons::Up;
            if (curStickDown && !s_stickDown)   pressed |= PadButtons::Down;

            if (curStickLeft)  held |= PadButtons::Left;
            if (curStickRight) held |= PadButtons::Right;
            if (curStickUp)    held |= PadButtons::Up;
            if (curStickDown)  held |= PadButtons::Down;

            s_stickLeft = curStickLeft;
            s_stickRight = curStickRight;
            s_stickUp = curStickUp;
            s_stickDown = curStickDown;

            evo_input_update(&evo_pad_state, padData.buttons, static_cast<uint64_t>(now_ms()));

            // Synthesize directional repeat events into pressed mask
            if (evo_input_fired(&evo_pad_state, EVO_ACT_UP))    pressed |= PadButtons::Up;
            if (evo_input_fired(&evo_pad_state, EVO_ACT_DOWN))  pressed |= PadButtons::Down;
            if (evo_input_fired(&evo_pad_state, EVO_ACT_LEFT))  pressed |= PadButtons::Left;
            if (evo_input_fired(&evo_pad_state, EVO_ACT_RIGHT)) pressed |= PadButtons::Right;

            // Touchpad gesture and left/right click synthesis
            static uint16_t s_lastTouchX = 960;
            static bool s_touchActive = false;
            static uint16_t s_swipeStartX = 0;
            static uint16_t s_swipeStartY = 0;
            static uint64_t s_swipeStartTime = 0;
            static bool s_swipeTriggered = false;

            const uint64_t curPadTime = static_cast<uint64_t>(now_ms());

            if (padData.touchData.touchNum > 0) {
                uint16_t tx = padData.touchData.touch[0].x;
                uint16_t ty = padData.touchData.touch[0].y;
                s_lastTouchX = tx;

                if (!s_touchActive) {
                    s_touchActive = true;
                    s_swipeStartX = tx;
                    s_swipeStartY = ty;
                    s_swipeStartTime = curPadTime;
                    s_swipeTriggered = false;
                } else if (!s_swipeTriggered && !(pressed & PadButtons::TouchPad)) {
                    int dx = static_cast<int>(tx) - static_cast<int>(s_swipeStartX);
                    int dy = static_cast<int>(ty) - static_cast<int>(s_swipeStartY);
                    uint64_t dt = curPadTime - s_swipeStartTime;

                    // Swipe: >= 250 px horizontally within 500 ms, with horizontal travel > 1.5 * vertical
                    if (dt <= 500 && std::abs(dx) >= 250 && std::abs(dx) > (std::abs(dy) * 3 / 2)) {
                        if (dx < 0) {
                            pressed |= PadButtons::TouchPadLeft;
                            evo_bt("touchpad: swipe left (dx=%d)", dx);
                        } else {
                            pressed |= PadButtons::TouchPadRight;
                            evo_bt("touchpad: swipe right (dx=%d)", dx);
                        }
                        s_swipeTriggered = true;
                    }
                }
            } else {
                s_touchActive = false;
                s_swipeTriggered = false;
            }

            // Touchpad physical click handling:
            if (pressed & PadButtons::TouchPad) {
                if (s_lastTouchX < 960) {
                    pressed |= PadButtons::TouchPadLeft;
                    evo_bt("touchpad: click left (x=%u)", s_lastTouchX);
                } else {
                    pressed |= PadButtons::TouchPadRight;
                    evo_bt("touchpad: click right (x=%u)", s_lastTouchX);
                }
            }
            if (held & PadButtons::TouchPad) {
                held |= (s_lastTouchX < 960 ? PadButtons::TouchPadLeft : PadButtons::TouchPadRight);
            }
            if (released & PadButtons::TouchPad) {
                released |= (PadButtons::TouchPadLeft | PadButtons::TouchPadRight);
            }

            hasInput = (pressed != 0 || released != 0 || evo_input_any(&evo_pad_state));
            if (hasInput) {
                evo::animation::AnimationManager::getInstance().triggerTransition(350.0);
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
                    {
                        char note[160];
                        std::snprintf(note, sizeof note, "EVO: screenshot saved - %s",
                                      name ? name + 1 : shotPath.c_str());
                        notifyOnScreen(note);
                    }
                    evo_bt("screenshot: wrote %s", shotPath.c_str());
                    evo_boot_log_flush();
                    evo_feedback(EVO_FB_CONFIRM);
                } else {
                    toast("SCREENSHOT", "Capture failed");
                    notifyOnScreen("EVO: screenshot failed");
                    evo_feedback(EVO_FB_BOUNDARY);
                }
                pressed &= ~(PadButtons::L3 | PadButtons::R3);
            }

            if (evo_webui_active()) {
                /* #101: the system browser is up and owns the controller. */
            } else if (evo_keyboard_is_open()) {
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
        g_pace.mark(1);   /* input */
        evo::animation::AnimationManager::getInstance().update(frameDeltaMs);
        /* #102: a finished subtitle auto-sync is applied on this thread. */
        prospero_subtitle_autosync_pump();
        m_screenManager->update(frameDeltaMs);
        g_pace.mark(2);   /* update */

        // 3. Determine if graphics needs to render/present
        bool isPlayer = (m_screenManager->getCurrentScreenId() == ScreenId::Player);
        bool isSurround = (m_screenManager->getCurrentScreenId() == ScreenId::SurroundTest);

        static bool s_was_player = false;
        if (isPlayer != s_was_player) {
            evo_agc_runtime_set_player_mode(isPlayer ? 1 : 0);
            s_was_player = isPlayer;
        }

        /*
         * Real HDR10: while an HDR10 (PQ) or HLG video is on screen, register
         * the display HDR10 so the TV switches into HDR; back to SDR for
         * everything else. Decided here, between frames, from the transfer of
         * the last frame presented. A refused switch is not retried until the
         * next playback session.
         */
        {
            static int  s_hdr_want = 0;
            static bool s_hdr_refused = false;
            const int trc = evo_agc_runtime_last_video_trc();
            const bool setting_on = !m_settingsService ||
                m_settingsService->getHdrOutputMode() == HdrOutputMode::Auto;
            const int want = (isPlayer && setting_on && (trc == 16 || trc == 18)) ? 1 : 0;
            if (!isPlayer) s_hdr_refused = false;
            if (want != s_hdr_want && !(want && s_hdr_refused)) {
                evo_bt("hdr10: %s (player=%d trc=%d setting=%s)",
                       want ? "ENTER" : "LEAVE", isPlayer ? 1 : 0, trc, setting_on ? "auto" : "off");
                if (evo_agc_runtime_set_hdr_output(want) == 0) {
                    s_hdr_want = want;
                } else if (want) {
                    s_hdr_refused = true;
                    toast("HDR", "The display did not accept HDR10 - playing tone-mapped");
                }
            }
        }

        /*
         * 120 Hz, Playback only. Each switch blanks the TV for a couple of
         * seconds (#114), so only switch when 120 Hz buys something: video
         * whose rate divides 119.88 (23.976, 24, 29.97, 30, 59.94, 60) and the
         * Surround Studio. 25/50 fps judders at 120 Hz just as at 60, and
         * music has no frames to pace. Between files the rate is not known
         * yet (no stream), so the current mode is held rather than flipped.
         *
         * Runs after the HDR decision on purpose: leaving an HDR video, the
         * SDR retype is flipped to the screen first and the 120 -> 60 switch
         * follows on the next frame, so the TV re-locks once into SDR 60 Hz
         * instead of re-locking for the rate and again, 2 s later, for SDR.
         *
         * Match video does the same, but with the video's own rate (what the
         * Blu-ray player calls 24p output): 23.976 / 24 Hz for film, 50 Hz for
         * 25 / 50 fps. Each repeats every frame for the same time, so pans stop
         * juddering. 29.97 / 59.94 already fit the default 59.94 Hz. Film
         * falls back to 119.88 Hz if the display refuses its exact rate.
         */
        const RefreshRateMode rrMode = m_settingsService ? m_settingsService->getRefreshRateMode()
                                                         : RefreshRateMode::Off;
        const bool rrPlayback = rrMode == RefreshRateMode::PlaybackOnly && evo_agc_runtime_supports_120hz();
        if (rrPlayback || rrMode == RefreshRateMode::MatchVideo) {
            static int s_wait_frames = 0;
            /* The last rate asked for, so a refused mode is not retried every frame. */
            static evo_vo_rate s_asked = evo_agc_runtime_get_output_rate();
            evo_vo_rate want = s_asked;
            const bool can120 = evo_agc_runtime_supports_120hz() != 0;
            if (isSurround) {
                want = (rrPlayback && can120) ? EVO_VO_RATE_119_88 : EVO_VO_RATE_DEFAULT;
            } else if (!isPlayer) {
                want = EVO_VO_RATE_DEFAULT;
            } else if (m_playbackController && m_playbackController->isActive()) {
                if (m_playbackController->isMusicMode()) {
                    want = EVO_VO_RATE_DEFAULT;
                } else {
                    const double fps = evo_pb_video_fps();
                    if (fps > 1.0)
                        want = rrPlayback ? (fits_rate(fps, 119.88) ? EVO_VO_RATE_119_88 : EVO_VO_RATE_DEFAULT)
                                          : match_video_rate(fps, can120);
                }
            }
            /* A pending HDR retype goes to the screen before the rate change.
             * Bounded: if nothing flips for half a second, switch anyway. */
            const evo_vo_rate have = evo_agc_runtime_get_output_rate();
            const bool hold = evo_agc_runtime_mode_switch_pending() && s_wait_frames < 30;
            if (want != s_asked && hold) {
                ++s_wait_frames;
            } else if (want != s_asked) {
                evo_bt("refresh: %s -> %s (%s player=%d surround=%d fps=%.3f)",
                       evo_agc_runtime_output_rate_name(have), evo_agc_runtime_output_rate_name(want),
                       rrPlayback ? "playback-only" : "match-video",
                       isPlayer ? 1 : 0, isSurround ? 1 : 0, evo_pb_video_fps());
                if (evo_agc_runtime_set_output_rate(want) != 0 &&
                    (want == EVO_VO_RATE_23_976 || want == EVO_VO_RATE_24) && can120) {
                    /* Refused: 120 Hz still repeats film evenly (5:5). 50 Hz
                     * content gains nothing from it, so it stays default. */
                    evo_agc_runtime_set_output_rate(EVO_VO_RATE_119_88);
                }
                s_asked = want;
                s_wait_frames = 0;
            }
        }

        bool hasAnim = evo::animation::AnimationManager::getInstance().hasActiveAnimations();
        /*
         * #90: a provider screen lives in its own Rml context, so its changes
         * are invisible to evo_rmlui_needs_frame(). Without this a catalog
         * page or a bundle refresh landing between two button presses would
         * not be drawn until the next press.
         */
        int uiActive = (frame < 10) || isPlayer || isSurround || hasInput || hasAnim ||
                       evo_rmlui_needs_frame() ||
                       evo_rmlui_provider_needs_frame() || (jb_repaint > 0);
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

        g_pace.mark(3);   /* decide: player mode, 120 Hz, HDR10, uiActive */
        // 4. Video Quad blit & Screen rendering
        bool swap = false;
        static int64_t s_last_pts = -1;
        if (!isPlayer) {
            s_last_pts = -1;
            g_pace.stop();
        }

        if (isPlayer) {
            /* Heap snapshot every 2 s of playback (PlaybackController.cpp,
             * evo_log_alloc_state). The open/stop lines bracket a file; this is
             * what survives when the file never reaches stop. */
            {
                static auto s_alloc_next = std::chrono::steady_clock::time_point{};
                const auto now = std::chrono::steady_clock::now();
                if (now >= s_alloc_next) {
                    s_alloc_next = now + std::chrono::seconds(2);
                    evo_log_alloc_state("play");
                }
            }
            pp_video_frame f;
            std::memset(&f, 0, sizeof(f));
            int have = (pp_playback_get_video_frame(&g_pp_pb, &f) && f.ready);
            g_pace.mark(4);   /* fetch: alloc snapshot + get the video frame */
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

            /* #105: query motion smoothing phase from presentation clock */
            if (m_settingsService) {
                evo_agc_motion_smoothing_set_mode(static_cast<int>(m_settingsService->getMotionSmoothing()));
            }
            double video_fps = evo_pb_video_fps();
            evo_agc_motion_smoothing_set_source_fps(video_fps);

            /* Phase 1.0 is "show frame B exactly" - B is the newest decoded
             * frame, so that is the right answer whenever there is no phase to
             * interpolate at: paused, scrubbing, between files. Phase 0.0 would
             * hand back frame A and the picture would jump back one frame on
             * every pause. */
            float interp_phase = 1.0f;
            int valid_phase = pp_playback_get_interp_phase(&g_pp_pb, &interp_phase);
            evo_agc_motion_smoothing_set_phase(valid_phase ? interp_phase : 1.0f);

            bool smoothing_active = evo_agc_motion_smoothing_is_active() && !is_paused && !is_scrubbing && !g_pp_pb.seek_discarding;

            bool should_render = (have && new_frame) || buffer_stale || overlay_active || is_paused || is_scrubbing || toast_visible || g_pp_pb.seek_discarding || (have && smoothing_active);

            /* #103 `upcompare`: capture what the previous iteration presented,
             * then move to the next mode. */
            bool upcmp_frame = false;
            if (s_upcmp.state != 0 && have) {
                if (s_upcmp.state < 0) {
                    s_upcmp.state = 1;
                    s_upcmp.step = 0;
                    s_upcmp.frames = 0;
                    s_upcmp.wasPaused = is_paused;
                    if (!is_paused && m_playbackController)
                        m_playbackController->setPaused(true);
                    evo_boot_log("%s: start pos=%.2f was_paused=%d",
                           s_upcmp.kind ? "dbcompare" : "upcompare", evo_player_position_s(), (int)s_upcmp.wasPaused);
                } else if (s_upcmp.frames >= (s_upcmp.step == 0 ? kUpcmpSettleFirst
                                                                : kUpcmpSettle)) {
                    char path[96];
                    const char* stepName = s_upcmp.kind ? kDbcmpNames[s_upcmp.step]
                                                        : kUpcmpNames[s_upcmp.step];
                    std::snprintf(path, sizeof path, "/mnt/usb0/evo_%s_%s.bmp",
                                  s_upcmp.kind ? "db" : "up", stepName);
                    FILE* fp = std::fopen(path, "wb");
                    const bool ok = fp != nullptr && write_scanout_bmp(fp);
                    evo_boot_log("%s: %s -> %s (active=\"%s\" pts=%lld)",
                           s_upcmp.kind ? "dbcompare" : "upcompare",
                           stepName, ok ? path : "WRITE FAILED",
                           s_upcmp.kind ? kDbcmpNames[s_upcmp.step] : evo_agc_upscale_label(),
                           (long long)current_pts);
                    evo_boot_log_flush();
                    s_upcmp.frames = 0;
                    if (++s_upcmp.step >= upcmpSteps()) {
                        s_upcmp.state = 0;
                        if (!s_upcmp.wasPaused && m_playbackController)
                            m_playbackController->setPaused(false);
                        notifyOnScreen(s_upcmp.kind ? "EVO: deep blacks comparison saved to USB"
                                                 : "EVO: upscaler comparison saved to USB");
                    }
                }
                if (s_upcmp.state != 0) {
                    upcmp_frame = true;
                    should_render = true;
                    s_upcmp.frames++;
                }
            }

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
                /* #103: a plain store; the runtime decides per frame whether
                 * the source actually gets upscaled. */
                /* #119: a plain store too, so the toggle works mid-playback. */
                if (upcmp_frame && s_upcmp.kind) {
                    evo_agc_deepblack_set_mode(s_upcmp.step);
                } else if (m_settingsService) {
                    evo_agc_deepblack_set_mode(static_cast<int>(m_settingsService->getDeepBlacks()));
                }
                if (upcmp_frame && !s_upcmp.kind) {
                    evo_agc_upscale_set_mode(kUpcmpModes[s_upcmp.step].mode);
                    evo_agc_upscale_set_network(kUpcmpModes[s_upcmp.step].net);
                } else if (m_settingsService) {
                    evo_agc_upscale_set_mode(static_cast<int>(m_settingsService->getUpscaler()));
                    evo_agc_upscale_set_network(static_cast<int>(m_settingsService->getAiNetwork()));
                }
                const int up_capped = evo_agc_upscale_take_downgrade();
                if (up_capped >= 0)
                    toast("UPSCALING", up_capped == static_cast<int>(Upscaler::AI)
                                           ? "GPU over budget - using a smaller AI network"
                                           : up_capped == static_cast<int>(Upscaler::Sharp)
                                           ? "GPU over budget - using Sharp"
                                           : "GPU over budget - turned off");

                const int sm_capped = evo_agc_motion_smoothing_take_downgrade();
                if (sm_capped >= 0)
                    toast("SMOOTHING", sm_capped == static_cast<int>(MotionSmoothing::Low)
                                           ? "GPU over budget - using Low smoothing"
                                           : "GPU over budget - turned off");
                int is_direct = (evo_pb_active_backend() == EVO_VDEC_BACKEND_NATIVE && !f.held && f.uv != nullptr) ? 1 : 0;
                if (new_frame) g_pace.new_frame();
                if (isPlayer && evo_sweep_active()) s_sw_p0 = evo_sweep_now_us();
                const auto blit_t0 = PaceTrace::clk::now();
                evo_agc_blit_yuv(f.y, f.y_pitch, f.uv, f.uv_pitch,
                                 f.u, f.u_pitch, f.v, f.v_pitch,
                                 static_cast<int>(f.coded_w), static_cast<int>(f.coded_h),
                                 static_cast<int>(f.disp_w), static_cast<int>(f.disp_h),
                                 view_mode, f.ten_bit, f.color_trc,
                                 is_direct, current_pts, f.dovi);
                g_pace.blit(PaceTrace::ms(blit_t0, PaceTrace::clk::now()));
                if (isPlayer && evo_sweep_active()) s_sw_p1 = evo_sweep_now_us();
                swap = true;
            }

            if (m_uiScratch && should_render && !upcmp_frame) {
                const auto ui_t0 = PaceTrace::clk::now();
                m_screenManager->render(m_uiScratch, DisplayWidth, DisplayHeight);
                g_pace.ui(PaceTrace::ms(ui_t0, PaceTrace::clk::now()));
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

        /*
         * Draw the toast, over whatever the screen just drew.
         *
         * evo_toast.h says this is "called every frame by the main render
         * loop" and nothing called it at all - the only reference anywhere was
         * evo_toast_visible() being used as a render gate. So toast() set its
         * state, the state expired on schedule, and not one pixel was ever
         * drawn. That is why a screenshot appeared to do nothing, and why
         * "Decoder seek failed" and the rest have been silent too.
         *
         * It forces a present of its own: a toast is often the only thing that
         * changed, and without this it would wait for something else to
         * trigger a frame.
         */
        if (uiActive && evo_toast_visible()) {
            draw_prospero_toast(m_uiScratch);
            swap = true;
        }

        /*
         * The virtual keyboard, for the same reason and in the same place.
         *
         * evo_rmlui_render_keyboard() had exactly one caller -
         * evo_screen_keyboard() - and nothing called that, so the virtual
         * keyboard could not draw on ANY screen. It still opened and still
         * swallowed every button (the input dispatch routes everything to it
         * while it is up), so the symptom was a frozen-looking screen with no
         * prompt on it. That is what #90's provider screen hit; the browser's
         * search had the same latent bug and was only saved by the native IME
         * being the default.
         *
         * Skipped when the native IME is active: the system draws that dialog
         * itself, and drawing over it would be wrong.
         */
        if (uiActive && evo_keyboard_is_open() && !evo_keyboard_is_native_active()) {
            evo_screen_keyboard(m_uiScratch);
            m_keyboardDrawn = true;
            swap = true;
        } else if (uiActive && m_keyboardDrawn) {
            /*
             * One last call on the frame after it closes. evo_screen_keyboard()
             * hides its RmlUi document when the keyboard is shut, but this gate
             * meant that branch could never run: the document stayed visible,
             * so #115's `ui` readback went on reporting a keyboard modal over
             * every screen that followed - the dev remote's view of the UI was
             * wrong from the first text entry onwards.
             */
            evo_screen_keyboard(m_uiScratch);
            m_keyboardDrawn = false;
            swap = true;
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
            g_pace.mark(5);   /* draw: blit, UI, toast, keyboard */
            const auto present_t0 = PaceTrace::clk::now();
            evo_agc_runtime_present();
            g_pace.mark(6);   /* present, incl. the flip wait */
            if (isPlayer) g_pace.present(present_t0, PaceTrace::clk::now());
            if (isPlayer && evo_sweep_active()) {
                uint64_t sw_p2 = evo_sweep_now_us();
                if (s_sw_p0 == 0) s_sw_p0 = s_sw_p1 = sw_p2;
                evo_sweep_note_present(s_sw_p0, s_sw_p1, sw_p2, swap ? 1 : 0);
                s_sw_p0 = s_sw_p1 = 0;
                evo_sweep_probe_colour();
            }
            evo_rmlui_end_frame();

#if defined(EVO_APP_MODULE) || defined(EVO_TARGET_PS5)
            /*
             * Dismiss the system splash, once EVO has something real on
             * screen behind it.
             *
             * The shell shows sce_sys/pic1.png while a title starts and leaves
             * it up until the app says it is ready. Nothing here ever did,
             * which went unnoticed for as long as the project shipped no
             * pic0/pic1 at all - with no artwork there was no splash, so there
             * was nothing to dismiss. Adding the background art in 1584ddd
             * gave the shell something to show, and the player then rendered
             * happily underneath it forever: the frame loop presents at 60 fps
             * with zero flip failures while the panel shows the splash.
             *
             * Held until a frame has presented so the splash gives way
             * to a drawn UI rather than to one black frame.
             */
            static bool s_splash_hidden = false;
            if (!s_splash_hidden && frame >= 1) {
                s_splash_hidden = true;
                const int rc = sceSystemServiceHideSplashScreen();
                evo_boot_log("splash: hide rc=%d (0 = dismissed)", rc);
                evo_boot_log_flush();
            }
#endif
            if (log_this_present) {
                evo_boot_log("app present done isPlayer=1");
                evo_boot_log_flush();
            }
            if (frame < 5) {
                evo_bt("frame %d: present done", frame);
                evo_boot_log_flush();
            }
        }

#if defined(EVO_APP_MODULE) || defined(EVO_TARGET_PS5)
        /* Fallback: if present didn't dismiss it by frame 20, force-dismiss splash */
        static bool s_splash_fallback_hidden = false;
        if (!s_splash_fallback_hidden && frame >= 20) {
            s_splash_fallback_hidden = true;
            const int rc = sceSystemServiceHideSplashScreen();
            evo_boot_log("splash: fallback hide rc=%d", rc);
            evo_boot_log_flush();
        }
#endif

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
