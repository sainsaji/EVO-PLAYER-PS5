#ifndef EVO_LAUNCH_SCREEN_HPP
#define EVO_LAUNCH_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include "evo/fsm/StateMachine.hpp"
#include <string>

namespace evo {

/**
 * @brief Discrete navigation states on the Home/Launch screen.
 */
enum class LaunchScreenState : int {
    HeroFocused = 0,
    RecentShelf = 1,
    LibraryShelf = 2
};

/**
 * @brief Discrete triggers for Home/Launch navigation transitions.
 */
enum class LaunchScreenEvent : int {
    FocusHero = 0,
    FocusRecent = 1,
    FocusLibrary = 2
};

class LaunchScreen : public StatefulScreen {
public:
    LaunchScreen();
    ~LaunchScreen() override = default;

    // --- IStatefulFeature ---
    IStateMachine* getStateMachine() override { return &m_launchFsm; }
    const IStateMachine* getStateMachine() const override { return &m_launchFsm; }

    LaunchScreenState getLaunchState() const {
        return m_launchFsm.getCurrentState();
    }

    ScreenId getScreenId() const override { return ScreenId::MainMenu; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    void initLaunchStateMachine();
    void navigate(int dx, int dy);
    void activateSelection();

    StateMachine<LaunchScreenState, LaunchScreenEvent> m_launchFsm;
    int m_selectedRow = 0;
    int m_selectedCol = 0;
};

} // namespace evo

#endif // EVO_LAUNCH_SCREEN_HPP
