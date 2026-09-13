#ifndef EVO_BROWSER_SCREEN_HPP
#define EVO_BROWSER_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include "evo/fsm/StateMachine.hpp"
#include "evo/interfaces/IMediaMetadataService.hpp"
#include <string>
#include <vector>

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

enum class BrowserFocusPane : int {
    Sidebar = 0,
    Grid = 1
};

struct BrowserItem {
    std::string name;
    std::string fullPath;
    FileCategory category = FileCategory::Unknown;
    int progress = -1;
    double lastPos = 0.0;
    double duration = 0.0;
    bool isFavorite = false;
    std::string detail;
    std::string badge;
    std::string iconPath;
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
    void resetSelection();
    void setSource(int sourceIndex);
    void setSearchQuery(const std::string& query);

private:
    void initBrowserStateMachine();
    void rebuildItems();
    void navigate(int delta);
    void jumpPage(int direction);
    void jumpLetter(int direction);
    void activateSelection();
    void activateSidebar();
    void openSearch();

    StateMachine<BrowserScreenState, BrowserScreenEvent> m_browserFsm;

    BrowserFocusPane m_focusPane = BrowserFocusPane::Grid;
    int m_sidebarIndex = 0;
    int m_activeSource = 0; // 0: USB Drive, 1: Internal, 2: Favorites, 3: Recent Media
    int m_categoryFilter = -1; // -1: All, 1: Video, 2: Audio, 3: Image
    std::vector<BrowserItem> m_items;
    std::string m_searchQuery;
    bool m_isSearching = false;

    int m_selectedIndex = 0;
    int m_scrollOffset = 0;
    double m_settleMs = 0.0;
    MediaMetadataInfo m_cachedMetadata;
    char m_formattedPath[256];
    char m_rowDurations[16][32];
    char m_insName[128];
    char m_insKind[32];
    char m_insExt[16];
    char m_statusRes[64];
    char m_statusVCodec[64];
    char m_statusACodec[64];
    char m_statusDuration[32];
    char m_statusSize[32];
};

} // namespace evo

#endif // EVO_BROWSER_SCREEN_HPP
