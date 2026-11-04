#ifndef EVO_WEB_SCREENS_HPP
#define EVO_WEB_SCREENS_HPP

/*
 * The Web section (#237, docs/ui/browser.md): a home list (address, bookmarks,
 * history) and the page view, drawn from the out-of-process browser engine
 * (include/evo_browser_engine.h).
 */

#include "evo/screens/StatefulScreen.hpp"

#include <string>
#include <vector>

namespace evo {

/* Bookmarks and history, kept in EVO's data folder (web_bookmarks.txt,
 * web_history.txt: one "url<TAB>title" per line). Main thread only. */
struct WebEntry {
    std::string url;
    std::string title;
};

class WebLibrary {
public:
    static WebLibrary& instance();

    const std::vector<WebEntry>& bookmarks();
    const std::vector<WebEntry>& history();
    /* A page was shown: it goes to the top of the history (at most kMaxHistory). */
    void noteVisit(const std::string& url, const std::string& title);
    /* Adds the page, or removes it when it is bookmarked. true: now bookmarked. */
    bool toggleBookmark(const std::string& url, const std::string& title);
    bool isBookmarked(const std::string& url);
    void clearHistory();

    static constexpr size_t kMaxHistory = 50;

private:
    void load();
    void save(const char* leaf, const std::vector<WebEntry>& list);
    bool m_loaded = false;
    std::vector<WebEntry> m_bookmarks;
    std::vector<WebEntry> m_history;
};

class WebHomeScreen : public StatefulScreen {
public:
    WebHomeScreen();

    ScreenId getScreenId() const override { return ScreenId::WebHome; }
    void onEnter() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    enum class RowKind { Address, Bookmark, History };
    struct Row {
        RowKind kind;
        WebEntry entry;
    };
    void rebuild();
    void openAddress();
    static void onAddressSubmitted(const char* text, void* self);

    std::vector<Row> m_rows;
    int m_selected = 0;
    int m_scroll = 0;
};

class WebPageScreen : public StatefulScreen {
public:
    WebPageScreen();

    ScreenId getScreenId() const override { return ScreenId::WebPage; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

    /* The page to show when the screen opens (or now, if it is open). */
    void openUrl(const std::string& url);

private:
    void pollEvents();
    void movePointer(double dtMs);
    void scrollWithStick(double dtMs);
    void editAddress();
    static void onAddressSubmitted(const char* text, void* self);
    static void onFieldSubmitted(const char* text, void* self);
    int pageX() const;
    int pageY() const;

    std::string m_url;
    std::string m_title;
    std::string m_titleUrl;     /* m_url when m_title arrived */
    std::string m_visitedUrl;   /* the last page written to history (a finished load) */
    std::string m_pendingUrl;
    bool m_loading = false;
    bool m_canBack = false;
    bool m_canForward = false;
    bool m_setupSent = false;   /* resize + visible sent since the engine got ready */
    bool m_hand = false;
    bool m_crossDown = false;
    float m_px = 960.0f;        /* pointer, screen pixels */
    float m_py = 540.0f;
    int m_sentX = -1, m_sentY = -1;
    float m_scrollAccX = 0.0f, m_scrollAccY = 0.0f;
    long long m_lastClickMs = -100000;
};

} // namespace evo

#endif // EVO_WEB_SCREENS_HPP
