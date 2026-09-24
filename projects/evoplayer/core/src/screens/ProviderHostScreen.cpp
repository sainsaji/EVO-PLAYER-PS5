#include "evo/screens/ProviderHostScreen.hpp"
#include "evo/Application.hpp"
#include "evo/screens/ScreenManager.hpp"
#include "evo/services/PlaybackController.hpp"

#include "evo_rmlui_provider.h"
#include "evo_rmlui_bridge.h"   /* evo_rmlui_render_nav_overlay */
#include "evo_feedback.h"
#include "evo_toast.h"

extern "C" {
#include "evo_provider.h"
#include "evo_boot_trace.h"
}

#include <cstdio>
#include <cstring>
#include <string>

namespace evo {

namespace {

/* Set by the launch tile / rail before navigating here. Empty means "whichever
 * provider is enabled", which is what the single shared rail slot does. */
std::string g_pendingProvider;

/*
 * The resolve callback's state.
 *
 * File-scope rather than a member because the callback is a C function pointer
 * with a void* and the screen outlives the process; carrying the screen pointer
 * would be an invitation to use it after a navigate. What it actually needs is
 * only the title and identity to build a PlaybackSource, all of which is copied
 * here at activation time.
 */
struct PendingPlay {
    char provider[EVO_PROVIDER_MAX_ID];
    char item_id[EVO_PROVIDER_MAX_ITEM_ID];
    char title[EVO_PROVIDER_MAX_TITLE];
    int  is_live;
};
PendingPlay g_pending;

/*
 * Set by on_resolved, cleared by update(). The callback must not reach back
 * into the screen - it can land on a frame after the user has navigated away -
 * so it reports completion through a flag the screen polls instead.
 */
bool g_resolve_finished = false;

void on_resolved(int ok, const evo_stream_choice_t* choices, int count, void* ud)
{
    PendingPlay* pp = (PendingPlay*)ud;
    g_resolve_finished = true;

    if (!ok || count <= 0 || !choices) {
        evo_bt("provider: resolve failed for %s/%s", pp->provider, pp->item_id);
        toast("STREAM", "That item could not be played");
        return;
    }

    /*
     * Best first, by contract. A quality picker over `choices` is a
     * per-provider story - the seam's job is to have produced more than one
     * and said which is preferred.
     */
    const evo_stream_choice_t& c = choices[0];

    PlaybackSource src;
    src.url      = c.url;
    src.title    = pp->title;
    src.provider = pp->provider;
    src.item_id  = pp->item_id;
    /* Either side may know it is live: the provider's catalog said so, or the
     * resolved choice did (an HLS playlist with no EXT-X-ENDLIST). */
    src.is_live  = (pp->is_live || c.is_live) ? true : false;

    auto pb = Application::getInstance().getPlaybackController();
    if (!pb) return;

    if (!pb->startPlaybackSource(src, 0.0)) {
        /* startPlaybackSource has already toasted the reason. Stay on the
         * provider screen rather than navigating to a player with nothing in
         * it - a black Player screen with working transport controls is the
         * most confusing possible outcome. */
        return;
    }

    if (auto sm = Application::getInstance().getScreenManager())
        sm->navigateTo(ScreenId::Player);
}

} // namespace

ProviderHostScreen::ProviderHostScreen()
    : StatefulScreen("ProviderHostScreen") {
}

void ProviderHostScreen::setPendingProvider(const std::string& id)
{
    g_pendingProvider = id;
}

void ProviderHostScreen::onEnter()
{
    StatefulScreen::onEnter();
    m_resolving = false;

    std::string want = g_pendingProvider;
    if (want.empty()) {
        /*
         * No explicit choice: the first enabled provider. With one rail slot
         * shared by all of them this is what the slot means, and a provider
         * picker is a separate screen for when there is more than one set up.
         */
        for (int i = 0; i < evo_provider_count(); ++i) {
            const evo_provider_t* p = evo_provider_at(i);
            if (p && evo_provider_is_enabled(p->id)) { want = p->id; break; }
        }
    }

    if (want.empty()) {
        /* Nothing configured. Not an error - it is the state a fresh install
         * is in - so say so and send the user back rather than opening an
         * empty host. */
        toast("PROVIDERS", "No provider is set up yet");
        if (auto sm = Application::getInstance().getScreenManager())
            sm->navigateTo(ScreenId::Settings);
        return;
    }

    m_providerId = want;
    m_opened = evo_rmlui_provider_open(m_providerId.c_str(),
                                        DisplayWidth, DisplayHeight) != 0;
    if (!m_opened) {
        evo_bt("provider: could not open host for '%s'", m_providerId.c_str());
        toast("PROVIDERS", "That provider is not available");
        if (auto sm = Application::getInstance().getScreenManager())
            sm->navigateTo(ScreenId::MainMenu);
    }
}

void ProviderHostScreen::onExit()
{
    if (m_opened) {
        evo_rmlui_provider_close();
        m_opened = false;
    }
    m_resolving = false;
    StatefulScreen::onExit();
}

bool ProviderHostScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released)
{
    (void)held;
    (void)released;
    if (!m_opened) return false;

    auto sm = Application::getInstance().getScreenManager();
    bool railFocused = sm && sm->isRailFocused();

    /* While the rail has focus the provider document must not move: the rail
     * owns Up/Down and Left/Right in that state, exactly as on every other
     * screen. */
    if (railFocused) return false;

    /*
     * Everything below goes into RmlUi. EVO does not translate a direction into
     * a row index anywhere in this function - that is the difference between
     * this screen and every other one in the tree.
     */
    if (pressed & PadButtons::Up)
        return evo_rmlui_provider_key(EvoRmlProviderHost::KeyUp) != 0;
    if (pressed & PadButtons::Down)
        return evo_rmlui_provider_key(EvoRmlProviderHost::KeyDown) != 0;
    if (pressed & PadButtons::Right)
        return evo_rmlui_provider_key(EvoRmlProviderHost::KeyRight) != 0;

    if (pressed & PadButtons::Left) {
        /* Let the document have it first - a grid's leftmost column is where
         * the rail should take over, and only the document knows where that
         * is. An unconsumed Left focuses the rail. */
        if (evo_rmlui_provider_key(EvoRmlProviderHost::KeyLeft) != 0)
            return true;
        if (sm) { sm->setRailFocused(true); return true; }
        return false;
    }

    if (pressed & PadButtons::Cross) {
        if (m_resolving) return true;      /* one activation at a time */
        evo_feedback(EVO_FB_OPEN);
        return evo_rmlui_provider_key(EvoRmlProviderHost::KeyAccept) != 0;
    }

    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        /*
         * The exit contract. The host consumes Back while it is deeper than the
         * provider's root and pops a level; at the root it declines, and the
         * screen leaves. A provider that swallowed Back at its root would be a
         * screen the user cannot get out of, and the only remaining way out is
         * the PS button - the close path that panicked the console.
         */
        if (evo_rmlui_provider_key(EvoRmlProviderHost::KeyBack) != 0)
            return true;
        if (sm) sm->navigateTo(ScreenId::MainMenu);
        return true;
    }

    if (pressed & PadButtons::Square)
        return evo_rmlui_provider_key(EvoRmlProviderHost::KeySearch) != 0;

    return false;
}

void ProviderHostScreen::startSelected()
{
    evo_provider_selection_t sel;
    if (!evo_rmlui_provider_take_selection(&sel)) return;
    if (m_resolving) return;

    std::memset(&g_pending, 0, sizeof g_pending);
    std::snprintf(g_pending.provider, sizeof g_pending.provider, "%s", sel.provider_id);
    std::snprintf(g_pending.item_id,  sizeof g_pending.item_id,  "%s", sel.item_id);
    std::snprintf(g_pending.title,    sizeof g_pending.title,    "%s", sel.title);
    g_pending.is_live = sel.is_live;
    g_resolve_finished = false;

    /*
     * The resolver chain, not the provider's resolve() directly. That is what
     * lets a catalog item whose "URL" is a magnet be handed to a debrid
     * provider for the playable one, without either provider knowing the other
     * exists - and it costs nothing when, as with IPTV, the first answer is
     * already playable.
     */
    m_resolving = true;
    if (evo_provider_resolve_chain(sel.provider_id, sel.item_id,
                                    on_resolved, &g_pending) != 0) {
        m_resolving = false;
        toast("STREAM", "That item could not be played");
    }
}

void ProviderHostScreen::update(double deltaMs)
{
    StatefulScreen::update(deltaMs);
    if (!m_opened) return;

    /* Bundle refresh, artwork decode, and any catalog reply that landed. */
    evo_rmlui_provider_tick();

    /*
     * Take the activated item here rather than inside the Rml event handler
     * that produced it. Starting playback tears down and rebuilds the VideoOut
     * configuration, and doing that from underneath Context::Update() - which
     * is still walking the element tree that raised the event - is the shape of
     * bug that cost two console cycles in #32.
     */
    startSelected();

    /* A resolve that finished - either way - releases the guard, so a failed
     * one does not leave the screen permanently unable to activate anything. */
    if (m_resolving && g_resolve_finished) {
        m_resolving = false;
        g_resolve_finished = false;
    }
}

void ProviderHostScreen::render(uint32_t* framebuffer, int width, int height)
{
    if (!m_opened) return;
    evo_rmlui_provider_render(framebuffer, width, height);
    /* The rail is a document in the MAIN context, so it does not come with the
     * provider's own context - it is composited on top afterwards. */
    evo_rmlui_render_nav_overlay(framebuffer, width, height);
    evo_rmlui_provider_clear_frame();
}

} // namespace evo
