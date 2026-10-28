#ifndef EVO_NET_ART_CACHE_HPP
#define EVO_NET_ART_CACHE_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace evo {

// A capped on-disk cache of already-scaled posters (raw BGRA, 320x180 = 230400 bytes, plus a 16-byte header: magic 'NART', w, h, reserved).
class NetArtCache {
public:
    NetArtCache(const std::string& dir, size_t maxBytes);        // creates dir if missing (mkdir, one level)
    // key = std::string made by the caller: server + path (credentials already stripped) + size + mtime.
    bool get(const std::string& key, uint32_t* outPixels, int w, int h);   // false on miss or bad header/size
    void put(const std::string& key, const uint32_t* pixels, int w, int h); // writes tmp file then rename(); then evicts
    size_t sizeBytes();  // total of cache files, for tests

private:
    std::string m_dir;
    size_t m_maxBytes;

    std::string keyToFilename(const std::string& key) const;
    void evictIfNeeded();
};

} // namespace evo

#endif // EVO_NET_ART_CACHE_HPP
