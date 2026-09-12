#include "evo/services/FileSystemBrowser.hpp"
#include "evo_readdir.h"
#include "evo_jailbreak.h"
#include "evo_data_path.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cctype>

namespace evo {

static bool CaseInsensitiveStringCompare(const std::string& a, const std::string& b) {
    return std::lexicographical_compare(
        a.begin(), a.end(),
        b.begin(), b.end(),
        [](char c1, char c2) {
            return std::tolower(static_cast<unsigned char>(c1)) <
                   std::tolower(static_cast<unsigned char>(c2));
        }
    );
}

static bool CaseInsensitiveContains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;

    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char c1, char c2) {
            return std::tolower(static_cast<unsigned char>(c1)) ==
                   std::tolower(static_cast<unsigned char>(c2));
        }
    );
    return (it != haystack.end());
}

FileSystemBrowser::FileSystemBrowser() {
    m_sources = {
        { "USB DRIVE",        "USB",      "/mnt/usb0" },
        { "INTERNAL STORAGE", "INTERNAL", "/data"     }
    };
    m_currentPath = ""; // Root picker
}

void FileSystemBrowser::setCurrentPath(const std::string& path) {
    m_currentPath = path;
    refresh();
}

bool FileSystemBrowser::isSafePath(const std::string& path) const {
    if (path.empty()) return false;
    for (const auto& src : m_sources) {
        if (path.rfind(src.rootPath, 0) == 0 &&
            (path.size() == src.rootPath.size() || path[src.rootPath.size()] == '/')) {
            return true;
        }
    }
    return false;
}

bool FileSystemBrowser::navigateToSource(size_t sourceIndex) {
    if (sourceIndex < m_sources.size()) {
        m_currentPath = m_sources[sourceIndex].rootPath;
        clearSearch();
        return refresh();
    }
    return false;
}

bool FileSystemBrowser::navigateInto(const std::string& folderName) {
    if (m_currentPath.empty()) {
        // Picker mode: navigate into chosen source
        for (const auto& src : m_sources) {
            if (src.label == folderName) {
                m_currentPath = src.rootPath;
                return refresh();
            }
        }
        return false;
    }

    std::string newPath = m_currentPath + "/" + folderName;
    m_currentPath = newPath;
    clearSearch();
    return refresh();
}

bool FileSystemBrowser::navigateUp() {
    if (m_currentPath.empty()) {
        return false;
    }

    // Check if at source root
    for (const auto& src : m_sources) {
        if (m_currentPath == src.rootPath) {
            m_currentPath = ""; // Return to root picker
            clearSearch();
            return refresh();
        }
    }

    size_t lastSlash = m_currentPath.find_last_of('/');
    if (lastSlash != std::string::npos && lastSlash > 0) {
        m_currentPath = m_currentPath.substr(0, lastSlash);
    } else {
        m_currentPath = "";
    }

    clearSearch();
    return refresh();
}

bool FileSystemBrowser::refresh() {
    m_entries.clear();

    evo_jailbreak_ensure();

    // Sentinel: at root picker above any source
    if (m_currentPath.empty()) {
        for (const auto& src : m_sources) {
            BrowserEntry entry;
            entry.name = src.label;
            entry.relativePath = src.label;
            entry.category = FileCategory::Folder;
            entry.directoryType = 4; // DT_DIR
            m_entries.push_back(std::move(entry));
        }
        return true;
    }

    if (!m_searchQuery.empty()) {
        scanRecursive(m_currentPath, "", m_searchQuery, 0);
    } else {
        scanDirectory(m_currentPath);
    }

    // Sort entries
    if (m_entries.size() > 1) {
        std::sort(m_entries.begin(), m_entries.end(), [this](const BrowserEntry& a, const BrowserEntry& b) {
            if (m_sortFoldersFirst) {
                bool aIsDir = (a.category == FileCategory::Folder);
                bool bIsDir = (b.category == FileCategory::Folder);
                if (aIsDir != bIsDir) {
                    return aIsDir; // Folders come first
                }
            }
            return CaseInsensitiveStringCompare(a.name, b.name);
        });
    }

    return true;
}

void FileSystemBrowser::scanDirectory(const std::string& dirPath) {
    evo_dir_t* dir = evo_opendir(dirPath.c_str());
    if (!dir) {
        // Fallback to source root if subfolder was deleted
        for (const auto& src : m_sources) {
            if (dirPath.rfind(src.rootPath, 0) == 0 && dirPath != src.rootPath) {
                m_currentPath = src.rootPath;
                dir = evo_opendir(m_currentPath.c_str());
                break;
            }
        }
    }

    if (!dir) {
        return;
    }

    struct dirent* entry;
    while ((entry = evo_readdir(dir)) != nullptr && m_entries.size() < 256) {
        if (entry->d_name[0] == '.') continue;
        if (std::strcmp(entry->d_name, "$RECYCLE.BIN") == 0) continue;
        if (std::strcmp(entry->d_name, "System Volume Information") == 0) continue;

        BrowserEntry bEntry;
        bEntry.name = entry->d_name;
        bEntry.relativePath = entry->d_name;
        bEntry.directoryType = entry->d_type;
        bEntry.category = classifyFile(entry->d_name, entry->d_type);
        m_entries.push_back(std::move(bEntry));
    }

    evo_closedir(dir);
}

void FileSystemBrowser::scanRecursive(const std::string& basePath, const std::string& relPath,
                                     const std::string& query, int depth) {
    if (depth > 5 || m_entries.size() >= 255) {
        return;
    }

    std::string fullDirPath = relPath.empty() ? basePath : (basePath + "/" + relPath);
    evo_dir_t* dir = evo_opendir(fullDirPath.c_str());
    if (!dir) return;

    struct dirent* entry;
    while ((entry = evo_readdir(dir)) != nullptr && m_entries.size() < 255) {
        if (entry->d_name[0] == '.') continue;
        if (std::strcmp(entry->d_name, "$RECYCLE.BIN") == 0) continue;
        if (std::strcmp(entry->d_name, "System Volume Information") == 0) continue;

        std::string itemRel = relPath.empty() ? entry->d_name : (relPath + "/" + entry->d_name);

        if (CaseInsensitiveContains(entry->d_name, query) || CaseInsensitiveContains(itemRel, query)) {
            BrowserEntry bEntry;
            bEntry.name = itemRel;
            bEntry.relativePath = itemRel;
            bEntry.directoryType = entry->d_type;
            bEntry.category = classifyFile(entry->d_name, entry->d_type);
            m_entries.push_back(std::move(bEntry));
        }

        if (entry->d_type == 4) { // DT_DIR
            scanRecursive(basePath, itemRel, query, depth + 1);
        }
    }

    evo_closedir(dir);
}

void FileSystemBrowser::search(const std::string& query) {
    m_searchQuery = query;
    refresh();
}

void FileSystemBrowser::clearSearch() {
    m_searchQuery.clear();
}

const BrowserEntry* FileSystemBrowser::getEntry(size_t index) const {
    if (index < m_entries.size()) {
        return &m_entries[index];
    }
    return nullptr;
}

std::string FileSystemBrowser::getFullPath(size_t index) const {
    if (index >= m_entries.size()) return "";
    if (m_currentPath.empty()) {
        for (const auto& src : m_sources) {
            if (src.label == m_entries[index].name) {
                return src.rootPath;
            }
        }
        return "";
    }
    return m_currentPath + "/" + m_entries[index].relativePath;
}

FileCategory FileSystemBrowser::classifyFile(const std::string& fileName, int entryType) const {
    if (entryType == 4) {
        return FileCategory::Folder;
    }

    size_t dot = fileName.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= fileName.size()) {
        return FileCategory::Unknown;
    }

    std::string ext = fileName.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Video
    if (ext == "mkv" || ext == "mp4" || ext == "mov" || ext == "m4v" ||
        ext == "avi" || ext == "webm" || ext == "ts" || ext == "m2ts" ||
        ext == "mpg" || ext == "mpeg" || ext == "wmv" || ext == "flv") {
        return FileCategory::Video;
    }

    // Audio
    if (ext == "mp3" || ext == "flac" || ext == "wav" || ext == "aac" ||
        ext == "m4a" || ext == "ogg") {
        return FileCategory::Audio;
    }

    // Image
    if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "bmp" || ext == "webp") {
        return FileCategory::Image;
    }

    // Document
    if (ext == "srt" || ext == "ass" || ext == "ssa" || ext == "vtt" ||
        ext == "txt" || ext == "log" || ext == "md"  || ext == "nfo" ||
        ext == "ini" || ext == "cfg" || ext == "json" || ext == "csv") {
        return FileCategory::Document;
    }

    // Disc Image
    if (ext == "iso" || ext == "img") {
        return FileCategory::DiscImage;
    }

    // Homebrew
    if (ext == "elf" || ext == "bin" || ext == "sprx") {
        return FileCategory::Homebrew;
    }

    return FileCategory::Unknown;
}

const char* FileSystemBrowser::getFileCategoryLabel(FileCategory category) const {
    switch (category) {
        case FileCategory::Folder:    return "FOLDER";
        case FileCategory::Video:     return "VIDEO";
        case FileCategory::Audio:     return "AUDIO";
        case FileCategory::Image:     return "IMAGE";
        case FileCategory::Document:  return "DOCUMENT";
        case FileCategory::DiscImage: return "DISC IMAGE";
        case FileCategory::Homebrew:  return "HOMEBREW";
        default:                      return "FILE";
    }
}

void FileSystemBrowser::saveLastFolder() {
    if (!isSafePath(m_currentPath)) {
        return;
    }

    const char* configPath = evo_data_path("evo_last_folder.cfg");
    FILE* file = std::fopen(configPath, "w");
    if (file) {
        std::fprintf(file, "%s\n", m_currentPath.c_str());
        std::fclose(file);
    }
}

void FileSystemBrowser::loadLastFolder() {
    m_currentPath = ""; // default to source picker

    const char* configPath = evo_data_path("evo_last_folder.cfg");
    FILE* file = std::fopen(configPath, "r");
    if (!file) {
        return;
    }

    char savedFolder[512] = {0};
    if (std::fgets(savedFolder, sizeof(savedFolder), file)) {
        size_t len = std::strlen(savedFolder);
        while (len > 0 && (savedFolder[len - 1] == '\n' || savedFolder[len - 1] == '\r')) {
            savedFolder[len - 1] = '\0';
            len--;
        }
    }
    std::fclose(file);

    if (savedFolder[0] != '\0' && isSafePath(savedFolder)) {
        evo_dir_t* dir = evo_opendir(savedFolder);
        if (dir) {
            evo_closedir(dir);
            m_currentPath = savedFolder;
        }
    }
}

} // namespace evo
