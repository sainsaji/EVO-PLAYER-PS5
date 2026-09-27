#include "evo/screens/SurroundTestScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_toast.h"

#include <cstdio>
#include <cstring>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace evo {

static const struct { int dx, dy; } kSpeakerPos8[8] = {
    { -280, -175 },   /* 0 FL  front left   */
    {  280, -175 },   /* 1 FR  front right  */
    {    0, -225 },   /* 2 FC  centre       */
    {    0,  215 },   /* 3 LFE subwoofer    */
    { -220,  195 },   /* 4 BL  back left    */
    {  220,  195 },   /* 5 BR  back right   */
    { -350,   30 },   /* 6 SL  side left    */
    {  350,   30 },   /* 7 SR  side right   */
};

SurroundTestScreen::SurroundTestScreen()
    : StatefulScreen("SurroundTestScreen") {
}

void SurroundTestScreen::onExit() {
    StatefulScreen::onExit();
    if (auto surround = Application::getInstance().getSurroundTestService()) {
        surround->stop();
    }
}

int SurroundTestScreen::speakerCount() const {
    auto surround = Application::getInstance().getSurroundTestService();
    return (surround && surround->is51Layout()) ? 6 : 8;
}

void SurroundTestScreen::onEnter() {
    StatefulScreen::onEnter();
    m_focusPane = PaneActions;
    m_selectedAction = 0;
    m_selectedSpeaker = 0;
    m_isOrbMode = false;
    m_orbX = 0.0f;
    m_orbY = 0.0f;
    m_targetOrbX = 0.0f;
    m_targetOrbY = 0.0f;
    stopSweep();
    updateProximities();
}

void SurroundTestScreen::navigateActions(int dir) {
    m_selectedAction += dir;
    if (m_selectedAction < 0) {
        m_selectedAction = 0;
        evo_feedback(EVO_FB_BOUNDARY);
    } else if (m_selectedAction >= actionCount()) {
        m_selectedAction = actionCount() - 1;
        evo_feedback(EVO_FB_BOUNDARY);
    } else {
        evo_feedback(EVO_FB_MOVE);
    }
}

void SurroundTestScreen::navigateSpeakers2D(int dirX, int dirY) {
    auto surround = Application::getInstance().getSurroundTestService();
    const bool is51 = surround ? surround->is51Layout() : false;

    int cur = m_selectedSpeaker;
    int next = cur;

    if (dirX < 0) { // LEFT
        switch (cur) {
            case 2: next = 0; break;               // FC -> FL
            case 1: next = 2; break;               // FR -> FC
            case 7: next = is51 ? 2 : 3; break;    // SR -> LFE / FC
            case 5: next = 3; break;               // BR -> LFE
            case 3: next = 4; break;               // LFE -> BL
            case 0: case 6: case 4:                // Leftmost speakers
                switchPane(-1);                    // Return to Actions column
                return;
            default: break;
        }
    } else if (dirX > 0) { // RIGHT
        switch (cur) {
            case 0: next = 2; break;               // FL -> FC
            case 2: next = 1; break;               // FC -> FR
            case 6: next = 3; break;               // SL -> LFE
            case 4: next = 3; break;               // BL -> LFE
            case 3: next = 5; break;               // LFE -> BR
            case 1: case 7: case 5:                // Right edge boundary
                evo_feedback(EVO_FB_BOUNDARY);
                return;
            default: break;
        }
    } else if (dirY < 0) { // UP
        switch (cur) {
            case 6: next = 0; break;               // SL -> FL
            case 7: next = 1; break;               // SR -> FR
            case 4: next = is51 ? 0 : 6; break;    // BL -> SL (7.1) or FL (5.1)
            case 5: next = is51 ? 1 : 7; break;    // BR -> SR (7.1) or FR (5.1)
            case 3: next = 2; break;               // LFE -> FC
            case 0: case 2: case 1:                // Front row boundary
                evo_feedback(EVO_FB_BOUNDARY);
                return;
            default: break;
        }
    } else if (dirY > 0) { // DOWN
        switch (cur) {
            case 0: next = is51 ? 4 : 6; break;    // FL -> SL (7.1) or BL (5.1)
            case 1: next = is51 ? 5 : 7; break;    // FR -> SR (7.1) or BR (5.1)
            case 2: next = 3; break;               // FC -> LFE
            case 6: next = 4; break;               // SL -> BL
            case 7: next = 5; break;               // SR -> BR
            case 4: case 3: case 5:                // Back row boundary
                evo_feedback(EVO_FB_BOUNDARY);
                return;
            default: break;
        }
    }

    if (is51 && (next == 6 || next == 7)) {
        next = (next == 6) ? 4 : 5;
    }

    if (next != cur) {
        m_selectedSpeaker = next;
        evo_feedback(EVO_FB_MOVE);
    } else {
        evo_feedback(EVO_FB_BOUNDARY);
    }
}

void SurroundTestScreen::moveOrb(float dx, float dy) {
    m_orbX += dx;
    m_orbY += dy;

    // Clamp orb to sound stage bounds
    const float kMaxX = 350.0f;
    const float kMaxY = 240.0f;
    if (m_orbX < -kMaxX) m_orbX = -kMaxX;
    if (m_orbX >  kMaxX) m_orbX =  kMaxX;
    if (m_orbY < -kMaxY) m_orbY = -kMaxY;
    if (m_orbY >  kMaxY) m_orbY =  kMaxY;

    updateProximities();
    evo_feedback(EVO_FB_MOVE);
}

void SurroundTestScreen::updateProximities() {
    auto surround = Application::getInstance().getSurroundTestService();
    const bool is51 = surround ? surround->is51Layout() : false;

    float maxP = 0.0f;
    int nearestSpk = -1;

    for (int i = 0; i < 8; ++i) {
        if (is51 && (i == 6 || i == 7)) {
            m_speakerProximity[i] = 0.0f;
            continue;
        }
        float spkX = static_cast<float>(kSpeakerPos8[i].dx) * 0.88f;
        float spkY = static_cast<float>(kSpeakerPos8[i].dy) * 0.88f;
        float dX = m_orbX - spkX;
        float dY = m_orbY - spkY;
        float dist = std::sqrt(dX * dX + dY * dY);

        const float kRadius = 180.0f;
        float p = (dist < kRadius) ? (1.0f - dist / kRadius) : 0.0f;
        m_speakerProximity[i] = p;

        if (p > maxP) {
            maxP = p;
            nearestSpk = i;
        }
    }

    if (m_focusPane == PaneOrb && nearestSpk >= 0) {
        m_selectedSpeaker = nearestSpk;
    }
}

void SurroundTestScreen::switchPane(int dir) {
    if (dir > 0) {
        if (m_focusPane == PaneActions) {
            m_focusPane = PaneSpeakers;
            m_isOrbMode = false;
            evo_feedback(EVO_FB_MOVE);
        } else if (m_focusPane == PaneSpeakers) {
            evo_feedback(EVO_FB_BOUNDARY);
        }
    } else {
        if (m_focusPane == PaneSpeakers || m_focusPane == PaneOrb) {
            m_focusPane = PaneActions;
            m_isOrbMode = false;
            evo_feedback(EVO_FB_MOVE);
        } else {
            evo_feedback(EVO_FB_BOUNDARY);
        }
    }
}

void SurroundTestScreen::startSweep(int mode) {
    auto surround = Application::getInstance().getSurroundTestService();
    if (!surround) return;

    if (mode == 0) surround->set51Layout(true);
    else if (mode == 1) surround->set51Layout(false);

    if (!surround->start()) {
        toast("SURROUND TEST", "Audio port unavailable");
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    }

    m_sweepMode = mode;
    m_sweepStep = 0;
    m_sweepMs = 0.0;
    m_isOrbMode = false;
    evo_feedback(EVO_FB_CONFIRM);

    if (mode == 0)      toast("AUTO TEST", "5.1 - 6 channel calibration");
    else if (mode == 1) toast("AUTO TEST", "7.1 - 8 channel calibration");
    else                toast("360 SWEEP", "Circular perimeter pan");
}

void SurroundTestScreen::stopSweep() {
    m_sweepMode = -1;
    m_sweepStep = 0;
    m_sweepMs = 0.0;
}

void SurroundTestScreen::activateSelection() {
    auto surround = Application::getInstance().getSurroundTestService();

    if (m_focusPane == PaneOrb) {
        // Trigger tone on nearest speaker to the orb
        playSelectedChannel();
        return;
    }

    if (m_focusPane == PaneSpeakers) {
        stopSweep();
        playSelectedChannel();
        return;
    }

    switch (m_selectedAction) {
    case 0: /* FREE-ROAM SOUND ORB */
        stopSweep();
        m_focusPane = PaneOrb;
        m_isOrbMode = true;
        evo_feedback(EVO_FB_CONFIRM);
        toast("FREE-ROAM ORB", "D-Pad / Left Stick to position sound");
        break;
    case 1: startSweep(2); break;              /* 360 ROTATION SWEEP */
    case 2: startSweep(0); break;              /* AUTO TEST 5.1 */
    case 3: startSweep(1); break;              /* AUTO TEST 7.1 */
    case 4:                                    /* SPEAKER LAYOUT */
        if (surround) {
            const bool next51 = !surround->is51Layout();
            surround->set51Layout(next51);
            if (m_selectedSpeaker >= speakerCount()) m_selectedSpeaker = speakerCount() - 1;
            updateProximities();
            evo_feedback(EVO_FB_TOGGLE);
            toast("SPEAKER LAYOUT", next51 ? "5.1 Surround (6 Channels)"
                                           : "7.1 Surround (8 Channels)");
        }
        break;
    case 5:                                    /* SILENCE / STOP */
    default:
        stopSweep();
        if (surround) surround->stop();
        m_isOrbMode = false;
        evo_feedback(EVO_FB_CANCEL);
        toast("SURROUND TEST", "Silenced");
        break;
    }
}

void SurroundTestScreen::playSelectedChannel() {
    auto surround = Application::getInstance().getSurroundTestService();
    if (surround) {
        evo_feedback(EVO_FB_CONFIRM);
        surround->triggerTone(surround->is51Layout(), m_selectedSpeaker);
    }
}

void SurroundTestScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
    m_animTimeSec += deltaMs / 1000.0;

    auto surround = Application::getInstance().getSurroundTestService();

    // Orb positioning logic
    if (m_focusPane == PaneOrb) {
        // In free-roam mode, allow fluid continuous movement with left analog stick
        float stickX = 0.0f, stickY = 0.0f;
        Application::getInstance().getLeftStick(&stickX, &stickY);
        if (std::abs(stickX) > 0.01f || std::abs(stickY) > 0.01f) {
            float speed = 380.0f; // pixels per second
            float dt = static_cast<float>(deltaMs / 1000.0);
            moveOrb(stickX * speed * dt, stickY * speed * dt);
        }
        updateProximities();
    } else if (m_sweepMode == 2) {
        // 360 Rotation Sweep: orb orbits in a circle
        static const int kRotation[] = {0, 2, 1, 7, 5, 4, 6};
        const int count = static_cast<int>(sizeof(kRotation) / sizeof(kRotation[0]));
        double sweepAngle = (m_sweepStep * (2.0 * M_PI / count)) +
                            ((1.0 - m_sweepMs / 900.0) * (2.0 * M_PI / count));
        m_orbX = static_cast<float>(260.0 * std::sin(sweepAngle));
        m_orbY = static_cast<float>(-170.0 * std::cos(sweepAngle));
        updateProximities();
    } else if (m_sweepMode == 0 || m_sweepMode == 1) {
        // Step auto-test: orb glides towards the active test speaker (in front of cabinet)
        int ch = (m_selectedSpeaker >= 0 && m_selectedSpeaker < 8) ? m_selectedSpeaker : 0;
        m_targetOrbX = static_cast<float>(kSpeakerPos8[ch].dx) * 0.62f;
        m_targetOrbY = static_cast<float>(kSpeakerPos8[ch].dy) * 0.62f;
        m_orbX += (m_targetOrbX - m_orbX) * 0.22f;
        m_orbY += (m_targetOrbY - m_orbY) * 0.22f;
        updateProximities();
    } else if (m_focusPane == PaneSpeakers) {
        // Speaker focus mode: orb smoothly hovers in front of the selected speaker
        int spk = (m_selectedSpeaker >= 0 && m_selectedSpeaker < 8) ? m_selectedSpeaker : 0;
        m_targetOrbX = static_cast<float>(kSpeakerPos8[spk].dx) * 0.62f;
        m_targetOrbY = static_cast<float>(kSpeakerPos8[spk].dy) * 0.62f;
        m_orbX += (m_targetOrbX - m_orbX) * 0.20f;
        m_orbY += (m_targetOrbY - m_orbY) * 0.20f;
        updateProximities();
    } else {
        // Actions mode idle: gentle subtle ambient drift around sweet spot
        m_orbX = static_cast<float>(250.0 * std::cos(m_animTimeSec * 0.8));
        m_orbY = static_cast<float>(160.0 * std::sin(m_animTimeSec * 0.8));
        updateProximities();
    }

    if (m_sweepMode < 0) return;
    if (!surround) { stopSweep(); return; }

    static const int kRotation[] = {0, 2, 1, 7, 5, 4, 6};
    const int count = (m_sweepMode == 2)
                        ? static_cast<int>(sizeof(kRotation) / sizeof(kRotation[0]))
                        : speakerCount();

    const double kDwellMs = 900.0;
    if (m_sweepMs <= 0.0) {
        const int ch = (m_sweepMode == 2) ? kRotation[m_sweepStep % count] : m_sweepStep;
        surround->triggerTone(surround->is51Layout(), ch);
        m_selectedSpeaker = (ch < speakerCount()) ? ch : 0;
        m_sweepMs = kDwellMs;
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

bool SurroundTestScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)released;

    auto surround = Application::getInstance().getSurroundTestService();

    // 1. Interactive Free-Roam Orb Control Mode
    if (m_focusPane == PaneOrb) {
        const float kTapStep = 18.0f;
        const float kHeldStep = 6.0f;
        if (pressed & PadButtons::Up)        { moveOrb(0.0f, -kTapStep); return true; }
        else if (held & PadButtons::Up)     { moveOrb(0.0f, -kHeldStep); return true; }
        if (pressed & PadButtons::Down)      { moveOrb(0.0f, kTapStep); return true; }
        else if (held & PadButtons::Down)   { moveOrb(0.0f, kHeldStep); return true; }
        if (pressed & PadButtons::Left)      { moveOrb(-kTapStep, 0.0f); return true; }
        else if (held & PadButtons::Left)   { moveOrb(-kHeldStep, 0.0f); return true; }
        if (pressed & PadButtons::Right)     { moveOrb(kTapStep, 0.0f); return true; }
        else if (held & PadButtons::Right)  { moveOrb(kHeldStep, 0.0f); return true; }
        if (pressed & PadButtons::Cross) {
            activateSelection();
            return true;
        }
        if (pressed & PadButtons::Triangle) {
            if (surround) surround->stop();
            evo_feedback(EVO_FB_CANCEL);
            toast("SURROUND TEST", "Silenced");
            return true;
        }
        if (pressed & PadButtons::Square) {
            if (surround) {
                const bool next51 = !surround->is51Layout();
                surround->set51Layout(next51);
                updateProximities();
                evo_feedback(EVO_FB_TOGGLE);
                toast("SPEAKER LAYOUT", next51 ? "5.1 Surround (6 Channels)"
                                               : "7.1 Surround (8 Channels)");
            }
            return true;
        }
        if (pressed & (PadButtons::Circle | PadButtons::L3)) {
            m_focusPane = PaneActions;
            m_isOrbMode = false;
            evo_feedback(EVO_FB_CANCEL);
            toast("FREE-ROAM ORB", "Returned to Actions");
            return true;
        }
        return false;
    }

    // 2. Speaker 2D Spatial Navigation Mode
    if (m_focusPane == PaneSpeakers) {
        if (pressed & PadButtons::Up) {
            navigateSpeakers2D(0, -1);
            return true;
        }
        if (pressed & PadButtons::Down) {
            navigateSpeakers2D(0, 1);
            return true;
        }
        if (pressed & PadButtons::Left) {
            navigateSpeakers2D(-1, 0);
            return true;
        }
        if (pressed & PadButtons::Right) {
            navigateSpeakers2D(1, 0);
            return true;
        }
        if (pressed & PadButtons::Cross) {
            activateSelection();
            return true;
        }
        if (pressed & PadButtons::Square) {
            if (surround) {
                const bool next51 = !surround->is51Layout();
                surround->set51Layout(next51);
                evo_feedback(EVO_FB_TOGGLE);
                toast("SPEAKER LAYOUT", next51 ? "5.1 Surround (6 Channels)"
                                               : "7.1 Surround (8 Channels)");
                if (m_selectedSpeaker >= speakerCount()) {
                    m_selectedSpeaker = speakerCount() - 1;
                }
                updateProximities();
            }
            return true;
        }
        if (pressed & PadButtons::Triangle) {
            stopSweep();
            if (surround) {
                surround->stop();
                evo_feedback(EVO_FB_CANCEL);
                toast("SURROUND TEST", "Silenced");
            }
            return true;
        }
        if (pressed & PadButtons::Circle) {
            switchPane(-1);
            return true;
        }
        return false;
    }

    // 3. Actions List Mode
    if (pressed & PadButtons::Up) {
        navigateActions(-1);
        return true;
    }
    if (pressed & PadButtons::Down) {
        navigateActions(1);
        return true;
    }
    if (pressed & PadButtons::Right) {
        switchPane(1);
        return true;
    }
    if (pressed & PadButtons::Cross) {
        activateSelection();
        return true;
    }
    if (pressed & PadButtons::Square) {
        if (surround) {
            const bool next51 = !surround->is51Layout();
            surround->set51Layout(next51);
            evo_feedback(EVO_FB_TOGGLE);
            toast("SPEAKER LAYOUT", next51 ? "5.1 Surround (6 Channels)"
                                           : "7.1 Surround (8 Channels)");
            if (m_selectedSpeaker >= speakerCount()) {
                m_selectedSpeaker = speakerCount() - 1;
            }
            updateProximities();
        }
        return true;
    }
    if (pressed & PadButtons::Triangle) {
        stopSweep();
        if (surround) {
            surround->stop();
            evo_feedback(EVO_FB_CANCEL);
            toast("SURROUND TEST", "Silenced");
        }
        return true;
    }
    if (pressed & PadButtons::Circle) {
        stopSweep();
        evo_feedback(EVO_FB_CANCEL);
        if (auto sm = Application::getInstance().getScreenManager()) {
            sm->navigateBack(ScreenId::SettingsPlayback);
        }
        return true;
    }

    return false;
}

void SurroundTestScreen::render(uint32_t* framebuffer, int width, int height) {
    auto surround = Application::getInstance().getSurroundTestService();
    bool is51 = surround ? surround->is51Layout() : false;

    evo_rmlui_surround_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.rail_focused = 0;
    params.is_51_layout = is51 ? 1 : 0;
    if (m_focusPane == PaneActions) {
        params.selected_item = m_selectedAction;
    } else if (m_focusPane == PaneSpeakers) {
        params.selected_item = 6 + m_selectedSpeaker;
    } else {
        params.selected_item = 0; // Free-roam orb
    }
    params.active_channel = surround ? surround->getCurrentChannel() : -1;
    params.surround_mode = (m_focusPane == PaneOrb) ? 2 : ((m_sweepMode >= 0) ? 1 : 0);
    params.speaker_count = 8;
    params.anim_time = static_cast<float>(m_animTimeSec);
    params.orb_x = m_orbX;
    params.orb_y = m_orbY;
    params.orb_active = (m_focusPane == PaneOrb) ? 1 : 0;

    for (int i = 0; i < 8; ++i) {
        params.proximity[i] = m_speakerProximity[i];
    }

    static const char* speakerNames8[] = {
        "FRONT LEFT", "FRONT RIGHT", "CENTER",
        "LFE SUBWOOFER",
        "REAR LEFT", "REAR RIGHT",
        "SIDE LEFT", "SIDE RIGHT"
    };
    static const char* speakerLabels8[] = {
        "FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR"
    };

    for (int i = 0; i < 8; ++i) {
        params.speakers[i].name = speakerNames8[i];
        params.speakers[i].label = speakerLabels8[i];
        params.speakers[i].ch = i;
        params.speakers[i].item_idx = 6 + i;
        params.speakers[i].dx = kSpeakerPos8[i].dx;
        params.speakers[i].dy = kSpeakerPos8[i].dy;
        params.speakers[i].hidden = (is51 && (i == 6 || i == 7)) ? 1 : 0;
        params.speakers[i].hz = (i == 3) ? 80.0 : 440.0;
    }

    evo_rmlui_update_surround(&params);
    evo_rmlui_render_surround(framebuffer, width, height);
}

} // namespace evo
