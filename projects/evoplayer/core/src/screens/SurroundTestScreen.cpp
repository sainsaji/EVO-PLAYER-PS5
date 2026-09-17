#include "evo/screens/SurroundTestScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_toast.h"

#include <cstdio>
#include <cstring>

namespace evo {

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
    stopSweep();
}

void SurroundTestScreen::navigate(int dir) {
    const int total = (m_focusPane == PaneActions) ? actionCount() : speakerCount();
    int& cursor = (m_focusPane == PaneActions) ? m_selectedAction : m_selectedSpeaker;

    cursor += dir;
    if (cursor < 0) {
        cursor = 0;
        evo_feedback(EVO_FB_BOUNDARY);
    } else if (cursor >= total) {
        cursor = total - 1;
        evo_feedback(EVO_FB_BOUNDARY);
    } else {
        evo_feedback(EVO_FB_MOVE);
    }
}

void SurroundTestScreen::switchPane(int dir) {
    const int next = (dir > 0) ? PaneSpeakers : PaneActions;
    if (next == m_focusPane) {
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    }
    m_focusPane = next;
    if (m_focusPane == PaneSpeakers && m_selectedSpeaker >= speakerCount()) {
        m_selectedSpeaker = speakerCount() - 1;
    }
    evo_feedback(EVO_FB_MOVE);
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

    if (m_focusPane == PaneSpeakers) {
        stopSweep();
        playSelectedChannel();
        return;
    }

    switch (m_selectedAction) {
    case 0: startSweep(0); break;              /* AUTO TEST 5.1 */
    case 1: startSweep(1); break;              /* AUTO TEST 7.1 */
    case 2: startSweep(2); break;              /* 360 ROTATION SWEEP */
    case 3:                                    /* SPEAKER LAYOUT */
        if (surround) {
            const bool next51 = !surround->is51Layout();
            surround->set51Layout(next51);
            if (m_selectedSpeaker >= speakerCount()) m_selectedSpeaker = speakerCount() - 1;
            evo_feedback(EVO_FB_TOGGLE);
            toast("SPEAKER LAYOUT", next51 ? "5.1 Surround (6 Channels)"
                                           : "7.1 Surround (8 Channels)");
        }
        break;
    case 4:                                    /* SILENCE / STOP */
    default:
        stopSweep();
        if (surround) surround->stop();
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
    if (m_sweepMode < 0) return;

    auto surround = Application::getInstance().getSurroundTestService();
    if (!surround) { stopSweep(); return; }

    /* Perimeter order for the 360 sweep so the tone visibly travels around the
     * listener rather than following channel index. Indices match the render's
     * speaker table: FL FR FC LFE BL BR SL SR. */
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
    (void)held;
    (void)released;

    auto surround = Application::getInstance().getSurroundTestService();

    /* Up/Down move within the focused pane; Left/Right move between panes.
     * They used to be conflated - Up and Left both stepped the same linear
     * cursor - so the two-column layout could not be navigated. */
    if (pressed & PadButtons::Up) {
        navigate(-1);
        return true;
    }
    if (pressed & PadButtons::Down) {
        navigate(1);
        return true;
    }
    if (pressed & PadButtons::Left) {
        switchPane(-1);
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
    params.selected_item = (m_focusPane == PaneActions)
                             ? m_selectedAction
                             : (5 + m_selectedSpeaker);
    params.active_channel = surround ? surround->getCurrentChannel() : -1;
    params.speaker_count = 8;

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
        params.speakers[i].item_idx = 5 + i;
        params.speakers[i].hidden = (is51 && i >= 6) ? 1 : 0;
        params.speakers[i].hz = (i == 3) ? 80.0 : 440.0;
    }

    evo_rmlui_update_surround(&params);
    evo_rmlui_render_surround(framebuffer, width, height);
}

} // namespace evo
