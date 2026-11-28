/*
 * tests/test_dtshd.cpp - DTS-HD passthrough bursts are byte for byte what FFmpeg's spdif
 * muxer makes with -dtshd_rate 192000 (whole frames as IEC 61937-9 Type IV), and a
 * DTS-HD stream on the core path still gives FFmpeg's plain DTS core bursts. Also the core
 * fallbacks and their log line, whole packets, and the choice of DTS-HD or the core.
 * The vectors in tests/data/dtshd/ come from make_vectors.py there.
 */

#include "evo/audio/iec61937.hpp"

#include <libavcodec/defs.h>

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
// Fails with `reason` and the first difference when `diff` (from compare) is not "".
#define TEST_ASSERT_SAME(diff, reason) \
    do { const std::string d_ = (diff); if (!d_.empty()) { printf("\n       %s\n      ", d_.c_str()); TEST_FAIL(reason); return; } } while (0)

typedef std::vector<std::uint8_t> Bytes;

// Burst lengths in bytes on the port: a 512-sample frame as Type IV at 192 kHz, and its
// core as Type I at 48 kHz.
static const size_t kTypeIvBurstBytes = 8192;
static const size_t kCoreBurstBytes = 2048;
// Fewer bytes than this hold no frame header, so the feed loops stop there.
static const size_t kFrameHeaderBytes = 8;

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
        while (buf.size() - cursor >= kFrameHeaderBytes)
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
        TEST_ASSERT_SAME(diff, chunk == es.size() ? "bursts differ from FFmpeg's, fed whole"
                                                  : "bursts differ from FFmpeg's, fed in pieces");
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
    TEST_ASSERT(compare(got, ref, kTypeIvBurstBytes).empty(), "the core-only fallback carried over");
    TEST_PASS();
}

// mpv ends the fallback at a seek: ad_spdif_reset frees its spdif muxer, so the next
// packet starts a new one with dtshd_skip at 0.
static void test_core_fallback_ends_at_a_seek(void)
{
    TEST_START("DTS-HD: after a seek whole frames go out again");
    const Bytes big = read_file(data_path("dtshd-big.es"));
    const Bytes big_ref = read_reference("dtshd-big-t4.ref");
    const Bytes es = read_file(data_path("dtshd.es"));
    const Bytes ref = read_reference("dtshd-t4.ref");
    TEST_ASSERT(!big.empty() && !big_ref.empty() && !es.empty() && !ref.empty(), "test vector missing");

    // Checks Packer::reset(), which evo_pt_reset calls on every seek. That wiring is
    // checked by the 11-state-survives repro, since evo_pt.cpp has no host test.
    pt::Packer packer(pt::Codec::dtshd);
    const char *error = nullptr;
    const Bytes before = pack(packer, big, big.size(), &error);
    TEST_ASSERT(error == nullptr, error);
    TEST_ASSERT(compare(before, big_ref, kTypeIvBurstBytes).empty(),
                "the core-only fallback did not start");
    packer.reset();
    const Bytes got = pack(packer, es, es.size(), &error);
    TEST_ASSERT(error == nullptr, error);
    TEST_ASSERT_SAME(compare(got, ref, kTypeIvBurstBytes), "the core-only fallback outlived the seek");
    TEST_PASS();
}

static Bytes concat(const Bytes &a, const Bytes &b)
{
    Bytes out = a;
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

static void test_frames_with_no_period_are_dropped(void)
{
    TEST_START("DTS-HD: frames with no Type IV period are dropped");
    // The route picks DTS-HD only for a stream rate with a Type IV period, so a core whose
    // own rate has none can only turn up mid-stream. Its frames are dropped: Type I core
    // bursts would go out at the wrong rate on the 192 kHz port, and FFmpeg stops there.
    const Bytes es = read_file(data_path("dtshd.es"));
    const Bytes rate44k = read_file(data_path("dtshd-44k.es"));
    const Bytes ref = read_reference("dtshd-t4.ref");
    TEST_ASSERT(!es.empty() && !rate44k.empty() && !ref.empty(), "test vector missing");
    const Bytes stream = concat(concat(es, rate44k), es);
    const Bytes want = concat(ref, ref);

    const size_t chunks[] = {stream.size(), 61};
    for (size_t chunk : chunks)
    {
        pt::Packer packer(pt::Codec::dtshd);
        const char *error = nullptr;
        const Bytes got = pack(packer, stream, chunk, &error);
        TEST_ASSERT(error == nullptr, error);
        TEST_ASSERT(compare(got, want, kTypeIvBurstBytes).empty(),
                    "not the frames around the dropped ones as FFmpeg's Type IV");
    }
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
        notices += !packer.take_notice().empty();
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
    for (size_t i = 0; i < es.size(); ++i)
        if (pt::parse_frame(pt::Codec::dts, es, i).size != 0)
            starts.push_back(i);
    starts.push_back(es.size());
    std::vector<Bytes> packets;
    for (size_t k = 0; k + 1 < starts.size(); ++k)
        packets.emplace_back(es.begin() + starts[k], es.begin() + starts[k + 1]);
    return packets;
}

// What push_packets gives: the bursts, each packet's notice as one line of `log`, the
// most bytes left in the buffer after a packet, and an error if take_bursts dropped a
// frame next_burst could not send.
struct Pushed
{
    Bytes out;
    std::string log;
    size_t most_held = 0;
    const char *error = nullptr;
};

// Feeds the packets the way evo_pt_push_packet does: append each to the stream buffer and
// call take_bursts, which flushes at the packet's end where the codec's packets end a frame
// (packet_ends_frame). take_bursts steps over a frame next_burst gives an error for and
// logs it, so that line is the error here.
static Pushed push_packets(pt::Packer &packer, const std::vector<Bytes> &packets)
{
    Pushed r;
    Bytes buf;
    for (const Bytes &packet : packets)
    {
        buf.insert(buf.end(), packet.begin(), packet.end());
        packer.take_bursts(&buf, &r.out);
        r.most_held = std::max(r.most_held, buf.size());
        const std::string notice = packer.take_notice();
        if (!notice.empty()) r.log += notice + "\n";
        if (notice.find("cannot send") != std::string::npos)
        {
            r.error = "take_bursts dropped a frame it cannot send";
            return r;
        }
    }
    return r;
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
    const Pushed got = push_packets(packer, packets);
    TEST_ASSERT(got.error == nullptr, got.error);
    TEST_ASSERT_SAME(compare(got.out, ref, kTypeIvBurstBytes), "bursts differ from FFmpeg's");
    TEST_PASS();
}

// evo_pt_push_packet hands each whole packet to take_bursts. A DTS-HD packet ends its
// frame, so the frame must go out at that packet's end, not wait for the next frame's
// sync word: after packet k, k bursts are out and nothing is held. The last frame has no
// next frame and would never go out.
static void test_take_bursts_sends_each_frame_at_its_packet(void)
{
    TEST_START("DTS-HD: take_bursts sends each frame at its packet's end");
    const Bytes es = read_file(data_path("dtshd.es"));
    const Bytes ref = read_reference("dtshd-t4.ref");
    TEST_ASSERT(!es.empty() && !ref.empty(), "test vector missing");
    const std::vector<Bytes> packets = packets_of(es);
    TEST_ASSERT(packets.size() == 6 && ref.size() == 6 * kTypeIvBurstBytes, "not 6 frames");

    pt::Packer packer(pt::Codec::dtshd);
    Bytes stream, out;
    for (size_t k = 0; k < packets.size(); ++k)
    {
        stream.insert(stream.end(), packets[k].begin(), packets[k].end());
        packer.take_bursts(&stream, &out);
        TEST_ASSERT(out.size() == (k + 1) * kTypeIvBurstBytes, "a frame waited past its packet");
        TEST_ASSERT(stream.empty(), "bytes held after a whole packet");
        TEST_ASSERT(packer.take_notice().empty(), "a notice for a clean stream");
    }
    TEST_ASSERT_SAME(compare(out, ref, kTypeIvBurstBytes), "bursts differ from FFmpeg's");
    TEST_PASS();
}

// A lone DTS core of 8200 bytes, more than the 8172 a 8192-byte burst holds: the header
// of `frame` with a new FSIZE, then zeros.
static Bytes oversized_core(const Bytes &frame)
{
    const size_t core_bytes = 8200;
    Bytes core(frame.begin(), frame.begin() + 16);
    const size_t fsize = core_bytes - 1;
    core[5] = static_cast<std::uint8_t>((core[5] & ~3) | (fsize >> 12));
    core[6] = static_cast<std::uint8_t>(fsize >> 4);
    core[7] = static_cast<std::uint8_t>(((fsize & 0xF) << 4) | (core[7] & 0xF));
    core.resize(core_bytes, 0);
    return core;
}

static void test_core_too_big_is_dropped(void)
{
    TEST_START("DTS-HD: a core too big for the burst is dropped");
    const Bytes big = read_file(data_path("dtshd-big.es"));
    const Bytes ref = read_reference("dtshd-big-t4.ref");
    TEST_ASSERT(!big.empty() && ref.size() == 3 * kTypeIvBurstBytes, "test vector missing");
    std::vector<Bytes> frames = packets_of(big);
    TEST_ASSERT(frames.size() == 3, "dtshd-big did not split into its 3 frames");

    // Frame 1 (the one too big for the burst) becomes a lone core too big for it as well.
    const Bytes core = oversized_core(frames[1]);
    TEST_ASSERT(pt::parse_frame(pt::Codec::dtshd, core, 0).size == core.size(), "core header");
    frames[1] = core;

    // It is dropped instead of stalling the stream. As in FFmpeg's output it starts the
    // 60 s core-only fallback, so frame 2 still goes out as its core.
    Bytes want(ref.begin(), ref.begin() + kTypeIvBurstBytes);
    want.insert(want.end(), ref.begin() + 2 * kTypeIvBurstBytes, ref.end());
    pt::Packer packer(pt::Codec::dtshd);
    const Pushed got = push_packets(packer, frames);
    TEST_ASSERT(got.error == nullptr, got.error);
    TEST_ASSERT_SAME(compare(got.out, want, kTypeIvBurstBytes), "not frames 0 and 2 as FFmpeg's");
    TEST_PASS();
}

static void test_each_drop_reason_is_logged(void)
{
    TEST_START("DTS-HD: each reason for dropping frames is logged");
    const Bytes rate44k = read_file(data_path("dtshd-44k.es"));
    const Bytes big = read_file(data_path("dtshd-big.es"));
    TEST_ASSERT(!rate44k.empty() && !big.empty(), "test vector missing");
    const std::vector<Bytes> frames = packets_of(big);
    TEST_ASSERT(frames.size() == 3, "dtshd-big did not split into its 3 frames");
    // Frames with no Type IV period, then a core too big for the burst, in one stream.
    const Bytes stream = concat(concat(rate44k, rate44k), oversized_core(frames[1]));

    pt::Packer packer(pt::Codec::dtshd);
    std::vector<std::uint8_t> out;
    const char *error = nullptr;
    std::string log;
    size_t cursor = 0;
    while (packer.next_burst(stream, &cursor, &out, &error, true))
        log += packer.take_notice() + "\n";
    TEST_ASSERT(error == nullptr, error);
    TEST_ASSERT(log.find("no Type IV period") != std::string::npos, "no-period drop not logged");
    TEST_ASSERT(log.find("no Type IV period") == log.rfind("no Type IV period"),
                "no-period drop logged more than once");
    TEST_ASSERT(log.find("core is too big") != std::string::npos,
                "core-too-big drop not logged after a no-period drop");
    TEST_PASS();
}

// Damage that leaves an extension substream's FSIZE at its largest (64 KiB and 1 MiB here)
// must not hold the frame until that much has come: FFmpeg sends each packet whole, the
// bad size in it, and every frame after it stays whole-frame DTS-HD.
static void test_bad_extension_size_ends_at_the_packet(void)
{
    TEST_START("DTS-HD: a bad extension size ends at the packet");
    const Bytes es = read_file(data_path("dtshd-bad-fsize.es"));
    const Bytes ref = read_reference("dtshd-bad-fsize-t4.ref");
    TEST_ASSERT(!es.empty() && !ref.empty(), "test vector missing");
    const std::vector<Bytes> packets = packets_of(es);
    TEST_ASSERT(packets.size() == 6, "the stream did not split into its 6 frames");

    pt::Packer packer(pt::Codec::dtshd);
    const Pushed got = push_packets(packer, packets);
    TEST_ASSERT(got.error == nullptr, got.error);
    TEST_ASSERT(got.most_held == 0, "a packet's frame was held for the next packet");
    TEST_ASSERT_SAME(compare(got.out, ref, kTypeIvBurstBytes), "bursts differ from FFmpeg's");
    TEST_ASSERT(got.log.find("match") != std::string::npos, "the bad size not logged");
    TEST_ASSERT(got.log.find('\n') + 1 == got.log.size(), "not one log line for two bad sizes");
    TEST_PASS();
}

// Packs `name`.es packet by packet and compares with `want`; "" or what went wrong.
static std::string push_and_compare(const char *name, const Bytes &want, std::string *log)
{
    const Bytes es = read_file(data_path(std::string(name) + ".es"));
    if (es.empty() || want.empty()) return "test vector missing";
    pt::Packer packer(pt::Codec::dtshd);
    const Pushed got = push_packets(packer, packets_of(es));
    *log = got.log;
    if (got.error) return got.error;
    return compare(got.out, want, kTypeIvBurstBytes);
}

// `count` bytes of `bytes` from `from`, or nothing if they are not all there.
static Bytes slice(const Bytes &bytes, size_t from, size_t count)
{
    if (from + count > bytes.size()) return Bytes();
    return Bytes(bytes.begin() + from, bytes.begin() + from + count);
}

// A core size that is wrong but valid (64 bytes off, inside the packet) must not cut the
// frame short: the packet holds the whole frame, and FFmpeg sends all of it.
static void test_wrong_core_size_sends_the_packet(void)
{
    TEST_START("DTS-HD: a wrong core size still sends the packet");
    std::string log;
    TEST_ASSERT_SAME(push_and_compare("dtshd-core-size", read_reference("dtshd-core-size-t4.ref"), &log),
                     "bursts differ from FFmpeg's");
    TEST_ASSERT(log.find("match") != std::string::npos, "the wrong size not logged");
    TEST_PASS();
}

// A frame cut short that the demuxer joined to the next one in one packet: the header sizes
// of the cut frame then end inside the next frame. The cut piece goes out as a short frame
// and the next frame stays whole (FFmpeg sends the packet as one frame, too big for the
// burst, and goes core-only for 60 s). Cut inside the core header, the core and the EXSS.
static void test_cut_frame_joined_to_the_next(void)
{
    TEST_START("DTS-HD: a cut frame joined to the next keeps it whole");
    const Bytes es = read_file(data_path("dtshd.es"));
    const Bytes ref = read_reference("dtshd-t4.ref");
    TEST_ASSERT(!es.empty() && ref.size() == 6 * kTypeIvBurstBytes, "test vector missing");
    const std::vector<Bytes> clean = packets_of(es);
    TEST_ASSERT(clean.size() == 6, "the stream did not split into its 6 frames");
    // Frame 0 is a 512-byte core and a 1500-byte EXSS; frame 1 a core and a 3000-byte EXSS,
    // so frame 0's sizes end inside frame 1 for each cut.
    TEST_ASSERT(clean[0].size() == 2012, "frame 0 is not 512 + 1500 bytes");
    for (size_t cut : {12, 300, 1000})
    {
        std::vector<Bytes> packets(clean.begin() + 1, clean.end());
        packets[0].insert(packets[0].begin(), clean[0].begin(), clean[0].begin() + cut);
        pt::Packer packer(pt::Codec::dtshd);
        const Pushed got = push_packets(packer, packets);
        TEST_ASSERT(got.error == nullptr, got.error);
        TEST_ASSERT(got.most_held == 0, "a packet's frame was held for the next packet");
        // Burst 0 is the cut piece; every other frame is FFmpeg's clean burst.
        TEST_ASSERT(got.out.size() == ref.size(), "not one burst per frame");
        TEST_ASSERT_SAME(compare(slice(got.out, kTypeIvBurstBytes, 5 * kTypeIvBurstBytes),
                                 slice(ref, kTypeIvBurstBytes, 5 * kTypeIvBurstBytes), kTypeIvBurstBytes),
                         cut == 12 ? "cut at 12: a later frame differs from FFmpeg's"
                         : cut == 300 ? "cut in the core: a later frame differs from FFmpeg's"
                                      : "cut in the EXSS: a later frame differs from FFmpeg's");
        TEST_ASSERT(got.log.find("60 s") == std::string::npos, "the cut started the 60 s fallback");
        TEST_ASSERT(got.log.find("match") != std::string::npos, "the cut not logged");
    }
    TEST_PASS();
}

// A core whose size is not a whole number of 4-byte words may be padded to one before its
// EXSS. FFmpeg's parser looks for the EXSS there (dca_parser.c, FFALIGN(frame_size, 4)),
// so such a stream is clean: no notice, and each packet goes out whole.
static void test_padded_core_is_clean(void)
{
    TEST_START("DTS-HD: a core padded to 4 bytes is not damage");
    std::string log;
    TEST_ASSERT_SAME(push_and_compare("dtshd-padded", read_reference("dtshd-padded-t4.ref"), &log),
                     "bursts differ from FFmpeg's");
    TEST_ASSERT(log.empty(), "a notice for a clean padded stream");
    TEST_PASS();
}

// Frame lengths may change mid-stream, and a last frame may be short: spec-valid headers go
// out as FFmpeg sends them, each at the subtype its length gives.
static void test_frame_length_changes_are_sent(void)
{
    TEST_START("DTS-HD: frame length changes go out as FFmpeg's");
    std::string log;
    TEST_ASSERT_SAME(push_and_compare("dtshd-shape", read_reference("dtshd-shape-t4.ref"), &log),
                     "bursts differ from FFmpeg's");
    TEST_ASSERT(log.empty(), "a notice for spec-valid frames");
    TEST_PASS();
}

// Any frame whose packet holds more than its burst carries starts FFmpeg's 60 s core-only
// fallback, wherever it is and whatever its length: damage can't make a packet bigger. The
// output is FFmpeg's, byte for byte.
static void test_too_big_frames_fall_back_as_ffmpeg(void)
{
    TEST_START("DTS-HD: too-big frames fall back as FFmpeg's do");
    const char *streams[] = {
        "dtshd-big-first",  // the first frame
        "dtshd-mid-short0", // the first frame, at 256 samples (a shorter burst)
        "dtshd-chg-from3",  // a change to 256 samples, too big, then frames that fit
        "dtshd-chg-from6",  // a change to 256 samples, two frames too big
    };
    for (const char *name : streams)
    {
        std::string log;
        TEST_ASSERT_SAME(push_and_compare(name, read_reference(std::string(name) + "-t4.ref"), &log), name);
        TEST_ASSERT(log.find("60 s") != std::string::npos, name);
    }
    TEST_PASS();
}

// The core header of `frame` with SFREQ code `sfreq`, NBLKS `nblks` and FSIZE for
// `core_bytes`, each left as it is when negative (ETSI TS 102 114 5.3.1).
static Bytes damage_core(Bytes frame, int sfreq, int nblks, long core_bytes)
{
    if (sfreq >= 0)
        frame[8] = static_cast<std::uint8_t>((frame[8] & ~0x3C) | (sfreq << 2));
    if (nblks >= 0)
    {
        frame[4] = static_cast<std::uint8_t>((frame[4] & ~1) | (nblks >> 6));
        frame[5] = static_cast<std::uint8_t>((frame[5] & 3) | ((nblks & 0x3F) << 2));
    }
    if (core_bytes >= 0)
    {
        const long fsize = core_bytes - 1;
        frame[5] = static_cast<std::uint8_t>((frame[5] & ~3) | (fsize >> 12));
        frame[6] = static_cast<std::uint8_t>(fsize >> 4);
        frame[7] = static_cast<std::uint8_t>(((fsize & 0xF) << 4) | (frame[7] & 0xF));
    }
    return frame;
}

// The frame bytes a Type IV burst carries: the 16-bit size after the start code, at burst
// bytes 18-19 (payload bytes 10 and 11 as one little-endian sample).
static size_t type_iv_frame_bytes(const Bytes &out, size_t burst)
{
    const size_t at = burst * kTypeIvBurstBytes + 18;
    return at + 2 <= out.size() ? (out[at + 1] << 8) | out[at] : 0;
}

// In the core-only fallback a damaged frame cut short at a false core sync inside its core
// sends only its bytes up to that sync, not the core size from its header: the bytes after
// it are the next frame's and go out (or are dropped) as that.
static void test_fallback_core_stops_at_the_frame_end(void)
{
    TEST_START("DTS-HD: a fallback core stops at the frame's end");
    const Bytes big = read_file(data_path("dtshd-big.es"));
    TEST_ASSERT(!big.empty(), "test vector missing");
    std::vector<Bytes> packets = packets_of(big);
    TEST_ASSERT(packets.size() == 3, "dtshd-big did not split into its 3 frames");
    // Frame 1 starts the fallback. Frame 2's core size is 64 bytes too big (its sizes then
    // end inside its EXSS), and a false sync with an invalid header sits 200 bytes in.
    const size_t false_sync = 200;
    const size_t core_bytes = pt::parse_frame(pt::Codec::dtshd, packets[2], 0).size;
    TEST_ASSERT(core_bytes == 512, "frame 2's core is not 512 bytes");
    Bytes &x = packets[2];
    x = damage_core(x, -1, -1, static_cast<long>(core_bytes + 64));
    const std::uint8_t sync[] = {0x7F, 0xFE, 0x80, 0x01};
    std::copy(sync, sync + 4, x.begin() + false_sync);
    std::fill(x.begin() + false_sync + 4, x.begin() + false_sync + 16, 0);

    pt::Packer packer(pt::Codec::dtshd);
    const Pushed got = push_packets(packer, packets);
    TEST_ASSERT(got.error == nullptr, got.error);
    TEST_ASSERT(got.out.size() == 3 * kTypeIvBurstBytes, "not one burst per frame");
    TEST_ASSERT(type_iv_frame_bytes(got.out, 2) == false_sync, "frame 2 sent bytes past its end");
    TEST_PASS();
}

// Core header fields that ETSI TS 102 114 5.3.1 calls invalid are damage, and FFmpeg's muxer
// would believe them (an SFREQ of 15 reads as 192 kHz there, so a frame "too big" starts its
// 60 s core-only fallback). Each such frame is dropped, logged once, and the frames around
// it are FFmpeg's clean bursts.
static void test_invalid_core_header_is_dropped(void)
{
    TEST_START("DTS-HD: an invalid core header drops its frame");
    const Bytes one = read_file(data_path("dtshd.es"));
    const Bytes one_ref = read_reference("dtshd-t4.ref");
    TEST_ASSERT(!one.empty() && one_ref.size() == 6 * kTypeIvBurstBytes, "test vector missing");
    const Bytes ref = concat(one_ref, one_ref);
    std::vector<Bytes> packets = packets_of(concat(one, one));
    TEST_ASSERT(packets.size() == 12, "the stream did not split into its 12 frames");
    packets[1] = damage_core(packets[1], 15, -1, -1);  // SFREQ invalid, twice in a row
    packets[2] = damage_core(packets[2], 15, -1, -1);
    packets[4] = damage_core(packets[4], -1, -1, 1);   // FSIZE under the 96 B minimum
    packets[6] = damage_core(packets[6], -1, 4, -1);   // NBLKS under 5
    packets[7] = damage_core(packets[7], -1, 9, -1);   // 320 samples: no normal frame's length
    TEST_ASSERT(packets[10].size() + packets[11].size() < 4000, "frame 10 core size");
    packets[10] = damage_core(packets[10], -1, -1, 4000); // a core past its packet

    pt::Packer packer(pt::Codec::dtshd);
    const Pushed got = push_packets(packer, packets);
    TEST_ASSERT(got.error == nullptr, got.error);
    Bytes want;
    for (size_t burst : {0, 3, 5, 8, 9, 11})
    {
        const Bytes b = slice(ref, burst * kTypeIvBurstBytes, kTypeIvBurstBytes);
        want.insert(want.end(), b.begin(), b.end());
    }
    TEST_ASSERT_SAME(compare(got.out, want, kTypeIvBurstBytes), "not FFmpeg's bursts 0, 3, 5, 8, 9 and 11");
    TEST_ASSERT(got.log.find("invalid core header") != std::string::npos, "the damage not logged");
    TEST_ASSERT(got.log.find('\n') + 1 == got.log.size(), "not one log line for six damaged frames");
    TEST_PASS();
}

static void test_only_dtshd_packets_end_frames(void)
{
    TEST_START("Packets end frames for DTS-HD only");
    // The other codecs keep waiting for the next sync word, as before DTS-HD: a flush
    // there would send a short burst.
    TEST_ASSERT(pt::packet_ends_frame(pt::Codec::dtshd), "DTS-HD");
    const pt::Codec others[] = {pt::Codec::ac3, pt::Codec::eac3, pt::Codec::dts, pt::Codec::aac,
                                pt::Codec::truehd, pt::Codec::unknown};
    for (pt::Codec codec : others)
        TEST_ASSERT(!pt::packet_ends_frame(codec), pt::codec_name(codec));
    TEST_PASS();
}


static void test_dts_route(void)
{
    TEST_START("DTS route: whole frames only for HD at 48k family");
    const pt::Codec hd = pt::Codec::dtshd, core = pt::Codec::dts;
    TEST_ASSERT(pt::dts_route(AV_PROFILE_DTS_HD_MA, 48000, true) == hd, "MA 48 kHz, sink lists DTS-HD");
    TEST_ASSERT(pt::dts_route(AV_PROFILE_DTS_HD_HRA, 96000, true) == hd, "HRA 96 kHz");
    TEST_ASSERT(pt::dts_route(AV_PROFILE_DTS_HD_MA_X, 48000, true) == hd, "MA X 48 kHz");
    TEST_ASSERT(pt::dts_route(AV_PROFILE_DTS_HD_MA_X_IMAX, 192000, true) == hd, "MA X IMAX 192 kHz");
    TEST_ASSERT(pt::dts_route(AV_PROFILE_DTS_HD_MA, 48000, false) == core, "sink without DTS-HD");
    TEST_ASSERT(pt::dts_route(AV_PROFILE_DTS_HD_MA, 44100, true) == core, "MA 44.1 kHz has no Type IV period");
    TEST_ASSERT(pt::dts_route(AV_PROFILE_DTS_HD_MA, 88200, true) == core, "MA 88.2 kHz has no Type IV period");
    TEST_ASSERT(pt::dts_route(AV_PROFILE_DTS_HD_MA, 0, true) == core, "unknown rate");
    TEST_ASSERT(pt::dts_route(AV_PROFILE_DTS, 48000, true) == core, "plain DTS");
    TEST_ASSERT(pt::dts_route(AV_PROFILE_DTS_ES, 48000, true) == core, "DTS-ES");
    TEST_ASSERT(pt::dts_route(AV_PROFILE_DTS_96_24, 96000, true) == core, "DTS 96/24");
    TEST_ASSERT(pt::dts_route(AV_PROFILE_DTS_EXPRESS, 48000, true) == core, "DTS Express has no core");
    TEST_ASSERT(pt::dts_route(-99, 48000, true) == core, "unknown profile");
    TEST_PASS();
}

extern "C" void run_dtshd_tests(void)
{
    expect_ffmpeg_bursts("DTS-HD: whole frames as Type IV match FFmpeg", pt::Codec::dtshd,
                         "dtshd", "dtshd-t4", kTypeIvBurstBytes);
    // Frame 1 does not fit the burst: its core goes out, then only cores for 60 s.
    expect_ffmpeg_bursts("DTS-HD: a frame too big falls back to the core", pt::Codec::dtshd,
                         "dtshd-big", "dtshd-big-t4", kTypeIvBurstBytes);
    test_core_fallback_ends_with_the_stream();
    test_core_fallback_ends_at_a_seek();
    // A sink without DTS-HD keeps today's path: the core of each frame, Type I.
    expect_ffmpeg_bursts("DTS-HD: on the DTS path only cores go out", pt::Codec::dts,
                         "dtshd", "dtshd-core", kCoreBurstBytes);
    // dts_route sends a 44.1 kHz stream on the DTS path: its core bursts.
    expect_ffmpeg_bursts("DTS-HD: 44.1 kHz on the DTS path sends cores",
                         pt::Codec::dts, "dtshd-44k", "dtshd-44k-core", kCoreBurstBytes);
    // Cores padded to 4 bytes before their EXSS, fed with no packets: whole frames, and the
    // core alone on the DTS path.
    expect_ffmpeg_bursts("DTS-HD: padded cores as Type IV match FFmpeg", pt::Codec::dtshd,
                         "dtshd-padded", "dtshd-padded-t4", kTypeIvBurstBytes);
    expect_ffmpeg_bursts("DTS-HD: padded cores on the DTS path match FFmpeg", pt::Codec::dts,
                         "dtshd-padded", "dtshd-padded-core", kCoreBurstBytes);
    test_frames_with_no_period_are_dropped();
    test_core_fallback_logs_once();
    test_dts_route();
    test_packets_send_every_frame();
    test_take_bursts_sends_each_frame_at_its_packet();
    test_only_dtshd_packets_end_frames();
    test_core_too_big_is_dropped();
    test_each_drop_reason_is_logged();
    test_bad_extension_size_ends_at_the_packet();
    test_frame_length_changes_are_sent();
    test_wrong_core_size_sends_the_packet();
    test_cut_frame_joined_to_the_next();
    test_padded_core_is_clean();
    test_fallback_core_stops_at_the_frame_end();
    test_too_big_frames_fall_back_as_ffmpeg();
    test_invalid_core_header_is_dropped();
}

__attribute__((constructor)) static void auto_run_dtshd_tests(void)
{
    run_dtshd_tests();
}
