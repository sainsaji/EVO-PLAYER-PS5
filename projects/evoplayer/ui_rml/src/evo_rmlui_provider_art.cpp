/*
 * evo_rmlui_provider_art.cpp — remote artwork for provider screens (#90).
 *
 * Poster walls are the point of "the service's own look", so this is not an
 * optional extra: a channel grid with no logos and a library with no posters
 * both look like EVO drew them, which is the outcome the whole seam exists to
 * avoid.
 *
 * THE PATH
 *
 *   url -> evo_net GET -> bytes on disk under the provider's cache
 *       -> FFmpeg decode by path -> BGRA -> SetMemoryTexture("evo:mem/...")
 *
 * Two deliberate choices in that chain:
 *
 * Bytes land on DISK before they are decoded, rather than being decoded from
 * memory. It costs a small write per poster and buys three things: the decode
 * is the same avformat_open_input + image2 path CoverArtService already uses,
 * so no new FFmpeg demuxer has to be enabled and no in-memory AVIOContext has
 * to be maintained; the poster cache survives a relaunch, so the second visit
 * to a library paints immediately; and a malformed image is contained by
 * FFmpeg's own probing rather than by code written here.
 *
 * The cost of going through image2 is that the decoder is chosen from the
 * FILENAME, so the cache file has to be named for its sniffed type - see
 * sniff_ext(). A poster whose format has no decoder in this build (WebP, GIF)
 * is refused on arrival rather than written and failed later.
 *
 * The decoded texture lives in the render interface's "evo:mem/" registry,
 * which both backends already resolve before touching the filesystem
 * (evo_rmlui_render.cpp LoadTexture, evo_rmlui_render_agc.cpp). Nothing in the
 * render path changes for this feature at all - that is why it is safe to add
 * to a backend whose failure mode is a silent mis-render.
 *
 * THE BUDGET
 *
 * An LRU over decoded bytes. A 500-channel playlist asks for 500 logos and
 * would otherwise hold half a gigabyte of premultiplied BGRA; the cap keeps it
 * to EVO_BUNDLE_MAX_TEXTURE_BYTES and evicting something still on screen just
 * makes that element draw empty again for a frame, which is why the cap is
 * generous rather than tight.
 */
#include "evo_rmlui_app.h"
#include "evo_rmlui_render_bridge.h"

#include <RmlUi/Core.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "evo_provider.h"
#include "evo_provider_bundle.h"
#include "evo_net.h"
#include "evo_data_path.h"
#include "evo_provider_log.h"

/*
 * EVO_PROVIDER_ART_NO_DECODE: host renderer only.
 *
 * tools/uiview_playback_rml.sh links RmlUi and freetype but no FFmpeg - there
 * are no host FFmpeg development libraries in the dev image, and building them
 * to decode a poster in a layout preview is not a trade worth making. With
 * this defined the download, the disk cache, the key registry, the LRU and
 * every path check all still run and are all still exercised; only the decode
 * itself is absent, so the harness renders the no-artwork branch that a
 * provider bundle has to handle anyway (and which is the first frame of every
 * poster wall on hardware).
 *
 * It is never defined for a device build - the Makefile does not pass it.
 */
#ifndef EVO_PROVIDER_ART_NO_DECODE
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#endif
}

namespace {

/* Posters are scaled to fit this box. Bigger than any tile the UI draws at
 * 1080p, small enough that 200 of them fit the budget. */
constexpr int kMaxDim = 480;

struct Entry {
    std::string key;          /* "evo:mem/prov-<hash>" */
    std::string file;         /* where the raw bytes were written */
    size_t      bytes = 0;    /* decoded size, 0 until registered */
    bool        ready = false;
    bool        failed = false;
};

struct ArtState {
    std::unordered_map<std::string, Entry> by_key;
    /* Most-recently-used at the front. Holds keys, not iterators, because the
     * map rehashes. */
    std::list<std::string> lru;
    size_t used = 0;
    size_t budget = EVO_BUNDLE_MAX_TEXTURE_BYTES;
    /* Keys whose bytes have arrived and are waiting to be decoded. Decoding is
     * deferred to poll() so it happens on the render thread's frame, where the
     * cost is visible in the frame time rather than hidden in a callback. */
    std::vector<std::string> arrived;
    int in_flight = 0;
};

ArtState& S()
{
    static ArtState s;
    return s;
}

/* At most this many downloads outstanding. evo_net's queue is 32 slots and is
 * shared with catalog requests, so art must not be allowed to fill it - a
 * poster wall that starves the catalog request behind it would show an empty
 * screen full of pictures. */
constexpr int kMaxInFlight = 8;

/* FNV-1a over the URL. Only needs to avoid collisions between the posters of
 * one session, and a 64-bit hash does that with room to spare; it is not a
 * security boundary - the file it names is written by us, into a directory
 * built by evo_bundle_path(). */
std::string url_hash(const char* url)
{
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char* p = (const unsigned char*)url; *p; ++p) {
        h ^= *p;
        h *= 1099511628211ull;
    }
    char buf[32];
    snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
    return std::string(buf);
}

/*
 * The cache file's extension is load-bearing, not cosmetic. FFmpeg's image2
 * demuxer picks the decoder from the FILENAME (ff_guess_image2_codec), never
 * from the content. This cache used to name every download "<hash>.img", and
 * ".img" is in that table - it maps to AV_CODEC_ID_GEM, a format the FFmpeg
 * profile does not build a decoder for. So every logo opened fine and then
 * died at avcodec_find_decoder, which reads as "no logos" with nothing in the
 * log. Naming the file for what the bytes actually are keeps the decode on the
 * png/mjpeg decoders scripts/build-ffmpeg.sh does enable.
 */
const char* sniff_ext(const char* bytes, size_t len)
{
    if (len >= 8 && memcmp(bytes, "\x89PNG\r\n\x1a\n", 8) == 0) return ".png";
    if (len >= 3 && memcmp(bytes, "\xff\xd8\xff", 3) == 0)      return ".jpg";
    return nullptr;   /* WebP and GIF land here: no decoder in this build. */
}

/* Every extension sniff_ext() can produce, for locating a file a previous
 * session cached when the bytes are not in hand to sniff again. */
const char* const kArtExts[] = { ".png", ".jpg" };

/*
 * Why this module logs at all, and why it is capped.
 *
 * It used to log nothing whatsoever, so "no logos" was indistinguishable from
 * "no logos in the playlist", a 403, a write that failed, and a decode that
 * failed - none of which leave any other trace. But art is driven from Tick(),
 * i.e. every frame, and a per-frame PROV_LOG in this path is what produced a
 * 16.9 MB log and cost frame rate once already.
 *
 * So: every line below is on a ONE-SHOT path (a per-key state transition that
 * happens at most once, because `failed`/`ready` latch), and the total is capped
 * anyway in case a level has hundreds of broken posters.
 */
int art_log_budget = 24;
#define ART_LOG(...)                                                          \
    do {                                                                      \
        if (art_log_budget > 0) { art_log_budget--; PROV_LOG(__VA_ARGS__); }  \
    } while (0)

void touch(const std::string& key)
{
    ArtState& s = S();
    for (auto it = s.lru.begin(); it != s.lru.end(); ++it) {
        if (*it == key) { s.lru.splice(s.lru.begin(), s.lru, it); return; }
    }
    s.lru.push_front(key);
}

void evict_to_budget()
{
    ArtState& s = S();
    EvoRenderBridge* r = EvoRmlApp::Instance().RenderBridge();
    while (s.used > s.budget && !s.lru.empty()) {
        std::string victim = s.lru.back();
        s.lru.pop_back();
        auto it = s.by_key.find(victim);
        if (it == s.by_key.end()) continue;
        if (it->second.ready && r) r->DropMemoryTexture(it->second.key);
        s.used -= it->second.bytes;
        /* The raw file stays: it is the disk cache, and re-decoding it is far
         * cheaper than downloading it again. */
        it->second.ready = false;
        it->second.bytes = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Decode                                                                    */
/* ------------------------------------------------------------------------- */
/*
 * One image file to premultiplied BGRA at or under kMaxDim.
 *
 * Premultiplied because that is what the CPU rasteriser and the AGC backend
 * both expect from the evo:mem registry - handing them straight alpha makes
 * every poster's edges fringe, which is a silent wrong-looking result rather
 * than an error.
 */
#ifdef EVO_PROVIDER_ART_NO_DECODE

bool decode_image(const char* path, std::vector<uint32_t>& out, int& ow, int& oh)
{
    (void)path; (void)out; (void)ow; (void)oh;
    return false;   /* see EVO_PROVIDER_ART_NO_DECODE above */
}

#else

bool decode_image(const char* path, std::vector<uint32_t>& out, int& ow, int& oh)
{
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path, nullptr, nullptr) < 0) return false;

    /* Everything the cleanup path touches is declared before the first goto -
     * a jump past an initialisation does not compile in C++. */
    bool ok = false;
    AVCodecContext* dec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* sws = nullptr;
    int vs = -1;

    if (avformat_find_stream_info(fmt, nullptr) < 0) goto done;

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vs = (int)i;
            break;
        }
    }
    if (vs < 0) goto done;

    {
        AVCodecParameters* par = fmt->streams[vs]->codecpar;
        /* A poster with absurd dimensions is a decode that allocates hundreds
         * of megabytes before anything here gets a say. Refuse it up front. */
        if (par->width <= 0 || par->height <= 0 ||
            par->width > 8192 || par->height > 8192) goto done;

        const AVCodec* c = avcodec_find_decoder(par->codec_id);
        if (!c) goto done;
        dec = avcodec_alloc_context3(c);
        if (!dec) goto done;
        if (avcodec_parameters_to_context(dec, par) < 0) goto done;
        if (avcodec_open2(dec, c, nullptr) < 0) goto done;
    }

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) goto done;

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != vs) { av_packet_unref(pkt); continue; }
        int rc = avcodec_send_packet(dec, pkt);
        av_packet_unref(pkt);
        if (rc < 0) break;
        rc = avcodec_receive_frame(dec, frame);
        if (rc == 0) break;
        if (rc != AVERROR(EAGAIN)) break;
    }
    if (frame->width <= 0 || frame->height <= 0) goto done;

    {
        int tw = frame->width, th = frame->height;
        if (tw > kMaxDim || th > kMaxDim) {
            double sc = (double)kMaxDim / (double)(tw > th ? tw : th);
            tw = (int)(tw * sc);
            th = (int)(th * sc);
            if (tw < 1) tw = 1;
            if (th < 1) th = 1;
        }

        sws = sws_getContext(frame->width, frame->height,
                             (AVPixelFormat)frame->format,
                             tw, th, AV_PIX_FMT_BGRA,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) goto done;

        out.assign((size_t)tw * (size_t)th, 0);
        uint8_t* dst[4] = { (uint8_t*)out.data(), nullptr, nullptr, nullptr };
        int dst_ls[4] = { tw * 4, 0, 0, 0 };
        sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dst_ls);

        /*
         * Premultiply. swscale gives straight alpha; the framebuffer format is
         * 0xAABBGGRR with premultiplied colour, and getting this wrong shows up
         * as haloed poster edges rather than as anything that looks like a bug.
         */
        for (uint32_t& px : out) {
            uint32_t a = (px >> 24) & 0xFF;
            if (a == 255) continue;
            uint32_t b = ((px >> 16) & 0xFF) * a / 255;
            uint32_t g = ((px >> 8) & 0xFF) * a / 255;
            uint32_t rr = (px & 0xFF) * a / 255;
            px = (a << 24) | (b << 16) | (g << 8) | rr;
        }

        ow = tw;
        oh = th;
        ok = true;
    }

done:
    if (sws) sws_freeContext(sws);
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    if (dec) avcodec_free_context(&dec);
    if (fmt) avformat_close_input(&fmt);
    return ok;
}

#endif /* EVO_PROVIDER_ART_NO_DECODE */

/* ------------------------------------------------------------------------- */
/* Download                                                                  */
/* ------------------------------------------------------------------------- */

struct FetchCtx {
    std::string key;
    /* The cache path without an extension. on_art() appends the one it sniffs
     * from the bytes, because the type is not known before they arrive. */
    std::string base;
};

void on_art(int success, int status, const char* body, size_t len, void* ud)
{
    FetchCtx* ctx = (FetchCtx*)ud;
    ArtState& s = S();
    if (s.in_flight > 0) s.in_flight--;

    auto it = s.by_key.find(ctx->key);
    if (it == s.by_key.end()) { delete ctx; return; }

    if (!success || status != 200 || !body || len == 0) {
        /* Remember the failure so the same broken URL is not retried on every
         * page turn - a poster wall of 404s would otherwise be 500 requests
         * per navigation. */
        ART_LOG("art FETCH fail status=%d ok=%d len=%zu key=%s",
                status, success, len, ctx->key.c_str());
        it->second.failed = true;
        delete ctx;
        return;
    }

    /* Name the file for what the bytes actually are (see sniff_ext), then make
     * sure the directory exists: the provider's "art/" level - and, when the UI
     * is embedded rather than fetched, the provider directory above it - is not
     * created by anything else, so this fopen used to fail every time. */
    const char* ext = sniff_ext(body, len);
    if (!ext) {
        ART_LOG("art FORMAT unsupported magic=%02x%02x%02x%02x len=%zu key=%s",
                (unsigned char)body[0], (unsigned char)(len > 1 ? body[1] : 0),
                (unsigned char)(len > 2 ? body[2] : 0),
                (unsigned char)(len > 3 ? body[3] : 0), len, ctx->key.c_str());
        it->second.failed = true;
        delete ctx;
        return;
    }
    it->second.file = ctx->base + ext;

    if (evo_bundle_ensure_parent_dirs(it->second.file.c_str()) != 0) {
        ART_LOG("art MKDIR fail path=%s", it->second.file.c_str());
        it->second.failed = true;
        delete ctx;
        return;
    }

    FILE* f = fopen(it->second.file.c_str(), "wb");
    if (!f) {
        ART_LOG("art OPEN fail path=%s", it->second.file.c_str());
        it->second.failed = true;
        delete ctx;
        return;
    }
    size_t put = fwrite(body, 1, len, f);
    bool wrote = (put == len) && (fclose(f) == 0);
    if (!wrote) {
        remove(it->second.file.c_str());
        it->second.failed = true;
        delete ctx;
        return;
    }

    s.arrived.push_back(ctx->key);
    delete ctx;
}

} /* namespace */

/* ------------------------------------------------------------------------- */
/* Public API - see evo_provider_bundle.h for the contract                   */
/* ------------------------------------------------------------------------- */

extern "C" int evo_provider_art_request(const char* provider_id, const char* url,
                                        char* out_key, size_t out_sz)
{
    if (!url || !*url || !out_key || out_sz == 0) return -1;
    /* Only http(s). A provider item's art_url is data from a remote service,
     * so a "file:///..." in it must not turn into a local read. */
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
        return -1;

    ArtState& s = S();
    std::string h = url_hash(url);
    std::string key = "evo:mem/prov-" + h;
    if (key.size() + 1 > out_sz) return -1;
    memcpy(out_key, key.c_str(), key.size() + 1);

    auto it = s.by_key.find(key);
    if (it != s.by_key.end()) {
        if (it->second.failed) return -1;
        if (it->second.ready) { touch(key); return 1; }
        /* Bytes are on disk from a previous session or an earlier eviction:
         * queue a decode rather than a download. An entry still in flight has
         * no filename yet, so there is nothing to look for. */
        if (!it->second.file.empty()) {
            FILE* f = fopen(it->second.file.c_str(), "rb");
            if (f) { fclose(f); s.arrived.push_back(key); }
        }
        return 0;
    }

    /* Where the raw bytes go. Through evo_bundle_path() because that is the
     * only sanctioned way to build a path inside a provider's directory, even
     * for a name this file generated itself. The extension is deliberately
     * absent here - it is whatever on_art() sniffs from the bytes. */
    char base[512];
    std::string rel = "art/" + h;
    if (evo_bundle_path(provider_id, rel.c_str(), base, sizeof base) != 0)
        return -1;

    Entry e;
    e.key = key;
    s.by_key.emplace(key, e);

    /* Already cached on disk from a previous run: no network at all. Which
     * extension it was written under is not recorded anywhere, so try each. */
    for (const char* ext : kArtExts) {
        std::string cached = std::string(base) + ext;
        FILE* f = fopen(cached.c_str(), "rb");
        if (f) {
            fclose(f);
            s.by_key[key].file = cached;
            s.arrived.push_back(key);
            return 0;
        }
    }

    if (s.in_flight >= kMaxInFlight) {
        /* Queue full. Do not erase or drop permanently - return -2 so the host
         * keeps the URL and tries again on a future tick when in-flight slots drain. */
        s.by_key.erase(key);
        return -2;
    }

    FetchCtx* ctx = new FetchCtx{key, base};
    if (evo_net_request_async("GET", url, nullptr, nullptr, 0, on_art, ctx) != 0) {
        delete ctx;
        s.by_key.erase(key);
        return -1;
    }
    s.in_flight++;
    return 0;
}

extern "C" int evo_provider_art_ready(const char* key)
{
    if (!key || !*key) return 0;
    ArtState& s = S();
    auto it = s.by_key.find(key);
    if (it == s.by_key.end() || !it->second.ready) return 0;
    touch(it->first);
    return 1;
}

extern "C" void evo_provider_art_poll(void)
{
    ArtState& s = S();
    if (s.arrived.empty()) return;

    EvoRenderBridge* r = EvoRmlApp::Instance().RenderBridge();
    if (!r) { s.arrived.clear(); return; }

    /*
     * At most a couple of decodes per frame. A poster wall's worth of PNG
     * decodes in one frame is a visible hitch, and the rows are already on
     * screen - the art filling in over the next few frames is the better
     * trade, and is what every storefront on this console does.
     */
    int budget = 2;
    while (!s.arrived.empty() && budget-- > 0) {
        std::string key = s.arrived.back();
        s.arrived.pop_back();

        auto it = s.by_key.find(key);
        if (it == s.by_key.end() || it->second.ready) continue;

        std::vector<uint32_t> px;
        int w = 0, h = 0;
        if (!decode_image(it->second.file.c_str(), px, w, h)) {
            ART_LOG("art DECODE fail path=%s", it->second.file.c_str());
            it->second.failed = true;
            /* A file that will not decode is not going to start; drop it so
             * the disk cache does not keep handing it back. */
            remove(it->second.file.c_str());
            continue;
        }

        /* One line for the first poster that works, so a log can distinguish
         * "the path is broken" from "this level simply has no art". */
        static bool s_logged_first_ok = false;
        if (!s_logged_first_ok) {
            s_logged_first_ok = true;
            ART_LOG("art OK first poster %dx%d key=%s", w, h, it->second.key.c_str());
        }

        r->SetMemoryTexture(it->second.key, px.data(), w, h);
        it->second.bytes = px.size() * sizeof(uint32_t);
        it->second.ready = true;
        s.used += it->second.bytes;
        touch(key);
    }

    evict_to_budget();
    EvoRmlApp::Instance().MarkFrameDirty();
}

extern "C" void evo_provider_art_set_budget(size_t bytes)
{
    S().budget = bytes;
    evict_to_budget();
}

extern "C" void evo_provider_art_clear(void)
{
    ArtState& s = S();
    EvoRenderBridge* r = EvoRmlApp::Instance().RenderBridge();
    for (auto& kv : s.by_key)
        if (kv.second.ready && r) r->DropMemoryTexture(kv.second.key);
    s.by_key.clear();
    s.lru.clear();
    s.arrived.clear();
    s.used = 0;
}
