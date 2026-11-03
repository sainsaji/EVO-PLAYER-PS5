#ifndef EVO_SUBTITLE_FILE_SCREEN_HPP
#define EVO_SUBTITLE_FILE_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include <string>
#include <vector>

namespace evo {

/*
 * Pick a subtitle file from anywhere on the console, for the video playing
 * now (Subtitles panel > LOAD FROM FILE...). Opens in the video's own folder;
 * Circle goes up a folder, and from a place's top back to the list of places:
 * this video's folder, every USB and NTFS drive, Internal Storage (/data) and
 * the whole console (/). Lists folders and .srt files; Cross on a file loads
 * it and makes it the subtitle track.
 */
class SubtitleFileScreen : public StatefulScreen {
public:
    SubtitleFileScreen();
    ~SubtitleFileScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::SubtitleFile; }
    void onEnter() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    struct Row {
        std::string name;    /* shown */
        std::string path;    /* full path or URL */
        std::string detail;
        std::string badge;
        bool isDir = false;
    };

    void showPlaces();
    void openFolder(const std::string& path, const std::string& label);
    void goUp();
    void activate();
    void leave();
    void navigate(int delta);

    std::vector<Row> m_rows;
    /* The folders entered from a place, innermost last; empty = the places. */
    std::vector<std::string> m_stack;
    std::string m_placeLabel;   /* the place the stack started from */
    int m_selectedIndex = 0;
    int m_scrollOffset = 0;
};

} // namespace evo

#endif // EVO_SUBTITLE_FILE_SCREEN_HPP
