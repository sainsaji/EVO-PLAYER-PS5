#ifndef EVO_RECENT_FILES_SCREEN_HPP
#define EVO_RECENT_FILES_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"

namespace evo {

class RecentFilesScreen : public StatefulScreen {
public:
    RecentFilesScreen();
    ~RecentFilesScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::RecentFiles; }
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

#endif // EVO_RECENT_FILES_SCREEN_HPP
