#include "evo/screens/TextReaderScreen.hpp"
#include "evo/Application.hpp"
#include "evo_textreader.h"
#include "evo_screens.h"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_toast.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace evo {

static int ReaderMeasure(const char* s, int len, int face) {
    (void)s;
    int adv = (face == 0) ? 9 : (face == 1) ? 12 : (face == 2) ? 16 : 21;
    return len * adv;
}

static evo_text_doc s_doc;
static bool s_docLoaded = false;

TextReaderScreen::TextReaderScreen()
    : StatefulScreen("TextReaderScreen") {
}

void TextReaderScreen::openFile(const std::string& path) {
    m_filePath = path;
    if (s_docLoaded) {
        evo_text_free(&s_doc);
        s_docLoaded = false;
    }
    std::memset(&s_doc, 0, sizeof(s_doc));
    s_doc.face = m_fontFace;
    evo_text_load(&s_doc, path.c_str());

    int wrapW = evo_screen_reader_wrap_w();
    if (wrapW <= 0) wrapW = 1400;
    if (s_doc.status == EVO_TEXT_OK) {
        evo_text_wrap(&s_doc, wrapW, ReaderMeasure);
    }
    s_docLoaded = true;
    m_scrollLine = 0;
}

void TextReaderScreen::onEnter() {
    StatefulScreen::onEnter();
    if (!m_filePath.empty() && (!s_docLoaded || m_filePath != s_doc.path)) {
        openFile(m_filePath);
    }
}

void TextReaderScreen::onExit() {
    StatefulScreen::onExit();
}

void TextReaderScreen::scroll(int delta) {
    if (!s_docLoaded || s_doc.line_count == 0) return;

    int cap = evo_screen_reader_capacity(s_doc.face);
    if (cap <= 0) cap = 16;

    int oldTop = s_doc.top;
    evo_text_scroll(&s_doc, delta, cap);
    m_scrollLine = s_doc.top;

    if (s_doc.top == oldTop) {
        evo_feedback(EVO_FB_BOUNDARY);
    } else {
        evo_feedback(EVO_FB_MOVE);
    }
}

void TextReaderScreen::cycleFace() {
    m_fontFace = (m_fontFace + 1) % 3;
    if (s_docLoaded && s_doc.status == EVO_TEXT_OK) {
        s_doc.face = m_fontFace;
        int wrapW = evo_screen_reader_wrap_w();
        if (wrapW <= 0) wrapW = 1400;
        evo_text_wrap(&s_doc, wrapW, ReaderMeasure);
    }
    const char* names[] = { "SMALL", "MEDIUM", "LARGE" };
    toast("TEXT SIZE", names[m_fontFace % 3]);
}

bool TextReaderScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    if (pressed & PadButtons::Up) {
        scroll(-1);
        return true;
    }
    if (pressed & PadButtons::Down) {
        scroll(1);
        return true;
    }
    if (pressed & PadButtons::L1) {
        int cap = evo_screen_reader_capacity(s_doc.face);
        scroll(-(cap > 0 ? cap : 16));
        return true;
    }
    if (pressed & PadButtons::R1) {
        int cap = evo_screen_reader_capacity(s_doc.face);
        scroll(cap > 0 ? cap : 16);
        return true;
    }
    if (pressed & PadButtons::Triangle) {
        cycleFace();
        return true;
    }
    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto sm = Application::getInstance().getScreenManager()) {
            if (!sm->navigateBack()) {
                sm->navigateTo(ScreenId::UsbBrowser);
            }
        }
        return true;
    }

    return false;
}

void TextReaderScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
}

void TextReaderScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_reader_params_t params;
    std::memset(&params, 0, sizeof(params));

    if (!s_docLoaded) {
        params.title = "Document Viewer";
        params.subtitle = m_filePath.c_str();
        params.notice = "NO DOCUMENT LOADED";
        evo_rmlui_update_reader(&params);
        evo_rmlui_render_reader(framebuffer, width, height);
        return;
    }

    params.title = s_doc.title[0] ? s_doc.title : "Document Viewer";
    params.subtitle = s_doc.path[0] ? s_doc.path : nullptr;
    params.badge = "TEXT";
    params.face = s_doc.face;
    params.rail_focused = 0;

    switch (s_doc.status) {
        case EVO_TEXT_ERR_OPEN:   params.notice = "COULD NOT OPEN THIS FILE"; break;
        case EVO_TEXT_ERR_EMPTY:  params.notice = "THIS FILE IS EMPTY"; break;
        case EVO_TEXT_ERR_MEMORY: params.notice = "NOT ENOUGH MEMORY TO READ THIS FILE"; break;
        case EVO_TEXT_ERR_BINARY: params.notice = "THIS IS NOT A TEXT FILE"; break;
        default:
            params.notice = (s_doc.line_count > 0) ? nullptr : "NOTHING TO SHOW";
            break;
    }

    char badgeBuf[64];
    if (!params.notice && s_doc.lines && s_doc.line_count > 0 && s_doc.top >= 0 && s_doc.top < s_doc.line_count) {
        std::snprintf(badgeBuf, sizeof(badgeBuf), "LINE %d OF %d",
                      s_doc.lines[s_doc.top].source_line,
                      s_doc.source_lines);
        params.badge = badgeBuf;
    }

    int cap = evo_screen_reader_capacity(s_doc.face);
    if (cap <= 0) cap = 16;
    int shown = s_doc.line_count - s_doc.top;
    if (shown > cap) shown = cap;
    if (shown < 0) shown = 0;
    if (shown > EVO_RMLUI_READER_LINES) shown = EVO_RMLUI_READER_LINES;
    params.line_count = shown;

    if (s_doc.line_count > 0) {
        params.progress = evo_text_progress(&s_doc, cap);
        params.visible_frac = static_cast<double>(cap) / static_cast<double>(s_doc.line_count);
    }

    char footBuf[128];
    if (s_doc.truncated) {
        std::snprintf(footBuf, sizeof(footBuf),
                      "SHOWING THE FIRST %u KB OF %u KB",
                      static_cast<unsigned>(EVO_TEXT_MAX_BYTES / 1024u),
                      static_cast<unsigned>(s_doc.file_bytes / 1024u));
        params.footnote = footBuf;
    }

    static char lineBuffers[EVO_RMLUI_READER_LINES][512];
    for (int i = 0; i < shown; ++i) {
        int idx = s_doc.top + i;
        if (idx < s_doc.line_count && s_doc.lines && s_doc.lines[idx].begin) {
            int n = s_doc.lines[idx].len;
            if (n > 511) n = 511;
            std::memcpy(lineBuffers[i], s_doc.lines[idx].begin, n);
            lineBuffers[i][n] = '\0';
            params.lines[i] = lineBuffers[i];
        } else {
            lineBuffers[i][0] = '\0';
            params.lines[i] = lineBuffers[i];
        }
    }

    evo_rmlui_update_reader(&params);
    evo_rmlui_render_reader(framebuffer, width, height);
}

} // namespace evo
