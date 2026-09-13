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

void SurroundTestScreen::onEnter() {
    StatefulScreen::onEnter();
    m_selectedSpeaker = 0;
}

void SurroundTestScreen::onExit() {
    StatefulScreen::onExit();
    if (auto surround = Application::getInstance().getSurroundTestService()) {
        surround->stop();
    }
}

void SurroundTestScreen::navigate(int dir) {
    auto surround = Application::getInstance().getSurroundTestService();
    bool is51 = surround ? surround->is51Layout() : false;
    int maxChannels = is51 ? 6 : 8;

    m_selectedSpeaker += dir;
    if (m_selectedSpeaker < 0) {
        m_selectedSpeaker = 0;
        evo_feedback(EVO_FB_BOUNDARY);
    } else if (m_selectedSpeaker >= maxChannels) {
        m_selectedSpeaker = maxChannels - 1;
        evo_feedback(EVO_FB_BOUNDARY);
    } else {
        evo_feedback(EVO_FB_MOVE);
    }
}

void SurroundTestScreen::playSelectedChannel() {
    auto surround = Application::getInstance().getSurroundTestService();
    if (surround) {
        evo_feedback(EVO_FB_CONFIRM);
        surround->triggerTone(surround->is51Layout(), m_selectedSpeaker);
    }
}

bool SurroundTestScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    auto surround = Application::getInstance().getSurroundTestService();

    if (pressed & (PadButtons::Up | PadButtons::Left)) {
        navigate(-1);
        return true;
    }
    if (pressed & (PadButtons::Down | PadButtons::Right)) {
        navigate(1);
        return true;
    }
    if (pressed & PadButtons::Cross) {
        playSelectedChannel();
        return true;
    }
    if (pressed & PadButtons::Square) {
        if (surround) {
            bool next51 = !surround->is51Layout();
            surround->set51Layout(next51);
            evo_feedback(EVO_FB_TOGGLE);
            toast("SPEAKER LAYOUT", next51 ? "5.1 Surround (6 Channels)" : "7.1 Surround (8 Channels)");
            if (m_selectedSpeaker >= 6 && next51) {
                m_selectedSpeaker = 5;
            }
        }
        return true;
    }
    if (pressed & PadButtons::Triangle) {
        if (surround) {
            surround->stop();
            evo_feedback(EVO_FB_CANCEL);
            toast("SURROUND TEST", "Silenced");
        }
        return true;
    }
    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto sm = Application::getInstance().getScreenManager()) {
            sm->navigateBack(ScreenId::SettingsPlayback);
        }
        return true;
    }

    return false;
}

void SurroundTestScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
}

void SurroundTestScreen::render(uint32_t* framebuffer, int width, int height) {
    auto surround = Application::getInstance().getSurroundTestService();
    bool is51 = surround ? surround->is51Layout() : false;

    evo_rmlui_surround_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.rail_focused = 0;
    params.is_51_layout = is51 ? 1 : 0;
    params.selected_item = 5 + m_selectedSpeaker;
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
