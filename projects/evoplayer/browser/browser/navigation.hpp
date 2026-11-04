// Prospero Browser - WPE WebKit Port for PlayStation 5
// Omnibox Navigation and Search Query Resolver.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace prospero::browser {

enum class SearchEngine {
    DuckDuckGo,
    Google,
    Bing,
    Startpage,
};
constexpr int kSearchEngineCount = 4;

class Navigation {
public:
    // Resolve omnibox input to either a full URL or a search query
    static std::string resolve_input(std::string_view input,
                                     SearchEngine engine = SearchEngine::DuckDuckGo);

    // Check if input looks like a direct URL rather than search terms
    static bool is_direct_url(std::string_view input);

    // Construct search URL for specified engine and query
    static std::string make_search_url(std::string_view query, SearchEngine engine);

    // Extract hostname / domain for clean display in address bar
    static std::string extract_host(std::string_view url);

    // URL percent-encoding for query strings
    static std::string url_encode(std::string_view text);

    // Return human-readable search engine name
    static const char* search_engine_name(SearchEngine engine);

    // Check if URL is secure (HTTPS)
    static bool is_secure(std::string_view url);

    // Resolve relative or absolute link against base URL
    static std::string resolve_link(std::string_view base_url, std::string_view href);

    // TV-style web apps (youtube.com/tv, tv.* hosts, leanback UIs) handle the
    // remote's Back key themselves; Circle sends them Escape instead of leaving.
    static bool is_tv_app(std::string_view url);
};

} // namespace prospero::browser
