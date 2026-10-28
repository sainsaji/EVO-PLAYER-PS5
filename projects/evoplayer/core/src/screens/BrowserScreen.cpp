#include "evo/screens/ProviderHostScreen.hpp"
#include "evo/screens/BrowserScreen.hpp"
#include "evo_jailbreak.h"
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
#include "evo_smb_io.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <atomic>
#include <map>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "evo/services/NetworkStorageService.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

namespace evo {

static bool s_addServerChooserOpen = false;
static int s_addServerChooserFocused = 0;

/* Add-server flow (#226): address -> Guest/Account -> login test -> saved.
 * Each step is a modal or a keyboard prompt; the test runs on a worker. A prompt
 * is never opened from inside another prompt's callback: the callback sets
 * s_addPendingKb and update() opens it. */
enum AddKb { ADDKB_NONE = 0, ADDKB_ADDRESS, ADDKB_USER, ADDKB_PASS };
static NetworkShare s_addDraft;
static bool s_addSignInOpen = false;
static int s_addSignInFocused = 0;
static int s_addPendingKb = ADDKB_NONE;
static std::string s_addError;      // why the last test failed, shown on the next prompt
static bool s_addTesting = false;

struct TestJob {
    NetworkShare share;
    NetworkTestResult result;
    std::atomic<bool> done{false};
};
static std::shared_ptr<TestJob> s_testJob;      // UI thread only
static double s_testElapsedMs = 0.0;
static constexpr double kTestDeadlineMs = 15000.0;

static void* testEntry(void* arg) {
    auto* sp = static_cast<std::shared_ptr<TestJob>*>(arg);
    std::shared_ptr<TestJob> job = *sp;
    delete sp;
    job->result = NetworkStorageService::testConnection(job->share);
    job->done.store(true);
    return nullptr;
}

static std::string trimText(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

/* Fills s_addDraft from what was typed. Returns true when the text carried its
 * own login (user:pass@host), so the Guest/Account step is skipped. */
static bool parseAddAddress(const std::string& text) {
    const NetworkProtocol proto = s_addDraft.protocol;
    std::string t = trimText(text);
    NetworkShare d;
    d.protocol = proto;
    bool hasLogin = false;
    if (proto == NetworkProtocol::SMB) {
        if (!NetworkStorageService::parseSmbUrl(t, d)) return false;
        hasLogin = !d.user.empty() && d.user != "guest";
    } else {
        if (t.rfind("ftp://", 0) == 0) t = t.substr(6);
        std::string user = "anonymous", pass = "anonymous";
        size_t at = t.find('@');
        if (at != std::string::npos) {
            std::string auth = t.substr(0, at);
            t = t.substr(at + 1);
            size_t c = auth.find(':');
            user = c == std::string::npos ? auth : auth.substr(0, c);
            if (c != std::string::npos) pass = auth.substr(c + 1);
            hasLogin = true;
        }
        size_t slash = t.find('/');
        d.startPath = slash == std::string::npos ? "/" : t.substr(slash);
        if (slash != std::string::npos) t = t.substr(0, slash);
        d.host = t;
        d.port = 21;
        size_t colon = t.find(':');
        if (colon != std::string::npos) {
            d.host = t.substr(0, colon);
            d.port = std::atoi(t.substr(colon + 1).c_str());
            if (d.port <= 0 || d.port > 65535) d.port = 21;
        }
        if (d.host.empty()) return false;
        d.user = user;
        d.password = pass;
        d.name = d.host + (d.port != 21 ? ":" + std::to_string(d.port) : "");
    }
    s_addDraft = d;
    return hasLogin;
}

static void startAddTest() {
    auto job = std::make_shared<TestJob>();
    job->share = s_addDraft;
    auto* arg = new std::shared_ptr<TestJob>(job);
    pthread_attr_t attr;
    pthread_t th;
    int rc;
    if (pthread_attr_init(&attr) == 0) {
        pthread_attr_setstacksize(&attr, 1u << 18);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        rc = pthread_create(&th, &attr, testEntry, arg);
        pthread_attr_destroy(&attr);
    } else {
        rc = pthread_create(&th, nullptr, testEntry, arg);
        if (rc == 0) pthread_detach(th);
    }
    if (rc != 0) {
        delete arg;
        return;
    }
    s_testJob = job;
    s_testElapsedMs = 0.0;
    s_addTesting = true;
}

static void OnAddAddress(const char* text, void* userdata) {
    (void)userdata;
    if (!text || !trimText(text).size()) return;
    if (!parseAddAddress(text)) {
        s_addSignInOpen = true;
        s_addSignInFocused = 0;
    } else {
        startAddTest();
    }
}

static void OnAddUser(const char* text, void* userdata) {
    (void)userdata;
    s_addDraft.user = text ? trimText(text) : "";
    s_addPendingKb = ADDKB_PASS;
}

static void OnAddPass(const char* text, void* userdata) {
    (void)userdata;
    s_addDraft.password = text ? text : "";
    startAddTest();
}

/* "192.168.0.10:4450/Movies" - the draft as the person typed it, no login. */
static std::string draftAddressText() {
    std::string a = s_addDraft.host;
    const int def = s_addDraft.protocol == NetworkProtocol::SMB ? 445 : 21;
    if (s_addDraft.port > 0 && s_addDraft.port != def) a += ":" + std::to_string(s_addDraft.port);
    if (s_addDraft.startPath.size() > 1) a += s_addDraft.startPath;
    return a;
}

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

void BrowserScreen::handleAddSmbHost(const std::string& hostStr) {
    NetworkShare share;
    if (!NetworkStorageService::parseSmbUrl(hostStr, share)) {
        return;
    }
    share.id = "smb_" + std::to_string(time(nullptr));
    NetworkStorageService::getInstance().addShare(share);
    m_networkShareId.clear();
    m_networkRemotePath = "/";
    resetSelection();
    toast("SMB ADDED", share.name.c_str());
}

void BrowserScreen::handleAddFtpHost(const std::string& hostStr) {
    std::string trimmed = hostStr;
    size_t start = trimmed.find_first_not_of(" \t\r\n");
    if (start != std::string::npos) {
        size_t end = trimmed.find_last_not_of(" \t\r\n");
        trimmed = trimmed.substr(start, end - start + 1);
    } else {
        return;
    }
    if (trimmed.empty()) return;

    std::string host = trimmed;
    int port = 21;
    size_t colon = trimmed.find(':');
    if (colon != std::string::npos) {
        host = trimmed.substr(0, colon);
        port = std::atoi(trimmed.substr(colon + 1).c_str());
        if (port <= 0 || port > 65535) port = 21;
    }

    NetworkShare share;
    share.id = "ftp_" + std::to_string(time(nullptr));
    share.name = host + (port != 21 ? (":" + std::to_string(port)) : "");
    share.protocol = NetworkProtocol::FTP;
    share.host = host;
    share.port = port;
    share.user = "anonymous";
    share.password = "anonymous";
    share.startPath = "/";

    NetworkStorageService::getInstance().addShare(share);
    m_networkShareId.clear();
    m_networkRemotePath = "/";
    resetSelection();
    toast("FTP ADDED", share.name.c_str());
}

static void OnEditSmbSubmitted(const char* text, void* userdata) {
    auto* self = static_cast<BrowserScreen*>(userdata);
    if (!self || !text) return;
    self->handleEditSmbHost(text);
}

static void OnEditFtpSubmitted(const char* text, void* userdata) {
    auto* self = static_cast<BrowserScreen*>(userdata);
    if (!self || !text) return;
    self->handleEditFtpHost(text);
}

void BrowserScreen::openEditShareKeyboard(const std::string& shareId) {
    const auto* share = NetworkStorageService::getInstance().getShare(shareId);
    if (!share) return;

    m_editingShareId = shareId;
    std::string initial = NetworkStorageService::buildEditUrl(*share);
    if (share->protocol == NetworkProtocol::SMB) {
        evo_keyboard_open_phone("Edit SMB Share (user:pass@host/Share)...", initial.c_str(), 512, OnEditSmbSubmitted, this);
    } else {
        evo_keyboard_open_phone("Edit FTP Server (user:pass@host:port)...", initial.c_str(), 512, OnEditFtpSubmitted, this);
    }
}

void BrowserScreen::handleEditSmbHost(const std::string& hostStr) {
    const auto* orig = NetworkStorageService::getInstance().getShare(m_editingShareId);
    if (!orig) return;

    NetworkShare share = *orig;
    if (!NetworkStorageService::parseSmbUrl(hostStr, share)) {
        return;
    }
    share.id = orig->id;
    NetworkStorageService::getInstance().addShare(share);
    resetSelection();
    toast("SMB UPDATED", share.name.c_str());
    evo_feedback(EVO_FB_CONFIRM);
}

void BrowserScreen::handleEditFtpHost(const std::string& hostStr) {
    const auto* orig = NetworkStorageService::getInstance().getShare(m_editingShareId);
    if (!orig) return;

    std::string trimmed = hostStr;
    size_t start = trimmed.find_first_not_of(" \t\r\n");
    if (start != std::string::npos) {
        size_t end = trimmed.find_last_not_of(" \t\r\n");
        trimmed = trimmed.substr(start, end - start + 1);
    } else {
        return;
    }
    if (trimmed.empty()) return;

    if (trimmed.rfind("ftp://", 0) == 0) {
        trimmed = trimmed.substr(6);
    }

    std::string user = "anonymous";
    std::string pass = "anonymous";
    size_t at = trimmed.find('@');
    if (at != std::string::npos) {
        std::string auth = trimmed.substr(0, at);
        trimmed = trimmed.substr(at + 1);
        size_t colon = auth.find(':');
        if (colon != std::string::npos) {
            user = auth.substr(0, colon);
            pass = auth.substr(colon + 1);
        } else {
            user = auth;
        }
    }

    std::string host = trimmed;
    int port = 21;
    size_t colon = trimmed.find(':');
    if (colon != std::string::npos) {
        host = trimmed.substr(0, colon);
        port = std::atoi(trimmed.substr(colon + 1).c_str());
        if (port <= 0 || port > 65535) port = 21;
    }

    if (host.empty()) return;

    NetworkShare share = *orig;
    share.name = host + (port != 21 ? (":" + std::to_string(port)) : "");
    share.protocol = NetworkProtocol::FTP;
    share.host = host;
    share.port = port;
    share.user = user;
    share.password = pass;

    NetworkStorageService::getInstance().addShare(share);
    resetSelection();
    toast("FTP UPDATED", share.name.c_str());
    evo_feedback(EVO_FB_CONFIRM);
}

void BrowserScreen::openServerMenu(const std::string& shareId, const std::string& shareName) {
    m_serverMenuOpen = true;
    m_serverMenuFocused = 0;
    m_serverMenuShareId = shareId;
    m_serverMenuShareName = shareName;
    evo_feedback(EVO_FB_CONFIRM);
}

void BrowserScreen::performServerAction(int actionIndex) {
    m_serverMenuOpen = false;
    switch (actionIndex) {
        case 0: { // Open
            m_networkShareId = m_serverMenuShareId;
            m_networkRemotePath = "/";
            resetSelection();
            evo_feedback(EVO_FB_CONFIRM);
            break;
        }
        case 1: { // Edit Server
            evo_feedback(EVO_FB_OPEN);
            openEditShareKeyboard(m_serverMenuShareId);
            break;
        }
        case 2: { // Remove Server
            m_removeShareConfirmOpen = true;
            m_removeShareConfirmButton = 0; // Default to Cancel
            m_removeShareId = m_serverMenuShareId;
            m_removeShareName = m_serverMenuShareName;
            evo_feedback(EVO_FB_CONFIRM);
            break;
        }
        case 3: // Close
        default:
            evo_feedback(EVO_FB_CANCEL);
            break;
    }
}

static void OnRenameSubmitted(const char* text, void* userdata) {
    auto* self = static_cast<BrowserScreen*>(userdata);
    if (!self || !text) return;
    self->handleActionRename(text);
}

static void OnNewFolderSubmitted(const char* text, void* userdata) {
    auto* self = static_cast<BrowserScreen*>(userdata);
    if (!self || !text) return;
    self->handleActionNewFolder(text);
}

void BrowserScreen::handleActionRename(const std::string& newName) {
    std::string trimmed = newName;
    size_t start = trimmed.find_first_not_of(" \t\r\n");
    if (start != std::string::npos) {
        size_t end = trimmed.find_last_not_of(" \t\r\n");
        trimmed = trimmed.substr(start, end - start + 1);
    } else {
        return;
    }
    if (trimmed.empty() || trimmed == m_actionTargetName) return;

    auto transferService = Application::getInstance().getFileTransferService();
    if (transferService && !m_actionTargetPath.empty()) {
        bool ok = transferService->renameItem(m_actionTargetPath, trimmed);
        if (ok) {
            toast("RENAMED", trimmed.c_str());
            evo_feedback(EVO_FB_CONFIRM);
            auto browser = Application::getInstance().getFileSystemBrowser();
            if (browser) {
                browser->refresh();
                resetSelection();
            }
        } else {
            toast("ERROR", "Rename failed");
            evo_feedback(EVO_FB_ERROR);
        }
    }
}

void BrowserScreen::handleActionNewFolder(const std::string& folderName) {
    std::string trimmed = folderName;
    size_t start = trimmed.find_first_not_of(" \t\r\n");
    if (start != std::string::npos) {
        size_t end = trimmed.find_last_not_of(" \t\r\n");
        trimmed = trimmed.substr(start, end - start + 1);
    } else {
        return;
    }
    if (trimmed.empty()) return;

    auto browser = Application::getInstance().getFileSystemBrowser();
    std::string currentPath = browser ? browser->getCurrentPath() : "";
    if (currentPath.empty()) {
        toast("ERROR", "Invalid path");
        return;
    }

    auto transferService = Application::getInstance().getFileTransferService();
    if (transferService) {
        bool ok = transferService->createDirectory(currentPath, trimmed);
        if (ok) {
            toast("FOLDER CREATED", trimmed.c_str());
            evo_feedback(EVO_FB_CONFIRM);
            if (browser) {
                browser->refresh();
                resetSelection();
            }
        } else {
            toast("ERROR", "Could not create folder");
            evo_feedback(EVO_FB_ERROR);
        }
    }
}

void BrowserScreen::openActionMenu() {
    if (m_activeSource == 2 && m_networkShareId.empty()) {
        if (m_focusPane == BrowserFocusPane::Grid && m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_items.size())) {
            const auto& item = m_items[m_selectedIndex];
            if (item.fullPath.rfind("netshare://", 0) == 0 &&
                item.fullPath != "netshare://add" &&
                item.fullPath != "netshare://add_smb" &&
                item.fullPath != "netshare://add_server") {
                openServerMenu(item.fullPath.substr(11), item.name);
                return;
            }
        }
        return;
    }

    auto browser = Application::getInstance().getFileSystemBrowser();
    std::string currentPath = browser ? browser->getCurrentPath() : "";

    if (m_focusPane == BrowserFocusPane::Grid && m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_items.size())) {
        const auto& item = m_items[m_selectedIndex];
        m_actionTargetName = item.name;
        m_actionTargetPath = item.fullPath;
        m_actionTargetIsDir = (item.category == FileCategory::Folder);
    } else {
        if (!currentPath.empty()) {
            size_t slash = currentPath.find_last_of('/');
            if (slash != std::string::npos && slash + 1 < currentPath.size()) {
                m_actionTargetName = currentPath.substr(slash + 1);
            } else {
                m_actionTargetName = currentPath;
            }
            if (currentPath.rfind("smb://", 0) == 0) {
                m_actionTargetName = evo_url_decode_string(m_actionTargetName);
            }
            m_actionTargetPath = currentPath;
            m_actionTargetIsDir = true;
        } else {
            m_actionTargetName = "None";
            m_actionTargetPath.clear();
            m_actionTargetIsDir = false;
        }
    }

    auto transferService = Application::getInstance().getFileTransferService();
    if (transferService && transferService->hasClipboardItem() && (m_actionTargetIsDir || m_actionTargetPath.empty())) {
        m_actionMenuFocused = 2; // Default to Paste if clipboard has content
    } else {
        m_actionMenuFocused = 0; // Default to Copy
    }

    m_actionMenuOpen = true;
    evo_feedback(EVO_FB_CONFIRM);
}

void BrowserScreen::performAction(int actionIndex) {
    auto transferService = Application::getInstance().getFileTransferService();
    auto browser = Application::getInstance().getFileSystemBrowser();
    std::string currentPath = browser ? browser->getCurrentPath() : "";

    switch (actionIndex) {
        case 0: { // Copy
            if (!transferService || m_actionTargetPath.empty()) {
                toast("ERROR", "No file selected");
                evo_feedback(EVO_FB_BOUNDARY);
                return;
            }
            transferService->copyToClipboard(m_actionTargetPath, m_actionTargetName, m_actionTargetIsDir);
            m_actionMenuOpen = false;
            toast("CLIPBOARD", ("Copied: " + m_actionTargetName).c_str());
            evo_feedback(EVO_FB_CONFIRM);
            break;
        }
        case 1: { // Cut (Move)
            if (!transferService || m_actionTargetPath.empty()) {
                toast("ERROR", "No file selected");
                evo_feedback(EVO_FB_BOUNDARY);
                return;
            }
            transferService->cutToClipboard(m_actionTargetPath, m_actionTargetName, m_actionTargetIsDir);
            m_actionMenuOpen = false;
            toast("CLIPBOARD", ("Cut: " + m_actionTargetName).c_str());
            evo_feedback(EVO_FB_CONFIRM);
            break;
        }
        case 2: { // Paste
            if (!transferService || !transferService->hasClipboardItem()) {
                toast("PASTE", "Clipboard is empty");
                evo_feedback(EVO_FB_BOUNDARY);
                return;
            }
            std::string destDir = currentPath;
            if (destDir.empty()) {
                toast("ERROR", "Cannot paste at root");
                evo_feedback(EVO_FB_BOUNDARY);
                return;
            }
            m_actionMenuOpen = false;
            bool ok = transferService->startPaste(destDir);
            if (ok) {
                m_showTransferDialog = true;
                evo_feedback(EVO_FB_CONFIRM);
            } else {
                toast("ERROR", "Could not start paste");
                evo_feedback(EVO_FB_ERROR);
            }
            break;
        }
        case 3: { // Rename
            if (!transferService || m_actionTargetPath.empty()) {
                toast("ERROR", "No item to rename");
                evo_feedback(EVO_FB_BOUNDARY);
                return;
            }
            m_actionMenuOpen = false;
            evo_keyboard_open("Rename item...", m_actionTargetName.c_str(), 128, OnRenameSubmitted, this);
            break;
        }
        case 4: { // Delete
            if (!transferService || m_actionTargetPath.empty()) {
                toast("ERROR", "No item to delete");
                evo_feedback(EVO_FB_BOUNDARY);
                return;
            }
            m_actionMenuOpen = false;
            m_showDeleteConfirm = true;
            m_deleteConfirmButton = 0; // Default to Cancel for safety
            evo_feedback(EVO_FB_CONFIRM);
            break;
        }
        case 5: { // New Folder
            if (currentPath.empty()) {
                toast("ERROR", "Cannot create folder here");
                evo_feedback(EVO_FB_BOUNDARY);
                return;
            }
            m_actionMenuOpen = false;
            evo_keyboard_open("New folder name...", "New Folder", 64, OnNewFolderSubmitted, this);
            break;
        }
        case 6: // Cancel
        default: {
            m_actionMenuOpen = false;
            evo_feedback(EVO_FB_CANCEL);
            break;
        }
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

/* Hard overall deadline for one folder listing (connect, retry, list). */
static constexpr double kNetListDeadlineMs = 12000.0;

struct NetListJob {
    std::string shareId;
    std::string path;
    std::vector<BrowserEntry> entries;
    std::string error;
    bool ok = false;
    bool launched = false;           // UI thread only
    std::atomic<bool> done{false};
};

/* The service owns one FTP and one SMB client, so only one listing may run.
 * A request abandoned by timeout keeps running to its own bound; the next
 * one waits for it. */
static std::atomic<bool> s_netInFlight{false};

static void* netListEntry(void* arg) {
    auto* sp = static_cast<std::shared_ptr<NetListJob>*>(arg);
    std::shared_ptr<NetListJob> job = *sp;
    delete sp;
    job->ok = NetworkStorageService::getInstance().listEntries(job->shareId, job->path, job->entries, job->error);
    job->done.store(true);
    s_netInFlight.store(false);
    return nullptr;
}

void BrowserScreen::startNetJob() {
    m_netJob = std::make_shared<NetListJob>();
    m_netJob->shareId = m_networkShareId;
    m_netJob->path = m_networkRemotePath;
    pollNetList(0.0);   // launches now when the worker is free
}

void BrowserScreen::pollNetList(double deltaMs) {
    if (m_netState != NetListState::Loading || !m_netJob) return;
    m_netElapsedMs += deltaMs;

    if (!m_netJob->launched && !s_netInFlight.load()) {
        m_netJob->launched = true;
        s_netInFlight.store(true);
        auto* arg = new std::shared_ptr<NetListJob>(m_netJob);
        pthread_attr_t attr;
        pthread_t th;
        int rc;
        if (pthread_attr_init(&attr) == 0) {
            pthread_attr_setstacksize(&attr, 1u << 20);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            rc = pthread_create(&th, &attr, netListEntry, arg);
            pthread_attr_destroy(&attr);
        } else {
            rc = pthread_create(&th, nullptr, netListEntry, arg);
            if (rc == 0) pthread_detach(th);
        }
        if (rc != 0) {
            delete arg;
            s_netInFlight.store(false);
            m_netJob->error = "Could not start the request";
            m_netJob->done.store(true);
        }
    }

    if (m_netJob->done.load()) {
        m_netEntries = std::move(m_netJob->entries);
        m_netError = m_netJob->error;
        m_netState = m_netJob->ok ? NetListState::Ready : NetListState::Failed;
        m_netJob.reset();
        rebuildItems();
    } else if (m_netElapsedMs > kNetListDeadlineMs) {
        m_netError = "Timed out waiting for the server";
        m_netState = NetListState::Failed;
        m_netJob.reset();   // the worker finishes on its own; its result is dropped
        rebuildItems();
    }
}

/* Server status chips (#226). The probe is a short TCP connect on a worker,
 * one at a time. It does not take s_netInFlight: it touches no SMB/FTP client,
 * and a connect that hangs must not hold up a folder listing. Results are
 * cached for the session; an Offline server is looked at again after
 * kOfflineRecheckMs. */
static constexpr int kProbeConnectMs = 2500;
static constexpr double kProbeDeadlineMs = 5000.0;
static constexpr double kOfflineRecheckMs = 30000.0;

struct ProbeJob {
    std::string key;
    std::string host;
    int port = 0;
    int ms = -1;
    std::atomic<bool> done{false};
};

struct ServerStatus {
    int ms = -1;          // -1 = offline
    double at = 0.0;
};

static std::map<std::string, ServerStatus> s_serverStatus;
static std::shared_ptr<ProbeJob> s_probeJob;    // UI thread only
static double s_probeElapsedMs = 0.0;
static double s_statusClockMs = 0.0;

static std::string serverStatusKey(const NetworkShare& s) {
    return s.id + "|" + s.host + ":" + std::to_string(s.port);
}

static void* probeEntry(void* arg) {
    auto* sp = static_cast<std::shared_ptr<ProbeJob>*>(arg);
    std::shared_ptr<ProbeJob> job = *sp;
    delete sp;
    job->ms = NetworkStorageService::probeReachable(job->host, job->port, kProbeConnectMs);
    job->done.store(true);
    return nullptr;
}

void BrowserScreen::pollServerStatus(double deltaMs) {
    s_statusClockMs += deltaMs;

    if (s_probeJob) {
        s_probeElapsedMs += deltaMs;
        if (s_probeJob->done.load()) {
            s_serverStatus[s_probeJob->key] = {s_probeJob->ms, s_statusClockMs};
            s_probeJob.reset();
            rebuildItems();
        } else if (s_probeElapsedMs > kProbeDeadlineMs) {
            s_serverStatus[s_probeJob->key] = {-1, s_statusClockMs};
            s_probeJob.reset();   // the worker finishes on its own; its result is dropped
            rebuildItems();
        }
        return;
    }

    for (const auto& share : NetworkStorageService::getInstance().getShares()) {
        const std::string key = serverStatusKey(share);
        auto it = s_serverStatus.find(key);
        if (it != s_serverStatus.end() &&
            (it->second.ms >= 0 || s_statusClockMs - it->second.at < kOfflineRecheckMs)) {
            continue;
        }
        auto job = std::make_shared<ProbeJob>();
        job->key = key;
        job->host = share.host;
        job->port = share.port;
        auto* arg = new std::shared_ptr<ProbeJob>(job);
        pthread_attr_t attr;
        pthread_t th;
        int rc;
        if (pthread_attr_init(&attr) == 0) {
            pthread_attr_setstacksize(&attr, 1u << 18);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            rc = pthread_create(&th, &attr, probeEntry, arg);
            pthread_attr_destroy(&attr);
        } else {
            rc = pthread_create(&th, nullptr, probeEntry, arg);
            if (rc == 0) pthread_detach(th);
        }
        if (rc != 0) {
            delete arg;
            s_serverStatus[key] = {-1, s_statusClockMs};
        } else {
            s_probeJob = job;
            s_probeElapsedMs = 0.0;
        }
        return;
    }
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
                item.iconPath = (!m_isSearching && browser->getCurrentPath() == browser->usbHome())
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
        // Network Storage (FTP / SMB)
        m_networkEmptyTitle.clear();
        m_networkEmptyHint.clear();

        if (m_networkShareId.empty()) {
            // Put "+ Add Server" at the top of the network root list
            if (!m_isSearching) {
                BrowserItem addServerItem;
                addServerItem.name = "+ Add Server";
                addServerItem.fullPath = "netshare://add_server";
                addServerItem.category = FileCategory::Folder;
                addServerItem.badge = "SETUP";
                addServerItem.iconPath = "../icons/icon_settings.png";
                addServerItem.detail = "SMB or FTP";
                m_items.push_back(std::move(addServerItem));
            }

            // Show list of configured Network Shares
            const auto& shares = NetworkStorageService::getInstance().getShares();
            for (const auto& share : shares) {
                if (m_isSearching && !matchesQuery(share.name, m_searchQuery) && !matchesQuery(share.host, m_searchQuery)) {
                    continue;
                }

                BrowserItem item;
                item.name = share.name;
                item.fullPath = "netshare://" + share.id;
                item.category = FileCategory::Folder;
                item.badge = (share.protocol == NetworkProtocol::SMB) ? "SMB" : "FTP";
                item.iconPath = "../icons/icon_folder.png";
                item.detail = share.host + ":" + std::to_string(share.port);
                auto st = s_serverStatus.find(serverStatusKey(share));
                if (st == s_serverStatus.end()) {
                    item.statusKind = 1;
                    item.statusText = "Checking";
                } else if (st->second.ms >= 0) {
                    item.statusKind = 2;
                    item.statusText = "Online  " + std::to_string(st->second.ms) + " ms";
                } else {
                    item.statusKind = 3;
                    item.statusText = "Offline";
                }
                m_items.push_back(std::move(item));
            }
        } else {
            // Browse within the selected share. The listing comes from the
            // worker; this only reads what it has delivered for this folder.
            const std::string key = m_networkShareId + "\n" + m_networkRemotePath;
            if (m_netState == NetListState::Idle || m_netKey != key) {
                m_netKey = key;
                m_netState = NetListState::Loading;
                m_netElapsedMs = 0.0;
                m_netEntries.clear();
                m_netError.clear();
                startNetJob();
            }
            if (m_netState == NetListState::Failed) {
                const auto* share = NetworkStorageService::getInstance().getShare(m_networkShareId);
                const char* protoName = (share && share->protocol == NetworkProtocol::SMB) ? "SMB" : "FTP";
                m_networkEmptyTitle = "COULD NOT CONNECT";
                m_networkEmptyHint = m_netError.empty() ? (std::string(protoName) + " server not responding") : m_netError;
            } else if (m_netState == NetListState::Ready) {
                if (m_netEntries.empty()) {
                    m_networkEmptyTitle = "FOLDER IS EMPTY";
                    m_networkEmptyHint = "Nothing here";
                }
                for (const auto& entry : m_netEntries) {
                    if (m_isSearching && !matchesQuery(entry.name, m_searchQuery)) {
                        continue;
                    }

                    BrowserItem item;
                    item.name = entry.name;
                    item.fullPath = entry.fullPath;
                    item.category = entry.category;
                    item.badge = (browser && entry.category != FileCategory::Folder)
                        ? browser->getFileCategoryLabel(entry.category) : "DIR";
                    item.isFavorite = favorites_is_favorite(item.fullPath.c_str());

                    double pos = recent_lookup(item.fullPath.c_str());
                    item.lastPos = pos;
                    if (pos > 0.0 && m_cachedMetadata.filePath == item.fullPath && m_cachedMetadata.durationSeconds > 0.0) {
                        item.duration = m_cachedMetadata.durationSeconds;
                        item.progress = static_cast<int>((pos / m_cachedMetadata.durationSeconds) * 100.0);
                        if (item.progress > 100) item.progress = 100;
                    }

                    if (entry.category == FileCategory::Folder) {
                        item.iconPath = "../icons/icon_folder.png";
                        item.detail = "Folder";
                    } else {
                        if (entry.category == FileCategory::Video) item.iconPath = "../icons/icon_resume.png";
                        else if (entry.category == FileCategory::Audio) item.iconPath = "../icons/icon_subtitles.png";
                        else if (entry.category == FileCategory::Image) item.iconPath = "../icons/icon_palette.png";
                        else item.iconPath = "../icons/icon_about_support.png";
                        item.detail = "Network Media";
                    }
                    m_items.push_back(std::move(item));
                }
            }
        }
    } else if (m_activeSource == 3) {
        // Favorites
        favorites_load();
        for (int i = 0; i < favorite_count; ++i) {
            const auto& fav = favorite_files[i];
            if (fav.path[0] == '\0') continue;

            FileCategory cat = browser ? browser->classifyFile(fav.path, 0) : FileCategory::Video;

            std::string displayName = fav.title[0] ? fav.title : "";
            if (displayName.empty()) {
                std::string p(fav.path);
                size_t slash = p.find_last_of('/');
                displayName = (slash != std::string::npos) ? p.substr(slash + 1) : p;
                if (p.rfind("smb://", 0) == 0) {
                    displayName = evo_url_decode_string(displayName);
                }
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
    } else if (m_activeSource == 4) {
        // Recent Media
        recent_load();
        for (int i = 0; i < recent_file_count; ++i) {
            const auto& rec = recent_files[i];
            if (rec.path[0] == '\0') continue;

            FileCategory cat = browser ? browser->classifyFile(rec.path, 0) : FileCategory::Video;

            std::string displayName = rec.title[0] ? rec.title : "";
            if (displayName.empty()) {
                std::string p(rec.path);
                size_t slash = p.find_last_of('/');
                displayName = (slash != std::string::npos) ? p.substr(slash + 1) : p;
                if (p.rfind("smb://", 0) == 0) {
                    displayName = evo_url_decode_string(displayName);
                }
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

    applyFolderFilter();
}

std::string BrowserScreen::filterScopeKey() const {
    /* Real scopes are absolute paths, so a leading ':' cannot collide. */
    if (m_activeSource == 3) return ":favorites";
    if (m_activeSource == 4) return ":recent";
    if (m_activeSource == 2) return ":network:" + m_networkShareId + ":" + m_networkRemotePath;
    auto browser = Application::getInstance().getFileSystemBrowser();
    return browser ? browser->getCurrentPath() : std::string();
}

/*
 * Work out which chips this folder gets, and narrow the listing to the chosen
 * one.
 *
 * The chips are built from what is actually here, so choosing one can never
 * produce an empty grid, and the row is dropped entirely when there is nothing
 * to choose between. The choice belongs to the folder: walk into another one
 * and it resets. That is what keeps it from becoming a mode - the old
 * sidebar filter could not be cleared by re-picking a source, so it followed
 * you around until you switched to a different drive.
 *
 * Folders are never filtered out. Hiding them would make a filtered folder a
 * dead end you could not walk out of.
 */
void BrowserScreen::applyFolderFilter() {
    const std::string scope = filterScopeKey();
    if (scope != m_filterScope) {
        m_filterScope = scope;
        m_filterIndex = 0;
        if (m_focusPane == BrowserFocusPane::Filter)
            m_focusPane = BrowserFocusPane::Grid;
    }

    m_folderCats.clear();
    for (const auto& it : m_items) {
        if (it.category != FileCategory::Video &&
            it.category != FileCategory::Audio &&
            it.category != FileCategory::Image)
            continue;
        const int c = static_cast<int>(it.category);
        if (std::find(m_folderCats.begin(), m_folderCats.end(), c) == m_folderCats.end())
            m_folderCats.push_back(c);
    }
    std::sort(m_folderCats.begin(), m_folderCats.end());

    if (m_folderCats.size() < 2) {
        m_folderCats.clear();          // one kind of file, or none: no row
        m_filterIndex = 0;
        if (m_focusPane == BrowserFocusPane::Filter)
            m_focusPane = BrowserFocusPane::Grid;
    }
    if (m_filterIndex > static_cast<int>(m_folderCats.size()))
        m_filterIndex = 0;

    m_categoryFilter = (m_filterIndex > 0) ? m_folderCats[m_filterIndex - 1] : -1;
    if (m_categoryFilter < 0)
        return;

    std::vector<BrowserItem> kept;
    kept.reserve(m_items.size());
    for (auto& it : m_items) {
        if (it.category == FileCategory::Folder ||
            static_cast<int>(it.category) == m_categoryFilter)
            kept.push_back(std::move(it));
    }
    m_items.swap(kept);
}

void BrowserScreen::applyFilterChange() {
    evo_feedback(EVO_FB_MOVE);
    m_selectedIndex = 0;
    m_scrollOffset = 0;
    m_settleMs = 0.0;
    m_cachedMetadata = MediaMetadataInfo();
    rebuildItems();
    m_browserFsm.postEvent(BrowserScreenEvent::CursorMoved);
}

void BrowserScreen::resetSelection() {
    m_netState = NetListState::Idle;   // moving or retrying: list the folder again
    m_selectedIndex = 0;
    m_scrollOffset = 0;
    m_settleMs = 0.0;
    m_cachedMetadata = MediaMetadataInfo();
    rebuildItems();
    m_browserFsm.postEvent(BrowserScreenEvent::FinishSearch);
    m_browserFsm.postEvent(BrowserScreenEvent::CursorMoved);
}

void BrowserScreen::setSource(int sourceIndex) {
    /* Sources only now - the category filters moved out of this panel and
     * into the folder header, where they are chosen per folder. */
    if (sourceIndex < 0 || sourceIndex > 4) sourceIndex = 0;
    auto browser = Application::getInstance().getFileSystemBrowser();
    if (sourceIndex == 0 && browser && browser->usbMounts().empty()) {
        if (!NetworkStorageService::getInstance().getShares().empty()) {
            sourceIndex = 2;
        } else {
            sourceIndex = 1;
        }
    }
    m_sidebarIndex = sourceIndex;
    const bool focusGrid = evo_jailbreak_is_open() || (sourceIndex == 2 || sourceIndex == 3 || sourceIndex == 4);
    activateSidebar(focusGrid);
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
        const std::vector<std::string> mounts = browser->usbMounts();
        if (m_activeSource <= 1) {
            if (mounts.empty()) {
                // No USB drive plugged in: fallback to Network Storage if shares exist, else Internal Storage
                if (!NetworkStorageService::getInstance().getShares().empty()) {
                    m_activeSource = 2;
                    m_sidebarIndex = 2;
                } else {
                    m_activeSource = 1;
                    m_sidebarIndex = 1;
                    browser->navigateToSource(1);
                }
            } else {
                if (browser->getCurrentPath().empty()) {
                    browser->loadLastFolder();
                    if (browser->getCurrentPath().empty()) {
                        browser->setCurrentPath(browser->usbHome());
                    }
                }
                /* Never open on an empty pane: a restored folder that has since been
                 * deleted or emptied still resolves, so fall back to the USB root
                 * rather than showing nothing. */
                if (evo_jailbreak_is_open() && browser->getEntryCount() == 0 && !m_isSearching) {
                    browser->navigateToSource(0);
                }
                const std::string& path = browser->getCurrentPath();
                if (path.rfind("/data", 0) == 0) {
                    m_activeSource = 1;
                    m_sidebarIndex = (m_categoryFilter != -1) ? (m_categoryFilter + 3) : 1;
                } else {
                    m_activeSource = 0;
                    m_sidebarIndex = (m_categoryFilter != -1) ? (m_categoryFilter + 3) : 0;
                }
            }
        } else {
            m_sidebarIndex = (m_categoryFilter != -1) ? (m_categoryFilter + 3) : m_activeSource;
        }
    }
    resetSelection();
    if (!m_returnFocusPath.empty()) {
        for (size_t i = 0; i < m_items.size(); ++i) {
            if (m_items[i].fullPath == m_returnFocusPath) {
                m_selectedIndex = static_cast<int>(i);
                break;
            }
        }
        m_returnFocusPath.clear();
    }
    if (m_items.empty()) {
        m_focusPane = BrowserFocusPane::Sidebar;
    } else if (!evo_jailbreak_is_open() && (m_activeSource == 0 || m_activeSource == 1)) {
        m_focusPane = BrowserFocusPane::Sidebar;
    }

    m_actionMenuOpen = false;
    m_showDeleteConfirm = false;
    m_serverMenuOpen = false;
    s_addServerChooserOpen = false;
    m_removeShareConfirmOpen = false;
    auto transferService = Application::getInstance().getFileTransferService();
    if (transferService && transferService->isBusy()) {
        m_showTransferDialog = true;
    } else {
        m_showTransferDialog = false;
    }
}

void BrowserScreen::onExit() {
    StatefulScreen::onExit();
    m_actionMenuOpen = false;
    m_showDeleteConfirm = false;
    m_serverMenuOpen = false;
    s_addServerChooserOpen = false;
    s_addSignInOpen = false;
    s_addTesting = false;
    s_testJob.reset();
    s_addPendingKb = ADDKB_NONE;
    m_removeShareConfirmOpen = false;
    m_showTransferDialog = false;
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

    switch (m_sidebarIndex) {
        case 0: // USB Drive
            if (m_activeSource != 0 || (browser && !browser->isUsbPath(browser->getCurrentPath()))) {
                m_activeSource = 0;
                m_isSearching = false;
                m_searchQuery.clear();
                if (browser) {
                    browser->clearSearch();
                    browser->setCurrentPath(browser->usbHome());
                }
                resetSelection();
                if (focusGrid) toast("STORAGE", "USB Drive");
            }
            break;
        case 1: // Internal Storage
            if (m_activeSource != 1 || (browser && browser->getCurrentPath().rfind("/data", 0) != 0)) {
                m_activeSource = 1;
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
        case 2: // Network Storage (FTP / SMB)
            if (m_activeSource != 2 || !m_networkShareId.empty() || m_items.empty()) {
                m_activeSource = 2;
                m_networkShareId.clear();
                m_networkRemotePath = "/";
                m_isSearching = false;
                m_searchQuery.clear();
                resetSelection();
                if (focusGrid) toast("STORAGE", "Network Storage");
            }
            break;
        case 3: // Favorites
            if (m_activeSource != 3) {
                m_activeSource = 3;
                m_isSearching = false;
                m_searchQuery.clear();
                resetSelection();
                if (focusGrid) toast("SOURCES", "Favorites");
            }
            break;
        case 4: // Recent Media
            if (m_activeSource != 4) {
                m_activeSource = 4;
                m_isSearching = false;
                m_searchQuery.clear();
                resetSelection();
                if (focusGrid) toast("SOURCES", "Recent Media");
            }
            break;
        default:
            break;
    }

    if (focusGrid && !m_items.empty()) {
        evo_feedback(EVO_FB_CONFIRM);
        m_focusPane = BrowserFocusPane::Grid;
    } else {
        evo_feedback(EVO_FB_MOVE);
        m_focusPane = BrowserFocusPane::Sidebar;
    }
}

void BrowserScreen::activateSelection() {
    if (m_activeSource == 2 && !m_networkShareId.empty() && m_netState == NetListState::Failed) {
        evo_feedback(EVO_FB_CONFIRM);   // one retry per press, never automatic
        resetSelection();
        return;
    }
    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_items.size())) {
        return;
    }

    const auto& item = m_items[m_selectedIndex];
    if (item.category == FileCategory::Video || item.category == FileCategory::Audio)
        m_returnFocusPath = item.fullPath;
    auto browser = Application::getInstance().getFileSystemBrowser();
    auto playback = Application::getInstance().getPlaybackController();
    auto screenMgr = Application::getInstance().getScreenManager();
    if (!playback || !screenMgr) return;

    if (item.category == FileCategory::Folder) {
        if (m_activeSource == 2) {
            if (m_networkShareId.empty()) {
                if (item.fullPath == "netshare://add_server" ||
                    item.fullPath == "netshare://add" ||
                    item.fullPath == "netshare://add_smb") {
                    s_addServerChooserOpen = true;
                    s_addServerChooserFocused = 0;
                    evo_feedback(EVO_FB_CONFIRM);
                    return;
                }
                if (item.fullPath.rfind("netshare://", 0) == 0) {
                    evo_feedback(EVO_FB_OPEN);
                    m_networkShareId = item.fullPath.substr(11);
                    m_networkRemotePath = "/";
                    resetSelection();
                    return;
                }
            } else {
                evo_feedback(EVO_FB_OPEN);
                if (m_networkRemotePath == "/" || m_networkRemotePath.empty()) {
                    m_networkRemotePath = "/" + item.name;
                } else {
                    m_networkRemotePath = m_networkRemotePath + "/" + item.name;
                }
                resetSelection();
                return;
            }
        }
        if (browser) {
            evo_feedback(EVO_FB_OPEN);
            browser->navigateInto(item.name);
            resetSelection();
        }
    } else if (m_activeSource == 4 &&
               item.fullPath.compare(0, 6, "evo://") == 0) {
        evo_feedback(EVO_FB_CONFIRM);
        ProviderHostScreen::launchRecent(
            item.fullPath.c_str(), item.name.c_str(), item.lastPos);
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
            if (!playback->startPlayback(item.fullPath, 0.0)) return;   // toast already shown
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

    // 0. Active Transfer Dialog - intercept all input
    if (m_showTransferDialog) {
        if (pressed & (PadButtons::Circle | PadButtons::Cross)) {
            auto ts = Application::getInstance().getFileTransferService();
            if (ts) {
                ts->cancelTransfer();
                toast("TRANSFER", "Cancelling...");
            }
        }
        return true;
    }

    // 0a0. Add Server Chooser Modal
    if (s_addServerChooserOpen) {
        if (pressed & PadButtons::Up) {
            if (s_addServerChooserFocused > 0) s_addServerChooserFocused--;
            else s_addServerChooserFocused = 1;
            evo_feedback(EVO_FB_MOVE);
            return true;
        }
        if (pressed & PadButtons::Down) {
            if (s_addServerChooserFocused < 1) s_addServerChooserFocused++;
            else s_addServerChooserFocused = 0;
            evo_feedback(EVO_FB_MOVE);
            return true;
        }
        if (pressed & (PadButtons::Circle | PadButtons::Triangle | PadButtons::Options)) {
            s_addServerChooserOpen = false;
            evo_feedback(EVO_FB_CANCEL);
            return true;
        }
        if (pressed & PadButtons::Cross) {
            s_addServerChooserOpen = false;
            s_addDraft = NetworkShare();
            s_addDraft.host.clear();
            s_addDraft.port = 0;
            s_addError.clear();
            s_addDraft.protocol = s_addServerChooserFocused == 0 ? NetworkProtocol::SMB : NetworkProtocol::FTP;
            evo_feedback(EVO_FB_OPEN);
            s_addPendingKb = ADDKB_ADDRESS;
            return true;
        }
        return true;
    }

    // 0a0b. Add-server test running: Circle cancels
    if (s_addTesting) {
        if (pressed & (PadButtons::Circle | PadButtons::Options)) {
            s_addTesting = false;
            s_testJob.reset();   // the worker finishes on its own; its result is dropped
            evo_feedback(EVO_FB_CANCEL);
        }
        return true;
    }

    // 0a0c. Add-server sign-in: Guest or Account
    if (s_addSignInOpen) {
        if (pressed & (PadButtons::Up | PadButtons::Down)) {
            s_addSignInFocused = s_addSignInFocused ? 0 : 1;
            evo_feedback(EVO_FB_MOVE);
            return true;
        }
        if (pressed & (PadButtons::Circle | PadButtons::Triangle | PadButtons::Options)) {
            s_addSignInOpen = false;
            s_addError.clear();
            s_addPendingKb = ADDKB_ADDRESS;   // back one step
            evo_feedback(EVO_FB_CANCEL);
            return true;
        }
        if (pressed & PadButtons::Cross) {
            s_addSignInOpen = false;
            s_addError.clear();
            evo_feedback(EVO_FB_OPEN);
            if (s_addSignInFocused == 0) {
                const bool smb = s_addDraft.protocol == NetworkProtocol::SMB;
                s_addDraft.user = smb ? "guest" : "anonymous";
                s_addDraft.password = smb ? "" : "anonymous";
                startAddTest();
            } else {
                s_addPendingKb = ADDKB_USER;
            }
            return true;
        }
        return true;
    }

    // 0a1. Remove Server Confirmation Modal
    if (m_removeShareConfirmOpen) {
        if (pressed & (PadButtons::Left | PadButtons::Right)) {
            m_removeShareConfirmButton = (m_removeShareConfirmButton == 0) ? 1 : 0;
            evo_feedback(EVO_FB_MOVE);
            return true;
        }
        if (pressed & (PadButtons::Circle | PadButtons::Options)) {
            m_removeShareConfirmOpen = false;
            evo_feedback(EVO_FB_CANCEL);
            return true;
        }
        if (pressed & PadButtons::Cross) {
            m_removeShareConfirmOpen = false;
            if (m_removeShareConfirmButton == 1) { // Confirm Remove
                NetworkStorageService::getInstance().removeShare(m_removeShareId);
                toast("SERVER REMOVED", m_removeShareName.c_str());
                evo_feedback(EVO_FB_CONFIRM);
                rebuildItems();
                if (m_selectedIndex >= static_cast<int>(m_items.size()) && m_selectedIndex > 0) {
                    m_selectedIndex = static_cast<int>(m_items.size()) - 1;
                }
            } else {
                evo_feedback(EVO_FB_CANCEL);
            }
            return true;
        }
        return true;
    }

    // 0a2. Server Options Menu Modal
    if (m_serverMenuOpen) {
        if (pressed & PadButtons::Up) {
            if (m_serverMenuFocused > 0) m_serverMenuFocused--;
            else m_serverMenuFocused = 3;
            evo_feedback(EVO_FB_MOVE);
            return true;
        }
        if (pressed & PadButtons::Down) {
            if (m_serverMenuFocused < 3) m_serverMenuFocused++;
            else m_serverMenuFocused = 0;
            evo_feedback(EVO_FB_MOVE);
            return true;
        }
        if (pressed & (PadButtons::Circle | PadButtons::Options)) {
            m_serverMenuOpen = false;
            evo_feedback(EVO_FB_CANCEL);
            return true;
        }
        if (pressed & PadButtons::Cross) {
            performServerAction(m_serverMenuFocused);
            return true;
        }
        return true;
    }

    // 0b. Delete Confirmation Modal
    if (m_showDeleteConfirm) {
        if (pressed & (PadButtons::Left | PadButtons::Right)) {
            m_deleteConfirmButton = (m_deleteConfirmButton == 0) ? 1 : 0;
            evo_feedback(EVO_FB_MOVE);
            return true;
        }
        if (pressed & (PadButtons::Circle | PadButtons::Options)) {
            m_showDeleteConfirm = false;
            evo_feedback(EVO_FB_CANCEL);
            return true;
        }
        if (pressed & PadButtons::Cross) {
            if (m_deleteConfirmButton == 0) { // Cancel
                m_showDeleteConfirm = false;
                evo_feedback(EVO_FB_CANCEL);
            } else { // Confirm Delete
                m_showDeleteConfirm = false;
                auto ts = Application::getInstance().getFileTransferService();
                if (ts && !m_actionTargetPath.empty()) {
                    bool ok = ts->startDelete(m_actionTargetPath, m_actionTargetIsDir);
                    if (ok) {
                        m_showTransferDialog = true;
                        evo_feedback(EVO_FB_CONFIRM);
                    } else {
                        toast("ERROR", "Could not start delete");
                        evo_feedback(EVO_FB_ERROR);
                    }
                }
            }
            return true;
        }
        return true;
    }

    // 0c. File Operations Action Menu Modal
    if (m_actionMenuOpen) {
        if (pressed & PadButtons::Up) {
            if (m_actionMenuFocused > 0) m_actionMenuFocused--;
            else m_actionMenuFocused = 6;
            evo_feedback(EVO_FB_MOVE);
            return true;
        }
        if (pressed & PadButtons::Down) {
            if (m_actionMenuFocused < 6) m_actionMenuFocused++;
            else m_actionMenuFocused = 0;
            evo_feedback(EVO_FB_MOVE);
            return true;
        }
        if (pressed & (PadButtons::Circle | PadButtons::Options)) {
            m_actionMenuOpen = false;
            evo_feedback(EVO_FB_CANCEL);
            return true;
        }
        if (pressed & PadButtons::Cross) {
            performAction(m_actionMenuFocused);
            return true;
        }
        return true;
    }

    // Options button toggles action menu
    if (pressed & PadButtons::Options) {
        openActionMenu();
        return true;
    }

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
            if (m_sidebarIndex < 4) {
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

    // Folder filter chips, reached with Up from the top row of the grid
    if (m_focusPane == BrowserFocusPane::Filter) {
        const int last = static_cast<int>(m_folderCats.size());
        if (pressed & PadButtons::Left) {
            if (m_filterIndex > 0) { m_filterIndex--; applyFilterChange(); }
            else evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        if (pressed & PadButtons::Right) {
            if (m_filterIndex < last) { m_filterIndex++; applyFilterChange(); }
            else evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        if (pressed & (PadButtons::Down | PadButtons::Cross)) {
            evo_feedback(EVO_FB_CONFIRM);
            m_focusPane = BrowserFocusPane::Grid;
            return true;
        }
        if (pressed & PadButtons::Circle) {
            evo_feedback(EVO_FB_CANCEL);
            m_focusPane = BrowserFocusPane::Grid;
            return true;
        }
        if (pressed & PadButtons::Up) {
            evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        return false;
    }

    // Media Grid navigation (4 columns)
    if (pressed & PadButtons::Left) {
        if (m_items.empty() || m_selectedIndex % 4 == 0) {
            m_focusPane = BrowserFocusPane::Sidebar;
            m_sidebarIndex = m_activeSource;
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
        if (m_activeSource == 2 && m_networkShareId.empty()) {
            if (m_selectedIndex == 0) {
                evo_feedback(EVO_FB_BOUNDARY);
                return true;
            }
            if (m_selectedIndex < 4) {
                m_selectedIndex = 0;
                m_settleMs = 0.0;
                m_cachedMetadata = MediaMetadataInfo();
                m_browserFsm.postEvent(BrowserScreenEvent::CursorMoved);
                evo_feedback(EVO_FB_MOVE);
                return true;
            }
        } else {
            /* From the top row, Up reaches the folder's filter chips. Everywhere
             * else it is an ordinary row move. */
            if (m_selectedIndex < 4 && !m_folderCats.empty()) {
                m_focusPane = BrowserFocusPane::Filter;
                evo_feedback(EVO_FB_MOVE);
                return true;
            }
        }
        navigate(-4);
        return true;
    }
    if (pressed & PadButtons::Down) {
        if (m_activeSource == 2 && m_networkShareId.empty() && m_selectedIndex == 0) {
            if (m_items.size() > 1) {
                m_selectedIndex = 1;
                m_settleMs = 0.0;
                m_cachedMetadata = MediaMetadataInfo();
                m_browserFsm.postEvent(BrowserScreenEvent::CursorMoved);
                evo_feedback(EVO_FB_MOVE);
            } else {
                evo_feedback(EVO_FB_BOUNDARY);
            }
            return true;
        }
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

        if (m_activeSource == 2) {
            if (!m_networkShareId.empty()) {
                if (m_networkRemotePath != "/" && !m_networkRemotePath.empty()) {
                    size_t slash = m_networkRemotePath.find_last_of('/');
                    if (slash != std::string::npos && slash > 0) {
                        m_networkRemotePath = m_networkRemotePath.substr(0, slash);
                    } else {
                        m_networkRemotePath = "/";
                    }
                    resetSelection();
                    evo_feedback(EVO_FB_CANCEL);
                    return true;
                } else {
                    std::string exitedShareId = m_networkShareId;
                    m_networkShareId.clear();
                    m_networkRemotePath = "/";
                    resetSelection();
                    if (!exitedShareId.empty()) {
                        std::string targetPath = "netshare://" + exitedShareId;
                        for (size_t i = 0; i < m_items.size(); ++i) {
                            if (m_items[i].fullPath == targetPath) {
                                m_selectedIndex = static_cast<int>(i);
                                break;
                            }
                        }
                    }
                    evo_feedback(EVO_FB_CANCEL);
                    return true;
                }
            } else {
                m_focusPane = BrowserFocusPane::Sidebar;
                evo_feedback(EVO_FB_CANCEL);
                return true;
            }
        }

        if (m_activeSource == 0 || m_activeSource == 1) {
            auto browser = Application::getInstance().getFileSystemBrowser();
            if (browser && !browser->getCurrentPath().empty() &&
                browser->getCurrentPath() != browser->usbHome() &&
                browser->getCurrentPath() != "/data") {
                evo_feedback(EVO_FB_CANCEL);
                browser->navigateUp();
                resetSelection();
                return true;
            }
        }

        // At root or in Favorites / Recent Media / Network: step back into Sidebar
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
            if (m_activeSource == 2 && m_networkShareId.empty()) {
                if (item.fullPath.rfind("netshare://", 0) == 0 &&
                    item.fullPath != "netshare://add" &&
                    item.fullPath != "netshare://add_smb" &&
                    item.fullPath != "netshare://add_server") {
                    m_removeShareConfirmOpen = true;
                    m_removeShareConfirmButton = 0; // Default to Cancel for safety
                    m_removeShareId = item.fullPath.substr(11);
                    m_removeShareName = item.name;
                    evo_feedback(EVO_FB_CONFIRM);
                    return true;
                }
            }
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

    if (m_activeSource == 2) {
        pollNetList(deltaMs);
        if (m_networkShareId.empty()) pollServerStatus(deltaMs);
    }

    // Add-server flow: open the next prompt, collect the login test
    if (s_addPendingKb != ADDKB_NONE) {
        const int kb = s_addPendingKb;
        s_addPendingKb = ADDKB_NONE;
        const bool smb = s_addDraft.protocol == NetworkProtocol::SMB;
        if (kb == ADDKB_ADDRESS) {
            static std::string title;
            title = !s_addError.empty() ? s_addError
                  : smb ? "Server address (e.g. 192.168.0.10 or smb://nas/Movies)"
                        : "Server address (e.g. 192.168.0.10:21 or ftp://host)";
            s_addError.clear();
            evo_keyboard_open_phone(title.c_str(), draftAddressText().c_str(), 512, OnAddAddress, this);
        } else if (kb == ADDKB_USER) {
            evo_keyboard_open_phone("User name", s_addDraft.user.c_str(), 128, OnAddUser, this);
        } else {
            evo_keyboard_open_phone("Password", "", 128, OnAddPass, this);
            evo_keyboard_mark_secret();
        }
    }
    if (s_addTesting && s_testJob) {
        s_testElapsedMs += deltaMs;
        const bool timedOut = s_testElapsedMs > kTestDeadlineMs;
        if (s_testJob->done.load() || timedOut) {
            NetworkTestResult res = timedOut && !s_testJob->done.load() ? NetworkTestResult() : s_testJob->result;
            if (timedOut && !s_testJob->done.load()) {
                res.stage = NetworkTestStage::Address;
                res.message = "No answer from " + s_addDraft.host + ". Check the address.";
            }
            s_testJob.reset();
            s_addTesting = false;
            if (res.stage == NetworkTestStage::Ok) {
                NetworkShare share = s_addDraft;
                share.id = (share.protocol == NetworkProtocol::SMB ? "smb_" : "ftp_") + std::to_string(time(nullptr));
                if (share.name.empty()) share.name = share.host;
                NetworkStorageService::getInstance().addShare(share);
                m_networkShareId.clear();
                m_networkRemotePath = "/";
                resetSelection();
                toast("ADDED", share.name.c_str());
                evo_feedback(EVO_FB_CONFIRM);
            } else {
                const char* title = res.stage == NetworkTestStage::Login ? "LOGIN REFUSED"
                                  : res.stage == NetworkTestStage::Address ? "CAN'T REACH SERVER"
                                  : res.stage == NetworkTestStage::Share ? "SHARE NOT FOUND" : "FOLDER NOT FOUND";
                toast(title, res.message.c_str());
                s_addError = res.message;   // the toast hides behind the next prompt
                evo_feedback(EVO_FB_ERROR);
                if (res.stage == NetworkTestStage::Login) {
                    s_addSignInOpen = true;
                    s_addSignInFocused = 1;
                } else {
                    s_addPendingKb = ADDKB_ADDRESS;
                }
            }
        }
    }

    // Poll active file transfer operations
    auto transferService = Application::getInstance().getFileTransferService();
    if (transferService && m_showTransferDialog) {
        TransferProgress prog = transferService->getProgress();
        if (prog.status == TransferStatus::Completed) {
            m_showTransferDialog = false;
            toast("COMPLETED", "File operation completed!");
            evo_feedback(EVO_FB_CONFIRM);
            transferService->resetStatus();
            auto browser = Application::getInstance().getFileSystemBrowser();
            if (browser) {
                browser->refresh();
                resetSelection();
            }
        } else if (prog.status == TransferStatus::Failed) {
            m_showTransferDialog = false;
            toast("TRANSFER FAILED", prog.errorMessage.empty() ? "Operation failed" : prog.errorMessage.c_str());
            evo_feedback(EVO_FB_ERROR);
            transferService->resetStatus();
            auto browser = Application::getInstance().getFileSystemBrowser();
            if (browser) {
                browser->refresh();
                resetSelection();
            }
        } else if (prog.status == TransferStatus::Cancelled) {
            m_showTransferDialog = false;
            toast("CANCELLED", "Operation was cancelled");
            evo_feedback(EVO_FB_CANCEL);
            transferService->resetStatus();
            auto browser = Application::getInstance().getFileSystemBrowser();
            if (browser) {
                browser->refresh();
                resetSelection();
            }
        }
    }

    m_settleMs += deltaMs;
    auto coverService = Application::getInstance().getCoverArtService();
    if (coverService) coverService->pumpAsync();

    if (!m_showTransferDialog && m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_items.size())) {
        const auto& item = m_items[m_selectedIndex];
        bool isDir = (item.category == FileCategory::Folder);
        if (m_settleMs >= 200.0) {
            if (coverService) {
                coverService->requestBrowserPreview(item.fullPath, isDir);
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
            const std::string usbMount = browser->usbMountOf(browser->getCurrentPath());
            if (browser->getCurrentPath() == browser->usbHome()) {
                std::snprintf(m_formattedPath, sizeof(m_formattedPath), "USB Drive");
            } else if (!usbMount.empty()) {
                /* A drive among several is named by its mount; the path after it is the same length (9) for all. */
                const std::string root = (browser->usbHome() == usbMount) ? std::string("USB Drive")
                                                                         : browser->usbMountLabel(usbMount);
                std::string bc = formatPathBreadcrumbs(root.c_str(), browser->getCurrentPath(), usbMount.size());
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
        if (m_networkShareId.empty()) {
            std::snprintf(m_formattedPath, sizeof(m_formattedPath), "Network Storage");
        } else {
            const auto* share = NetworkStorageService::getInstance().getShare(m_networkShareId);
            std::string shareName = share ? share->name : "FTP";
            if (m_networkRemotePath == "/" || m_networkRemotePath.empty()) {
                std::snprintf(m_formattedPath, sizeof(m_formattedPath), "Network / %s", shareName.c_str());
            } else {
                std::string bc = formatPathBreadcrumbs("Network / " + shareName, m_networkRemotePath, 0);
                std::snprintf(m_formattedPath, sizeof(m_formattedPath), "%s", bc.c_str());
            }
        }
    } else if (m_activeSource == 3) {
        std::snprintf(m_formattedPath, sizeof(m_formattedPath), "Favorites");
    } else if (m_activeSource == 4) {
        std::snprintf(m_formattedPath, sizeof(m_formattedPath), "Recent Media");
    }

    params.path = m_formattedPath;

    // At root check
    if (m_activeSource == 0) {
        params.at_root = (!browser || browser->getCurrentPath().empty() || browser->getCurrentPath() == browser->usbHome()) ? 1 : 0;
    } else if (m_activeSource == 1) {
        params.at_root = (!browser || browser->getCurrentPath().empty() || browser->getCurrentPath() == "/data") ? 1 : 0;
    } else if (m_activeSource == 2) {
        params.at_root = m_networkShareId.empty() ? 1 : 0;
    } else {
        params.at_root = 1;
    }

    /*
     * A shut sandbox looks exactly like an empty folder from in here.
     *
     * The app module launches into a per-title sandbox where /mnt/usb0 and
     * /data are both ENOENT, and it cannot lift that itself - it asks the
     * jailbreak daemon to. When no daemon is running the request is never
     * answered, every listing comes back with nothing in it, and the browser
     * used to report "FOLDER IS EMPTY - No supported media files found",
     * which is true of what it can see and useless to the person reading it.
     * Say what is actually wrong, on the two sources that need the promotion.
     */
    const bool storage_locked = !evo_jailbreak_is_open() &&
                                (m_activeSource == 0 || m_activeSource == 1);

    if (storage_locked) {
        params.empty_title = "ELEVATED PRIVILEGES NEEDED";
        params.empty_hint = "Start the jailbreak payload (Lapy JB or etaHEN), "
                            "then relaunch EVO";
    } else if (m_activeSource == 2) {
        params.empty_title = m_networkEmptyTitle.empty() ? "NO SHARES CONFIGURED" : m_networkEmptyTitle.c_str();
        params.empty_hint = m_networkEmptyHint.empty() ? "Press Cross on [+ Add Server] to configure" : m_networkEmptyHint.c_str();
    } else if (m_activeSource == 3) {
        params.empty_title = "NO FAVORITES YET";
        params.empty_hint = "Press [Triangle] on any file to add to Favorites";
    } else if (m_activeSource == 4) {
        params.empty_title = "NO RECENT MEDIA";
        params.empty_hint = "Media you play will appear here";
    } else if (m_isSearching) {
        params.empty_title = "NO MATCHES FOUND";
        params.empty_hint = "Try a different search keyword";
    } else if (m_categoryFilter == static_cast<int>(FileCategory::Video)) {
        params.empty_title = "NO VIDEOS FOUND";
        params.empty_hint = "Nothing playable in this source";
    } else if (m_categoryFilter == static_cast<int>(FileCategory::Audio)) {
        params.empty_title = "NO MUSIC FOUND";
        params.empty_hint = "Nothing playable in this source";
    } else if (m_categoryFilter == static_cast<int>(FileCategory::Image)) {
        params.empty_title = "NO PHOTOS FOUND";
        params.empty_hint = "Nothing viewable in this source";
    } else {
        params.empty_title = "FOLDER IS EMPTY";
        params.empty_hint = "No supported media files found";
    }

    bool railFocused = false;
    if (auto sm = Application::getInstance().getScreenManager()) {
        railFocused = sm->isRailFocused();
    }
    params.rail_focused = railFocused ? 1 : 0;
    params.sidebar_focused = (m_focusPane == BrowserFocusPane::Sidebar && !railFocused) ? 1 : 0;
    params.sidebar_index = m_sidebarIndex;
    params.active_source = m_activeSource;

    /* Chips: "All", then one per category the folder actually holds. Sending
     * none is how the row is hidden. */
    params.filter_focused = (m_focusPane == BrowserFocusPane::Filter) ? 1 : 0;
    params.filter_selected = m_filterIndex;
    params.filter_count = 0;
    if (!m_folderCats.empty()) {
        params.filter_labels[params.filter_count++] = "All";
        for (size_t i = 0; i < m_folderCats.size() && params.filter_count < 4; ++i) {
            switch (static_cast<FileCategory>(m_folderCats[i])) {
                case FileCategory::Video: params.filter_labels[params.filter_count++] = "Videos"; break;
                case FileCategory::Audio: params.filter_labels[params.filter_count++] = "Music";  break;
                case FileCategory::Image: params.filter_labels[params.filter_count++] = "Photos"; break;
                default: break;
            }
        }
    }

    int totalCount = static_cast<int>(m_items.size());
    params.total_count = totalCount;
    params.is_empty = (totalCount == 0);

    char loadingText[160];
    if (m_activeSource == 2 && !m_networkShareId.empty()) {
        params.loading = (m_netState == NetListState::Loading) ? 1 : 0;
        params.failed = (m_netState == NetListState::Failed) ? 1 : 0;
        if (params.loading) {
            const auto* share = NetworkStorageService::getInstance().getShare(m_networkShareId);
            if (m_networkRemotePath == "/" || m_networkRemotePath.empty())
                std::snprintf(loadingText, sizeof(loadingText), "Connecting to %s...", share ? share->name.c_str() : "server");
            else
                std::snprintf(loadingText, sizeof(loadingText), "Loading folder...");
            params.loading_text = loadingText;
        }
    }

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

    /* Poster extraction runs on the CoverArtService worker, which paces itself;
     * render() only asks. */
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
        params.rows[i].status_kind = item.statusKind;
        params.rows[i].status_text = item.statusText.c_str();

        const uint32_t* art = nullptr;
        if (!m_showTransferDialog && coverService && item.fullPath.find("://") == std::string::npos) {
            art = coverService->peekCoverArt(item.fullPath);
            bool tried = coverService->hasTriedCoverArt(item.fullPath);
            if (!art && !tried) {
                /* Decoded on the worker; the tile fills in after pumpAsync(). */
                coverService->requestCoverArt(item.fullPath, item.category == FileCategory::Folder);
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
        if (!curItem.fullPath.empty() && curItem.fullPath.find("://") == std::string::npos && stat(curItem.fullPath.c_str(), &st) == 0 && curItem.category != FileCategory::Folder) {
            double sz = static_cast<double>(st.st_size);
            const char* unit = "B";
            if (sz >= 1024.0) { sz /= 1024.0; unit = "KB"; }
            if (sz >= 1024.0) { sz /= 1024.0; unit = "MB"; }
            if (sz >= 1024.0) { sz /= 1024.0; unit = "GB"; }
            std::snprintf(m_statusSize, sizeof(m_statusSize), "%.2f %s", sz, unit);
            params.status_size = m_statusSize;
        }

        if (coverService && curItem.fullPath.find("://") == std::string::npos) {
            params.ins_preview = coverService->getBrowserPreviewPixels();
            params.ins_preview_w = ICoverArtService::PreviewWidth;
            params.ins_preview_h = ICoverArtService::PreviewHeight;
        }

        params.ins_probing = (m_settleMs >= 200.0 && curItem.fullPath.find("://") == std::string::npos && m_cachedMetadata.filePath != curItem.fullPath && curItem.category != FileCategory::Folder) ? 1 : 0;
    }

    if (m_activeSource == 2 && m_networkShareId.empty()) {
        if (m_selectedIndex >= 0 && m_selectedIndex < totalCount) {
            const auto& curItem = m_items[m_selectedIndex];
            if (curItem.fullPath.rfind("netshare://", 0) == 0 &&
                curItem.fullPath != "netshare://add" &&
                curItem.fullPath != "netshare://add_smb" &&
                curItem.fullPath != "netshare://add_server") {
                params.footer_triangle_label = "REMOVE";
                params.footer_options_label = "OPTIONS";
            }
        }
    }

    auto transferService = Application::getInstance().getFileTransferService();
    if (s_addTesting) {
        params.action_menu_open = 1;
        params.action_menu_focused = 0;
        params.action_menu_target = s_addDraft.host.c_str();
        params.action_target_sub = s_addDraft.protocol == NetworkProtocol::SMB ? "SMB" : "FTP";
        params.action_menu_eyebrow = "TESTING";
        params.action_menu_count = 1;
        params.action_menu_items[0].label = "Cancel";
        params.action_menu_items[0].desc = "Checking the address and login...";
        params.action_menu_items[0].icon_path = "../icons/icon_folder.png";
        params.action_menu_items[0].danger = 0;
        params.action_paste_enabled = 0;
        params.action_clipboard_info = nullptr;
    } else if (s_addSignInOpen) {
        params.action_menu_open = 1;
        params.action_menu_focused = s_addSignInFocused;
        params.action_menu_target = s_addDraft.host.c_str();
        params.action_target_sub = !s_addError.empty() ? s_addError.c_str()
                                 : s_addDraft.protocol == NetworkProtocol::SMB ? "SMB" : "FTP";
        params.action_menu_eyebrow = "SIGN IN";
        params.action_menu_count = 2;
        params.action_menu_items[0].label = "Guest";
        params.action_menu_items[0].desc = "No user or password";
        params.action_menu_items[0].icon_path = "../icons/icon_folder.png";
        params.action_menu_items[0].danger = 0;
        params.action_menu_items[1].label = "Account";
        params.action_menu_items[1].desc = "Enter a user and password";
        params.action_menu_items[1].icon_path = "../icons/icon_folder.png";
        params.action_menu_items[1].danger = 0;
        params.action_paste_enabled = 0;
        params.action_clipboard_info = nullptr;
    } else if (s_addServerChooserOpen) {
        params.action_menu_open = 1;
        params.action_menu_focused = s_addServerChooserFocused;
        params.action_menu_target = "+ Add Server";
        params.action_target_sub = "SMB or FTP";
        params.action_menu_eyebrow = "ADD SERVER";
        params.action_menu_count = 2;

        params.action_menu_items[0].label = "Samba / Windows share (SMB)";
        params.action_menu_items[0].desc = "Connect via Samba / Windows share (SMB2/3)";
        params.action_menu_items[0].icon_path = "../icons/icon_folder.png";
        params.action_menu_items[0].danger = 0;

        params.action_menu_items[1].label = "FTP server";
        params.action_menu_items[1].desc = "Connect via Network FTP";
        params.action_menu_items[1].icon_path = "../icons/icon_folder.png";
        params.action_menu_items[1].danger = 0;

        params.action_paste_enabled = 0;
        params.action_clipboard_info = nullptr;
    } else if (m_serverMenuOpen) {
        params.action_menu_open = 1;
        params.action_menu_focused = m_serverMenuFocused;
        params.action_menu_target = m_serverMenuShareName.c_str();
        const auto* share = NetworkStorageService::getInstance().getShare(m_serverMenuShareId);
        const bool isSmb = (share && share->protocol == NetworkProtocol::SMB);
        params.action_target_sub = isSmb ? "Windows Share (SMB)" : "FTP Server";
        params.action_menu_eyebrow = isSmb ? "OPTIONS  •  SMB SHARE" : "OPTIONS  •  FTP SERVER";
        params.action_menu_count = 4;

        params.action_menu_items[0].label = "Open";
        params.action_menu_items[0].desc = isSmb ? "Browse this Windows share" : "Browse this FTP server";
        params.action_menu_items[0].icon_path = "../icons/icon_resume.png";
        params.action_menu_items[0].danger = 0;

        params.action_menu_items[1].label = "Edit Server";
        params.action_menu_items[1].desc = "Address, host, share or credentials";
        params.action_menu_items[1].icon_path = "../icons/icon_keyboard.png";
        params.action_menu_items[1].danger = 0;

        params.action_menu_items[2].label = "Remove Server";
        params.action_menu_items[2].desc = "Forget this server on this console";
        params.action_menu_items[2].icon_path = "../icons/icon_trash.png";
        params.action_menu_items[2].danger = 1;

        params.action_menu_items[3].label = "Close";
        params.action_menu_items[3].desc = "Dismiss menu";
        params.action_menu_items[3].icon_path = "../icons/btn_circle.png";
        params.action_menu_items[3].danger = 0;

        params.action_paste_enabled = 0;
        params.action_clipboard_info = nullptr;
    } else {
        params.action_menu_count = 0;
        params.action_menu_eyebrow = nullptr;
        params.action_menu_open = m_actionMenuOpen ? 1 : 0;
        params.action_menu_focused = m_actionMenuFocused;
        params.action_menu_target = m_actionTargetName.c_str();
        params.action_paste_enabled = (transferService && transferService->hasClipboardItem()) ? 1 : 0;

        if (m_actionTargetIsDir) {
            std::snprintf(m_actionTargetSubBuf, sizeof(m_actionTargetSubBuf), "Directory");
        } else if (m_selectedIndex >= 0 && m_selectedIndex < totalCount) {
            std::snprintf(m_actionTargetSubBuf, sizeof(m_actionTargetSubBuf), "%s • %s", m_insExt, m_statusSize[0] ? m_statusSize : "File");
        } else {
            std::snprintf(m_actionTargetSubBuf, sizeof(m_actionTargetSubBuf), "Current Directory");
        }
        params.action_target_sub = m_actionTargetSubBuf;

        if (transferService && transferService->hasClipboardItem()) {
            const auto& clip = transferService->getClipboard();
            std::snprintf(m_actionClipboardBuf, sizeof(m_actionClipboardBuf), "CLIPBOARD: %s (%s)",
                          clip.name.c_str(), clip.action == ClipboardAction::Cut ? "Move" : "Copy");
            params.action_clipboard_info = m_actionClipboardBuf;
        } else {
            params.action_clipboard_info = nullptr;
        }
    }

    // Populate Transfer Progress Modal parameters
    if (m_showTransferDialog && transferService) {
        TransferProgress prog = transferService->getProgress();
        params.transfer_modal_open = 1;

        if (prog.opType == FileOpType::Copy) {
            std::snprintf(m_transferTitleBuf, sizeof(m_transferTitleBuf), "COPYING TO STORAGE");
        } else if (prog.opType == FileOpType::Move) {
            std::snprintf(m_transferTitleBuf, sizeof(m_transferTitleBuf), "MOVING FILE");
        } else if (prog.opType == FileOpType::Delete) {
            std::snprintf(m_transferTitleBuf, sizeof(m_transferTitleBuf), "DELETING FROM STORAGE");
        } else {
            std::snprintf(m_transferTitleBuf, sizeof(m_transferTitleBuf), "FILE OPERATION");
        }
        params.transfer_op_title = m_transferTitleBuf;
        params.transfer_item_name = prog.currentItemName.empty() ? "Processing..." : prog.currentItemName.c_str();

        // Speed format
        double speedMB = prog.currentSpeedBps / (1024.0 * 1024.0);
        if (speedMB >= 100.0) {
            std::snprintf(m_transferSpeedBuf, sizeof(m_transferSpeedBuf), "%.0f MB/s", speedMB);
        } else if (speedMB >= 0.1) {
            std::snprintf(m_transferSpeedBuf, sizeof(m_transferSpeedBuf), "%.1f MB/s", speedMB);
        } else if (prog.currentSpeedBps > 0.0) {
            std::snprintf(m_transferSpeedBuf, sizeof(m_transferSpeedBuf), "%.0f KB/s", prog.currentSpeedBps / 1024.0);
        } else {
            std::snprintf(m_transferSpeedBuf, sizeof(m_transferSpeedBuf), "0.0 MB/s");
        }
        params.transfer_speed_str = m_transferSpeedBuf;

        // Size format (Transferred / Total)
        double transferredMB = static_cast<double>(prog.bytesTransferred) / (1024.0 * 1024.0);
        double totalMB = static_cast<double>(prog.totalBytes) / (1024.0 * 1024.0);
        if (totalMB >= 1024.0) {
            std::snprintf(m_transferBytesBuf, sizeof(m_transferBytesBuf), "%.2f GB / %.2f GB",
                          transferredMB / 1024.0, totalMB / 1024.0);
        } else if (totalMB > 0.0) {
            std::snprintf(m_transferBytesBuf, sizeof(m_transferBytesBuf), "%.1f MB / %.1f MB",
                          transferredMB, totalMB);
        } else if (prog.totalFiles > 0) {
            std::snprintf(m_transferBytesBuf, sizeof(m_transferBytesBuf), "%u of %u items",
                          prog.filesTransferred, prog.totalFiles);
        } else {
            std::snprintf(m_transferBytesBuf, sizeof(m_transferBytesBuf), "Scanning filesystem...");
        }
        params.transfer_bytes_str = m_transferBytesBuf;

        // ETA format
        if (prog.estimatedSecondsRemaining > 0) {
            int etaSec = prog.estimatedSecondsRemaining;
            if (etaSec >= 60) {
                std::snprintf(m_transferEtaBuf, sizeof(m_transferEtaBuf), "ETA %dm %02ds", etaSec / 60, etaSec % 60);
            } else {
                std::snprintf(m_transferEtaBuf, sizeof(m_transferEtaBuf), "ETA %ds", etaSec);
            }
        } else if (prog.totalFiles > 1) {
            std::snprintf(m_transferEtaBuf, sizeof(m_transferEtaBuf), "Item %u of %u", prog.filesTransferred + 1, prog.totalFiles);
        } else {
            std::snprintf(m_transferEtaBuf, sizeof(m_transferEtaBuf), "In progress...");
        }
        params.transfer_eta_str = m_transferEtaBuf;

        // Percentage
        std::snprintf(m_transferPercentBuf, sizeof(m_transferPercentBuf), "%.0f%%", prog.progressPercent);
        params.transfer_percent_str = m_transferPercentBuf;
        params.transfer_progress_pct = prog.progressPercent;

        // Paths
        std::snprintf(m_transferSrcBuf, sizeof(m_transferSrcBuf), "%s", prog.currentSourcePath.empty() ? "-" : prog.currentSourcePath.c_str());
        std::snprintf(m_transferDstBuf, sizeof(m_transferDstBuf), "%s", prog.currentDestPath.empty() ? "-" : prog.currentDestPath.c_str());
        params.transfer_src_path = m_transferSrcBuf;
        params.transfer_dst_path = m_transferDstBuf;
    } else {
        params.transfer_modal_open = 0;
    }

    evo_rmlui_update_browser(&params);
    evo_rmlui_render_browser(framebuffer, width, height);

    // Render Delete Confirmation Dialog if open
    if (m_showDeleteConfirm) {
        evo_rmlui_dialog_params_t dlg;
        std::memset(&dlg, 0, sizeof(dlg));
        dlg.eyebrow = "DELETE PERMANENTLY";
        dlg.title = m_actionTargetName.c_str();
        std::snprintf(m_transferDetailBuf, sizeof(m_transferDetailBuf),
                      "Are you sure you want to permanently delete this %s?",
                      m_actionTargetIsDir ? "folder and all its contents" : "file");
        dlg.detail = m_transferDetailBuf;
        dlg.progress_pct = -1.0;
        dlg.action_count = 2;
        dlg.focused_action = m_deleteConfirmButton;

        dlg.actions[0].label = "CANCEL";
        dlg.actions[0].icon_path = "../icons/btn_circle.png";
        dlg.actions[0].is_primary = (m_deleteConfirmButton == 0) ? 1 : 0;

        dlg.actions[1].label = "DELETE";
        dlg.actions[1].icon_path = "../icons/btn_cross.png";
        dlg.actions[1].is_primary = (m_deleteConfirmButton == 1) ? 1 : 0;

        evo_rmlui_update_dialog(&dlg);
        evo_rmlui_render_dialog(framebuffer, width, height);
    }

    // Render Remove Server Confirmation Dialog if open
    if (m_removeShareConfirmOpen) {
        evo_rmlui_dialog_params_t dlg;
        std::memset(&dlg, 0, sizeof(dlg));
        dlg.eyebrow = "REMOVE SERVER";
        dlg.title = m_removeShareName.c_str();
        dlg.detail = "Are you sure you want to remove this network server from this console?";
        dlg.progress_pct = -1.0;
        dlg.action_count = 2;
        dlg.focused_action = m_removeShareConfirmButton;

        dlg.actions[0].label = "CANCEL";
        dlg.actions[0].icon_path = "../icons/btn_circle.png";
        dlg.actions[0].is_primary = (m_removeShareConfirmButton == 0) ? 1 : 0;

        dlg.actions[1].label = "REMOVE";
        dlg.actions[1].icon_path = "../icons/btn_cross.png";
        dlg.actions[1].is_primary = (m_removeShareConfirmButton == 1) ? 1 : 0;

        evo_rmlui_update_dialog(&dlg);
        evo_rmlui_render_dialog(framebuffer, width, height);
    }
}

} // namespace evo
