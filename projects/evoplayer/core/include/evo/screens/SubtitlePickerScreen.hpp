#ifndef EVO_SUBTITLE_PICKER_SCREEN_HPP
#define EVO_SUBTITLE_PICKER_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include <string>
#include <vector>

namespace evo {

class SubtitlePickerScreen : public StatefulScreen {
public:
    SubtitlePickerScreen();
    ~SubtitlePickerScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::SubtitlePicker; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    struct SubtitleTrackEntry {
        int trackId = -2; // -2 = Off, -1 = External SRT, >= 0 = Stream index
        std::string label;
        std::string detail;
    };

    void refreshTracks();
    void navigate(int delta);
    void activateSelection();
    void cycleSize();

    std::vector<SubtitleTrackEntry> m_tracks;
    int m_selectedIndex = 0;
};

} // namespace evo

#endif // EVO_SUBTITLE_PICKER_SCREEN_HPP
