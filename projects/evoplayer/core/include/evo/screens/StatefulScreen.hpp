#ifndef EVO_STATEFUL_SCREEN_HPP
#define EVO_STATEFUL_SCREEN_HPP

#include "evo/interfaces/IScreen.hpp"
#include "evo/fsm/StateMachine.hpp"

namespace evo {

/**
 * @brief Discrete lifecycle states for all UI screens in EVO Player.
 */
enum class ScreenLifecycleState : int {
    Uninitialized = 0,
    Entering = 1,
    Active = 2,
    Suspended = 3,
    Exiting = 4
};

/**
 * @brief Discrete lifecycle triggers for UI screens.
 */
enum class ScreenLifecycleEvent : int {
    Initialize = 0,
    Enter = 1,
    Activate = 2,
    Suspend = 3,
    Resume = 4,
    Exit = 5
};

/**
 * @brief Reusable base class implementing the state-machine contract for UI screens.
 *
 * Future features and screens can inherit from StatefulScreen to automatically receive
 * formal state-machine lifecycle handling, or override getStateMachine() to supply a
 * specialized domain state machine.
 */
class StatefulScreen : public IScreen {
public:
    explicit StatefulScreen(const char* name = "StatefulScreen");
    ~StatefulScreen() override = default;

    // --- IStatefulFeature ---
    IStateMachine* getStateMachine() override { return &m_lifecycleFsm; }
    const IStateMachine* getStateMachine() const override { return &m_lifecycleFsm; }

    // --- IScreen Lifecycle Defaults ---
    void onEnter() override;
    void onExit() override;
    void update(double deltaMs) override;

    ScreenLifecycleState getLifecycleState() const {
        return m_lifecycleFsm.getCurrentState();
    }

    bool isScreenActive() const {
        return m_lifecycleFsm.getCurrentState() == ScreenLifecycleState::Active;
    }

protected:
    virtual void onEntered() {}
    virtual void onExited() {}

    StateMachine<ScreenLifecycleState, ScreenLifecycleEvent> m_lifecycleFsm;
};

} // namespace evo

#endif // EVO_STATEFUL_SCREEN_HPP
