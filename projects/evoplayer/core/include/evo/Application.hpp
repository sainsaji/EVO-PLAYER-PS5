#ifndef EVO_APPLICATION_HPP
#define EVO_APPLICATION_HPP

#include <csignal>

#include "evo/interfaces/IApplication.hpp"
#include "evo/services/SettingsService.hpp"
#include "evo/services/MediaMetadataService.hpp"
#include "evo/services/CoverArtService.hpp"
#include "evo/services/SoundEffectEngine.hpp"
#include "evo/services/SurroundTestService.hpp"
#include "evo/services/FileSystemBrowser.hpp"
#include "evo/services/PlaybackController.hpp"
#include "evo/screens/ScreenManager.hpp"
#include "evo/fsm/StateMachine.hpp"

#include <memory>

namespace evo {

class Application : public IApplication {
public:
    static Application& getInstance();

    // --- IStatefulFeature ---
    IStateMachine* getStateMachine() override { return &m_appFsm; }
    const IStateMachine* getStateMachine() const override { return &m_appFsm; }

    ApplicationState getApplicationState() const override {
        return m_appFsm.getCurrentState();
    }

    bool initialize(int argc, char** argv) override;
    int run() override;
    void shutdown() override;
    void requestExit() override;

    /*
     * Soft close: release everything that makes a kill dangerous, then stop.
     *
     * A PS5 app is not really meant to terminate itself - no game ships a quit
     * menu - and returning from main() drops into a libc exit path that
     * crashed every time it was tried (2026-09-18). So this does not exit at
     * all. It stops playback, decoders and audio, shows a message, drains the
     * GPU, and then parks the frame loop forever: no input, no render, no
     * present, no submits in flight.
     *
     * The display keeps showing the last presented frame, because VideoOut
     * stays registered and the scanout buffer is simply never written again -
     * so the "safe to close" message stays on screen indefinitely.
     *
     * Closing it from the switcher at that point kills a process holding a
     * quiescent GPU instead of one submitting at 60 Hz, which is the condition
     * that made the ordinary PS-button close a panic risk.
     */
    void requestSoftClose();

    ISettingsService* getSettingsService() const override { return m_settingsService.get(); }
    IMediaMetadataService* getMediaMetadataService() const override { return m_mediaMetadataService.get(); }
    ICoverArtService* getCoverArtService() const override { return m_coverArtService.get(); }
    ISoundEffectEngine* getSoundEffectEngine() const override { return m_soundEffectEngine.get(); }
    ISurroundTestService* getSurroundTestService() const override { return m_surroundTestService.get(); }
    IFileSystemBrowser* getFileSystemBrowser() const override { return m_fileSystemBrowser.get(); }
    IPlaybackController* getPlaybackController() const override { return m_playbackController.get(); }
    ScreenManager* getScreenManager() const override { return m_screenManager.get(); }

private:
    Application();
    ~Application() override;

    void initStateMachine();
    bool initHardware();
    bool initServices();
    bool initScreens();

    StateMachine<ApplicationState, ApplicationEvent> m_appFsm;

    std::unique_ptr<SettingsService> m_settingsService;
    std::unique_ptr<MediaMetadataService> m_mediaMetadataService;
    std::unique_ptr<CoverArtService> m_coverArtService;
    std::unique_ptr<SoundEffectEngine> m_soundEffectEngine;
    std::unique_ptr<SurroundTestService> m_surroundTestService;
    std::unique_ptr<FileSystemBrowser> m_fileSystemBrowser;
    std::unique_ptr<PlaybackController> m_playbackController;
    std::unique_ptr<ScreenManager> m_screenManager;

    int m_padHandle = -1;
    uint32_t* m_uiScratch = nullptr;
    bool m_running = false;
    /* shutdown() is reached from both run() and ~Application(); see the note
     * on its definition. Nothing in it is written to survive a second pass. */
    bool m_shutdownDone = false;
    bool m_softClose = false;      /* soft close requested            */
    bool m_softClosed = false;     /* parked; loop does nothing but sleep */
    int  m_softCloseFrames = 0;    /* frames rendered since the request */
};

} // namespace evo

/*
 * Set by the SIGTERM handler, polled by the frame loop.
 *
 * Closing the app from the PS button kills the process, so Application::run()
 * never returned and shutdown() - the only thing that drains the GPU and gives
 * back the scanout registration and the direct-memory pool - never ran. The
 * kernel was left to reclaim all of it underneath a GPU that might still be
 * mid-submit.
 *
 * The handler does nothing but set this: signal context is no place for
 * sceVideoOut calls, munmap or logging, and the loop is already running at
 * display cadence or better, so it notices within a frame and exits through
 * the ordinary shutdown path. sig_atomic_t and volatile because that is the
 * only thing a handler may portably touch.
 *
 * This is best-effort. If the system sends SIGKILL without a SIGTERM first,
 * nothing here runs and teardown is the kernel's problem again - which is why
 * the drain in evo_agc_runtime_shutdown() matters on its own.
 */
extern "C" volatile sig_atomic_t g_evo_term_requested;

#endif // EVO_APPLICATION_HPP
