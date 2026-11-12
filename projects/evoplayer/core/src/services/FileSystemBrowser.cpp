#include "evo/services/FileSystemBrowser.hpp"
#include "evo_readdir.h"
#include "evo_jailbreak.h"
#include "evo_data_path.h"
#include "evo_boot_trace.h"
#include "evo_boot_log.h"
#include "evo_ntfs.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <sys/stat.h>
#include <time.h>

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

/* The console mounts each USB port as usbN and M.2 extended storage as extN.
 * They all exist as directories whether or not anything is plugged in, so a
 * mount counts only when it has an entry. */
static const char* const kUsbMounts[] = {
    "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
    "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7",
    "/mnt/ext0", "/mnt/ext1",
};
/* Not a real directory: the list of drives, shown when there is more than one. */
static const char* const kUsbRoot = "/mnt/usb";

static bool hasEntries(const char* path) {
    evo_dir_t* dir = evo_opendir(path);
    if (!dir) return false;
    bool any = false;
    struct dirent* e;
    while ((e = evo_readdir(dir)) != nullptr) {
        if (e->d_name[0] != '.') { any = true; break; }
    }
    evo_closedir(dir);
    return any;
}

static uint64_t nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec / 1000000);
}

/* Cached: BrowserScreen asks usbHome() several times a frame and once per folder
 * row, and each scan opens and reads all ten mounts - over a thousand directory
 * reads a second on the USB source, the ones on a real stick going to the device.
 * refresh() drops the cache, so a drive plugged in or pulled shows on the next
 * move, or within kUsbMountsTtlMs while the browser sits still. */
static const uint64_t kUsbMountsTtlMs = 3000;

std::vector<std::string> FileSystemBrowser::usbMounts() const {
    const uint64_t now = nowMs();
    if (m_usbMountsAtMs != 0 && now - m_usbMountsAtMs < kUsbMountsTtlMs) return m_usbMounts;
    evo_jailbreak_ensure();
    std::vector<std::string> out;
    for (const char* m : kUsbMounts) {
        if (hasEntries(m)) out.push_back(m);
    }
    /* #217: NTFS drives the console leaves unmounted, read by evo_ntfs.
     * Listed even when empty: a volume that mounted is a drive. Probed in the
     * background: a new drive shows on the next look (kUsbMountsTtlMs). */
    evo_ntfs_refresh_async();
    for (int i = 0; i < evo_ntfs_volume_count(); ++i) {
        if (const char* url = evo_ntfs_volume_url(i)) out.push_back(url);
    }
    m_usbMounts = out;
    m_usbMountsAtMs = now;
    return out;
}

std::string FileSystemBrowser::usbHome() const {
    const std::vector<std::string> mounts = usbMounts();
    if (mounts.size() > 1) return kUsbRoot;
    if (mounts.size() == 1) return mounts[0];
    return "/mnt/usb0";
}

std::string FileSystemBrowser::usbMountOf(const std::string& path) const {
    for (const char* m : kUsbMounts) {
        const size_t n = std::strlen(m);
        if (path.rfind(m, 0) == 0 && (path.size() == n || path[n] == '/')) return m;
    }
    if (evo_ntfs_is_url(path.c_str())) {   /* ntfs://<volume>[/...] -> ntfs://<volume> */
        const size_t slash = path.find('/', sizeof(EVO_NTFS_SCHEME) - 1);
        return slash == std::string::npos ? path : path.substr(0, slash);
    }
    return "";
}

bool FileSystemBrowser::isUsbPath(const std::string& path) const {
    return path == kUsbRoot || !usbMountOf(path).empty();
}

std::string FileSystemBrowser::usbMountLabel(const std::string& mount) const {
    if (evo_ntfs_is_url(mount.c_str())) {   /* #217: "NTFS DRIVE (Movies)" from the volume label */
        for (int i = 0; i < evo_ntfs_volume_count(); ++i) {
            const char* url = evo_ntfs_volume_url(i);
            if (url && mount == url) {
                const char* label = evo_ntfs_volume_label(i);
                return std::string("NTFS DRIVE (") + (label ? label : mount.c_str() + 7) + ")";
            }
        }
        return "NTFS DRIVE (" + mount.substr(sizeof(EVO_NTFS_SCHEME) - 1) + ")";
    }
    const std::string name = mount.substr(mount.find_last_of('/') + 1);   /* usb1, ext0 */
    return (name.rfind("ext", 0) == 0 ? "M.2 STORAGE (" : "USB DRIVE (") + name + ")";
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
    if (isUsbPath(path)) return true;
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
    // If currently searching, navigate into the matching entry
    if (!m_searchQuery.empty()) {
        for (const auto& entry : m_entries) {
            if ((entry.name == folderName || entry.relativePath == folderName || entry.fullPath == folderName) &&
                entry.category == FileCategory::Folder) {
                m_currentPath = entry.fullPath;
                clearSearch();
                return refresh();
            }
        }
    }

    if (m_currentPath.empty()) {
        // Picker mode: navigate into chosen source
        for (const auto& src : m_sources) {
            if (src.label == folderName) {
                m_currentPath = src.rootPath;
                clearSearch();
                return refresh();
            }
        }
        return false;
    }

    if (m_currentPath == kUsbRoot) {   /* the list of drives: entries carry the mount */
        for (const auto& entry : m_entries) {
            if (entry.name == folderName || entry.relativePath == folderName) {
                m_currentPath = entry.fullPath;
                clearSearch();
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

    if (m_currentPath == kUsbRoot) {   /* the top of the USB source */
        m_currentPath = "";
        clearSearch();
        return refresh();
    }
    /* A drive's own root goes back to the list of drives when there is one. */
    if (!usbMountOf(m_currentPath).empty() && usbMountOf(m_currentPath) == m_currentPath) {
        m_currentPath = (usbHome() == kUsbRoot) ? kUsbRoot : "";
        clearSearch();
        return refresh();
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
    m_usbMountsAtMs = 0;   /* rescan the drives once per refresh */

    evo_jailbreak_ensure();

    // Sentinel: at root picker above any source
    if (m_currentPath.empty()) {
        if (!m_searchQuery.empty()) {
            for (const auto& src : m_sources) {
                scanRecursive(src.rootPath, "", m_searchQuery, 0, src.label);
            }
            for (const auto& mount : usbMounts()) {   /* the other drives; usb0 is a source */
                if (mount != "/mnt/usb0") scanRecursive(mount, "", m_searchQuery, 0, usbMountLabel(mount));
            }
        } else {
            for (const auto& src : m_sources) {
                BrowserEntry entry;
                entry.name = src.label;
                entry.relativePath = src.label;
                entry.fullPath = src.rootPath;
                entry.category = FileCategory::Folder;
                entry.directoryType = 4; // DT_DIR
                m_entries.push_back(std::move(entry));
            }
            return true;
        }
    } else if (m_currentPath == kUsbRoot) {
        for (const auto& mount : usbMounts()) {
            const std::string label = usbMountLabel(mount);
            if (!m_searchQuery.empty()) {
                scanRecursive(mount, "", m_searchQuery, 0, label);
            } else {
                BrowserEntry entry;
                entry.name = label;
                entry.relativePath = label;
                entry.fullPath = mount;
                entry.category = FileCategory::Folder;
                entry.directoryType = 4; // DT_DIR
                m_entries.push_back(std::move(entry));
            }
        }
        if (m_searchQuery.empty()) return true;
    } else {
        if (!m_searchQuery.empty()) {
            scanRecursive(m_currentPath, "", m_searchQuery, 0);
        } else {
            scanDirectory(m_currentPath);
        }
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
    EVO_LOGI("files", "scanDirectory '%s'", dirPath.c_str());
    evo_dir_t* dir = evo_opendir(dirPath.c_str());
    if (!dir && dirPath.rfind("/data", 0) == 0) {
        EVO_LOGW("files", "evo_opendir('%s') null, ensuring unjail retry", dirPath.c_str());
        evo_jailbreak_ensure();
        dir = evo_opendir(dirPath.c_str());
    }
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
        EVO_LOGE("files", "scanDirectory '%s' failed (errno=%d)", dirPath.c_str(), errno);
        return;
    }

    struct dirent* entry;
    while ((entry = evo_readdir(dir)) != nullptr && m_entries.size() < 256) {
        if (entry->d_name[0] == '.') continue;
        if (std::strcmp(entry->d_name, "$RECYCLE.BIN") == 0) continue;
        if (std::strcmp(entry->d_name, "System Volume Information") == 0) continue;

        std::string fullPath = dirPath + "/" + entry->d_name;
        uint8_t dType = entry->d_type;
        if (dType == 0 || dType == 10 /* DT_LNK */) {
            struct stat st;
            if (stat(fullPath.c_str(), &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    dType = 4; // DT_DIR
                } else if (S_ISREG(st.st_mode)) {
                    dType = 8; // DT_REG
                }
            } else {
                evo_dir_t* testDir = evo_opendir(fullPath.c_str());
                if (testDir) {
                    dType = 4;
                    evo_closedir(testDir);
                }
            }
        }

        BrowserEntry bEntry;
        bEntry.name = entry->d_name;
        bEntry.relativePath = entry->d_name;
        bEntry.fullPath = fullPath;
        bEntry.directoryType = dType;
        bEntry.category = classifyFile(entry->d_name, dType);
        m_entries.push_back(std::move(bEntry));
    }

    EVO_LOGI("files", "scanDirectory '%s' complete, found %zu entries", dirPath.c_str(), m_entries.size());
    evo_closedir(dir);
}

void FileSystemBrowser::scanRecursive(const std::string& basePath, const std::string& relPath,
                                     const std::string& query, int depth, const std::string& sourcePrefix) {
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
        std::string childFullPath = basePath + "/" + itemRel;

        uint8_t dType = entry->d_type;
        if (dType == 0 || dType == 10 /* DT_LNK */) {
            struct stat st;
            if (stat(childFullPath.c_str(), &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    dType = 4; // DT_DIR
                } else if (S_ISREG(st.st_mode)) {
                    dType = 8; // DT_REG
                }
            } else {
                evo_dir_t* testDir = evo_opendir(childFullPath.c_str());
                if (testDir) {
                    dType = 4;
                    evo_closedir(testDir);
                }
            }
        }

        std::string displayName = sourcePrefix.empty() ? itemRel : (sourcePrefix + "/" + itemRel);

        if (CaseInsensitiveContains(entry->d_name, query) || CaseInsensitiveContains(itemRel, query) ||
            (!sourcePrefix.empty() && CaseInsensitiveContains(displayName, query))) {
            BrowserEntry bEntry;
            bEntry.name = displayName;
            bEntry.relativePath = itemRel;
            bEntry.fullPath = childFullPath;
            bEntry.directoryType = dType;
            bEntry.category = classifyFile(entry->d_name, dType);
            m_entries.push_back(std::move(bEntry));
        }

        if (dType == 4) { // DT_DIR
            scanRecursive(basePath, itemRel, query, depth + 1, sourcePrefix);
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
    if (!m_entries[index].fullPath.empty()) {
        return m_entries[index].fullPath;
    }
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
    if (ext == "mkv"  || ext == "mp4"  || ext == "mov"  || ext == "m4v"  ||
        ext == "avi"  || ext == "webm" || ext == "ts"   || ext == "m2ts" ||
        ext == "mpg"  || ext == "mpeg" || ext == "wmv"  || ext == "flv"  ||
        /* playable since the 2026-09-24 video codec pass */
        ext == "asf"  || ext == "wm"   || ext == "rm"   || ext == "rmvb" ||
        ext == "ogv"  || ext == "dv"   || ext == "mxf"  || ext == "mts"  ||
        ext == "vob"  || ext == "m2v"  || ext == "3gp"  || ext == "3g2"  ||
        /* .ivf and .obu are raw AV1. The obu demuxer drags in av1_parser and
         * av1_frame_merge; both are built again as of #94 - see the parser
         * note in scripts/build-ffmpeg.sh. */
        ext == "mp2"  || ext == "f4v"  || ext == "ivf"  || ext == "obu") {
        return FileCategory::Video;
    }

    /*
     * Audio.
     *
     * A file that does not land in a category the browser knows is not shown
     * as playable, so this list is the other half of what the build can
     * decode - six extensions here meant an .opus or .wma sat in the folder
     * looking like a stray file while FFmpeg was perfectly able to play it.
     * Kept in step with AUDIO_DECODERS/AUDIO_DEMUXERS in build-ffmpeg.sh,
     * which is now every audio codec FFmpeg decodes natively.
     */
    if (ext == "mp3"  || ext == "flac" || ext == "wav"  || ext == "aac"  ||
        ext == "m4a"  || ext == "ogg"  || ext == "opus" || ext == "oga"  ||
        ext == "mka"  || ext == "m4b"  || ext == "wma"  || ext == "ac3"  ||
        ext == "eac3" || ext == "dts"  || ext == "dtshd"|| ext == "thd"  ||
        ext == "mlp"  || ext == "mp2"  || ext == "mpa"  || ext == "ape"  ||
        ext == "wv"   || ext == "tta"  || ext == "tak"  || ext == "shn"  ||
        ext == "aif"  || ext == "aiff" || ext == "au"   || ext == "caf"  ||
        ext == "w64"  || ext == "dsf"  || ext == "dff"  || ext == "amr"  ||
        ext == "awb"  || ext == "mpc"  || ext == "voc"  || ext == "ra"   ||
        ext == "oma"  || ext == "aa3"  ||
        ext == "at9"  || ext == "gsm"  || ext == "qoa") {
        return FileCategory::Audio;
    }

    // Image
    if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "bmp" || ext == "webp") {
        return FileCategory::Image;
    }

    // Document
    if (ext == "srt" || ext == "ass" || ext == "ssa" || ext == "vtt" ||
        /* external subtitle formats the build now decodes */
        ext == "sub" || ext == "smi" || ext == "sami" || ext == "mpl" ||
        ext == "jss" || ext == "rt"  || ext == "stl"  ||
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
            /* setCurrentPath, not a bare assignment: assigning m_currentPath
             * left m_entries empty, so the browser opened on the restored
             * folder showing nothing until you switched source and back
             * (navigateToSource is what actually refreshed). */
            setCurrentPath(savedFolder);
        }
    }
}

} // namespace evo
