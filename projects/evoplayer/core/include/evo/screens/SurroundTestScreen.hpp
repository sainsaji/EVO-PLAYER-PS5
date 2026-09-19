#ifndef EVO_SURROUND_TEST_SCREEN_HPP
#define EVO_SURROUND_TEST_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"

namespace evo {

class SurroundTestScreen : public StatefulScreen {
public:
    SurroundTestScreen();
    ~SurroundTestScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::SurroundTest; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    void navigate(int dir);
    void switchPane(int dir);
    void activateSelection();
    void playSelectedChannel();
    void startSweep(int mode);
    void stopSweep();
    int  actionCount() const { return 5; }
    int  speakerCount() const;

    /* Two panes, like Settings: the action list on the left and the speaker
     * grid on the right. Previously only the grid was reachable - selected_item
     * was hardcoded to 5 + m_selectedSpeaker, and items 0-4 (the actions) had
     * no input path at all despite rendering with focus styling. */
    enum Pane { PaneActions = 0, PaneSpeakers = 1 };

    int    m_focusPane = PaneActions;
    int    m_selectedAction = 0;
    int    m_selectedSpeaker = 0;

    /* Auto-test / rotation sweep, driven from update(): the service only opens
     * the port and plays one tone, so stepping through channels is ours. */
    int    m_sweepMode = -1;      /* -1 idle, 0 = 5.1, 1 = 7.1, 2 = rotation */
    int    m_sweepStep = 0;
    double m_sweepMs = 0.0;
};

} // namespace evo

#endif // EVO_SURROUND_TEST_SCREEN_HPP
