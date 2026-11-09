// Prospero Browser - WPE WebKit Port for PlayStation 5
// Browser Settings & Configuration.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later

#pragma once

#include "navigation.hpp"
#include <string>

namespace prospero::browser {

enum class UserAgentMode {
    ProsperoPS5,     // the engine's own (Sony's PlayStation) user agent
    DesktopChrome,
    DesktopSafari,
    MobileSafari,
    ChromecastTV,
};
constexpr int kUserAgentModeCount = 5;
const char* user_agent_mode_name(UserAgentMode mode);

struct BrowserSettings {
    SearchEngine search_engine{SearchEngine::DuckDuckGo};
    UserAgentMode user_agent_mode{UserAgentMode::ProsperoPS5};
    bool javascript_enabled{true};
    bool webgl_enabled{true};
    int theme_index{0}; // 0 = Acrylic, etc. (ps5-homebrew-ui themes)
    float cursor_sensitivity{1.0f};
    float default_zoom{1.0f};
    bool restore_tabs{true};  // reopen last session's tabs at startup

    // User-agent string generation
    std::string user_agent_string() const;
    // What the page engine is told: empty keeps its own user agent.
    std::string engine_user_agent() const;

    // Load and save settings
    bool load_from_file(const std::string& path);
    bool save_to_file(const std::string& path) const;
};

} // namespace prospero::browser
