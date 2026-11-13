// evo/audio/iec61937.cpp - IEC 61937 packing for AC-3, E-AC-3, DTS, and AAC.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "evo/audio/iec61937.hpp"
#include <algorithm>
#include <cstring>
#include <iterator>

namespace pt
{

namespace
{

constexpr std::uint16_t kPa = 0xF872;
constexpr std::uint16_t kPb = 0x4E1F;
// IEC 61937 data types
constexpr std::uint16_t kTypeAc3  = 0x01;
constexpr std::uint16_t kTypeDts1 = 0x0B; // 512 samples per frame
constexpr std::uint16_t kTypeDts2 = 0x0C; // 1024
constexpr std::uint16_t kTypeDts3 = 0x0D; // 2048
constexpr std::uint16_t kTypeEac3 = 0x15;
constexpr std::uint16_t kTypeAac  = 0x07; // MPEG-2/4 AAC, 1024 samples per frame
constexpr std::uint16_t kTypeTruehd = 0x16; // Dolby TrueHD, as MAT

// A TrueHD access unit is 1/1200 s of audio: 40 samples at 48 kHz, twice that
// for each doubling of the rate. A major sync (sync word at byte 4, then a
// format byte) gives the rate bits; FFmpeg's muxer needs at least the bytes
// up to the second rate byte in every unit.
constexpr std::uint8_t kTruehdMajorSync[3] = {0xF8, 0x72, 0x6F};
constexpr std::size_t kMajorSyncPos = 4;
constexpr std::size_t kMajorSyncFormatPos = kMajorSyncPos + sizeof(kTruehdMajorSync);
constexpr std::uint8_t kMajorSyncTruehd = 0xBA;
constexpr std::uint8_t kMajorSyncMlp = 0xBB;
constexpr std::size_t kTruehdRatePos = 8; // rate bits: high nibble, low 2 bits used
constexpr std::size_t kMlpRatePos = 9;
constexpr std::size_t kMajorSyncHeaderBytes = kMlpRatePos + 1;
constexpr int kTruehdBaseSamples = 40;

// One MAT frame holds 24 TrueHD time slots (so 20 ms) and goes out in one
// kMatBurstBytes burst on the 768 kHz port.
constexpr std::size_t kMatFrameSize = 61424;
constexpr std::size_t kMatMiddleCodePos = 30708;
constexpr std::size_t kMatSlots = 24;
constexpr std::size_t kMatSlotBytes = kMatBurstBytes / kMatSlots; // one 1/1200 s slot
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
    else if (codec == Codec::dts)
    {
        if (!is_dts_sync(d, at))
            return f;
        const int nblks = ((d[at + 4] & 1) << 6) | (d[at + 5] >> 2);
        const int fsize = (((d[at + 5] & 3) << 12) | (d[at + 6] << 4) | (d[at + 7] >> 4)) + 1;
        f.samples = (nblks + 1) * 32;
        f.size = static_cast<std::size_t>(fsize);
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
        if (at + f.size > d.size())
        {
            f.size = 0; // the rest of the unit has not arrived
            return f;
        }
        if (f.size < kMajorSyncHeaderBytes)
        {
            f.invalid = true;
        }
        else if (std::equal(std::begin(kTruehdMajorSync), std::end(kTruehdMajorSync),
                            d.data() + at + kMajorSyncPos))
        {
            const std::uint8_t format = d[at + kMajorSyncFormatPos];
            if (format == kMajorSyncTruehd || format == kMajorSyncMlp)
            {
                const std::uint8_t rate = d[at + (format == kMajorSyncTruehd ? kTruehdRatePos : kMlpRatePos)];
                f.major_sync = true;
                f.samples = kTruehdBaseSamples << ((rate >> 4) & 3);
            }
            else
            {
                f.invalid = true;
            }
        }
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

// Places one TrueHD access unit in the MAT frame the way FFmpeg's spdif muxer
// does (libavformat/spdifenc.c, spdif_header_truehd): each unit starts at its
// time slot from input_timing, kMatSlotBytes per slot of the 768 kHz port,
// zero padding fills the slot, and a unit that reaches a MAT code continues
// after it, into the next frame if it was the end code. The codes and the gap
// between frames count as padding. Returns true once a frame is complete and
// its burst is in `out`.
bool Packer::add_truehd_unit(const std::uint8_t *unit, std::size_t size,
                             std::vector<std::uint8_t> *out)
{
    struct MatCode
    {
        std::size_t pos;
        const std::uint8_t *code;
        std::size_t len;
    };
    static const MatCode kCodes[] = {
        {0, kMatStartCode, sizeof(kMatStartCode)},
        {kMatMiddleCodePos, kMatMiddleCode, sizeof(kMatMiddleCode)},
        {kMatFrameSize - sizeof(kMatEndCode), kMatEndCode, sizeof(kMatEndCode)},
    };
    constexpr std::size_t kCodeCount = std::size(kCodes);

    Truehd &t = truehd_;
    if (t.mat.size() != kMatFrameSize)
        t.mat.assign(kMatFrameSize, 0);

    const std::uint16_t input_timing = static_cast<std::uint16_t>((unit[2] << 8) | unit[3]);
    std::size_t padding = 0;
    if (t.prev_size != 0)
    {
        const std::uint16_t delta_samples = static_cast<std::uint16_t>(input_timing - t.prev_time);
        const std::size_t delta_bytes =
            delta_samples * kMatSlotBytes / static_cast<std::size_t>(t.samples_per_unit);
        // Timing that would go backwards or skip half a frame is ignored, as FFmpeg does.
        if (delta_bytes > t.prev_size && delta_bytes - t.prev_size < kMatFrameSize / 2)
            padding = delta_bytes - t.prev_size;
    }

    std::size_t next = 0;
    while (next < kCodeCount && t.mat_filled > kCodes[next].pos)
        ++next;

    std::size_t total = size; // bytes this unit takes on the port, codes it does not pad included
    const std::uint8_t *data = unit;
    std::size_t remaining = size;
    bool complete = false;
    while (padding > 0 || remaining > 0 || kCodes[next].pos == t.mat_filled)
    {
        if (kCodes[next].pos == t.mat_filled)
        {
            std::memcpy(t.mat.data() + t.mat_filled, kCodes[next].code, kCodes[next].len);
            t.mat_filled += kCodes[next].len;
            std::size_t code_left = kCodes[next].len;
            if (++next == kCodeCount)
            {
                write_burst(kTypeTruehd, static_cast<std::uint16_t>(kMatFrameSize),
                            Span<std::uint8_t>(t.mat), kMatBurstBytes, out);
                complete = true;
                next = 0;
                t.mat_filled = 0;
                code_left += kMatBurstBytes - kMatFrameSize; // the gap before the next frame
            }
            const std::size_t as_padding = std::min(padding, code_left);
            padding -= as_padding;
            total += code_left - as_padding;
        }
        const std::size_t room = kCodes[next].pos - t.mat_filled;
        if (padding > 0)
        {
            const std::size_t n = std::min(room, padding);
            std::memset(t.mat.data() + t.mat_filled, 0, n);
            t.mat_filled += n;
            padding -= n;
            if (padding > 0)
                continue; // a code comes first
        }
        if (remaining > 0)
        {
            const std::size_t n = std::min(kCodes[next].pos - t.mat_filled, remaining);
            std::memcpy(t.mat.data() + t.mat_filled, data, n);
            t.mat_filled += n;
            data += n;
            remaining -= n;
        }
    }

    t.prev_size = total;
    t.prev_time = input_timing;
    return complete;
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
        // One access unit at a time, until one completes a MAT frame.
        std::size_t cur = *cursor;
        bool emitted = false;
        while (!emitted)
        {
            const Frame f = parse_frame(codec_, d, cur);
            if (f.size == 0)
                break;
            const std::uint8_t *unit = d.data() + cur;
            cur += f.size;
            if (f.major_sync)
                truehd_.samples_per_unit = f.samples;
            // FFmpeg's muxer rejects a unit it cannot place (shorter than kMajorSyncHeaderBytes, a
            // major sync of unknown format, or before the first major sync, so
            // without a sample rate) with AVERROR_INVALIDDATA; here it is skipped
            // and the stream goes on.
            if (f.invalid || truehd_.samples_per_unit == 0)
                continue;
            emitted = add_truehd_unit(unit, f.size, out);
        }
        if (cur == *cursor)
            return false;
        *cursor = cur;
        return true;
    }

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

} // namespace pt
