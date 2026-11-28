// evo/audio/iec61937.cpp - IEC 61937 packing for AC-3, E-AC-3, DTS, and AAC.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "evo/audio/iec61937.hpp"
#include <libavcodec/defs.h>
#include <algorithm>
#include <cstring>
#include <string>

namespace pt
{

namespace
{

constexpr std::uint16_t kPa = 0xF872;
constexpr std::uint16_t kPb = 0x4E1F;
constexpr std::size_t kBurstHeaderBytes = 8; // Pa Pb Pc Pd
constexpr std::size_t kPortFrameBytes = 4;   // one S16 stereo frame
constexpr std::size_t kDtsSyncBytes = 4;     // a DTS core or extension substream sync word
// IEC 61937 data types
constexpr std::uint16_t kTypeAc3  = 0x01;
constexpr std::uint16_t kTypeDts1 = 0x0B; // 512 samples per frame
constexpr std::uint16_t kTypeDts2 = 0x0C; // 1024
constexpr std::uint16_t kTypeDts3 = 0x0D; // 2048
constexpr std::uint16_t kTypeEac3 = 0x15;
constexpr std::uint16_t kTypeAac  = 0x07; // MPEG-2/4 AAC, 1024 samples per frame
constexpr std::uint16_t kTypeTruehd = 0x16; // Dolby TrueHD, as MAT
constexpr std::uint16_t kTypeDtshd = 0x11;  // DTS-HD, IEC 61937-9 Type IV

// DTS-HD goes out the way FFmpeg's spdif muxer sends it with -dtshd_rate 192000: each
// whole frame after this start code and a 16-bit size, on the 192 kHz port.
constexpr int kDtshdRate = 192000;
constexpr int kDtshdFallbackSeconds = 60;
constexpr std::uint8_t kDtshdStartCode[10] = {0x01, 0x00, 0x00, 0x00, 0x00,
                                              0x00, 0x00, 0x00, 0xFE, 0xFE};
constexpr std::size_t kDtshdSizeAt = sizeof(kDtshdStartCode); // the 16-bit frame size
constexpr std::size_t kDtshdFrameAt = kDtshdSizeAt + 2;      // the frame itself
constexpr long kDtshdShortestPeriod = 512; // port frames in a subtype 0 burst
constexpr int kDtshdSubtypes = 6;          // 512 << 0 .. 512 << 5
// Pd is rounded up so that Pd % 16 == 8; some receivers reportedly need it (FFmpeg).
constexpr std::size_t kDtshdPdAlign = 16;
constexpr std::size_t kDtshdPdRemainder = 8;
// A DTS-HD stream's core frame: 512 samples, the length a 48 kHz-family core uses.
constexpr int kDtshdCoreSamples = 512;
// DTS core sample rates by the frame header's SFREQ field, 0 where ETSI TS 102 114 table
// 5-5 says invalid. FFmpeg's table reads 14 and 15 as 96 and 192 kHz, but a core is at most
// 48 kHz: higher rates live in its extensions (5.3.1).
constexpr int kDtsSampleRates[16] = {0,     8000, 16000, 32000, 0,     0,     11025, 22050,
                                     44100, 0,    0,     12000, 24000, 48000, 0,     0};

// One MAT frame holds up to 24 TrueHD access units (40 samples each, so 20 ms)
// and goes out in a 61440-byte burst on the 768 kHz port.
constexpr std::size_t kMatFrameSize = 61424;
constexpr std::size_t kMatBurstBytes = 61440;
constexpr int kMatAccessUnits = 24;
constexpr std::uint8_t kMatStartCode[20] = {
    0x07, 0x9E, 0x00, 0x03, 0x84, 0x01, 0x01, 0x01, 0x80, 0x00,
    0x56, 0xA5, 0x3B, 0xF4, 0x81, 0x83, 0x49, 0x80, 0x77, 0xE0,
};
constexpr std::uint8_t kMatMiddleCode[12] = {
    0xC3, 0xC1, 0x42, 0x49, 0x3B, 0xFA, 0x82, 0x83, 0x49, 0x80, 0x77, 0xE0,
};
constexpr std::uint8_t kMatEndCode[16] = {
    0xC3, 0xC2, 0xC0, 0xC4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x97, 0x11,
};

// AC-3 frame size in 16-bit words, by frmsizecod (0..37) and fscod (48, 44.1, 32 kHz).
constexpr std::uint16_t kAc3Words[38][3] = {
    {64, 69, 96},       {64, 70, 96},       {80, 87, 120},      {80, 88, 120},
    {96, 104, 144},     {96, 105, 144},     {112, 121, 168},    {112, 122, 168},
    {128, 139, 192},    {128, 140, 192},    {160, 174, 240},    {160, 175, 240},
    {192, 208, 288},    {192, 209, 288},    {224, 243, 336},    {224, 244, 336},
    {256, 278, 384},    {256, 279, 384},    {320, 348, 480},    {320, 349, 480},
    {384, 417, 576},    {384, 418, 576},    {448, 487, 672},    {448, 488, 672},
    {512, 557, 768},    {512, 558, 768},    {640, 696, 960},    {640, 697, 960},
    {768, 835, 1152},   {768, 836, 1152},   {896, 975, 1344},   {896, 976, 1344},
    {1024, 1114, 1536}, {1024, 1115, 1536}, {1152, 1253, 1728}, {1152, 1254, 1728},
    {1280, 1393, 1920}, {1280, 1394, 1920},
};

constexpr int kEac3Blocks[4] = {1, 2, 3, 6};

// DTS core: FFmpeg's channel layouts (AMODE 0-9), and the SFREQ codes its ff_dca_sample_rates
// gives a rate for.
constexpr int kDtsChannelLayouts = 10;
constexpr bool kDtsSampleRateValid[16] = {false, true, true, true, false, false, true, true,
                                          true, false, false, true, true, true, true, true};

// Fewer bytes than this hold no frame header.
constexpr std::size_t kFrameHeaderBytes = 8;
// More than any frame, or than the TrueHD units of one MAT frame (24 units at TrueHD's
// peak 18 Mbit/s are about 45 KB; a DTS core frame is at most 16 KB, six E-AC-3 blocks
// with their dependent frames 48 KB). A stream that holds this much without giving a
// frame waits on damaged data that no more data completes.
constexpr std::size_t kMaxHeldBytes = 128 * 1024;

constexpr std::size_t kAc3HeaderBytes = 6; // through bsid

bool is_ac3_sync(Span<std::uint8_t> d, std::size_t at)
{
    return at + kAc3HeaderBytes <= d.size() && d[at] == 0x0B && d[at + 1] == 0x77;
}

// Whether an AC-3 sync word with a valid header starts inside the frame at `at`, before
// `end`: a stated size that covers one is wrong.
bool covers_sync(Span<std::uint8_t> d, std::size_t at, std::size_t end)
{
    for (std::size_t i = at + kAc3HeaderBytes; i < end; ++i)
    {
        if (is_ac3_sync(d, i) && !parse_frame(Codec::eac3, d, i).invalid)
            return true;
    }
    return false;
}

bool is_dts_sync(Span<std::uint8_t> d, std::size_t at)
{
    return at + 10 <= d.size() && d[at] == 0x7F && d[at + 1] == 0xFE && d[at + 2] == 0x80 &&
           d[at + 3] == 0x01;
}

bool is_dts_exss_sync(Span<std::uint8_t> d, std::size_t at)
{
    return at + kDtsSyncBytes <= d.size() && d[at] == 0x64 && d[at + 1] == 0x58 && d[at + 2] == 0x20 &&
           d[at + 3] == 0x25;
}

// `count` bits (at most 24) of `d` from bit `bit` after byte `at`, most significant first.
std::uint32_t read_bits(Span<std::uint8_t> d, std::size_t at, std::size_t bit, int count)
{
    std::uint32_t value = 0;
    for (int i = 0; i < count; ++i, ++bit)
        value = (value << 1) | ((d[at + bit / 8] >> (7 - bit % 8)) & 1);
    return value;
}

// Bytes in the DTS-HD extension substream at `at`, or 0 if its header is not all in `d`
// yet. ETSI TS 102 114 7.5.1: after the 32-bit sync come 8 user bits, a 2-bit index and
// the header size type, then the header size and FSIZE in 8 and 16 bits (type 0) or 12
// and 20 bits (type 1), each stored minus one.
std::size_t dts_exss_size(Span<std::uint8_t> d, std::size_t at)
{
    constexpr std::size_t kSizeTypeBit = 32 + 8 + 2;
    constexpr std::size_t kHeaderBytes = 10; // through the end of a 20-bit FSIZE
    if (at + kHeaderBytes > d.size())
        return 0;
    const bool wide = read_bits(d, at, kSizeTypeBit, 1) != 0;
    const std::size_t fsize_bit = kSizeTypeBit + 1 + (wide ? 12 : 8);
    return static_cast<std::size_t>(read_bits(d, at, fsize_bit, wide ? 20 : 16)) + 1;
}

bool is_adts_sync(Span<std::uint8_t> d, std::size_t at)
{
    return at + 7 <= d.size() && d[at] == 0xFF && (d[at + 1] & 0xF6) == 0xF0;
}

bool is_sync_any(Codec codec, Span<std::uint8_t> d, std::size_t at)
{
    switch (codec)
    {
    case Codec::ac3:
    case Codec::eac3:
        return is_ac3_sync(d, at);
    case Codec::dts:
    case Codec::dtshd:
        return is_dts_sync(d, at);
    case Codec::aac:
        return is_adts_sync(d, at);
    case Codec::truehd:
        return true; // access units are packet-aligned: there is no sync word to find
    default:
        return false;
    }
}

} // namespace

int dtshd_subtype(int sample_rate, int samples)
{
    if (sample_rate <= 0 || samples <= 0)
        return -1;
    const long long scaled = static_cast<long long>(kDtshdRate) * samples;
    if (scaled % sample_rate != 0)
        return -1;
    const long long period = scaled / sample_rate;
    for (int subtype = 0; subtype < kDtshdSubtypes; ++subtype)
        if (period == (kDtshdShortestPeriod << subtype))
            return subtype;
    return -1;
}

bool packet_ends_frame(Codec codec)
{
    return codec == Codec::dtshd;
}

Codec dts_route(int profile, int sample_rate, bool sink_lists_dtshd)
{
    // HRA, MA, MA X and MA X IMAX carry a core to fall back to; DTS Express (70) has none.
    const bool hd = profile >= AV_PROFILE_DTS_HD_HRA && profile <= AV_PROFILE_DTS_HD_MA_X_IMAX;
    // FFmpeg gives the HD rate (96 kHz over a 48 kHz core, say), which is in its core's
    // family: a 44.1 kHz-family stream has no Type IV period, so it keeps the core path.
    const bool fits = dtshd_subtype(sample_rate, kDtshdCoreSamples) >= 0;
    return hd && fits && sink_lists_dtshd ? Codec::dtshd : Codec::dts;
}

const char *codec_name(Codec codec)
{
    switch (codec)
    {
    case Codec::ac3:
        return "Dolby Digital (AC-3)";
    case Codec::eac3:
        return "Dolby Digital Plus (E-AC-3)";
    case Codec::dts:
        return "DTS";
    case Codec::aac:
        return "AAC (ADTS)";
    case Codec::truehd:
        return "Dolby TrueHD";
    case Codec::dtshd:
        return "DTS-HD";
    case Codec::unknown:
        break;
    }
    return "Unknown";
}

Codec detect(Span<std::uint8_t> data)
{
    if (is_ac3_sync(data, 0))
        return (data[5] >> 3) > 10 ? Codec::eac3 : Codec::ac3; // bsid 16 is E-AC-3
    if (is_dts_sync(data, 0))
        return Codec::dts;
    if (is_adts_sync(data, 0))
        return Codec::aac;
    return Codec::unknown;
}

Carrier carrier_for(Codec codec)
{
    // From sceAudioOutExOpen's own tables in libSceAudioOut (FW 12.70).
    switch (codec)
    {
    case Codec::ac3:
        return {0, 256, 48000};
    case Codec::dts:
        return {2, 256, 48000};
    case Codec::eac3:
        return {3, 1024, 192000};
    case Codec::aac:
        return {1, 256, 48000};
    case Codec::truehd:
        // sceAudioOutSysOpen mode 5: a 768 kHz port of 1024 S16 8-channel
        // frames; SysConfigureOutput mode 5 is MAT (the Blu-ray player's route).
        return {5, 1024, 768000, true, 16};
    case Codec::dtshd:
        // Ex mode 4 (the console logs it as BITSTREAM DTS_HD_HR): a 192 kHz S16 stereo
        // port of 1024-frame grains, like E-AC-3's mode 3.
        return {4, 1024, 192000};
    case Codec::unknown:
        break;
    }
    return {};
}

Frame parse_frame(Codec codec, Span<std::uint8_t> d, std::size_t at)
{
    Frame f;
    f.offset = at;
    if (codec == Codec::ac3 || codec == Codec::eac3)
    {
        if (!is_ac3_sync(d, at))
            return f;
        // The checks of FFmpeg's ff_ac3_parse_header (libavcodec/ac3_parser.c): a header that
        // fails them is damaged.
        const int bsid = d[at + 5] >> 3;
        if (bsid > 16)
        {
            f.invalid = true;
            return f;
        }
        if (bsid <= 10)
        {
            const int fscod = d[at + 4] >> 6;
            const int frmsizecod = d[at + 4] & 0x3F;
            if (fscod > 2 || frmsizecod > 37)
            {
                f.invalid = true;
                return f;
            }
            f.size = static_cast<std::size_t>(kAc3Words[frmsizecod][fscod]) * 2;
            f.samples = 1536;
            f.blocks = 6;
            f.bsmod = d[at + 5] & 7;
        }
        else
        {
            const int strmtyp = d[at + 2] >> 6;
            const int substreamid = (d[at + 2] >> 3) & 7;
            const int frmsiz = ((d[at + 2] & 7) << 8) | d[at + 3];
            const int fscod = d[at + 4] >> 6;
            const int numblkscod = (d[at + 4] >> 4) & 3; // fscod2 when fscod is 3
            if (strmtyp == 3 || substreamid != 0 || frmsiz < 3 || (fscod == 3 && numblkscod == 3))
            {
                // reserved type, a substream FFmpeg's eac3_parse_header does not take, shorter
                // than its own header, reserved rate
                f.invalid = true;
                return f;
            }
            f.size = static_cast<std::size_t>(frmsiz + 1) * 2;
            f.blocks = fscod == 3 ? 6 : kEac3Blocks[numblkscod];
            f.samples = f.blocks * 256;
            f.dependent = strmtyp == 1;
        }
    }
    else if (codec == Codec::dts || codec == Codec::dtshd)
    {
        if (!is_dts_sync(d, at))
            return f;
        // The checks of FFmpeg's ff_dca_parse_core_frame_header (libavcodec/dca.c), on the
        // header through PCMR (ETSI TS 102 114 5.3): a header that fails them is damaged.
        const bool crc = d[at + 4] & 2;
        if (at + (crc ? 15 : 13) > d.size())
            return f;
        const int deficit = ((d[at + 4] >> 2) & 0x1F) + 1;
        const int nblks = ((d[at + 4] & 1) << 6) | (d[at + 5] >> 2);
        const int fsize = (((d[at + 5] & 3) << 12) | (d[at + 6] << 4) | (d[at + 7] >> 4)) + 1;
        const int amode = ((d[at + 7] & 0x0F) << 2) | (d[at + 8] >> 6);
        const int sfreq = (d[at + 8] >> 2) & 0x0F;
        const bool reserved = d[at + 9] & 0x10;
        const int lff = (d[at + 10] >> 1) & 3;
        const std::size_t pcmr_at = at + (crc ? 13 : 11);
        const int pcmr = ((d[pcmr_at] & 1) << 2) | (d[pcmr_at + 1] >> 6);
        if (deficit != 32 || (nblks + 1) % 8 != 0 || fsize < 96 || amode >= kDtsChannelLayouts ||
            !kDtsSampleRateValid[sfreq] || reserved || lff == 3 || pcmr == 4 || pcmr == 7)
        {
            f.invalid = true;
            return f;
        }
        f.samples = (nblks + 1) * 32;
        f.size = static_cast<std::size_t>(fsize);
        f.sample_rate = kDtsSampleRates[(d[at + 8] >> 2) & 0x0F];
    }
    else if (codec == Codec::aac)
    {
        if (!is_adts_sync(d, at))
            return f;
        const int length = ((d[at + 3] & 3) << 11) | (d[at + 4] << 3) | (d[at + 5] >> 5);
        f.size = static_cast<std::size_t>(length);
        f.samples = ((d[at + 6] & 3) + 1) * 1024; // raw data blocks in the frame
    }
    else if (codec == Codec::truehd)
    {
        // An access unit starts with a 16-bit word: 4 check bits, then its
        // length in 16-bit words.
        if (at + 2 > d.size())
            return f;
        f.size = ((static_cast<std::size_t>(d[at] & 0x0F) << 8) | d[at + 1]) * 2;
        f.samples = 40;
    }
    if (at + f.size > d.size())
        f.size = 0;
    return f;
}

namespace
{

// Writes `payload` as big-endian 16-bit words into little-endian samples at `b`: payload
// bytes a, b become the sample 0xaabb.
void put_words(Span<std::uint8_t> payload, std::uint8_t *b)
{
    for (std::size_t i = 0; i < payload.size(); i += 2)
    {
        b[i] = i + 1 < payload.size() ? payload[i + 1] : 0;
        b[i + 1] = payload[i];
    }
}

} // namespace

void write_burst(std::uint16_t data_type, std::uint16_t length_code,
                 Span<std::uint8_t> payload, std::size_t burst_bytes,
                 std::vector<std::uint8_t> *out)
{
    const std::size_t start = out->size();
    out->resize(start + burst_bytes, 0);
    std::uint8_t *b = out->data() + start;
    const auto put = [&](std::size_t word, std::uint16_t v)
    {
        b[word * 2] = static_cast<std::uint8_t>(v & 0xFF); // samples are little-endian
        b[word * 2 + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    put(0, kPa);
    put(1, kPb);
    put(2, data_type);
    put(3, length_code);
    put_words(payload, b + 8);
}

bool Packer::next_burst(Span<std::uint8_t> d, std::size_t *cursor,
                        std::vector<std::uint8_t> *out, const char **error,
                        bool flush)
{
    *error = nullptr;
    if (*cursor >= d.size())
        return false;

    // Resynchronize if cursor doesn't point to a sync word
    while (*cursor < d.size() && !is_sync_any(codec_, d, *cursor))
    {
        (*cursor)++;
    }
    if (*cursor >= d.size())
        return false;

    const Frame first = parse_frame(codec_, d, *cursor);
    if (first.invalid)
    {
        if (codec_ == Codec::dtshd)
        {
            // As next_dtshd_burst does with an invalid core: the frame goes, and its packet
            // with it (without packets, the next sync word ends it).
            notice_once(kNoticeInvalidCore);
            *cursor = flush ? d.size() : *cursor + 1;
            return true;
        }
        // No more data makes this header valid: step past its sync word, so the next call
        // finds the next frame.
        return skip_frame(Drop::damaged_header, cursor);
    }
    if (first.size == 0 && codec_ == Codec::dtshd && flush)
    {
        // A DTS-HD packet holds one whole frame, so a core that runs past its end (or a header
        // cut off there) is damaged. It is caught here, before next_dtshd_burst, because
        // parse_frame gives such a core size 0, which below means "wait for more data": that
        // would read the next frames into it.
        notice_once(kNoticeInvalidCore);
        *cursor = d.size();
        return true;
    }
    if (first.size == 0)
    {
        // Incomplete frame at the tail of the buffer; wait for more data
        return false;
    }

    if (codec_ == Codec::ac3)
    {
        constexpr std::size_t kBurst = 1536 * 4;
        if (first.size + 8 > kBurst)
        {
            // Cannot happen with a valid header (at most 3840 bytes); dropped as E-AC-3 and DTS are.
            return skip_frame(Drop::too_big, cursor);
        }
        write_burst(static_cast<std::uint16_t>(kTypeAc3 | (first.bsmod << 8)),
                    static_cast<std::uint16_t>(first.size * 8),
                    d.subspan(first.offset, first.size),
                    kBurst, out);
        *cursor += first.size;
        return true;
    }

    if (codec_ == Codec::eac3)
    {
        // One burst carries 6 audio blocks (1536 samples) and lasts 4x as long
        // in port frames because the port runs at 192 kHz: 6144 stereo frames.
        constexpr std::size_t kBurst = 6144 * 4;
        std::size_t end = *cursor;
        std::size_t last = *cursor; // where the last frame taken starts
        int blocks = 0;
        int last_blocks = 0;
        bool incomplete_tail = false;
        bool next_damaged = false; // a sync word follows, with a damaged header
        bool ends_in_junk = false; // bytes that are no frame follow
        for (;;)
        {
            const Frame f = parse_frame(codec_, d, end);
            if (f.size == 0)
            {
                next_damaged = f.invalid;
                ends_in_junk = !f.invalid && end + kAc3HeaderBytes <= d.size() && !is_ac3_sync(d, end);
                incomplete_tail = !next_damaged && !ends_in_junk;
                break;
            }
            if (!f.dependent && blocks >= 6)
                break; // the next independent frame starts the next burst
            last = end;
            last_blocks = f.dependent ? 0 : f.blocks;
            blocks += last_blocks;
            end += f.size;
        }

        if (blocks < 6 && incomplete_tail && !flush)
        {
            // Wait for remaining blocks of this 6-block burst
            return false;
        }
        // When the last frame's stated size ends in bytes that are no frame, its header is
        // damaged, or the data after it is. A size that covers another frame's sync word is
        // wrong. One that does not is taken as right: a whole burst goes out, and the next
        // call skips the stray bytes after it.
        if (ends_in_junk && blocks >= 6 && !covers_sync(d, last, end))
            ends_in_junk = false;
        if (ends_in_junk && !flush)
        {
            // Drop the last frame, and the frames before it unless they make a whole burst
            // (sent first; the next call drops it). Then look for the next frame from just
            // after its sync word: a wrong size would skip it.
            if (blocks - last_blocks >= 6)
            {
                end = last;
            }
            else
            {
                return skip_to(last == *cursor ? Drop::damaged_header : Drop::cut_off, cursor, last + 1);
            }
        }
        else if (blocks < 6 && next_damaged && !flush)
        {
            // The rest of this burst will not come, and a short burst would play too fast.
            return skip_to(Drop::cut_off, cursor, end);
        }

        const std::size_t length = end - *cursor;
        if (length + 8 > kBurst)
        {
            // FFmpeg's muxer refuses this packet ("bitrate is too high") and writes nothing for it.
            return skip_to(Drop::too_big, cursor, end);
        }
        write_burst(kTypeEac3, static_cast<std::uint16_t>(length),
                    d.subspan(*cursor, length),
                    kBurst, out);
        *cursor = end;
        return true;
    }

    if (codec_ == Codec::aac)
    {
        // One ADTS frame, header included, per burst of 1024 samples.
        if (first.samples != 1024)
        {
            *error = "AAC frame is not 1024 samples (multi-block ADTS)";
            return false;
        }
        constexpr std::size_t kBurst = 1024 * 4;
        if (first.size + 8 > kBurst)
        {
            *error = "AAC frame too large for one burst";
            return false;
        }
        write_burst(kTypeAac, static_cast<std::uint16_t>(((first.size + 1) & ~std::size_t{1}) * 8), // Pd: bits, padded to a whole 16-bit word
                    d.subspan(first.offset, first.size), kBurst, out);
        *cursor += first.size;
        return true;
    }

    if (codec_ == Codec::truehd)
    {
        // Pack 24 access units into one MAT frame. A short frame would last
        // as long as a full one and run the audio fast, so wait for all 24
        // unless the stream is ending.
        std::size_t cur = *cursor;
        int available = 0;
        while (available < kMatAccessUnits)
        {
            const Frame f = parse_frame(codec_, d, cur);
            if (f.size == 0)
                break;
            cur += f.size;
            ++available;
        }
        if (available == 0 || (available < kMatAccessUnits && !flush))
            return false;

        std::vector<std::uint8_t> mat(kMatFrameSize, 0);
        std::memcpy(mat.data(), kMatStartCode, sizeof(kMatStartCode));
        std::memcpy(mat.data() + 30708, kMatMiddleCode, sizeof(kMatMiddleCode));
        std::memcpy(mat.data() + (kMatFrameSize - sizeof(kMatEndCode)), kMatEndCode,
                    sizeof(kMatEndCode));

        cur = *cursor;
        std::size_t pos = sizeof(kMatStartCode);
        int packed = 0;
        while (packed < available)
        {
            const Frame f = parse_frame(codec_, d, cur);
            if (f.size == 0)
                break;
            if (packed >= 12 && pos < 30720)
                pos = 30720; // the second half of the frame starts after the middle code
            std::size_t limit = (pos < 30708) ? 30708 : (kMatFrameSize - sizeof(kMatEndCode));
            if (pos + f.size > limit)
            {
                if (pos < 30708)
                {
                    pos = 30720;
                    limit = kMatFrameSize - sizeof(kMatEndCode);
                }
                if (pos + f.size > limit)
                    break;
            }
            std::memcpy(mat.data() + pos, d.data() + cur, f.size);
            pos += f.size;
            cur += f.size;
            ++packed;
        }
        if (packed == 0)
        {
            // Cannot happen: a unit is at most 8190 bytes. If it did, take_bursts would step
            // over it.
            *error = "TrueHD access unit does not fit a MAT frame";
            return false;
        }

        write_burst(kTypeTruehd, static_cast<std::uint16_t>(kMatFrameSize),
                    Span<std::uint8_t>(mat), kMatBurstBytes, out);
        *cursor = cur;
        return true;
    }

    if (codec_ == Codec::dtshd)
        return next_dtshd_burst(d, first, cursor, out, flush);

    if (codec_ == Codec::dts)
    {
        std::uint16_t type = 0;
        switch (first.samples)
        {
        case 512:
            type = kTypeDts1;
            break;
        case 1024:
            type = kTypeDts2;
            break;
        case 2048:
            type = kTypeDts3;
            break;
        default:
            // No burst type carries it, as in FFmpeg's muxer: most likely a damaged header.
            return skip_frame(Drop::no_burst_type, cursor);
        }
        const std::size_t burst = static_cast<std::size_t>(first.samples) * 4;
        if (first.size == burst)
        {
            // The frame fills its burst exactly: 2048 bytes at 512 samples, as FFmpeg's encoder
            // writes at 1536k. FFmpeg's spdif muxer sends it without the preamble, which would
            // not fit. (Disc cores at 1509 kbit/s are 2012 bytes and fit with it.)
            const std::size_t start = out->size();
            out->resize(start + burst, 0);
            put_words(d.subspan(first.offset, first.size), out->data() + start);
            *cursor += first.size;
            return true;
        }
        if (first.size + 8 > burst)
        {
            return skip_frame(Drop::too_big, cursor);
        }
        write_burst(type, static_cast<std::uint16_t>(first.size * 8),
                    d.subspan(first.offset, first.size), burst, out);
        *cursor += first.size;
        return true;
    }

    *error = "unsupported codec";
    return false;
}

// Each DTS-HD notice is logged once per stream: a frame that cannot go out whole would
// otherwise log at the frame rate.
void Packer::notice_once(DtshdNotice notice)
{
    if (dtshd_notices_given_ & notice)
        return;
    dtshd_notices_given_ |= notice;
    switch (notice)
    {
    case kNoticeNoPeriod:
        add_notice("DTS-HD: dropping frames whose core rate gives no Type IV period");
        break;
    case kNoticeCoreTooBig:
        add_notice("DTS-HD: dropping frames whose core is too big for the Type IV burst");
        break;
    case kNoticeSizeMismatch:
        add_notice("DTS-HD: frame sizes in the headers do not match the packet, ending each such frame at the next core or the packet end");
        break;
    case kNoticeInvalidCore:
        add_notice("DTS-HD: dropping frames with an invalid core header");
        break;
    }
}

namespace
{

// Whether the core header at `at` holds only values ETSI TS 102 114 5.3.1 allows, for the
// fields the packer uses: FSIZE 95 or more, NBLKS 5 or more and, for a normal frame (FTYPE
// 1), a length of 256 to 4096 samples in powers of two, and an SFREQ with a rate.
bool dts_core_valid(Span<std::uint8_t> d, std::size_t at, const Frame &core)
{
    constexpr std::size_t kMinCoreBytes = 96; // FSIZE 95, plus one
    constexpr int kMinSamples = 6 * 32;       // NBLKS 5, plus one
    const bool normal = (d[at + 4] & 0x80) != 0;
    const bool normal_length = core.samples >= 256 && core.samples <= 4096 && (core.samples & (core.samples - 1)) == 0;
    return core.size >= kMinCoreBytes && core.samples >= kMinSamples && (!normal || normal_length) &&
           core.sample_rate != 0;
}

} // namespace

// Notices given before take_notice joins one log line.
void Packer::add_notice(const std::string &notice)
{
    notice_ += (notice_.empty() ? "" : "; ") + notice;
}

// Counts the core-only fallback on by one frame and gives how many bytes of a DTS-HD frame
// of `frame_bytes` go out: all of it, or its core only (never more than the frame). A frame too big for the burst
// starts the fallback: it sends its core, and so do the frames after it for
// kDtshdFallbackSeconds: a decoder may drop a stream whose HD part keeps breaking off
// (FFmpeg's dtshd_fallback_time, default 60).
std::size_t Packer::step_dtshd_fallback(const Frame &core, std::size_t frame_bytes, std::size_t max_frame_bytes)
{
    if (frame_bytes > max_frame_bytes)
    {
        if (dtshd_core_only_ == 0)
            add_notice("DTS-HD frame too big for the Type IV burst, sending the DTS core only for " +
                       std::to_string(kDtshdFallbackSeconds) + " s");
        dtshd_core_only_ = static_cast<long>(core.sample_rate) * kDtshdFallbackSeconds / core.samples;
    }
    if (dtshd_core_only_ == 0)
        return frame_bytes;
    --dtshd_core_only_;
    // A damaged frame cut short at a false sync inside its core holds less than the core.
    return std::min(core.size, frame_bytes);
}

// One whole DTS frame (the core plus the extension substreams after it) as a Type IV
// burst, as FFmpeg's spdif_header_dts4 builds it.
bool Packer::next_dtshd_burst(Span<std::uint8_t> d, const Frame &core, std::size_t *cursor,
                              std::vector<std::uint8_t> *out, bool flush)
{
    // A core header with values the spec calls invalid is damage. FFmpeg's muxer believes it:
    // a bad rate or length can shorten the Type IV period, make the frame too big and start
    // its 60 s core-only fallback. Here the frame goes, and its packet with it (without
    // packets, the next sync word ends it).
    if (!dts_core_valid(d, *cursor, core))
    {
        notice_once(kNoticeInvalidCore);
        *cursor = flush ? d.size() : *cursor + 1;
        return true;
    }
    std::size_t end = *cursor + core.size;
    // A core of a size that is not a whole number of 4-byte words may be padded to one:
    // FFmpeg's parser looks for the EXSS at the core size rounded up to 4 bytes
    // (dca_parser.c, FFALIGN(frame_size, 4)), and so does this, when nothing starts where the
    // core ends.
    const std::size_t padded = *cursor + (core.size + 3) / 4 * 4;
    constexpr std::size_t kSyncCheckBytes = 10; // what is_dts_sync reads
    if (!flush && padded + kSyncCheckBytes > d.size())
        return false; // what follows the padding is not all here yet
    const auto starts_here = [&](std::size_t at) {
        return is_dts_exss_sync(d, at) || is_dts_sync(d, at) || (flush && at == d.size());
    };
    if (!starts_here(end) && starts_here(padded))
        end = padded;
    while (is_dts_exss_sync(d, end))
    {
        const std::size_t substream_bytes = dts_exss_size(d, end);
        if (substream_bytes == 0 || end + substream_bytes > d.size())
        {
            if (!flush)
                return false; // the extension is not all here yet
            break;
        }
        end += substream_bytes;
    }
    if (flush && end != d.size() && !is_dts_sync(d, end))
    {
        // The packet holds the whole frame (packet_ends_frame), and FFmpeg's muxer sends the
        // packet as it is. Header sizes that end anywhere but at the packet's end or at a
        // next core (data fed with no packets) are damage: a 20-bit extension FSIZE can claim
        // 1 MB, which held the frame open, and a wrong core FSIZE cut it short. The frame then
        // runs to the first core sync word after its own, or to the packet's end. Searching
        // from its start, not from where the sizes ended, keeps a cut frame that a demuxer
        // joined to the next one from swallowing it (and from looking too big). Clean frames
        // are never searched: a payload can hold the sync word.
        end = *cursor + kDtsSyncBytes;
        while (end < d.size() && !is_dts_sync(d, end))
            ++end;
        notice_once(kNoticeSizeMismatch);
    }
    // Another extension substream may still come: the frame is whole once the next sync
    // word shows.
    if (!flush && end + kDtsSyncBytes > d.size())
        return false;

    const int subtype = dtshd_subtype(core.sample_rate, core.samples);
    if (subtype < 0)
    {
        // dts_route only picks DTS-HD for a stream rate with a Type IV period, so this is a
        // core whose own rate differs mid-stream. FFmpeg's muxer stops here. Its core as
        // Type I would play at the wrong rate on the 192 kHz port, so the frame is dropped.
        notice_once(kNoticeNoPeriod);
        *cursor = end;
        return true;
    }

    const std::size_t burst_bytes = (static_cast<std::size_t>(kDtshdShortestPeriod) << subtype) * kPortFrameBytes;
    const std::size_t max_frame_bytes = burst_bytes - kBurstHeaderBytes - kDtshdFrameAt;
    // The bytes the frame really holds, never more than its packet, so a damaged size in a
    // header can't make it too big. A damaged rate or length that is still valid can (512
    // samples read as 256 halve the burst), and then starts the fallback as in FFmpeg.
    const std::size_t frame_bytes = step_dtshd_fallback(core, end - *cursor, max_frame_bytes);
    if (frame_bytes > max_frame_bytes)
    {
        // FFmpeg's muxer fails this packet ("bitrate is too high").
        notice_once(kNoticeCoreTooBig);
        *cursor = end;
        return true;
    }
    const std::size_t payload_bytes = kDtshdFrameAt + frame_bytes;

    std::vector<std::uint8_t> payload(payload_bytes);
    std::memcpy(payload.data(), kDtshdStartCode, sizeof(kDtshdStartCode));
    payload[kDtshdSizeAt] = static_cast<std::uint8_t>(frame_bytes >> 8);
    payload[kDtshdSizeAt + 1] = static_cast<std::uint8_t>(frame_bytes & 0xFF);
    std::memcpy(payload.data() + kDtshdFrameAt, d.data() + *cursor, frame_bytes);
    // Pd counts bytes.
    const std::size_t aligned = (payload_bytes + kDtshdPdRemainder + kDtshdPdAlign - 1) / kDtshdPdAlign * kDtshdPdAlign;
    const std::uint16_t pd = static_cast<std::uint16_t>(aligned - kDtshdPdRemainder);
    write_burst(static_cast<std::uint16_t>(kTypeDtshd | (subtype << 8)), pd,
                Span<std::uint8_t>(payload), burst_bytes, out);
    *cursor = end;
    return true;
}

void Packer::take_bursts(std::vector<std::uint8_t> *stream, std::vector<std::uint8_t> *out)
{
    std::size_t cursor = 0;
    while (stream->size() - cursor >= kFrameHeaderBytes)
    {
        Span<std::uint8_t> rest(stream->data() + cursor, stream->size() - cursor);
        std::size_t consumed = 0; // bytes of `rest` that next_burst used or skipped
        const char *error = nullptr;
        // The packet just added ends a DTS-HD frame (packet_ends_frame): flush it there.
        if (next_burst(rest, &consumed, out, &error, packet_ends_frame(codec_)))
        {
            cursor += consumed;
        }
        else if (error)
        {
            // The frame next_burst stopped at can never go out: step past its first byte, so
            // the next call finds the next frame instead of failing on this one until the next seek.
            drop(Drop::unsendable, error);
            cursor += consumed + 1;
        }
        else
        {
            break; // the rest of a frame has not arrived
        }
    }
    if (stream->size() - cursor > kMaxHeldBytes)
    {
        drop(Drop::no_frame);
        cursor = stream->size();
    }
    stream->erase(stream->begin(), stream->begin() + static_cast<std::ptrdiff_t>(cursor));
}

bool Packer::skip_frame(Drop why, std::size_t *cursor)
{
    return skip_to(why, cursor, *cursor + 1);
}

bool Packer::skip_to(Drop why, std::size_t *cursor, std::size_t at)
{
    drop(why);
    *cursor = at;
    return true;
}

void Packer::drop(Drop why, const char *error)
{
    const unsigned bit = 1u << static_cast<unsigned>(why);
    if (drops_logged_ & bit)
        return;
    drops_logged_ |= bit;
    std::string line = codec_name(codec_);
    switch (why)
    {
    case Drop::unsendable:
        line += std::string(": dropped a frame it cannot send (") + (error ? error : "no reason given") + ")";
        break;
    case Drop::no_frame:
        line += ": dropped more than " + std::to_string(kMaxHeldBytes / 1024) + " KiB that held no frame";
        break;
    case Drop::damaged_header:
        line += ": dropped frames with a damaged header";
        break;
    case Drop::too_big:
        line += ": dropped frames too big for their burst";
        break;
    case Drop::no_burst_type:
        line += ": dropped frames of a sample count no burst type carries";
        break;
    case Drop::cut_off:
        line += ": dropped frames cut off before a whole burst";
        break;
    }
    line += " (logged once per stream)";
    add_notice(line);
}

} // namespace pt
