/*
 * tests/test_dtshd.cpp - DTS-HD passthrough bursts are byte for byte what FFmpeg's spdif
 * muxer makes with -dtshd_rate 192000 (whole frames as IEC 61937-9 Type IV), and a
 * DTS-HD stream on the core path still gives FFmpeg's plain DTS core bursts. Also the core
 * fallbacks and their log line, whole packets, and the choice of DTS-HD or the core.
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

// Feeds the stream `chunk` bytes at a time with no packet boundaries: append to a buffer,
// take bursts while next_burst succeeds, drop what it consumed. A frame then waits for the
// next sync word; at the end it flushes, so the last frame goes out too.
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

// Packs the whole stream burst by burst and counts the notices the packer gives for the log.
static int count_notices(pt::Codec codec, const Bytes &es)
{
    pt::Packer packer(codec);
    std::vector<std::uint8_t> out;
    const char *error = nullptr;
    int notices = 0;
    size_t cursor = 0;
    while (packer.next_burst(es, &cursor, &out, &error, true))
        notices += packer.take_notice() != nullptr;
    return notices;
}

static void test_core_fallback_logs_once(void)
{
    TEST_START("DTS-HD: a core fallback gives one log line");
    const Bytes big = read_file(data_path("dtshd-big.es"));
    const Bytes rate44k = read_file(data_path("dtshd-44k.es"));
    const Bytes es = read_file(data_path("dtshd.es"));
    TEST_ASSERT(!big.empty() && !rate44k.empty() && !es.empty(), "test vector missing");
    // FFmpeg warns once for dtshd-big ("DTS-HD bitrate too high, temporarily sending core only").
    TEST_ASSERT(count_notices(pt::Codec::dtshd, big) == 1, "frame too big: not one notice");
    TEST_ASSERT(count_notices(pt::Codec::dtshd, rate44k) == 1, "no Type IV period: not one notice");
    TEST_ASSERT(count_notices(pt::Codec::dtshd, es) == 0, "a notice without a fallback");
    TEST_PASS();
}

// The stream as FFmpeg's demuxer hands it over: one packet per whole frame, the core plus
// its extension substreams. A frame starts at each core sync word (the filler holds none).
static std::vector<Bytes> packets_of(const Bytes &es)
{
    std::vector<size_t> starts;
    for (size_t i = 0; i + 4 <= es.size(); ++i)
        if (es[i] == 0x7F && es[i + 1] == 0xFE && es[i + 2] == 0x80 && es[i + 3] == 0x01)
            starts.push_back(i);
    starts.push_back(es.size());
    std::vector<Bytes> packets;
    for (size_t k = 0; k + 1 < starts.size(); ++k)
        packets.emplace_back(es.begin() + starts[k], es.begin() + starts[k + 1]);
    return packets;
}

static void test_packets_send_every_frame(void)
{
    TEST_START("DTS-HD: packets send every frame, the last too");
    const Bytes es = read_file(data_path("dtshd.es"));
    const Bytes ref = read_reference("dtshd-t4.ref");
    TEST_ASSERT(!es.empty() && !ref.empty(), "test vector missing");
    const std::vector<Bytes> packets = packets_of(es);
    TEST_ASSERT(packets.size() == 6, "the stream did not split into its 6 frames");

    pt::Packer packer(pt::Codec::dtshd);
    Bytes got;
    const char *error = nullptr;
    for (const Bytes &packet : packets)
        TEST_ASSERT(packer.push_packet(packet, &got, &error), error);
    const std::string diff = compare(got, ref, 8192);
    if (!diff.empty())
    {
        printf("\n       %s\n      ", diff.c_str());
        TEST_FAIL("bursts differ from FFmpeg's");
        return;
    }
    TEST_PASS();
}

// FFmpeg's profile numbers (libavcodec/defs.h AV_PROFILE_DTS_*).
enum { kDts = 20, kDtsEs = 30, kDts9624 = 40, kHra = 50, kMa = 60, kMaX = 61, kMaXImax = 62, kExpress = 70 };

static void test_dts_route(void)
{
    TEST_START("DTS route: whole frames only for HD at 48k family");
    const pt::Codec hd = pt::Codec::dtshd, core = pt::Codec::dts;
    TEST_ASSERT(pt::dts_route(kMa, 48000, true) == hd, "MA 48 kHz, sink lists DTS-HD");
    TEST_ASSERT(pt::dts_route(kHra, 96000, true) == hd, "HRA 96 kHz");
    TEST_ASSERT(pt::dts_route(kMaX, 48000, true) == hd, "MA X 48 kHz");
    TEST_ASSERT(pt::dts_route(kMaXImax, 192000, true) == hd, "MA X IMAX 192 kHz");
    TEST_ASSERT(pt::dts_route(kMa, 48000, false) == core, "sink without DTS-HD");
    TEST_ASSERT(pt::dts_route(kMa, 44100, true) == core, "MA 44.1 kHz has no Type IV period");
    TEST_ASSERT(pt::dts_route(kMa, 88200, true) == core, "MA 88.2 kHz has no Type IV period");
    TEST_ASSERT(pt::dts_route(kMa, 0, true) == core, "unknown rate");
    TEST_ASSERT(pt::dts_route(kDts, 48000, true) == core, "plain DTS");
    TEST_ASSERT(pt::dts_route(kDtsEs, 48000, true) == core, "DTS-ES");
    TEST_ASSERT(pt::dts_route(kDts9624, 96000, true) == core, "DTS 96/24");
    TEST_ASSERT(pt::dts_route(kExpress, 48000, true) == core, "DTS Express has no core");
    TEST_ASSERT(pt::dts_route(-99, 48000, true) == core, "unknown profile");
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
    // A 44.1 kHz core has no Type IV period at 192 kHz (FFmpeg stops there): the stream
    // goes on as core bursts instead of stalling.
    expect_ffmpeg_bursts("DTS-HD: a core with no Type IV period sends cores",
                         pt::Codec::dtshd, "dtshd-44k", "dtshd-44k-core", 2048);
    test_core_fallback_logs_once();
    test_dts_route();
    test_packets_send_every_frame();
}

__attribute__((constructor)) static void auto_run_dtshd_tests(void)
{
    run_dtshd_tests();
}
