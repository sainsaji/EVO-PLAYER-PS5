#include "evo/services/NetArtCache.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace evo {

#pragma pack(push, 1)
struct CacheHeader {
    char magic[4];     // 'N', 'A', 'R', 'T'
    uint32_t w;
    uint32_t h;
    uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(CacheHeader) == 16, "CacheHeader must be exactly 16 bytes");

static const char CACHE_MAGIC[4] = {'N', 'A', 'R', 'T'};

static uint64_t fnv1a64(const std::string& str) {
    uint64_t hash = 14695981039346656037ULL; // 0xcbf29ce484222325ULL
    for (char c : str) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ULL;            // 0x100000001b3ULL
    }
    return hash;
}

static bool isCacheFilename(const std::string& name) {
    if (name.size() != 21) { // 16 hex digits + ".nart"
        return false;
    }
    if (name.compare(16, 5, ".nart") != 0) {
        return false;
    }
    for (size_t i = 0; i < 16; ++i) {
        char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

NetArtCache::NetArtCache(const std::string& dir, size_t maxBytes)
    : m_dir(dir), m_maxBytes(maxBytes) {
    while (m_dir.size() > 1 && (m_dir.back() == '/' || m_dir.back() == '\\')) {
        m_dir.pop_back();
    }
    mkdir(m_dir.c_str(), 0777);
}

std::string NetArtCache::keyToFilename(const std::string& key) const {
    uint64_t hash = fnv1a64(key);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx.nart", static_cast<unsigned long long>(hash));
    return std::string(buf);
}

bool NetArtCache::get(const std::string& key, uint32_t* outPixels, int w, int h) {
    if (!outPixels || w <= 0 || h <= 0) {
        return false;
    }

    std::string path = m_dir + "/" + keyToFilename(key);
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        return false;
    }

    CacheHeader hdr;
    if (std::fread(&hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
        std::fclose(fp);
        return false;
    }

    if (std::memcmp(hdr.magic, CACHE_MAGIC, 4) != 0) {
        std::fclose(fp);
        return false;
    }

    if (hdr.w != static_cast<uint32_t>(w) || hdr.h != static_cast<uint32_t>(h)) {
        std::fclose(fp);
        return false;
    }

    size_t pixelBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * sizeof(uint32_t);
    size_t readBytes = std::fread(outPixels, 1, pixelBytes, fp);
    if (readBytes != pixelBytes) {
        std::fclose(fp);
        return false;
    }

    // Check for corrupt/extra trailing bytes
    char trailing;
    if (std::fread(&trailing, 1, 1, fp) != 0) {
        std::fclose(fp);
        return false;
    }

    std::fclose(fp);
    return true;
}

void NetArtCache::put(const std::string& key, const uint32_t* pixels, int w, int h) {
    if (!pixels || w <= 0 || h <= 0) {
        return;
    }

    mkdir(m_dir.c_str(), 0777);

    std::string targetFilename = keyToFilename(key);
    std::string targetPath = m_dir + "/" + targetFilename;

    static std::atomic<uint64_t> s_tmpCounter{0};
    std::string tmpPath = targetPath + ".tmp." + std::to_string(++s_tmpCounter);

    FILE* fp = std::fopen(tmpPath.c_str(), "wb");
    if (!fp) {
        return;
    }

    CacheHeader hdr;
    std::memcpy(hdr.magic, CACHE_MAGIC, 4);
    hdr.w = static_cast<uint32_t>(w);
    hdr.h = static_cast<uint32_t>(h);
    hdr.reserved = 0;

    if (std::fwrite(&hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
        std::fclose(fp);
        unlink(tmpPath.c_str());
        return;
    }

    size_t pixelBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * sizeof(uint32_t);
    if (std::fwrite(pixels, 1, pixelBytes, fp) != pixelBytes) {
        std::fclose(fp);
        unlink(tmpPath.c_str());
        return;
    }

    std::fclose(fp);

    if (std::rename(tmpPath.c_str(), targetPath.c_str()) != 0) {
        unlink(tmpPath.c_str());
        return;
    }

    evictIfNeeded();
}

size_t NetArtCache::sizeBytes() {
    DIR* dirp = opendir(m_dir.c_str());
    if (!dirp) {
        return 0;
    }

    size_t total = 0;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dirp)) != nullptr) {
        if (!isCacheFilename(entry->d_name)) {
            continue;
        }
        std::string fullPath = m_dir + "/" + entry->d_name;
        struct stat st;
        if (stat(fullPath.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            total += static_cast<size_t>(st.st_size);
        }
    }
    closedir(dirp);
    return total;
}

struct CacheFileInfo {
    std::string fullPath;
    size_t size = 0;
    time_t mtimeSec = 0;
    long mtimeNsec = 0;
};

void NetArtCache::evictIfNeeded() {
    DIR* dirp = opendir(m_dir.c_str());
    if (!dirp) {
        return;
    }

    std::vector<CacheFileInfo> files;
    size_t total = 0;
    struct dirent* entry = nullptr;

    while ((entry = readdir(dirp)) != nullptr) {
        if (!isCacheFilename(entry->d_name)) {
            continue;
        }
        std::string fullPath = m_dir + "/" + entry->d_name;
        struct stat st;
        if (stat(fullPath.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            CacheFileInfo info;
            info.fullPath = fullPath;
            info.size = static_cast<size_t>(st.st_size);
            info.mtimeSec = st.st_mtime;
#if defined(__linux__) || defined(__GLIBC__)
            info.mtimeNsec = st.st_mtim.tv_nsec;
#else
            info.mtimeNsec = 0;
#endif
            files.push_back(info);
            total += info.size;
        }
    }
    closedir(dirp);

    if (total <= m_maxBytes) {
        return;
    }

    // Oldest files first (ascending mtime)
    std::sort(files.begin(), files.end(), [](const CacheFileInfo& a, const CacheFileInfo& b) {
        if (a.mtimeSec != b.mtimeSec) {
            return a.mtimeSec < b.mtimeSec;
        }
        if (a.mtimeNsec != b.mtimeNsec) {
            return a.mtimeNsec < b.mtimeNsec;
        }
        return a.fullPath < b.fullPath;
    });

    for (const auto& item : files) {
        if (total <= m_maxBytes) {
            break;
        }
        if (unlink(item.fullPath.c_str()) == 0) {
            if (total >= item.size) {
                total -= item.size;
            } else {
                total = 0;
            }
        }
    }
}

} // namespace evo
