#ifndef EVO_SETTINGS_SCREEN_HPP
#define EVO_SETTINGS_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"

namespace evo {

class SettingsScreen : public StatefulScreen {
public:
    SettingsScreen();
    ~SettingsScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::Settings; }
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

/*
 * One settings section (VIDEO & DISPLAY, AUDIO, ...). Every section page
 * behaves the same; only the list of settings differs, and that list lives in
 * SettingsScreen.cpp's section table, keyed by the ScreenId passed here.
 */
class SettingsSectionScreen : public StatefulScreen {
public:
    explicit SettingsSectionScreen(ScreenId id);
    ~SettingsSectionScreen() override = default;

    ScreenId getScreenId() const override { return m_id; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    void navigate(int delta);
    void activateSelection();

    ScreenId m_id;
    int m_section;
    int m_selectedIndex = 0;
    /* Which VALUE setting is expanded to show its choices, -1 = none. */
    int m_expandedIndex = -1;
};

} // namespace evo

#endif // EVO_SETTINGS_SCREEN_HPP
