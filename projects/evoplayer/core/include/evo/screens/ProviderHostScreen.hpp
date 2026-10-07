#ifndef EVO_PROVIDER_HOST_SCREEN_HPP
#define EVO_PROVIDER_HOST_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include <string>
#include <vector>

struct evo_provider;    /* evo_provider.h, C */

namespace evo {

/**
 * @brief The generic provider screen (#90).
 *
 * One screen class for every provider. It draws nothing itself: the markup on
 * screen is the provider's own, fetched at runtime and rendered by
 * EvoRmlProviderHost in its own Rml context. This class is only the bridge
 * between EVO's screen/input/playback machinery and that host.
 *
 * It fills the `ScreenId::EmbyBrowse` slot, which had no class registered at
 * all - the id existed, the rail could reach it and nothing answered. The id
 * keeps its old name and value because renaming it would touch the rail
 * geometry arithmetic in ScreenManager for no behavioural gain; see
 * EVO_ENABLE_PROVIDERS in evo_features.h for the same reasoning about the flag.
 *
 * WHAT IT DOES NOT DO
 *
 * It never inspects a catalog row, never formats a title, never decides which
 * row is selected. Selection lives in RmlUi's focus, the rows live in a bound
 * data model, and this class's entire contribution is: forward the D-pad, take
 * the activated item out of the host's mailbox, run it through the resolver
 * chain, and hand the result to PlaybackController.
 */
class ProviderHostScreen : public StatefulScreen {
public:
    ProviderHostScreen();
    ~ProviderHostScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::EmbyBrowse; }

    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

    /** Which provider to show on the next onEnter(). Empty picks the first
     *  enabled one, which is the behaviour when the rail slot is used. */
    static void setPendingProvider(const std::string& id);

    void setNavigatingToPlayer(bool b) { m_navigatingToPlayer = b; }

private:
    void startSelected();

    /*
     * #101: the rail slot is shared by every provider. With exactly one set
     * up it goes straight in, as before; otherwise onEnter() shows this
     * chooser first (drawn with the generic list document). X opens a
     * provider - or its source editor, when it has no source yet - and
     * SQUARE edits the source of the highlighted one.
     */
    void enterPicker();
    void choosePicked(bool editSource);
    void renderPicker(uint32_t* framebuffer, int width, int height);

    /*
     * The stream picker (Settings -> ASK WHICH LIVE STREAM).
     *
     * After a channel resolves, its streams are listed here - the URL as the
     * playlist gave it, each quality variant read from its HLS master, and the
     * .ts/.m3u8 swap EVO would otherwise try on its own (marked as a guess) -
     * and EVO plays the one chosen. Nothing is substituted: if the chosen
     * stream does not open, the list comes back rather than another stream
     * being tried behind the user's back. With the setting off none of this
     * runs and a channel plays exactly as it always did.
     */
    void enterStreamPicker();
    void chooseStream(int index);
    void cancelStreamPicker();
    void renderStreamPicker(uint32_t* framebuffer, int width, int height);
    bool m_streamPick = false;
    int  m_streamIndex = 0;

    /* Open `id`: an RmlUi bundle for most providers, the system browser for
     * one with EVO_PROVIDER_CAP_WEBUI (openWebProvider). */
    void openProvider(const std::string& id);
    void openWebProvider();

    /*
     * Open the virtual keyboard on the hosted provider's source string - an
     * M3U URL for IPTV - and hand whatever comes back to set_source().
     *
     * Deliberately generic: it goes through EVO_PROVIDER_CAP_CONFIG, so this
     * screen still does not know which provider it is hosting. A richer setup
     * flow (an Xtream host/user/password triple, Emby credentials) belongs in
     * that provider's own screen; this is the one-line case, which is what an
     * M3U link is.
     *
     * Goes through evo_keyboard_open(), which picks the native PS5 IME when it
     * is available and falls back to the virtual keyboard when it is not. The
     * native path works on FW 12.70 - #34 is fixed, and the boot log says so
     * ("ime: native IME ready"). An earlier version of this comment claimed
     * the virtual keyboard was mandatory; that was stale.
     *
     * Worth knowing: the virtual fallback currently cannot DRAW on this
     * screen. evo_screen_keyboard() is the only thing that renders it and
     * nothing calls it any more, so if the native IME ever fails here the
     * prompt would be invisible while still swallowing input.
     */
    void openSourceEditor();
    /* A channel's address turned out to be a playlist: use it as the IPTV source. */
    bool adoptChannelPlaylist(const char* url);
    static void OnSourceSubmitted(const char* text, void* userdata);
    static void OnGuideSubmitted(const char* text, void* userdata);

    void browseUsb();
    static std::vector<std::string> scanUsbPlaylists();

    void openSearch();
    static void OnSearchSubmitted(const char* text, void* userdata);

    /* The provider this screen is currently hosting. Held so onExit can close
     * the right host even if the pending id has since changed. */
    std::string m_providerId;
    std::string m_searchQuery;
    std::string m_lastTypedUrl;
    bool m_opened = false;
    /* When navigating to Player, preserve the provider UI context, folder stack,
     * and channel selection so returning from playback lands back on the channel. */
    bool m_navigatingToPlayer = false;
    /* A resolve is in flight for the activated item; another activation is
     * ignored until it lands, so a double press cannot start two playbacks. */
    bool m_resolving = false;
    /* Two-frame deferral so the "Tuning..." spinner and toast render to the
     * display before startPlaybackSource() blocks the thread on network I/O. */
    bool m_tunePending = false;
    int  m_tuneFrames = 0;

    /* #101: the chooser (see enterPicker). */
    bool m_picking = false;
    int  m_pickIndex = 0;
    std::vector<std::string> m_pickIds;
    std::vector<std::string> m_pickDetail;

    /* #101: hosting a web-UI provider. The browser is a system overlay, so
     * m_opened (the RmlUi host) stays false; m_webSeen notes that the session
     * really started, so its end can be told from "not open yet". */
    bool m_web = false;
    bool m_webSeen = false;

    /* Looking for an Emby/Jellyfin server on the LAN before the address
     * keyboard opens, so the field can be pre-filled (evo_net_discover_*). */
    bool m_discovering = false;
    void openSourceKeyboard(const std::string& initial);
    void openProviderMenu(int focus = 0); /* IPTV: Options -> side panel        */
    void openGuidePicker();
    void applyChoice(const std::string& id);
    std::string m_panelPage;              /* "menu" / "guide" while the panel is up */

    /* The chooser's OPTIONS menu: everything that can be done to the focused
     * provider (open, change server, sign in again, sign out, web version). */
    struct MenuItem { std::string id, label, desc, icon; bool danger = false; };
    std::vector<MenuItem> m_menu;         /* empty = closed */
    int m_menuIndex = 0;
    void openPickerMenu();
    void runPickerMenu(const std::string& id);
    bool pickerMenuInput(uint32_t pressed);
    void discoverServer();                /* LAN search, then the address keyboard */

    /* Signing in to a media server (Emby, Jellyfin) on EVO's keyboard: user
     * name, then password, then the provider's sign_in. Keyboards are opened
     * from update(), never from a keyboard callback - the keyboard closes
     * itself AFTER calling back, which would close the next one too. */
    enum class SignIn { None, QcStart, QcWait, QcPolling, Suggest, OpenUser, User,
                        OpenPass, Pass, Signing };
    SignIn m_signIn = SignIn::None;
    std::string m_signInUser;
    std::string m_signInPass;
    void beginSignIn();
    void beginPasswordSignIn();
    void signInStep();
    bool signInQcInput(uint32_t pressed);
    void renderQuickConnect(uint32_t* framebuffer, int width, int height);
    static void OnQcCode(int ok, const char* code, void* ud);
    static void OnQcPoll(int state, const char* msg, void* ud);
    std::string m_qcCode;
    long long m_qcNextPollMs = 0;
    long long m_qcDeadlineMs = 0;
    static void OnSignInSuggest(const char* name, void* ud);
    static void OnSignInUser(const char* text, void* ud);
    static void OnSignInPass(const char* text, void* ud);
    static void OnSignInDone(int ok, const char* msg, void* ud);

    /* Emby/Jellyfin: browsed natively, with the site in the browser as an
     * extra. Nuvio: web only. */
    static bool isNativeWeb(const ::evo_provider* p);
};

} // namespace evo

#endif // EVO_PROVIDER_HOST_SCREEN_HPP
