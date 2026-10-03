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
    Grid = 1,
    Filter = 2
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
    void handleAddFtpHost(const std::string& hostStr);
    void handleActionRename(const std::string& newName);
    void handleActionNewFolder(const std::string& folderName);

private:
    void initBrowserStateMachine();
    void rebuildItems();
    void navigate(int delta);
    void jumpPage(int direction);
    void jumpLetter(int direction);
    void activateSelection();
    void activateSidebar(bool focusGrid = true);
    void applyFolderFilter();
    void applyFilterChange();
    std::string filterScopeKey() const;
    void openSearch();
    void openActionMenu();
    void performAction(int actionIndex);

    StateMachine<BrowserScreenState, BrowserScreenEvent> m_browserFsm;

    BrowserFocusPane m_focusPane = BrowserFocusPane::Grid;
    int m_sidebarIndex = 0;
    int m_activeSource = 0; // 0: USB Drive, 1: Internal, 2: Favorites, 3: Recent Media
    /*
     * Folder filter. m_folderCats holds the categories that actually have
     * files in the folder in view, so a chip can never produce an empty grid,
     * and it is left empty when there is nothing to choose between - one kind
     * of file, or none - which hides the row entirely. m_filterIndex is 0 for
     * "All" and otherwise indexes m_folderCats; m_filterScope records which
     * folder that choice belongs to, so walking into another one resets it.
     */
    std::vector<int> m_folderCats;
    int m_filterIndex = 0;
    std::string m_filterScope;
    int m_categoryFilter = -1; // derived from m_filterIndex; -1 = All
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

    std::string m_networkShareId;
    std::string m_networkRemotePath = "/";
    std::string m_networkEmptyTitle;
    std::string m_networkEmptyHint;

    // File operations & Action menu
    bool m_actionMenuOpen = false;
    int m_actionMenuFocused = 0;
    bool m_showDeleteConfirm = false;
    int m_deleteConfirmButton = 0;
    bool m_showTransferDialog = false;
    std::string m_actionTargetName;
    std::string m_actionTargetPath;
    bool m_actionTargetIsDir = false;
    char m_transferTitleBuf[64];
    char m_transferDetailBuf[256];
    char m_actionTargetSubBuf[64];
    char m_actionClipboardBuf[128];
    char m_transferSpeedBuf[32];
    char m_transferBytesBuf[64];
    char m_transferEtaBuf[64];
    char m_transferPercentBuf[16];
    char m_transferSrcBuf[256];
    char m_transferDstBuf[256];
};

} // namespace evo

#endif // EVO_BROWSER_SCREEN_HPP
