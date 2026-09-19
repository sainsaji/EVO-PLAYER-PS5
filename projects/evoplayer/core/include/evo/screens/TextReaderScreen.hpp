#ifndef EVO_TEXT_READER_SCREEN_HPP
#define EVO_TEXT_READER_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include <string>

namespace evo {

class TextReaderScreen : public StatefulScreen {
public:
    TextReaderScreen();
    ~TextReaderScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::TextReader; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

    void openFile(const std::string& path);

private:
    void scroll(int delta);
    void cycleFace();

    std::string m_filePath;
    int m_scrollLine = 0;
    int m_fontFace = 1; // Medium
};

} // namespace evo

#endif // EVO_TEXT_READER_SCREEN_HPP
