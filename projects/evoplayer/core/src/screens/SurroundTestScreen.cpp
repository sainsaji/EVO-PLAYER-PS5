#include "evo/screens/SurroundTestScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_speaker_cal.h"
#include "evo_toast.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace evo {

namespace {

constexpr float  kTapStepM      = 0.18f;   /* D-pad tap: 18 dp        */
constexpr float  kGlideMps      = 3.6f;    /* D-pad held: 6 dp / 60 Hz frame */
constexpr float  kStickMps      = 3.2f;    /* full left-stick tilt    */
constexpr float  kTouchSpanM    = 7.0f;    /* a full touchpad swipe   */
constexpr float  kHeightTapM    = 0.10f;
constexpr float  kHeightMps     = 1.0f;
constexpr double kRepeatDelayMs = 250.0;   /* tap -> held glide       */
constexpr double kSweepDwellMs  = 1300.0;  /* auto test: a whole 1.2 s tone + a breath */
constexpr double kSweepRevSec   = 12.0;    /* 360 sweep: one revolution */
constexpr double kOrbitRadPerS  = 0.55;
constexpr double kFlybySec      = 7.0;

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

float speakerAzimuth(bool is51, int ch) {
    return spatial::azimuthDeg(spatial::speakerPosition(is51, ch));
}

} // namespace

SurroundTestScreen::SurroundTestScreen()
    : StatefulScreen("SurroundTestScreen"),
      m_cal(new SpeakerCalibrationService()) {
}

SurroundTestScreen::~SurroundTestScreen() = default;

bool SurroundTestScreen::is51() const {
    auto surround = Application::getInstance().getSurroundTestService();
    return surround ? surround->is51Layout() : false;
}

void SurroundTestScreen::onEnter() {
    StatefulScreen::onEnter();
    m_focusPane = PaneActions;
    m_selectedAction = 0;
    m_selectedSpeaker = 0;
    m_src = spatial::Vec3();
    m_src.y = -1.2f;
    m_flight = FlightManual;
    m_toneFollow = false;
    m_toneChannel = -1;
    m_trailCount = 0;
    m_calStarted = false;
    m_calApplied = false;
    m_prevRaw = Application::getInstance().getPadButtons();
    stopSweep();
    updateField();
}

void SurroundTestScreen::onExit() {
    StatefulScreen::onExit();
    stopSweep();
    m_cal->cancel();
    if (auto surround = Application::getInstance().getSurroundTestService()) {
        surround->stop();
    }
}

/* ------------------------------------------------------------ navigation */

void SurroundTestScreen::navigateActions(int dir) {
    m_selectedAction += dir;
    if (m_selectedAction < 0) {
        m_selectedAction = 0;
        evo_feedback(EVO_FB_BOUNDARY);
    } else if (m_selectedAction >= ActCount) {
        m_selectedAction = ActCount - 1;
        evo_feedback(EVO_FB_BOUNDARY);
    } else {
        evo_feedback(EVO_FB_MOVE);
    }
}

/*
 * Move the speaker cursor by where the speakers physically are: the nearest
 * speaker inside a ~55 degree cone of the pressed direction, weighted against
 * sideways drift. Nothing to the left of FL / SL / SBL, so Left there hands
 * focus back to the action list instead of trapping it.
 */
void SurroundTestScreen::navigateSpeakers2D(int dirX, int dirY) {
    const bool l51 = is51();
    const spatial::Vec3 cur = spatial::speakerPosition(l51, m_selectedSpeaker);

    int best = -1;
    float bestScore = 1e9f;
    for (int ch = 0; ch < spatial::kChannels; ++ch) {
        if (ch == m_selectedSpeaker || !spatial::present(l51, ch)) continue;
        const spatial::Vec3 p = spatial::speakerPosition(l51, ch);
        const float dx = p.x - cur.x, dy = p.y - cur.y;
        const float along = dx * dirX + dy * dirY;
        const float perp = std::fabs(dx * dirY - dy * dirX);
        if (along <= 0.05f || perp > along * 1.5f) continue;
        const float score = along + 2.0f * perp;
        if (score < bestScore) { bestScore = score; best = ch; }
    }

    if (best >= 0) {
        m_selectedSpeaker = best;
        evo_feedback(EVO_FB_MOVE);
    } else if (dirX < 0) {
        switchPane(-1);
    } else {
        evo_feedback(EVO_FB_BOUNDARY);
    }
}

void SurroundTestScreen::switchPane(int dir) {
    if (dir > 0 && m_focusPane == PaneActions) {
        if (!spatial::present(is51(), m_selectedSpeaker)) m_selectedSpeaker = 0;
        m_focusPane = PaneSpeakers;
        evo_feedback(EVO_FB_MOVE);
    } else if (dir < 0 && m_focusPane != PaneActions) {
        m_focusPane = PaneActions;
        m_toneFollow = false;
        evo_feedback(EVO_FB_MOVE);
    } else {
        evo_feedback(EVO_FB_BOUNDARY);
    }
}

/* --------------------------------------------------------------- actions */

void SurroundTestScreen::activateSelection() {
    switch (m_selectedAction) {
    case ActSoundField: enterOrb(true, FlightOrbit); break;
    case ActCalibration:
        stopSweep();
        m_focusPane = PaneCalibration;
        m_calStarted = false;
        m_calApplied = false;
        evo_feedback(EVO_FB_CONFIRM);
        break;
    case ActFreeRoam:  enterOrb(false, FlightManual); break;
    case ActRotation:  startSweep(2); break;
    case ActAuto51:    startSweep(0); break;
    case ActAuto71:    startSweep(1); break;
    case ActLayout:    toggleLayout(); break;
    case ActSilence:
    default:           silence(); break;
    }
}

void SurroundTestScreen::enterOrb(bool toneFollow, Flight flight) {
    stopSweep();
    auto surround = Application::getInstance().getSurroundTestService();
    if (toneFollow && (!surround || !surround->startField())) {
        toast("SURROUND TEST", "Audio port unavailable");
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    }
    m_focusPane = PaneOrb;
    m_toneFollow = toneFollow;
    m_toneChannel = -1;
    m_trailCount = 0;
    setFlight(flight);
    evo_feedback(EVO_FB_CONFIRM);
    toast(toneFollow ? "3D SOUND FIELD" : "SPATIAL ORB",
          toneFollow ? "Music follows the source - Triangle changes mode"
                     : "Left stick / touchpad moves the source, L1 / R1 height");
}

void SurroundTestScreen::setFlight(Flight flight) {
    m_flight = flight;
    if (flight == FlightOrbit) {
        /* pick up from wherever the source is, so it doesn't jump */
        const float r = std::sqrt(m_src.x * m_src.x + m_src.y * m_src.y);
        m_orbitRadius = clampf(r, 1.2f, 3.0f);
        m_orbitAngle = std::atan2(m_src.x, -m_src.y);
        if (r < 0.3f) m_orbitAngle = 0.0;
    } else if (flight == FlightFlyby) {
        m_flybyT = 0.0;
    }
}

void SurroundTestScreen::resetOrb() {
    m_src = spatial::Vec3();
    m_flight = FlightManual;
    m_trailCount = 0;
    evo_feedback(EVO_FB_CANCEL);
}

void SurroundTestScreen::toggleLayout() {
    auto surround = Application::getInstance().getSurroundTestService();
    if (!surround) return;
    const bool next51 = !surround->is51Layout();
    surround->set51Layout(next51);
    if (!spatial::present(next51, m_selectedSpeaker)) m_selectedSpeaker = 0;
    updateField();
    evo_feedback(EVO_FB_TOGGLE);
    toast("SPEAKER LAYOUT", next51 ? "5.1 Surround (6 Channels)" : "7.1 Surround (8 Channels)");
}

void SurroundTestScreen::silence() {
    stopSweep();
    m_toneFollow = false;
    if (auto surround = Application::getInstance().getSurroundTestService()) surround->stop();
    evo_feedback(EVO_FB_CANCEL);
    toast("SURROUND TEST", "Silenced");
}

void SurroundTestScreen::playChannel(int ch) {
    auto surround = Application::getInstance().getSurroundTestService();
    if (!surround || ch < 0) return;
    evo_feedback(EVO_FB_CONFIRM);
    surround->triggerTone(surround->is51Layout(), ch);
    m_toneChannel = ch;
}

/* Non-LFE speakers clockwise from FRONT LEFT, for the 360 sweep. */
int SurroundTestScreen::rotationOrder(int* out) const {
    const bool l51 = is51();
    const float start = speakerAzimuth(l51, 0);
    int n = 0;
    for (int ch = 0; ch < spatial::kChannels; ++ch)
        if (ch != 3 && spatial::present(l51, ch)) out[n++] = ch;
    std::sort(out, out + n, [&](int a, int b) {
        const float ra = std::fmod(speakerAzimuth(l51, a) - start + 360.0f, 360.0f);
        const float rb = std::fmod(speakerAzimuth(l51, b) - start + 360.0f, 360.0f);
        return ra < rb;
    });
    return n;
}

void SurroundTestScreen::startSweep(int mode) {
    auto surround = Application::getInstance().getSurroundTestService();
    if (!surround) return;

    if (mode == 0) surround->set51Layout(true);
    else if (mode == 1) surround->set51Layout(false);

    /* 360 is one continuous revolution panned by the gain matrix (the 3D
     * SOUND FIELD engine), so what you hear can't drift from the orb. */
    if (!(mode == 2 ? surround->startField() : surround->start())) {
        toast("SURROUND TEST", "Audio port unavailable");
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    }
    if (mode != 2) surround->stopField();

    m_sweepMode = mode;
    m_sweepAngle = speakerAzimuth(surround->is51Layout(), 0) * M_PI / 180.0;   /* from FRONT LEFT */
    m_sweepTravel = 0.0;
    m_sweepStep = 0;
    m_sweepMs = 0.0;
    m_toneFollow = false;
    m_focusPane = PaneActions;
    evo_feedback(EVO_FB_CONFIRM);

    if (mode == 0)      toast("AUTO TEST", "5.1 - 6 channel sequence");
    else if (mode == 1) toast("AUTO TEST", "7.1 - 8 channel sequence");
    else                toast("360 SWEEP", "The source circles the listener");
}

void SurroundTestScreen::stopSweep() {
    if (m_sweepMode == 2)
        if (auto surround = Application::getInstance().getSurroundTestService()) surround->stopField();
    m_sweepMode = -1;
    m_sweepStep = 0;
    m_sweepMs = 0.0;
}

/* ----------------------------------------------------------- calibration */

void SurroundTestScreen::startCalibration() {
    auto surround = Application::getInstance().getSurroundTestService();
    if (!surround) return;
    surround->stop();                 /* nothing else may play during the run */
    m_calApplied = false;
    m_calStarted = m_cal->start(surround, surround->is51Layout());
    evo_feedback(m_calStarted ? EVO_FB_CONFIRM : EVO_FB_BOUNDARY);
}

void SurroundTestScreen::applyCalibration() {
    evo_speaker_cal_t profile;
    if (!m_cal->buildProfile(&profile)) {
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    }
    const bool saved = evo_speaker_cal_apply(&profile) != 0;
    m_calApplied = true;
    evo_feedback(EVO_FB_CONFIRM);
    toast("AUTO CALIBRATION", saved ? "Applied to playback and saved"
                                    : "Applied for this session - saving failed");
}

void SurroundTestScreen::leaveCalibration() {
    m_cal->cancel();
    if (auto surround = Application::getInstance().getSurroundTestService()) surround->stop();
    m_calStarted = false;
    m_focusPane = PaneActions;
    evo_feedback(EVO_FB_CANCEL);
}

/* ----------------------------------------------------------------- input */

bool SurroundTestScreen::handleOrbInput(uint32_t pressed) {
    /* D-pad, stick, touchpad and held L1/R1 are read in updateOrbMotion(). */
    if (pressed & PadButtons::Cross) {
        auto surround = Application::getInstance().getSurroundTestService();
        if (m_toneFollow && surround) {                  /* 3D SOUND FIELD: play / pause */
            if (surround->isFieldPlaying()) surround->stopField();
            else surround->startField();
            evo_feedback(EVO_FB_TOGGLE);
        } else {
            playChannel(m_loudest);
        }
        return true;
    }
    if (pressed & PadButtons::Triangle) {
        setFlight(static_cast<Flight>((m_flight + 1) % 3));
        evo_feedback(EVO_FB_TOGGLE);
        static const char* kNames[3] = { "Manual", "Orbit", "Flyby" };
        toast("FLIGHT MODE", kNames[m_flight]);
        return true;
    }
    if (pressed & PadButtons::Square) {
        resetOrb();
        return true;
    }
    if (pressed & PadButtons::L1) { m_src.z = clampf(m_src.z - kHeightTapM, -spatial::kMaxHeightM, spatial::kMaxHeightM); return true; }
    if (pressed & PadButtons::R1) { m_src.z = clampf(m_src.z + kHeightTapM, -spatial::kMaxHeightM, spatial::kMaxHeightM); return true; }
    if (pressed & (PadButtons::Circle | PadButtons::L3)) {
        if (m_toneFollow) {
            if (auto surround = Application::getInstance().getSurroundTestService()) surround->stopField();
        }
        switchPane(-1);
        return true;
    }
    /* swallow the D-pad so the stick-synthesised presses don't leak */
    return (pressed & (PadButtons::Up | PadButtons::Down | PadButtons::Left | PadButtons::Right)) != 0;
}

bool SurroundTestScreen::handleCalibrationInput(uint32_t pressed) {
    const auto phase = m_cal->snapshot().phase;
    using Phase = SpeakerCalibrationService::Phase;
    const bool busy = m_cal->isRunning();

    if (pressed & PadButtons::Cross) {
        if (phase == Phase::Complete && m_calStarted) leaveCalibration();   /* NEXT */
        else if (!busy) startCalibration();                                  /* START */
        return true;
    }
    if (pressed & PadButtons::Triangle) {                                   /* REPEAT */
        m_cal->cancel();
        startCalibration();
        return true;
    }
    if (pressed & PadButtons::Square) {                                     /* APPLY */
        if (phase == Phase::Complete && m_calStarted) applyCalibration();
        else evo_feedback(EVO_FB_BOUNDARY);
        return true;
    }
    if (pressed & PadButtons::Circle) {                                     /* BACK */
        leaveCalibration();
        return true;
    }
    return (pressed & (PadButtons::Up | PadButtons::Down | PadButtons::Left | PadButtons::Right)) != 0;
}

bool SurroundTestScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    if (m_focusPane == PaneOrb) return handleOrbInput(pressed);
    if (m_focusPane == PaneCalibration) return handleCalibrationInput(pressed);

    if (m_focusPane == PaneSpeakers) {
        if (pressed & PadButtons::Up)    { navigateSpeakers2D(0, -1); return true; }
        if (pressed & PadButtons::Down)  { navigateSpeakers2D(0, 1);  return true; }
        if (pressed & PadButtons::Left)  { navigateSpeakers2D(-1, 0); return true; }
        if (pressed & PadButtons::Right) { navigateSpeakers2D(1, 0);  return true; }
        if (pressed & PadButtons::Cross) { stopSweep(); playChannel(m_selectedSpeaker); return true; }
        if (pressed & PadButtons::Square)   { toggleLayout(); return true; }
        if (pressed & PadButtons::Triangle) { silence(); return true; }
        if (pressed & PadButtons::Circle)   { switchPane(-1); return true; }
        return false;
    }

    if (pressed & PadButtons::Up)       { navigateActions(-1); return true; }
    if (pressed & PadButtons::Down)     { navigateActions(1); return true; }
    if (pressed & PadButtons::Right)    { switchPane(1); return true; }
    if (pressed & PadButtons::Cross)    { activateSelection(); return true; }
    if (pressed & PadButtons::Square)   { toggleLayout(); return true; }
    if (pressed & PadButtons::Triangle) { silence(); return true; }
    if (pressed & PadButtons::Circle) {
        stopSweep();
        evo_feedback(EVO_FB_CANCEL);
        if (auto sm = Application::getInstance().getScreenManager()) {
            sm->navigateBack(ScreenId::SettingsAudio);
        }
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------- update */

void SurroundTestScreen::easeSourceTo(float x, float y, float z, double dt) {
    /* frame-rate independent exponential glide (~120 ms time constant) */
    const float k = 1.0f - static_cast<float>(std::exp(-dt / 0.12));
    m_src.x += (x - m_src.x) * k;
    m_src.y += (y - m_src.y) * k;
    m_src.z += (z - m_src.z) * k;
}

void SurroundTestScreen::updateOrbMotion(double dt) {
    Application& app = Application::getInstance();
    const uint32_t raw = app.getPadButtons();
    const uint32_t rawPressed = raw & ~m_prevRaw;
    m_prevRaw = raw;

    float mx = 0.0f, my = 0.0f;

    /* D-pad: one 18 dp step per tap, a 6 dp/frame glide once held */
    const uint32_t dpad = raw & (PadButtons::Up | PadButtons::Down | PadButtons::Left | PadButtons::Right);
    const float dirX = ((raw & PadButtons::Right) ? 1.0f : 0.0f) - ((raw & PadButtons::Left) ? 1.0f : 0.0f);
    const float dirY = ((raw & PadButtons::Down) ? 1.0f : 0.0f) - ((raw & PadButtons::Up) ? 1.0f : 0.0f);
    if (rawPressed & dpad) {
        const float tx = ((rawPressed & PadButtons::Right) ? 1.0f : 0.0f) - ((rawPressed & PadButtons::Left) ? 1.0f : 0.0f);
        const float ty = ((rawPressed & PadButtons::Down) ? 1.0f : 0.0f) - ((rawPressed & PadButtons::Up) ? 1.0f : 0.0f);
        mx += tx * kTapStepM;
        my += ty * kTapStepM;
        m_dpadHeldMs = 0.0;
    } else if (dpad) {
        m_dpadHeldMs += dt * 1000.0;
        if (m_dpadHeldMs >= kRepeatDelayMs) {
            mx += dirX * kGlideMps * static_cast<float>(dt);
            my += dirY * kGlideMps * static_cast<float>(dt);
        }
    }

    /* left stick (Application already applied the deadzone) */
    float sx = 0.0f, sy = 0.0f;
    app.getLeftStick(&sx, &sy);
    mx += sx * kStickMps * static_cast<float>(dt);
    my += sy * kStickMps * static_cast<float>(dt);

    /* touchpad: drag, relative to where the finger went down */
    float tx = 0.0f, ty = 0.0f;
    const bool touch = app.getTouchpad(&tx, &ty);
    if (touch && m_touchWasActive) {
        mx += (tx - m_touchX) * kTouchSpanM;
        my += (ty - m_touchY) * kTouchSpanM * (1080.0f / 1920.0f);
    }
    m_touchWasActive = touch;
    m_touchX = tx;
    m_touchY = ty;

    /* L1 / R1 held: continuous height */
    const uint32_t hgt = raw & (PadButtons::L1 | PadButtons::R1);
    if (hgt && !(rawPressed & hgt)) {
        m_heightHeldMs += dt * 1000.0;
        if (m_heightHeldMs >= kRepeatDelayMs) {
            const float dz = ((raw & PadButtons::R1) ? 1.0f : 0.0f) - ((raw & PadButtons::L1) ? 1.0f : 0.0f);
            m_src.z = clampf(m_src.z + dz * kHeightMps * static_cast<float>(dt),
                             -spatial::kMaxHeightM, spatial::kMaxHeightM);
        }
    } else {
        m_heightHeldMs = 0.0;
    }

    /* any hand on the controls takes the source back from the autopilot */
    if ((mx != 0.0f || my != 0.0f) && m_flight != FlightManual) m_flight = FlightManual;

    if (m_flight == FlightOrbit) {
        m_orbitAngle = std::fmod(m_orbitAngle + kOrbitRadPerS * dt, 2.0 * M_PI);
        easeSourceTo(static_cast<float>(m_orbitRadius * std::sin(m_orbitAngle)),
                     static_cast<float>(-m_orbitRadius * std::cos(m_orbitAngle)), m_src.z, dt);
    } else if (m_flight == FlightFlyby) {
        /* in from the front, through the seat, out the back - then again */
        m_flybyT += dt / kFlybySec;
        if (m_flybyT >= 1.0) m_flybyT -= 1.0;
        const double t = m_flybyT;
        const float y = static_cast<float>(-3.2 + 6.4 * t);
        const float x = static_cast<float>(0.6 * std::sin(t * M_PI * 1.5));
        easeSourceTo(x, y, m_src.z, dt);
    } else {
        m_src.x += mx;
        m_src.y += my;
    }

    /* keep the source inside the room */
    const float r = std::sqrt(m_src.x * m_src.x + m_src.y * m_src.y);
    if (r > spatial::kMaxRadiusM) {
        m_src.x *= spatial::kMaxRadiusM / r;
        m_src.y *= spatial::kMaxRadiusM / r;
    }
}

void SurroundTestScreen::updateField() {
    m_loudest = spatial::panGains(m_src, is51(), m_gains);
}

void SurroundTestScreen::recordTrail(double deltaMs) {
    m_trailMs += deltaMs;
    if (m_trailMs < 70.0) return;
    m_trailMs = 0.0;
    const float gx = m_src.x * spatial::kDpPerMeter, gy = m_src.y * spatial::kDpPerMeter;
    if (m_trailCount > 0) {
        const float dx = gx - m_trailX[0], dy = gy - m_trailY[0];
        if (dx * dx + dy * dy < 16.0f) {           /* parked: let the trail fade out */
            --m_trailCount;
            return;
        }
    }
    for (int i = kTrail - 1; i > 0; --i) {
        m_trailX[i] = m_trailX[i - 1];
        m_trailY[i] = m_trailY[i - 1];
    }
    m_trailX[0] = gx;
    m_trailY[0] = gy;
    if (m_trailCount < kTrail) ++m_trailCount;
}

void SurroundTestScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
    m_animTimeSec += deltaMs / 1000.0;
    const double dt = std::min(deltaMs, 100.0) / 1000.0;
    const bool l51 = is51();

    auto surround = Application::getInstance().getSurroundTestService();

    if (m_focusPane == PaneOrb) {
        updateOrbMotion(dt);
    } else {
        m_prevRaw = Application::getInstance().getPadButtons();
        m_touchWasActive = false;
        if (m_focusPane == PaneCalibration) {
            easeSourceTo(0.0f, 0.0f, 0.0f, dt);              /* the mic at the seat */
        } else if (m_sweepMode == 2) {
            /* 360: clockwise at a steady rate, on a radius that follows the
             * speaker ring (85 % of the way out) so each speaker is passed
             * close by. */
            const double step = 2.0 * M_PI * dt / kSweepRevSec;
            m_sweepAngle = std::fmod(m_sweepAngle + step, 2.0 * M_PI);
            m_sweepTravel += step;
            int rot[spatial::kChannels];
            const int n = rotationOrder(rot);
            double rad = 2.4;
            if (n > 1) {
                const double az = m_sweepAngle * 180.0 / M_PI;
                for (int k = 0; k < n; ++k) {
                    const int a = rot[k], b = rot[(k + 1) % n];
                    double aa = speakerAzimuth(l51, a), ab = speakerAzimuth(l51, b), x = az;
                    if (ab < aa) ab += 360.0;
                    if (x < aa) x += 360.0;
                    if (x >= aa && x <= ab) {
                        const double t = (ab > aa) ? (x - aa) / (ab - aa) : 0.0;
                        const spatial::Vec3 pa = spatial::speakerPosition(l51, a);
                        const spatial::Vec3 pb = spatial::speakerPosition(l51, b);
                        rad = 0.85 * (std::sqrt(pa.x * pa.x + pa.y * pa.y) * (1.0 - t) +
                                      std::sqrt(pb.x * pb.x + pb.y * pb.y) * t);
                        break;
                    }
                }
            }
            m_src.x = static_cast<float>(rad * std::sin(m_sweepAngle));
            m_src.y = static_cast<float>(-rad * std::cos(m_sweepAngle));
            m_src.z = 0.0f;
        } else if (m_sweepMode == 0 || m_sweepMode == 1 || m_focusPane == PaneSpeakers) {
            /* hover in front of the speaker being tested / under the cursor */
            const spatial::Vec3 p = spatial::speakerPosition(l51, m_selectedSpeaker);
            easeSourceTo(p.x * 0.7f, p.y * 0.7f, 0.0f, dt);
        } else {
            easeSourceTo(0.0f, -1.2f, 0.0f, dt);             /* resting in front */
        }
    }
    updateField();
    recordTrail(deltaMs);

    /* 3D SOUND FIELD: the music is panned continuously with the gain
     * matrix, a little louder as the source comes closer to the seat. */
    if (m_focusPane == PaneOrb && m_toneFollow && surround && surround->isFieldPlaying()) {
        const float master = clampf(1.0f / (0.75f + 0.25f * spatial::rangeM(m_src)), 0.45f, 1.0f);
        surround->setFieldGains(m_gains, master);
    }

    if (m_sweepMode < 0) return;
    if (!surround) { stopSweep(); return; }

    if (m_sweepMode == 2) {
        surround->setFieldGains(m_gains, 1.0f);
        m_selectedSpeaker = m_loudest >= 0 ? m_loudest : m_selectedSpeaker;
        if (m_sweepTravel >= 2.0 * M_PI) {
            stopSweep();
            toast("360 SWEEP", "Complete");
        }
        return;
    }

    int order[spatial::kChannels];
    const int count = spatial::physicalOrder(l51, order);
    if (count <= 0) { stopSweep(); return; }

    if (m_sweepMs <= 0.0) {
        const int ch = order[m_sweepStep % count];
        surround->triggerTone(l51, ch);
        m_selectedSpeaker = ch;
        m_sweepMs = kSweepDwellMs;
    }

    m_sweepMs -= deltaMs;
    if (m_sweepMs <= 0.0) {
        ++m_sweepStep;
        if (m_sweepStep >= count) {
            stopSweep();
            toast("SURROUND TEST", "Sweep complete");
        }
    }
}

/* ---------------------------------------------------------------- render */

void SurroundTestScreen::render(uint32_t* framebuffer, int width, int height) {
    auto surround = Application::getInstance().getSurroundTestService();
    const bool l51 = is51();
    const float ppm = spatial::kDpPerMeter;

    evo_rmlui_surround_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.rail_focused = 0;
    params.is_51_layout = l51 ? 1 : 0;
    params.selected_item = (m_focusPane == PaneSpeakers) ? EVO_RMLUI_SURROUND_ACTIONS + m_selectedSpeaker
                                                         : m_selectedAction;
    params.active_channel = (surround && surround->isActive()) ? surround->getCurrentChannel() : -1;
    params.surround_mode = (m_focusPane == PaneOrb) ? 2 : ((m_sweepMode >= 0) ? 1 : 0);
    params.view_mode = (m_focusPane == PaneOrb) ? EVO_SURROUND_VIEW_ORB
                     : (m_focusPane == PaneCalibration) ? EVO_SURROUND_VIEW_CALIBRATION
                     : EVO_SURROUND_VIEW_STAGE;
    params.anim_time = static_cast<float>(m_animTimeSec);
    params.orb_active = (m_focusPane == PaneOrb) ? 1 : 0;
    params.flight_mode = m_flight;
    params.tone_follow = m_toneFollow ? 1 : 0;
    params.field_playing = (surround && surround->isFieldPlaying()) ? 1 : 0;
    params.sweep_rotation = (m_sweepMode == 2) ? 1 : 0;
    params.nearest_channel = m_loudest;
    params.px_per_m = ppm;

    params.orb_x = m_src.x * ppm;
    params.orb_y = m_src.y * ppm;
    params.orb_z_m = m_src.z;
    params.azimuth_deg = spatial::azimuthDeg(m_src);
    params.elevation_deg = spatial::elevationDeg(m_src);
    params.distance_m = spatial::rangeM(m_src);
    params.x_m = m_src.x;
    params.y_m = -m_src.y;                 /* telemetry: +Y is towards the screen */
    params.source_dbfs = spatial::kToneDbfs;

    params.trail_count = m_trailCount;
    for (int i = 0; i < m_trailCount && i < EVO_RMLUI_SURROUND_TRAIL; ++i) {
        params.trail_x[i] = m_trailX[i];
        params.trail_y[i] = m_trailY[i];
    }
    params.order_count = spatial::physicalOrder(l51, params.order);

    params.speaker_count = spatial::kChannels;
    for (int ch = 0; ch < spatial::kChannels; ++ch) {
        const spatial::Vec3 p = spatial::speakerPosition(l51, ch);
        params.speakers[ch].name = spatial::speakerName(l51, ch);
        params.speakers[ch].label = spatial::speakerLabel(l51, ch);
        params.speakers[ch].ch = ch;
        params.speakers[ch].item_idx = EVO_RMLUI_SURROUND_ACTIONS + ch;
        params.speakers[ch].dx = static_cast<int>(std::lround(p.x * ppm));
        params.speakers[ch].dy = static_cast<int>(std::lround(p.y * ppm));
        params.speakers[ch].hidden = spatial::present(l51, ch) ? 0 : 1;
        params.speakers[ch].hz = (ch == 3) ? 60.0 : 440.0;   /* SurroundTestService's tones */
        params.proximity[ch] = m_gains[ch];
    }

    /* calibration */
    const SpeakerCalibrationService::Snapshot cal = m_cal->snapshot();
    using Phase = SpeakerCalibrationService::Phase;
    switch (m_calStarted ? cal.phase : Phase::Idle) {
    case Phase::MicCheck:  params.cal_phase = EVO_SURROUND_CAL_MIC_CHECK; break;
    case Phase::Measuring: params.cal_phase = EVO_SURROUND_CAL_MEASURING; break;
    case Phase::Analyzing: params.cal_phase = EVO_SURROUND_CAL_ANALYZING; break;
    case Phase::Complete:  params.cal_phase = EVO_SURROUND_CAL_COMPLETE; break;
    case Phase::Error:     params.cal_phase = EVO_SURROUND_CAL_ERROR; break;
    default:               params.cal_phase = EVO_SURROUND_CAL_INTRO; break;
    }
    params.cal_step = cal.step;
    params.cal_total = cal.total > 0 ? cal.total : params.order_count;
    params.cal_channel = cal.channel;
    params.cal_verifying = cal.verifying ? 1 : 0;
    params.cal_applied = m_calApplied ? 1 : 0;
    params.cal_noise_db = cal.noiseDb;
    params.cal_mic_level = cal.micLevel;
    params.cal_message = cal.message;
    for (int ch = 0; ch < spatial::kChannels; ++ch) {
        const SpeakerCalibrationService::ChannelResult& r = cal.results[ch];
        params.cal_measured[ch] = r.measured ? 1 : 0;
        params.cal_detected[ch] = r.detected ? 1 : 0;
        params.cal_trim_db[ch] = r.trimDb;
        params.cal_path_m[ch] = r.pathM;
        params.cal_delay_ms[ch] = r.delayMs;
    }
    evo_speaker_cal_t active;
    evo_speaker_cal_get(&active);
    params.cal_profile_active = active.enabled;

    evo_rmlui_update_surround(&params);
    evo_rmlui_render_surround(framebuffer, width, height);
}

} // namespace evo
