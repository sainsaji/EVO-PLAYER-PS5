#ifndef EVO_APPLICATION_HPP
#define EVO_APPLICATION_HPP

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
};

} // namespace evo

#endif // EVO_APPLICATION_HPP
