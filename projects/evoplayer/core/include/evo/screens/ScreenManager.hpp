#ifndef EVO_SCREEN_MANAGER_HPP
#define EVO_SCREEN_MANAGER_HPP

#include "evo/Common.hpp"
#include "evo/interfaces/IScreen.hpp"
#include "evo/interfaces/IStatefulFeature.hpp"
#include "evo/fsm/StateMachine.hpp"
#include <unordered_map>
#include <vector>
#include <memory>

namespace evo {

/**
 * @brief Discrete navigation states for the top-level application router.
 */
enum class NavigationState : int {
    ScreenActive = 0,
    RailFocused = 1,
    ModalActive = 2
};

/**
 * @brief Discrete navigation trigger events.
 */
enum class NavigationEvent : int {
    FocusRail = 0,
    UnfocusRail = 1,
    NavigateScreen = 2,
    OpenModal = 3,
    CloseModal = 4
};

/**
 * @brief Manages screen transitions, navigation stack, modal dialogs and routing.
 *
 * Implements IStatefulFeature to drive navigation state transitions cleanly.
 */
class ScreenManager : public IStatefulFeature {
public:
    ScreenManager();
    ~ScreenManager() override = default;

    // --- IStatefulFeature ---
    IStateMachine* getStateMachine() override { return &m_navFsm; }
    const IStateMachine* getStateMachine() const override { return &m_navFsm; }

    NavigationState getNavigationState() const {
        return m_navFsm.getCurrentState();
    }

    void registerScreen(std::unique_ptr<IScreen> screen);
    void navigateTo(ScreenId screenId);
    bool navigateBack();

    /**
     * @brief The screen the player was launched from.
     *
     * Walks the history back past the player and the screens that live on top
     * of it (exit confirm, resume prompt, media info, subtitle picker) to
     * whatever screen actually started playback, so stopping a video returns
     * there instead of a hardcoded destination. Falls back to MainMenu.
     */
    ScreenId getPlaybackReturnScreen() const;

    ScreenId getCurrentScreenId() const { return m_currentScreenId; }
    IScreen* getCurrentScreen() const;
    IScreen* getScreen(ScreenId screenId) const;

    void handleInput(uint32_t pressed, uint32_t held, uint32_t released);
    void update(double deltaMs);
    void render(uint32_t* framebuffer, int width, int height);

    void setModalDialogActive(bool active);
    bool isModalDialogActive() const { return m_modalActive; }

    // Nav rail (Side navigation bar)
    bool isRailFocused() const { return m_railFocused; }
    void setRailFocused(bool focused);
    int getRailIndex() const { return m_railIndex; }
    void setRailIndex(int index) { m_railIndex = index; }
    void stepRail(int delta);
    void activateRail();

    bool isNavRailVisible() const;
    int getSectionForScreen(ScreenId screenId) const;
    void syncNavRail();

private:
    void initStateMachine();

    StateMachine<NavigationState, NavigationEvent> m_navFsm;
    std::unordered_map<int, std::unique_ptr<IScreen>> m_screens;
    std::vector<ScreenId> m_history;
    ScreenId m_currentScreenId = static_cast<ScreenId>(-1);
    bool m_modalActive = false;

    bool m_railFocused = false;
    int m_railIndex = 0;
};

} // namespace evo

#endif // EVO_SCREEN_MANAGER_HPP
