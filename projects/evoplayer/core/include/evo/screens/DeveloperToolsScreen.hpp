#ifndef EVO_DEVELOPER_TOOLS_SCREEN_HPP
#define EVO_DEVELOPER_TOOLS_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"

namespace evo {

class DeveloperToolsScreen : public StatefulScreen {
public:
    DeveloperToolsScreen();
    ~DeveloperToolsScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::DeveloperTools; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    void navigate(int delta);
    void activateSelection();

    int m_selectedIndex = 0;
};

} // namespace evo

#endif // EVO_DEVELOPER_TOOLS_SCREEN_HPP
