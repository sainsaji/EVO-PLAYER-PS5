#include "evo/screens/StatefulScreen.hpp"
#include "evo_boot_log.h"

namespace evo {

StatefulScreen::StatefulScreen(const char* name)
    : m_lifecycleFsm(ScreenLifecycleState::Uninitialized, name)
{
    m_lifecycleFsm
        .addState(ScreenLifecycleState::Uninitialized, "Uninitialized")
        .addState(ScreenLifecycleState::Entering, "Entering")
        .addState(ScreenLifecycleState::Active, "Active")
        .addState(ScreenLifecycleState::Suspended, "Suspended")
        .addState(ScreenLifecycleState::Exiting, "Exiting");

    // Transitions
    m_lifecycleFsm
        .addTransition(ScreenLifecycleState::Uninitialized, ScreenLifecycleEvent::Enter, ScreenLifecycleState::Entering)
        .addTransition(ScreenLifecycleState::Entering, ScreenLifecycleEvent::Activate, ScreenLifecycleState::Active)
        .addTransition(ScreenLifecycleState::Active, ScreenLifecycleEvent::Exit, ScreenLifecycleState::Exiting)
        .addTransition(ScreenLifecycleState::Exiting, ScreenLifecycleEvent::Suspend, ScreenLifecycleState::Suspended)
        .addTransition(ScreenLifecycleState::Suspended, ScreenLifecycleEvent::Resume, ScreenLifecycleState::Active)
        .addTransition(ScreenLifecycleState::Suspended, ScreenLifecycleEvent::Enter, ScreenLifecycleState::Entering);
}

void StatefulScreen::onEnter() {
    if (m_lifecycleFsm.getCurrentState() == ScreenLifecycleState::Suspended) {
        m_lifecycleFsm.postEvent(ScreenLifecycleEvent::Resume);
    } else {
        m_lifecycleFsm.postEvent(ScreenLifecycleEvent::Enter);
        m_lifecycleFsm.postEvent(ScreenLifecycleEvent::Activate);
    }
    onEntered();
}

void StatefulScreen::onExit() {
    m_lifecycleFsm.postEvent(ScreenLifecycleEvent::Exit);
    m_lifecycleFsm.postEvent(ScreenLifecycleEvent::Suspend);
    onExited();
}

void StatefulScreen::update(double deltaMs) {
    m_lifecycleFsm.update(deltaMs);
}

} // namespace evo
