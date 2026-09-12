#ifndef EVO_PLAYER_SCREEN_HPP
#define EVO_PLAYER_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include "evo/fsm/StateMachine.hpp"

namespace evo {

/**
 * @brief Discrete UI states for the media player presentation layer.
 */
enum class PlayerScreenState : int {
    NormalPlayback = 0,
    OsdVisible = 1,
    Scrubbing = 2,
    StatsOverlay = 3
};

/**
 * @brief Discrete trigger events for the player presenter state machine.
 */
enum class PlayerScreenEvent : int {
    ShowOsd = 0,
    HideOsd = 1,
    StartScrub = 2,
    EndScrub = 3,
    ToggleStats = 4
};

class PlayerScreen : public StatefulScreen {
public:
    PlayerScreen();
    ~PlayerScreen() override = default;

    // --- IStatefulFeature ---
    IStateMachine* getStateMachine() override { return &m_playerFsm; }
    const IStateMachine* getStateMachine() const override { return &m_playerFsm; }

    PlayerScreenState getPlayerState() const {
        return m_playerFsm.getCurrentState();
    }

    ScreenId getScreenId() const override { return ScreenId::Player; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

    bool isStatsForNerdsVisible() const { return m_showStatsForNerds; }
    void toggleStatsForNerds();

private:
    void initPlayerStateMachine();
    void feedPerformanceHud();

    StateMachine<PlayerScreenState, PlayerScreenEvent> m_playerFsm;
    int m_osdVisibilityAlpha = 255;
    uint64_t m_controlsLastUsedMs = 0;
    bool m_showStatsForNerds = false;
};

} // namespace evo

#endif // EVO_PLAYER_SCREEN_HPP
