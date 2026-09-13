#ifndef EVO_AUDIO_TRACK_PICKER_SCREEN_HPP
#define EVO_AUDIO_TRACK_PICKER_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include <string>
#include <vector>

namespace evo {

/*
 * Audio track selection for the player.
 *
 * A picker rather than a cycle: switching the audio stream means re-opening the
 * file, so stepping through tracks one press at a time costs a reopen per step
 * — the same reason the subtitle picker exists instead of a next-track button.
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
    struct Entry {
        int streamIndex = -1;
        std::string label;
        std::string detail;
        std::string badge;
    };

    void refreshTracks();
    void navigate(int delta);
    void activateSelection();

    std::vector<Entry> m_tracks;
    int m_selectedIndex = 0;
    int m_activeIndex = 0;
    int m_scrollOffset = 0;
};

} // namespace evo

#endif // EVO_AUDIO_TRACK_PICKER_SCREEN_HPP
