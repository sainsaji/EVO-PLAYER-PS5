#include "evo_features.h"
#include "evo/screens/ScreenManager.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"

extern "C" {
extern int screen;
void evo_sync_rmlui_nav(int section, int rail_focused, int rail_index, int visible);
}

namespace evo {

ScreenManager::ScreenManager()
    : m_navFsm(NavigationState::ScreenActive, "NavigationFSM")
{
    initStateMachine();
}

void ScreenManager::initStateMachine() {
    m_navFsm
        .addState(NavigationState::ScreenActive, "ScreenActive")
        .addState(NavigationState::RailFocused, "RailFocused")
        .addState(NavigationState::ModalActive, "ModalActive");

    m_navFsm
        .addTransition(NavigationState::ScreenActive, NavigationEvent::FocusRail, NavigationState::RailFocused)
        .addTransition(NavigationState::ScreenActive, NavigationEvent::NavigateScreen, NavigationState::ScreenActive)
        .addTransition(NavigationState::ScreenActive, NavigationEvent::OpenModal, NavigationState::ModalActive)
        .addTransition(NavigationState::RailFocused, NavigationEvent::UnfocusRail, NavigationState::ScreenActive)
        .addTransition(NavigationState::RailFocused, NavigationEvent::NavigateScreen, NavigationState::ScreenActive)
        .addTransition(NavigationState::ModalActive, NavigationEvent::CloseModal, NavigationState::ScreenActive)
        .addTransition(NavigationState::ModalActive, NavigationEvent::NavigateScreen, NavigationState::ScreenActive);
}

void ScreenManager::setModalDialogActive(bool active) {
    m_modalActive = active;
    if (active) {
        m_navFsm.postEvent(NavigationEvent::OpenModal);
    } else {
        m_navFsm.postEvent(NavigationEvent::CloseModal);
    }
}

void ScreenManager::registerScreen(std::unique_ptr<IScreen> screenPtr) {
    if (screenPtr) {
        int id = static_cast<int>(screenPtr->getScreenId());
        m_screens[id] = std::move(screenPtr);
    }
}

IScreen* ScreenManager::getCurrentScreen() const {
    auto it = m_screens.find(static_cast<int>(m_currentScreenId));
    if (it != m_screens.end()) {
        return it->second.get();
    }
    return nullptr;
}

IScreen* ScreenManager::getScreen(ScreenId screenId) const {
    auto it = m_screens.find(static_cast<int>(screenId));
    if (it != m_screens.end()) {
        return it->second.get();
    }
    return nullptr;
}

int ScreenManager::getSectionForScreen(ScreenId screenId) const {
    switch (screenId) {
        case ScreenId::MainMenu:        return 0; // Home
        case ScreenId::UsbBrowser:      return 1; // Browse USB
        case ScreenId::RecentFiles:     return 1; // Recent Files (part of file browser)
        case ScreenId::Favorites:       return 1; // Favorites (part of file browser)
#if EVO_ENABLE_EMBY
        case ScreenId::EmbySetup:
        case ScreenId::EmbyBrowse:      return 2; // Emby
#endif
        case ScreenId::Settings:
        case ScreenId::SettingsPlayback:
        case ScreenId::SettingsSubtitles:
        case ScreenId::SettingsInterface:
        case ScreenId::SettingsSystem:  return EVO_ENABLE_EMBY ? 3 : 2; // Settings
        case ScreenId::AboutSupport:
        case ScreenId::Changelog:       return EVO_ENABLE_EMBY ? 4 : 3; // About & Support
        default:                        return -1; // Non-rail screens
    }
}

bool ScreenManager::isNavRailVisible() const {
    return getSectionForScreen(m_currentScreenId) >= 0;
}

void ScreenManager::setRailFocused(bool focused) {
    if (!isNavRailVisible() && focused) return;
    if (m_railFocused == focused) return;

    m_railFocused = focused;
    if (m_railFocused) {
        m_navFsm.postEvent(NavigationEvent::FocusRail);
        int sec = getSectionForScreen(m_currentScreenId);
        m_railIndex = (sec >= 0) ? sec : 0;
        evo_feedback(EVO_FB_OPEN);
    } else {
        m_navFsm.postEvent(NavigationEvent::UnfocusRail);
        evo_feedback(EVO_FB_CANCEL);
    }
    syncNavRail();
}

void ScreenManager::stepRail(int delta) {
    /* One fewer section while Emby is disabled, so the rail does not step onto
     * an entry with nothing behind it. See EVO_ENABLE_EMBY in evo_features.h. */
    const int numSections = EVO_ENABLE_EMBY ? 5 : 4;
    m_railIndex = (m_railIndex + delta) % numSections;
    if (m_railIndex < 0) m_railIndex += numSections;
    evo_feedback(EVO_FB_MOVE);
    syncNavRail();
}

ScreenId ScreenManager::getRootScreenForSection(int section) const {
    /* Must stay the inverse of getSectionForScreen() above, including the
     * shift when Emby is compiled out. */
#if EVO_ENABLE_EMBY
    switch (section) {
        case 0: return ScreenId::MainMenu;
        case 1: return ScreenId::UsbBrowser;
        case 2: return ScreenId::EmbySetup;
        case 3: return ScreenId::Settings;
        case 4: return ScreenId::AboutSupport;
        default: return ScreenId::MainMenu;
    }
#else
    switch (section) {
        case 0: return ScreenId::MainMenu;
        case 1: return ScreenId::UsbBrowser;
        case 2: return ScreenId::Settings;
        case 3: return ScreenId::AboutSupport;
        default: return ScreenId::MainMenu;
    }
#endif
}

bool ScreenManager::isPlaybackScreen(ScreenId screenId) const {
    switch (screenId) {
        case ScreenId::Player:
        case ScreenId::ExitConfirm:
        case ScreenId::ResumePrompt:
        case ScreenId::MediaInfo:
        case ScreenId::SubtitlePicker:
        case ScreenId::AudioTrackPicker:
        case ScreenId::PlaybackFinished:
            return true;
        default:
            return false;
    }
}

void ScreenManager::activateRail() {
    int targetSection = m_railIndex;
    m_railFocused = false;

    ScreenId targetRoot = getRootScreenForSection(targetSection);
    if (m_currentScreenId == targetRoot) {
        evo_feedback(EVO_FB_CANCEL);
        syncNavRail();
        return;
    }

    evo_feedback(EVO_FB_OPEN);
    navigateTo(targetRoot);
}

void ScreenManager::navigateTo(ScreenId screenId) {
    if (m_currentScreenId == screenId) {
        m_railFocused = false;
        syncNavRail();
        return;
    }

    if (auto current = getCurrentScreen()) {
        current->onExit();
    }

    m_history.push_back(screenId);
    m_currentScreenId = screenId;
    m_navFsm.postEvent(NavigationEvent::NavigateScreen);
    screen = static_cast<int>(screenId); // Sync global for legacy/RmlUi hooks

    m_railFocused = false;
    int sec = getSectionForScreen(screenId);
    if (sec >= 0) m_railIndex = sec;

    if (auto next = getCurrentScreen()) {
        next->onEnter();
    }

    syncNavRail();
}

bool ScreenManager::navigateBack() {
    if (m_history.size() <= 1) {
        return false;
    }

    if (auto current = getCurrentScreen()) {
        current->onExit();
    }

    m_history.pop_back();
    m_currentScreenId = m_history.back();
    screen = static_cast<int>(m_currentScreenId);

    m_railFocused = false;
    int sec = getSectionForScreen(m_currentScreenId);
    if (sec >= 0) m_railIndex = sec;

    if (auto next = getCurrentScreen()) {
        next->onEnter();
    }

    syncNavRail();
    return true;
}

bool ScreenManager::navigateBack(ScreenId fallbackScreen) {
    if (navigateBack()) {
        return true;
    }
    navigateTo(fallbackScreen);
    return false;
}

ScreenId ScreenManager::getPlaybackReturnScreen() const {
    for (auto it = m_history.rbegin(); it != m_history.rend(); ++it) {
        if (!isPlaybackScreen(*it)) {
            return *it;
        }
    }
    return ScreenId::MainMenu;
}

void ScreenManager::returnFromPlayback() {
    ScreenId target = getPlaybackReturnScreen();

    if (auto current = getCurrentScreen()) {
        current->onExit();
    }

    // Prune all playback session screens from the history stack
    while (!m_history.empty() && isPlaybackScreen(m_history.back())) {
        m_history.pop_back();
    }

    // Ensure the destination screen is at the top of history
    if (m_history.empty() || m_history.back() != target) {
        m_history.push_back(target);
    }

    m_currentScreenId = target;
    m_navFsm.postEvent(NavigationEvent::NavigateScreen);
    screen = static_cast<int>(target);

    m_railFocused = false;
    int sec = getSectionForScreen(target);
    if (sec >= 0) m_railIndex = sec;

    if (auto next = getCurrentScreen()) {
        next->onEnter();
    }

    syncNavRail();
}

void ScreenManager::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    if (m_railFocused) {
        if (pressed & PadButtons::Up) {
            stepRail(-1);
            return;
        }
        if (pressed & PadButtons::Down) {
            stepRail(1);
            return;
        }
        if (pressed & (PadButtons::Right | PadButtons::Circle)) {
            setRailFocused(false);
            return;
        }
        if (pressed & PadButtons::Cross) {
            activateRail();
            return;
        }
        return; // Absorb all other input while rail is focused
    }

    bool handled = false;
    if (auto current = getCurrentScreen()) {
        handled = current->handleInput(pressed, held, released);
    } else {
        if (pressed & PadButtons::Circle) {
            if (!navigateBack()) {
                navigateTo(ScreenId::MainMenu);
            }
            return;
        }
    }

    if (!handled && (pressed & PadButtons::Left) && isNavRailVisible()) {
        setRailFocused(true);
    }
}

void ScreenManager::update(double deltaMs) {
    m_navFsm.update(deltaMs);
    if (auto current = getCurrentScreen()) {
        current->update(deltaMs);
    }
}

void ScreenManager::render(uint32_t* framebuffer, int width, int height) {
    syncNavRail();
    if (auto current = getCurrentScreen()) {
        current->render(framebuffer, width, height);
    }
}

void ScreenManager::syncNavRail() {
    int section = getSectionForScreen(m_currentScreenId);
    bool visible = (section >= 0);
    if (!visible) section = 0;

    evo_sync_rmlui_nav(section, m_railFocused ? 1 : 0, m_railIndex, visible ? 1 : 0);
}

} // namespace evo
