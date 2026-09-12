#include "evo/services/MediaMetadataService.hpp"
#include "evo_toast.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
}

#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>

#ifndef EVO_PLAYER_VERSION
#define EVO_PLAYER_VERSION "0.7.6"
#endif

namespace evo {

static const char* ResolveCodecName(enum AVCodecID id) {
    const char* name = avcodec_get_name(id);
    return (name && name[0]) ? name : "Unknown";
}

MediaMetadataInfo MediaMetadataService::extractBasicMetadata(const std::string& filePath) {
    MediaMetadataInfo info;
    info.filePath = filePath;
    info.container = "Unknown";
    info.videoCodec = "Unknown";
    info.audioCodec = "Unknown";

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

void MediaMetadataService::cleanMediaTitle(const std::string& filePath, std::string& outTitle, std::string& outCategory) {
    if (filePath.empty()) {
        outTitle = "Unknown";
        outCategory = "Unknown";
        return;
    }

    size_t lastSlash = filePath.find_last_of("/\\");
    std::string filename = (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath;

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

    outTitle = filename;
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
                                                    PlaybackProfile profile,
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

    const char* profileStr = "Balanced";
    switch (profile) {
        case PlaybackProfile::Balanced:      profileStr = "Balanced"; break;
        case PlaybackProfile::Performance:   profileStr = "Performance"; break;
        case PlaybackProfile::Compatibility: profileStr = "Compatibility"; break;
        case PlaybackProfile::Debug:         profileStr = "Debug"; break;
    }

    std::fprintf(file, "EVO Player Compatibility Report\n");
    std::fprintf(file, "====================================\n\n");
    std::fprintf(file, "Build\nVERSION %s\n\n", EVO_PLAYER_VERSION);
    std::fprintf(file, "Playback Profile\n%s\n\n", profileStr);
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
