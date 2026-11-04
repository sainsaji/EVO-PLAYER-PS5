/*
 * The Web section (#237): WebLibrary (bookmarks, history), WebHomeScreen
 * (a list: address, bookmarks, history) and WebPageScreen (the page from the
 * out-of-process browser engine, EVO's pointer over it). docs/ui/browser.md.
 */
#include "evo/screens/WebScreens.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_hui.h"
#include "evo_feedback.h"
#include "evo_toast.h"
#include "evo_boot_log.h"
#include "evo_keyboard.h"
#include "evo_data_path.h"
#include "evo_web_url.h"
#include "evo_browser_engine.h"
#include "evo_browser_wire.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace evo {

namespace {

constexpr const char* kBookmarksFile = "web_bookmarks.txt";
constexpr const char* kHistoryFile = "web_history.txt";

/* What a fresh install offers. */
const WebEntry kDefaultBookmarks[] = {
    {"https://duckduckgo.com/", "DuckDuckGo"},
    {"https://en.m.wikipedia.org/", "Wikipedia"},
    {"https://github.com/sainsaji/EVO-PLAYER-PS5", "EVO Player on GitHub"},
};

void read_list(const char* leaf, std::vector<WebEntry>& out)
{
    out.clear();
    FILE* f = std::fopen(evo_data_path(leaf), "r");
    if (!f) return;
    char line[2304];
    while (std::fgets(line, sizeof line, f)) {
        line[std::strcspn(line, "\r\n")] = 0;
        char* tab = std::strchr(line, '\t');
        if (tab) *tab = 0;
        if (!line[0]) continue;
        out.push_back({line, tab ? tab + 1 : ""});
    }
    std::fclose(f);
}

/* A label for a row: the title, else the host. */
std::string label_for(const WebEntry& e)
{
    if (!e.title.empty()) return e.title;
    char host[256];
    evo_web_url_host(e.url.c_str(), host, sizeof host);
    return host[0] ? std::string(host) : e.url;
}

/* Keep the list rows to plain ASCII-safe text of a sane length. */
std::string row_text(const std::string& s, size_t max)
{
    std::string out = s.substr(0, max);
    for (char& c : out)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return out;
}

WebPageScreen* page_screen()
{
    if (auto sm = Application::getInstance().getScreenManager())
        return dynamic_cast<WebPageScreen*>(sm->getScreen(ScreenId::WebPage));
    return nullptr;
}

void open_page(const std::string& url)
{
    auto sm = Application::getInstance().getScreenManager();
    WebPageScreen* page = page_screen();
    if (!sm || !page) return;
    page->openUrl(url);
    sm->navigateTo(ScreenId::WebPage);
}

} // namespace

/* ------------------------------------------------------------ WebLibrary */

WebLibrary& WebLibrary::instance()
{
    static WebLibrary lib;
    return lib;
}

void WebLibrary::load()
{
    if (m_loaded) return;
    m_loaded = true;
    FILE* f = std::fopen(evo_data_path(kBookmarksFile), "r");
    if (f) {
        std::fclose(f);
        read_list(kBookmarksFile, m_bookmarks);
    } else {
        m_bookmarks.assign(std::begin(kDefaultBookmarks), std::end(kDefaultBookmarks));
    }
    read_list(kHistoryFile, m_history);
    if (m_history.size() > kMaxHistory) m_history.resize(kMaxHistory);
}

void WebLibrary::save(const char* leaf, const std::vector<WebEntry>& list)
{
    FILE* f = std::fopen(evo_data_path(leaf), "w");
    if (!f) {
        EVO_LOGW("web", "cannot write %s", leaf);
        return;
    }
    for (const auto& e : list)
        std::fprintf(f, "%s\t%s\n", row_text(e.url, 2048).c_str(), row_text(e.title, 200).c_str());
    std::fclose(f);
}

const std::vector<WebEntry>& WebLibrary::bookmarks()
{
    load();
    return m_bookmarks;
}

const std::vector<WebEntry>& WebLibrary::history()
{
    load();
    return m_history;
}

void WebLibrary::noteVisit(const std::string& url, const std::string& title)
{
    load();
    if (url.empty() || url.rfind("about:", 0) == 0) return;
    std::string keep = title;
    auto it = std::find_if(m_history.begin(), m_history.end(),
                           [&](const WebEntry& e) { return e.url == url; });
    if (it != m_history.end()) {
        if (keep.empty()) keep = it->title;
        m_history.erase(it);
    }
    m_history.insert(m_history.begin(), {url, keep});
    if (m_history.size() > kMaxHistory) m_history.resize(kMaxHistory);
    save(kHistoryFile, m_history);
}

bool WebLibrary::isBookmarked(const std::string& url)
{
    load();
    return std::any_of(m_bookmarks.begin(), m_bookmarks.end(),
                       [&](const WebEntry& e) { return e.url == url; });
}

bool WebLibrary::toggleBookmark(const std::string& url, const std::string& title)
{
    load();
    auto it = std::find_if(m_bookmarks.begin(), m_bookmarks.end(),
                           [&](const WebEntry& e) { return e.url == url; });
    const bool added = it == m_bookmarks.end();
    if (added)
        m_bookmarks.push_back({url, title});
    else
        m_bookmarks.erase(it);
    save(kBookmarksFile, m_bookmarks);
    return added;
}

void WebLibrary::clearHistory()
{
    load();
    m_history.clear();
    save(kHistoryFile, m_history);
}

/* --------------------------------------------------------- WebHomeScreen */

WebHomeScreen::WebHomeScreen() : StatefulScreen("WebHomeScreen") {}

void WebHomeScreen::rebuild()
{
    m_rows.clear();
    m_rows.push_back({RowKind::Address, {}});
    for (const auto& b : WebLibrary::instance().bookmarks())
        m_rows.push_back({RowKind::Bookmark, b});
    for (const auto& h : WebLibrary::instance().history())
        m_rows.push_back({RowKind::History, h});
    const int n = static_cast<int>(m_rows.size());
    m_selected = std::clamp(m_selected, 0, n - 1);
    m_scroll = std::clamp(m_scroll, 0, std::max(0, n - EVO_RMLUI_LIST_ROWS));
}

void WebHomeScreen::onEnter()
{
    StatefulScreen::onEnter();
    rebuild();
    /* Start the engine as soon as the section opens: the first page then
     * appears without the whole start-up wait. */
    evo_browser_open();
}

void WebHomeScreen::onAddressSubmitted(const char* text, void* self)
{
    (void)self;
    char url[2048];
    if (evo_web_normalize_input(text, url, sizeof url) == EVO_WEB_INPUT_EMPTY) return;
    open_page(url);
}

void WebHomeScreen::openAddress()
{
    evo_keyboard_open_phone("Web address or search", "", 1024, &WebHomeScreen::onAddressSubmitted, this);
}

bool WebHomeScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released)
{
    (void)held;
    (void)released;
    const int n = static_cast<int>(m_rows.size());

    if (pressed & PadButtons::Up) {
        if (m_selected > 0) {
            --m_selected;
            if (m_selected < m_scroll) m_scroll = m_selected;
            evo_feedback(EVO_FB_MOVE);
        } else {
            evo_feedback(EVO_FB_BOUNDARY);
        }
        return true;
    }
    if (pressed & PadButtons::Down) {
        if (m_selected < n - 1) {
            ++m_selected;
            if (m_selected >= m_scroll + EVO_RMLUI_LIST_ROWS) m_scroll = m_selected - EVO_RMLUI_LIST_ROWS + 1;
            evo_feedback(EVO_FB_MOVE);
        } else {
            evo_feedback(EVO_FB_BOUNDARY);
        }
        return true;
    }
    if (pressed & PadButtons::Left) {
        if (auto sm = Application::getInstance().getScreenManager()) {
            sm->setRailFocused(true);
            return true;
        }
    }
    if (pressed & PadButtons::Cross) {
        if (m_selected < 0 || m_selected >= n) return true;
        evo_feedback(EVO_FB_CONFIRM);
        const Row& r = m_rows[static_cast<size_t>(m_selected)];
        if (r.kind == RowKind::Address)
            openAddress();
        else
            open_page(r.entry.url);
        return true;
    }
    if (pressed & PadButtons::Square) {   /* type an address from anywhere in the list */
        evo_feedback(EVO_FB_CONFIRM);
        openAddress();
        return true;
    }
    if (pressed & PadButtons::Triangle) {
        if (m_selected >= 0 && m_selected < n) {
            const Row r = m_rows[static_cast<size_t>(m_selected)];
            if (r.kind == RowKind::Bookmark) {
                WebLibrary::instance().toggleBookmark(r.entry.url, r.entry.title);
                toast("WEB", "Bookmark removed");
                rebuild();
            } else if (r.kind == RowKind::History) {
                const bool added = WebLibrary::instance().toggleBookmark(r.entry.url, r.entry.title);
                toast("WEB", added ? "Bookmarked" : "Bookmark removed");
                rebuild();
            }
        }
        return true;
    }
    if (pressed & PadButtons::Options) {
        if (!WebLibrary::instance().history().empty()) {
            WebLibrary::instance().clearHistory();
            toast("WEB", "History cleared");
            rebuild();
        }
        return true;
    }
    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto sm = Application::getInstance().getScreenManager()) sm->navigateTo(ScreenId::MainMenu);
        return true;
    }
    return false;
}

void WebHomeScreen::render(uint32_t* framebuffer, int width, int height)
{
    evo_rmlui_list_params_t params;
    std::memset(&params, 0, sizeof(params));
    bool railFocused = false;
    if (auto sm = Application::getInstance().getScreenManager()) railFocused = sm->isRailFocused();

    char status[160] = "";
    switch (evo_browser_state(status, sizeof status)) {
    case EVO_BROWSER_READY:    std::snprintf(status, sizeof status, "BROWSE THE WEB"); break;
    case EVO_BROWSER_STARTING: std::snprintf(status, sizeof status, "STARTING THE BROWSER..."); break;
    case EVO_BROWSER_FAILED:   break;   /* the reason stays */
    default:                   std::snprintf(status, sizeof status, "BROWSE THE WEB"); break;
    }

    params.title = "WEB";
    params.subtitle = status;
    params.rail_focused = railFocused ? 1 : 0;
    params.total_count = static_cast<int>(m_rows.size());
    params.cursor_index = m_selected;
    const int count = std::min(EVO_RMLUI_LIST_ROWS,
                               std::max(0, static_cast<int>(m_rows.size()) - m_scroll));
    params.row_count = count;

    std::string titles[EVO_RMLUI_LIST_ROWS];
    std::string details[EVO_RMLUI_LIST_ROWS];
    for (int i = 0; i < count; ++i) {
        const int idx = m_scroll + i;
        const Row& r = m_rows[static_cast<size_t>(idx)];
        auto& row = params.rows[i];
        switch (r.kind) {
        case RowKind::Address:
            titles[i] = "Enter an address or search";
            details[i] = "Type with the keyboard, or from your phone";
            row.icon_path = "../icons/icon_globe.png";
            row.badge = "GO";
            break;
        case RowKind::Bookmark:
            titles[i] = row_text(label_for(r.entry), 120);
            details[i] = row_text(r.entry.url, 400);
            row.icon_path = "../icons/icon_favorites.png";
            row.badge = "BOOKMARK";
            break;
        case RowKind::History:
            titles[i] = row_text(label_for(r.entry), 120);
            details[i] = row_text(r.entry.url, 400);
            row.icon_path = "../icons/icon_recent_files.png";
            row.badge = "HISTORY";
            break;
        }
        row.title = titles[i].c_str();
        row.detail = details[i].c_str();
        row.progress = -1;
        row.is_focused = (!railFocused && idx == m_selected);
    }

    int h = 0;
    params.hints[h].glyph_path = "../icons/btn_cross.png";
    params.hints[h++].label = "OPEN";
    params.hints[h].glyph_path = "../icons/btn_square.png";
    params.hints[h++].label = "ADDRESS";
    if (m_selected > 0) {
        params.hints[h].glyph_path = "../icons/btn_triangle.png";
        params.hints[h++].label =
            m_rows[static_cast<size_t>(m_selected)].kind == RowKind::Bookmark ? "REMOVE" : "BOOKMARK";
    }
    params.hints[h].glyph_path = "../icons/btn_circle.png";
    params.hints[h++].label = "BACK";
    params.hint_count = h;

    evo_rmlui_update_list(&params);
    evo_rmlui_render_list(framebuffer, width, height);
}

/* --------------------------------------------------------- WebPageScreen */

namespace {
constexpr float kBarH = static_cast<float>(EVO_HUI_WEBPAGE_BAR_H);
constexpr float kPageX = static_cast<float>(EVO_HUI_WEBPAGE_X);   /* right of EVO's rail */

bool rail_focused()
{
    auto sm = Application::getInstance().getScreenManager();
    return sm && sm->isRailFocused();
}
constexpr float kPointerSpeed = 1500.0f;   /* px/s at full tilt */
constexpr float kScrollSpeed = 2200.0f;    /* px/s at full tilt */
constexpr int kDpadStep = 160;             /* px per D-pad press */
constexpr long long kFieldClickMs = 1500;  /* a focus this soon after a click opens the keyboard */
}

WebPageScreen::WebPageScreen() : StatefulScreen("WebPageScreen") {}

int WebPageScreen::pageX() const { return static_cast<int>(m_px - kPageX); }
int WebPageScreen::pageY() const { return static_cast<int>(m_py - kBarH); }

void WebPageScreen::openUrl(const std::string& url)
{
    m_pendingUrl = url;
    m_url = url;
    m_title.clear();
    if (isScreenActive()) {
        evo_browser_load(url.c_str());
        m_pendingUrl.clear();
    }
}

void WebPageScreen::onEnter()
{
    StatefulScreen::onEnter();
    evo_browser_open();
    m_setupSent = false;
    m_crossDown = false;
    m_sentX = m_sentY = -1;
    if (!m_pendingUrl.empty()) {
        evo_browser_load(m_pendingUrl.c_str());   /* queued until the engine is ready */
        m_pendingUrl.clear();
    }
}

void WebPageScreen::onExit()
{
    if (m_crossDown) evo_browser_pointer(pageX(), pageY(), EVOB_PTR_UP, 1);
    m_crossDown = false;
    evo_browser_set_visible(0);   /* no painting while EVO shows something else */
    StatefulScreen::onExit();
}

void WebPageScreen::pollEvents()
{
    evo_browser_event_t ev;
    while (evo_browser_poll_event(&ev)) {
        switch (ev.kind) {
        /* History gets a page only once its load finishes, so the short
         * stops of a redirect (en.m.wikipedia.org, no title) stay out. */
        case EVOB_EV_URL:
            m_url = ev.text;
            break;
        case EVOB_EV_TITLE:
            m_title = ev.text;
            m_titleUrl = m_url;
            if (!m_visitedUrl.empty() && m_visitedUrl == m_url)
                WebLibrary::instance().noteVisit(m_url, m_title);   /* a title after the load */
            break;
        case EVOB_EV_LOAD_STARTED:
            m_loading = true;
            break;
        case EVOB_EV_LOAD_FINISHED:
            m_loading = false;
            m_visitedUrl = m_url;
            /* the old page's title must not stick to a page that has none */
            WebLibrary::instance().noteVisit(m_url, m_titleUrl == m_url ? m_title : std::string());
            break;
        case EVOB_EV_LOAD_FAILED: {
            m_loading = false;
            const char* nl = std::strchr(ev.text, '\n');
            toast("PAGE DID NOT LOAD", nl && nl[1] ? nl + 1 : ev.text);
            break;
        }
        case EVOB_EV_NAV_STATE:
            m_canBack = (ev.arg & 1u) != 0;
            m_canForward = (ev.arg & 2u) != 0;
            break;
        case EVOB_EV_TEXT_FOCUS:
            /* Only a field the user just clicked: pages that focus a search
             * box on their own must not throw the keyboard up. */
            if (ev.arg && evo_log_mono_ms() - m_lastClickMs < kFieldClickMs && isScreenActive() &&
                !evo_keyboard_is_open())
                evo_keyboard_open("Type", "", 1024, &WebPageScreen::onFieldSubmitted, this);
            break;
        case EVOB_EV_NEW_WINDOW:
            /* one page at a time: a new window replaces this one */
            if (ev.text[0]) evo_browser_load(ev.text);
            break;
        case EVOB_EV_CURSOR:
            m_hand = ev.arg == 1;
            break;
        case EVOB_EV_ENGINE_ERROR:
            toast("WEB", "The browser engine could not start a page process");
            break;
        default:
            break;
        }
    }
}

void WebPageScreen::movePointer(double dtMs)
{
    float sx = 0.0f, sy = 0.0f;
    Application::getInstance().getLeftStick(&sx, &sy);
    const float dt = static_cast<float>(dtMs) / 1000.0f;
    /* squared response: fine control near the centre, speed at full tilt */
    auto curve = [](float v) { return v * std::fabs(v); };
    m_px = std::clamp(m_px + curve(sx) * kPointerSpeed * dt, kPageX, 1919.0f);
    m_py = std::clamp(m_py + curve(sy) * kPointerSpeed * dt, kBarH, 1079.0f);
    if (pageX() != m_sentX || pageY() != m_sentY) {
        m_sentX = pageX();
        m_sentY = pageY();
        evo_browser_pointer(m_sentX, m_sentY, EVOB_PTR_MOVE, 0);
    }
}

void WebPageScreen::scrollWithStick(double dtMs)
{
    float sx = 0.0f, sy = 0.0f;
    Application::getInstance().getRightStick(&sx, &sy);
    const float dt = static_cast<float>(dtMs) / 1000.0f;
    m_scrollAccX += sx * std::fabs(sx) * kScrollSpeed * dt;
    m_scrollAccY += sy * std::fabs(sy) * kScrollSpeed * dt;
    const int dx = static_cast<int>(m_scrollAccX);
    const int dy = static_cast<int>(m_scrollAccY);
    if (dx || dy) {
        m_scrollAccX -= static_cast<float>(dx);
        m_scrollAccY -= static_cast<float>(dy);
        evo_browser_scroll(pageX(), pageY(), dx, dy);
    }
    if (sx == 0.0f) m_scrollAccX = 0.0f;
    if (sy == 0.0f) m_scrollAccY = 0.0f;
}

void WebPageScreen::update(double deltaMs)
{
    StatefulScreen::update(deltaMs);
    if (!isScreenActive()) return;
    if (evo_browser_state(nullptr, 0) == EVO_BROWSER_READY && !m_setupSent) {
        m_setupSent = true;
        evo_browser_resize(EVO_HUI_WEBPAGE_W, EVO_HUI_WEBPAGE_H);
        evo_browser_set_visible(1);
    }
    pollEvents();
    if (evo_keyboard_is_open() || rail_focused()) return;   /* they own the pad */
    movePointer(deltaMs);
    scrollWithStick(deltaMs);
}

void WebPageScreen::onAddressSubmitted(const char* text, void* self)
{
    char url[2048];
    if (evo_web_normalize_input(text, url, sizeof url) == EVO_WEB_INPUT_EMPTY) return;
    static_cast<WebPageScreen*>(self)->openUrl(url);
}

void WebPageScreen::onFieldSubmitted(const char* text, void* self)
{
    (void)self;
    if (text && text[0]) evo_browser_text(text);
}

void WebPageScreen::editAddress()
{
    evo_keyboard_open_phone("Web address or search", m_url.c_str(), 1024,
                            &WebPageScreen::onAddressSubmitted, this);
}

bool WebPageScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released)
{
    (void)held;
    const uint32_t raw = Application::getInstance().getPadButtons();   /* real buttons, not stick tilts */
    const bool ready = evo_browser_state(nullptr, 0) == EVO_BROWSER_READY;

    if (pressed & PadButtons::Cross) {
        if (ready) {
            m_crossDown = true;
            m_lastClickMs = evo_log_mono_ms();
            evo_browser_pointer(pageX(), pageY(), EVOB_PTR_DOWN, 1);
        }
        return true;
    }
    if ((released & PadButtons::Cross) && m_crossDown) {
        m_crossDown = false;
        evo_browser_pointer(pageX(), pageY(), EVOB_PTR_UP, 1);
        return true;
    }
    /* D-pad Left opens EVO's rail, as on every other screen (the right
     * stick still scrolls sideways). */
    if (pressed & raw & PadButtons::Left) {
        if (auto sm = Application::getInstance().getScreenManager()) sm->setRailFocused(true);
        return true;
    }
    const uint32_t dirs = PadButtons::Up | PadButtons::Down | PadButtons::Left | PadButtons::Right;
    if (pressed & dirs) {
        /* The left stick also arrives as D-pad presses; on the page it only
         * moves the pointer, so those are swallowed here. */
        int dx = 0, dy = 0;
        if (pressed & raw & PadButtons::Up) dy = -kDpadStep;
        if (pressed & raw & PadButtons::Down) dy = kDpadStep;
        if (pressed & raw & PadButtons::Right) dx = kDpadStep;
        if (ready && (dx || dy)) evo_browser_scroll(pageX(), pageY(), dx, dy);
        return true;
    }
    if (pressed & PadButtons::Square) {
        evo_feedback(EVO_FB_CONFIRM);
        editAddress();
        return true;
    }
    if (pressed & PadButtons::L1) {
        if (m_canBack) evo_browser_nav(EVOB_NAV_BACK);
        return true;
    }
    if (pressed & PadButtons::R1) {
        if (m_canForward) evo_browser_nav(EVOB_NAV_FORWARD);
        return true;
    }
    if (pressed & PadButtons::Triangle) {
        evo_browser_nav(m_loading ? EVOB_NAV_STOP : EVOB_NAV_RELOAD);
        return true;
    }
    if (pressed & PadButtons::Options) {
        if (!m_url.empty()) {
            const bool added = WebLibrary::instance().toggleBookmark(m_url, m_title);
            toast("WEB", added ? "Bookmarked" : "Bookmark removed");
        }
        return true;
    }
    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (ready && m_canBack) {
            evo_browser_nav(EVOB_NAV_BACK);
        } else if (auto sm = Application::getInstance().getScreenManager()) {
            sm->navigateTo(ScreenId::WebHome);
        }
        return true;
    }
    return false;
}

void WebPageScreen::render(uint32_t* framebuffer, int width, int height)
{
    char why[160] = "";
    const evo_browser_state_t st = evo_browser_state(why, sizeof why);
    evo_hui_webpage_params_t p;
    std::memset(&p, 0, sizeof p);
    p.url = m_url.c_str();
    p.title = m_title.c_str();
    p.loading = m_loading ? 1 : 0;
    p.can_back = m_canBack ? 1 : 0;
    p.can_forward = m_canForward ? 1 : 0;
    p.state = static_cast<int>(st);
    p.message = st == EVO_BROWSER_FAILED ? why
              : st == EVO_BROWSER_READY  ? "Loading the page..."
                                         : "Starting the browser...";
    p.pointer_visible = st == EVO_BROWSER_READY ? 1 : 0;
    p.pointer_x = static_cast<int>(m_px);
    p.pointer_y = static_cast<int>(m_py);
    p.pointer_hand = m_hand ? 1 : 0;
    p.hints_visible = 1;
    evo_rmlui_update_webpage(&p);
    evo_rmlui_render_webpage(framebuffer, width, height);
}

} // namespace evo
