#ifndef EVO_AUDIO_TRACK_PICKER_SCREEN_HPP
#define EVO_AUDIO_TRACK_PICKER_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include <string>
#include <vector>

namespace evo {

/*
 * Quality and audio selection for the player (R2).
 *
 * A picker rather than a cycle: switching the audio stream or the video quality
 * means re-opening the stream, so stepping through them one press at a time
 * costs a reopen per step — the same reason the subtitle picker exists instead
 * of a next-track button.
 *
 * When the open stream has more than one video variant (an HLS master lists the
 * same channel at several qualities) the list starts with an AUTO row and one
 * row per variant, then the audio tracks. Quality is read from the stream once
 * it is open, so it does not depend on any separate fetch of the playlist. With
 * a single video stream the list is the audio tracks alone, as it always was.
 */
class AudioTrackPickerScreen : public StatefulScreen {
public:
    AudioTrackPickerScreen();
    ~AudioTrackPickerScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::AudioTrackPicker; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    enum Kind { KindAuto = 0, KindVideo, KindAudio };

    struct Entry {
        Kind kind = KindAudio;
        int streamIndex = -1;
        std::string label;
        std::string detail;
        std::string badge;
    };

    void refreshTracks();
    void navigate(int delta);
    void activateSelection();

    bool m_hasQuality = false;      /* the stream has more than one video variant */
    std::vector<Entry> m_tracks;
    int m_selectedIndex = 0;
    int m_activeIndex = 0;
    int m_scrollOffset = 0;
};

} // namespace evo

#endif // EVO_AUDIO_TRACK_PICKER_SCREEN_HPP
