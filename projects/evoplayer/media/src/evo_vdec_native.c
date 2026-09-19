/*
 * evo_vdec_native.c — sceVideodec2 (Route B) hardware-decode backend for the
 * evo_vdec.h seam. Native-decode plan Phase 4 (#31).
 *
 * A port of the hardware-verified sceVideodec2 bring-up / decode / teardown
 * sequence proven inside the full PPSA99039 player on 2026-09-03 (a 1920x1088
 * NV12 H.264 frame, every call 0). The ABI transcription lives in
 * third_party/ps5-hardware-video-decoding-research/ and docs/evo-pro/videodec2-abi.md.
 *
 * Real body only under EVO_APP_MODULE — sceVideodec2, a user session and the
 * GPU driver stack exist only in the registered app module. Host + payload
 * builds get the stubs at the bottom, so the dispatcher (evo_vdec_ffmpeg.c)
 * always downgrades to FFmpeg.
 *
 * CODEC-INDEPENDENT BACKEND (#41). One interface, one small mode table
 * (g_codec[]): H.264, HEVC Main and VP9 Profile 0, all 8-bit 4:2:0 -> NV12.
 * open / send / receive / present are shared; only the AU->submit adaptation
 * (bitstream filter, VP9 superframe split + hidden-frame suppression) is
 * per-codec, and it hides behind the same struct. HEVC Main10 and VP9
 * Profile 2 are 10-bit two-plane and stay on FFmpeg until the P010 present
 * lands (#41 section 4); AV1 has no sceVideodec2 route at all.
 *
 * RESIDENT DECODERS — the sequencing constraint, learned on hardware 2026-09-03.
 * The self-unjail (evo_jailbreak_self / _ensure) swaps process credentials
 * mid-run, after which EVERY libSceVideodec2 call fails
 * (sceVideodec2QueryComputeMemoryInfo -> 0x811D0111), not just
 * sceSysmoduleLoadModule. So each decoder is brought up ONCE at boot in
 * evo_vdec_native_probe() — before the first evo_jailbreak_self() — as a
 * session-resident slot (g_dec[]). Lazy per-playback creation is impossible:
 * by open() time the unjail has already happened. Each evo_vdec_native_open()
 * just sceVideodec2Reset()s the matching slot and feeds it; nothing is created
 * or destroyed at playback time.
 *
 * MEMORY. Three resident 4K decoders is a lot of direct memory against the
 * fake-signed budget - measured at ~1.76 GB total on hardware 2026-09-11
 * (AVC 505 MB + HEVC 549 MB + VP9 748 MB), all three at 4K, nothing failed.
 * A secondary bring-up failure is non-fatal: that codec falls back to FFmpeg
 * and the AVC slot stays up.
 *
 * DEMUX -> AU FORMAT. sceVideodec2 wants Annex-B for AVC/HEVC (start-code NALs,
 * VPS/SPS/PPS in-band). For mp4/mkv (avcC/hvcC extradata) we run AUs through
 * h264_mp4toannexb / hevc_mp4toannexb; already-Annex-B streams pass through.
 * VP9 has no Annex-B: a compound superframe packet is rejected whole, so every
 * AU goes through vp9_superframe_split and hidden (alt-ref, show_frame=0)
 * coded frames are submitted for reference but their output is suppressed.
 *
 * FRAME ORDER. SceVideodec2OutputInfo carries no PTS and no picture-detail
 * hook is bound. This backend assumes DISPLAY-order emission + min-PTS pairing,
 * with a small PTS-sorted reorder window as a safety net. If B-frame content
 * misbehaves, build with -DEVO_VDEC_NATIVE_DECODE_ORDER=1 and a deeper
 * -DEVO_VDEC_REORDER_DEPTH.
 */
#include "evo_vdec_native.h"
#include "evo_boot_log.h"

#ifdef EVO_APP_MODULE

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/rational.h>

#include "sce/sce_videodec2.h"

/* FFmpeg profile-id fallbacks — the pinned build has the FF_PROFILE_* spelling
 * (main.c uses FF_PROFILE_HEVC_MAIN_10); keep numeric fallbacks in case that
 * changes to AV_PROFILE_*. Values are per-codec namespaced. */
#ifndef FF_PROFILE_UNKNOWN
#define FF_PROFILE_UNKNOWN       (-99)
#endif
#ifndef FF_PROFILE_HEVC_MAIN
#define FF_PROFILE_HEVC_MAIN     1
#endif
#ifndef FF_PROFILE_HEVC_MAIN_10
#define FF_PROFILE_HEVC_MAIN_10  2
#endif
#ifndef FF_PROFILE_VP9_0
#define FF_PROFILE_VP9_0         0
#endif
#ifndef FF_PROFILE_VP9_2
#define FF_PROFILE_VP9_2         2
#endif

/* --- externs (libkernel + libSceSysmodule, both already DT_NEEDED) --------- */
extern int      sceKernelSendNotificationRequest(int, void *, unsigned long, int);
extern int      sceSysmoduleLoadModule(uint16_t id);
extern int64_t  sceKernelGetDirectMemorySize(void);
extern int      sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t *);
extern int      sceKernelMapDirectMemory(void **, size_t, int, int, int64_t, size_t);
extern int      sceKernelReleaseDirectMemory(int64_t, size_t);
extern int      sceKernelMunmap(void *, size_t);
extern int      sceKernelMapNamedFlexibleMemory(void **, size_t, int, int, const char *);
extern int      sceKernelReleaseFlexibleMemory(void *, size_t);
extern int      sceKernelAvailableFlexibleMemorySize(size_t *);
extern int      sceKernelConfiguredFlexibleMemorySize(size_t *);

#define SCE_SYSMODULE_VIDEODEC2_NUM 207
/*
 * Input AU ring slot. Four of these per decoder, and every resident decoder
 * pays for its own ring whether or not it is the one playing.
 *
 * 16 MiB a slot was 64 MiB a decoder, 256 MiB across the four - for access
 * units that measure in the hundreds of kilobytes (the 4K GTA trailer's
 * keyframes log at ~165 KB). 8 MiB still leaves roughly fifty times the
 * largest AU actually observed, and gives 128 MB back to the flexible-memory
 * pool that FFmpeg, the poster extractor and everything else share.
 */
#define INPUT_SLOT_BYTES            0x800000u   /* 8 MiB — ~50x the largest observed 4K IDR */

/* Match SharpProspero's production config: DPB auto-sized by the decoder
 * (MaxDpbFrameCount = -1), DecodeInputQueueDepth = 4. The frame buffers we
 * hand sceVideodec2Decode are output/detile targets consumed immediately
 * (ro_harvest copies out); the decoder keeps its own DPB internally in
 * GpuMemory. A small ring with margin over the input-queue depth suffices. */
#define SCE_VIDEODEC2_AUTO_FRAMES   (-1)
#define DECODE_INPUT_QUEUE_DEPTH    4u
#define PIPELINE_BUFFER_COUNT       4u          /* input-AU ring */
/*
 * Output/detile frame ring. These are targets handed to sceVideodec2Decode and
 * consumed immediately - ro_harvest copies out of them - while the decoder
 * keeps its real DPB internally in GpuMemory. The ring only has to stay ahead
 * of the input queue (4) and the reorder window (RO_SLOTS = 5), so 12 slots of
 * a full 4K frame each (~12 MiB) was ~50 MB per decoder of margin nobody was
 * using. 8 keeps three spare slots beyond the deepest consumer.
 */
#define FRAME_POOL_SLOTS            8

#ifndef EVO_VDEC_REORDER_DEPTH
#define EVO_VDEC_REORDER_DEPTH      4
#endif
#define RO_SLOTS   (EVO_VDEC_REORDER_DEPTH + 1)
#define PTS_POOL   (EVO_VDEC_REORDER_DEPTH + (int)PIPELINE_BUFFER_COUNT + 8)

/* Resident decoder size. 4K AVC High covers the case native decode exists for
 * (4K H.264 that blows the software frame pool — e.g. the GTA trailer). */
#ifndef EVO_VDEC_NATIVE_MAX_W
#define EVO_VDEC_NATIVE_MAX_W  3840
#endif
#ifndef EVO_VDEC_NATIVE_MAX_H
#define EVO_VDEC_NATIVE_MAX_H  2176
#endif

/* Secondary (HEVC / VP9) resident decoders — ON by default since 2026-09-11 (#41).
 *
 * Bring-up, decode and the direct-memory footprint are all hardware-verified,
 * two sessions:
 *   2026-09-10, 1080p: AVC/HEVC/VP9 bring-up rc=0; decode HEVC 475 frames,
 *     VP9 477 frames (vp9_superframe_split, real alt-ref superframes), both
 *     fatal=0, pitch 2048 (matches the research repo's documented 8-bit
 *     1080p pitch).
 *   2026-09-11, 4K: all three bring up at 4K with NO fallback-to-1080p retry
 *     firing; decode AVC 437 frames, HEVC 355 frames (both fatal=0), VP9
 *     clean mid-session heartbeat with the superframe splitter engaged.
 *     Direct-memory footprint ~1.76 GB total (AVC 505 MB + HEVC 549 MB +
 *     VP9 748 MB) against an unmeasured ceiling — nothing failed, but this
 *     is one clip per codec in one session each, not a soak test.
 *
 * The earlier "crashed before any diagnostic flushed" 2026-09-10 attempt does
 * NOT reproduce, and was likely never this code's fault: note() was blind at
 * the time, buffering into evo_boot_log pre-unjail, so a crash there would
 * have taken every diagnostic line with it regardless of cause.
 *
 * Build -DEVO_VDEC_NATIVE_SECONDARY=0 (package-app.sh --no-native-secondary)
 * to go back to AVC-only. */
#ifndef EVO_VDEC_NATIVE_SECONDARY
#define EVO_VDEC_NATIVE_SECONDARY 1
#endif

/* DEFAULT-ON since 2026-09-11 (see the verification note above). Build
 * -DEVO_VDEC_NATIVE_SECONDARY_4K=0 (package-app.sh --no-native-secondary-4k)
 * to drop HEVC/VP9 back to 1080p while keeping them on. */
#ifndef EVO_VDEC_NATIVE_SECONDARY_4K
#define EVO_VDEC_NATIVE_SECONDARY_4K 1
#endif
#if EVO_VDEC_NATIVE_SECONDARY_4K
#define EVO_VDEC_NATIVE_SECONDARY_MAX_W  EVO_VDEC_NATIVE_MAX_W
#define EVO_VDEC_NATIVE_SECONDARY_MAX_H  EVO_VDEC_NATIVE_MAX_H
#else
#define EVO_VDEC_NATIVE_SECONDARY_MAX_W  1920
#define EVO_VDEC_NATIVE_SECONDARY_MAX_H  1088
#endif

/*
 * VP9 runs at the same size as the other secondary decoders.
 *
 * It was capped at 1080p to reclaim the 748 MB its 4K slot reserves, on the
 * belief that the resident decoders were exhausting a ~2 GB budget. That
 * belief was wrong: the figure summed the decoders' DIRECT memory, and the
 * pool that actually runs out is flexible memory, measured at boot as 448 MB
 * configured with the decoders taking 179 MB of it. Direct memory was never
 * the constraint, so the cap cost 4K VP9 hardware decode and bought nothing.
 * Override these if a real measurement ever says otherwise.
 */
#ifndef EVO_VDEC_NATIVE_VP9_MAX_W
#define EVO_VDEC_NATIVE_VP9_MAX_W  EVO_VDEC_NATIVE_SECONDARY_MAX_W
#endif
#ifndef EVO_VDEC_NATIVE_VP9_MAX_H
#define EVO_VDEC_NATIVE_VP9_MAX_H  EVO_VDEC_NATIVE_SECONDARY_MAX_H
#endif

/* #41 Phase D: the two 10-bit resident decoders (HEVC Main10, VP9 Profile 2).
 *
 * WARNING - this default contradicts the finding recorded against it. On
 * 2026-09-11 default-on was judged wrong: with the three from Phase B already
 * resident, adding these two left only ~3 MB of flex memory free AT BOOT -
 * not during playback, at boot, before any file is opened - breaking the home
 * screen's own thumbnail decode for the Recent/Jump Back In shelf
 * (get_buffer() failed, avail=3M, fail climbing). The revert to off was
 * written down here and in package-app.sh but never actually applied: both
 * still default it on.
 *
 * That is very likely what the 2026-09-17 poster crashes are - the same
 * get_buffer() ENOMEM, in the same thumbnail decode, from the same cause.
 * Build -DEVO_VDEC_NATIVE_10BIT=0 (package-app.sh --no-native-10bit) to give
 * the flex memory back; 10-bit then decodes on the FFmpeg CPU path as it did
 * before Phase D.
 *
 * Decode + the pitch math are hardware-verified either way (see the comment
 * above EVO_VDEC_NATIVE_SECONDARY); what is in question is only whether the
 * console can afford both these slots and everything else, which is #38's
 * flex-memory budget. */
#ifndef EVO_VDEC_NATIVE_10BIT
#define EVO_VDEC_NATIVE_10BIT 1
#endif

/* ---- codec-independent mode table (#41) ---------------------------------- */
typedef enum {
    NAT_H264   = 0,
    NAT_HEVC   = 1,
    NAT_VP9    = 2,
    NAT_HEVC10 = 3,
    NAT_VP92   = 4,
    NAT_CODEC_COUNT
} nat_codec;

typedef struct {
    nat_codec    idx;
    int          codec_id;      /* AVCodecID from the demuxer                 */
    uint32_t     codec_type;    /* SCE_VIDEODEC2_CODEC_*                      */
    uint32_t     profile_cfg;   /* profile_idc handed to CreateDecoder        */
    int          level_1080;    /* max_level for <=1080p                      */
    int          level_4k;      /* max_level for >1080p                       */
    const char  *bsf_name;      /* AU adaptation bitstream filter, or NULL    */
    int          is_vp9;        /* always run the bsf; suppress hidden frames */
    const char  *tag;
    /* #38/#41: per-codec CreateDecoder pipeline depth. The research repo
     * documents this as a real memory/throughput tradeoff ("depth 1 is the
     * production default; deeper improves throughput but raises frame
     * residency") - it feeds the decoder's own reported flex-memory need
     * (mem.cpu_size / QueryDecoderMemoryInfo), a separate, previously
     * unmeasured pool from the five direct-memory fields. The three proven
     * codecs keep DECODE_INPUT_QUEUE_DEPTH (4) unchanged; the two 10-bit
     * decoders - low-priority, off by default, no throughput requirement -
     * get a shallower one to shrink their flex footprint. */
    int          pipeline_depth;
} nat_codec_desc;

/* max_level scale is per-codec: AVC = level x10 (51 = 5.1); HEVC =
 * general_level_idc = level x30 (123/150/153 for 1080/1440/2160, as
 * ProsperoLight passes); VP9 x10 is a best guess pending hardware.
 * #41 Phase D: HEVC10 (Main10, profile_cfg=2, level 4.1=123) and
 * VP9-2 (Profile 2, profile_cfg=2, level 4.1=41) resident decoders (1080p only). */
static const nat_codec_desc g_codec[NAT_CODEC_COUNT] = {
    { NAT_H264,   AV_CODEC_ID_H264, SCE_VIDEODEC2_CODEC_AVC,  100,  51,  52,
      "h264_mp4toannexb",     0, "AVC",    DECODE_INPUT_QUEUE_DEPTH },
    { NAT_HEVC,   AV_CODEC_ID_HEVC, SCE_VIDEODEC2_CODEC_HEVC,   1, 123, 153,
      "hevc_mp4toannexb",     0, "HEVC",   DECODE_INPUT_QUEUE_DEPTH },
    { NAT_VP9,    AV_CODEC_ID_VP9,  SCE_VIDEODEC2_CODEC_VP9,    0,  41,  51,
      "vp9_superframe_split", 1, "VP9",    DECODE_INPUT_QUEUE_DEPTH },
    /* #38: shallower pipeline (2, not the proven codecs' 4) to shrink the
     * decoder's own reported flex-memory need - these two are off by default
     * and have no throughput requirement to justify the deeper pipeline. */
    { NAT_HEVC10, AV_CODEC_ID_HEVC, SCE_VIDEODEC2_CODEC_HEVC,   2, 123, 123,
      "hevc_mp4toannexb",     0, "HEVC10", 1 },
    { NAT_VP92,   AV_CODEC_ID_VP9,  SCE_VIDEODEC2_CODEC_VP9,    2,  41,  41,
      "vp9_superframe_split", 1, "VP9-2",  1 },
};

static const nat_codec_desc *codec_desc_for(int codec_id, int profile, int bit_depth)
{
    if (codec_id == AV_CODEC_ID_H264)
        return &g_codec[NAT_H264];
    if (codec_id == AV_CODEC_ID_HEVC) {
        if (bit_depth == 10 || profile == FF_PROFILE_HEVC_MAIN_10)
            return &g_codec[NAT_HEVC10];
        return &g_codec[NAT_HEVC];
    }
    if (codec_id == AV_CODEC_ID_VP9) {
        if (bit_depth == 10 || profile == FF_PROFILE_VP9_2)
            return &g_codec[NAT_VP92];
        return &g_codec[NAT_VP9];
    }
    return NULL;
}

/* Decoder notes go to /mnt/usb0/evo.log (via evo_boot_log — one file, shared
 * with the boot trace + breadcrumbs). They are already gated at the call sites
 * (first 3 decodes, first error, a 300-decode heartbeat) so they never spam.
 * The notification popup shares evo_bt()'s EVO_BOOT_TRACE_POPUP opt-in
 * (scripts/package-app.sh --breadcrumbs) — it used to be unconditional and
 * was reported as recurring "EVO vdec native:" popups mid-playback. */
#if defined(EVO_BOOT_TRACE_POPUP)
struct v2n_note { char pad[45]; char msg[3075]; };
#endif
extern int sceKernelDebugOutText(int, const char *);   /* -> klog, live */

static void note(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

#if defined(EVO_BOOT_TRACE_POPUP)
    struct v2n_note n;
    memset(&n, 0, sizeof n);
    snprintf(n.msg, sizeof n.msg, "%s", msg);
    sceKernelSendNotificationRequest(0, &n, sizeof n, 0);
#endif

    evo_boot_log("%s", msg);

    /*
     * Also straight to klog, live — the reason #41's HEVC/VP9 bring-up was
     * unobservable.
     *
     * evo_vdec_native_probe() runs PRE-UNJAIL, and evo_boot_log only buffers in
     * memory until /mnt/usb0 opens after evo_jailbreak_self(). So a fault
     * anywhere inside a bring-up takes every line describing where it got to
     * down with it: "crashed pre-log", which is exactly how the 2026-09-10
     * attempt presented. The kernel writes klog straight through, so it
     * survives the process dying. Costs nothing — this is boot + gated
     * playback notes, never a per-frame path. Capture with tools/klog.sh.
     */
    char line[1088];
    snprintf(line, sizeof line, "%s\n", msg);
    sceKernelDebugOutText(0, line);
}

static size_t align16k(size_t v) { return (v + 0x3fffu) & ~(size_t)0x3fffu; }
static int    roundup16(int v)   { return (v + 15) & ~15; }

static int alloc_direct(size_t size, int prot, int64_t limit,
                        int64_t *start, void **addr)
{
    int rc = sceKernelAllocateDirectMemory(0, limit, size, 0x4000, 12, start);
    if (rc == 0)
        rc = sceKernelMapDirectMemory(addr, size, prot, 0, *start, 0x4000);
    return rc;
}
static void free_direct(void *addr, int64_t start, size_t size)
{
    if (addr)       sceKernelMunmap(addr, size);
    if (start >= 0) sceKernelReleaseDirectMemory(start, size);
}

/* ---------------------------------------------------------------------------
 * Session-resident decoders — one slot per codec, each created ONCE, pre-unjail,
 * in evo_vdec_native_probe().
 * ------------------------------------------------------------------------ */

struct dec_slot {
    int      ready;
    /*
     * One resident decoder per codec, handed out by evo_vdec_native_open() and
     * sceVideodec2Reset() on the way in - so a second opener would wipe the
     * first one's state mid-stream. Harmless while playback was the only
     * caller; the poster extractor is a second one. Claim the slot on open,
     * release it on close, and refuse an open that would collide: the
     * dispatcher then falls back to FFmpeg, which is exactly right for a
     * poster while a file is playing.
     */
    volatile int owned;
    void    *compute_queue;
    void    *compute_mem;   int64_t compute_start; size_t compute_size;
    void    *decoder;
    void    *cpu_mem;       size_t  cpu_map;
    void    *gpu_mem;       int64_t gpu_start;     size_t gpu_size;
    void    *cpu_gpu_mem;   int64_t cpu_gpu_start; size_t cpu_gpu_size;
    void    *input_mem;     int64_t input_start;   size_t input_pool;
    void    *frame_mem;     int64_t frame_start;   size_t frame_pool;
    size_t   frame_size;
    uint32_t max_w, max_h;
};

static int             g_boot_tried;
static int             g_boot_any;
static struct dec_slot g_dec[NAT_CODEC_COUNT];

static void slot_teardown(struct dec_slot *s)
{
    if (s->decoder) { sceVideodec2DeleteDecoder(s->decoder); s->decoder = NULL; }
    free_direct(s->frame_mem,   s->frame_start,   s->frame_pool);
    free_direct(s->input_mem,   s->input_start,   s->input_pool);
    free_direct(s->cpu_gpu_mem, s->cpu_gpu_start, s->cpu_gpu_size);
    free_direct(s->gpu_mem,     s->gpu_start,     s->gpu_size);
    if (s->cpu_mem) {
        sceKernelReleaseFlexibleMemory(s->cpu_mem, s->cpu_map);
        sceKernelMunmap(s->cpu_mem, s->cpu_map);
    }
    if (s->compute_queue) { sceVideodec2ReleaseComputeQueue(s->compute_queue); s->compute_queue = NULL; }
    free_direct(s->compute_mem, s->compute_start, s->compute_size);
    memset(s, 0, sizeof *s);
}

/*
 * Name each step as it is entered, not only when one returns an error.
 *
 * Half of these calls are Sony code that can fault rather than return: a bad
 * codec_type or max_level takes the process with it, and then the rc-on-failure
 * report never runs. The breadcrumb before the call is the only thing that
 * survives — the last klog line names what killed it. #41's HEVC/VP9 bring-up
 * was invisible for exactly this reason. Boot-only (at most three slots, a
 * dozen lines each), so it is not on any hot path.
 */
#define STAGE(name) do {                                                       \
        *stage = (name);                                                       \
        note("EVO vdec native: %s %dx%d [%s]", d->tag, w, h, *stage);          \
    } while (0)

/* Full bring-up for `d`'s decoder at (w x h). Returns 0 on success with
 * everything stored in `*s`; non-zero rc (and `*s` left torn down) on any
 * failure. MUST be called before the first evo_jailbreak_self(). */
static int slot_bringup(struct dec_slot *s, const nat_codec_desc *d,
                        int w, int h, const char **stage)
{
    int64_t dm = sceKernelGetDirectMemorySize();
    int rc;

    s->compute_start = s->gpu_start = s->cpu_gpu_start =
        s->input_start = s->frame_start = -1;

    SceVideodec2ComputeMemoryInfo cm;
    SceVideodec2ComputeConfigInfo cc;
    memset(&cm, 0, sizeof cm);
    memset(&cc, 0, sizeof cc);
    cm.size = sizeof cm;
    STAGE("QueryComputeMemoryInfo");
    if ((rc = sceVideodec2QueryComputeMemoryInfo(&cm)) != 0) return rc;
    s->compute_size = align16k((size_t)cm.cpu_gpu_size);
    STAGE("alloc(compute)");
    if ((rc = alloc_direct(s->compute_size, 0x33, dm,
                           &s->compute_start, &s->compute_mem)) != 0) return rc;
    cm.cpu_gpu      = s->compute_mem;
    cm.cpu_gpu_size = s->compute_size;
    cc.size = sizeof cc;
    STAGE("AllocateComputeQueue");
    if ((rc = sceVideodec2AllocateComputeQueue(&cc, &cm, &s->compute_queue)) != 0) return rc;
    if (!s->compute_queue) return -1;

    SceVideodec2DecoderConfigInfo config;
    SceVideodec2DecoderMemoryInfo mem;
    memset(&config, 0, sizeof config);
    memset(&mem, 0, sizeof mem);
    config.size                 = sizeof config;
    config.resource_type        = SCE_VIDEODEC2_RESOURCE_COMPUTE;
    config.codec_type           = d->codec_type;
    config.profile              = d->profile_cfg;
    config.max_level            = (w > 1920 || h > 1088) ? d->level_4k : d->level_1080;
    config.max_width            = w;
    config.max_height           = h;
    config.max_dpb_frames       = SCE_VIDEODEC2_AUTO_FRAMES;   /* decoder self-sizes */
    config.pipeline_depth       = (uint32_t)d->pipeline_depth;
    config.compute_queue        = (uint64_t)s->compute_queue;
    config.cpu_affinity         = 0x3f;
    config.cpu_priority         = 700;
    config.optimize_progressive = 1;

    mem.size = sizeof mem;
    /* The three values under suspicion for the 2026-09-10 crash, on the record
     * before the first call that consumes them: VP9's codec_type tag is marked
     * unverified in sce_videodec2.h, and HEVC's max_level x30 scale is inferred
     * from ProsperoLight rather than measured. */
    note("EVO vdec native: %s config codec_type=%u profile=%u max_level=%d "
         "max=%dx%d dpb=auto depth=%u",
         d->tag, (unsigned)config.codec_type, (unsigned)config.profile,
         (int)config.max_level, w, h, (unsigned)config.pipeline_depth);
    STAGE("QueryDecoderMemoryInfo");
    if ((rc = sceVideodec2QueryDecoderMemoryInfo(&config, &mem)) != 0) return rc;

    s->cpu_map = align16k((size_t)mem.cpu_size);
    STAGE("MapNamedFlexibleMemory");
    if ((rc = sceKernelMapNamedFlexibleMemory(&mem.cpu, s->cpu_map, 0x03, 0,
                                              "EvoVdecNative")) != 0) return rc;
    if (!mem.cpu) return -1;
    s->cpu_mem = mem.cpu;

    s->gpu_size     = align16k((size_t)mem.gpu_size);
    s->cpu_gpu_size = align16k((size_t)mem.cpu_gpu_size);
    s->frame_size   = align16k((size_t)mem.max_frame_size);
    s->input_pool   = (size_t)INPUT_SLOT_BYTES * PIPELINE_BUFFER_COUNT;
    s->frame_pool   = s->frame_size * FRAME_POOL_SLOTS;
    if (s->frame_size == 0) return -1;
    mem.gpu_size = s->gpu_size;
    if (s->cpu_gpu_size) mem.cpu_gpu_size = s->cpu_gpu_size;

    STAGE("alloc(gpu)");
    if ((rc = alloc_direct(s->gpu_size, 0x33, dm, &s->gpu_start, &s->gpu_mem)) != 0) return rc;
    mem.gpu = s->gpu_mem;
    if (s->cpu_gpu_size) {
        STAGE("alloc(cpu_gpu)");
        if ((rc = alloc_direct(s->cpu_gpu_size, 0x33, dm,
                               &s->cpu_gpu_start, &s->cpu_gpu_mem)) != 0) return rc;
        mem.cpu_gpu = s->cpu_gpu_mem;
    }
    STAGE("alloc(input)");
    if ((rc = alloc_direct(s->input_pool, 0x33, dm,
                           &s->input_start, &s->input_mem)) != 0) return rc;
    STAGE("alloc(frame)");
    if ((rc = alloc_direct(s->frame_pool, 0x33, dm,
                           &s->frame_start, &s->frame_mem)) != 0) return rc;

    STAGE("CreateDecoder");
    if ((rc = sceVideodec2CreateDecoder(&config, &mem, &s->decoder)) != 0) return rc;
    if (!s->decoder) return -1;
    STAGE("Reset");
    if ((rc = sceVideodec2Reset(s->decoder)) != 0) return rc;

    s->max_w = (uint32_t)w;
    s->max_h = (uint32_t)h;
    STAGE("ok");
    return 0;
}

static int g_prefer_nv12 = 0;
void evo_vdec_native_prefer_nv12(int on) { g_prefer_nv12 = on ? 1 : 0; }

/* Bring up one codec slot with an automatic 1080p retry on a 4K failure.
 * `required` streams also log at failure; a non-required (HEVC/VP9) failure is
 * quiet-ish and just leaves that slot !ready. */
static void probe_slot(nat_codec c, int w, int h, int required, unsigned sm)
{
    struct dec_slot *s = &g_dec[c];
    const nat_codec_desc *d = &g_codec[c];
    const char *stage = "?";

    int rc = slot_bringup(s, d, w, h, &stage);
    if (rc != 0 && (w > 1920 || h > 1088)) {
        note("EVO vdec native: %s 4K bring-up FAILED at [%s] rc=0x%08x - trying 1080p",
             d->tag, stage, (unsigned)rc);
        slot_teardown(s);
        rc = slot_bringup(s, d, 1920, 1088, &stage);
    }
    if (rc == 0) {
        s->ready   = 1;
        g_boot_any = 1;
        /* flex=cpu_map: the ONE flexible-memory allocation per slot
         * (sceKernelMapNamedFlexibleMemory, sized from the decoder's own
         * QueryDecoderMemoryInfo response) - a separate pool from the five
         * direct-memory fields above it, and the one FFmpeg's own
         * get_buffer()/av_malloc calls actually compete against (#38). Never
         * logged before 2026-09-11 because the flex-vs-direct distinction
         * was found only after HEVC10/VP9-2 broke home-screen thumbnail
         * decode - this is what should have been measured from the start. */
        note("EVO vdec native: RESIDENT %s decoder up  %ux%u  frame=%zuKB  "
             "total=%zuKB (compute=%zuKB gpu=%zuKB cpu_gpu=%zuKB input=%zuKB frame_pool=%zuKB)  "
             "flex=%zuKB  sysmod=0x%08x",
             d->tag, s->max_w, s->max_h, s->frame_size >> 10,
             (s->compute_size + s->gpu_size + s->cpu_gpu_size + s->input_pool + s->frame_pool) >> 10,
             s->compute_size >> 10, s->gpu_size >> 10, s->cpu_gpu_size >> 10,
             s->input_pool >> 10, s->frame_pool >> 10, s->cpu_map >> 10, sm);
        return;
    }
    note("EVO vdec native: %s bring-up FAILED at [%s] rc=0x%08x sysmod=0x%08x -> %s",
         d->tag, stage, (unsigned)rc, sm,
         required ? "no AVC native decode" : "FFmpeg for that codec");
    slot_teardown(s);
}

int evo_vdec_native_probe(void)
{
    if (g_boot_tried)
        return g_boot_any;
    g_boot_tried = 1;

    unsigned sm = (unsigned)sceSysmoduleLoadModule(SCE_SYSMODULE_VIDEODEC2_NUM);

    /*
     * Book-end the bring-up with the flexible-memory figure. Everything the
     * app does afterwards - FFmpeg, poster extraction, swscale - comes out of
     * what is left, and until now the budget was inferred from the decoders'
     * own totals rather than measured against the pool they come from.
     */
    size_t flex_before = 0, flex_total = 0;
    if (sceKernelAvailableFlexibleMemorySize(&flex_before) != 0)
        flex_before = 0;
    /* The ceiling, not just what is left of it. Every "budget" judgement in
     * this file has so far been made against the decoders' own self-reported
     * totals with no idea what they were a fraction OF. */
    if (sceKernelConfiguredFlexibleMemorySize(&flex_total) != 0)
        flex_total = 0;
    note("EVO vdec native: flex pool configured=%zuMB available=%zuMB before bring-up",
         flex_total >> 20, flex_before >> 20);

    /* AVC — the compatibility baseline, 4K (with the #31 1080p retry). */
    probe_slot(NAT_H264, EVO_VDEC_NATIVE_MAX_W, EVO_VDEC_NATIVE_MAX_H, 1, sm);

#if EVO_VDEC_NATIVE_SECONDARY
    /* HEVC + VP9 — secondary, non-fatal, independent of the AVC result.
     * Gated: see the EVO_VDEC_NATIVE_SECONDARY note above (2026-09-10 crash). */
    probe_slot(NAT_HEVC, EVO_VDEC_NATIVE_SECONDARY_MAX_W,
               EVO_VDEC_NATIVE_SECONDARY_MAX_H, 0, sm);
    probe_slot(NAT_VP9,  EVO_VDEC_NATIVE_VP9_MAX_W,
               EVO_VDEC_NATIVE_VP9_MAX_H, 0, sm);

#if EVO_VDEC_NATIVE_10BIT
    /* #41 Phase D: 10-bit resident decoders (HEVC Main10).
     * 1080p only (1920x1088), non-fatal. Depth 1 keeps flexible memory footprint
     * minimal (~14MB). VP9-2 is omitted to protect the flex-memory budget. */
    probe_slot(NAT_HEVC10, 1920, 1088, 0, sm);
#endif
#endif

    {
        size_t flex_after = 0;
        if (sceKernelAvailableFlexibleMemorySize(&flex_after) != 0)
            flex_after = 0;
        note("EVO vdec native: flex after bring-up = %zuMB (resident decoders took %zuMB)",
             flex_after >> 20,
             flex_before > flex_after ? (flex_before - flex_after) >> 20 : (size_t)0);
    }

    if (!g_boot_any)
        note("EVO vdec native: no resident decoder -> FFmpeg only (sysmod=0x%08x)", sm);
    return g_boot_any;
}

int evo_vdec_native_supports(int codec_id, int profile, int bit_depth,
                             int w, int h)
{
    if (!evo_vdec_native_probe())
        return 0;
    const nat_codec_desc *d = codec_desc_for(codec_id, profile, bit_depth);
    if (!d)
        return 0;
    const struct dec_slot *s = &g_dec[d->idx];
    if (!s->ready)
        return 0;
    if (bit_depth > 10)
        return 0;
    if (bit_depth > 8 && d->idx != NAT_HEVC10 && d->idx != NAT_VP92)
        return 0;

    switch (codec_id) {
    case AV_CODEC_ID_H264:
        if (bit_depth > 8)
            return 0;
        /* Baseline / Main / High / constrained-High (578). Anything above
         * High (Hi10 / Hi422 / Hi444) the 8-bit NV12 decoder cannot do. */
        if (profile != FF_PROFILE_UNKNOWN && profile > 100 && profile != 578)
            return 0;
        break;
    case AV_CODEC_ID_HEVC:
        if (bit_depth == 10) {
            if (profile != FF_PROFILE_UNKNOWN && profile != FF_PROFILE_HEVC_MAIN_10)
                return 0;
        } else {
            if (profile != FF_PROFILE_UNKNOWN && profile != FF_PROFILE_HEVC_MAIN)
                return 0;
        }
        break;
    case AV_CODEC_ID_VP9:
        if (bit_depth == 10) {
            if (profile != FF_PROFILE_UNKNOWN && profile != FF_PROFILE_VP9_2)
                return 0;
        } else {
            if (profile != FF_PROFILE_UNKNOWN && profile != FF_PROFILE_VP9_0)
                return 0;
        }
        break;
    default:
        return 0;
    }

    if (w > 0 && h > 0) {
        int rw = roundup16(w), rh = roundup16(h);
        if (rw < 16 || rh < 16)
            return 0;
        if ((uint32_t)rw > s->max_w || (uint32_t)rh > s->max_h)
            return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * Per-playback wrapper — borrows g_dec[codec].decoder, never creates/destroys it.
 * ------------------------------------------------------------------------ */

/*
 * The decoder outputs NV12, but EVO's fast (SIMD / parallel / 4K) converters
 * only accept planar YUV420P — an NV12 4K frame silently produces a black
 * screen. So ro_harvest de-interleaves NV12 -> I420 into the slot: Y copied
 * as-is, the interleaved UV plane split into separate U and V planes. The
 * chroma pass is ~w*h/2 bytes — trivial next to the decode it replaces.
 */
struct nat_slot {
    uint8_t *data;
    size_t   cap;
    int      used;
    int      borrowed;         /* 1 = data points into the decoder frame     */
                               /*     pool (NV12 path), not owned - do not    */
                               /*     realloc or free it                     */
    int      nv12;              /* 1 = data is straight NV12 (GL video path)  */
    int64_t  pts;
    uint32_t w, h;              /* display size                             */
    uint32_t coded_h;           /* MB-padded luma rows (UV starts here)      */
    uint32_t y_stride, c_stride;
    size_t   u_off, v_off;      /* byte offsets of U and V within data       */
};

struct evo_vdec_native {
    const nat_codec_desc *desc;  /* selected codec mode                */
    struct dec_slot *slot;  /* the claimed resident slot, released on close */
    void    *dec;           /* == g_dec[desc->idx].decoder (borrowed)  */
    uint8_t *input_mem;     /* == g_dec[desc->idx].input_mem           */
    uint8_t *frame_mem;     /* == g_dec[desc->idx].frame_mem           */
    size_t   frame_size;

    uint32_t disp_w, disp_h;
    unsigned au_ring;
    unsigned dec_calls;
    unsigned frames_out;
    int      flushing;
    int      fatal;
    int      nv12_out;      /* emit NV12 straight through (GL samples it), not I420 */
    int      first_valid_logged;
    int      first_err_logged;
    int      color_trc;     /* AVColorTransferCharacteristic from demuxer */

    AVBSFContext        *bsf;
    const char          *bsf_name;   /* for rebuild on seek */
    AVCodecParameters   *bsf_par;    /* owned copy, for rebuild on seek */
    AVPacket            *in_pkt;
    AVPacket            *filt_pkt;

    int64_t pts_pool[PTS_POOL];
    int     pts_n;

    struct nat_slot ro[RO_SLOTS];
    int             ro_count;
};

static void pts_push(evo_vdec_native *n, int64_t pts)
{
    if (n->pts_n >= PTS_POOL) {
        memmove(n->pts_pool, n->pts_pool + 1, (PTS_POOL - 1) * sizeof n->pts_pool[0]);
        n->pts_n = PTS_POOL - 1;
    }
    n->pts_pool[n->pts_n++] = pts;
}

static int64_t pts_take(evo_vdec_native *n)
{
    if (n->pts_n <= 0)
        return INT64_MIN;
#ifdef EVO_VDEC_NATIVE_DECODE_ORDER
    int64_t v = n->pts_pool[0];
    memmove(n->pts_pool, n->pts_pool + 1, (n->pts_n - 1) * sizeof n->pts_pool[0]);
    n->pts_n--;
    return v;
#else
    int mi = 0;
    for (int i = 1; i < n->pts_n; i++) {
        if (n->pts_pool[i] == INT64_MIN) continue;
        if (n->pts_pool[mi] == INT64_MIN || n->pts_pool[i] < n->pts_pool[mi])
            mi = i;
    }
    int64_t v = n->pts_pool[mi];
    n->pts_pool[mi] = n->pts_pool[--n->pts_n];
    return v;
#endif
}

static void ro_reset(evo_vdec_native *n)
{
    for (int i = 0; i < RO_SLOTS; i++)
        n->ro[i].used = 0;
    n->ro_count = 0;
}

static void ro_harvest(evo_vdec_native *n, const SceVideodec2OutputInfo *out)
{
    struct nat_slot *s = NULL;
    for (int i = 0; i < RO_SLOTS; i++)
        if (!n->ro[i].used) { s = &n->ro[i]; break; }
    if (!s)
        return;

    if (!out->buffer)
        return;

    int is_10bit = (n->desc->idx == NAT_HEVC10 || n->desc->idx == NAT_VP92);
    uint32_t cw = out->pitch_bytes ? out->pitch_bytes : out->pitch;
    if (is_10bit && cw < out->pitch * 2u)
        cw = out->pitch * 2u;
    const uint32_t codeh = out->height;                 /* MB-padded luma rows */
    const uint32_t dw = (n->disp_w && n->disp_w < out->width)  ? n->disp_w : out->width;
    const uint32_t dh = (n->disp_h && n->disp_h < out->height) ? n->disp_h : out->height;
    const uint32_t chw = (dw + 1u) / 2u;                /* display chroma w/h  */
    const uint32_t chh = (dh + 1u) / 2u;
    const uint32_t cstride = cw / 2u;                   /* per plane, samples  */

    /* I420 layout in the slot: [Y: cw*c+dh][U: cstride*chh][V: cstride*chh] */
    const size_t y_sz = (size_t)cw * codeh;
    const size_t c_sz = (size_t)cstride * ((codeh + 1u) / 2u);
    const size_t need = y_sz + 2u * c_sz;

    if (n->nv12_out || is_10bit) {
        /* NV12 path: the decoder already laid out contiguous NV12 (Y then the
         * interleaved UV plane) in the frame-pool slot it just wrote. Borrow
         * that pointer instead of copying it into an owned buffer - the reorder
         * window (<= RO_SLOTS frames) is far shorter than the FRAME_POOL_SLOTS
         * cycle before the decoder reuses the slot, so it stays valid until the
         * present consumes it. Saves ~62 MiB of heap (RO_SLOTS owned frames)
         * and a ~12 MiB/frame memcpy - both matter at 4K under the fake-signed
         * flexible-memory ceiling. The GL video shader samples NV12 directly. */
        s->data     = (uint8_t *)out->buffer;
        s->borrowed = 1;
        s->nv12     = 1;
        s->y_stride = cw;
        s->c_stride = cw;              /* NV12: chroma row pitch == luma pitch */
        s->u_off    = y_sz;
        s->v_off    = y_sz;
    } else {
        if (s->borrowed) { s->data = NULL; s->cap = 0; s->borrowed = 0; }
        if (s->cap < need) {
            uint8_t *g = (uint8_t *)realloc(s->data, need);
            if (!g)
                return;
            s->data = g;
            s->cap  = need;
        }
        /* Y: verbatim */
        memcpy(s->data, out->buffer, y_sz);

        /* UV interleaved (NV12) -> planar U, V */
        const uint8_t *uv = (const uint8_t *)out->buffer + (size_t)cw * codeh;
        uint8_t *du = s->data + y_sz;
        uint8_t *dv = du + c_sz;
        for (uint32_t row = 0; row < chh; row++) {
            const uint8_t *src = uv + (size_t)row * cw;
            uint8_t *pu = du + (size_t)row * cstride;
            uint8_t *pv = dv + (size_t)row * cstride;
            for (uint32_t x = 0; x < chw; x++) {
                pu[x] = src[2u * x];
                pv[x] = src[2u * x + 1u];
            }
        }

        s->nv12     = 0;
        s->y_stride = cw;
        s->c_stride = cstride;
        s->u_off    = y_sz;
        s->v_off    = y_sz + c_sz;
    }

    s->coded_h  = codeh;
    s->w        = dw;
    s->h        = dh;
    s->pts      = pts_take(n);
    s->used     = 1;
    n->ro_count++;
}

/* `present` == 0 for a VP9 hidden (alt-ref / show_frame=0) coded frame: the
 * decoder still needs it for reference, but its output must not be paired to a
 * PTS or handed to the presenter (research repo, packetization.cpp). Always 1
 * for AVC / HEVC. */
static int decode_one(evo_vdec_native *n, const uint8_t *au, int size,
                      int64_t pts, int present)
{
    if (size <= 0 || (size_t)size > INPUT_SLOT_BYTES)
        return -1;

    unsigned islot = n->au_ring % PIPELINE_BUFFER_COUNT;
    unsigned fslot = n->au_ring % FRAME_POOL_SLOTS;
    n->au_ring++;

    memcpy(n->input_mem + (size_t)islot * INPUT_SLOT_BYTES, au, (size_t)size);

    SceVideodec2InputData  in;
    SceVideodec2FrameBuffer fb;
    SceVideodec2OutputInfo  out;
    memset(&in, 0, sizeof in);
    memset(&fb, 0, sizeof fb);
    memset(&out, 0, sizeof out);
    in.size     = sizeof in;
    in.au       = n->input_mem + (size_t)islot * INPUT_SLOT_BYTES;
    in.au_size  = (uint64_t)size;
    in.pts      = (uint64_t)pts;
    in.dts      = UINT64_MAX;
    in.attached = 0;
    fb.size        = sizeof fb;
    fb.buffer      = n->frame_mem + (size_t)fslot * n->frame_size;
    fb.buffer_size = n->frame_size;
    out.size       = sizeof out;

    int rc = sceVideodec2Decode(n->dec, &in, &fb, &out);
    n->dec_calls++;
    if (n->dec_calls <= 3 || (out.valid && !n->first_valid_logged)) {
        if (out.valid) {
            n->first_valid_logged = 1;
            if (n->desc->idx == NAT_HEVC10 || n->desc->idx == NAT_VP92) {
                note("EVO vdec native: 10-bit first frame: %ux%u pitch=%u pitch_bytes=%u (expected 1920/3840)",
                     out.width, out.height, out.pitch, out.pitch_bytes);
            }
        }
        note("EVO vdec native: Decode #%u rc=0x%08x acc=%u valid=%u err=%u %ux%u "
             "pitch=%u", n->dec_calls, (unsigned)rc, (unsigned)fb.accepted,
             (unsigned)out.valid, (unsigned)out.error,
             out.width, out.height, out.pitch_bytes ? out.pitch_bytes : out.pitch);
    }
    if (rc != 0 || out.error) {
        if (!n->first_err_logged) {
            n->first_err_logged = 1;
            note("EVO vdec native: Decode FAIL #%u rc=0x%08x err=%u acc=%u au=%dB "
                 "valid=%u %ux%u", n->dec_calls, (unsigned)rc, (unsigned)out.error,
                 (unsigned)fb.accepted, size, (unsigned)out.valid,
                 out.width, out.height);
        }
        return -1;
    }

    if (present)
        pts_push(n, pts);
    if (out.valid && !out.error && out.picture_count) {
        if (present) {
            n->frames_out++;
            ro_harvest(n, &out);
        }
        /* hidden VP9 frame produced an output: consume it silently */
    }
    if (n->dec_calls % 300u == 0)
        note("EVO vdec native: heartbeat  decodes=%u framesout=%u ro=%d",
             n->dec_calls, n->frames_out, n->ro_count);
    return 0;
}

static void drain_decoder(evo_vdec_native *n)
{
    for (int guard = 0; guard < 64; guard++) {
        unsigned slot = n->au_ring % FRAME_POOL_SLOTS;
        n->au_ring++;

        SceVideodec2FrameBuffer fb;
        SceVideodec2OutputInfo  out;
        memset(&fb, 0, sizeof fb);
        memset(&out, 0, sizeof out);
        fb.size        = sizeof fb;
        fb.buffer      = n->frame_mem + (size_t)slot * n->frame_size;
        fb.buffer_size = n->frame_size;
        out.size       = sizeof out;

        if (sceVideodec2Flush(n->dec, &fb, &out) != 0)
            break;
        if (!(out.valid && !out.error && out.picture_count))
            break;
        ro_harvest(n, &out);
        if (n->ro_count >= RO_SLOTS - 1)
            break;
    }
}

/* (Re)create the Annex-B bitstream filter from the stored params. Called at
 * open and again on every flush/seek — a plain av_bsf_flush() does NOT make
 * h264_mp4toannexb re-emit SPS/PPS, so a post-seek IDR would reach the decoder
 * with no parameter sets and it would never restart. A fresh bsf re-injects
 * them on its first packet. */
static int bsf_build(evo_vdec_native *n)
{
    const AVBitStreamFilter *bf = av_bsf_get_by_name(n->bsf_name);
    if (!bf)
        return -1;
    if (n->bsf)
        av_bsf_free(&n->bsf);
    if (av_bsf_alloc(bf, &n->bsf) < 0) {
        n->bsf = NULL;
        return -1;
    }
    if (avcodec_parameters_copy(n->bsf->par_in, n->bsf_par) < 0) {
        av_bsf_free(&n->bsf);
        return -1;
    }
    n->bsf->time_base_in = (AVRational){ 1, 1000000 };
    if (av_bsf_init(n->bsf) < 0) {
        av_bsf_free(&n->bsf);
        return -1;
    }
    return 0;
}

/* Read bit `*pos` (MSB-first) from a byte buffer, advancing the cursor; 0 past
 * the end. */
static int bitrd(const uint8_t *b, int size, int *pos)
{
    int i = (*pos)++;
    if (i < 0 || (i >> 3) >= size)
        return 0;
    return (b[i >> 3] >> (7 - (i & 7))) & 1;
}

/* VP9 uncompressed-header prefix (6.2 of the spec): is this coded frame shown?
 * Hidden alt-ref frames (show_frame=0) must be decoded for reference but never
 * presented; show_existing_frame commands ARE presented (the tested decoder
 * materialises the referenced picture into the supplied slot). Anything we
 * can't parse defaults to "present". */
static int vp9_frame_is_shown(const uint8_t *b, int size)
{
    int p = 0;
    if (bitrd(b, size, &p) != 1 || bitrd(b, size, &p) != 0)   /* frame_marker 0b10 */
        return 1;
    int prof = bitrd(b, size, &p);                            /* profile_low_bit  */
    prof |= bitrd(b, size, &p) << 1;                          /* profile_high_bit */
    if (prof == 3)
        (void)bitrd(b, size, &p);                             /* reserved_zero    */
    if (bitrd(b, size, &p))                                   /* show_existing_frame */
        return 1;
    (void)bitrd(b, size, &p);                                 /* frame_type       */
    return bitrd(b, size, &p);                                /* show_frame       */
}

static int pkt_present(const evo_vdec_native *n, const AVPacket *pkt)
{
    if (n->desc && n->desc->is_vp9)
        return vp9_frame_is_shown(pkt->data, pkt->size);
    return 1;
}

evo_vdec_native *evo_vdec_native_open(const evo_vdec_open_params *p)
{
    if (!evo_vdec_native_probe())
        return NULL;
    if (!p || p->backend != EVO_VDEC_BACKEND_NATIVE || !p->avctx_params)
        return NULL;

    const AVCodecParameters *par = (const AVCodecParameters *)p->avctx_params;
    int bit_depth = par->bits_per_raw_sample > 8 ? par->bits_per_raw_sample : 8;
    if (par->format == AV_PIX_FMT_YUV420P10LE || par->format == AV_PIX_FMT_YUV420P10BE)
        bit_depth = 10;

    const nat_codec_desc *d = codec_desc_for(par->codec_id, par->profile, bit_depth);
    if (!d)
        return NULL;                     /* AV1 / anything with no sce route */

    int w = par->width  > 0 ? par->width  : p->width;
    int h = par->height > 0 ? par->height : p->height;

    if (!evo_vdec_native_supports(par->codec_id, par->profile, bit_depth, w, h)) {
        note("EVO vdec native: %s prof=%d %d-bit %dx%d unsupported -> FFmpeg",
             d->tag, par->profile, bit_depth, w, h);
        return NULL;
    }

    struct dec_slot *slot = &g_dec[d->idx];
    if (slot->owned) {
        note("EVO vdec native: %s slot already in use -> FFmpeg", d->tag);
        return NULL;
    }
    w = roundup16(w);
    h = roundup16(h);

    evo_vdec_native *n = (evo_vdec_native *)calloc(1, sizeof *n);
    if (!n)
        return NULL;
    n->desc       = d;
    n->slot       = slot;
    n->dec        = slot->decoder;
    n->input_mem  = (uint8_t *)slot->input_mem;
    n->frame_mem  = (uint8_t *)slot->frame_mem;
    n->frame_size = slot->frame_size;
    n->disp_w     = par->width  > 0 ? (uint32_t)par->width  : 0;
    n->disp_h     = par->height > 0 ? (uint32_t)par->height : 0;
    n->color_trc  = (int)par->color_trc;
    /* GL video path: emit NV12 straight from the decoder - the GLSL video
     * shader samples NV12 and does the YUV->RGB + scale on-GPU, so the CPU
     * never touches the pixels. Fixed for the stream's lifetime.
     * (g_prefer_nv12 is set unconditionally at boot; kept as a switch.) */
    n->nv12_out   = g_prefer_nv12;

    /* AU adaptation: VP9 always runs the superframe split; AVC/HEVC only when
     * the extradata is a mp4-style avcC/hvcC wrapper (configurationVersion 1),
     * not already-Annex-B (TS). */
    int need_bsf = d->is_vp9 ||
        (par->extradata && par->extradata_size >= 4 && par->extradata[0] == 1);
    if (need_bsf && d->bsf_name) {
        n->bsf_name = d->bsf_name;
        n->bsf_par  = avcodec_parameters_alloc();
        if (!n->bsf_par || avcodec_parameters_copy(n->bsf_par, par) < 0 ||
            bsf_build(n) != 0) {
            note("EVO vdec native: %s bsf '%s' setup failed (ex=%d) -> FFmpeg",
                 d->tag, d->bsf_name, par->extradata_size);
            evo_vdec_native_close(n);
            return NULL;
        }
    }
    n->in_pkt   = av_packet_alloc();
    n->filt_pkt = av_packet_alloc();
    if (!n->in_pkt || !n->filt_pkt) {
        evo_vdec_native_close(n);
        return NULL;
    }

    slot->owned = 1;             /* released by evo_vdec_native_close() */
    sceVideodec2Reset(n->dec);   /* fresh state for this stream */

    note("EVO vdec native: OPEN ok  resident %s decoder  %dx%d (disp %ux%u) bsf=%s depth=%d",
         d->tag, w, h, n->disp_w, n->disp_h,
         n->bsf_name ? n->bsf_name : "-", EVO_VDEC_REORDER_DEPTH);
    return n;
}

int evo_vdec_native_send(evo_vdec_native *v, const uint8_t *data, int size,
                         int64_t pts_us)
{
    if (!v || v->fatal)
        return -1;

    if (!data || size <= 0) {          /* end-of-stream: drain everything */
        if (v->bsf) {
            (void)av_bsf_send_packet(v->bsf, NULL);
            while (av_bsf_receive_packet(v->bsf, v->filt_pkt) == 0) {
                int64_t fp = v->filt_pkt->pts == AV_NOPTS_VALUE
                                 ? INT64_MIN : v->filt_pkt->pts;
                int dr = decode_one(v, v->filt_pkt->data, v->filt_pkt->size, fp,
                                    pkt_present(v, v->filt_pkt));
                av_packet_unref(v->filt_pkt);
                if (dr < 0) break;
            }
        }
        drain_decoder(v);
        v->flushing = 1;
        return 0;
    }

    if (v->ro_count > EVO_VDEC_REORDER_DEPTH)
        return 1;

    if (v->bsf) {
        av_packet_unref(v->in_pkt);
        if (av_new_packet(v->in_pkt, size) < 0)
            return -1;
        memcpy(v->in_pkt->data, data, (size_t)size);
        v->in_pkt->pts = (pts_us == INT64_MIN) ? AV_NOPTS_VALUE : pts_us;
        v->in_pkt->dts = AV_NOPTS_VALUE;
        if (av_bsf_send_packet(v->bsf, v->in_pkt) < 0)
            return -1;
        while (av_bsf_receive_packet(v->bsf, v->filt_pkt) == 0) {
            int64_t fp = v->filt_pkt->pts == AV_NOPTS_VALUE
                             ? INT64_MIN : v->filt_pkt->pts;
            int dr = decode_one(v, v->filt_pkt->data, v->filt_pkt->size, fp,
                                pkt_present(v, v->filt_pkt));
            av_packet_unref(v->filt_pkt);
            if (dr < 0) { v->fatal = 1; return -1; }
        }
        return 0;
    }

    if (decode_one(v, data, size, pts_us, 1) < 0) {
        v->fatal = 1;
        return -1;
    }
    return 0;
}

int evo_vdec_native_receive(evo_vdec_native *v, pp_frame *out)
{
    if (!v || !out || v->fatal)
        return -1;

    int threshold = v->flushing ? 0 : EVO_VDEC_REORDER_DEPTH;
    if (v->ro_count <= threshold)
        return 0;

    struct nat_slot *best = NULL;
    for (int i = 0; i < RO_SLOTS; i++) {
        if (!v->ro[i].used)
            continue;
        if (!best) { best = &v->ro[i]; continue; }
        if (best->pts == INT64_MIN ||
            (v->ro[i].pts != INT64_MIN && v->ro[i].pts < best->pts))
            best = &v->ro[i];
    }
    if (!best)
        return 0;

    int is_10bit = (v->desc->idx == NAT_HEVC10 || v->desc->idx == NAT_VP92);
    memset(out, 0, sizeof(*out));
    if (is_10bit)
        out->format   = PP_FRAME_NV12_10;
    else
        out->format   = best->nv12 ? PP_FRAME_NV12 : PP_FRAME_YUV420P;
    out->width        = best->w;
    out->height       = best->h;
    out->coded_height = best->coded_h;
    out->planes[0]    = best->data;
    out->planes[1]    = best->data + best->u_off;
    out->planes[2]    = best->data + best->v_off;
    out->strides[0]   = (int)best->y_stride;
    out->strides[1]   = (int)best->c_stride;
    out->strides[2]   = (int)best->c_stride;
    out->pts_us       = best->pts;
    out->color_trc    = v->color_trc;

    best->used = 0;
    v->ro_count--;
    return 1;
}

void evo_vdec_native_flush(evo_vdec_native *v)   /* seek */
{
    if (!v)
        return;
    ro_reset(v);
    v->pts_n          = 0;
    v->au_ring        = 0;
    v->flushing       = 0;
    v->fatal          = 0;
    v->first_err_logged = 0;
    /*
     * #57: a plain av_bsf_flush() drops h264_mp4toannexb's buffered state but
     * does NOT re-arm its one-shot SPS/PPS injection, so the first IDR after a
     * seek reaches sceVideodec2 with no in-band parameter sets and it faults
     * (0x811d0303) on every AU until playback gives up. Streams that repeat
     * SPS/PPS in-band per keyframe (e.g. the GTA trailer) happen to survive;
     * ones that keep them in avcC extradata only (mkv, most .mov) do not.
     * Rebuild the bsf instead — a fresh filter re-injects the parameter sets
     * on its first output packet. On rebuild failure, fault so the playback
     * layer falls back cleanly rather than feeding raw avcC to the decoder.
     */
    if (v->bsf_name && v->bsf_par) {
        if (bsf_build(v) != 0) {
            v->fatal = 1;
            note("EVO vdec native: FLUSH (seek) bsf rebuild FAILED -> fatal");
        }
    } else if (v->bsf) {
        av_bsf_flush(v->bsf);
    }
    if (v->dec)
        sceVideodec2Reset(v->dec);
    note("EVO vdec native: FLUSH (seek)  decodes=%u framesout=%u",
         v->dec_calls, v->frames_out);
}

void evo_vdec_native_close(evo_vdec_native *v)
{
    if (!v)
        return;
    note("EVO vdec native: CLOSE  decodes=%u framesout=%u fatal=%d",
         v->dec_calls, v->frames_out, v->fatal);
    if (v->dec)
        sceVideodec2Reset(v->dec);   /* leave the resident decoder alive */
    if (v->slot)
        v->slot->owned = 0;          /* the next opener may have it */
    if (v->bsf)
        av_bsf_free(&v->bsf);
    if (v->bsf_par)
        avcodec_parameters_free(&v->bsf_par);
    if (v->in_pkt)
        av_packet_free(&v->in_pkt);
    if (v->filt_pkt)
        av_packet_free(&v->filt_pkt);
    for (int i = 0; i < RO_SLOTS; i++)
        if (!v->ro[i].borrowed)
            free(v->ro[i].data);
    free(v);
}

#else /* !EVO_APP_MODULE — host + payload: native decode is unavailable */

int evo_vdec_native_probe(void) { return 0; }
void evo_vdec_native_prefer_nv12(int on) { (void)on; }
int evo_vdec_native_supports(int codec_id, int profile, int bit_depth, int w, int h)
{ (void)codec_id; (void)profile; (void)bit_depth; (void)w; (void)h; return 0; }
evo_vdec_native *evo_vdec_native_open(const evo_vdec_open_params *p) { (void)p; return 0; }
int evo_vdec_native_send(evo_vdec_native *v, const uint8_t *d, int s, int64_t p)
{ (void)v; (void)d; (void)s; (void)p; return -1; }
int evo_vdec_native_receive(evo_vdec_native *v, pp_frame *o) { (void)v; (void)o; return -1; }
void evo_vdec_native_flush(evo_vdec_native *v) { (void)v; }
void evo_vdec_native_close(evo_vdec_native *v) { (void)v; }

#endif /* EVO_APP_MODULE */
