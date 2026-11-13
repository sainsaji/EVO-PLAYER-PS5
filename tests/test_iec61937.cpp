/*
 * tests/test_iec61937.cpp - passthrough bursts are byte for byte what FFmpeg's spdif
 * muxer makes from the same stream (TrueHD as MAT, AC-3, E-AC-3, DTS core).
 * The vectors in tests/data/iec61937/ come from make_vectors.py there.
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

static std::string data_path(const char *name)
{
    std::string dir = __FILE__;
    dir.erase(dir.find_last_of('/') + 1);
    return dir + "data/iec61937/" + name;
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
static Bytes read_reference(const char *name)
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
// buffer, take bursts while next_burst succeeds, drop what it consumed.
static Bytes pack(pt::Packer &packer, const Bytes &es, size_t chunk, const char **error)
{
    Bytes buf, out;
    *error = nullptr;
    for (size_t fed = 0; fed < es.size(); fed += chunk)
    {
        const size_t n = std::min(chunk, es.size() - fed);
        buf.insert(buf.end(), es.begin() + fed, es.begin() + fed + n);
        size_t cursor = 0;
        while (buf.size() - cursor >= 8)
        {
            pt::Span<std::uint8_t> span(buf.data() + cursor, buf.size() - cursor);
            size_t consumed = 0;
            if (!packer.next_burst(span, &consumed, &out, error, false))
                break;
            cursor += consumed;
        }
        if (*error) return out;
        buf.erase(buf.begin(), buf.begin() + cursor);
    }
    return out;
}

// "" when equal, else where the first difference is.
static std::string compare(const Bytes &got, const Bytes &want)
{
    char msg[160];
    const size_t n = std::min(got.size(), want.size());
    for (size_t i = 0; i < n; ++i)
    {
        if (got[i] != want[i])
        {
            std::snprintf(msg, sizeof msg, "byte %zu (burst offset %zu): got %02x, FFmpeg %02x",
                          i, i % 61440, got[i], want[i]);
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

static void expect_ffmpeg_bursts(const char *title, pt::Codec codec, const char *name)
{
    TEST_START(title);
    const Bytes es = read_file(data_path((std::string(name) + ".es").c_str()));
    const Bytes ref = read_reference((std::string(name) + ".ref").c_str());
    TEST_ASSERT(!es.empty() && !ref.empty(), "test vector missing");

    // Whole stream at once, and in small odd pieces as packets arrive.
    const size_t chunks[] = {es.size(), 61};
    for (size_t chunk : chunks)
    {
        pt::Packer packer(codec);
        const char *error = nullptr;
        const Bytes got = pack(packer, es, chunk, &error);
        TEST_ASSERT(error == nullptr, error);
        const std::string diff = compare(got, ref);
        if (!diff.empty())
        {
            printf("\n       %s, fed %zu bytes at a time\n      ", diff.c_str(), chunk);
            TEST_FAIL("bursts differ from FFmpeg's");
            return;
        }
    }
    TEST_PASS();
}

static void test_truehd_restarts_after_reset(void)
{
    TEST_START("IEC 61937: TrueHD starts a new MAT frame after reset");
    const Bytes es = read_file(data_path("truehd48.es"));
    const Bytes ref = read_reference("truehd48.ref");
    TEST_ASSERT(!es.empty() && !ref.empty(), "test vector missing");

    // Half a MAT frame, then a seek: what follows must not carry the old frame's state.
    pt::Packer packer(pt::Codec::truehd);
    const char *error = nullptr;
    pack(packer, Bytes(es.begin(), es.begin() + es.size() / 4), es.size(), &error);
    packer.reset();
    const Bytes got = pack(packer, es, es.size(), &error);
    TEST_ASSERT(error == nullptr, error);
    TEST_ASSERT(compare(got, ref).empty(), "bursts after reset differ from FFmpeg's");
    TEST_PASS();
}

extern "C" void run_iec61937_tests(void)
{
    expect_ffmpeg_bursts("IEC 61937: TrueHD 48 kHz MAT matches FFmpeg", pt::Codec::truehd, "truehd48");
    expect_ffmpeg_bursts("IEC 61937: TrueHD 96 kHz MAT matches FFmpeg", pt::Codec::truehd, "truehd96");
    test_truehd_restarts_after_reset();
    expect_ffmpeg_bursts("IEC 61937: AC-3 bursts match FFmpeg", pt::Codec::ac3, "ac3");
    expect_ffmpeg_bursts("IEC 61937: E-AC-3 bursts match FFmpeg", pt::Codec::eac3, "eac3");
    expect_ffmpeg_bursts("IEC 61937: DTS core bursts match FFmpeg", pt::Codec::dts, "dts");
}

__attribute__((constructor)) static void auto_run_iec61937_tests(void)
{
    run_iec61937_tests();
}
