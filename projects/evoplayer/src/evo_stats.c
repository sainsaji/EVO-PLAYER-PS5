/*
 * evo_stats.c - one JSON snapshot of the player's health, for the log server's
 * GET /stats and the /dash page that graphs it on a PC.
 *
 * Memory is the reason this exists. On hardware 2026-10-07 a DTS:X stream ran
 * the 448 MB flexible pool dry and the picture stopped, while the in-player
 * HUD's RAM graph (the 64 MB GPU pool) showed nothing wrong. So the pools are
 * reported the way the malloc shim sees them, next to the read-ahead and the
 * network reader that fill them.
 *
 * Everything read here is a counter or a flag another thread owns; a snapshot
 * may be a few milliseconds inconsistent, which a 1 Hz graph never notices.
 */
#if defined(EVO_APP_MODULE)

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "evo_build_id.h"
#include "evo_demux.h"
#include "evo_direct_mem.h"
#include "evo_packet_queue.h"
#include "evo_parallel_io.h"
#include "evo_playback.h"

/* malloc shim (weak: absent outside the app module's link) */
__attribute__((weak)) void evo_alloc_stats(uint64_t *live, uint64_t *peak, uint64_t *large_n);
__attribute__((weak)) void evo_alloc_map_info(uint64_t *fails, uint64_t *flex,
                                              uint64_t *anon, uint64_t *flex_avail);
__attribute__((weak)) void evo_alloc_direct_info(uint64_t *served, uint64_t *live,
                                                 uint64_t *peak);

long long now_ms(void);
double evo_player_position_s(void);
extern volatile int pb_prebuffer_hold;
extern long long video_queue_byte_cap;
extern long long audio_queue_byte_cap;
extern int perf_render_fps;
extern int perf_decode_fps;
extern volatile int demux_recoveries_total;
extern volatile int demux_recovering;

/* The title's flexible budget is whatever param.json asked for (1 GiB since
 * 2026-10-07, 448 MB without the key) - ask the kernel, never assume. */
extern int sceKernelConfiguredFlexibleMemorySize(size_t *outSize);

#define MB(x) ((double)(x) / (1024.0 * 1024.0))

size_t evo_stats_json(char *out, size_t cap)
{
    uint64_t live = 0, peak = 0, large_n = 0, fails = 0, flex = 0, anon = 0, avail = 0;
    uint64_t dserved = 0, dlive = 0, dpeak = 0;
    if (evo_alloc_stats)       evo_alloc_stats(&live, &peak, &large_n);
    if (evo_alloc_map_info)    evo_alloc_map_info(&fails, &flex, &anon, &avail);
    if (evo_alloc_direct_info) evo_alloc_direct_info(&dserved, &dlive, &dpeak);

    size_t flex_total = 0;
    if (sceKernelConfiguredFlexibleMemorySize(&flex_total) != 0)
        flex_total = 0;

    evo_direct_mem_stats_t dm;
    memset(&dm, 0, sizeof dm);
    evo_direct_mem_get_stats(&dm);

    int vq = 0, aq = 0;
    int64_t vdur = 0, adur = 0, vbytes = 0, abytes = 0;
    packet_queue_level(&video_packet_queue, &vq, &vdur, &vbytes);
    packet_queue_level(&audio_packet_queue, &aq, &adur, &abytes);

    const int active = evo_pb_is_active();
    const double fps = evo_pb_video_fps();

    evo_pio_stats_t pio;
    evo_pio_get_stats(&pio);

    int n = snprintf(out, cap,
        "{\"t\":%.3f,\"build\":\"%s\","
        "\"mem\":{\"live_mb\":%.1f,\"peak_mb\":%.1f,\"flex_free_mb\":%.1f,\"flex_total_mb\":%.0f,"
        "\"direct_mb\":%.1f,\"direct_peak_mb\":%.1f,\"map_fail\":%llu,"
        "\"gpu_pool_mb\":%.1f,\"gpu_pool_total_mb\":%.1f},"
        "\"play\":{\"active\":%d,\"paused\":%d,\"eof\":%d,\"fatal\":%d,\"buffering\":%d,"
        "\"pos\":%.2f,\"dur\":%.1f,\"fps\":%.3f,\"backend\":%d,\"render_fps\":%d,\"decode_fps\":%d},"
        "\"queue\":{\"video_pkts\":%d,\"video_mb\":%.1f,\"video_cap_mb\":%.1f,\"video_s\":%.1f,"
        "\"audio_pkts\":%d,\"audio_mb\":%.1f,\"audio_cap_mb\":%.1f},"
        "\"net\":{\"pio\":%d,\"connections\":%d,\"ready_mb\":%d,\"window_mb\":%d,"
        "\"bytes\":%lld,\"chunk_failures\":%lld,\"stall_ms\":%lld,"
        "\"recoveries\":%d,\"recovering\":%d}}",
        now_ms() / 1000.0, EVO_BUILD_ID,
        MB(live), MB(peak), MB(avail), MB(flex_total),
        MB(dlive), MB(dpeak), (unsigned long long)fails,
        MB(dm.allocated_bytes), MB(dm.total_bytes),
        active, evo_pb_is_paused(), evo_pb_is_eof(), evo_pb_decode_fatal(),
        active ? (int)(pb_prebuffer_hold || (pio.stall_ms > 1500 && vq < 3)) : 0,
        active ? evo_player_position_s() : 0.0, active ? evo_pb_duration_s() : 0.0, fps,
        evo_pb_active_backend(), perf_render_fps, perf_decode_fps,
        vq, MB(vbytes), MB(video_queue_byte_cap), (fps > 1.0 ? vq / fps : 0.0),
        aq, MB(abytes), MB(audio_queue_byte_cap),
        pio.active, pio.connections, pio.ready_chunks * pio.chunk_mb,
        pio.window_chunks * pio.chunk_mb, pio.bytes, pio.chunk_failures, pio.stall_ms,
        (int)demux_recoveries_total, (int)demux_recovering);
    if (n < 0)
        return 0;
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

#endif /* EVO_APP_MODULE */
