#include "evo/screens/BrowserScreen.hpp"
#include "evo/screens/TextReaderScreen.hpp"
#include "evo/screens/ImageViewerScreen.hpp"
#include "evo/screens/ModalDialogScreen.hpp"
#include "evo/animation/AnimationManager.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_keyboard.h"
#include "evo_favorites.h"
#include "evo_feedback.h"
#include "evo_toast.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <sys/stat.h>

extern "C" {
#include <libavformat/avformat.h>
}



namespace evo {

static void OnSearchSubmitted(const char* text, void* userdata) {
    auto* self = static_cast<BrowserScreen*>(userdata);
    auto browser = Application::getInstance().getFileSystemBrowser();
    if (!browser) return;

    std::string queryStr;
    if (text) {
        queryStr = text;
        // Trim leading and trailing whitespace, CR, LF, tabs
        size_t start = queryStr.find_first_not_of(" \t\r\n");
        if (start != std::string::npos) {
            size_t end = queryStr.find_last_not_of(" \t\r\n");
            queryStr = queryStr.substr(start, end - start + 1);
        } else {
            queryStr.clear();
        }
    }

    if (!queryStr.empty()) {
        browser->search(queryStr);
        if (self) {
            self->resetSelection();
        }
        size_t count = browser->getEntryCount();
        char msg[96];
        if (count > 0) {
            std::snprintf(msg, sizeof(msg), "\"%s\" (%zu found)", queryStr.c_str(), count);
        } else {
            std::snprintf(msg, sizeof(msg), "\"%s\" (no matches)", queryStr.c_str());
        }
        toast("SEARCH", msg);
    } else {
        browser->clearSearch();
        browser->refresh();
        if (self) {
            self->resetSelection();
        }
        toast("SEARCH", "Cleared");
    }
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

void BrowserScreen::resetSelection() {
    m_selectedIndex = 0;
    m_scrollOffset = 0;
    m_settleMs = 0.0;
    m_cachedMetadata = MediaMetadataInfo();
    m_browserFsm.postEvent(BrowserScreenEvent::FinishSearch);
    m_browserFsm.postEvent(BrowserScreenEvent::CursorMoved);
}

void BrowserScreen::onEnter() {
    StatefulScreen::onEnter();
    auto browser = Application::getInstance().getFileSystemBrowser();
    if (browser) {
        browser->refresh();
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

void BrowserScreen::navigate(int delta) {
    auto browser = Application::getInstance().getFileSystemBrowser();
    if (!browser || browser->getEntryCount() == 0) return;

    m_settleMs = 0.0;
    m_browserFsm.postEvent(BrowserScreenEvent::CursorMoved);
    evo::animation::AnimationManager::getInstance().triggerTransition(350.0);
    int count = static_cast<int>(browser->getEntryCount());
    m_selectedIndex += delta;

    if (m_selectedIndex < 0) {
        m_selectedIndex = 0;
        evo_feedback(EVO_FB_BOUNDARY);
    } else if (m_selectedIndex >= count) {
        m_selectedIndex = count - 1;
        evo_feedback(EVO_FB_BOUNDARY);
    } else {
        evo_feedback(EVO_FB_MOVE);
    }

    // Scroll window of 8 rows (strictly matches 742px height of #browser-list in browser.rcss)
    constexpr int visibleRows = 8;
    if (m_selectedIndex < m_scrollOffset) {
        m_scrollOffset = m_selectedIndex;
    } else if (m_selectedIndex >= m_scrollOffset + visibleRows) {
        m_scrollOffset = m_selectedIndex - visibleRows + 1;
    }
}

void BrowserScreen::jumpPage(int direction) {
    navigate(direction * 7);
}

void BrowserScreen::jumpLetter(int direction) {
    auto browser = Application::getInstance().getFileSystemBrowser();
    if (!browser || browser->getEntryCount() == 0) return;

    int count = static_cast<int>(browser->getEntryCount());
    const auto* currentEntry = browser->getEntry(m_selectedIndex);
    if (!currentEntry || currentEntry->name.empty()) return;

    char currentInitial = std::toupper(static_cast<unsigned char>(currentEntry->name[0]));
    int targetIndex = m_selectedIndex;

    if (direction > 0) {
        for (int i = m_selectedIndex + 1; i < count; ++i) {
            const auto* entry = browser->getEntry(i);
            if (entry && !entry->name.empty()) {
                char init = std::toupper(static_cast<unsigned char>(entry->name[0]));
                if (init != currentInitial) {
                    targetIndex = i;
                    break;
                }
            }
        }
    } else {
        for (int i = m_selectedIndex - 1; i >= 0; --i) {
            const auto* entry = browser->getEntry(i);
            if (entry && !entry->name.empty()) {
                char init = std::toupper(static_cast<unsigned char>(entry->name[0]));
                if (init != currentInitial) {
                    // Find first of this previous letter
                    while (i > 0) {
                        const auto* prev = browser->getEntry(i - 1);
                        if (!prev || prev->name.empty() ||
                            std::toupper(static_cast<unsigned char>(prev->name[0])) != init) {
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

void BrowserScreen::activateSelection() {
    auto browser = Application::getInstance().getFileSystemBrowser();
    auto playback = Application::getInstance().getPlaybackController();
    auto screenMgr = Application::getInstance().getScreenManager();
    if (!browser || !playback || !screenMgr) return;

    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(browser->getEntryCount())) {
        return;
    }

    const auto* entry = browser->getEntry(m_selectedIndex);
    if (!entry) return;

    if (entry->category == FileCategory::Folder) {
        evo_feedback(EVO_FB_OPEN);
        browser->navigateInto(entry->name);
        m_selectedIndex = 0;
        m_scrollOffset = 0;
    } else if (entry->category == FileCategory::Video || entry->category == FileCategory::Audio) {
        std::string fullPath = browser->getFullPath(m_selectedIndex);
        auto settings = Application::getInstance().getSettingsService();
        bool resumeEnabled = settings ? settings->isResumePlaybackEnabled() : true;
        double resumePos = resumeEnabled ? playback->loadResumePosition(fullPath) : 0.0;
        if (resumePos > 5.0) {
            evo_feedback(EVO_FB_OPEN);
            if (auto dialog = dynamic_cast<ModalDialogScreen*>(screenMgr->getScreen(ScreenId::ResumePrompt))) {
                dialog->setResumeTarget(fullPath, resumePos, m_cachedMetadata.durationSeconds);
            }
            screenMgr->navigateTo(ScreenId::ResumePrompt);
        } else {
            evo_feedback(EVO_FB_CONFIRM);
            playback->startPlayback(fullPath, 0.0);
            screenMgr->navigateTo(ScreenId::Player);
        }
    } else if (entry->category == FileCategory::Document) {
        evo_feedback(EVO_FB_CONFIRM);
        std::string fullPath = browser->getFullPath(m_selectedIndex);
        if (auto reader = dynamic_cast<TextReaderScreen*>(screenMgr->getScreen(ScreenId::TextReader))) {
            reader->openFile(fullPath);
        }
        screenMgr->navigateTo(ScreenId::TextReader);
    } else if (entry->category == FileCategory::Image) {
        evo_feedback(EVO_FB_CONFIRM);
        std::string fullPath = browser->getFullPath(m_selectedIndex);
        if (auto imgViewer = dynamic_cast<ImageViewerScreen*>(screenMgr->getScreen(ScreenId::ImageViewer))) {
            imgViewer->openImage(fullPath);
        }
        screenMgr->navigateTo(ScreenId::ImageViewer);
    } else {
        toast("UNSUPPORTED", entry->name.c_str());
    }
}

void BrowserScreen::openSearch() {
    m_browserFsm.postEvent(BrowserScreenEvent::StartSearch);
    auto browser = Application::getInstance().getFileSystemBrowser();
    const char* query = browser ? browser->getSearchQuery().c_str() : "";
    evo_keyboard_open("SEARCH IN DIRECTORY", query, 48, OnSearchSubmitted, this);
}

bool BrowserScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    if (pressed & PadButtons::Up) {
        navigate(-1);
        return true;
    }
    if (pressed & PadButtons::Down) {
        navigate(1);
        return true;
    }
    if (pressed & PadButtons::Left) {
        if (auto sm = Application::getInstance().getScreenManager()) {
            sm->setRailFocused(true);
            return true;
        }
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
        auto browser = Application::getInstance().getFileSystemBrowser();
        if (browser && browser->isSearching()) {
            evo_feedback(EVO_FB_CANCEL);
            browser->clearSearch();
            browser->refresh();
            resetSelection();
            toast("SEARCH", "Cleared");
            return true;
        }
        if (browser && !browser->getCurrentPath().empty()) {
            evo_feedback(EVO_FB_CANCEL);
            browser->navigateUp();
            resetSelection();
        } else {
            evo_feedback(EVO_FB_CANCEL);
            if (auto sm = Application::getInstance().getScreenManager()) {
                sm->navigateTo(ScreenId::MainMenu);
            }
        }
        return true;
    }
    if (pressed & PadButtons::Square) {
        openSearch();
        return true;
    }
    if (pressed & PadButtons::Triangle) {
        auto browser = Application::getInstance().getFileSystemBrowser();
        if (browser && m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(browser->getEntryCount())) {
            std::string fullPath = browser->getFullPath(m_selectedIndex);
            const auto* entry = browser->getEntry(m_selectedIndex);
            if (favorites_is_favorite(fullPath.c_str())) {
                favorites_remove(fullPath.c_str());
                favorites_save();
                toast("FAVORITES", "Removed");
            } else if (entry && entry->category != FileCategory::Folder) {
                favorites_add(fullPath.c_str(), entry->name.c_str(), 0.0);
                favorites_save();
                toast("FAVORITES", "Added");
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
    auto browser = Application::getInstance().getFileSystemBrowser();
    auto coverService = Application::getInstance().getCoverArtService();
    if (browser && m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(browser->getEntryCount())) {
        std::string fullPath = browser->getFullPath(m_selectedIndex);
        const auto* entry = browser->getEntry(m_selectedIndex);
        bool isDir = (entry && entry->category == FileCategory::Folder);
        // Settle gate: Extracting video thumbnail frames and probing media headers are
        // expensive operations (avformat_open_input, demux, avcodec decode, sws_scale).
        // Matches main.c.legacy EVO_PROBE_SETTLE_FRAMES (12 frames ~ 200ms).
        // During fast scrolling, settle timer is continually reset, completely preventing
        // background decode churn, UI latency spikes, and decoder crashes.
        if (m_settleMs >= 200.0) {
            if (coverService) {
                coverService->ensureBrowserPreview(fullPath, isDir);
            }

            // Probe media metadata when cursor has settled on a file for > 200ms
            if (!isDir && (entry->category == FileCategory::Video || entry->category == FileCategory::Audio) &&
                m_cachedMetadata.filePath != fullPath) {
                m_browserFsm.postEvent(BrowserScreenEvent::SettleTriggered);
                if (auto metaService = Application::getInstance().getMediaMetadataService()) {
                    AVFormatContext* fmt = nullptr;
                    if (avformat_open_input(&fmt, fullPath.c_str(), nullptr, nullptr) == 0) {
                        if (avformat_find_stream_info(fmt, nullptr) >= 0) {
                            m_cachedMetadata = metaService->extractMetadataFromFormat(fmt, fullPath);
                        } else {
                            m_cachedMetadata = metaService->extractBasicMetadata(fullPath);
                        }
                        avformat_close_input(&fmt);
                    } else {
                        m_cachedMetadata = metaService->extractBasicMetadata(fullPath);
                    }
                }
            }
        } else if (!isDir && entry->category != FileCategory::Video && entry->category != FileCategory::Audio &&
                   m_cachedMetadata.filePath != fullPath) {
            if (auto metaService = Application::getInstance().getMediaMetadataService()) {
                m_cachedMetadata = metaService->extractBasicMetadata(fullPath);
            }
        }
    }
}

void BrowserScreen::render(uint32_t* framebuffer, int width, int height) {
    auto browser = Application::getInstance().getFileSystemBrowser();
    auto coverService = Application::getInstance().getCoverArtService();
    if (!browser) return;

    evo_rmlui_browser_params_t params;
    std::memset(&params, 0, sizeof(params));

    bool isSearching = browser->isSearching();
    std::string currentPath = browser->getCurrentPath();
    std::string searchBreadcrumb;
    if (isSearching) {
        if (currentPath.empty()) {
            searchBreadcrumb = "ALL STORAGE - [SEARCH: \"" + browser->getSearchQuery() + "\"]";
        } else {
            searchBreadcrumb = currentPath + " - [SEARCH: \"" + browser->getSearchQuery() + "\"]";
        }
        params.path = searchBreadcrumb.c_str();
        params.title = "SEARCH RESULTS";
        params.at_root = 0;
        params.empty_title = "NO RESULTS FOUND";
        params.empty_hint = "No files matched your search query";
    } else {
        params.path = currentPath.empty() ? "SELECT STORAGE" : currentPath.c_str();
        params.title = "STORAGE BROWSER";
        params.at_root = currentPath.empty() || currentPath == "/" ? 1 : 0;
        params.empty_title = "THIS FOLDER IS EMPTY";
        params.empty_hint = "No supported media files found";
    }

    bool railFocused = false;
    if (auto sm = Application::getInstance().getScreenManager()) {
        railFocused = sm->isRailFocused();
    }
    params.rail_focused = railFocused ? 1 : 0;

    int totalCount = static_cast<int>(browser->getEntryCount());
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

    constexpr int visibleRows = 8;
    if (m_selectedIndex < m_scrollOffset) {
        m_scrollOffset = m_selectedIndex;
    } else if (m_selectedIndex >= m_scrollOffset + visibleRows) {
        m_scrollOffset = m_selectedIndex - visibleRows + 1;
    }
    if (m_scrollOffset < 0) m_scrollOffset = 0;

    params.row_count = std::min(visibleRows, std::max(0, totalCount - m_scrollOffset));

    for (int i = 0; i < params.row_count; ++i) {
        int idx = m_scrollOffset + i;
        const auto* entry = browser->getEntry(idx);
        if (entry) {
            params.rows[i].name = entry->name.c_str();
            params.rows[i].badge = browser->getFileCategoryLabel(entry->category);
            params.rows[i].is_focused = (m_selectedIndex == idx);
            params.rows[i].progress = -1;
            std::string fullPath = browser->getFullPath(idx);
            params.rows[i].is_favorite = favorites_is_favorite(fullPath.c_str());

            const char* iconPath = "../icons/icon_about_support.png";
            if (entry->category == FileCategory::Folder) {
                iconPath = "../icons/icon_folder.png";
            } else if (entry->category == FileCategory::Video) {
                iconPath = "../icons/icon_resume.png";
            } else if (entry->category == FileCategory::Audio) {
                iconPath = "../icons/icon_subtitles.png";
            } else if (entry->category == FileCategory::Image) {
                iconPath = "../icons/icon_palette.png";
            }
            if (!isSearching && params.at_root) {
                iconPath = "../icons/icon_browse_usb.png";
            }
            params.rows[i].icon_path = iconPath;

            if (entry->category == FileCategory::Folder) {
                std::snprintf(m_rowDetails[i], sizeof(m_rowDetails[i]), "Folder");
            } else {
                struct stat st;
                if (!fullPath.empty() && stat(fullPath.c_str(), &st) == 0) {
                    double sz = static_cast<double>(st.st_size);
                    const char* unit = "B";
                    if (sz >= 1024.0) { sz /= 1024.0; unit = "KB"; }
                    if (sz >= 1024.0) { sz /= 1024.0; unit = "MB"; }
                    if (sz >= 1024.0) { sz /= 1024.0; unit = "GB"; }
                    std::snprintf(m_rowDetails[i], sizeof(m_rowDetails[i]), "%.2f %s", sz, unit);
                } else {
                    std::snprintf(m_rowDetails[i], sizeof(m_rowDetails[i]), "Media File");
                }
            }
            params.rows[i].detail = m_rowDetails[i];
        }
    }

    if (m_selectedIndex >= 0 && m_selectedIndex < totalCount) {
        const auto* curEntry = browser->getEntry(m_selectedIndex);
        if (curEntry) {
            std::string selectedPath = browser->getFullPath(m_selectedIndex);
            std::snprintf(m_insName, sizeof(m_insName), "%s", curEntry->name.c_str());
            std::snprintf(m_insKind, sizeof(m_insKind), "%s", browser->getFileCategoryLabel(curEntry->category));

            size_t lastDot = curEntry->name.find_last_of('.');
            if (lastDot != std::string::npos && lastDot + 1 < curEntry->name.size()) {
                std::string ext = curEntry->name.substr(lastDot + 1);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
                std::snprintf(m_insExt, sizeof(m_insExt), "%s", ext.c_str());
            } else {
                m_insExt[0] = '\0';
            }

            params.ins_name = m_insName;
            params.ins_kind = m_insKind;
            params.ins_ext = m_insExt[0] ? m_insExt : nullptr;

            if (coverService) {
                params.ins_preview = coverService->getBrowserPreviewPixels();
                params.ins_preview_w = CoverArtService::PreviewWidth;
                params.ins_preview_h = CoverArtService::PreviewHeight;
            }

            int propCount = 0;
            if (curEntry->category == FileCategory::Folder) {
                m_insProps[propCount].key = "TYPE";
                m_insProps[propCount].value = "Directory";
                propCount++;
                params.ins_preview_badge = "FOLDER";
            } else {
                struct stat st;
                if (!selectedPath.empty() && stat(selectedPath.c_str(), &st) == 0) {
                    double sz = static_cast<double>(st.st_size);
                    const char* unit = "B";
                    if (sz >= 1024.0) { sz /= 1024.0; unit = "KB"; }
                    if (sz >= 1024.0) { sz /= 1024.0; unit = "MB"; }
                    if (sz >= 1024.0) { sz /= 1024.0; unit = "GB"; }
                    std::snprintf(m_insSize, sizeof(m_insSize), "%.2f %s", sz, unit);
                    m_insProps[propCount].key = "SIZE";
                    m_insProps[propCount].value = m_insSize;
                    propCount++;
                }

                if (m_cachedMetadata.filePath == selectedPath) {
                    if (!m_cachedMetadata.container.empty() && m_cachedMetadata.container != "Unknown") {
                        std::snprintf(m_insContainer, sizeof(m_insContainer), "%s", m_cachedMetadata.container.c_str());
                        m_insProps[propCount].key = "CONTAINER";
                        m_insProps[propCount].value = m_insContainer;
                        propCount++;
                    }
                    if (m_cachedMetadata.durationSeconds > 0.0) {
                        int dur = static_cast<int>(m_cachedMetadata.durationSeconds);
                        int h = dur / 3600;
                        int m = (dur % 3600) / 60;
                        int s = dur % 60;
                        if (h > 0) {
                            std::snprintf(m_insDuration, sizeof(m_insDuration), "%d:%02d:%02d", h, m, s);
                        } else {
                            std::snprintf(m_insDuration, sizeof(m_insDuration), "%02d:%02d", m, s);
                        }
                        m_insProps[propCount].key = "DURATION";
                        m_insProps[propCount].value = m_insDuration;
                        propCount++;
                        params.ins_preview_badge = m_insDuration;
                    } else {
                        params.ins_preview_badge = m_insExt[0] ? m_insExt : "MEDIA";
                    }
                    if (m_cachedMetadata.width > 0 && m_cachedMetadata.height > 0) {
                        std::snprintf(m_insResolution, sizeof(m_insResolution), "%d x %d", m_cachedMetadata.width, m_cachedMetadata.height);
                        m_insProps[propCount].key = "RESOLUTION";
                        m_insProps[propCount].value = m_insResolution;
                        propCount++;
                    }
                    if (!m_cachedMetadata.videoCodec.empty() && m_cachedMetadata.videoCodec != "Unknown") {
                        std::snprintf(m_insVCodec, sizeof(m_insVCodec), "%s", m_cachedMetadata.videoCodec.c_str());
                        m_insProps[propCount].key = "VIDEO";
                        m_insProps[propCount].value = m_insVCodec;
                        propCount++;
                    }
                    if (!m_cachedMetadata.audioCodec.empty() && m_cachedMetadata.audioCodec != "Unknown") {
                        std::snprintf(m_insACodec, sizeof(m_insACodec), "%s", m_cachedMetadata.audioCodec.c_str());
                        m_insProps[propCount].key = "AUDIO";
                        m_insProps[propCount].value = m_insACodec;
                        propCount++;
                    }
                    std::snprintf(m_insSubs, sizeof(m_insSubs), "%s", m_cachedMetadata.hasSubtitles ? "YES" : "NO");
                    m_insProps[propCount].key = "SUBTITLES";
                    m_insProps[propCount].value = m_insSubs;
                    propCount++;
                } else {
                    params.ins_preview_badge = m_insExt[0] ? m_insExt : "MEDIA";
                }
            }

            params.ins_prop_count = propCount;
            for (int p = 0; p < propCount; ++p) {
                params.ins_props[p].key = m_insProps[p].key;
                params.ins_props[p].value = m_insProps[p].value;
            }
        }
    }

    evo_rmlui_update_browser(&params);
    evo_rmlui_render_browser(framebuffer, width, height);
}

} // namespace evo
