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
    void navigateActions(int dir);
    void navigateSpeakers2D(int dirX, int dirY);
    void moveOrb(float dx, float dy);
    void updateProximities();
    void switchPane(int dir);
    void activateSelection();
    void playSelectedChannel();
    void startSweep(int mode);
    void stopSweep();
    int  actionCount() const { return 6; }
    int  speakerCount() const;

    enum Pane { PaneActions = 0, PaneSpeakers = 1, PaneOrb = 2 };

    int    m_focusPane = PaneActions;
    int    m_selectedAction = 0;
    int    m_selectedSpeaker = 0;

    /* Interactive 2.5D sound orb and spatial audio coordinates */
    float  m_orbX = 0.0f;
    float  m_orbY = 0.0f;
    float  m_targetOrbX = 0.0f;
    float  m_targetOrbY = 0.0f;
    bool   m_isOrbMode = false;
    float  m_speakerProximity[8] = {0.0f};

    /* Auto-test / rotation sweep, driven from update() */
    int    m_sweepMode = -1;      /* -1 idle, 0 = 5.1, 1 = 7.1, 2 = rotation */
    int    m_sweepStep = 0;
    double m_sweepMs = 0.0;
    double m_animTimeSec = 0.0;
};

} // namespace evo

#endif // EVO_SURROUND_TEST_SCREEN_HPP
