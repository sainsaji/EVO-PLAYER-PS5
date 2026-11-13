/*
 * tests/test_dtshd.cpp - DTS-HD passthrough bursts are byte for byte what FFmpeg's spdif
 * muxer makes with -dtshd_rate 192000 (whole frames as IEC 61937-9 Type IV), and a
 * DTS-HD stream on the core path still gives FFmpeg's plain DTS core bursts.
 * The vectors in tests/data/dtshd/ come from make_vectors.py there.
 */

#include "evo/audio/iec61937.hpp"

#include <algorithm>
#include <cstdio>
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

typedef std::vector<std::uint8_t> Bytes;

static std::string data_path(const std::string &name)
{
    std::string dir = __FILE__;
    dir.erase(dir.find_last_of('/') + 1);
    return dir + "data/dtshd/" + name;
}

static Bytes read_file(const std::string &path)
{
    Bytes d;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return d;
    std::uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    return d;
}

static std::uint32_t le32(const Bytes &d, size_t at)
{
    return d[at] | (d[at + 1] << 8) | (d[at + 2] << 16) | (static_cast<std::uint32_t>(d[at + 3]) << 24);
}

// A .ref file: u32 total length, then records of u32 offset, u16 length, bytes; the rest is zero.
static Bytes read_reference(const std::string &name)
{
    const Bytes s = read_file(data_path(name));
    if (s.size() < 4) return Bytes();
    Bytes out(le32(s, 0), 0);
    size_t at = 4;
    while (at + 6 <= s.size())
    {
        const size_t off = le32(s, at);
        const size_t len = s[at + 4] | (s[at + 5] << 8);
        at += 6;
        if (at + len > s.size() || off + len > out.size()) return Bytes();
        std::memcpy(out.data() + off, s.data() + at, len);
        at += len;
    }
    return out;
}

// Feeds the stream `chunk` bytes at a time the way evo_pt_push_packet does: append to a
// buffer, take bursts while next_burst succeeds, drop what it consumed. At the end it
// flushes, so the last frame (held back until the next sync shows it is whole) goes out too.
static Bytes pack(pt::Packer &packer, const Bytes &es, size_t chunk, const char **error)
{
    Bytes buf, out;
    *error = nullptr;
    for (size_t fed = 0; fed < es.size(); fed += chunk)
    {
        const size_t n = std::min(chunk, es.size() - fed);
        buf.insert(buf.end(), es.begin() + fed, es.begin() + fed + n);
        const bool last = fed + n == es.size();
        size_t cursor = 0;
        while (buf.size() - cursor >= 8)
        {
            pt::Span<std::uint8_t> span(buf.data() + cursor, buf.size() - cursor);
            size_t consumed = 0;
            if (!packer.next_burst(span, &consumed, &out, error, last))
                break;
            cursor += consumed;
        }
        if (*error) return out;
        buf.erase(buf.begin(), buf.begin() + cursor);
    }
    return out;
}

// "" when equal, else where the first difference is.
static std::string compare(const Bytes &got, const Bytes &want, size_t burst_bytes)
{
    char msg[160];
    const size_t n = std::min(got.size(), want.size());
    for (size_t i = 0; i < n; ++i)
    {
        if (got[i] != want[i])
        {
            std::snprintf(msg, sizeof msg, "byte %zu (burst %zu offset %zu): got %02x, FFmpeg %02x",
                          i, i / burst_bytes, i % burst_bytes, got[i], want[i]);
            return msg;
        }
    }
    if (got.size() != want.size())
    {
        std::snprintf(msg, sizeof msg, "length %zu, FFmpeg %zu", got.size(), want.size());
        return msg;
    }
    return "";
}

// Packs <stream>.es with `codec`, whole and in small odd pieces, and compares with <ref>.ref.
static void expect_ffmpeg_bursts(const char *title, pt::Codec codec, const char *stream,
                                 const char *ref_name, size_t burst_bytes)
{
    TEST_START(title);
    const Bytes es = read_file(data_path(std::string(stream) + ".es"));
    const Bytes ref = read_reference(std::string(ref_name) + ".ref");
    TEST_ASSERT(!es.empty() && !ref.empty(), "test vector missing");

    const size_t chunks[] = {es.size(), 61};
    for (size_t chunk : chunks)
    {
        pt::Packer packer(codec);
        const char *error = nullptr;
        const Bytes got = pack(packer, es, chunk, &error);
        TEST_ASSERT(error == nullptr, error);
        const std::string diff = compare(got, ref, burst_bytes);
        if (!diff.empty())
        {
            printf("\n       %s, fed %zu bytes at a time\n      ", diff.c_str(), chunk);
            TEST_FAIL("bursts differ from FFmpeg's");
            return;
        }
    }
    TEST_PASS();
}

static void test_core_fallback_ends_with_the_stream(void)
{
    TEST_START("DTS-HD: a new stream sends whole frames again");
    const Bytes big = read_file(data_path("dtshd-big.es"));
    const Bytes es = read_file(data_path("dtshd.es"));
    const Bytes ref = read_reference("dtshd-t4.ref");
    TEST_ASSERT(!big.empty() && !es.empty() && !ref.empty(), "test vector missing");

    // evo_pt_open sets the codec on its one packer for every new playback.
    pt::Packer packer(pt::Codec::dtshd);
    const char *error = nullptr;
    pack(packer, big, big.size(), &error);
    packer.set_codec(pt::Codec::dtshd);
    const Bytes got = pack(packer, es, es.size(), &error);
    TEST_ASSERT(error == nullptr, error);
    TEST_ASSERT(compare(got, ref, 8192).empty(), "the core-only fallback carried over");
    TEST_PASS();
}

extern "C" void run_dtshd_tests(void)
{
    expect_ffmpeg_bursts("DTS-HD: whole frames as Type IV match FFmpeg", pt::Codec::dtshd,
                         "dtshd", "dtshd-t4", 8192);
    // Frame 1 does not fit the burst: its core goes out, then only cores for 60 s.
    expect_ffmpeg_bursts("DTS-HD: a frame too big falls back to the core", pt::Codec::dtshd,
                         "dtshd-big", "dtshd-big-t4", 8192);
    test_core_fallback_ends_with_the_stream();
    // A sink without DTS-HD keeps today's path: the core of each frame, Type I.
    expect_ffmpeg_bursts("DTS-HD: on the DTS path only cores go out", pt::Codec::dts,
                         "dtshd", "dtshd-core", 2048);
}

__attribute__((constructor)) static void auto_run_dtshd_tests(void)
{
    run_dtshd_tests();
}
