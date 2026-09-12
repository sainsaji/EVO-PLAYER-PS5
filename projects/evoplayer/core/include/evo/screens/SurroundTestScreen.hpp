#ifndef EVO_SURROUND_TEST_SCREEN_HPP
#define EVO_SURROUND_TEST_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"

namespace evo {

class SurroundTestScreen : public StatefulScreen {
public:
    SurroundTestScreen();
    ~SurroundTestScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::SurroundTest; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    void navigate(int dir);
    void playSelectedChannel();

    int m_selectedSpeaker = 0;
};

} // namespace evo

#endif // EVO_SURROUND_TEST_SCREEN_HPP
