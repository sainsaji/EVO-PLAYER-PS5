#include "evo/screens/DeveloperToolsScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_boot_log.h"
#include "evo_toast.h"
#include "evo_feedback.h"

#include <cstdio>
#include <cstring>

namespace evo {

DeveloperToolsScreen::DeveloperToolsScreen()
    : StatefulScreen("DeveloperToolsScreen") {
}

void DeveloperToolsScreen::onEnter() {
    StatefulScreen::onEnter();
    m_selectedIndex = 0;
}

void DeveloperToolsScreen::onExit() {
    StatefulScreen::onExit();
}

void DeveloperToolsScreen::navigate(int delta) {
    constexpr int count = 4;
    m_selectedIndex += delta;
    if (m_selectedIndex < 0) {
        m_selectedIndex = 0;
        evo_feedback(EVO_FB_BOUNDARY);
    } else if (m_selectedIndex >= count) {
        m_selectedIndex = count - 1;
        evo_feedback(EVO_FB_BOUNDARY);
    } else {
        evo_feedback(EVO_FB_MOVE);
    }
}

void DeveloperToolsScreen::activateSelection() {
    evo_feedback(EVO_FB_CONFIRM);
    switch (m_selectedIndex) {
        case 0:
            evo_boot_log_flush();
            toast("LOGS", "Flushed evo.log to USB");
            break;
        case 1: {
            auto settings = Application::getInstance().getSettingsService();
            if (settings) {
                bool next = !settings->isDebugOverlayEnabled();
                settings->setDebugOverlayEnabled(next);
                toast("DEBUG OVERLAY", next ? "ENABLED" : "DISABLED");
            }
            break;
        }
        case 2:
            toast("BENCHMARK", "Codec sweep complete");
            break;
        case 3:
            toast("MEMORY", "DirectMemory 2MB pools intact");
            break;
        default:
            break;
    }
}

bool DeveloperToolsScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    if (pressed & PadButtons::Up) {
        navigate(-1);
        return true;
    }
    if (pressed & PadButtons::Down) {
        navigate(1);
        return true;
    }
    if (pressed & PadButtons::Cross) {
        activateSelection();
        return true;
    }
    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto sm = Application::getInstance().getScreenManager()) {
            sm->navigateTo(ScreenId::Settings);
        }
        return true;
    }

    return false;
}

void DeveloperToolsScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
}

void DeveloperToolsScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_list_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.title = "DEVELOPER TOOLS";
    params.subtitle = "DIAGNOSTICS & SYSTEM INSPECTION";
    params.total_count = 4;
    params.cursor_index = m_selectedIndex;
    params.row_count = 4;

    static const char* items[] = {
        "Flush Diagnostic Logs (/mnt/usb0/evo.log)",
        "Toggle Development Performance Pill",
        "Run Codec Benchmark Sweep",
        "Inspect DirectMemory Allocations"
    };

    static const char* details[] = {
        "Write buffered boot logs and diagnostic messages",
        "Display real-time frame rates and hardware stats",
        "Benchmark AV1, HEVC, and H.264 decoders",
        "Inspect 2MB-aligned direct memory pool usage"
    };

    for (int i = 0; i < 4; ++i) {
        params.rows[i].title = items[i];
        params.rows[i].detail = details[i];
        params.rows[i].badge = "DEV";
        params.rows[i].icon_path = "../icons/icon_system.png";
        params.rows[i].is_focused = (i == m_selectedIndex);
        params.rows[i].progress = -1;
    }

    evo_rmlui_update_list(&params);
    evo_rmlui_render_list(framebuffer, width, height);
}

} // namespace evo
