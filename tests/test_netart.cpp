/*
 * tests/test_netart.cpp — Unit tests for network cover art sidecar resolution and on-disk art cache.
 */

#include "evo/services/NetArtSidecar.hpp"
#include "evo/services/NetArtCache.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

extern "C" {
    extern int g_tests_run;
    extern int g_tests_passed;
    extern int g_tests_failed;
}

#define TEST_START(name) do { \
    g_tests_run++; \
    printf("  RUN  %-50s", name); \
    fflush(stdout); \
} while(0)

#define TEST_PASS() do { \
    g_tests_passed++; \
    printf(" [ PASS ]\n"); \
} while(0)

#define TEST_FAIL(reason) do { \
    g_tests_failed++; \
    printf(" [ FAIL ] (%s at line %d)\n", reason, __LINE__); \
} while(0)

#define TEST_ASSERT(cond, reason) do { \
    if (!(cond)) { \
        TEST_FAIL(reason); \
        return; \
    } \
} while(0)

static void clean_dir(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
            continue;
        }
        std::string p = dir + "/" + e->d_name;
        unlink(p.c_str());
    }
    closedir(d);
}

static std::string testKeyToFilename(const std::string& key) {
    uint64_t hash = 14695981039346656037ULL;
    for (char c : key) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ULL;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx.nart", static_cast<unsigned long long>(hash));
    return std::string(buf);
}

static void set_file_mtime(const std::string& path, time_t t) {
    struct utimbuf ub;
    ub.actime = t;
    ub.modtime = t;
    utime(path.c_str(), &ub);
}

// ----------------------------------------------------------------------------
// Sidecar Unit Tests
// ----------------------------------------------------------------------------

static void test_sidecar_tier_priority(void) {
    TEST_START("NetArt: Sidecar Tier Priority (1-5)");

    std::string media = "Inception.mkv";

    // 1. Each tier alone
    {
        std::vector<evo::SidecarEntry> s1 = { {"other.jpg", "url", false}, {"Inception-poster.jpg", "url1", false} };
        TEST_ASSERT(evo::pickSidecar(media, s1) == 1, "tier 1 (<name>-poster.<ext>) should be selected");
    }
    {
        std::vector<evo::SidecarEntry> s2 = { {"other.jpg", "url", false}, {"Inception.jpg", "url2", false} };
        TEST_ASSERT(evo::pickSidecar(media, s2) == 1, "tier 2 (<name>.<ext>) should be selected");
    }
    {
        std::vector<evo::SidecarEntry> s3 = { {"other.jpg", "url", false}, {"poster.jpg", "url3", false} };
        TEST_ASSERT(evo::pickSidecar(media, s3) == 1, "tier 3 (poster.<ext>) should be selected");
    }
    {
        std::vector<evo::SidecarEntry> s4 = { {"other.jpg", "url", false}, {"folder.jpg", "url4", false} };
        TEST_ASSERT(evo::pickSidecar(media, s4) == 1, "tier 4 (folder.<ext>) should be selected");
    }
    {
        std::vector<evo::SidecarEntry> s5 = { {"other.jpg", "url", false}, {"cover.jpg", "url5", false} };
        TEST_ASSERT(evo::pickSidecar(media, s5) == 1, "tier 5 (cover.<ext>) should be selected");
    }

    // 2. Cross-tier precedence: Tier 1 beats Tier 2, etc., even if lower tier has preferred extension (.jpg vs .png)
    {
        std::vector<evo::SidecarEntry> sibs = {
            {"Inception.jpg", "url2", false},          // Tier 2 (.jpg)
            {"Inception-poster.png", "url1", false}     // Tier 1 (.png)
        };
        TEST_ASSERT(evo::pickSidecar(media, sibs) == 1, "Tier 1 should beat Tier 2 regardless of extension");
    }
    {
        std::vector<evo::SidecarEntry> sibs = {
            {"poster.jpg", "url3", false},              // Tier 3 (.jpg)
            {"Inception.png", "url2", false}            // Tier 2 (.png)
        };
        TEST_ASSERT(evo::pickSidecar(media, sibs) == 1, "Tier 2 should beat Tier 3 regardless of extension");
    }
    {
        std::vector<evo::SidecarEntry> sibs = {
            {"folder.jpg", "url4", false},              // Tier 4 (.jpg)
            {"poster.png", "url3", false}               // Tier 3 (.png)
        };
        TEST_ASSERT(evo::pickSidecar(media, sibs) == 1, "Tier 3 should beat Tier 4 regardless of extension");
    }
    {
        std::vector<evo::SidecarEntry> sibs = {
            {"cover.jpg", "url5", false},               // Tier 5 (.jpg)
            {"folder.png", "url4", false}               // Tier 4 (.png)
        };
        TEST_ASSERT(evo::pickSidecar(media, sibs) == 1, "Tier 4 should beat Tier 5 regardless of extension");
    }

    // 3. All tiers present simultaneously
    {
        std::vector<evo::SidecarEntry> allTiers = {
            {"cover.jpg", "url5", false},
            {"folder.jpg", "url4", false},
            {"Inception-poster.png", "url1", false},
            {"poster.jpg", "url3", false},
            {"Inception.jpeg", "url2", false}
        };
        TEST_ASSERT(evo::pickSidecar(media, allTiers) == 2, "Tier 1 must win when all tiers are present");
    }

    TEST_PASS();
}

static void test_sidecar_extension_precedence(void) {
    TEST_START("NetArt: Extension Preference (.jpg > .jpeg > .png)");

    std::string media = "Matrix.mkv";

    // Tier 1
    {
        std::vector<evo::SidecarEntry> s = {
            {"Matrix-poster.png", "url1", false},
            {"Matrix-poster.jpeg", "url2", false},
            {"Matrix-poster.jpg", "url3", false}
        };
        TEST_ASSERT(evo::pickSidecar(media, s) == 2, "Tier 1: .jpg should beat .jpeg and .png");
    }
    {
        std::vector<evo::SidecarEntry> s = {
            {"Matrix-poster.png", "url1", false},
            {"Matrix-poster.jpeg", "url2", false}
        };
        TEST_ASSERT(evo::pickSidecar(media, s) == 1, "Tier 1: .jpeg should beat .png");
    }

    // Tier 2
    {
        std::vector<evo::SidecarEntry> s = {
            {"Matrix.png", "url1", false},
            {"Matrix.jpeg", "url2", false},
            {"Matrix.jpg", "url3", false}
        };
        TEST_ASSERT(evo::pickSidecar(media, s) == 2, "Tier 2: .jpg should beat .jpeg and .png");
    }

    // Tier 3
    {
        std::vector<evo::SidecarEntry> s = {
            {"poster.png", "url1", false},
            {"poster.jpeg", "url2", false},
            {"poster.jpg", "url3", false}
        };
        TEST_ASSERT(evo::pickSidecar(media, s) == 2, "Tier 3: .jpg should beat .jpeg and .png");
    }

    // Tier 4
    {
        std::vector<evo::SidecarEntry> s = {
            {"folder.png", "url1", false},
            {"folder.jpeg", "url2", false},
            {"folder.jpg", "url3", false}
        };
        TEST_ASSERT(evo::pickSidecar(media, s) == 2, "Tier 4: .jpg should beat .jpeg and .png");
    }

    // Tier 5
    {
        std::vector<evo::SidecarEntry> s = {
            {"cover.png", "url1", false},
            {"cover.jpeg", "url2", false},
            {"cover.jpg", "url3", false}
        };
        TEST_ASSERT(evo::pickSidecar(media, s) == 2, "Tier 5: .jpg should beat .jpeg and .png");
    }

    TEST_PASS();
}

static void test_sidecar_case_insensitivity(void) {
    TEST_START("NetArt: Case-Insensitivity Across Tiers & Exts");

    // Uppercase media name & mixed-case candidates
    std::string media = "INTERSTELLAR.MKV";

    {
        std::vector<evo::SidecarEntry> s1 = { {"interstellar-POSTER.JPG", "url1", false} };
        TEST_ASSERT(evo::pickSidecar(media, s1) == 0, "case-insensitive tier 1");
    }
    {
        std::vector<evo::SidecarEntry> s2 = { {"Interstellar.JPEG", "url2", false} };
        TEST_ASSERT(evo::pickSidecar(media, s2) == 0, "case-insensitive tier 2");
    }
    {
        std::vector<evo::SidecarEntry> s3 = { {"POSTER.PNG", "url3", false} };
        TEST_ASSERT(evo::pickSidecar(media, s3) == 0, "case-insensitive tier 3");
    }
    {
        std::vector<evo::SidecarEntry> s4 = { {"FoLdEr.JpG", "url4", false} };
        TEST_ASSERT(evo::pickSidecar(media, s4) == 0, "case-insensitive tier 4");
    }
    {
        std::vector<evo::SidecarEntry> s5 = { {"cOvEr.JpEg", "url5", false} };
        TEST_ASSERT(evo::pickSidecar(media, s5) == 0, "case-insensitive tier 5");
    }

    TEST_PASS();
}

static void test_sidecar_edge_cases(void) {
    TEST_START("NetArt: Sidecar Edge Cases & Exclusions");

    // 1. Explicit requirement: "Movie.2020.mkv" -> "Movie.2020.jpg"
    {
        std::vector<evo::SidecarEntry> s = {
            {"Movie.2020.nfo", "url0", false},
            {"Movie.2020.jpg", "url1", false},
            {"Movie.2020.srt", "url2", false}
        };
        TEST_ASSERT(evo::pickSidecar("Movie.2020.mkv", s) == 1,
                    "Movie.2020.mkv should match Movie.2020.jpg (Tier 2)");
    }

    // 2. Explicit requirement: directory named poster.jpg ignored
    {
        std::vector<evo::SidecarEntry> s = {
            {"poster.jpg", "url0", true}, // directory!
            {"folder.jpg", "url1", false}
        };
        TEST_ASSERT(evo::pickSidecar("Movie.mkv", s) == 1,
                    "directory named poster.jpg must be ignored, picking folder.jpg");
    }
    {
        std::vector<evo::SidecarEntry> s = {
            {"poster.jpg", "url0", true}  // directory!
        };
        TEST_ASSERT(evo::pickSidecar("Movie.mkv", s) == -1,
                    "only directory candidate returns -1");
    }

    // 3. Never pick the media file itself
    {
        std::vector<evo::SidecarEntry> s = {
            {"poster.jpg", "url0", false},
            {"folder.jpg", "url1", false}
        };
        TEST_ASSERT(evo::pickSidecar("poster.jpg", s) == 1,
                    "media file itself (poster.jpg) must not be picked as its own cover");
    }
    {
        std::vector<evo::SidecarEntry> s = {
            {"Movie.jpg", "url0", false}
        };
        TEST_ASSERT(evo::pickSidecar("Movie.jpg", s) == -1,
                    "media file itself cannot be picked");
    }

    // 4. No match: empty siblings, non-images, unapproved extensions, unrelated names
    {
        std::vector<evo::SidecarEntry> empty;
        TEST_ASSERT(evo::pickSidecar("Movie.mkv", empty) == -1, "empty siblings returns -1");
        TEST_ASSERT(evo::pickSidecar("", empty) == -1, "empty media returns -1");
    }
    {
        std::vector<evo::SidecarEntry> unapprovedExts = {
            {"poster.webp", "url1", false},
            {"poster.bmp", "url2", false},
            {"poster.gif", "url3", false},
            {"poster.txt", "url4", false}
        };
        TEST_ASSERT(evo::pickSidecar("Movie.mkv", unapprovedExts) == -1,
                    "unapproved extensions must not be picked");
    }
    {
        std::vector<evo::SidecarEntry> unrelatedNames = {
            {"fanart.jpg", "url1", false},
            {"backdrop.jpg", "url2", false},
            {"banner.jpg", "url3", false},
            {"thumb.jpg", "url4", false}
        };
        TEST_ASSERT(evo::pickSidecar("Movie.mkv", unrelatedNames) == -1,
                    "unrelated names must not be picked");
    }

    // 5. Media name containing folder path
    {
        std::vector<evo::SidecarEntry> s = {
            {"Movie-poster.jpg", "url1", false}
        };
        TEST_ASSERT(evo::pickSidecar("/mnt/smb/Movies/Action/Movie.mkv", s) == 0,
                    "media name with path prefix should match base filename");
    }

    TEST_PASS();
}

// ----------------------------------------------------------------------------
// Cache Unit Tests
// ----------------------------------------------------------------------------

static void test_netart_cache_put_get(void) {
    TEST_START("NetArtCache: Put/Get Round Trip & Key Misses");

    std::string testDir = "/tmp/test_netart_cache_pg";
    clean_dir(testDir);

    evo::NetArtCache cache(testDir, 10 * 1024 * 1024);

    const int w = 320;
    const int h = 180;
    const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);

    std::vector<uint32_t> inPixels(pixelCount);
    for (size_t i = 0; i < pixelCount; ++i) {
        // Distinct BGRA pattern
        inPixels[i] = static_cast<uint32_t>(0xFF000000 | ((i & 0xFF) << 16) | (((i >> 4) & 0xFF) << 8) | ((i >> 8) & 0xFF));
    }

    std::string key = "smb://nas.lan/media/video1.mkv:1048576:1700000000";
    cache.put(key, inPixels.data(), w, h);

    // 1. Get exact key
    std::vector<uint32_t> outPixels(pixelCount, 0);
    bool ok = cache.get(key, outPixels.data(), w, h);
    TEST_ASSERT(ok, "cache.get should return true on exact hit");
    TEST_ASSERT(std::memcmp(inPixels.data(), outPixels.data(), pixelCount * sizeof(uint32_t)) == 0,
                "cached pixel data must match original buffer exactly");

    // 2. Expected sizeBytes: 16 header + 320*180*4 = 230416 bytes
    TEST_ASSERT(cache.sizeBytes() == (16 + 230400), "cache sizeBytes must match header + raw BGRA payload");

    // 3. Miss on size change
    std::string keyChangedSize = "smb://nas.lan/media/video1.mkv:1048577:1700000000";
    std::vector<uint32_t> missPixels(pixelCount, 0);
    TEST_ASSERT(!cache.get(keyChangedSize, missPixels.data(), w, h), "size change should miss");

    // 4. Miss on mtime change
    std::string keyChangedMtime = "smb://nas.lan/media/video1.mkv:1048576:1700000001";
    TEST_ASSERT(!cache.get(keyChangedMtime, missPixels.data(), w, h), "mtime change should miss");

    // 5. Miss on completely non-existent key
    TEST_ASSERT(!cache.get("nonexistent_key", missPixels.data(), w, h), "unknown key should miss");

    // 6. Miss on dimension mismatch
    TEST_ASSERT(!cache.get(key, missPixels.data(), 320, 181), "dimension mismatch (height) should miss");
    TEST_ASSERT(!cache.get(key, missPixels.data(), 160, 90), "dimension mismatch (half-res) should miss");

    clean_dir(testDir);
    TEST_PASS();
}

static void test_netart_cache_corrupt_short(void) {
    TEST_START("NetArtCache: Corrupt, Truncated & Extra Bytes");

    std::string testDir = "/tmp/test_netart_cache_corrupt";
    clean_dir(testDir);

    evo::NetArtCache cache(testDir, 10 * 1024 * 1024);
    std::vector<uint32_t> out(320 * 180, 0);

    // 1. Truncated header (< 16 bytes)
    std::string keyShortHdr = "corrupt_short_hdr";
    std::string pathShortHdr = testDir + "/" + testKeyToFilename(keyShortHdr);
    {
        FILE* fp = std::fopen(pathShortHdr.c_str(), "wb");
        TEST_ASSERT(fp != nullptr, "failed to create test file");
        char dummy[8] = {'N', 'A', 'R', 'T', 1, 2, 3, 4};
        std::fwrite(dummy, 1, sizeof(dummy), fp);
        std::fclose(fp);
    }
    TEST_ASSERT(!cache.get(keyShortHdr, out.data(), 320, 180), "file with < 16 byte header must fail get");

    // 2. Bad magic (16 bytes, but not 'NART')
    std::string keyBadMagic = "corrupt_bad_magic";
    std::string pathBadMagic = testDir + "/" + testKeyToFilename(keyBadMagic);
    {
        FILE* fp = std::fopen(pathBadMagic.c_str(), "wb");
        TEST_ASSERT(fp != nullptr, "failed to create test file");
        uint32_t badHdr[4] = { 0x58585858, 320, 180, 0 }; // 'XXXX'
        std::fwrite(badHdr, 1, sizeof(badHdr), fp);
        std::fclose(fp);
    }
    TEST_ASSERT(!cache.get(keyBadMagic, out.data(), 320, 180), "file with bad magic must fail get");

    // 3. Valid header but truncated pixel payload (< 230400 bytes)
    std::string keyShortPayload = "corrupt_short_payload";
    std::string pathShortPayload = testDir + "/" + testKeyToFilename(keyShortPayload);
    {
        FILE* fp = std::fopen(pathShortPayload.c_str(), "wb");
        TEST_ASSERT(fp != nullptr, "failed to create test file");
        char magic[4] = {'N', 'A', 'R', 'T'};
        uint32_t w = 320, h = 180, res = 0;
        std::fwrite(magic, 1, 4, fp);
        std::fwrite(&w, sizeof(w), 1, fp);
        std::fwrite(&h, sizeof(h), 1, fp);
        std::fwrite(&res, sizeof(res), 1, fp);
        // Write only 128 bytes instead of 230400
        uint8_t shortData[128] = {0};
        std::fwrite(shortData, 1, sizeof(shortData), fp);
        std::fclose(fp);
    }
    TEST_ASSERT(!cache.get(keyShortPayload, out.data(), 320, 180), "file with truncated payload must fail get");

    // 4. Valid header and pixels, but corrupted with extra trailing bytes
    std::string keyExtra = "corrupt_extra_bytes";
    std::string pathExtra = testDir + "/" + testKeyToFilename(keyExtra);
    {
        FILE* fp = std::fopen(pathExtra.c_str(), "wb");
        TEST_ASSERT(fp != nullptr, "failed to create test file");
        char magic[4] = {'N', 'A', 'R', 'T'};
        uint32_t w = 320, h = 180, res = 0;
        std::fwrite(magic, 1, 4, fp);
        std::fwrite(&w, sizeof(w), 1, fp);
        std::fwrite(&h, sizeof(h), 1, fp);
        std::fwrite(&res, sizeof(res), 1, fp);
        std::vector<uint32_t> data(320 * 180, 0x11223344);
        std::fwrite(data.data(), sizeof(uint32_t), data.size(), fp);
        // Write extra trailing junk
        const char junk[] = "EXTRA_JUNK";
        std::fwrite(junk, 1, sizeof(junk), fp);
        std::fclose(fp);
    }
    TEST_ASSERT(!cache.get(keyExtra, out.data(), 320, 180), "file with extra trailing bytes must fail get");

    clean_dir(testDir);
    TEST_PASS();
}

static void test_netart_cache_eviction(void) {
    TEST_START("NetArtCache: LRU Eviction Under Max Cap");

    std::string testDir = "/tmp/test_netart_cache_evict";
    clean_dir(testDir);

    // Each cache file: 16 bytes header + 320*180*4 = 230416 bytes.
    // Set cap to exactly 2 files (230416 * 2 = 460832 bytes).
    const size_t fileBytes = 16 + (320 * 180 * sizeof(uint32_t));
    const size_t maxCap = fileBytes * 2;

    evo::NetArtCache cache(testDir, maxCap);

    std::vector<uint32_t> pixels(320 * 180, 0xAA001122);
    std::vector<uint32_t> out(320 * 180, 0);

    std::string key1 = "server1:/path/item1.mkv:100:1000";
    std::string key2 = "server1:/path/item2.mkv:200:2000";
    std::string key3 = "server1:/path/item3.mkv:300:3000";

    // 1. Put item 1
    cache.put(key1, pixels.data(), 320, 180);
    std::string path1 = testDir + "/" + testKeyToFilename(key1);
    set_file_mtime(path1, 1000); // Oldest
    TEST_ASSERT(cache.sizeBytes() == fileBytes, "size after item 1 should be 1 file");
    TEST_ASSERT(cache.get(key1, out.data(), 320, 180), "item 1 should be cached");

    // 2. Put item 2
    cache.put(key2, pixels.data(), 320, 180);
    std::string path2 = testDir + "/" + testKeyToFilename(key2);
    set_file_mtime(path2, 2000); // Middle
    TEST_ASSERT(cache.sizeBytes() == fileBytes * 2, "size after item 2 should be 2 files");
    TEST_ASSERT(cache.get(key1, out.data(), 320, 180), "item 1 should still be cached");
    TEST_ASSERT(cache.get(key2, out.data(), 320, 180), "item 2 should be cached");

    // 3. Put item 3 -> exceeds maxCap, should evict oldest (item 1 with mtime 1000)
    cache.put(key3, pixels.data(), 320, 180);
    std::string path3 = testDir + "/" + testKeyToFilename(key3);
    set_file_mtime(path3, 3000); // Newest

    TEST_ASSERT(cache.sizeBytes() <= maxCap, "cache total size must stay under or equal to max cap");
    TEST_ASSERT(cache.sizeBytes() == fileBytes * 2, "cache should retain exactly 2 files after eviction");

    // Verify item 1 was evicted and items 2 & 3 remain
    TEST_ASSERT(!cache.get(key1, out.data(), 320, 180), "oldest item 1 must be evicted");
    TEST_ASSERT(cache.get(key2, out.data(), 320, 180), "item 2 must remain cached");
    TEST_ASSERT(cache.get(key3, out.data(), 320, 180), "item 3 must remain cached");

    clean_dir(testDir);
    TEST_PASS();
}

// ----------------------------------------------------------------------------
// Test Runner Harness Hook
// ----------------------------------------------------------------------------

extern "C" void run_netart_tests(void) {
    test_sidecar_tier_priority();
    test_sidecar_extension_precedence();
    test_sidecar_case_insensitivity();
    test_sidecar_edge_cases();
    test_netart_cache_put_get();
    test_netart_cache_corrupt_short();
    test_netart_cache_eviction();
}

__attribute__((constructor)) static void auto_run_netart_tests(void) {
    run_netart_tests();
}
