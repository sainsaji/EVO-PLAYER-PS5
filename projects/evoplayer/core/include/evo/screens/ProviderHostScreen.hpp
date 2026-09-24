#ifndef EVO_PROVIDER_HOST_SCREEN_HPP
#define EVO_PROVIDER_HOST_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include <string>

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

private:
    void startSelected();

    /* The provider this screen is currently hosting. Held so onExit can close
     * the right host even if the pending id has since changed. */
    std::string m_providerId;
    bool m_opened = false;
    /* A resolve is in flight for the activated item; another activation is
     * ignored until it lands, so a double press cannot start two playbacks. */
    bool m_resolving = false;
};

} // namespace evo

#endif // EVO_PROVIDER_HOST_SCREEN_HPP
