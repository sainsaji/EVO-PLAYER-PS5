// evo/audio/iec61937.hpp - IEC 61937 packing for AC-3, E-AC-3, DTS, and AAC.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pt
{

template <typename T>
struct Span
{
    const T *data_ = nullptr;
    std::size_t size_ = 0;

    constexpr Span() = default;
    constexpr Span(const T *data, std::size_t size) : data_(data), size_(size) {}
    constexpr Span(const std::vector<T> &v) : data_(v.data()), size_(v.size()) {}

    constexpr const T *data() const { return data_; }
    constexpr std::size_t size() const { return size_; }
    constexpr bool empty() const { return size_ == 0; }
    constexpr const T &operator[](std::size_t i) const { return data_[i]; }

    constexpr Span<T> subspan(std::size_t offset, std::size_t count = static_cast<std::size_t>(-1)) const
    {
        if (offset >= size_) return Span<T>(data_ + size_, 0);
        std::size_t rem = size_ - offset;
        return Span<T>(data_ + offset, count < rem ? count : rem);
    }
};

enum class Codec : std::uint8_t
{
    ac3,
    eac3,
    dts,
    aac, // ADTS (MPEG-2/4 AAC with a header on every frame)
    truehd, // Dolby TrueHD access units, carried as MAT
    dtshd,  // DTS with extension substreams (HRA, MA), whole frames as Type IV
    unknown,
};

const char *codec_name(Codec codec);

// Guesses the codec from the first bytes of an elementary stream.
Codec detect(Span<std::uint8_t> data);

// What the console needs to know to carry one codec.
struct Carrier
{
    int mode = -1;            // sceAudioOutExOpen / ExConfigureOutput mode
    int grain_frames = 0;     // stereo frames per sceAudioOutOutput call
    int sample_rate = 0;      // the port's rate, 48 kHz, 192 kHz or 768 kHz
    bool sys = false;         // open with sceAudioOutSysOpen / SysConfigureOutput (its own mode table)
    int frame_bytes = 4;      // bytes per port frame (stereo S16 = 4, 8-channel S16 = 16)
    // Port frames per second as the port really consumes them. The 768 kHz port is a
    // stereo-rate label: its 16-byte frames are taken at 192 kHz (a 16384-byte grain blocks 5.3 ms).
    int frame_rate() const { return frame_bytes == 16 && sample_rate == 768000 ? 192000 : sample_rate; }
};
Carrier carrier_for(Codec codec);

// One frame found in a stream.
struct Frame
{
    std::size_t offset = 0;
    std::size_t size = 0;
    int samples = 0;        // PCM samples per channel it decodes to (DTS, AC-3)
    int blocks = 0;         // E-AC-3: audio blocks (6 make one burst)
    bool dependent = false; // E-AC-3 dependent substream (7.1 extension)
    int bsmod = 0;          // AC-3 bitstream mode, goes into the burst header
    int sample_rate = 0;    // DTS: the core's rate from its header, 0 for a reserved code
};

// Size and kind of the frame at `at`, or size 0 if there is no valid frame.
Frame parse_frame(Codec codec, Span<std::uint8_t> data, std::size_t at);

// The IEC 61937-9 Type IV subtype of a DTS frame of `samples` at `sample_rate` on the
// 192 kHz port (its burst lasts 512 << subtype port frames), or -1 if none fits. Only a
// 48 kHz-family rate has one: 44.1 kHz and its multiples never do.
int dtshd_subtype(int sample_rate, int samples);

// How a DTS stream goes out, from its FFmpeg profile and rate and whether the sink lists
// DTS-HD (CEA-861 coding 11): whole frames as DTS-HD for an HRA, MA or MA X stream at a
// 48 kHz-family rate on such a sink, else the DTS core.
Codec dts_route(int profile, int sample_rate, bool sink_lists_dtshd);

// Whether the end of a demuxed packet ends the frame in it, so next_burst can flush there
// instead of waiting for the next sync word. Only for DTS-HD: FFmpeg's demuxers hand over
// each DTS-HD frame whole, core and extension substreams in one packet (its DTS parser
// joins them, and its spdif muxer counts on it). Waiting held every frame back by one, and
// the last frame of a file never went out.
bool packet_ends_frame(Codec codec);

// Splits an elementary stream into bursts, each exactly as long as the
// audio it holds, as little-endian S16 stereo samples ready for the port.
class Packer
{
  public:
    explicit Packer(Codec codec) : codec_(codec)
    {
    }

    // A new stream: also ends a DTS-HD core-only fallback.
    void set_codec(Codec codec)
    {
        codec_ = codec;
        dtshd_core_only_ = 0;
        dtshd_drops_logged_ = 0;
        notice_.clear();
    }
    Codec codec() const { return codec_; }

    // A change worth one log line (DTS-HD falling back to its core or dropping frames),
    // once, or "".
    std::string take_notice()
    {
        std::string notice;
        notice.swap(notice_);
        return notice;
    }

    // Appends the next burst built from `data` starting at *cursor to `out`
    // and advances *cursor. Returns false at the end of data or on an incomplete/
    // uncarriable frame (then *error says why if fatal).
    bool next_burst(Span<std::uint8_t> data, std::size_t *cursor,
                    std::vector<std::uint8_t> *out, const char **error,
                    bool flush = false);

  private:
    bool next_dtshd_burst(Span<std::uint8_t> data, const Frame &core, std::size_t *cursor,
                          std::vector<std::uint8_t> *out, bool flush);
    // Why a DTS-HD frame is dropped; each reason is logged once per stream.
    enum DtshdDrop : unsigned { kDropNoPeriod = 1, kDropCoreTooBig = 2 };
    void drop_dtshd_frame(DtshdDrop why);
    void add_notice(const std::string &notice);

    Codec codec_;
    long dtshd_core_only_ = 0; // DTS-HD frames left to send as core only, after one too big
    unsigned dtshd_drops_logged_ = 0; // DtshdDrop reasons already logged for this stream
    std::string notice_;
};

// Writes one burst: header words Pa Pb Pc Pd, the payload as big-endian
// words, zero padding to burst_bytes.
void write_burst(std::uint16_t data_type, std::uint16_t length_code,
                 Span<std::uint8_t> payload, std::size_t burst_bytes,
                 std::vector<std::uint8_t> *out);

} // namespace pt
