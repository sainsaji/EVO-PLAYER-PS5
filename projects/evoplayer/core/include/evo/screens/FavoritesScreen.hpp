#ifndef EVO_FAVORITES_SCREEN_HPP
#define EVO_FAVORITES_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"

namespace evo {

class FavoritesScreen : public StatefulScreen {
public:
    FavoritesScreen();
    ~FavoritesScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::Favorites; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    int m_selectedIndex = 0;
    int m_scrollOffset = 0;
};

} // namespace evo

#endif // EVO_FAVORITES_SCREEN_HPP
