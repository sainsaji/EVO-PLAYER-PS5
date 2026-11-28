/*
 * tests/test_pt_damage.cpp - damaged audio costs passthrough the damaged frames, not the
 * rest of the stream. Before, a frame the packer could not send, or a header it read as
 * "wait for more data", stopped evo_pt_push_packet for good: nothing went out until the
 * next seek. The other frames must still come out as FFmpeg's spdif bursts, and each
 * reason gives one log line per stream.
 * The seam is pt::Packer::take_bursts, which evo_pt_push_packet calls after each packet.
 * The vectors in tests/data/pt-damage/ come from make_vectors.py there.
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

// The vectors: 6 AC-3 frames of 768 bytes in 6144-byte bursts; 6 DTS core frames of 512
// bytes (512 samples) in 2048-byte bursts; 6 six-block E-AC-3 frames of 768 bytes, one
// per 24576-byte burst; 18 one-block E-AC-3 frames of 768 bytes, 6 to a burst; 25 AAC
// frames of varying size in 4096-byte bursts.
static const size_t kFrames = 6;
static const size_t kAc3BurstBytes = 6144;
static const size_t kDtsFrameBytes = 512;
static const size_t kDtsBurstBytes = 2048;
static const size_t kEac3FrameBytes = 768;
static const size_t kEac3BurstBytes = 24576;
static const size_t kEac3FramesPerBurst = 6;
// The frame the tests damage.
static const size_t kDamaged = 2;
// Pieces the streams are fed in besides whole: smaller than any frame, and odd.
static const size_t kPiece = 61;

static std::string data_path(const std::string &name)
{
    std::string dir = __FILE__;
    dir.erase(dir.find_last_of('/') + 1);
    return dir + "data/pt-damage/" + name;
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

// What passthrough sends for `packets`, fed one after the other as evo_pt_push_packet
// does. The packer's log lines go to *log, one per line.
static Bytes pack_packets(pt::Codec codec, const std::vector<Bytes> &packets, std::string *log)
{
    pt::Packer packer(codec);
    Bytes stream, out;
    log->clear();
    for (const Bytes &packet : packets)
    {
        stream.insert(stream.end(), packet.begin(), packet.end());
        packer.take_bursts(&stream, &out);
        const std::string notice = packer.take_notice();
        if (!notice.empty()) *log += notice + "\n";
    }
    return out;
}

// The same, for `es` cut into packets of `piece` bytes.
static Bytes pack(pt::Codec codec, const Bytes &es, size_t piece, std::string *log)
{
    std::vector<Bytes> packets;
    for (size_t at = 0; at < es.size(); at += piece)
        packets.emplace_back(es.begin() + at, es.begin() + std::min(es.size(), at + piece));
    return pack_packets(codec, packets, log);
}

// FFmpeg's bursts without burst `skip`.
static Bytes without_burst(const Bytes &ref, size_t burst_bytes, size_t skip)
{
    Bytes out(ref.begin(), ref.begin() + skip * burst_bytes);
    out.insert(out.end(), ref.begin() + (skip + 1) * burst_bytes, ref.end());
    return out;
}

static size_t count_lines(const std::string &log)
{
    return static_cast<size_t>(std::count(log.begin(), log.end(), '\n'));
}

// Packs `stream` whole and in pieces: it must give `want` and `lines` log lines. Ends
// the test the caller started.
static void expect_bursts(pt::Codec codec, const Bytes &stream, const Bytes &want, size_t lines = 1)
{
    const size_t pieces[] = {stream.size(), kPiece};
    for (size_t piece : pieces)
    {
        std::string log;
        const Bytes got = pack(codec, stream, piece, &log);
        TEST_ASSERT(got.size() == want.size(), "not the other frames' bursts");
        TEST_ASSERT(got == want, "the other frames differ from FFmpeg's bursts");
        TEST_ASSERT(count_lines(log) == lines, "not the expected log lines");
    }
    TEST_PASS();
}

typedef void (*Damage)(std::uint8_t *frame);

// Where frame `k` of a clean stream starts, read from the frame headers before it.
static size_t frame_offset(pt::Codec codec, const Bytes &es, size_t k)
{
    size_t at = 0;
    for (size_t i = 0; i < k; ++i)
        at += pt::parse_frame(codec, es, at).size;
    return at;
}

// Packs vector `name` with frame `kDamaged` damaged: the output must be FFmpeg's bursts
// without that frame's, with one log line.
static void expect_frame_dropped(const char *title, pt::Codec codec, const char *name,
                                 size_t burst_bytes, Damage damage)
{
    TEST_START(title);
    Bytes es = read_file(data_path(std::string(name) + ".es"));
    const Bytes ref = read_reference(std::string(name) + ".ref");
    TEST_ASSERT(!es.empty() && ref.size() > kDamaged * burst_bytes && ref.size() % burst_bytes == 0,
                "test vector missing");
    damage(es.data() + frame_offset(codec, es, kDamaged));
    expect_bursts(codec, es, without_burst(ref, burst_bytes, kDamaged));
}

// DTS core header (ETSI TS 102 114 5.3): NBLKS (7 bits) from bit 46, FSIZE (14 bits) after.
static void dts_160_samples(std::uint8_t *frame)
{
    const int nblks = 4; // (4 + 1) * 32 samples
    frame[4] = static_cast<std::uint8_t>((frame[4] & 0xFE) | (nblks >> 6));
    frame[5] = static_cast<std::uint8_t>((frame[5] & 0x03) | ((nblks & 0x3F) << 2));
}

static void dts_frame_bytes(std::uint8_t *frame, int bytes)
{
    const int fsize = bytes - 1;
    frame[5] = static_cast<std::uint8_t>((frame[5] & 0xFC) | (fsize >> 12));
    frame[6] = static_cast<std::uint8_t>(fsize >> 4);
    frame[7] = static_cast<std::uint8_t>(((fsize & 0xF) << 4) | (frame[7] & 0xF));
}
static void dts_too_big(std::uint8_t *frame) { dts_frame_bytes(frame, 2041); } // over 2048 - 8 for the burst header
static void dts_95_bytes(std::uint8_t *frame) { dts_frame_bytes(frame, 95); }  // FFmpeg's parser wants 96 or more
// The other checks of FFmpeg's ff_dca_parse_core_frame_header (libavcodec/dca.c). The
// vector's frames have no header CRC, so PCMR sits in bytes 11-12.
static void dts_deficit_samples(std::uint8_t *frame) { frame[4] &= 0x83; }   // SHORT 0: 1 sample, not 32
static void dts_amode_10(std::uint8_t *frame) // AMODE 10, past FFmpeg's 10 channel layouts
{
    frame[7] = static_cast<std::uint8_t>((frame[7] & 0xF0) | (10 >> 2));
    frame[8] = static_cast<std::uint8_t>((frame[8] & 0x3F) | ((10 & 3) << 6));
}
static void dts_reserved_rate(std::uint8_t *frame) { frame[8] = static_cast<std::uint8_t>((frame[8] & 0xC3) | (4 << 2)); } // SFREQ 4
static void dts_reserved_bit(std::uint8_t *frame) { frame[9] |= 0x10; }
static void dts_reserved_lfe(std::uint8_t *frame) { frame[10] |= 0x06; } // LFF 3
static void dts_reserved_pcmr(std::uint8_t *frame) // PCMR 7
{
    frame[11] |= 0x01;
    frame[12] |= 0xC0;
}

// AC-3 header (A/52 5.3): sync, crc1, then fscod (2 bits) and frmsizecod (6 bits).
static void ac3_reserved_rate(std::uint8_t *frame) { frame[4] |= 0xC0; } // fscod 3
static void ac3_reserved_size(std::uint8_t *frame) { frame[4] = static_cast<std::uint8_t>((frame[4] & 0xC0) | 38); }

// E-AC-3 header (A/52 E.1.2.1): sync, strmtyp (2 bits), substreamid (3), frmsiz (11),
// fscod (2), numblkscod or fscod2 (2). FFmpeg's parser rejects the four below.
static void eac3_reserved_type(std::uint8_t *frame) { frame[2] |= 0xC0; } // strmtyp 3
static void eac3_six_bytes(std::uint8_t *frame) // frmsiz 2: 6 bytes, shorter than its header
{
    frame[2] &= 0xF8;
    frame[3] = 2;
}
static void eac3_reserved_rate(std::uint8_t *frame) { frame[4] |= 0xF0; } // fscod 3, fscod2 3
// substreamid 7: FFmpeg's eac3_parse_header takes only 0.
static void eac3_substream_7(std::uint8_t *frame) { frame[2] |= 0x38; }
// Bytes 2-4 as a fuzz run left them: a valid header for a dependent frame of 2854 bytes,
// which ends inside frame 5. Only frame 2 may be lost.
static void eac3_random_header_bytes(std::uint8_t *frame)
{
    frame[2] = 0x65;
    frame[3] = 0x92;
    frame[4] = 0xD4;
}

static Bytes slice(const Bytes &d, size_t from, size_t to)
{
    return Bytes(d.begin() + from, d.begin() + to);
}

static Bytes concat(const Bytes &a, const Bytes &b)
{
    Bytes out = a;
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

static void test_eac3_burst_too_big(void)
{
    TEST_START("Damage: E-AC-3 frames too big for one burst");
    const Bytes es = read_file(data_path("eac3-1blk.es"));
    const Bytes ref = read_reference("eac3-1blk.ref");
    TEST_ASSERT(es.size() == 18 * kEac3FrameBytes && ref.size() == 3 * kEac3BurstBytes, "test vector missing");
    // Six one-block frames of 4096 bytes each (frmsiz 2047, the most) between the first
    // and second bursts' frames: 24576 bytes, more than a burst holds after its header.
    // FFmpeg's muxer refuses that packet ("bitrate is too high") and writes nothing for it.
    Bytes big(es.begin(), es.begin() + 6);
    big[2] |= 0x07;
    big[3] = 0xFF;
    big.resize(4096, 0);
    const size_t split = kEac3FramesPerBurst * kEac3FrameBytes;
    Bytes stream = slice(es, 0, split);
    for (size_t i = 0; i < kEac3FramesPerBurst; ++i)
        stream = concat(stream, big);
    stream = concat(stream, slice(es, split, es.size()));
    expect_bursts(pt::Codec::eac3, stream, ref);
}

static void test_eac3_cut_off(void)
{
    TEST_START("Damage: E-AC-3 frames cut off before a burst");
    const Bytes es = read_file(data_path("eac3-1blk.es"));
    const Bytes ref = read_reference("eac3-1blk.ref");
    TEST_ASSERT(es.size() == 18 * kEac3FrameBytes && ref.size() == 3 * kEac3BurstBytes, "test vector missing");
    // Frames 6-8, then 100 bytes that are no frame, then frames 12-17: the 3 frames can't
    // make a whole burst, and waiting for the rest held the stream until the next seek.
    Bytes stream = slice(es, 0, 9 * kEac3FrameBytes);
    stream = concat(stream, Bytes(100, 0));
    stream = concat(stream, slice(es, 12 * kEac3FrameBytes, es.size()));
    expect_bursts(pt::Codec::eac3, stream, without_burst(ref, kEac3BurstBytes, 1));
}

// Stray bytes after a good frame cost only themselves: they hold no sync word, so the
// packer skips them as it skips any bytes between frames, without a log line.
static void expect_stray_bytes_skipped(const char *title, const char *name, size_t frames_before)
{
    TEST_START(title);
    const Bytes es = read_file(data_path(std::string(name) + ".es"));
    const Bytes ref = read_reference(std::string(name) + ".ref");
    TEST_ASSERT(es.size() % kEac3FrameBytes == 0 && !ref.empty(), "test vector missing");
    const size_t split = frames_before * kEac3FrameBytes;
    Bytes stream = slice(es, 0, split);
    stream = concat(stream, Bytes(100, 0));
    stream = concat(stream, slice(es, split, es.size()));
    expect_bursts(pt::Codec::eac3, stream, ref, 0);
}

static void test_clean_streams_match_ffmpeg(void)
{
    TEST_START("Damage: clean streams match FFmpeg");
    const char *names[] = {"ac3", "dts", "eac3", "eac3-1blk", "aac"};
    const pt::Codec codecs[] = {pt::Codec::ac3, pt::Codec::dts, pt::Codec::eac3, pt::Codec::eac3, pt::Codec::aac};
    for (int i = 0; i < 5; ++i)
    {
        const Bytes es = read_file(data_path(std::string(names[i]) + ".es"));
        const Bytes ref = read_reference(std::string(names[i]) + ".ref");
        TEST_ASSERT(!es.empty() && !ref.empty(), "test vector missing");
        const size_t pieces[] = {es.size(), kPiece};
        for (size_t piece : pieces)
        {
            std::string log;
            TEST_ASSERT(pack(codecs[i], es, piece, &log) == ref, names[i]);
            TEST_ASSERT(log.empty(), "a log line for a clean stream");
        }
    }
    TEST_PASS();
}

// TrueHD access units of 1000 bytes, all alike: the packer reads only each unit's length
// word, so any 24 of them make the same MAT burst. One unit per packet, as Matroska and
// M2TS carry them.
static const size_t kTruehdUnitBytes = 1000;
static const size_t kTruehdUnits = 600;
static const size_t kMatUnits = 24;
static const size_t kMatBurstBytes = 61440;

static std::vector<Bytes> truehd_units(void)
{
    Bytes unit(kTruehdUnitBytes, 0x5A);
    unit[0] = static_cast<std::uint8_t>(0xF0 | ((kTruehdUnitBytes / 2) >> 8));
    unit[1] = static_cast<std::uint8_t>((kTruehdUnitBytes / 2) & 0xFF);
    return std::vector<Bytes>(kTruehdUnits, unit);
}

// The packer waits for the rest of a unit whose length word says 0, and no data ever
// completes it. Passthrough must drop what it holds then, rather than hold it until the
// next seek. The next packet starts a unit, so the stream goes on from there.
static void test_truehd_zero_length_unit(void)
{
    TEST_START("Damage: TrueHD unit of length 0");
    std::string log;
    std::vector<Bytes> units = truehd_units();
    const Bytes clean = pack_packets(pt::Codec::truehd, units, &log);
    TEST_ASSERT(clean.size() == kTruehdUnits / kMatUnits * kMatBurstBytes && log.empty(), "clean stream");

    units[100][0] &= 0xF0;
    units[100][1] = 0;
    const Bytes got = pack_packets(pt::Codec::truehd, units, &log);
    // What may be lost: the units held when the damage came, and the 128 KiB held after it.
    const size_t most_lost = (kMatUnits + 128 * 1024 / kTruehdUnitBytes + 1) / kMatUnits + 1;
    TEST_ASSERT(got.size() % kMatBurstBytes == 0, "not whole MAT bursts");
    TEST_ASSERT(got.size() / kMatBurstBytes + most_lost >= clean.size() / kMatBurstBytes,
                "the units after the damage were not sent");
    for (size_t at = 0; at < got.size(); at += kMatBurstBytes)
        TEST_ASSERT(std::equal(got.begin() + at, got.begin() + at + kMatBurstBytes, clean.begin()),
                    "a burst differs from the clean ones");
    TEST_ASSERT(count_lines(log) == 1, "not one log line");
    TEST_PASS();
}

// DTS from FFmpeg's encoder at 1536k: each 2048-byte frame fills its 2048-byte burst
// exactly, and FFmpeg's spdif muxer sends it without the burst preamble. (Disc cores at
// 1509 kbit/s are 2012 bytes and fit with the preamble.)
static void test_dts_frames_fill_burst(void)
{
    TEST_START("Damage: clean DTS frames that fill their burst");
    const Bytes es = read_file(data_path("dts-full.es"));
    const Bytes ref = read_reference("dts-full.ref");
    TEST_ASSERT(es.size() == kFrames * kDtsBurstBytes && ref.size() == es.size(), "test vector missing");
    const size_t pieces[] = {es.size(), kPiece};
    for (size_t piece : pieces)
    {
        std::string log;
        TEST_ASSERT(pack(pt::Codec::dts, es, piece, &log) == ref, "not FFmpeg's bursts");
        TEST_ASSERT(log.empty(), "a log line for a clean stream");
    }
    TEST_PASS();
}

static void test_each_reason_logs_once(void)
{
    TEST_START("Damage: each reason is logged once per stream");
    Bytes es = read_file(data_path("dts.es"));
    TEST_ASSERT(es.size() == kFrames * kDtsFrameBytes, "test vector missing");
    dts_160_samples(es.data() + 1 * kDtsFrameBytes);
    dts_160_samples(es.data() + 3 * kDtsFrameBytes);
    pt::Packer packer(pt::Codec::dts);
    Bytes stream = es, out;
    packer.take_bursts(&stream, &out);
    const std::string first = packer.take_notice();
    TEST_ASSERT(out.size() == (kFrames - 2) * kDtsBurstBytes, "not the 4 good frames");
    TEST_ASSERT(!first.empty() && count_lines(first + "\n") == 1, "not one log line for two drops");
    stream = es;
    packer.take_bursts(&stream, &out);
    TEST_ASSERT(packer.take_notice().empty(), "logged again in the same stream");
    packer.set_codec(pt::Codec::dts);
    stream = es;
    packer.take_bursts(&stream, &out);
    TEST_ASSERT(!packer.take_notice().empty(), "a new stream was not logged");
    TEST_PASS();
}

// More than 128 KiB without a sync word: no frame can start in it, so take_bursts must not
// keep waiting on it. It drops the bytes with one "held no frame" line, holds nothing, and
// the frames after it come out as FFmpeg's bursts.
static void test_backstop_drops_bytes_with_no_frame(void)
{
    TEST_START("Damage: over 128 KiB with no sync word is dropped");
    const Bytes es = read_file(data_path("ac3.es"));
    const Bytes ref = read_reference("ac3.ref");
    TEST_ASSERT(!es.empty() && ref.size() == kFrames * kAc3BurstBytes, "test vector missing");
    const Bytes junk(128 * 1024 + 1, 0x00); // 0x0B77 never appears in it
    const size_t pieces[] = {junk.size(), kPiece};
    for (size_t piece : pieces)
    {
        pt::Packer packer(pt::Codec::ac3);
        Bytes stream, out;
        std::string log;
        for (size_t at = 0; at < junk.size(); at += piece)
        {
            stream.insert(stream.end(), junk.begin() + at, junk.begin() + std::min(junk.size(), at + piece));
            packer.take_bursts(&stream, &out);
            const std::string notice = packer.take_notice();
            if (!notice.empty()) log += notice + "\n";
        }
        TEST_ASSERT(stream.empty(), "the bytes with no frame are still held");
        TEST_ASSERT(out.empty(), "bursts out of bytes with no frame");
        TEST_ASSERT(count_lines(log) == 1 && log.find("held no frame") != std::string::npos,
                    "not one \"held no frame\" line");
        stream = es;
        packer.take_bursts(&stream, &out);
        TEST_ASSERT(out == ref, "the frames after it differ from FFmpeg's bursts");
        TEST_ASSERT(packer.take_notice().empty(), "a log line for the clean frames");
    }
    TEST_PASS();
}

extern "C" void run_pt_damage_tests(void)
{
    test_clean_streams_match_ffmpeg();
    expect_frame_dropped("Damage: DTS frame of 160 samples", pt::Codec::dts, "dts",
                         kDtsBurstBytes, dts_160_samples);
    expect_frame_dropped("Damage: DTS frame too big for its burst", pt::Codec::dts, "dts",
                         kDtsBurstBytes, dts_too_big);
    expect_frame_dropped("Damage: DTS frame of 95 bytes", pt::Codec::dts, "dts",
                         kDtsBurstBytes, dts_95_bytes);
    expect_frame_dropped("Damage: DTS frame of 1 deficit sample", pt::Codec::dts, "dts",
                         kDtsBurstBytes, dts_deficit_samples);
    expect_frame_dropped("Damage: DTS reserved channel layout", pt::Codec::dts, "dts",
                         kDtsBurstBytes, dts_amode_10);
    expect_frame_dropped("Damage: DTS reserved sample rate code", pt::Codec::dts, "dts",
                         kDtsBurstBytes, dts_reserved_rate);
    expect_frame_dropped("Damage: DTS reserved bit set", pt::Codec::dts, "dts",
                         kDtsBurstBytes, dts_reserved_bit);
    expect_frame_dropped("Damage: DTS reserved LFE code", pt::Codec::dts, "dts",
                         kDtsBurstBytes, dts_reserved_lfe);
    expect_frame_dropped("Damage: DTS reserved PCM resolution", pt::Codec::dts, "dts",
                         kDtsBurstBytes, dts_reserved_pcmr);
    test_dts_frames_fill_burst();
    expect_frame_dropped("Damage: DTS frame of 160 samples among frames that fill their burst",
                         pt::Codec::dts, "dts-full", kDtsBurstBytes, dts_160_samples);
    test_each_reason_logs_once();
    test_truehd_zero_length_unit();
    test_backstop_drops_bytes_with_no_frame();
    expect_frame_dropped("Damage: AC-3 reserved sample rate code", pt::Codec::ac3, "ac3",
                         kAc3BurstBytes, ac3_reserved_rate);
    expect_frame_dropped("Damage: AC-3 reserved frame size code", pt::Codec::ac3, "ac3",
                         kAc3BurstBytes, ac3_reserved_size);
    expect_frame_dropped("Damage: E-AC-3 reserved stream type", pt::Codec::eac3, "eac3",
                         kEac3BurstBytes, eac3_reserved_type);
    expect_frame_dropped("Damage: E-AC-3 frame shorter than its header", pt::Codec::eac3, "eac3",
                         kEac3BurstBytes, eac3_six_bytes);
    expect_frame_dropped("Damage: E-AC-3 reserved sample rate code", pt::Codec::eac3, "eac3",
                         kEac3BurstBytes, eac3_reserved_rate);
    expect_frame_dropped("Damage: E-AC-3 random bytes in a header", pt::Codec::eac3, "eac3",
                         kEac3BurstBytes, eac3_random_header_bytes);
    expect_frame_dropped("Damage: E-AC-3 substream ID other than 0", pt::Codec::eac3, "eac3",
                         kEac3BurstBytes, eac3_substream_7);
    test_eac3_burst_too_big();
    test_eac3_cut_off();
    expect_stray_bytes_skipped("Damage: stray bytes after a 1-block E-AC-3 burst", "eac3-1blk",
                               kEac3FramesPerBurst);
    expect_stray_bytes_skipped("Damage: stray bytes after a 6-block E-AC-3 frame", "eac3", kDamaged);
}

__attribute__((constructor)) static void auto_run_pt_damage_tests(void)
{
    run_pt_damage_tests();
}
