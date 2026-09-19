#ifndef EVO_CHANGELOG_SCREEN_HPP
#define EVO_CHANGELOG_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include <string>

namespace evo {

class ChangelogScreen : public StatefulScreen {
public:
    ChangelogScreen();
    ~ChangelogScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::Changelog; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    void navigate(int delta);

    int m_cursorIndex = 0;
    int m_scrollOffset = 0;
};

} // namespace evo

#endif // EVO_CHANGELOG_SCREEN_HPP
