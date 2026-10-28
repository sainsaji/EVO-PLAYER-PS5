#include "evo/services/NetArtSidecar.hpp"

#include <cctype>
#include <string>
#include <vector>

namespace evo {

static std::string toLowerAscii(const std::string& str) {
    std::string out;
    out.reserve(str.size());
    for (char c : str) {
        if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c + ('a' - 'A')));
        } else {
            out.push_back(c);
        }
    }
    return out;
}

int pickSidecar(const std::string& mediaName, const std::vector<SidecarEntry>& siblings) {
    if (mediaName.empty() || siblings.empty()) {
        return -1;
    }

    std::string mediaFile = mediaName;
    size_t mediaSlash = mediaFile.find_last_of("/\\");
    if (mediaSlash != std::string::npos) {
        mediaFile = mediaFile.substr(mediaSlash + 1);
    }
    if (mediaFile.empty()) {
        return -1;
    }

    // Stem (<name>): file name without its last extension
    size_t lastDot = mediaFile.find_last_of('.');
    std::string mediaStem = (lastDot != std::string::npos && lastDot > 0)
                                ? mediaFile.substr(0, lastDot)
                                : mediaFile;

    std::string mediaFileLower = toLowerAscii(mediaFile);
    std::string mediaStemLower = toLowerAscii(mediaStem);

    int bestIndex = -1;
    int bestTier = 999;
    int bestExtRank = 999;

    for (size_t i = 0; i < siblings.size(); ++i) {
        const auto& entry = siblings[i];
        if (entry.isDirectory) {
            continue; // Never pick a directory
        }

        std::string entryFile = entry.name;
        size_t entrySlash = entryFile.find_last_of("/\\");
        if (entrySlash != std::string::npos) {
            entryFile = entryFile.substr(entrySlash + 1);
        }
        if (entryFile.empty()) {
            continue;
        }

        std::string entryFileLower = toLowerAscii(entryFile);
        if (entryFileLower == mediaFileLower) {
            continue; // Never pick the media file itself
        }

        // Extensions .jpg .jpeg .png only
        size_t dot = entryFileLower.find_last_of('.');
        if (dot == std::string::npos) {
            continue;
        }

        std::string ext = entryFileLower.substr(dot + 1);
        int extRank = 0;
        if (ext == "jpg") {
            extRank = 1;
        } else if (ext == "jpeg") {
            extRank = 2;
        } else if (ext == "png") {
            extRank = 3;
        } else {
            continue; // Unsupported extension
        }

        std::string entryStemLower = entryFileLower.substr(0, dot);
        int tier = 0;

        // Priority:
        // 1. <name>-poster.<ext>
        // 2. <name>.<ext>
        // 3. poster.<ext>
        // 4. folder.<ext>
        // 5. cover.<ext>
        if (!mediaStemLower.empty() && entryStemLower == mediaStemLower + "-poster") {
            tier = 1;
        } else if (!mediaStemLower.empty() && entryStemLower == mediaStemLower) {
            tier = 2;
        } else if (entryStemLower == "poster") {
            tier = 3;
        } else if (entryStemLower == "folder") {
            tier = 4;
        } else if (entryStemLower == "cover") {
            tier = 5;
        } else {
            continue;
        }

        if (tier < bestTier || (tier == bestTier && extRank < bestExtRank)) {
            bestTier = tier;
            bestExtRank = extRank;
            bestIndex = static_cast<int>(i);
        }
    }

    return bestIndex;
}

} // namespace evo
