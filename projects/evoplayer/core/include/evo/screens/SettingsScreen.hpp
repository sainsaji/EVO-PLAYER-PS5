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
    void adjustValue(int delta);

    int m_selectedIndex = 0;
};

class SettingsPlaybackScreen : public StatefulScreen {
public:
    SettingsPlaybackScreen();
    ~SettingsPlaybackScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::SettingsPlayback; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    void navigate(int delta);
    void activateSelection();

    int m_selectedIndex = 0;
    /* Which VALUE setting is expanded to show its choices, -1 = none. */
    int m_expandedIndex = -1;
};

class SettingsSubtitlesScreen : public StatefulScreen {
public:
    SettingsSubtitlesScreen();
    ~SettingsSubtitlesScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::SettingsSubtitles; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    void navigate(int delta);
    void activateSelection();

    int m_selectedIndex = 0;
    /* Which VALUE setting is expanded to show its choices, -1 = none. */
    int m_expandedIndex = -1;
};

class SettingsInterfaceScreen : public StatefulScreen {
public:
    SettingsInterfaceScreen();
    ~SettingsInterfaceScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::SettingsInterface; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    void navigate(int delta);
    void activateSelection();

    int m_selectedIndex = 0;
    /* Which VALUE setting is expanded to show its choices, -1 = none. */
    int m_expandedIndex = -1;
};

class SettingsSystemScreen : public StatefulScreen {
public:
    SettingsSystemScreen();
    ~SettingsSystemScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::SettingsSystem; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    void navigate(int delta);
    void activateSelection();

    int m_selectedIndex = 0;
    /* Which VALUE setting is expanded to show its choices, -1 = none. */
    int m_expandedIndex = -1;
};

} // namespace evo

#endif // EVO_SETTINGS_SCREEN_HPP
