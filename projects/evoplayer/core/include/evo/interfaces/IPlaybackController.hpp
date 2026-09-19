#ifndef EVO_I_PLAYBACK_CONTROLLER_HPP
#define EVO_I_PLAYBACK_CONTROLLER_HPP

#include "evo/Common.hpp"
#include "evo/interfaces/IStatefulFeature.hpp"
#include <string>
#include <vector>

namespace evo {

/**
 * @brief Discrete playback engine states.
 */
enum class PlaybackState : int {
    Stopped = 0,
    Opening = 1,
    Playing = 2,
    Paused = 3,
    Scrubbing = 4,
    Seeking = 5,
    Finished = 6,
    Error = 7
};

/**
 * @brief Discrete playback trigger events.
 */
enum class PlaybackEvent : int {
    Open = 0,
    Play = 1,
    Pause = 2,
    Resume = 3,
    StartScrub = 4,
    MoveScrub = 5,
    ConfirmScrub = 6,
    CancelScrub = 7,
    Seek = 8,
    Finish = 9,
    Stop = 10,
    Fail = 11
};

/**
 * @brief Interface for playback session management, A/V synchronization, scrubbing, and transport controls.
 *
 * Implements IStatefulFeature to guarantee state integrity across media transitions.
 */
class IPlaybackController : public IStatefulFeature {
public:
    virtual ~IPlaybackController() = default;

    virtual bool startPlayback(const std::string& filePath, double resumeOffset = 0.0) = 0;
    virtual void stopPlayback() = 0;

    virtual void togglePause() = 0;
    virtual void setPaused(bool paused) = 0;
    virtual bool isPaused() const = 0;
    virtual bool isActive() const = 0;
    virtual bool isMusicMode() const = 0;

    virtual PlaybackState getPlaybackState() const = 0;

    virtual double getPositionSeconds() const = 0;
    virtual double getDurationSeconds() const = 0;
    virtual double getPercentage() const = 0;
    virtual const std::string& getCurrentFilePath() const = 0;

    virtual void seekTo(double targetSeconds) = 0;
    virtual void beginScrub() = 0;
    virtual void moveScrub(double deltaSeconds) = 0;
    virtual bool confirmScrub() = 0;
    virtual void cancelScrub() = 0;
    virtual bool isScrubbing() const = 0;
    virtual double getScrubTargetSeconds() const = 0;
    virtual void updateScrubHold(uint32_t heldButtons) = 0;
    virtual void tickScrubAutoCommit() = 0;

    virtual void jumpChapter(int direction) = 0;

    virtual void cycleViewMode() = 0;
    virtual ViewMode getViewMode() const = 0;
    virtual void setViewMode(ViewMode mode) = 0;

    virtual void saveResumePosition() = 0;
    virtual double loadResumePosition(const std::string& filePath) const = 0;

    virtual bool playNextVideo() = 0;
    virtual bool replay() = 0;

    /* --- audio tracks ------------------------------------------------- */
    struct AudioTrackInfo {
        int         streamIndex = -1;
        std::string title;       /* stream metadata title, if any */
        std::string language;    /* "eng", "jpn", ... or "UND" */
        std::string codecName;
        int         channels = 0;
        int         sampleRate = 0;
    };

    /* Every audio stream in the open file that has a usable decoder. */
    virtual std::vector<AudioTrackInfo> getAudioTracks() const = 0;
    virtual int  getActiveAudioStream() const = 0;
    /*
     * Switch the live audio stream. Re-opens the file at the current position
     * with that stream selected - there is no way to swap the decoder under a
     * running session, which is also why this is a picker and not a cycle.
     */
    virtual bool switchAudioTrack(int streamIndex) = 0;
};

} // namespace evo

#endif // EVO_I_PLAYBACK_CONTROLLER_HPP
