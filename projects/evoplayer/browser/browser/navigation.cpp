// Prospero Browser - WPE WebKit Port for PlayStation 5
// Omnibox Navigation and Search Query Resolver implementation.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later

#include "navigation.hpp"
#include <algorithm>
#include <cctype>

namespace prospero::browser {

namespace {

bool starts_with_case_insensitive(std::string_view str, std::string_view prefix) {
    if (str.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(str[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string Navigation::resolve_input(std::string_view input, SearchEngine engine) {
    // Trim whitespace
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front()))) {
        input.remove_prefix(1);
    }
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back()))) {
        input.remove_suffix(1);
    }

    if (input.empty()) {
        return "about:start";
    }

    // Direct protocol check
    if (starts_with_case_insensitive(input, "http://") ||
        starts_with_case_insensitive(input, "https://") ||
        starts_with_case_insensitive(input, "about:") ||
        starts_with_case_insensitive(input, "file://")) {
        return std::string(input);
    }

    // If it looks like a direct URL (domain name or IP)
    if (is_direct_url(input)) {
        return "https://" + std::string(input);
    }

    // Otherwise, treat as web search
    return make_search_url(input, engine);
}

bool Navigation::is_direct_url(std::string_view input) {
    // If it contains spaces, it's a search query
    if (input.find(' ') != std::string_view::npos) {
        return false;
    }

    if (input == "localhost") {
        return true;
    }

    // Check for IP address format (contains dots and only digits/dots)
    auto dot_pos = input.find('.');
    if (dot_pos != std::string_view::npos && dot_pos > 0 && dot_pos + 1 < input.size()) {
        // Has a dot and no spaces: likely domain or IP
        return true;
    }

    return false;
}

std::string Navigation::make_search_url(std::string_view query, SearchEngine engine) {
    std::string encoded_query = url_encode(query);
    switch (engine) {
        case SearchEngine::Google:
            return "https://www.google.com/search?q=" + encoded_query + "&gbv=1";
        case SearchEngine::Bing:
            return "https://www.bing.com/search?q=" + encoded_query;
        case SearchEngine::Startpage:
            return "https://www.startpage.com/sp/search?query=" + encoded_query;
        case SearchEngine::DuckDuckGo:
        default:
            return "https://html.duckduckgo.com/html/?q=" + encoded_query;
    }
}

std::string Navigation::extract_host(std::string_view url) {
    std::string_view clean = url;
    auto scheme_pos = clean.find("://");
    if (scheme_pos != std::string_view::npos) {
        clean.remove_prefix(scheme_pos + 3);
    }

    auto slash_pos = clean.find('/');
    if (slash_pos != std::string_view::npos) {
        clean = clean.substr(0, slash_pos);
    }

    auto colon_pos = clean.find(':');
    if (colon_pos != std::string_view::npos) {
        clean = clean.substr(0, colon_pos);
    }

    return std::string(clean);
}

std::string Navigation::url_encode(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size() * 3);
    static const char hex_digits[] = "0123456789ABCDEF";

    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            escaped.push_back('+');
        } else {
            escaped.push_back('%');
            escaped.push_back(hex_digits[(c >> 4) & 0x0F]);
            escaped.push_back(hex_digits[c & 0x0F]);
        }
    }

    return escaped;
}

const char* Navigation::search_engine_name(SearchEngine engine) {
    switch (engine) {
        case SearchEngine::Google: return "Google";
        case SearchEngine::Bing: return "Bing";
        case SearchEngine::Startpage: return "Startpage";
        case SearchEngine::DuckDuckGo:
        default: return "DuckDuckGo";
    }
}

bool Navigation::is_secure(std::string_view url) {
    return starts_with_case_insensitive(url, "https://");
}

std::string Navigation::resolve_link(std::string_view base_url, std::string_view href) {
    if (href.empty()) return std::string(base_url);

    // Direct protocols
    if (starts_with_case_insensitive(href, "http://") ||
        starts_with_case_insensitive(href, "https://") ||
        starts_with_case_insensitive(href, "about:") ||
        starts_with_case_insensitive(href, "file://") ||
        starts_with_case_insensitive(href, "javascript:")) {
        return std::string(href);
    }

    // Protocol-relative: //domain/path
    if (href.size() >= 2 && href[0] == '/' && href[1] == '/') {
        return "https:" + std::string(href);
    }

    // Scheme and host extraction from base_url
    std::string scheme = "https";
    std::string host;
    auto scheme_pos = base_url.find("://");
    if (scheme_pos != std::string_view::npos) {
        scheme = std::string(base_url.substr(0, scheme_pos));
        auto after_scheme = base_url.substr(scheme_pos + 3);
        auto slash_pos = after_scheme.find('/');
        if (slash_pos != std::string_view::npos) {
            host = std::string(after_scheme.substr(0, slash_pos));
        } else {
            host = std::string(after_scheme);
        }
    } else {
        host = std::string(base_url);
    }

    // Root-relative: /path
    if (href.front() == '/') {
        return scheme + "://" + host + std::string(href);
    }

    // Hash anchor: #anchor
    if (href.front() == '#') {
        auto hash_pos = base_url.find('#');
        if (hash_pos != std::string_view::npos) {
            return std::string(base_url.substr(0, hash_pos)) + std::string(href);
        }
        return std::string(base_url) + std::string(href);
    }

    // Path-relative: path/to/page
    std::string base_dir = scheme + "://" + host + "/";
    if (scheme_pos != std::string_view::npos) {
        auto last_slash = base_url.rfind('/');
        if (last_slash != std::string_view::npos && last_slash > scheme_pos + 2) {
            base_dir = std::string(base_url.substr(0, last_slash + 1));
        }
    }
    return base_dir + std::string(href);
}

bool Navigation::is_tv_app(std::string_view url) {
    const std::string host = extract_host(url);
    if (host.rfind("tv.", 0) == 0) return true;
    const auto scheme = url.find("://");
    std::string_view rest = scheme == std::string_view::npos ? url : url.substr(scheme + 3);
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos) return false;
    const std::string_view path = rest.substr(slash);
    auto segment_is = [&](std::string_view name) {
        if (path.substr(0, name.size()) != name) return false;
        return path.size() == name.size() || path[name.size()] == '/' || path[name.size()] == '?' ||
               path[name.size()] == '#';
    };
    return segment_is("/tv") || segment_is("/leanback");
}

} // namespace prospero::browser
