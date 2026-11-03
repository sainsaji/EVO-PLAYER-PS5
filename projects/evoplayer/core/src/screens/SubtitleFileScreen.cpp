#include "evo/screens/SubtitleFileScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_toast.h"
#include "evo_readdir.h"
#include "evo_ntfs.h"
#include "evo_subtitle.h"
#include "evo_boot_log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

extern "C" char current_media_path[768];

namespace evo {

static const int kMaxRows = 2000;

SubtitleFileScreen::SubtitleFileScreen()
    : StatefulScreen("SubtitleFileScreen") {
}

static bool hasSubtitleExt(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == "srt";
}

static bool lessNoCase(const std::string& a, const std::string& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const int x = std::tolower(static_cast<unsigned char>(a[i]));
        const int y = std::tolower(static_cast<unsigned char>(b[i]));
        if (x != y) return x < y;
    }
    return a.size() < b.size();
}

/* The folder of the video playing now: a local path or an ntfs:// URL. */
static std::string videoFolder() {
    const std::string p = current_media_path;
    if (p.empty() || (p[0] != '/' && !evo_ntfs_is_url(p.c_str()))) return "";
    const size_t slash = p.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return p[0] == '/' ? "/" : "";
    return p.substr(0, slash);
}

void SubtitleFileScreen::onEnter() {
    StatefulScreen::onEnter();
    m_stack.clear();
    const std::string here = videoFolder();
    if (!here.empty()) openFolder(here, "This video's folder");
    else showPlaces();
}

void SubtitleFileScreen::showPlaces() {
    m_stack.clear();
    m_placeLabel.clear();
    m_rows.clear();
    auto add = [this](const std::string& name, const std::string& path, const std::string& detail) {
        Row r;
        r.name = name;
        r.path = path;
        r.detail = detail;
        r.isDir = true;
        m_rows.push_back(std::move(r));
    };
    const std::string here = videoFolder();
    if (!here.empty()) add("This video's folder", here, here);
    if (auto fs = Application::getInstance().getFileSystemBrowser()) {
        for (const auto& m : fs->usbMounts()) add(fs->usbMountLabel(m), m, m);
    }
    add("Internal Storage", "/data", "/data");
    add("Whole console", "/", "Every folder EVO can see");
    m_selectedIndex = 0;
    m_scrollOffset = 0;
}

void SubtitleFileScreen::openFolder(const std::string& path, const std::string& label) {
    evo_dir_t* d = evo_opendir(path.c_str());
    if (!d) {
        toast("SUBTITLES", "That folder cannot be opened");
        evo_feedback(EVO_FB_ERROR);
        if (m_stack.empty()) showPlaces();
        return;
    }
    std::vector<Row> dirs, files;
    struct dirent* e;
    while ((e = evo_readdir(d)) != nullptr && static_cast<int>(dirs.size() + files.size()) < kMaxRows) {
        const std::string name = e->d_name;
        if (name.empty() || name[0] == '.' || name[0] == '$') continue;
        if (name == "System Volume Information") continue;   /* Windows' own folder on NTFS/exFAT drives */
        const std::string full = (path == "/" ? std::string("/") : path + "/") + name;
        bool isDir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
            struct stat st;
            isDir = stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }
        Row r;
        r.name = name;
        r.path = full;
        r.isDir = isDir;
        if (isDir) {
            r.detail = "Folder";
            dirs.push_back(std::move(r));
        } else if (hasSubtitleExt(name)) {
            r.detail = "Subtitle file";
            r.badge = "SRT";
            files.push_back(std::move(r));
        }
    }
    evo_closedir(d);
    std::sort(dirs.begin(), dirs.end(), [](const Row& a, const Row& b) { return lessNoCase(a.name, b.name); });
    std::sort(files.begin(), files.end(), [](const Row& a, const Row& b) { return lessNoCase(a.name, b.name); });
    m_rows = std::move(files);   /* subtitles first: they are what this screen is for */
    m_rows.insert(m_rows.end(), dirs.begin(), dirs.end());
    if (m_stack.empty()) m_placeLabel = label;
    m_stack.push_back(path);
    m_selectedIndex = 0;
    m_scrollOffset = 0;
}

void SubtitleFileScreen::goUp() {
    if (m_stack.empty()) {
        leave();
        return;
    }
    m_stack.pop_back();
    if (m_stack.empty()) {
        showPlaces();
        return;
    }
    const std::string parent = m_stack.back();
    m_stack.pop_back();
    openFolder(parent, m_placeLabel);
}

void SubtitleFileScreen::leave() {
    if (auto sm = Application::getInstance().getScreenManager()) {
        if (!sm->navigateBack()) sm->navigateTo(ScreenId::Player);
    }
}

void SubtitleFileScreen::activate() {
    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_rows.size())) {
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    }
    const Row r = m_rows[m_selectedIndex];
    if (r.isDir) {
        evo_feedback(EVO_FB_CONFIRM);
        openFolder(r.path, m_stack.empty() ? r.name : m_placeLabel);
        return;
    }
    const int cues = prospero_subtitle_load_external(r.path.c_str());   /* toasts "Loaded N cues" */
    if (cues <= 0 || !prospero_subtitle_select_primary(-1)) {
        EVO_LOGW("subtitles", "picked %s: no cues read", r.path.c_str());
        toast("SUBTITLES", "No subtitles could be read from that file");
        evo_feedback(EVO_FB_ERROR);
        return;
    }
    EVO_LOGI("subtitles", "picked %s: %d cues", r.path.c_str(), cues);
    evo_feedback(EVO_FB_CONFIRM);
    if (auto sm = Application::getInstance().getScreenManager()) sm->navigateTo(ScreenId::Player);
}

void SubtitleFileScreen::navigate(int delta) {
    const int count = static_cast<int>(m_rows.size());
    if (count == 0) return;
    int next = m_selectedIndex + delta;
    if (next < 0 || next >= count) {
        next = std::max(0, std::min(count - 1, next));
        if (next == m_selectedIndex) {
            evo_feedback(EVO_FB_BOUNDARY);
            return;
        }
    }
    m_selectedIndex = next;
    if (m_selectedIndex >= m_scrollOffset + EVO_RMLUI_LIST_ROWS) m_scrollOffset = m_selectedIndex - EVO_RMLUI_LIST_ROWS + 1;
    if (m_selectedIndex < m_scrollOffset) m_scrollOffset = m_selectedIndex;
    evo_feedback(EVO_FB_MOVE);
}

bool SubtitleFileScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;
    if (pressed & PadButtons::Up) { navigate(-1); return true; }
    if (pressed & PadButtons::Down) { navigate(1); return true; }
    if (pressed & PadButtons::L1) { navigate(-EVO_RMLUI_LIST_ROWS); return true; }
    if (pressed & PadButtons::R1) { navigate(EVO_RMLUI_LIST_ROWS); return true; }
    if (pressed & PadButtons::Cross) { activate(); return true; }
    if (pressed & PadButtons::Triangle) {   /* straight back to the places */
        evo_feedback(EVO_FB_CANCEL);
        showPlaces();
        return true;
    }
    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        goUp();
        return true;
    }
    return true;   /* the player underneath must not see keys meant for the picker */
}

void SubtitleFileScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_list_params_t params;
    std::memset(&params, 0, sizeof(params));

    static std::string s_where;
    if (m_stack.empty()) {
        s_where = "Choose where to look";
    } else {
        /* "USB DRIVE (usb0) / Movies / Extras" - the path under the place. */
        s_where = m_placeLabel;
        const std::string& top = m_stack.front();
        const std::string& cur = m_stack.back();
        if (cur.size() > top.size()) s_where += " " + cur.substr(top.size() == 1 ? 0 : top.size());
    }
    params.title = "Load Subtitle File";
    params.subtitle = s_where.c_str();
    params.section = -1;
    params.rail_focused = 0;

    const int total = static_cast<int>(m_rows.size());
    params.total_count = total;
    params.cursor_index = total ? m_selectedIndex : -1;
    if (total == 0) {
        params.is_empty = 1;
        params.empty_title = "No subtitle files here";
        params.empty_hint = "Only .srt files and folders are listed. Circle goes up a folder.";
        params.empty_icon = "../icons/icon_subtitles.png";
    } else {
        const int rows = std::min(EVO_RMLUI_LIST_ROWS, std::max(0, total - m_scrollOffset));
        params.row_count = rows;
        for (int i = 0; i < rows; ++i) {
            const Row& r = m_rows[m_scrollOffset + i];
            params.rows[i].title = r.name.c_str();
            params.rows[i].detail = r.detail.c_str();
            params.rows[i].badge = r.badge.c_str();
            params.rows[i].icon_path = r.isDir ? "../icons/icon_folder.png" : "../icons/icon_subtitles.png";
            params.rows[i].progress = -1;
            params.rows[i].has_chevron = r.isDir ? 1 : 0;
            params.rows[i].is_focused = (m_scrollOffset + i == m_selectedIndex);
        }
    }

    params.hint_count = 3;
    params.hints[0].glyph_path = "../icons/btn_cross.png";
    params.hints[0].label = (total && !m_rows[m_selectedIndex].isDir) ? "LOAD" : "OPEN";
    params.hints[1].glyph_path = "../icons/btn_triangle.png";
    params.hints[1].label = "PLACES";
    params.hints[2].glyph_path = "../icons/btn_circle.png";
    params.hints[2].label = m_stack.empty() ? "BACK" : "UP";

    evo_rmlui_update_list(&params);
    evo_rmlui_render_list(framebuffer, width, height);
}

} // namespace evo
