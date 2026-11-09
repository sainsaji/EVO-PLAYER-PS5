// Prospero Browser - WPE WebKit Port for PlayStation 5
// Browser Settings & Configuration implementation.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later

#include "browser_settings.hpp"
#include <cstdio>
#include <cstdlib>

namespace prospero::browser {

std::string BrowserSettings::user_agent_string() const {
    switch (user_agent_mode) {
        case UserAgentMode::DesktopChrome:
            return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                   "(KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36";
        case UserAgentMode::DesktopSafari:
            return "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_5) AppleWebKit/605.1.15 "
                   "(KHTML, like Gecko) Version/17.5 Safari/605.1.15";
        case UserAgentMode::MobileSafari:
            return "Mozilla/5.0 (iPhone; CPU iPhone OS 17_5 like Mac OS X) AppleWebKit/605.1.15 "
                   "(KHTML, like Gecko) Version/17.5 Mobile/15E148 Safari/604.1";
        case UserAgentMode::ChromecastTV:
            return "Mozilla/5.0 (Linux; Android 12; Chromecast Build/STTE.221215.004) AppleWebKit/537.36 "
                   "(KHTML, like Gecko) Chrome/108.0.0.0 Safari/537.36 CrKey/1.56.500000";
        case UserAgentMode::ProsperoPS5:
        default:
            return "Mozilla/5.0 (PlayStation; PlayStation 5/1.0) AppleWebKit/605.1.15 "
                   "(KHTML, like Gecko) Version/17.0 Safari/605.1.15 ProsperoBrowser/1.0";
    }
}

std::string BrowserSettings::engine_user_agent() const {
    return user_agent_mode == UserAgentMode::ProsperoPS5 ? std::string() : user_agent_string();
}

const char* user_agent_mode_name(UserAgentMode mode) {
    switch (mode) {
        case UserAgentMode::DesktopChrome: return "Desktop (Chrome)";
        case UserAgentMode::DesktopSafari: return "Desktop (Safari)";
        case UserAgentMode::MobileSafari: return "Mobile (iPhone)";
        case UserAgentMode::ChromecastTV: return "Smart TV (Chromecast)";
        case UserAgentMode::ProsperoPS5:
        default: return "PlayStation 5 (system)";
    }
}

bool BrowserSettings::load_from_file(const std::string& path) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;

    std::fseek(fp, 0, SEEK_END);
    long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(fp);
        return false;
    }

    std::string content(static_cast<std::size_t>(size), '\0');
    if (size > 0) {
        std::size_t read = std::fread(content.data(), 1, static_cast<std::size_t>(size), fp);
        content.resize(read);
    }
    std::fclose(fp);

    std::size_t start = 0;
    while (start < content.size()) {
        std::size_t end = content.find('\n', start);
        if (end == std::string::npos) end = content.size();
        std::string line = content.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        start = end + 1;

        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (key == "search_engine") {
            search_engine = static_cast<SearchEngine>(std::atoi(val.c_str()));
        } else if (key == "user_agent_mode") {
            user_agent_mode = static_cast<UserAgentMode>(std::atoi(val.c_str()));
        } else if (key == "javascript_enabled") {
            javascript_enabled = (val == "1" || val == "true");
        } else if (key == "webgl_enabled") {
            webgl_enabled = (val == "1" || val == "true");
        } else if (key == "theme_index") {
            theme_index = std::atoi(val.c_str());
        } else if (key == "cursor_sensitivity") {
            cursor_sensitivity = static_cast<float>(std::atof(val.c_str()));
        } else if (key == "default_zoom") {
            default_zoom = static_cast<float>(std::atof(val.c_str()));
        } else if (key == "restore_tabs") {
            restore_tabs = (val == "1" || val == "true");
        }
    }
    return true;
}

bool BrowserSettings::save_to_file(const std::string& path) const {
    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;

    std::fprintf(fp, "# Prospero Browser Settings\n");
    std::fprintf(fp, "search_engine=%d\n", static_cast<int>(search_engine));
    std::fprintf(fp, "user_agent_mode=%d\n", static_cast<int>(user_agent_mode));
    std::fprintf(fp, "javascript_enabled=%s\n", javascript_enabled ? "1" : "0");
    std::fprintf(fp, "webgl_enabled=%s\n", webgl_enabled ? "1" : "0");
    std::fprintf(fp, "theme_index=%d\n", theme_index);
    std::fprintf(fp, "cursor_sensitivity=%.2f\n", cursor_sensitivity);
    std::fprintf(fp, "default_zoom=%.2f\n", default_zoom);
    std::fprintf(fp, "restore_tabs=%s\n", restore_tabs ? "1" : "0");
    std::fclose(fp);
    return true;
}

} // namespace prospero::browser
