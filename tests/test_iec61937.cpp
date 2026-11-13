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
                          i, i % pt::kMatBurstBytes, got[i], want[i]);
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

// The samples per access unit a TrueHD stream's first unit (a major sync) gives.
static void expect_truehd_unit_samples(const char *title, const char *name, int want)
{
    TEST_START(title);
    const Bytes es = read_file(data_path(name));
    TEST_ASSERT(!es.empty(), "test vector missing");
    const pt::Frame f = pt::parse_frame(pt::Codec::truehd, es, 0);
    TEST_ASSERT(f.size != 0, "first access unit not found");
    TEST_ASSERT(f.major_sync, "first access unit is not a major sync");
    TEST_ASSERT(f.samples == want, "wrong samples per access unit");
    TEST_PASS();
}

// A 12-byte access unit: length word (6 words), input timing, then a major sync
// of the given format and the two bytes that may hold the rate bits.
static Bytes truehd_major_sync_unit(std::uint8_t format, std::uint8_t byte8, std::uint8_t byte9)
{
    return Bytes{0x00, 0x06, 0x12, 0x34, 0xF8, 0x72, 0x6F, format, byte8, byte9, 0x00, 0x00};
}

static void test_truehd_mlp_major_sync_rate(void)
{
    TEST_START("IEC 61937: MLP major sync (0xBB) rate comes from byte 9");
    // FFmpeg's spdifenc reads the rate bits from byte 9 for 0xBB (byte 8 for 0xBA).
    const Bytes mlp = truehd_major_sync_unit(0xBB, 0x20, 0x10);
    const pt::Frame f = pt::parse_frame(pt::Codec::truehd, mlp, 0);
    TEST_ASSERT(f.size == 12 && f.major_sync && !f.invalid, "MLP major sync not recognized");
    TEST_ASSERT(f.samples == 80, "rate bits not taken from byte 9");
    TEST_PASS();
}

static void test_truehd_units_ffmpeg_rejects(void)
{
    TEST_START("IEC 61937: TrueHD units FFmpeg rejects are marked");
    const Bytes unknown = truehd_major_sync_unit(0xBC, 0x00, 0x00);
    const pt::Frame u = pt::parse_frame(pt::Codec::truehd, unknown, 0);
    TEST_ASSERT(u.size == 12 && u.invalid, "major sync of unknown format not marked invalid");
    const Bytes tiny{0x00, 0x04, 0x12, 0x34, 0xF8, 0x72, 0x6F, 0xBA};
    const pt::Frame t = pt::parse_frame(pt::Codec::truehd, tiny, 0);
    TEST_ASSERT(t.size == 8 && t.invalid, "unit under 10 bytes not marked invalid");
    const Bytes plain{0x00, 0x06, 0x12, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const pt::Frame p = pt::parse_frame(pt::Codec::truehd, plain, 0);
    TEST_ASSERT(p.size == 12 && !p.invalid && !p.major_sync && p.samples == 0,
                "unit without a major sync should carry no rate");
    TEST_PASS();
}

extern "C" void run_iec61937_tests(void)
{
    expect_truehd_unit_samples("IEC 61937: TrueHD 48 kHz unit is 40 samples", "truehd48.es", 40);
    expect_truehd_unit_samples("IEC 61937: TrueHD 96 kHz unit is 80 samples", "truehd96.es", 80);
    test_truehd_mlp_major_sync_rate();
    test_truehd_units_ffmpeg_rejects();
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
