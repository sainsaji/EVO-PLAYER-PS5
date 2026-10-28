#ifndef EVO_I_FILE_SYSTEM_BROWSER_HPP
#define EVO_I_FILE_SYSTEM_BROWSER_HPP

#include "evo/Common.hpp"
#include <string>
#include <vector>

namespace evo {

struct MediaSourceInfo {
    std::string label;
    std::string badge;
    std::string rootPath;
};

struct BrowserEntry {
    std::string name;
    std::string relativePath;
    std::string fullPath;
    FileCategory category = FileCategory::Unknown;
    uint8_t directoryType = 0;
    /* Network listings only: the cover image picked from the same listing (#230).
     * artPath is the sidecar's path on the server; artKey is server + path + size +
     * mtime of that sidecar, so the disk cache refetches only when it changes. Both
     * are empty when there is no match or the source has cover art switched off. */
    std::string artPath;
    std::string artKey;
};

/**
 * @brief Interface for local and external storage directory exploration and search.
 */
class IFileSystemBrowser {
public:
    virtual ~IFileSystemBrowser() = default;

    virtual const std::vector<MediaSourceInfo>& getSources() const = 0;
    virtual const std::string& getCurrentPath() const = 0;
    virtual void setCurrentPath(const std::string& path) = 0;

    virtual bool navigateToSource(size_t sourceIndex) = 0;
    virtual bool navigateInto(const std::string& folderName) = 0;
    virtual bool navigateUp() = 0;
    virtual bool refresh() = 0;

    virtual const std::vector<BrowserEntry>& getEntries() const = 0;
    virtual size_t getEntryCount() const = 0;
    virtual const BrowserEntry* getEntry(size_t index) const = 0;

    virtual void search(const std::string& query) = 0;
    virtual void clearSearch() = 0;
    virtual bool isSearching() const = 0;
    virtual const std::string& getSearchQuery() const = 0;

    virtual void saveLastFolder() = 0;
    virtual void loadLastFolder() = 0;

    /* Every USB drive and M.2 extension the console mounts (usb0-usb7, ext0,
     * ext1) that has something on it. usbHome() is where the USB source opens:
     * the one drive's own root, or kUsbRoot - a list of the drives - when
     * there are several. */
    virtual std::vector<std::string> usbMounts() const = 0;
    virtual std::string usbHome() const = 0;
    virtual bool isUsbPath(const std::string& path) const = 0;
    /* "/mnt/usb1/Movies" -> "/mnt/usb1"; "" when path is on no drive. */
    virtual std::string usbMountOf(const std::string& path) const = 0;
    virtual std::string usbMountLabel(const std::string& mount) const = 0;

    virtual std::string getFullPath(size_t index) const = 0;
    virtual FileCategory classifyFile(const std::string& fileName, int entryType) const = 0;
    virtual const char* getFileCategoryLabel(FileCategory category) const = 0;

    /* Library view across the whole source - see FileSystemBrowser. Unknown
     * turns it off and returns to ordinary directory listing. */
};

} // namespace evo

#endif // EVO_I_FILE_SYSTEM_BROWSER_HPP
