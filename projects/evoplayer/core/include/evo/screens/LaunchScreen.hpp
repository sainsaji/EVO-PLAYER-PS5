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

    /* Path the hero backdrop should show for the current cursor position. */
    std::string heroPathForSelection() const;

    StateMachine<LaunchScreenState, LaunchScreenEvent> m_launchFsm;
    int m_selectedRow = 0;
    int m_selectedCol = 0;

    /*
     * Hero backdrop resolution is debounced. ensureHeroArt() decodes a video
     * frame synchronously on the render thread, and the hero follows the
     * recent-shelf cursor, so resolving it on every D-pad press put a full
     * demux + decode + scale in the input path. The cached poster stands in
     * (same 16:9, just 320x180) until the cursor has been still long enough
     * for the sharp 960x540 extraction to be worth paying for.
     */
    static constexpr double HeroSettleMs = 260.0;
    std::string m_heroSettlePath;
    double m_heroSettleMs = 0.0;
};

} // namespace evo

#endif // EVO_LAUNCH_SCREEN_HPP
