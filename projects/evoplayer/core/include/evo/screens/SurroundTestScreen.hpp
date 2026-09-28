#ifndef EVO_SURROUND_TEST_SCREEN_HPP
#define EVO_SURROUND_TEST_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include "evo/audio/SpatialField.hpp"
#include "evo/services/SpeakerCalibrationService.hpp"

#include <memory>

namespace evo {

/*
 * Surround Sound Studio (#106). Four panes share one 2.5D soundstage:
 *   Actions      - the test list on the left
 *   Speakers     - a D-pad cursor that moves by room position
 *   Orb          - the spatial sound source (3D SOUND FIELD / FREE ROAM)
 *   Calibration  - AUTO CALIBRATION (MIC) through the DualSense microphone
 * The room model is evo/audio/SpatialField.hpp; this class owns input, the
 * source's motion and which test is running.
 */
class SurroundTestScreen : public StatefulScreen {
public:
    SurroundTestScreen();
    ~SurroundTestScreen() override;

    ScreenId getScreenId() const override { return ScreenId::SurroundTest; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    enum Pane { PaneActions = 0, PaneSpeakers, PaneOrb, PaneCalibration };
    enum Flight { FlightManual = 0, FlightOrbit, FlightFlyby };
    enum Action {
        ActSoundField = 0, ActCalibration, ActFreeRoam, ActRotation,
        ActAuto51, ActAuto71, ActLayout, ActSilence, ActCount
    };

    bool handleOrbInput(uint32_t pressed);
    bool handleCalibrationInput(uint32_t pressed);
    void navigateActions(int dir);
    void navigateSpeakers2D(int dirX, int dirY);
    void switchPane(int dir);
    void activateSelection();
    void enterOrb(bool toneFollow, Flight flight);
    void setFlight(Flight flight);
    void resetOrb();
    void toggleLayout();
    void silence();
    void playChannel(int ch);
    void startSweep(int mode);
    void stopSweep();
    void startCalibration();
    void applyCalibration();
    void leaveCalibration();

    void updateOrbMotion(double dt);
    void easeSourceTo(float x, float y, float z, double dt);
    void updateField();
    void recordTrail(double deltaMs);

    bool is51() const;
    int  rotationOrder(int* out) const;

    int    m_focusPane = PaneActions;
    int    m_selectedAction = 0;
    int    m_selectedSpeaker = 0;          /* channel index */

    /* the sound source, metres (+y = rear) */
    spatial::Vec3 m_src;
    int    m_flight = FlightManual;
    bool   m_toneFollow = false;           /* 3D SOUND FIELD */
    int    m_toneChannel = -1;
    double m_orbitAngle = 0.0;             /* radians, 0 = front */
    double m_orbitRadius = 2.2;
    double m_flybyT = 0.0;
    float  m_gains[spatial::kChannels] = {0.0f};
    int    m_loudest = -1;

    /* input state the orb reads itself (raw pad, not the stick-synth D-pad) */
    uint32_t m_prevRaw = 0;
    double m_dpadHeldMs = 0.0;
    double m_heightHeldMs = 0.0;
    bool   m_touchWasActive = false;
    float  m_touchX = 0.0f, m_touchY = 0.0f;

    static constexpr int kTrail = 5;
    float  m_trailX[kTrail] = {0.0f}, m_trailY[kTrail] = {0.0f};
    int    m_trailCount = 0;
    double m_trailMs = 0.0;

    /* Auto-test / rotation sweep, driven from update() */
    int    m_sweepMode = -1;      /* -1 idle, 0 = 5.1, 1 = 7.1, 2 = rotation */
    int    m_sweepStep = 0;
    double m_sweepMs = 0.0;
    double m_sweepAngle = 0.0;     /* 360: radians clockwise from the front */
    double m_sweepTravel = 0.0;    /* 360: radians covered so far */
    double m_animTimeSec = 0.0;

    std::unique_ptr<SpeakerCalibrationService> m_cal;
    bool   m_calStarted = false;
    bool   m_calApplied = false;
};

} // namespace evo

#endif // EVO_SURROUND_TEST_SCREEN_HPP
