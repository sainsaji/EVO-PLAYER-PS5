#ifndef EVO_PLAYBACK_CONTROLLER_HPP
#define EVO_PLAYBACK_CONTROLLER_HPP

#include "evo/interfaces/IPlaybackController.hpp"
#include "evo/fsm/StateMachine.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace evo {

class PlaybackController : public IPlaybackController {
public:
    PlaybackController();
    ~PlaybackController() override;

    // --- IStatefulFeature ---
    IStateMachine* getStateMachine() override { return &m_playbackFsm; }
    const IStateMachine* getStateMachine() const override { return &m_playbackFsm; }

    PlaybackState getPlaybackState() const override {
        return m_playbackFsm.getCurrentState();
    }

    bool startPlayback(const std::string& filePath, double resumeOffset = 0.0) override;
    void stopPlayback() override;

    void togglePause() override;
    void setPaused(bool paused) override;
    bool isPaused() const override;
    bool isActive() const override;
    bool isMusicMode() const override { return m_musicMode; }

    double getPositionSeconds() const override;
    double getDurationSeconds() const override { return m_durationSeconds; }
    double getPercentage() const override;
    const std::string& getCurrentFilePath() const override { return m_currentFilePath; }

    void seekTo(double targetSeconds) override;
    void beginScrub() override;
    void moveScrub(double deltaSeconds) override;
    bool confirmScrub() override;
    void cancelScrub() override;
    bool isScrubbing() const override;
    double getScrubTargetSeconds() const override { return m_scrubTargetSeconds; }
    void updateScrubHold(uint32_t heldButtons) override;
    void tickScrubAutoCommit() override;

    void jumpChapter(int direction) override;

    void cycleViewMode() override;
    ViewMode getViewMode() const override { return m_viewMode; }
    void setViewMode(ViewMode mode) override;

    void saveResumePosition() override;
    double loadResumePosition(const std::string& filePath) const override;

    bool playNextVideo() override;
    bool replay() override;

    std::vector<AudioTrackInfo> getAudioTracks() const override;
    int  getActiveAudioStream() const override;
    bool switchAudioTrack(int streamIndex) override;

private:
    void initStateMachine();
    void applyViewMode();
    double clampScrubTarget(double target) const;
    void resetScrubHold();

    StateMachine<PlaybackState, PlaybackEvent> m_playbackFsm;

    std::string m_currentFilePath;
    double m_durationSeconds = 0.0;
    double m_resumeBaseOffset = 0.0;
    bool m_musicMode = false;
    ViewMode m_viewMode = ViewMode::Fit;

    // Scrub state
    double m_scrubTargetSeconds = 0.0;
    int m_scrubHoldDirection = 0;
    uint64_t m_scrubHoldStartMs = 0;
    uint64_t m_scrubHoldLastStepMs = 0;
    uint64_t m_scrubAutoCommitDeadlineMs = 0;

    /* Stream the next open() should pick, or -1 for "first decodable". Set
     * only across a switchAudioTrack() reopen and cleared by startPlayback. */
    int m_requestedAudioStream = -1;
};

} // namespace evo

#endif // EVO_PLAYBACK_CONTROLLER_HPP
