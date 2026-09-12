#ifndef EVO_BROWSER_SCREEN_HPP
#define EVO_BROWSER_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include "evo/fsm/StateMachine.hpp"
#include "evo/interfaces/IMediaMetadataService.hpp"
#include <string>

namespace evo {

/**
 * @brief Discrete functional states for storage browsing and media inspection.
 */
enum class BrowserScreenState : int {
    Browsing = 0,
    ProbingMedia = 1,
    Searching = 2
};

/**
 * @brief Discrete trigger events for the browser state machine.
 */
enum class BrowserScreenEvent : int {
    CursorMoved = 0,
    SettleTriggered = 1,
    StartSearch = 2,
    FinishSearch = 3
};

class BrowserScreen : public StatefulScreen {
public:
    BrowserScreen();
    ~BrowserScreen() override = default;

    // --- IStatefulFeature ---
    IStateMachine* getStateMachine() override { return &m_browserFsm; }
    const IStateMachine* getStateMachine() const override { return &m_browserFsm; }

    BrowserScreenState getBrowserState() const {
        return m_browserFsm.getCurrentState();
    }

    ScreenId getScreenId() const override { return ScreenId::UsbBrowser; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    void initBrowserStateMachine();
    void navigate(int delta);
    void jumpPage(int direction);
    void jumpLetter(int direction);
    void activateSelection();
    void openSearch();

    StateMachine<BrowserScreenState, BrowserScreenEvent> m_browserFsm;

    int m_selectedIndex = 0;
    int m_scrollOffset = 0;
    double m_settleMs = 0.0;
    MediaMetadataInfo m_cachedMetadata;
    char m_rowDetails[16][64];
    char m_insName[128];
    char m_insKind[32];
    char m_insExt[16];
    char m_insSize[32];
    char m_insDuration[32];
    char m_insResolution[32];
    char m_insVCodec[32];
    char m_insACodec[32];
    char m_insSubs[16];
    char m_insContainer[32];
    struct InsProp {
        const char* key;
        const char* value;
    } m_insProps[10];
};

} // namespace evo

#endif // EVO_BROWSER_SCREEN_HPP
