// evo_pt.cpp - Implementation of PS5 HDMI Audio Bitstream Passthrough
// SPDX-License-Identifier: GPL-3.0-or-later

#include "evo_pt.h"
#include "evo/audio/iec61937.hpp"
#include "evo/audio/bitstream.hpp"
#include "evo_boot_log.h"

#include <libavcodec/codec_id.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <pthread.h>
#include <thread>
#include <vector>

extern "C"
{
    int sceAudioOutOutput(int handle, const void *samples);
    int sceAudioOutExOpen(int user, int mode);                                        // 6X6dp+07h4U
    int sceAudioOutExClose(int handle);                                               // 0TfjSulCV2A
    int sceAudioOutExConfigureOutput(int zero, unsigned flags, int mode, int target,
                                     std::uint64_t opt);                              // VcE+gXSwFXI
}

namespace
{

constexpr int kSystemUser = 0xFF;
constexpr int kTargetHdmi = 1;     // citroncore target: 1 (HDMI)
constexpr int kModeDefault = 0xFF; // normal PCM output restore code

// A receiver follows HDMI format changes slowly. Switching formats back to
// back left it stuck on the previous one (a PCM stream labelled AAC, and AAC
// silent until the receiver was restarted). So a new switch waits out a settle
// time after the last reset, the stream is bracketed by IEC 61937 null bursts,
// and the reset is spaced from the port close.
constexpr int kLeadInMs = 400;      // null bursts after the switch, before the first frame
constexpr int kLeadOutMs = 500;     // null bursts after the last frame, before the port closes
constexpr int kAfterCloseMs = 250;  // between closing the port and resetting HDMI
constexpr int kAfterResetMs = 400;  // after the reset, before returning
constexpr int kBeforeOpenMs = 1000; // minimum gap since the last reset before a new switch
std::int64_t g_last_reset_ms = -100000;

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void sleep_ms(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// IEC 61937 null data (data type 0), one port grain per call, for `ms` of audio.
void send_null_bursts(int handle, const pt::Carrier &carrier, int ms)
{
    if (handle < 1 || carrier.grain_frames <= 0 || carrier.sample_rate <= 0)
        return;
    std::vector<uint8_t> grain(static_cast<size_t>(carrier.grain_frames) * 4, 0);
    grain[0] = 0x72; // Pa 0xF872, little-endian
    grain[1] = 0xF8;
    grain[2] = 0x1F; // Pb 0x4E1F
    grain[3] = 0x4E;
    const int grains = ms * carrier.sample_rate / 1000 / carrier.grain_frames;
    for (int i = 0; i < grains; ++i)
        sceAudioOutOutput(handle, grain.data());
}

int                     g_pt_handle = -1;
volatile int            g_pt_active = 0;
pt::Codec               g_pt_codec = pt::Codec::unknown;
pt::Carrier             g_pt_carrier{};
pt::Packer              g_pt_packer(pt::Codec::unknown);
std::vector<uint8_t>    g_pt_stream_buf;
std::deque<std::vector<uint8_t>> g_pt_grain_queue;
pthread_mutex_t         g_pt_mutex = PTHREAD_MUTEX_INITIALIZER;

pt::Codec codec_from_av(int av_codec_id)
{
    switch (av_codec_id)
    {
    case AV_CODEC_ID_AC3:
        return pt::Codec::ac3;
    case AV_CODEC_ID_EAC3:
        return pt::Codec::eac3;
    case AV_CODEC_ID_DTS:
        return pt::Codec::dts;
    case AV_CODEC_ID_AAC:
    case AV_CODEC_ID_AAC_LATM:
        return pt::Codec::aac;
    default:
        return pt::Codec::unknown;
    }
}

void make_adts_header(uint8_t *header, int sample_rate, int channels, size_t aac_frame_len)
{
    int srate_idx = 4; // default 44100
    static const int srates[] = {96000, 88200, 64000, 48000, 44100, 32000,
                                 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    for (int i = 0; i < 13; ++i)
    {
        if (srates[i] == sample_rate)
        {
            srate_idx = i;
            break;
        }
    }
    const size_t frame_len = aac_frame_len + 7;
    const int profile = 1; // AAC LC (audio object type 2 - 1)

    header[0] = 0xFF;
    header[1] = 0xF1; // MPEG-4, Layer 0, no CRC (protection absent = 1)
    header[2] = static_cast<uint8_t>(((profile & 3) << 6) | ((srate_idx & 0x0F) << 2) | ((channels >> 2) & 1));
    header[3] = static_cast<uint8_t>(((channels & 3) << 6) | ((frame_len >> 11) & 3));
    header[4] = static_cast<uint8_t>((frame_len >> 3) & 0xFF);
    header[5] = static_cast<uint8_t>(((frame_len & 7) << 5) | 0x1F);
    header[6] = 0xFC;
}

} // namespace

extern "C"
{

int evo_pt_probe_sink_support(int av_codec_id)
{
    const pt::Codec codec = codec_from_av(av_codec_id);
    if (codec == pt::Codec::unknown)
        return 0;

    const int coding = pt::coding_for(codec);
    if (coding == 0)
        return 0;

    const pt::Sink sink = pt::query_sink();
    evo_boot_log("[PT] Query sink: ok=%d rc=0x%08x name='%s' formats=%zu",
                 sink.ok, static_cast<unsigned>(sink.rc), sink.name.c_str(), sink.formats.size());

    if (sink.ok && !sink.formats.empty())
    {
        const bool sup = sink.supports(coding);
        evo_boot_log("[PT] Sink format support for %s (coding %d): %s",
                     pt::codec_name(codec), coding, sup ? "YES" : "NO");
        return sup ? 1 : 0;
    }

    // If HDMI monitor info returned no short audio descriptors (e.g. basic splitter or capture card),
    // allow passthrough when requested
    evo_boot_log("[PT] Sink descriptor list empty, allowing passthrough attempt for %s", pt::codec_name(codec));
    return 1;
}

int evo_pt_open(int av_codec_id)
{
    const pt::Codec codec = codec_from_av(av_codec_id);
    if (codec == pt::Codec::unknown)
    {
        evo_boot_log("[PT] evo_pt_open: unknown/unsupported codec %d", av_codec_id);
        return -1;
    }

    const pt::Carrier carrier = pt::carrier_for(codec);
    if (carrier.mode < 0)
    {
        evo_boot_log("[PT] evo_pt_open: no carrier mode for codec %s", pt::codec_name(codec));
        return -1;
    }

    evo_boot_log("[PT] Opening bitstream port: mode=%d grain=%d rate=%d (%s)",
                 carrier.mode, carrier.grain_frames, carrier.sample_rate, pt::codec_name(codec));

    const std::int64_t since_reset = now_ms() - g_last_reset_ms;
    if (since_reset < kBeforeOpenMs)
        sleep_ms(static_cast<int>(kBeforeOpenMs - since_reset));

    // 1. Open the bitstream port first (citroncore sequence)
    const int handle = sceAudioOutExOpen(kSystemUser, carrier.mode);
    evo_boot_log("[PT] sceAudioOutExOpen(0xFF, %d) -> %d (0x%08x)",
                 carrier.mode, handle, static_cast<unsigned>(handle));
    if (handle < 1)
    {
        evo_boot_log("[PT] sceAudioOutExOpen failed: %d", handle);
        return handle;
    }

    // 2. Switch HDMI audio to bitstream mode for this format
    const int cfg_rc = sceAudioOutExConfigureOutput(0, 0, carrier.mode, kTargetHdmi, 0);
    evo_boot_log("[PT] sceAudioOutExConfigureOutput(mode=%d, target=%d) -> 0x%08x",
                 carrier.mode, kTargetHdmi, static_cast<unsigned>(cfg_rc));
    if (cfg_rc < 0)
    {
        evo_boot_log("[PT] sceAudioOutExConfigureOutput failed (%d), closing port and reverting", cfg_rc);
        sceAudioOutExClose(handle);
        sceAudioOutExConfigureOutput(0, 0, kModeDefault, kModeDefault, 0);
        return cfg_rc;
    }

    send_null_bursts(handle, carrier, kLeadInMs);

    pthread_mutex_lock(&g_pt_mutex);
    g_pt_handle = handle;
    g_pt_codec = codec;
    g_pt_carrier = carrier;
    g_pt_packer.set_codec(codec);
    g_pt_stream_buf.clear();
    g_pt_grain_queue.clear();
    g_pt_active = 1;
    pthread_mutex_unlock(&g_pt_mutex);

    evo_boot_log("[PT] Bitstream passthrough started successfully on handle %d", handle);
    return handle;
}

void evo_pt_close(int handle)
{
    evo_boot_log("[PT] Closing bitstream passthrough (handle=%d)", handle);

    if (handle >= 1)
    {
        // 3. Tell the receiver the stream is ending, then drain pending bursts
        send_null_bursts(handle, g_pt_carrier, kLeadOutMs);
        sceAudioOutOutput(handle, nullptr);
        // 4. Close bitstream port
        const int close_rc = sceAudioOutExClose(handle);
        evo_boot_log("[PT] sceAudioOutExClose(%d) -> 0x%08x", handle, static_cast<unsigned>(close_rc));
        sleep_ms(kAfterCloseMs);
    }

    // 5. Restore HDMI to normal PCM output
    const int rst_rc = sceAudioOutExConfigureOutput(0, 0, kModeDefault, kModeDefault, 0);
    evo_boot_log("[PT] sceAudioOutExConfigureOutput(0xFF, 0xFF) -> 0x%08x", static_cast<unsigned>(rst_rc));
    g_last_reset_ms = now_ms();
    sleep_ms(kAfterResetMs);

    pthread_mutex_lock(&g_pt_mutex);
    g_pt_handle = -1;
    g_pt_active = 0;
    g_pt_codec = pt::Codec::unknown;
    g_pt_carrier = pt::Carrier{};
    g_pt_stream_buf.clear();
    g_pt_grain_queue.clear();
    pthread_mutex_unlock(&g_pt_mutex);
}

void evo_pt_reset(void)
{
    pthread_mutex_lock(&g_pt_mutex);
    g_pt_stream_buf.clear();
    g_pt_grain_queue.clear();
    pthread_mutex_unlock(&g_pt_mutex);
    evo_boot_log("[PT] Queues flushed (seek)");
}

int evo_pt_push_packet(const uint8_t *data, size_t size, int sample_rate, int channels)
{
    if (!g_pt_active || !data || size == 0)
        return 0;

    pthread_mutex_lock(&g_pt_mutex);

    // If AAC, check if frame has ADTS header; if not, wrap it
    if (g_pt_codec == pt::Codec::aac)
    {
        const bool has_adts = (size >= 7 && data[0] == 0xFF && (data[1] & 0xF6) == 0xF0);
        if (!has_adts)
        {
            uint8_t adts[7];
            make_adts_header(adts, sample_rate, channels, size);
            g_pt_stream_buf.insert(g_pt_stream_buf.end(), adts, adts + 7);
        }
    }

    g_pt_stream_buf.insert(g_pt_stream_buf.end(), data, data + size);

    const size_t grain_bytes = static_cast<size_t>(g_pt_carrier.grain_frames) * 4;
    size_t cursor = 0;
    const char *error = nullptr;
    std::vector<uint8_t> burst;

    while (g_pt_stream_buf.size() - cursor >= 8)
    {
        pt::Span<uint8_t> span(g_pt_stream_buf.data() + cursor, g_pt_stream_buf.size() - cursor);
        size_t consumed = 0;
        burst.clear();

        if (!g_pt_packer.next_burst(span, &consumed, &burst, &error, false))
        {
            break;
        }

        cursor += consumed;

        // Split the generated IEC 61937 burst into port grains
        for (size_t off = 0; off < burst.size(); off += grain_bytes)
        {
            const size_t take = std::min(grain_bytes, burst.size() - off);
            std::vector<uint8_t> grain(burst.begin() + off, burst.begin() + off + take);
            if (grain.size() < grain_bytes)
                grain.resize(grain_bytes, 0);
            g_pt_grain_queue.push_back(std::move(grain));
        }
    }

    if (cursor > 0)
    {
        g_pt_stream_buf.erase(g_pt_stream_buf.begin(), g_pt_stream_buf.begin() + cursor);
    }

    pthread_mutex_unlock(&g_pt_mutex);
    return 1;
}

int evo_pt_pop_grain(uint8_t *out_grain, size_t grain_bytes)
{
    if (!g_pt_active || !out_grain)
        return 0;

    pthread_mutex_lock(&g_pt_mutex);
    if (g_pt_grain_queue.empty())
    {
        pthread_mutex_unlock(&g_pt_mutex);
        return 0;
    }

    const std::vector<uint8_t> &grain = g_pt_grain_queue.front();
    const size_t to_copy = std::min(grain_bytes, grain.size());
    std::memcpy(out_grain, grain.data(), to_copy);
    if (to_copy < grain_bytes)
        std::memset(out_grain + to_copy, 0, grain_bytes - to_copy);

    g_pt_grain_queue.pop_front();
    pthread_mutex_unlock(&g_pt_mutex);
    return 1;
}

size_t evo_pt_queued_grains(void)
{
    pthread_mutex_lock(&g_pt_mutex);
    const size_t sz = g_pt_grain_queue.size();
    pthread_mutex_unlock(&g_pt_mutex);
    return sz;
}

int evo_pt_is_active(void)
{
    return g_pt_active;
}

size_t evo_pt_grain_bytes(void)
{
    return static_cast<size_t>(g_pt_carrier.grain_frames) * 4;
}

int evo_pt_grain_frames(void)
{
    return g_pt_carrier.grain_frames;
}

int evo_pt_sample_rate(void)
{
    return g_pt_carrier.sample_rate > 0 ? g_pt_carrier.sample_rate : 48000;
}

const char *evo_pt_active_codec_name(void)
{
    return pt::codec_name(g_pt_codec);
}

void evo_pt_cleanup(void)
{
    if (g_pt_active)
    {
        evo_boot_log("[PT] Emergency cleanup restoring HDMI PCM mode");
        if (g_pt_handle >= 1)
        {
            sceAudioOutOutput(g_pt_handle, nullptr);
            sceAudioOutExClose(g_pt_handle);
            g_pt_handle = -1;
        }
        sceAudioOutExConfigureOutput(0, 0, kModeDefault, kModeDefault, 0);
        g_last_reset_ms = now_ms();
        g_pt_active = 0;
    }
}

} // extern "C"
