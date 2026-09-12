#include "evo/services/CoverArtService.hpp"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../../../stb_image.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/mathematics.h>
#include <unistd.h>
}

#include "evo_boot_trace.h"
#include "evo_boot_log.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace evo {

CoverArtService::CoverArtService() {
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

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "probesize", "1048576", 0);
    av_dict_set(&opts, "analyzeduration", "1000000", 0);

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
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    evo_bt("extractVideoFrame: cleanup complete, success=%d", success ? 1 : 0);
    evo_boot_log_flush();
    return success;
}

const uint32_t* CoverArtService::getCoverArt(const std::string& mediaPath, bool isDirectory) {
    evo_bt("getCoverArt: %s (isDir=%d)", mediaPath.c_str(), isDirectory ? 1 : 0);
    evo_boot_log_flush();

    if (mediaPath.empty()) return nullptr;
    if (access(mediaPath.c_str(), R_OK) != 0) return nullptr;

    CacheEntry* slot = findOrAllocateSlot(mediaPath);
    if (!slot) return nullptr;

    if (slot->tried && slot->pathKey == mediaPath) {
        return slot->valid ? slot->pixels.data() : nullptr;
    }

    slot->pathKey = mediaPath;
    slot->tried = true;
    slot->valid = false;

    std::string sidecar = resolveSidecarPath(mediaPath, isDirectory);
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
        if (extractVideoFrame(mediaPath, slot->pixels.data(), PosterWidth, PosterHeight)) {
            slot->valid = true;
            return slot->pixels.data();
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
