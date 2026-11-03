#include "evo/services/MediaMetadataService.hpp"
#include "evo_toast.h"
#include "evo_smb_io.h"
#include "evo_error.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <algorithm>

#ifndef EVO_PLAYER_VERSION
#define EVO_PLAYER_VERSION "v0.13.0"
#endif

namespace evo {

static const char* ResolveCodecName(enum AVCodecID id) {
    const char* name = avcodec_get_name(id);
    return (name && name[0]) ? name : "Unknown";
}

MediaMetadataInfo MediaMetadataService::extractBasicMetadata(const std::string& filePath) {
    MediaMetadataInfo info;
    char cleanPath[512];
    evo_url_sanitize(filePath.c_str(), cleanPath, sizeof(cleanPath));
    info.filePath = cleanPath;
    /*
     * Empty, not "Unknown".
     *
     * Every consumer decides whether to show these by testing .empty(), so a
     * literal "Unknown" defeats the test: the browser's status strip showed
     * an "Unknown" resolution pill and an "Unknown" codec pill next to a .log
     * file, which has neither and never will. "No value" has to be
     * representable for the guards to mean anything.
     *
     * ResolveCodecName() still returns "Unknown" - it only runs when a stream
     * genuinely exists and merely cannot be named, which is a different fact
     * from having no stream at all.
     */
    info.container.clear();
    info.videoCodec.clear();
    info.audioCodec.clear();

    cleanMediaTitle(filePath, info.title, info.container);

    FILE* file = std::fopen(filePath.c_str(), "rb");
    if (file) {
        std::fseek(file, 0, SEEK_END);
        info.fileSizeBytes = std::ftell(file);
        std::fclose(file);
    }
    return info;
}

MediaMetadataInfo MediaMetadataService::extractMetadataFromFormat(AVFormatContext* formatContext, const std::string& filePath) {
    MediaMetadataInfo info = extractBasicMetadata(filePath);
    if (!formatContext) {
        return info;
    }

    if (formatContext->duration > 0) {
        info.durationSeconds = static_cast<double>(formatContext->duration) / static_cast<double>(AV_TIME_BASE);
    }

    if (formatContext->iformat && formatContext->iformat->name) {
        info.container = formatContext->iformat->name;
    }

    for (unsigned int i = 0; i < formatContext->nb_streams; ++i) {
        AVStream* stream = formatContext->streams[i];
        if (!stream || !stream->codecpar) continue;

        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !info.hasVideo) {
            info.hasVideo = true;
            info.width = stream->codecpar->width;
            info.height = stream->codecpar->height;
            info.videoCodec = ResolveCodecName(stream->codecpar->codec_id);
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && !info.hasAudio) {
            info.hasAudio = true;
            info.audioCodec = ResolveCodecName(stream->codecpar->codec_id);
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            info.hasSubtitles = true;
        }
    }

    return info;
}

void MediaMetadataService::loadChapters(AVFormatContext* formatContext) {
    clearChapters();
    if (!formatContext || formatContext->nb_chapters <= 0) {
        return;
    }

    m_chapters.reserve(formatContext->nb_chapters);
    for (unsigned int i = 0; i < formatContext->nb_chapters && m_chapters.size() < 128; ++i) {
        AVChapter* chapter = formatContext->chapters[i];
        if (!chapter) continue;

        double startSec = chapter->start * av_q2d(chapter->time_base);
        if (startSec < 0.0) startSec = 0.0;

        ChapterInfo info;
        info.startTimeSeconds = startSec;

        const AVDictionaryEntry* titleEntry = av_dict_get(chapter->metadata, "title", nullptr, 0);
        if (titleEntry && titleEntry->value && titleEntry->value[0]) {
            info.title = titleEntry->value;
        } else {
            char fallback[48];
            std::snprintf(fallback, sizeof(fallback), "Chapter %zu", m_chapters.size() + 1);
            info.title = fallback;
        }

        m_chapters.push_back(std::move(info));
    }
}

void MediaMetadataService::clearChapters() {
    m_chapters.clear();
}

const ChapterInfo* MediaMetadataService::getChapter(size_t index) const {
    if (index < m_chapters.size()) {
        return &m_chapters[index];
    }
    return nullptr;
}

int MediaMetadataService::getChapterIndexAtPosition(double positionSeconds) const {
    int bestMatch = -1;
    for (size_t i = 0; i < m_chapters.size(); ++i) {
        if (m_chapters[i].startTimeSeconds <= positionSeconds + 0.05) {
            bestMatch = static_cast<int>(i);
        } else {
            break;
        }
    }
    return bestMatch;
}

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

std::string MediaMetadataService::tidyReleaseName(const std::string& spaced) {
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

void MediaMetadataService::cleanMediaTitle(const std::string& filePath, std::string& outTitle, std::string& outCategory) {
    if (filePath.empty()) {
        outTitle = "Unknown";
        outCategory = "Unknown";
        return;
    }

    size_t lastSlash = filePath.find_last_of("/\\");
    std::string filename = (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath;
    if (filePath.rfind("smb://", 0) == 0) {
        filename = evo_url_decode_string(filename);
    }

    size_t lastDot = filename.find_last_of('.');
    std::string extension;
    if (lastDot != std::string::npos && lastDot > 0) {
        extension = filename.substr(lastDot + 1);
        filename = filename.substr(0, lastDot);
    }

    for (char& c : filename) {
        if (c == '.' || c == '_') {
            c = ' ';
        }
    }

    // Strip leading/trailing spaces
    size_t start = filename.find_first_not_of(" \t\r\n");
    size_t end = filename.find_last_not_of(" \t\r\n");
    if (start != std::string::npos && end != std::string::npos) {
        filename = filename.substr(start, end - start + 1);
    }

    const std::string tidy = tidyReleaseName(filename);
    outTitle = tidy.empty() ? filename : tidy;
    outCategory = extension.empty() ? "MEDIA" : extension;
    std::transform(outCategory.begin(), outCategory.end(), outCategory.begin(), ::toupper);
}

std::string MediaMetadataService::formatDuration(double durationSeconds) {
    if (durationSeconds < 0.0) durationSeconds = 0.0;
    int totalSec = static_cast<int>(durationSeconds);
    int hours = totalSec / 3600;
    int minutes = (totalSec % 3600) / 60;
    int seconds = totalSec % 60;

    char buffer[32];
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", hours, minutes, seconds);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d", minutes, seconds);
    }
    return buffer;
}

std::string MediaMetadataService::formatFileSize(int64_t bytes) {
    double value = static_cast<double>(bytes);
    const char* unit = "B";

    if (value >= 1024.0) { value /= 1024.0; unit = "KB"; }
    if (value >= 1024.0) { value /= 1024.0; unit = "MB"; }
    if (value >= 1024.0) { value /= 1024.0; unit = "GB"; }

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f %s", value, unit);
    return buffer;
}

std::string MediaMetadataService::formatRemainingTime(double currentPosition, double duration) {
    double remaining = duration - currentPosition;
    if (remaining < 0.0) remaining = 0.0;
    return "-" + formatDuration(remaining);
}

int MediaMetadataService::calculateProgressPermille(double currentPosition, double duration) {
    if (duration <= 0.1) return 0;
    double ratio = currentPosition / duration;
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    return static_cast<int>(ratio * 1000.0);
}

bool MediaMetadataService::exportCompatibilityReport(const std::string& outputPath,
                                                    const MediaMetadataInfo& metadata,
                                                    double playbackPosition,
                                                    int recentCount,
                                                    int favoriteCount,
                                                    bool isPlaying) {
    FILE* file = std::fopen(outputPath.c_str(), "w");
    if (!file) {
        toast("REPORT", "Save failed");
        return false;
    }

    std::string fileSizeStr = formatFileSize(metadata.fileSizeBytes);
    std::string durationStr = formatDuration(metadata.durationSeconds);
    std::string positionStr = formatDuration(playbackPosition);

    std::fprintf(file, "EVO Player Compatibility Report\n");
    std::fprintf(file, "====================================\n\n");
    std::fprintf(file, "Build\nVERSION %s\n\n", EVO_PLAYER_VERSION);
    std::fprintf(file, "File\n%s\n\n", metadata.filePath.c_str());
    std::fprintf(file, "Title\n%s\n\n", metadata.title.c_str());
    std::fprintf(file, "Container\n%s\n\n", metadata.container.c_str());
    std::fprintf(file, "Video Codec\n%s\n\n", metadata.videoCodec.c_str());
    std::fprintf(file, "Resolution\n%d x %d\n\n", metadata.width, metadata.height);
    std::fprintf(file, "Audio Codec\n%s\n\n", metadata.audioCodec.c_str());
    std::fprintf(file, "Subtitles\n%s\n\n", metadata.hasSubtitles ? "Yes" : "No");
    std::fprintf(file, "Duration\n%s\n\n", durationStr.c_str());
    std::fprintf(file, "Current Position\n%s\n\n", positionStr.c_str());
    std::fprintf(file, "File Size\n%s\n\n", fileSizeStr.c_str());
    std::fprintf(file, "Recent DB Entries\n%d\n\n", recentCount);
    std::fprintf(file, "Favorites Entries\n%d\n\n", favoriteCount);
    std::fprintf(file, "Playback State\n%s\n\n", isPlaying ? "Playing Screen" : "Not On Player Screen");
    std::fprintf(file, "Notes\nGenerated locally from EVO Player metadata and playback state.\n");

    std::fclose(file);
    toast("REPORT", "Saved to USB");
    return true;
}

} // namespace evo
