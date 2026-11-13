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
// DTS core sample rates by the frame header's SFREQ field (ETSI TS 102 114 table 5-5).
constexpr int kDtsSampleRates[16] = {0,     8000,  16000, 32000, 0,     0,     11025, 22050,
                                     44100, 0,     0,     12000, 24000, 48000, 96000, 192000};

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

bool is_ac3_sync(Span<std::uint8_t> d, std::size_t at)
{
    return at + 6 <= d.size() && d[at] == 0x0B && d[at + 1] == 0x77;
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
        const int bsid = d[at + 5] >> 3;
        if (bsid <= 10)
        {
            const int fscod = d[at + 4] >> 6;
            const int frmsizecod = d[at + 4] & 0x3F;
            if (fscod > 2 || frmsizecod > 37)
                return f;
            f.size = static_cast<std::size_t>(kAc3Words[frmsizecod][fscod]) * 2;
            f.samples = 1536;
            f.blocks = 6;
            f.bsmod = d[at + 5] & 7;
        }
        else
        {
            const int strmtyp = d[at + 2] >> 6;
            const int frmsiz = ((d[at + 2] & 7) << 8) | d[at + 3];
            const int fscod = d[at + 4] >> 6;
            const int numblkscod = (d[at + 4] >> 4) & 3;
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
        const int nblks = ((d[at + 4] & 1) << 6) | (d[at + 5] >> 2);
        const int fsize = (((d[at + 5] & 3) << 12) | (d[at + 6] << 4) | (d[at + 7] >> 4)) + 1;
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
    // payload bytes a, b become the sample 0xaabb
    for (std::size_t i = 0; i < payload.size(); i += 2)
    {
        const std::uint8_t hi = payload[i];
        const std::uint8_t lo = i + 1 < payload.size() ? payload[i + 1] : 0;
        put(4 + i / 2, static_cast<std::uint16_t>((hi << 8) | lo));
    }
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
            *error = "AC-3 frame too large for one burst";
            return false;
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
        int blocks = 0;
        bool incomplete_tail = false;
        for (;;)
        {
            const Frame f = parse_frame(codec_, d, end);
            if (f.size == 0)
            {
                incomplete_tail = true;
                break;
            }
            if (!f.dependent && blocks >= 6)
                break; // the next independent frame starts the next burst
            if (!f.dependent)
                blocks += f.blocks;
            end += f.size;
        }

        if (blocks < 6 && incomplete_tail && !flush)
        {
            // Wait for remaining blocks of this 6-block burst
            return false;
        }

        const std::size_t length = end - *cursor;
        if (length + 8 > kBurst)
        {
            *error = "E-AC-3 frames too large for one burst";
            return false;
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
            *error = "DTS frame length not 512, 1024 or 2048 samples";
            return false;
        }
        const std::size_t burst = static_cast<std::size_t>(first.samples) * 4;
        if (first.size + 8 > burst)
        {
            *error = "DTS frame too large for one burst (bitrate too high for type I-III)";
            return false;
        }
        write_burst(type, static_cast<std::uint16_t>(first.size * 8),
                    d.subspan(first.offset, first.size), burst, out);
        *cursor += first.size;
        return true;
    }

    *error = "unsupported codec";
    return false;
}

// A DTS-HD frame that cannot go out is dropped, with one log line per reason and stream:
// holding it would stall the stream behind it.
void Packer::drop_dtshd_frame(DtshdDrop why)
{
    if (dtshd_drops_logged_ & why)
        return;
    dtshd_drops_logged_ |= why;
    add_notice(why == kDropNoPeriod
                   ? "DTS-HD: dropping frames whose core rate gives no Type IV period"
                   : "DTS-HD: dropping frames whose core is too big for the Type IV burst");
}

// Notices given before take_notice joins one log line.
void Packer::add_notice(const std::string &notice)
{
    notice_ += (notice_.empty() ? "" : "; ") + notice;
}

// One whole DTS frame (the core plus the extension substreams after it) as a Type IV
// burst, as FFmpeg's spdif_header_dts4 builds it.
bool Packer::next_dtshd_burst(Span<std::uint8_t> d, const Frame &core, std::size_t *cursor,
                              std::vector<std::uint8_t> *out, bool flush)
{
    std::size_t end = *cursor + core.size;
    while (is_dts_exss_sync(d, end))
    {
        const std::size_t substream_bytes = dts_exss_size(d, end);
        if (substream_bytes == 0 || end + substream_bytes > d.size())
            return false; // the extension is not all here yet
        end += substream_bytes;
    }
    // Another extension substream may still come: the frame is whole once the next sync
    // word shows, or at `flush` (the end of its packet, see packet_ends_frame).
    if (end + kDtsSyncBytes > d.size() && !flush)
        return false;

    const int subtype = dtshd_subtype(core.sample_rate, core.samples);
    if (subtype < 0)
    {
        // dts_route only picks DTS-HD for a stream rate with a Type IV period, so this is a
        // core whose own rate differs mid-stream. FFmpeg's muxer stops here. Its core as
        // Type I would play at the wrong rate on the 192 kHz port, so the frame is dropped.
        drop_dtshd_frame(kDropNoPeriod);
        *cursor = end;
        return true;
    }

    const std::size_t burst_bytes = (static_cast<std::size_t>(kDtshdShortestPeriod) << subtype) * kPortFrameBytes;
    const std::size_t max_frame_bytes = burst_bytes - kBurstHeaderBytes - kDtshdFrameAt;
    std::size_t frame_bytes = end - *cursor;
    // A frame too big for the burst sends its core, and so do the frames after it for
    // kDtshdFallbackSeconds: a decoder may drop a stream whose HD part keeps breaking off
    // (FFmpeg's dtshd_fallback_time, default 60).
    if (frame_bytes > max_frame_bytes)
    {
        if (dtshd_core_only_ == 0)
            add_notice("DTS-HD frame too big for the Type IV burst, sending the DTS core only for " +
                       std::to_string(kDtshdFallbackSeconds) + " s");
        dtshd_core_only_ = static_cast<long>(core.sample_rate) * kDtshdFallbackSeconds / core.samples;
    }
    if (dtshd_core_only_ > 0)
    {
        frame_bytes = core.size;
        --dtshd_core_only_;
        if (frame_bytes > max_frame_bytes)
        {
            // FFmpeg's muxer fails this packet ("bitrate is too high").
            drop_dtshd_frame(kDropCoreTooBig);
            *cursor = end;
            return true;
        }
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

} // namespace pt
