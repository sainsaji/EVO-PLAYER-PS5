#include "evo/services/ReleaseName.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <vector>

/* Pure string work, no FFmpeg: built into the host tests (tests/test_titles.cpp). */

namespace evo {

static bool allDigits(const std::string& s, size_t n) {
    if (s.size() != n) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

static bool isReleaseTag(std::string t) {
    if (t.size() >= 2 && (t.front() == '[' || t.front() == '(')) return true;   // [1080p], (WEB)
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const size_t dash = t.find('-');
    if (dash != std::string::npos && dash > 0 && t.compare(0, dash, "web") != 0 && t.compare(0, dash, "blu") != 0)
        t = t.substr(0, dash);                                               // "x264-galaxytv"
    // 720p, 1080p, 2160p, 1080i
    if (t.size() >= 4 && t.size() <= 5 && (t.back() == 'p' || t.back() == 'i') &&
        allDigits(t.substr(0, t.size() - 1), t.size() - 1))
        return true;
    static const char* const kTags[] = {
        "4k", "uhd", "hdr", "hdr10", "hdr10+", "dv", "dovi", "10bit", "8bit", "sdr",
        "x264", "x265", "h264", "h265", "hevc", "avc", "av1", "vp9", "xvid", "divx",
        /* Not "web", "max", "extended" and the like: they are title words too. */
        "webrip", "web-dl", "webdl", "web-rip", "bluray", "blu-ray", "brrip", "bdrip", "bdremux",
        "dvdrip", "hdtv", "hdrip", "remux", "amzn", "dsnp", "hmax", "atvp", "pcok",
        "proper", "repack", "unrated",
        "aac", "ac3", "eac3", "ddp", "ddp5", "dd5", "dts", "dts-hd", "truehd", "atmos", "flac",
    };
    for (const char* k : kTags)
        if (t == k) return true;
    return t.rfind("ddp", 0) == 0 || t.rfind("dd5", 0) == 0 || t.rfind("aac2", 0) == 0;   // DDP5.1 split as "DDP5 1"
}

std::string tidyReleaseName(const std::string& spaced) {
    std::vector<std::string> tok;
    for (size_t i = 0; i < spaced.size();) {
        const size_t j = spaced.find(' ', i);
        const std::string t = spaced.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!t.empty()) tok.push_back(t);
        if (j == std::string::npos) break;
        i = j + 1;
    }
    if (tok.empty()) return "";

    // Camera / capture style: YYYYMMDD HHMMSS ...
    if (tok.size() >= 2 && allDigits(tok[0], 8) && allDigits(tok[1], 6)) {
        static const char* const kMon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        const int mon = std::atoi(tok[0].substr(4, 2).c_str());
        const int day = std::atoi(tok[0].substr(6, 2).c_str());
        const int hh = std::atoi(tok[1].substr(0, 2).c_str());
        const int mm = std::atoi(tok[1].substr(2, 2).c_str());
        if (mon >= 1 && mon <= 12 && day >= 1 && day <= 31 && hh < 24 && mm < 60) {
            char buf[48];
            std::snprintf(buf, sizeof buf, "%d %s %s, %02d:%02d", day, kMon[mon - 1], tok[0].substr(0, 4).c_str(),
                          hh, mm);
            return buf;
        }
    }

    size_t first = 0;
    if (tok.size() > 1 && tok[0].front() == '[' && tok[0].back() == ']') first = 1;   // "[Group] Show - 01"
    // "www.Site.com - Title ...": the site name and its dash go.
    if ((tok[first] == "www" || tok[first] == "WWW" || tok[first] == "Www") && tok.size() > first + 2)
        for (size_t k = first + 1; k < tok.size() - 1 && k <= first + 5; ++k)
            if (tok[k] == "-") { first = k + 1; break; }

    std::string out;
    for (size_t i = first; i < tok.size(); ++i) {
        const std::string& t = tok[i];
        if (i > first) {
            // A year right before the tags (or at the end) is the film's year.
            std::string y = t;
            if (y.size() == 6 && (y.front() == '(' || y.front() == '[')) y = y.substr(1, 4);
            if (allDigits(y, 4)) {
                const int year = std::atoi(y.c_str());
                const bool last = i + 1 == tok.size() || isReleaseTag(tok[i + 1]);
                if (year >= 1920 && year <= 2035 && last) {
                    out += " (" + y + ")";
                    break;
                }
            }
            if (isReleaseTag(t)) break;
            if ((t == "H" || t == "h") && i + 1 < tok.size() && (tok[i + 1] == "264" || tok[i + 1] == "265"))
                break;   // "H.264" after the dots became spaces
        }
        if (!out.empty()) out += ' ';
        out += t;
    }
    while (!out.empty() && (out.back() == '-' || out.back() == ' ')) out.pop_back();
    return out;
}

} // namespace evo
