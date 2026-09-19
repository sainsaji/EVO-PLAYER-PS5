#include "evo/screens/BrowserScreen.hpp"
#include "evo/screens/TextReaderScreen.hpp"
#include "evo/screens/ImageViewerScreen.hpp"
#include "evo/screens/ModalDialogScreen.hpp"
#include "evo/animation/AnimationManager.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_keyboard.h"
#include "evo_favorites.h"
#include "evo_recent.h"
#include "evo_feedback.h"
#include "evo_toast.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <sys/stat.h>
#include <sys/time.h>

extern "C" {
#include <libavformat/avformat.h>
}

namespace evo {

static bool matchesQuery(const std::string& str, const std::string& q) {
    if (q.empty()) return true;
    auto it = std::search(
        str.begin(), str.end(),
        q.begin(), q.end(),
        [](char ch1, char ch2) { return std::toupper(static_cast<unsigned char>(ch1)) == std::toupper(static_cast<unsigned char>(ch2)); }
    );
    return it != str.end();
}

static void OnSearchSubmitted(const char* text, void* userdata) {
    auto* self = static_cast<BrowserScreen*>(userdata);
    if (!self) return;

    std::string queryStr;
    if (text) {
        queryStr = text;
        size_t start = queryStr.find_first_not_of(" \t\r\n");
        if (start != std::string::npos) {
            size_t end = queryStr.find_last_not_of(" \t\r\n");
            queryStr = queryStr.substr(start, end - start + 1);
        } else {
            queryStr.clear();
        }
    }

    self->setSearchQuery(queryStr);
}

BrowserScreen::BrowserScreen()
    : StatefulScreen("BrowserScreen")
    , m_browserFsm(BrowserScreenState::Browsing, "BrowserFSM")
{
    initBrowserStateMachine();
}

void BrowserScreen::initBrowserStateMachine() {
    m_browserFsm
        .addState(BrowserScreenState::Browsing, "Browsing")
        .addState(BrowserScreenState::ProbingMedia, "ProbingMedia")
        .addState(BrowserScreenState::Searching, "Searching");

    m_browserFsm
        .addTransition(BrowserScreenState::Browsing, BrowserScreenEvent::SettleTriggered, BrowserScreenState::ProbingMedia)
        .addTransition(BrowserScreenState::Browsing, BrowserScreenEvent::StartSearch, BrowserScreenState::Searching)
        .addTransition(BrowserScreenState::Browsing, BrowserScreenEvent::CursorMoved, BrowserScreenState::Browsing)
        .addTransition(BrowserScreenState::ProbingMedia, BrowserScreenEvent::CursorMoved, BrowserScreenState::Browsing)
        .addTransition(BrowserScreenState::ProbingMedia, BrowserScreenEvent::StartSearch, BrowserScreenState::Searching)
        .addTransition(BrowserScreenState::Searching, BrowserScreenEvent::FinishSearch, BrowserScreenState::Browsing)
        .addTransition(BrowserScreenState::Searching, BrowserScreenEvent::CursorMoved, BrowserScreenState::Browsing);
}

void BrowserScreen::rebuildItems() {
    m_items.clear();
    auto browser = Application::getInstance().getFileSystemBrowser();

    if (m_activeSource == 0 || m_activeSource == 1) {
        if (!browser) return;
        size_t count = browser->getEntryCount();
        for (size_t i = 0; i < count; ++i) {
            const auto* entry = browser->getEntry(i);
            if (!entry) continue;

            if (m_categoryFilter != -1 && entry->category != FileCategory::Folder &&
                static_cast<int>(entry->category) != m_categoryFilter) {
                continue;
            }

            if (m_isSearching && !matchesQuery(entry->name, m_searchQuery)) {
                continue;
            }

            BrowserItem item;
            item.name = entry->name;
            item.fullPath = browser->getFullPath(i);
            item.category = entry->category;
            item.badge = browser->getFileCategoryLabel(entry->category);
            item.isFavorite = favorites_is_favorite(item.fullPath.c_str());

            double pos = recent_lookup(item.fullPath.c_str());
            item.lastPos = pos;
            if (pos > 0.0 && m_cachedMetadata.filePath == item.fullPath && m_cachedMetadata.durationSeconds > 0.0) {
                item.duration = m_cachedMetadata.durationSeconds;
                item.progress = static_cast<int>((pos / m_cachedMetadata.durationSeconds) * 100.0);
                if (item.progress > 100) item.progress = 100;
            }

            if (entry->category == FileCategory::Folder) {
                item.iconPath = (!m_isSearching && browser->getCurrentPath() == "/mnt/usb0")
                    ? "../icons/icon_browse_usb.png"
                    : "../icons/icon_folder.png";
                item.detail = "Folder";
            } else {
                if (entry->category == FileCategory::Video) item.iconPath = "../icons/icon_resume.png";
                else if (entry->category == FileCategory::Audio) item.iconPath = "../icons/icon_subtitles.png";
                else if (entry->category == FileCategory::Image) item.iconPath = "../icons/icon_palette.png";
                else item.iconPath = "../icons/icon_about_support.png";

                struct stat st;
                if (!item.fullPath.empty() && stat(item.fullPath.c_str(), &st) == 0) {
                    double sz = static_cast<double>(st.st_size);
                    const char* unit = "B";
                    if (sz >= 1024.0) { sz /= 1024.0; unit = "KB"; }
                    if (sz >= 1024.0) { sz /= 1024.0; unit = "MB"; }
                    if (sz >= 1024.0) { sz /= 1024.0; unit = "GB"; }
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%.2f %s", sz, unit);
                    item.detail = buf;
                } else {
                    item.detail = "Media File";
                }
            }
            m_items.push_back(std::move(item));
        }
    } else if (m_activeSource == 2) {
        // Favorites
        favorites_load();
        for (int i = 0; i < favorite_count; ++i) {
            const auto& fav = favorite_files[i];
            if (fav.path[0] == '\0') continue;

            FileCategory cat = browser ? browser->classifyFile(fav.path, 0) : FileCategory::Video;
            if (m_categoryFilter != -1 && static_cast<int>(cat) != m_categoryFilter) {
                continue;
            }

            std::string displayName = fav.title[0] ? fav.title : "";
            if (displayName.empty()) {
                std::string p(fav.path);
                size_t slash = p.find_last_of('/');
                displayName = (slash != std::string::npos) ? p.substr(slash + 1) : p;
            }

            if (m_isSearching && !matchesQuery(displayName, m_searchQuery) && !matchesQuery(fav.path, m_searchQuery)) {
                continue;
            }

            BrowserItem item;
            item.name = displayName;
            item.fullPath = fav.path;
            item.category = cat;
            item.badge = browser ? browser->getFileCategoryLabel(cat) : "FAVORITE";
            item.isFavorite = true;
            item.duration = fav.duration;

            double pos = recent_lookup(fav.path);
            item.lastPos = pos;
            if (pos > 0.0 && fav.duration > 0.0) {
                item.progress = static_cast<int>((pos / fav.duration) * 100.0);
                if (item.progress > 100) item.progress = 100;
            }

            if (cat == FileCategory::Video) item.iconPath = "../icons/icon_resume.png";
            else if (cat == FileCategory::Audio) item.iconPath = "../icons/icon_subtitles.png";
            else if (cat == FileCategory::Image) item.iconPath = "../icons/icon_palette.png";
            else item.iconPath = "../icons/icon_favorites.png";

            struct stat st;
            if (stat(fav.path, &st) == 0) {
                double sz = static_cast<double>(st.st_size);
                const char* unit = "B";
                if (sz >= 1024.0) { sz /= 1024.0; unit = "KB"; }
                if (sz >= 1024.0) { sz /= 1024.0; unit = "MB"; }
                if (sz >= 1024.0) { sz /= 1024.0; unit = "GB"; }
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.2f %s", sz, unit);
                item.detail = buf;
            } else {
                item.detail = "Favorite";
            }
            m_items.push_back(std::move(item));
        }
    } else if (m_activeSource == 3) {
        // Recent Media
        recent_load();
        for (int i = 0; i < recent_file_count; ++i) {
            const auto& rec = recent_files[i];
            if (rec.path[0] == '\0') continue;

            FileCategory cat = browser ? browser->classifyFile(rec.path, 0) : FileCategory::Video;
            if (m_categoryFilter != -1 && static_cast<int>(cat) != m_categoryFilter) {
                continue;
            }

            std::string displayName = rec.title[0] ? rec.title : "";
            if (displayName.empty()) {
                std::string p(rec.path);
                size_t slash = p.find_last_of('/');
                displayName = (slash != std::string::npos) ? p.substr(slash + 1) : p;
            }

            if (m_isSearching && !matchesQuery(displayName, m_searchQuery) && !matchesQuery(rec.path, m_searchQuery)) {
                continue;
            }

            BrowserItem item;
            item.name = displayName;
            item.fullPath = rec.path;
            item.category = cat;
            item.badge = browser ? browser->getFileCategoryLabel(cat) : "RECENT";
            item.isFavorite = favorites_is_favorite(rec.path);
            item.lastPos = rec.last_pos;
            item.duration = rec.duration;
            if (rec.last_pos > 0.0 && rec.duration > 0.0) {
                item.progress = static_cast<int>((rec.last_pos / rec.duration) * 100.0);
                if (item.progress > 100) item.progress = 100;
            }

            if (cat == FileCategory::Video) item.iconPath = "../icons/icon_resume.png";
            else if (cat == FileCategory::Audio) item.iconPath = "../icons/icon_subtitles.png";
            else if (cat == FileCategory::Image) item.iconPath = "../icons/icon_palette.png";
            else item.iconPath = "../icons/icon_recent_files.png";

            struct stat st;
            if (stat(rec.path, &st) == 0) {
                double sz = static_cast<double>(st.st_size);
                const char* unit = "B";
                if (sz >= 1024.0) { sz /= 1024.0; unit = "KB"; }
                if (sz >= 1024.0) { sz /= 1024.0; unit = "MB"; }
                if (sz >= 1024.0) { sz /= 1024.0; unit = "GB"; }
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.2f %s", sz, unit);
                item.detail = buf;
            } else {
                item.detail = "Recent";
            }
            m_items.push_back(std::move(item));
        }
    }
}

void BrowserScreen::resetSelection() {
    m_selectedIndex = 0;
    m_scrollOffset = 0;
    m_settleMs = 0.0;
    m_cachedMetadata = MediaMetadataInfo();
    rebuildItems();
    m_browserFsm.postEvent(BrowserScreenEvent::FinishSearch);
    m_browserFsm.postEvent(BrowserScreenEvent::CursorMoved);
}

void BrowserScreen::setSource(int sourceIndex) {
    if (sourceIndex < 0 || sourceIndex > 6) sourceIndex = 0;
    m_sidebarIndex = sourceIndex;
    activateSidebar(true);
}

void BrowserScreen::setSearchQuery(const std::string& query) {
    auto browser = Application::getInstance().getFileSystemBrowser();
    if (!query.empty()) {
        m_isSearching = true;
        m_searchQuery = query;
        if (browser && (m_activeSource == 0 || m_activeSource == 1)) {
            browser->search(query);
        }
        resetSelection();
        size_t count = m_items.size();
        char msg[96];
        if (count > 0) {
            std::snprintf(msg, sizeof(msg), "\"%s\" (%zu found)", query.c_str(), count);
        } else {
            std::snprintf(msg, sizeof(msg), "\"%s\" (no matches)", query.c_str());
        }
        toast("SEARCH", msg);
    } else {
        m_isSearching = false;
        m_searchQuery.clear();
        if (browser) {
            browser->clearSearch();
            browser->refresh();
        }
        resetSelection();
        toast("SEARCH", "Cleared");
    }
}

void BrowserScreen::onEnter() {
    StatefulScreen::onEnter();
    favorites_load();
    recent_load();

    auto browser = Application::getInstance().getFileSystemBrowser();
    if (browser) {
        if (browser->getCurrentPath().empty()) {
            browser->loadLastFolder();
            if (browser->getCurrentPath().empty()) {
                browser->setCurrentPath("/mnt/usb0");
            }
        }
        /* Never open on an empty pane: a restored folder that has since been
         * deleted or emptied still resolves, so fall back to the USB root
         * rather than showing nothing. */
        if (browser->getEntryCount() == 0 && !m_isSearching) {
            browser->navigateToSource(0);
        }
        if (m_activeSource != 2 && m_activeSource != 3) {
            const std::string& path = browser->getCurrentPath();
            if (path.rfind("/data", 0) == 0) {
                m_activeSource = 1;
                m_sidebarIndex = (m_categoryFilter != -1) ? (m_categoryFilter + 3) : 1;
            } else {
                m_activeSource = 0;
                m_sidebarIndex = (m_categoryFilter != -1) ? (m_categoryFilter + 3) : 0;
            }
        } else {
            m_sidebarIndex = (m_categoryFilter != -1) ? (m_categoryFilter + 3) : m_activeSource;
        }
    }
    resetSelection();
}

void BrowserScreen::onExit() {
    StatefulScreen::onExit();
    auto browser = Application::getInstance().getFileSystemBrowser();
    if (browser) {
        browser->saveLastFolder();
    }
}

void BrowserScreen::openSearch() {
    m_browserFsm.postEvent(BrowserScreenEvent::StartSearch);
    evo_keyboard_open("Search media...", "", 64, OnSearchSubmitted, this);
}

void BrowserScreen::navigate(int delta) {
    int totalCount = static_cast<int>(m_items.size());
    if (totalCount == 0) {
        m_selectedIndex = 0;
        m_scrollOffset = 0;
        return;
    }

    int nextIndex = m_selectedIndex + delta;
    if (nextIndex < 0) {
        nextIndex = 0;
        evo_feedback(EVO_FB_BOUNDARY);
    } else if (nextIndex >= totalCount) {
        nextIndex = totalCount - 1;
        evo_feedback(EVO_FB_BOUNDARY);
    } else {
        evo_feedback(EVO_FB_MOVE);
    }

    if (nextIndex != m_selectedIndex) {
        m_selectedIndex = nextIndex;
        m_settleMs = 0.0;
        m_cachedMetadata = MediaMetadataInfo();
        m_browserFsm.postEvent(BrowserScreenEvent::CursorMoved);
    }
}

void BrowserScreen::jumpPage(int direction) {
    constexpr int pageSize = 8;
    navigate(direction * pageSize);
}

void BrowserScreen::jumpLetter(int direction) {
    if (m_items.empty() || m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_items.size())) {
        return;
    }

    const auto& currentItem = m_items[m_selectedIndex];
    char currentInitial = currentItem.name.empty() ? ' ' : std::toupper(static_cast<unsigned char>(currentItem.name[0]));
    int targetIndex = m_selectedIndex;

    if (direction > 0) {
        for (size_t i = m_selectedIndex + 1; i < m_items.size(); ++i) {
            const auto& item = m_items[i];
            if (!item.name.empty()) {
                char init = std::toupper(static_cast<unsigned char>(item.name[0]));
                if (init != currentInitial) {
                    targetIndex = static_cast<int>(i);
                    break;
                }
            }
        }
    } else {
        for (int i = m_selectedIndex - 1; i >= 0; --i) {
            const auto& item = m_items[i];
            if (!item.name.empty()) {
                char init = std::toupper(static_cast<unsigned char>(item.name[0]));
                if (init != currentInitial) {
                    while (i > 0) {
                        const auto& prev = m_items[i - 1];
                        if (prev.name.empty() || std::toupper(static_cast<unsigned char>(prev.name[0])) != init) {
                            break;
                        }
                        i--;
                    }
                    targetIndex = i;
                    break;
                }
            }
        }
    }

    if (targetIndex != m_selectedIndex) {
        navigate(targetIndex - m_selectedIndex);
    }
}

void BrowserScreen::activateSidebar(bool focusGrid) {
    auto browser = Application::getInstance().getFileSystemBrowser();

    if (focusGrid) {
        evo_feedback(EVO_FB_CONFIRM);
        m_focusPane = BrowserFocusPane::Grid;
    } else {
        evo_feedback(EVO_FB_MOVE);
    }

    switch (m_sidebarIndex) {
        case 0: // USB Drive
            if (m_activeSource != 0 || (browser && browser->getCurrentPath().rfind("/mnt/usb0", 0) != 0)) {
                m_activeSource = 0;
                m_categoryFilter = -1;
                m_isSearching = false;
                m_searchQuery.clear();
                if (browser) {
                    browser->clearSearch();
                    browser->setCurrentPath("/mnt/usb0");
                }
                resetSelection();
                if (focusGrid) toast("STORAGE", "USB Drive");
            }
            break;
        case 1: // Internal Storage
            if (m_activeSource != 1 || (browser && browser->getCurrentPath().rfind("/data", 0) != 0)) {
                m_activeSource = 1;
                m_categoryFilter = -1;
                m_isSearching = false;
                m_searchQuery.clear();
                if (browser) {
                    browser->clearSearch();
                    browser->setCurrentPath("/data");
                }
                resetSelection();
                if (focusGrid) toast("STORAGE", "Internal Storage");
            }
            break;
        case 2: // Favorites
            if (m_activeSource != 2) {
                m_activeSource = 2;
                m_categoryFilter = -1;
                m_isSearching = false;
                m_searchQuery.clear();
                resetSelection();
                if (focusGrid) toast("SOURCES", "Favorites");
            }
            break;
        case 3: // Recent Media
            if (m_activeSource != 3) {
                m_activeSource = 3;
                m_categoryFilter = -1;
                m_isSearching = false;
                m_searchQuery.clear();
                resetSelection();
                if (focusGrid) toast("SOURCES", "Recent Media");
            }
            break;
        case 4: // All Videos
            if (m_categoryFilter != static_cast<int>(FileCategory::Video)) {
                m_categoryFilter = static_cast<int>(FileCategory::Video);
                resetSelection();
                if (focusGrid) toast("FILTER", "Videos Only");
            }
            break;
        case 5: // All Music
            if (m_categoryFilter != static_cast<int>(FileCategory::Audio)) {
                m_categoryFilter = static_cast<int>(FileCategory::Audio);
                resetSelection();
                if (focusGrid) toast("FILTER", "Music Only");
            }
            break;
        case 6: // All Photos
            if (m_categoryFilter != static_cast<int>(FileCategory::Image)) {
                m_categoryFilter = static_cast<int>(FileCategory::Image);
                resetSelection();
                if (focusGrid) toast("FILTER", "Photos Only");
            }
            break;
        default:
            break;
    }
}

void BrowserScreen::activateSelection() {
    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_items.size())) {
        return;
    }

    const auto& item = m_items[m_selectedIndex];
    auto browser = Application::getInstance().getFileSystemBrowser();
    auto playback = Application::getInstance().getPlaybackController();
    auto screenMgr = Application::getInstance().getScreenManager();
    if (!playback || !screenMgr) return;

    if (item.category == FileCategory::Folder) {
        if (browser) {
            evo_feedback(EVO_FB_OPEN);
            browser->navigateInto(item.name);
            resetSelection();
        }
    } else if (item.category == FileCategory::Video || item.category == FileCategory::Audio) {
        auto settings = Application::getInstance().getSettingsService();
        bool resumeEnabled = settings ? settings->isResumePlaybackEnabled() : true;
        double resumePos = (item.lastPos > 0.0) ? item.lastPos :
                           (resumeEnabled ? playback->loadResumePosition(item.fullPath) : 0.0);
        if (resumePos > 5.0) {
            evo_feedback(EVO_FB_OPEN);
            if (auto dialog = dynamic_cast<ModalDialogScreen*>(screenMgr->getScreen(ScreenId::ResumePrompt))) {
                dialog->setResumeTarget(item.fullPath, resumePos, item.duration > 0.0 ? item.duration : m_cachedMetadata.durationSeconds);
            }
            screenMgr->navigateTo(ScreenId::ResumePrompt);
        } else {
            evo_feedback(EVO_FB_CONFIRM);
            playback->startPlayback(item.fullPath, 0.0);
            screenMgr->navigateTo(ScreenId::Player);
        }
    } else if (item.category == FileCategory::Document) {
        evo_feedback(EVO_FB_CONFIRM);
        if (auto reader = dynamic_cast<TextReaderScreen*>(screenMgr->getScreen(ScreenId::TextReader))) {
            reader->openFile(item.fullPath);
        }
        screenMgr->navigateTo(ScreenId::TextReader);
    } else if (item.category == FileCategory::Image) {
        evo_feedback(EVO_FB_CONFIRM);
        if (auto viewer = dynamic_cast<ImageViewerScreen*>(screenMgr->getScreen(ScreenId::ImageViewer))) {
            viewer->openImage(item.fullPath);
        }
        screenMgr->navigateTo(ScreenId::ImageViewer);
    }
}

bool BrowserScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    // Sidebar navigation pane
    if (m_focusPane == BrowserFocusPane::Sidebar) {
        if (pressed & PadButtons::Up) {
            if (m_sidebarIndex > 0) {
                m_sidebarIndex--;
                activateSidebar(false);
            } else {
                evo_feedback(EVO_FB_BOUNDARY);
            }
            return true;
        }
        if (pressed & PadButtons::Down) {
            if (m_sidebarIndex < 6) {
                m_sidebarIndex++;
                activateSidebar(false);
            } else {
                evo_feedback(EVO_FB_BOUNDARY);
            }
            return true;
        }
        if (pressed & PadButtons::Right) {
            activateSidebar(true);
            return true;
        }
        if (pressed & PadButtons::Left) {
            if (auto sm = Application::getInstance().getScreenManager()) {
                sm->setRailFocused(true);
                return true;
            }
        }
        if (pressed & PadButtons::Cross) {
            activateSidebar(true);
            return true;
        }
        if (pressed & PadButtons::Circle) {
            evo_feedback(EVO_FB_CANCEL);
            if (auto sm = Application::getInstance().getScreenManager()) {
                sm->navigateTo(ScreenId::MainMenu);
            }
            return true;
        }
        return false;
    }

    // Media Grid navigation (4 columns)
    if (pressed & PadButtons::Left) {
        if (m_selectedIndex % 4 == 0) {
            m_focusPane = BrowserFocusPane::Sidebar;
            if (m_categoryFilter != -1) {
                m_sidebarIndex = m_categoryFilter + 3;
            } else {
                m_sidebarIndex = m_activeSource;
            }
            evo_feedback(EVO_FB_MOVE);
            return true;
        }
        navigate(-1);
        return true;
    }
    if (pressed & PadButtons::Right) {
        navigate(1);
        return true;
    }
    if (pressed & PadButtons::Up) {
        navigate(-4);
        return true;
    }
    if (pressed & PadButtons::Down) {
        navigate(4);
        return true;
    }
    if (pressed & PadButtons::L1) {
        jumpPage(-1);
        return true;
    }
    if (pressed & PadButtons::R1) {
        jumpPage(1);
        return true;
    }
    if (pressed & PadButtons::L2) {
        jumpLetter(-1);
        return true;
    }
    if (pressed & PadButtons::R2) {
        jumpLetter(1);
        return true;
    }
    if (pressed & PadButtons::Cross) {
        activateSelection();
        return true;
    }
    if (pressed & PadButtons::Circle) {
        if (m_isSearching) {
            evo_feedback(EVO_FB_CANCEL);
            m_isSearching = false;
            m_searchQuery.clear();
            auto browser = Application::getInstance().getFileSystemBrowser();
            if (browser) {
                browser->clearSearch();
                browser->refresh();
            }
            resetSelection();
            toast("SEARCH", "Cleared");
            return true;
        }

        if (m_activeSource == 0 || m_activeSource == 1) {
            auto browser = Application::getInstance().getFileSystemBrowser();
            if (browser && !browser->getCurrentPath().empty() &&
                browser->getCurrentPath() != "/mnt/usb0" &&
                browser->getCurrentPath() != "/data") {
                evo_feedback(EVO_FB_CANCEL);
                browser->navigateUp();
                resetSelection();
                return true;
            }
        }

        // At root or in Favorites / Recent Media: step back into Sidebar
        m_focusPane = BrowserFocusPane::Sidebar;
        evo_feedback(EVO_FB_CANCEL);
        return true;
    }
    if (pressed & PadButtons::Square) {
        openSearch();
        return true;
    }
    if (pressed & PadButtons::Triangle) {
        if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_items.size())) {
            auto& item = m_items[m_selectedIndex];
            if (item.category != FileCategory::Folder) {
                if (favorites_is_favorite(item.fullPath.c_str())) {
                    favorites_remove(item.fullPath.c_str());
                    favorites_save();
                    item.isFavorite = false;
                    toast("FAVORITES", "Removed");
                } else {
                    favorites_add(item.fullPath.c_str(), item.name.c_str(), item.duration);
                    favorites_save();
                    item.isFavorite = true;
                    toast("FAVORITES", "Added");
                }
                if (m_activeSource == 2) {
                    rebuildItems();
                    if (m_selectedIndex >= static_cast<int>(m_items.size()) && m_selectedIndex > 0) {
                        m_selectedIndex = static_cast<int>(m_items.size()) - 1;
                    }
                }
            }
        }
        return true;
    }

    return false;
}

void BrowserScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
    m_browserFsm.update(deltaMs);

    m_settleMs += deltaMs;
    auto coverService = Application::getInstance().getCoverArtService();

    if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_items.size())) {
        const auto& item = m_items[m_selectedIndex];
        bool isDir = (item.category == FileCategory::Folder);
        if (m_settleMs >= 200.0) {
            if (coverService) {
                coverService->ensureBrowserPreview(item.fullPath, isDir);
            }
            if (m_browserFsm.getCurrentState() == BrowserScreenState::Browsing) {
                m_browserFsm.postEvent(BrowserScreenEvent::SettleTriggered);
                if (auto metaService = Application::getInstance().getMediaMetadataService()) {
                    if (m_cachedMetadata.filePath != item.fullPath) {
                        m_cachedMetadata = metaService->extractBasicMetadata(item.fullPath);
                    }
                }
            }
        }
    }
}

void BrowserScreen::render(uint32_t* framebuffer, int width, int height) {
    auto browser = Application::getInstance().getFileSystemBrowser();
    auto coverService = Application::getInstance().getCoverArtService();

    evo_rmlui_browser_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.title = "STORAGE";

    // Format top location breadcrumbs
    auto formatPathBreadcrumbs = [](const std::string& prefix, const std::string& fullPath, size_t prefixLen) -> std::string {
        if (fullPath.length() <= prefixLen) {
            return prefix;
        }
        std::string sub = fullPath.substr(prefixLen);
        std::string result = prefix;
        std::string currentPart;
        for (char ch : sub) {
            if (ch == '/') {
                if (!currentPart.empty()) {
                    result += " / " + currentPart;
                    currentPart.clear();
                }
            } else {
                currentPart += ch;
            }
        }
        if (!currentPart.empty()) {
            result += " / " + currentPart;
        }
        return result;
    };

    if (m_activeSource == 0) {
        if (browser && !browser->getCurrentPath().empty()) {
            if (browser->getCurrentPath() == "/mnt/usb0") {
                std::snprintf(m_formattedPath, sizeof(m_formattedPath), "USB Drive");
            } else if (browser->getCurrentPath().rfind("/mnt/usb0", 0) == 0) {
                std::string bc = formatPathBreadcrumbs("USB Drive", browser->getCurrentPath(), 9);
                std::snprintf(m_formattedPath, sizeof(m_formattedPath), "%s", bc.c_str());
            } else {
                std::snprintf(m_formattedPath, sizeof(m_formattedPath), "%s", browser->getCurrentPath().c_str());
            }
        } else {
            std::snprintf(m_formattedPath, sizeof(m_formattedPath), "USB Drive");
        }
    } else if (m_activeSource == 1) {
        if (browser && !browser->getCurrentPath().empty()) {
            if (browser->getCurrentPath() == "/data") {
                std::snprintf(m_formattedPath, sizeof(m_formattedPath), "Internal Storage");
            } else if (browser->getCurrentPath().rfind("/data", 0) == 0) {
                std::string bc = formatPathBreadcrumbs("Internal Storage", browser->getCurrentPath(), 5);
                std::snprintf(m_formattedPath, sizeof(m_formattedPath), "%s", bc.c_str());
            } else {
                std::snprintf(m_formattedPath, sizeof(m_formattedPath), "%s", browser->getCurrentPath().c_str());
            }
        } else {
            std::snprintf(m_formattedPath, sizeof(m_formattedPath), "Internal Storage");
        }
    } else if (m_activeSource == 2) {
        std::snprintf(m_formattedPath, sizeof(m_formattedPath), "Favorites");
    } else if (m_activeSource == 3) {
        std::snprintf(m_formattedPath, sizeof(m_formattedPath), "Recent Media");
    }

    if (m_categoryFilter == static_cast<int>(FileCategory::Video)) {
        std::strncat(m_formattedPath, " / Videos", sizeof(m_formattedPath) - std::strlen(m_formattedPath) - 1);
    } else if (m_categoryFilter == static_cast<int>(FileCategory::Audio)) {
        std::strncat(m_formattedPath, " / Music", sizeof(m_formattedPath) - std::strlen(m_formattedPath) - 1);
    } else if (m_categoryFilter == static_cast<int>(FileCategory::Image)) {
        std::strncat(m_formattedPath, " / Photos", sizeof(m_formattedPath) - std::strlen(m_formattedPath) - 1);
    }

    params.path = m_formattedPath;

    // At root check
    if (m_activeSource == 0) {
        params.at_root = (!browser || browser->getCurrentPath().empty() || browser->getCurrentPath() == "/mnt/usb0") ? 1 : 0;
    } else if (m_activeSource == 1) {
        params.at_root = (!browser || browser->getCurrentPath().empty() || browser->getCurrentPath() == "/data") ? 1 : 0;
    } else {
        params.at_root = 1;
    }

    if (m_activeSource == 2) {
        params.empty_title = "NO FAVORITES YET";
        params.empty_hint = "Press [Triangle] on any file to add to Favorites";
    } else if (m_activeSource == 3) {
        params.empty_title = "NO RECENT MEDIA";
        params.empty_hint = "Media you play will appear here";
    } else if (m_isSearching) {
        params.empty_title = "NO MATCHES FOUND";
        params.empty_hint = "Try a different search keyword";
    } else {
        params.empty_title = "FOLDER IS EMPTY";
        params.empty_hint = "No supported media files found";
    }

    bool railFocused = false;
    if (auto sm = Application::getInstance().getScreenManager()) {
        railFocused = sm->isRailFocused();
    }
    params.rail_focused = railFocused ? 1 : 0;
    params.sidebar_focused = (m_focusPane == BrowserFocusPane::Sidebar) ? 1 : 0;
    params.sidebar_index = m_sidebarIndex;
    params.active_source = m_activeSource;

    int totalCount = static_cast<int>(m_items.size());
    params.total_count = totalCount;
    params.is_empty = (totalCount == 0);

    if (totalCount == 0) {
        m_selectedIndex = 0;
        m_scrollOffset = 0;
    } else if (m_selectedIndex >= totalCount) {
        m_selectedIndex = totalCount - 1;
    } else if (m_selectedIndex < 0) {
        m_selectedIndex = 0;
    }

    params.cursor_index = totalCount > 0 ? m_selectedIndex : -1;

    // 8 cards visible per page (2 rows x 4 columns)
    constexpr int visibleCards = 8;
    if (m_selectedIndex < m_scrollOffset) {
        m_scrollOffset = (m_selectedIndex / 4) * 4;
    } else if (m_selectedIndex >= m_scrollOffset + visibleCards) {
        m_scrollOffset = ((m_selectedIndex - visibleCards + 4) / 4) * 4;
    }
    if (m_scrollOffset < 0) m_scrollOffset = 0;

    params.row_count = std::min(visibleCards, std::max(0, totalCount - m_scrollOffset));

    /*
     * One poster extraction per CoverIntervalMs of wall clock.
     *
     * This used to be one per render() call, which sounds like a throttle and
     * is not: render() runs on every pass of the main loop, hundreds of times
     * a second in a menu, so extractions ran back to back with only the decode
     * itself between them. A page of 4K HEVC files then allocated and freed
     * tens of MB per decode with no gap, and after a few dozen the heap could
     * no longer serve one - see the pre-flight in
     * CoverArtService::extractVideoFrame. Pacing on the clock gives the
     * allocator room to settle and keeps paging responsive.
     */
    constexpr uint64_t CoverIntervalMs = 150;
    static uint64_t s_lastCoverMs = 0;
    uint64_t nowCoverMs = 0;
    {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        nowCoverMs = static_cast<uint64_t>(tv.tv_sec) * 1000ULL +
                     static_cast<uint64_t>(tv.tv_usec / 1000ULL);
    }
    int coverBudget = (nowCoverMs - s_lastCoverMs >= CoverIntervalMs) ? 1 : 0;

    for (int i = 0; i < params.row_count; ++i) {
        int idx = m_scrollOffset + i;
        const auto& item = m_items[idx];
        params.rows[i].name = item.name.c_str();
        params.rows[i].badge = item.badge.c_str();
        params.rows[i].is_focused = (m_selectedIndex == idx);
        params.rows[i].progress = item.progress;
        params.rows[i].is_favorite = item.isFavorite;
        params.rows[i].icon_path = item.iconPath.c_str();
        params.rows[i].detail = item.detail.c_str();

        const uint32_t* art = nullptr;
        if (coverService) {
            art = coverService->peekCoverArt(item.fullPath);
            bool tried = coverService->hasTriedCoverArt(item.fullPath);
            if (!art && !tried && coverBudget > 0) {
                art = coverService->getCoverArt(item.fullPath, item.category == FileCategory::Folder);
                coverBudget--;
                /* Stamped after, not before: the interval is a gap between
                 * extractions, so a slow 4K decode does not immediately earn
                 * the next one. */
                struct timeval tvDone;
                gettimeofday(&tvDone, nullptr);
                s_lastCoverMs = static_cast<uint64_t>(tvDone.tv_sec) * 1000ULL +
                                static_cast<uint64_t>(tvDone.tv_usec / 1000ULL);
            }
        }
        params.rows[i].art = art;
        params.rows[i].art_w = ICoverArtService::PosterWidth;
        params.rows[i].art_h = ICoverArtService::PosterHeight;

        if (item.duration > 0.0) {
            int dur = static_cast<int>(item.duration);
            int h = dur / 3600;
            int m = (dur % 3600) / 60;
            int s = dur % 60;
            if (h > 0) std::snprintf(m_rowDurations[i], sizeof(m_rowDurations[i]), "%d:%02d:%02d", h, m, s);
            else std::snprintf(m_rowDurations[i], sizeof(m_rowDurations[i]), "%02d:%02d", m, s);
            params.rows[i].duration = m_rowDurations[i];
        } else if (m_cachedMetadata.filePath == item.fullPath && m_cachedMetadata.durationSeconds > 0.0) {
            int dur = static_cast<int>(m_cachedMetadata.durationSeconds);
            int h = dur / 3600;
            int m = (dur % 3600) / 60;
            int s = dur % 60;
            if (h > 0) std::snprintf(m_rowDurations[i], sizeof(m_rowDurations[i]), "%d:%02d:%02d", h, m, s);
            else std::snprintf(m_rowDurations[i], sizeof(m_rowDurations[i]), "%02d:%02d", m, s);
            params.rows[i].duration = m_rowDurations[i];
        } else {
            m_rowDurations[i][0] = '\0';
            params.rows[i].duration = nullptr;
        }
    }

    // Selected item metadata for preview and bottom status strip
    if (m_selectedIndex >= 0 && m_selectedIndex < totalCount) {
        const auto& curItem = m_items[m_selectedIndex];
        std::snprintf(m_insName, sizeof(m_insName), "%s", curItem.name.c_str());
        std::snprintf(m_insKind, sizeof(m_insKind), "%s", curItem.badge.c_str());

        size_t lastDot = curItem.name.find_last_of('.');
        if (lastDot != std::string::npos && lastDot + 1 < curItem.name.size()) {
            std::string ext = curItem.name.substr(lastDot + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
            std::snprintf(m_insExt, sizeof(m_insExt), "%s", ext.c_str());
        } else {
            std::snprintf(m_insExt, sizeof(m_insExt), "%s", curItem.category == FileCategory::Folder ? "DIR" : "FILE");
        }

        params.ins_name = m_insName;
        params.ins_kind = m_insKind;
        params.ins_ext = m_insExt;

        // Technical specs for bottom status strip
        if (m_cachedMetadata.filePath == curItem.fullPath) {
            if (m_cachedMetadata.width > 0 && m_cachedMetadata.height > 0) {
                if (m_cachedMetadata.width >= 3840 || m_cachedMetadata.height >= 2160) {
                    std::snprintf(m_statusRes, sizeof(m_statusRes), "4K UHD (%dx%d)", m_cachedMetadata.width, m_cachedMetadata.height);
                } else if (m_cachedMetadata.width >= 1920 || m_cachedMetadata.height >= 1080) {
                    std::snprintf(m_statusRes, sizeof(m_statusRes), "1080p (%dx%d)", m_cachedMetadata.width, m_cachedMetadata.height);
                } else if (m_cachedMetadata.width >= 1280 || m_cachedMetadata.height >= 720) {
                    std::snprintf(m_statusRes, sizeof(m_statusRes), "720p (%dx%d)", m_cachedMetadata.width, m_cachedMetadata.height);
                } else {
                    std::snprintf(m_statusRes, sizeof(m_statusRes), "%dx%d", m_cachedMetadata.width, m_cachedMetadata.height);
                }
                params.status_res = m_statusRes;
            }

            if (!m_cachedMetadata.videoCodec.empty()) {
                std::snprintf(m_statusVCodec, sizeof(m_statusVCodec), "%s", m_cachedMetadata.videoCodec.c_str());
                params.status_vcodec = m_statusVCodec;
            }
            if (!m_cachedMetadata.audioCodec.empty()) {
                std::snprintf(m_statusACodec, sizeof(m_statusACodec), "%s", m_cachedMetadata.audioCodec.c_str());
                params.status_acodec = m_statusACodec;
            }

            double totalDur = (m_cachedMetadata.durationSeconds > 0.0) ? m_cachedMetadata.durationSeconds : curItem.duration;
            if (totalDur > 0.0) {
                int dur = static_cast<int>(totalDur);
                int h = dur / 3600;
                int m = (dur % 3600) / 60;
                int s = dur % 60;
                if (h > 0) std::snprintf(m_statusDuration, sizeof(m_statusDuration), "%02d:%02d:%02d", h, m, s);
                else std::snprintf(m_statusDuration, sizeof(m_statusDuration), "%02d:%02d", m, s);
                params.status_duration = m_statusDuration;
            }
        }

        struct stat st;
        if (!curItem.fullPath.empty() && stat(curItem.fullPath.c_str(), &st) == 0 && curItem.category != FileCategory::Folder) {
            double sz = static_cast<double>(st.st_size);
            const char* unit = "B";
            if (sz >= 1024.0) { sz /= 1024.0; unit = "KB"; }
            if (sz >= 1024.0) { sz /= 1024.0; unit = "MB"; }
            if (sz >= 1024.0) { sz /= 1024.0; unit = "GB"; }
            std::snprintf(m_statusSize, sizeof(m_statusSize), "%.2f %s", sz, unit);
            params.status_size = m_statusSize;
        }

        if (coverService) {
            params.ins_preview = coverService->getBrowserPreviewPixels();
            params.ins_preview_w = ICoverArtService::PreviewWidth;
            params.ins_preview_h = ICoverArtService::PreviewHeight;
        }

        params.ins_probing = (m_settleMs >= 200.0 && m_cachedMetadata.filePath != curItem.fullPath && curItem.category != FileCategory::Folder) ? 1 : 0;
    }

    evo_rmlui_update_browser(&params);
    evo_rmlui_render_browser(framebuffer, width, height);
}

} // namespace evo
