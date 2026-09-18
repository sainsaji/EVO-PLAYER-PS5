#include "evo/services/CoverArtService.hpp"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../../../stb_image.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <unistd.h>
}

#include "evo_boot_trace.h"
#include "evo_boot_log.h"
#include "evo_crash_note.h"
#include "evo_vdec.h"
#include "pp_frame.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace evo {
#ifdef EVO_APP_MODULE
/*
 * Flexible memory is what actually runs out here.
 *
 * On the app module malloc() comes from the flexible-memory heap (see
 * evo_direct_mem.c), which is the same pool the resident sceVideodec2
 * decoders reserve out of at boot - five of them since #41 Phase D.
 *
 * Not in the SDK headers; the symbol is in the linked libkernel, declared the
 * same way evo_vdec_native.c declares sceKernelGetDirectMemorySize().
 */
extern "C" int sceKernelAvailableFlexibleMemorySize(size_t *size);

static size_t availableFlexBytes() {
    size_t avail = 0;
    if (sceKernelAvailableFlexibleMemorySize(&avail) != 0)
        return 0;          /* unknown - callers treat 0 as "do not gate" */
    return avail;
}
#else
static size_t availableFlexBytes() { return 0; }
#endif

/*
 * The decoders fault rather than fail when a picture buffer cannot be
 * allocated - libavcodec logs "get_buffer() failed" and the HEVC decoder then
 * walks into its own error path and dereferences null. That log line is the
 * last thing in the register dump from the EVO_TEST_hevc8_4k.mp4 crash, but
 * libavcodec writes it to its own av_log, which goes nowhere here, and it
 * never says how much it was asking for.
 *
 * Wrapping the allocator puts the size in evo.log, flushed before the fault
 * lands. It is also the seam a fix would go in: a get_buffer2 that can satisfy
 * the request from a reserve never returns the error that triggers the fault.
 */
static int thumbGetBuffer(AVCodecContext* avctx, AVFrame* frame, int flags) {
    int ret = avcodec_default_get_buffer2(avctx, frame, flags);
    if (ret < 0) {
        /*
         * Report the context, not the frame. libavcodec's video_get_buffer()
         * calls av_frame_unref() on its own failure path, which resets the
         * frame's width, height and format - so reading them here showed
         * "0x0 fmt=-1" for what was actually a full-size request, and sent me
         * looking for a format problem that did not exist. The context's
         * fields survive and are what the allocation was sized from.
         */
        evo_bt("extractVideoFrame: get_buffer2 FAILED ctx=%dx%d coded=%dx%d pix=%d rc=%d",
               avctx->width, avctx->height,
               avctx->coded_width, avctx->coded_height,
               avctx->pix_fmt, ret);
        evo_bt("extractVideoFrame: flex %zuMB free at failure",
               availableFlexBytes() / (1024u * 1024u));
        evo_boot_log_flush();
    }
    return ret;
}

/*
 * Decode one poster frame on the hardware decoder.
 *
 * This exists because FFmpeg's software HEVC decoder faults rather than fails
 * when a picture buffer cannot be allocated, and EVO_TEST_hevc8_4k.mp4 makes
 * it do exactly that - a file the hardware decoder plays start to finish
 * without complaint. Going through the same decoder playback uses turns the
 * worst case from "the process dies" into "sceVideodec2 returns an error",
 * and a 4K frame arrives in a fraction of the time software takes.
 *
 * Returns false when the hardware path was not available or produced nothing;
 * the caller then runs its own avcodec path, so this is only ever an
 * improvement on the files it accepts. The format context is left rewound so
 * that fallback starts from a sane position.
 *
 * Only ever called off the player screen: the resident decoder is claimed for
 * the whole of a playing file, so during playback the open below fails the
 * slot check and we fall back - which is what we want, both for correctness
 * and because a poster must never disturb the picture.
 */
static bool decodePosterOnHardware(AVFormatContext* fmt, AVStream* st, int vstream,
                                   double seekSec, uint32_t* outPixels,
                                   int targetWidth, int targetHeight)
{
    if (!fmt || !st || !st->codecpar || !outPixels)
        return false;

    int bitDepth = st->codecpar->bits_per_raw_sample > 8
                 ? st->codecpar->bits_per_raw_sample : 8;
    if (st->codecpar->format == AV_PIX_FMT_YUV420P10LE ||
        st->codecpar->format == AV_PIX_FMT_YUV420P10BE)
        bitDepth = 10;

    if (!evo_vdec_native_can_open(st->codecpar->codec_id, st->codecpar->profile,
                                  bitDepth, st->codecpar->width,
                                  st->codecpar->height))
        return false;

    evo_vdec_open_params np;
    std::memset(&np, 0, sizeof(np));
    np.backend        = EVO_VDEC_BACKEND_NATIVE;
    np.codec_id       = st->codecpar->codec_id;
    np.width          = st->codecpar->width;
    np.height         = st->codecpar->height;
    np.extradata      = st->codecpar->extradata;
    np.extradata_size = st->codecpar->extradata_size;
    np.avctx_params   = st->codecpar;
    np.thread_count     = EVO_VDEC_KEEP;
    np.thread_type      = EVO_VDEC_KEEP;
    np.skip_loop_filter = EVO_VDEC_KEEP;
    np.skip_frame       = EVO_VDEC_KEEP;
    np.skip_idct        = EVO_VDEC_KEEP;

    evo_vdec_backend chosen = EVO_VDEC_BACKEND_FFMPEG;
    evo_vdec* dec = evo_vdec_open(&np, &chosen);
    if (!dec)
        return false;
    if (chosen != EVO_VDEC_BACKEND_NATIVE) {
        /* Downgraded - the slot is busy or the stream was refused. The
         * caller's own avcodec path is better tuned than this one would be. */
        evo_vdec_close(dec);
        return false;
    }

    evo_bt("extractVideoFrame: hardware decoder for the poster");
    evo_boot_log_flush();

    if (seekSec > 0.0 && st->time_base.den > 0) {
        int64_t seekTs = static_cast<int64_t>(seekSec / av_q2d(st->time_base));
        if (seekTs < 0) seekTs = 0;
        if (av_seek_frame(fmt, vstream, seekTs, AVSEEK_FLAG_BACKWARD) < 0)
            av_seek_frame(fmt, vstream, 0, AVSEEK_FLAG_BACKWARD);
    }
    /*
     * Always flush after the seek, exactly as the play loop does, even though
     * the decoder was opened moments ago. The flush is what rebuilds the
     * mp4toannexb filter, and a fresh filter is what re-injects the parameter
     * sets on its first output packet. Without it every HEVC poster failed
     * (`Decode FAIL err=1`, no frames) while every H.264 one succeeded -
     * the same split evo_vdec_native.c documents on seek, because AVC streams
     * tend to repeat SPS/PPS in-band per keyframe and HEVC ones keep them in
     * hvcC extradata only.
     */
    evo_vdec_flush(dec);

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        evo_vdec_close(dec);
        return false;
    }

    pp_frame pf;
    std::memset(&pf, 0, sizeof(pf));
    bool gotFrame = false;
    int packetCount = 0;

    while (!gotFrame && packetCount < 48 && av_read_frame(fmt, pkt) >= 0) {
        packetCount++;
        if (pkt->stream_index == vstream) {
            int64_t pts = (pkt->pts != AV_NOPTS_VALUE)
                        ? av_rescale_q(pkt->pts, st->time_base, AVRational{1, 1000000})
                        : INT64_MIN;
            /* send() > 0 means the reorder buffer is full: drain and retry the
             * same unit, exactly as the play loop does. */
            for (int attempt = 0; attempt < 8; ++attempt) {
                int sent = evo_vdec_send(dec, pkt->data, pkt->size, pts);
                if (sent < 0) { packetCount = 48; break; }
                if (evo_vdec_receive(dec, &pf) == 1) gotFrame = true;
                if (sent == 0) break;
                if (gotFrame) break;
            }
        }
        av_packet_unref(pkt);
    }

    if (!gotFrame) {          /* flush whatever is still held back */
        evo_vdec_send(dec, nullptr, 0, INT64_MIN);
        gotFrame = (evo_vdec_receive(dec, &pf) == 1);
    }

    bool success = false;
    if (gotFrame && pf.planes[0] && pf.width > 0 && pf.height > 0) {
        enum AVPixelFormat srcFormat;
        switch (pf.format) {
            case PP_FRAME_NV12:     srcFormat = AV_PIX_FMT_NV12;       break;
            case PP_FRAME_YUV420P:  srcFormat = AV_PIX_FMT_YUV420P;    break;
            case PP_FRAME_NV12_10:  srcFormat = AV_PIX_FMT_P010LE;     break;
            case PP_FRAME_YUV420P10:srcFormat = AV_PIX_FMT_YUV420P10LE;break;
            default:                srcFormat = AV_PIX_FMT_NONE;       break;
        }

        SwsContext* sws = (srcFormat == AV_PIX_FMT_NONE) ? nullptr : sws_getContext(
            static_cast<int>(pf.width), static_cast<int>(pf.height), srcFormat,
            targetWidth, targetHeight, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);

        if (sws) {
            const uint8_t* srcData[4] = { pf.planes[0], pf.planes[1],
                                          pf.planes[2], pf.planes[3] };
            int srcLines[4] = { pf.strides[0], pf.strides[1],
                                pf.strides[2], pf.strides[3] };
            uint8_t* dst[4] = { reinterpret_cast<uint8_t*>(outPixels), nullptr, nullptr, nullptr };
            int dstLines[4] = { targetWidth * 4, 0, 0, 0 };
            sws_scale(sws, srcData, srcLines, 0, static_cast<int>(pf.height), dst, dstLines);
            sws_freeContext(sws);
            success = true;
            evo_bt("extractVideoFrame: hardware poster ok %ux%u fmt=%d",
                   pf.width, pf.height, (int)pf.format);
            evo_boot_log_flush();
        }
    }

    av_packet_free(&pkt);
    evo_vdec_close(dec);     /* releases the resident slot */

    if (!success) {
        /* Put the demuxer back where the caller's own path expects it. */
        av_seek_frame(fmt, vstream, 0, AVSEEK_FLAG_BACKWARD);
        evo_bt("extractVideoFrame: hardware poster produced nothing -> FFmpeg");
        evo_boot_log_flush();
    }
    return success;
}

CoverArtService::CoverArtService() {
    /* Resolve the quarantine path before the first extraction, so the crash
     * handler has a stable buffer to write into if one takes us down. */
    evo_crash_note_init();
    m_cache.resize(CacheCapacity);
    for (auto& entry : m_cache) {
        entry.pixels.resize(PosterWidth * PosterHeight, 0);
    }
    m_heroArtPixels.resize(HeroWidth * HeroHeight, 0);
    m_browserPreviewPixels.resize(PreviewWidth * PreviewHeight, 0);
}

void CoverArtService::clearCache() {
    for (auto& entry : m_cache) {
        entry.pathKey.clear();
        entry.valid = false;
        entry.tried = false;
        std::fill(entry.pixels.begin(), entry.pixels.end(), 0);
    }
    m_heroArtPath.clear();
    m_heroArtValid = false;
    std::fill(m_heroArtPixels.begin(), m_heroArtPixels.end(), 0);

    m_browserPreviewPath.clear();
    m_browserPreviewValid = false;
    m_browserPreviewFailed = false;
    std::fill(m_browserPreviewPixels.begin(), m_browserPreviewPixels.end(), 0);
}

bool CoverArtService::isVideoFile(const std::string& path) const {
    if (path.empty()) return false;
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) return false;

    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    return (ext == ".mkv"  || ext == ".mp4"  || ext == ".mov" ||
            ext == ".m4v"  || ext == ".avi"  || ext == ".webm" ||
            ext == ".ts"   || ext == ".m2ts" || ext == ".mpg" ||
            ext == ".mpeg" || ext == ".wmv"  || ext == ".flv");
}

std::string CoverArtService::resolveSidecarPath(const std::string& mediaPath, bool isDirectory) {
    if (mediaPath.empty()) return "";

    static const char* const sidecarExts[] = {
        ".jpg", ".jpeg", ".png", ".JPG", ".JPEG", ".PNG", nullptr
    };
    static const char* const folderNames[] = {
        "folder.jpg", "folder.png", "poster.jpg", "poster.png",
        "cover.jpg", "cover.png", "Folder.jpg", "Poster.jpg", nullptr
    };

    if (isDirectory) {
        for (int i = 0; folderNames[i]; ++i) {
            std::string candidate = mediaPath + "/" + folderNames[i];
            if (access(candidate.c_str(), R_OK) == 0) {
                return candidate;
            }
        }
        return "";
    }

    size_t lastSlash = mediaPath.find_last_of('/');
    if (lastSlash == std::string::npos || lastSlash == 0) return "";

    std::string dir = mediaPath.substr(0, lastSlash);
    std::string base = mediaPath.substr(lastSlash + 1);
    size_t lastDot = base.find_last_of('.');
    if (lastDot != std::string::npos && lastDot > 0) {
        base = base.substr(0, lastDot);
    }

    for (int i = 0; sidecarExts[i]; ++i) {
        std::string candidate = dir + "/" + base + sidecarExts[i];
        if (access(candidate.c_str(), R_OK) == 0) {
            return candidate;
        }
    }
    for (int i = 0; folderNames[i]; ++i) {
        std::string candidate = dir + "/" + folderNames[i];
        if (access(candidate.c_str(), R_OK) == 0) {
            return candidate;
        }
    }
    return "";
}

CoverArtService::CacheEntry* CoverArtService::findOrAllocateSlot(const std::string& key) {
    if (key.empty()) return nullptr;

    int freeIdx = -1;
    for (size_t i = 0; i < m_cache.size(); ++i) {
        if (m_cache[i].tried && m_cache[i].pathKey == key) {
            return &m_cache[i];
        }
        if (!m_cache[i].tried && freeIdx < 0) {
            freeIdx = static_cast<int>(i);
        }
    }

    if (freeIdx >= 0) {
        return &m_cache[freeIdx];
    }

    // Reuse invalid slot or LRU
    for (size_t i = 0; i < m_cache.size(); ++i) {
        if (!m_cache[i].valid) {
            return &m_cache[i];
        }
    }

    int oldestIdx = (m_accessClock++) % static_cast<int>(m_cache.size());
    return &m_cache[oldestIdx];
}

const CoverArtService::CacheEntry* CoverArtService::findSlot(const std::string& key) const {
    if (key.empty()) return nullptr;
    for (const auto& entry : m_cache) {
        if (entry.tried && entry.pathKey == key) {
            return &entry;
        }
    }
    return nullptr;
}

const uint32_t* CoverArtService::peekCoverArt(const std::string& mediaPath) const {
    if (mediaPath.empty()) return nullptr;
    const auto* slot = findSlot(mediaPath);
    if (slot && slot->valid) {
        return slot->pixels.data();
    }
    return nullptr;
}

bool CoverArtService::hasTriedCoverArt(const std::string& mediaPath) const {
    if (mediaPath.empty()) return false;
    const auto* slot = findSlot(mediaPath);
    return slot && slot->tried;
}

void CoverArtService::boxFilterScaleRgba(const uint8_t* sourceRgba, int srcWidth, int srcHeight,
                                        uint32_t* destBgra, int destWidth, int destHeight) {
    if (!sourceRgba || !destBgra || srcWidth < 1 || srcHeight < 1 || destWidth < 1 || destHeight < 1) {
        return;
    }

    for (int y = 0; y < destHeight; ++y) {
        int y0 = static_cast<int>((static_cast<int64_t>(y) * srcHeight) / destHeight);
        int y1 = static_cast<int>((static_cast<int64_t>(y + 1) * srcHeight) / destHeight);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > srcHeight) y1 = srcHeight;

        for (int x = 0; x < destWidth; ++x) {
            int x0 = static_cast<int>((static_cast<int64_t>(x) * srcWidth) / destWidth);
            int x1 = static_cast<int>((static_cast<int64_t>(x + 1) * srcWidth) / destWidth);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > srcWidth) x1 = srcWidth;

            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; ++yy) {
                const uint8_t* row = sourceRgba + (static_cast<size_t>(yy) * static_cast<size_t>(srcWidth) + static_cast<size_t>(x0)) * 4u;
                for (int xx = x0; xx < x1; ++xx, row += 4) {
                    r += row[0];
                    g += row[1];
                    b += row[2];
                    n++;
                }
            }
            if (!n) n = 1;
            destBgra[y * destWidth + x] = MakeColorBgra(static_cast<uint8_t>(r / n),
                                                        static_cast<uint8_t>(g / n),
                                                        static_cast<uint8_t>(b / n),
                                                        255);
        }
    }
}

bool CoverArtService::extractVideoFrame(const std::string& videoPath, uint32_t* outPixels, int targetWidth, int targetHeight) {
    evo_bt("extractVideoFrame: %s (%dx%d)", videoPath.c_str(), targetWidth, targetHeight);
    evo_boot_log_flush();

    if (videoPath.empty() || !outPixels || targetWidth < 2 || targetHeight < 2) {
        return false;
    }
    if (access(videoPath.c_str(), R_OK) != 0) {
        evo_bt("extractVideoFrame: access failed: %s", videoPath.c_str());
        evo_boot_log_flush();
        return false;
    }

    /*
     * Floor check, before the demuxer is even opened.
     *
     * avformat_find_stream_info() opens decoders of its own to work out the
     * stream parameters, so it allocates picture buffers exactly like a real
     * decode does - and it faulted there on a 4K file with flexible memory
     * exhausted, before any of the gates further down could run. Those gates
     * size themselves from codecpar, which does not exist yet at this point,
     * so this one is a flat floor: below it, do not touch the file at all.
     *
     * The resident sceVideodec2 decoders reserve ~2 GB of flexible memory at
     * boot (#41 Phase B + D), which is what makes this reachable at all; see
     * the note above EVO_VDEC_NATIVE_10BIT in evo_vdec_native.c.
     */
    {
        constexpr size_t ProbeFloorBytes = 64u * 1024u * 1024u;
        const size_t availBytes = availableFlexBytes();
        if (availBytes > 0 && availBytes < ProbeFloorBytes) {
            evo_bt("extractVideoFrame: skipping - only %zuMB flex free, need %zuMB to probe",
                   availBytes / (1024u * 1024u), ProbeFloorBytes / (1024u * 1024u));
            evo_boot_log_flush();
            return false;
        }
    }

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "probesize", "1048576", 0);
    av_dict_set(&opts, "analyzeduration", "1000000", 0);
    /* A poster needs the stream's geometry, not a confident frame-rate
     * estimate. Fewer probe packets means fewer buffers find_stream_info
     * allocates before it is satisfied. */
    av_dict_set(&opts, "max_probe_packets", "16", 0);

    AVFormatContext* fmt = nullptr;
    evo_bt("extractVideoFrame: calling avformat_open_input");
    evo_boot_log_flush();
    if (avformat_open_input(&fmt, videoPath.c_str(), nullptr, &opts) < 0) {
        evo_bt("extractVideoFrame: open_input failed: %s", videoPath.c_str());
        evo_boot_log_flush();
        av_dict_free(&opts);
        return false;
    }
    av_dict_free(&opts);
    evo_bt("extractVideoFrame: avformat_open_input ok");
    evo_boot_log_flush();

    int vstream = -1;
    for (unsigned int i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar && fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vstream = static_cast<int>(i);
            break;
        }
    }
    if (vstream >= 0 && fmt->streams[vstream]->codecpar) {
        if (fmt->streams[vstream]->codecpar->codec_id == AV_CODEC_ID_AV1) {
            evo_bt("extractVideoFrame: skipping AV1 codec before find_stream_info (unsupported on PS5)");
            evo_boot_log_flush();
            avformat_close_input(&fmt);
            return false;
        }
    }

    evo_bt("extractVideoFrame: calling avformat_find_stream_info");
    evo_boot_log_flush();
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        evo_bt("extractVideoFrame: find_stream_info failed");
        evo_boot_log_flush();
        avformat_close_input(&fmt);
        return false;
    }
    evo_bt("extractVideoFrame: avformat_find_stream_info ok, nb_streams=%u", fmt->nb_streams);
    evo_boot_log_flush();

    vstream = -1;
    for (unsigned int i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar && fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vstream = static_cast<int>(i);
            break;
        }
    }
    if (vstream < 0) {
        evo_bt("extractVideoFrame: no video stream");
        evo_boot_log_flush();
        avformat_close_input(&fmt);
        return false;
    }

    AVStream* st = fmt->streams[vstream];
    if (!st || !st->codecpar) {
        evo_bt("extractVideoFrame: invalid stream or codecpar");
        evo_boot_log_flush();
        avformat_close_input(&fmt);
        return false;
    }

    if (st->codecpar->codec_id == AV_CODEC_ID_AV1) {
        evo_bt("extractVideoFrame: skipping AV1 codec (unsupported on PS5)");
        evo_boot_log_flush();
        avformat_close_input(&fmt);
        return false;
    }

    double duration = (fmt->duration > 0) ? (static_cast<double>(fmt->duration) / static_cast<double>(AV_TIME_BASE)) : 0.0;
    double seekSec = 0.0;
    if (duration > 2.0) {
        seekSec = duration * 0.10;
        if (seekSec < 2.0) seekSec = 2.0;
        if (seekSec > 30.0) seekSec = 30.0;
        if (seekSec > duration - 1.0) seekSec = duration * 0.5;
    }

    /*
     * Hardware first, and before the quarantine check on purpose.
     *
     * The quarantine records files that killed the process, and every one of
     * them killed it inside a *software* decoder. Consulting it first would
     * deny the hardware path to precisely the files the hardware path exists
     * to rescue - EVO_TEST_hevc8_4k.mp4 is quarantined and decodes on the
     * hardware decoder without complaint. So a quarantined file still gets
     * its hardware attempt, and only the software fallback is gated.
     */
    if (decodePosterOnHardware(fmt, st, vstream, seekSec, outPixels,
                               targetWidth, targetHeight)) {
        avformat_close_input(&fmt);
        evo_bt("extractVideoFrame: cleanup complete, success=1 (hardware)");
        evo_boot_log_flush();
        return true;
    }

    /*
     * Software from here. A stream that has already taken the process down
     * once gets no second attempt on this path: libavcodec's software
     * decoders fault rather than return an error when an internal allocation
     * fails, so there is nothing to catch. Delete <data>/thumb_quarantine to
     * retry.
     */
    if (evo_crash_note_is_quarantined(videoPath.c_str())) {
        evo_bt("extractVideoFrame: quarantined after an earlier crash, skipping");
        evo_boot_log_flush();
        avformat_close_input(&fmt);
        return false;
    }

    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        evo_bt("extractVideoFrame: decoder not found for id=%d", st->codecpar->codec_id);
        evo_boot_log_flush();
        avformat_close_input(&fmt);
        return false;
    }
    evo_bt("extractVideoFrame: found decoder %s (id=%d)", dec->name ? dec->name : "?", st->codecpar->codec_id);
    evo_boot_log_flush();

    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    if (!ctx || avcodec_parameters_to_context(ctx, st->codecpar) < 0) {
        evo_bt("extractVideoFrame: alloc_context/parameters failed");
        evo_boot_log_flush();
        if (ctx) avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return false;
    }

    ctx->thread_count = 1;
#ifdef AV_CODEC_FLAG2_FAST
    ctx->flags2 |= AV_CODEC_FLAG2_FAST;
#endif
    /* A 320x180 poster does not care about deblocking, and skipping it means
     * fewer buffers touched as well as less work. */
    ctx->skip_loop_filter = AVDISCARD_ALL;
    ctx->get_buffer2 = thumbGetBuffer;

    /*
     * Working-set pre-flight.
     *
     * A 4K decoder's picture buffers are tens of MB each and the DPB holds
     * several. When flexible memory cannot serve them, libavcodec's
     * get_buffer() fails mid-decode and the HEVC decoder walks into its own
     * error path and dereferences null - a SIGSEGV with nothing to catch from
     * here. Declining the poster costs a thumbnail; letting it through costs
     * the process.
     *
     * Measured, not probed. The probe this replaced allocated four frames'
     * worth and freed them again, and concluded there was room even as the
     * decoder failed: a handful of large mallocs can still be served out of a
     * pool far too tight to build a picture pool in. The headroom multiplier
     * is deliberately generous - the pool has to hold the whole DPB plus the
     * decoder's own tables, and this is the last gate before code that faults
     * rather than fails.
     */
    {
        int bytesPerSample = 1;
        enum AVPixelFormat pxf = static_cast<enum AVPixelFormat>(st->codecpar->format);
        const AVPixFmtDescriptor* pfd = (pxf != AV_PIX_FMT_NONE) ? av_pix_fmt_desc_get(pxf) : nullptr;
        if (pfd) {
            if (pfd->comp[0].depth > 8) bytesPerSample = 2;
        } else if (st->codecpar->bits_per_raw_sample > 8) {
            bytesPerSample = 2;
        }

        const size_t frameBytes = static_cast<size_t>(st->codecpar->width) *
                                  static_cast<size_t>(st->codecpar->height) *
                                  3u / 2u * static_cast<size_t>(bytesPerSample);
        const size_t needBytes  = frameBytes * 6u;
        const size_t availBytes = availableFlexBytes();

        if (frameBytes > 0 && availBytes > 0 && availBytes < needBytes) {
            evo_bt("extractVideoFrame: skipping %dx%d - flex %zuMB free, needs ~%zuMB",
                   st->codecpar->width, st->codecpar->height,
                   availBytes / (1024u * 1024u), needBytes / (1024u * 1024u));
            evo_boot_log_flush();
            avcodec_free_context(&ctx);
            avformat_close_input(&fmt);
            return false;
        }
    }

    evo_bt("extractVideoFrame: calling avcodec_open2");
    evo_boot_log_flush();
    if (avcodec_open2(ctx, dec, nullptr) < 0) {
        evo_bt("extractVideoFrame: avcodec_open2 failed");
        evo_boot_log_flush();
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return false;
    }
    evo_bt("extractVideoFrame: avcodec_open2 ok");
    evo_boot_log_flush();

    if (seekSec > 0.0 && st->time_base.den > 0) {
        int64_t seekTs = static_cast<int64_t>(seekSec / av_q2d(st->time_base));
        if (seekTs < 0) seekTs = 0;
        evo_bt("extractVideoFrame: seeking to ts=%lld", static_cast<long long>(seekTs));
        evo_boot_log_flush();
        if (av_seek_frame(fmt, vstream, seekTs, AVSEEK_FLAG_BACKWARD) < 0) {
            av_seek_frame(fmt, vstream, 0, AVSEEK_FLAG_BACKWARD);
        }
        avcodec_flush_buffers(ctx);
        evo_bt("extractVideoFrame: seek done");
        evo_boot_log_flush();
    }

    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    if (!frame || !pkt) {
        evo_bt("extractVideoFrame: frame/pkt alloc failed");
        evo_boot_log_flush();
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return false;
    }
    bool gotFrame = false;
    int packetCount = 0;

    evo_bt("extractVideoFrame: starting read loop");
    evo_boot_log_flush();

    /* From here until the decoder is released we are inside code that can
     * fault instead of failing. Leave a note for the crash handler. */
    evo_crash_note_set(videoPath.c_str());

    while (packetCount < 48 && av_read_frame(fmt, pkt) >= 0) {
        packetCount++;
        if (pkt->stream_index == vstream) {
            if (avcodec_send_packet(ctx, pkt) == 0) {
                if (avcodec_receive_frame(ctx, frame) == 0) {
                    gotFrame = true;
                    evo_bt("extractVideoFrame: got frame on packet %d (%dx%d fmt=%d)", packetCount, frame->width, frame->height, frame->format);
                    evo_boot_log_flush();
                    av_packet_unref(pkt);
                    break;
                }
            }
        }
        av_packet_unref(pkt);
    }

    if (!gotFrame) {
        evo_bt("extractVideoFrame: flushing decoder");
        evo_boot_log_flush();
        avcodec_send_packet(ctx, nullptr);
        if (avcodec_receive_frame(ctx, frame) == 0) {
            gotFrame = true;
            evo_bt("extractVideoFrame: got frame after flush (%dx%d fmt=%d)", frame->width, frame->height, frame->format);
            evo_boot_log_flush();
        }
    }

    /*
     * Release the decoder and the demuxer before scaling.
     *
     * The frame's planes are reference-counted, so they outlive the context
     * that produced them; everything else the decoder was holding - the rest
     * of the DPB, the parser, the demuxer's buffers - is handed back here
     * instead of staying live across sws_getContext(). At 4K that is the
     * difference between the scaler asking for its tables with tens of MB
     * free and asking for them with none, which is where this used to fail.
     */
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);

    /* Out of the decoder; swscale has never been the thing that faults. */
    evo_crash_note_set(nullptr);

    bool success = false;
    if (gotFrame && frame->width > 0 && frame->height > 0 && frame->format >= 0 && frame->data[0]) {
        evo_bt("extractVideoFrame: scaling via sws_getContext (%dx%d -> %dx%d fmt=%d)",
               frame->width, frame->height, targetWidth, targetHeight, frame->format);
        evo_boot_log_flush();

        enum AVPixelFormat srcFormat = static_cast<enum AVPixelFormat>(frame->format);
        if (srcFormat == AV_PIX_FMT_YUVJ420P) srcFormat = AV_PIX_FMT_YUV420P;
        else if (srcFormat == AV_PIX_FMT_YUVJ422P) srcFormat = AV_PIX_FMT_YUV422P;
        else if (srcFormat == AV_PIX_FMT_YUVJ444P) srcFormat = AV_PIX_FMT_YUV444P;

        SwsContext* sws = sws_getContext(
            frame->width, frame->height, srcFormat,
            targetWidth, targetHeight, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );

        if (sws) {
            uint8_t* dst[4] = { reinterpret_cast<uint8_t*>(outPixels), nullptr, nullptr, nullptr };
            int dstLinesize[4] = { targetWidth * 4, 0, 0, 0 };
            sws_scale(sws, (const uint8_t* const*)frame->data, frame->linesize, 0, frame->height, dst, dstLinesize);
            sws_freeContext(sws);
            success = true;
            evo_bt("extractVideoFrame: sws_scale done");
            evo_boot_log_flush();
        } else {
            evo_bt("extractVideoFrame: sws_getContext failed");
            evo_boot_log_flush();
        }
    } else {
        evo_bt("extractVideoFrame: no valid frame decoded");
        evo_boot_log_flush();
    }

    av_frame_free(&frame);
    evo_bt("extractVideoFrame: cleanup complete, success=%d", success ? 1 : 0);
    evo_boot_log_flush();
    return success;
}

const uint32_t* CoverArtService::getCoverArt(const std::string& mediaPath, bool isDirectory) {
    evo_bt("getCoverArt: %s (isDir=%d)", mediaPath.c_str(), isDirectory ? 1 : 0);
    evo_boot_log_flush();

    if (mediaPath.empty()) { evo_bt("getCoverArt: empty path"); evo_boot_log_flush(); return nullptr; }
    if (access(mediaPath.c_str(), R_OK) != 0) { evo_bt("getCoverArt: access fail len=%zu", mediaPath.length()); evo_boot_log_flush(); evo_bt("getCoverArt: returning nullptr"); evo_boot_log_flush(); return nullptr; }
    evo_bt("getCoverArt: access ok, calling findOrAllocateSlot");
    evo_boot_log_flush();

    CacheEntry* slot = findOrAllocateSlot(mediaPath);
    if (!slot) { evo_bt("getCoverArt: no slot"); evo_boot_log_flush(); return nullptr; }
    evo_bt("getCoverArt: slot=%p tried=%d", (void*)slot, slot->tried);
    evo_boot_log_flush();

    if (slot->tried && slot->pathKey == mediaPath) {
        return slot->valid ? slot->pixels.data() : nullptr;
    }

    slot->pathKey = mediaPath;
    slot->tried = true;
    slot->valid = false;

    std::string sidecar = resolveSidecarPath(mediaPath, isDirectory);
    evo_bt("getCoverArt: sidecar='%s'", sidecar.c_str());
    evo_boot_log_flush();
    if (!sidecar.empty() && access(sidecar.c_str(), R_OK) == 0) {
        int w = 0, h = 0, ch = 0;
        unsigned char* data = stbi_load(sidecar.c_str(), &w, &h, &ch, 4);
        if (data && w >= 2 && h >= 2) {
            boxFilterScaleRgba(data, w, h, slot->pixels.data(), PosterWidth, PosterHeight);
            stbi_image_free(data);
            slot->valid = true;
            return slot->pixels.data();
        }
        if (data) stbi_image_free(data);
    }

    if (!isDirectory && isVideoFile(mediaPath)) {
        evo_bt("getCoverArt: isVideo, calling extractVideoFrame");
        evo_boot_log_flush();
        if (extractVideoFrame(mediaPath, slot->pixels.data(), PosterWidth, PosterHeight)) {
            slot->valid = true;
            return slot->pixels.data();
        }
    }

    if (!isDirectory) {
        size_t dot = mediaPath.find_last_of('.');
        if (dot != std::string::npos) {
            std::string ext = mediaPath.substr(dot);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp" || ext == ".webp") {
                int w = 0, h = 0, ch = 0;
                unsigned char* data = stbi_load(mediaPath.c_str(), &w, &h, &ch, 4);
                if (data && w >= 2 && h >= 2) {
                    boxFilterScaleRgba(data, w, h, slot->pixels.data(), PosterWidth, PosterHeight);
                    stbi_image_free(data);
                    slot->valid = true;
                    return slot->pixels.data();
                }
                if (data) stbi_image_free(data);
            }
        }
    }

    return nullptr;
}

void CoverArtService::ensureHeroArt(const std::string& mediaPath) {
    if (mediaPath.empty()) {
        m_heroArtPath.clear();
        m_heroArtValid = false;
        return;
    }

    if (m_heroArtPath == mediaPath) {
        return;
    }

    m_heroArtPath = mediaPath;
    m_heroArtValid = false;

    if (access(mediaPath.c_str(), R_OK) != 0) {
        return;
    }

    if (isVideoFile(mediaPath)) {
        if (extractVideoFrame(mediaPath, m_heroArtPixels.data(), HeroWidth, HeroHeight)) {
            m_heroArtValid = true;
            return;
        }
    }

    std::string sidecar = resolveSidecarPath(mediaPath, false);
    if (!sidecar.empty() && access(sidecar.c_str(), R_OK) == 0) {
        int w = 0, h = 0, ch = 0;
        unsigned char* data = stbi_load(sidecar.c_str(), &w, &h, &ch, 4);
        if (data && w >= 2 && h >= 2) {
            boxFilterScaleRgba(data, w, h, m_heroArtPixels.data(), HeroWidth, HeroHeight);
            stbi_image_free(data);
            m_heroArtValid = true;
            return;
        }
        if (data) stbi_image_free(data);
    }
}

void CoverArtService::ensureBrowserPreview(const std::string& mediaPath, bool isDirectory) {
    if (mediaPath.empty()) {
        m_browserPreviewValid = false;
        m_browserPreviewPath.clear();
        return;
    }

    if (m_browserPreviewPath == mediaPath) {
        if (m_browserPreviewValid || m_browserPreviewFailed) {
            return;
        }
    }

    m_browserPreviewPath = mediaPath;
    m_browserPreviewValid = false;
    m_browserPreviewFailed = false;

    std::string sidecar = resolveSidecarPath(mediaPath, isDirectory);
    if (!sidecar.empty()) {
        int w = 0, h = 0, ch = 0;
        unsigned char* data = stbi_load(sidecar.c_str(), &w, &h, &ch, 4);
        if (data && w >= 2 && h >= 2) {
            boxFilterScaleRgba(data, w, h, m_browserPreviewPixels.data(), PreviewWidth, PreviewHeight);
            stbi_image_free(data);
            m_browserPreviewValid = true;
            return;
        }
        if (data) stbi_image_free(data);
    }

    if (!isDirectory && isVideoFile(mediaPath)) {
        if (extractVideoFrame(mediaPath, m_browserPreviewPixels.data(), PreviewWidth, PreviewHeight)) {
            m_browserPreviewValid = true;
            return;
        }
    }

    if (!isDirectory) {
        size_t dot = mediaPath.find_last_of('.');
        if (dot != std::string::npos) {
            std::string ext = mediaPath.substr(dot);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp" || ext == ".webp") {
                int w = 0, h = 0, ch = 0;
                unsigned char* data = stbi_load(mediaPath.c_str(), &w, &h, &ch, 4);
                if (data && w >= 2 && h >= 2) {
                    boxFilterScaleRgba(data, w, h, m_browserPreviewPixels.data(), PreviewWidth, PreviewHeight);
                    stbi_image_free(data);
                    m_browserPreviewValid = true;
                    return;
                }
                if (data) stbi_image_free(data);
            }
        }
    }

    m_browserPreviewFailed = true;
}

const uint32_t* CoverArtService::getBrowserPreviewPixels() const {
    return m_browserPreviewValid ? m_browserPreviewPixels.data() : nullptr;
}

} // namespace evo
