#include "evo/screens/ModalDialogScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"

#include <cstdio>
#include <cstring>

namespace evo {

ModalDialogScreen::ModalDialogScreen(ModalType type)
    : StatefulScreen("ModalDialogScreen")
    , m_type(type)
    , m_modalFsm(ModalDialogState::Inactive, "ModalDialogFSM")
{
    initModalStateMachine();
}

void ModalDialogScreen::initModalStateMachine() {
    m_modalFsm
        .addState(ModalDialogState::Inactive, "Inactive")
        .addState(ModalDialogState::Prompting, "Prompting")
        .addState(ModalDialogState::Confirmed, "Confirmed")
        .addState(ModalDialogState::Cancelled, "Cancelled");

    m_modalFsm
        .addTransition(ModalDialogState::Inactive, ModalDialogEvent::Open, ModalDialogState::Prompting)
        .addTransition(ModalDialogState::Prompting, ModalDialogEvent::Confirm, ModalDialogState::Confirmed)
        .addTransition(ModalDialogState::Prompting, ModalDialogEvent::Cancel, ModalDialogState::Cancelled)
        .addTransition(ModalDialogState::Confirmed, ModalDialogEvent::Dismiss, ModalDialogState::Inactive)
        .addTransition(ModalDialogState::Cancelled, ModalDialogEvent::Dismiss, ModalDialogState::Inactive)
        .addTransition(ModalDialogState::Prompting, ModalDialogEvent::Dismiss, ModalDialogState::Inactive);
}

ScreenId ModalDialogScreen::getScreenId() const {
    return (m_type == ModalType::ExitConfirm) ? ScreenId::ExitConfirm : ScreenId::ResumePrompt;
}

void ModalDialogScreen::onEnter() {
    StatefulScreen::onEnter();
    m_modalFsm.postEvent(ModalDialogEvent::Open);
    m_focusedButton = (m_type == ModalType::ExitConfirm) ? 1 : 0; // Default STOP (1) or RESUME (0)
}

void ModalDialogScreen::onExit() {
    StatefulScreen::onExit();
    m_modalFsm.postEvent(ModalDialogEvent::Dismiss);
}

void ModalDialogScreen::executeAction(int actionIndex) {
    auto playback = Application::getInstance().getPlaybackController();
    auto screenMgr = Application::getInstance().getScreenManager();
    if (!screenMgr) return;

    if (m_type == ModalType::ExitConfirm) {
        if (actionIndex == 0) {
            // "KEEP WATCHING"
            evo_feedback(EVO_FB_CANCEL);
            screenMgr->navigateTo(ScreenId::Player);
        } else {
            // "STOP"
            evo_feedback(EVO_FB_CONFIRM);
            if (playback) {
                playback->stopPlayback();
            }
            screenMgr->navigateTo(ScreenId::UsbBrowser);
        }
    } else {
        // Resume prompt
        if (actionIndex == 0) {
            // "RESUME"
            evo_feedback(EVO_FB_CONFIRM);
            if (playback && !m_resumePath.empty()) {
                playback->startPlayback(m_resumePath, m_resumeTimestamp);
            }
            screenMgr->navigateTo(ScreenId::Player);
        } else {
            // "START OVER"
            evo_feedback(EVO_FB_CONFIRM);
            if (playback && !m_resumePath.empty()) {
                playback->startPlayback(m_resumePath, 0.0);
            }
            screenMgr->navigateTo(ScreenId::Player);
        }
    }
}

bool ModalDialogScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    if (pressed & (PadButtons::Left | PadButtons::Right | PadButtons::Up | PadButtons::Down)) {
        m_focusedButton = (m_focusedButton == 0) ? 1 : 0;
        evo_feedback(EVO_FB_MOVE);
        return true;
    }

    if (pressed & PadButtons::Cross) {
        executeAction(m_focusedButton);
        return true;
    }

    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto sm = Application::getInstance().getScreenManager()) {
            if (m_type == ModalType::ExitConfirm) {
                sm->navigateTo(ScreenId::Player);
            } else {
                sm->navigateTo(ScreenId::UsbBrowser);
            }
        }
        return true;
    }

    return false;
}

void ModalDialogScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
    m_modalFsm.update(deltaMs);
}

void ModalDialogScreen::render(uint32_t* framebuffer, int width, int height) {
    if (!evo_rmlui_is_initialized()) return;

    evo_rmlui_dialog_params_t dialog;
    std::memset(&dialog, 0, sizeof(dialog));

    if (m_type == ModalType::ExitConfirm) {
        dialog.eyebrow = "PLAYBACK";
        dialog.title = "Stop playback?";
        dialog.detail = "Your current position will be saved automatically.";
        dialog.progress_pct = -1.0;
        dialog.action_count = 2;
        dialog.focused_action = m_focusedButton;

        dialog.actions[0].label = "KEEP WATCHING";
        dialog.actions[0].icon_path = "/assets/icons/btn_circle.png";
        dialog.actions[0].is_primary = 0;

        dialog.actions[1].label = "STOP";
        dialog.actions[1].icon_path = "/assets/icons/btn_cross.png";
        dialog.actions[1].is_primary = 1;
        dialog.eyebrow = "RESUME";
        dialog.title = "Continue from previous position?";

        static char resumeDetailBuf[128];
        int pos = static_cast<int>(m_resumeTimestamp);
        int ph = pos / 3600, pm = (pos % 3600) / 60, ps = pos % 60;
        int dur = static_cast<int>(m_resumeDuration);
        int dh = dur / 3600, dm = (dur % 3600) / 60, ds = dur % 60;
        if (dh > 0) {
            std::snprintf(resumeDetailBuf, sizeof(resumeDetailBuf), "STOPPED AT %02d:%02d:%02d OF %02d:%02d:%02d",
                          ph, pm, ps, dh, dm, ds);
        } else if (dur > 0) {
            std::snprintf(resumeDetailBuf, sizeof(resumeDetailBuf), "STOPPED AT %02d:%02d OF %02d:%02d",
                          pm, ps, dm, ds);
        } else {
            std::snprintf(resumeDetailBuf, sizeof(resumeDetailBuf), "STOPPED AT %02d:%02d", pm, ps);
        }
        dialog.detail = resumeDetailBuf;

        if (m_resumeDuration > 0.0) {
            dialog.progress_pct = (m_resumeTimestamp / m_resumeDuration) * 100.0;
        } else {
            dialog.progress_pct = -1.0;
        }

        dialog.action_count = 2;
        dialog.focused_action = m_focusedButton;

        dialog.actions[0].label = "RESUME";
        dialog.actions[0].icon_path = "/assets/icons/btn_cross.png";
        dialog.actions[0].is_primary = 1;

        dialog.actions[1].label = "START OVER";
        dialog.actions[1].icon_path = "/assets/icons/btn_circle.png";
        dialog.actions[1].is_primary = 0;
    }

    evo_rmlui_update_dialog(&dialog);
    evo_rmlui_render_dialog(framebuffer, width, height);
}

} // namespace evo
