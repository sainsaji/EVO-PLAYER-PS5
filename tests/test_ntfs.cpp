/*
 * tests/test_ntfs.cpp - EVO's NTFS reader (#217) against real NTFS images
 * made by ntfs-3g (tools/make_ntfs_image.sh -> output/ntfs/*.img): a volume
 * on the whole disk, an MBR partition at sector 63, a GPT partition with
 * 64 KiB clusters. Skipped when the images are not there.
 */

#include "evo_ntfs.h"
#include "evo_readdir.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
    extern int g_tests_run;
    extern int g_tests_passed;
    extern int g_tests_failed;
}

#define TEST_START(name) do { g_tests_run++; printf("  RUN  %-50s", name); fflush(stdout); } while (0)
#define TEST_PASS() do { g_tests_passed++; printf(" [ PASS ]\n"); } while (0)
#define TEST_FAIL(reason) do { g_tests_failed++; printf(" [ FAIL ] (%s at line %d)\n", reason, __LINE__); } while (0)
#define TEST_ASSERT(cond, reason) do { if (!(cond)) { TEST_FAIL(reason); return; } } while (0)

static std::vector<std::string> list(const std::string &url)
{
    std::vector<std::string> out;
    evo_ntfs_dir_t *d = evo_ntfs_opendir(url.c_str());
    if (!d) return out;
    const char *name;
    int is_dir;
    uint64_t size;
    while (evo_ntfs_readdir(d, &name, &is_dir, &size)) out.push_back(std::string(is_dir ? "D " : "F ") + name);
    evo_ntfs_closedir(d);
    return out;
}

static bool has(const std::vector<std::string> &v, const std::string &s)
{
    for (const auto &x : v)
        if (x == s) return true;
    return false;
}

static std::string read_all(const std::string &url)
{
    std::string s;
    evo_ntfs_file_t *f = evo_ntfs_open(url.c_str());
    if (!f) return "<open failed>";
    const uint64_t n = evo_ntfs_size(f);
    s.resize((size_t)n);
    uint64_t off = 0;
    while (off < n) {   /* odd chunk size: reads cross clusters and runs at odd places */
        long r = evo_ntfs_read(f, &s[(size_t)off], 70001, off);
        if (r <= 0) { s = "<read failed>"; break; }
        off += (uint64_t)r;
    }
    evo_ntfs_close(f);
    return s;
}

static std::string file_bytes(const char *path)
{
    std::string s;
    FILE *f = std::fopen(path, "rb");
    if (!f) return s;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

static void check_volume(const char *img, const char *name, const char *vol_url)
{
    char title[96];
    std::snprintf(title, sizeof title, "NTFS: %s (list, read, unicode, frag)", img);
    TEST_START(title);
    std::string path = std::string("output/ntfs/") + img;
    if (!std::fopen(path.c_str(), "rb")) {
        g_tests_passed++;
        printf(" [ SKIP ] (no %s - run tools/make_ntfs_image.sh)\n", path.c_str());
        return;
    }
    TEST_ASSERT(evo_ntfs_add_device(path.c_str(), name) == 1, "volume not found");
    const std::string v = vol_url;

    const auto root = list(v);
    TEST_ASSERT(has(root, "D Movies") && has(root, "D Big Folder") && has(root, "D Empty") && has(root, "F tiny.txt") &&
                    has(root, "F frag_c.bin") && has(root, "D \xc3\x9cn\xc3\xaf""c\xc3\xb6""d\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac"),
                "root listing");
    for (const auto &e : root) TEST_ASSERT(e.size() < 3 || e[2] != '$', "system file listed");
    TEST_ASSERT(!has(root, "F frag_b.bin"), "deleted file listed");

    const auto big = list(v + "/Big Folder");
    TEST_ASSERT(big.size() == 300, "300 files in an index with subnodes");
    TEST_ASSERT(list(v + "/Empty").empty(), "empty folder");

    TEST_ASSERT(read_all(v + "/tiny.txt") == "hello\n", "resident file");
    {
        unsigned char *buf = nullptr;
        size_t len = 0;
        TEST_ASSERT(evo_ntfs_read_file((v + "/TINY.TXT").c_str(), &buf, &len, 1024) == 0 && len == 6 &&
                        std::memcmp(buf, "hello\n", 6) == 0,
                    "read_file (subtitles, images)");
        std::free(buf);
        TEST_ASSERT(evo_ntfs_read_file((v + "/frag_c.bin").c_str(), &buf, &len, 1024) != 0, "read_file size cap");
    }
    TEST_ASSERT(read_all(v + "/a/b/c/d/deep.txt") == "deep\n", "deep path");
    TEST_ASSERT(read_all(v + "/\xc3\x9cn\xc3\xaf""c\xc3\xb6""d\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac/\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab.txt") == "uni\n",
                "unicode path");
    const std::string expected_sample =
        !file_bytes("output/ntfs/sample.mkv").empty()
            ? file_bytes("output/ntfs/sample.mkv")
            : file_bytes("output/sample/evo_sample.mkv");
    TEST_ASSERT(read_all(v + "/MOVIES/some movie (2019)/MOVIE.MKV") == expected_sample,
                "movie bytes (case-insensitive path)");

    const std::string frag = read_all(v + "/frag_c.bin");
    TEST_ASSERT(frag.size() == 9u * 1024 * 1024 + 123, "fragmented file size");
    bool same = true;
    for (size_t i = 0; i < frag.size() && same; ++i) same = (unsigned char)frag[i] == i % 251;
    TEST_ASSERT(same, "fragmented file bytes");

    /* Random reads match the sequential read. */
    evo_ntfs_file_t *f = evo_ntfs_open((v + "/frag_c.bin").c_str());
    TEST_ASSERT(f != nullptr, "reopen");
    unsigned seed = 7;
    for (int i = 0; i < 200; ++i) {
        seed = seed * 1103515245u + 12345u;
        const uint64_t off = seed % frag.size();
        char buf[5000];
        const long r = evo_ntfs_read(f, buf, sizeof buf, off);
        const size_t want = std::min<size_t>(sizeof buf, frag.size() - (size_t)off);
        if (r != (long)want || std::memcmp(buf, frag.data() + off, want) != 0) { evo_ntfs_close(f); TEST_FAIL("random read"); return; }
    }
    TEST_ASSERT(evo_ntfs_read(f, frag.data() ? (void *)frag.data() : nullptr, 10, frag.size()) == 0, "read at the end");
    evo_ntfs_close(f);

    int is_dir = -1;
    uint64_t size = 0;
    TEST_ASSERT(evo_ntfs_stat((v + "/frag_c.bin").c_str(), &is_dir, &size) == 0 && !is_dir && size == frag.size(), "stat file");
    TEST_ASSERT(evo_ntfs_stat((v + "/Movies").c_str(), &is_dir, &size) == 0 && is_dir, "stat dir");
    TEST_ASSERT(evo_ntfs_stat((v + "/nope.txt").c_str(), nullptr, nullptr) != 0, "missing path");
    TEST_ASSERT(evo_ntfs_open((v + "/Movies").c_str()) == nullptr, "a folder does not open as a file");
    TEST_PASS();
}

/* The browser lists through evo_opendir/evo_readdir: ntfs:// goes there too. */
static void test_ntfs_readdir_dispatch(void)
{
    TEST_START("NTFS: evo_opendir/evo_readdir list ntfs:// paths");
    if (!std::fopen("output/ntfs/whole.img", "rb")) {
        g_tests_passed++;
        printf(" [ SKIP ] (no images)\n");
        return;
    }
    evo_dir_t *d = evo_opendir("ntfs://whole/Movies");
    TEST_ASSERT(d != nullptr, "opendir");
    struct dirent *de = evo_readdir(d);
    TEST_ASSERT(de != nullptr && std::strcmp(de->d_name, "Some Movie (2019)") == 0 && de->d_type == DT_DIR, "folder entry");
    TEST_ASSERT(evo_readdir(d) == nullptr, "one entry");
    evo_closedir(d);
    d = evo_opendir("ntfs://whole/Movies/Some Movie (2019)");
    TEST_ASSERT(d != nullptr, "opendir 2");
    de = evo_readdir(d);
    TEST_ASSERT(de != nullptr && std::strcmp(de->d_name, "movie.mkv") == 0 && de->d_type == DT_REG, "file entry");
    evo_closedir(d);
    TEST_ASSERT(evo_opendir("ntfs://whole/no/such/dir") == nullptr, "missing folder");
    TEST_PASS();
}

static void test_ntfs_not_ntfs(void)
{
    TEST_START("NTFS: a non-NTFS image is not mounted");
    FILE *f = std::fopen("/tmp/evo_not_ntfs.img", "wb");
    TEST_ASSERT(f != nullptr, "temp file");
    std::vector<char> zeros(1 << 20, 0);
    std::fwrite(zeros.data(), 1, zeros.size(), f);
    std::fclose(f);
    TEST_ASSERT(evo_ntfs_add_device("/tmp/evo_not_ntfs.img", "zero") == 0, "zeros mounted");
    TEST_ASSERT(evo_ntfs_add_device("/tmp/does-not-exist.img", "none") == 0, "missing file mounted");
    TEST_ASSERT(evo_ntfs_opendir("ntfs://nosuch") == nullptr, "unknown volume");
    TEST_ASSERT(!evo_ntfs_is_url("/mnt/usb0/a") && evo_ntfs_is_url("ntfs://x/y"), "scheme test");
    std::remove("/tmp/evo_not_ntfs.img");
    TEST_PASS();
}

extern "C" void run_ntfs_tests(void)
{
    check_volume("whole.img", "whole", "ntfs://whole");
    check_volume("mbr.img", "mbr", "ntfs://mbrp1");
    check_volume("gpt.img", "gpt", "ntfs://gptp1");
    test_ntfs_readdir_dispatch();
    test_ntfs_not_ntfs();
}

__attribute__((constructor)) static void auto_run_ntfs_tests(void)
{
    run_ntfs_tests();
}
