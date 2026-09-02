/* =============================================================================
 * avplayer_test — libSceAvPlayer callback-port REFERENCE (payload build).
 *
 * NOT the native-decode gate. An ELF payload (elfldr or hbldr) is a
 * borrowed-process sandbox and cannot reach hardware decode — it hits the
 * errno-5200 wall, same as Route B did. The real Route A gate is the boot-time
 * probe compiled into the PPSA99039 app module:
 *     projects/evoplayer/src/evo_avplayer_probe.c   (package-app.sh --avplayer-probe)
 * See docs/evo-pro/avplayer-abi.md §4/§5.
 *
 * This file is kept as a compile-checked, self-contained reference for the
 * AvPlayer call sequence and the MediaPlayer.cs callback port. Running it will
 * at best exercise Init + demux; a decoded frame is not expected here.
 *
 * WHAT THIS BUILD DOES
 *   - resolves every entry point at run time by NID (nid_encode +
 *     kernel_dynlib_resolve) — no hardcoded NID literals, no -lSceAvPlayer.
 *   - supplies the four mandatory memory callbacks (general = aligned heap;
 *     texture = GPU-visible direct memory, per SharpProspero MediaPlayer.cs).
 *   - supplies file-replacement callbacks that LOG every call and then service
 *     it from the real fs (instrument-before-first-call — hardware-decode-
 *     review §8).
 *   - dumps the first decoded frame's planes + every AvPlayerFrameInfoEx field
 *     to /data, and characterises it (NV12? pitch vs width? crop? CPU-readable
 *     without a fault?) — hardware-decode-review §5.
 *   - a watchdog thread _exit()s the process if any call hangs.
 *
 * WHAT THIS BUILD DELIBERATELY DOES NOT DO
 *   - it never calls sceVideoOutOpen. That is a documented kernel-panic vector
 *     from a payload once a compute queue is allocated after it, and AvPlayer
 *     allocates a compute queue internally. Presenting the frame is a separate
 *     concern (pp_agc / VideoOut) and out of scope for the gate.
 *
 * KNOWN PAYLOAD-CONTEXT CAVEAT
 *   sceKernelGetDirectMemorySize() reports 0 in an elfldr payload, so the
 *   texture allocator here uses sceKernelAllocateMainDirectMemory (the main
 *   pool) instead of MediaPlayer.cs's sceKernelAllocateDirectMemory. If the
 *   decoder rejects that memory, that is itself the finding: Route A needs the
 *   registered-app-module context. See docs/evo-pro/avplayer-abi.md.
 * =============================================================================
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>

#include <ps5/kernel.h>
#include <ps5/nid.h>

#include "evo_ps5.h"
#include "sce/sce_avplayer.h"

/* Flush after every line: if a later call crashes, the transcript over the
 * /hbldr pipe still shows exactly how far we got. */
#define P(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)

/* ---- default test file ------------------------------------------------- */
/* AvPlayer only opens containers it recognises by extension: .mp4/.m4v/.mov
 * or .webm. Override with argv[1]. */
#ifndef AVPX_DEFAULT_FILE
#define AVPX_DEFAULT_FILE "/data/bunny.mp4"
#endif

/* ============================ watchdog ================================= */
static volatile int g_wd_stage = 0;
static volatile int g_wd_last  = -1;

static const char *g_stage_name[] = {
    "start", "module-load", "resolve", "init", "add-source",
    "stream-enum", "start", "decode-loop", "shutdown", "done",
};

static void *watchdog(void *arg)
{
    (void)arg;
    /* 40 s hard cap; each cleared stage resets the countdown. */
    for (int i = 0; i < 40; i++) {
        sceKernelUsleep(1000 * 1000);
        if (g_wd_stage != g_wd_last) { g_wd_last = g_wd_stage; i = 0; }
    }
    evo_notify("avplayer_test: WATCHDOG fired at stage %d (%s) - force exit",
               g_wd_stage,
               g_wd_stage < (int)(sizeof g_stage_name / sizeof *g_stage_name)
                   ? g_stage_name[g_wd_stage] : "?");
    P("\n[!] WATCHDOG force-exit at stage %d\n", g_wd_stage);
    _exit(42 + g_wd_stage);
    return NULL;
}

/* ==================== SIGSEGV/SIGBUS guard for the CPU-read probe ======= */
static sigjmp_buf g_fault_jmp;
static volatile sig_atomic_t g_fault_armed = 0;

static void fault_handler(int sig)
{
    if (g_fault_armed) {
        g_fault_armed = 0;
        siglongjmp(g_fault_jmp, sig);
    }
    _exit(120 + sig);
}

/* ======================= memory callbacks ============================== */
/* general: the player calls these from its own threads for bookkeeping
 * allocations. Plain aligned heap. */
static void *av_alloc(void *arg, uint32_t alignment, uint32_t size)
{
    (void)arg;
    void *p = NULL;
    size_t align = alignment < sizeof(void *) ? sizeof(void *) : alignment;
    /* posix_memalign needs a power-of-two multiple of sizeof(void*). */
    size_t a = sizeof(void *);
    while (a < align) a <<= 1;
    if (posix_memalign(&p, a, size) != 0)
        return NULL;
    return p;
}

static void av_free(void *arg, void *ptr)
{
    (void)arg;
    free(ptr);
}

/* texture: GPU-visible frame buffers. MediaPlayer.cs uses
 * sceKernelAllocateDirectMemory + MemoryTypeCachedShared(12) + prot 0x33, and
 * the deallocator MUST unmap as well as release. In a payload the direct pool
 * size is 0, so we go through the main pool. Track (addr -> pa, size). */
#define AVPX_TEX_SLOTS 64
static struct { void *addr; intptr_t pa; size_t size; } g_tex[AVPX_TEX_SLOTS];
static pthread_mutex_t g_tex_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_tex_high = 0;

static void *av_alloc_texture(void *arg, uint32_t alignment, uint32_t size)
{
    (void)arg;
    size_t align = alignment <= 0x4000 ? 0x4000 : alignment;
    size_t a = 0x4000;
    while (a < align) a <<= 1;
    size_t bytes = (size + a - 1) & ~(a - 1);

    intptr_t pa = 0;
    int rc = sceKernelAllocateMainDirectMemory(bytes, a,
                                               3 /* WC_GARLIC; see caveat */, &pa);
    if (rc != 0) {
        P("    [tex] AllocateMainDirectMemory(%zu) -> 0x%08x\n", bytes, rc);
        return NULL;
    }
    void *addr = NULL;
    rc = sceKernelMapDirectMemory(&addr, bytes,
                                  SCE_KERNEL_PROT_CPU_RW | SCE_KERNEL_PROT_GPU_ALL,
                                  0, pa, a);
    if (rc != 0 || !addr) {
        P("    [tex] MapDirectMemory(%zu) -> 0x%08x\n", bytes, rc);
        sceKernelReleaseDirectMemory(pa, bytes);
        return NULL;
    }

    pthread_mutex_lock(&g_tex_lock);
    int used = 0;
    for (int i = 0; i < AVPX_TEX_SLOTS; i++) {
        if (!g_tex[i].addr) {
            g_tex[i].addr = addr; g_tex[i].pa = pa; g_tex[i].size = bytes;
            break;
        }
        used++;
    }
    if (used > g_tex_high) g_tex_high = used;
    pthread_mutex_unlock(&g_tex_lock);

    P("    [tex] alloc %zu B align %zu -> %p (pa 0x%lx)\n", bytes, a, addr, (long)pa);
    return addr;
}

static void av_free_texture(void *arg, void *ptr)
{
    (void)arg;
    if (!ptr) return;
    pthread_mutex_lock(&g_tex_lock);
    for (int i = 0; i < AVPX_TEX_SLOTS; i++) {
        if (g_tex[i].addr == ptr) {
            sceKernelReleaseDirectMemory(g_tex[i].pa, g_tex[i].size);
            g_tex[i].addr = NULL; g_tex[i].pa = 0; g_tex[i].size = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_tex_lock);
}

/* ======================= file callbacks (instrument + serve) =========== */
static int      g_file_fd    = -1;
static uint64_t g_file_calls = 0;

static int av_file_open(void *arg, const char *path)
{
    (void)arg;
    g_file_fd = open(path, O_RDONLY);
    P("    [file] open(\"%s\") -> fd %d\n", path, g_file_fd);
    return g_file_fd >= 0 ? 0 : -1;
}

static int av_file_close(void *arg)
{
    (void)arg;
    P("    [file] close (after %llu read calls)\n",
      (unsigned long long)g_file_calls);
    if (g_file_fd >= 0) { close(g_file_fd); g_file_fd = -1; }
    return 0;
}

static int av_file_read_offset(void *arg, uint8_t *buf,
                               uint64_t position, uint32_t length)
{
    (void)arg;
    g_file_calls++;
    if (g_file_fd < 0) return -1;
    if (lseek(g_file_fd, (off_t)position, SEEK_SET) == (off_t)-1)
        return -1;
    ssize_t n = read(g_file_fd, buf, length);
    if (g_file_calls <= 4 || (g_file_calls % 256) == 0)
        P("    [file] read @%llu len %u -> %zd\n",
          (unsigned long long)position, length, n);
    return (int)n;
}

static uint64_t av_file_size(void *arg)
{
    (void)arg;
    struct stat st;
    if (g_file_fd < 0 || fstat(g_file_fd, &st) != 0) return 0;
    P("    [file] size -> %lld\n", (long long)st.st_size);
    return (uint64_t)st.st_size;
}

/* ======================= event callback (log-only) ==================== */
static void av_event(void *arg, int32_t event_id, int32_t source_id, void *data)
{
    (void)arg; (void)data;
    P("    [event] id=%d source=%d\n", event_id, source_id);
}

/* ============================ symbol table ============================= */
struct av_api {
    void    *(*Init)(const SceAvPlayerInitData *);
    int32_t  (*AddSource)(void *, const char *);
    int32_t  (*Start)(void *);
    int32_t  (*Stop)(void *);
    int32_t  (*IsActive)(void *);
    int32_t  (*SetLooping)(void *, int32_t);
    int32_t  (*StreamCount)(void *);
    int32_t  (*GetStreamInfo)(void *, uint32_t, SceAvPlayerStreamInfo *);
    int32_t  (*EnableStream)(void *, uint32_t);
    int32_t  (*GetVideoDataEx)(void *, SceAvPlayerFrameInfoEx *);
    int32_t  (*GetAudioData)(void *, SceAvPlayerFrameInfo *);
    int32_t  (*Close)(void *);
};

static void *resolve1(uint32_t h, const char *name)
{
    char nid[12];
    nid_encode(name, nid);
    intptr_t a = kernel_dynlib_resolve(getpid(), h, nid);
    P("  %-28s %s -> %p\n", name, nid, (void *)a);
    return (void *)a;
}

static int resolve_all(struct av_api *api)
{
    int mod = sceKernelLoadStartModule("/system/common/lib/libSceAvPlayer.sprx",
                                       0, 0, 0, 0, NULL);
    P("[*] sceKernelLoadStartModule(libSceAvPlayer.sprx) -> %d\n", mod);

    uint32_t h = 0;
    if (kernel_dynlib_handle(getpid(), "libSceAvPlayer.sprx", &h) != 0 || !h) {
        P("[-] libSceAvPlayer.sprx not in process\n");
        return -1;
    }
    P("[+] dynlib handle 0x%08x\n", h);

    api->Init           = resolve1(h, "sceAvPlayerInit");
    api->AddSource      = resolve1(h, "sceAvPlayerAddSource");
    api->Start          = resolve1(h, "sceAvPlayerStart");
    api->Stop           = resolve1(h, "sceAvPlayerStop");
    api->IsActive       = resolve1(h, "sceAvPlayerIsActive");
    api->SetLooping     = resolve1(h, "sceAvPlayerSetLooping");
    api->StreamCount    = resolve1(h, "sceAvPlayerStreamCount");
    api->GetStreamInfo  = resolve1(h, "sceAvPlayerGetStreamInfo");
    api->EnableStream   = resolve1(h, "sceAvPlayerEnableStream");
    api->GetVideoDataEx = resolve1(h, "sceAvPlayerGetVideoDataEx");
    api->GetAudioData   = resolve1(h, "sceAvPlayerGetAudioData");
    api->Close          = resolve1(h, "sceAvPlayerClose");

    if (!api->Init || !api->AddSource || !api->Start ||
        !api->GetVideoDataEx || !api->Close) {
        P("[-] essential symbols missing\n");
        return -1;
    }
    return 0;
}

/* ======================= frame dump + characterise =================== */
static void dump_dir(char *out, size_t n)
{
    const char *dirs[] = { "/data/avplayer_probe", "/mnt/usb0/avplayer_probe" };
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        if (mkdir(dirs[i], 0777) == 0 || access(dirs[i], W_OK) == 0) {
            snprintf(out, n, "%s", dirs[i]);
            return;
        }
    }
    snprintf(out, n, "/data");
}

static void characterise_and_dump(const SceAvPlayerFrameInfoEx *f)
{
    unsigned w    = f->video_width;
    unsigned h    = f->video_height;
    unsigned pitch= f->video_pitch;
    unsigned visw = pitch > (f->crop_left + f->crop_right)
                        ? pitch - f->crop_left - f->crop_right : 0;
    unsigned vish = h > (f->crop_top + f->crop_bottom)
                        ? h - f->crop_top - f->crop_bottom : 0;

    P("\n=== FIRST VIDEO FRAME ===\n");
    P("  data            %p\n", f->data);
    P("  timestamp_ms    %llu\n", (unsigned long long)f->timestamp_ms);
    P("  video_width     %u\n", w);
    P("  video_height    %u   (buffer height)\n", h);
    P("  video_pitch     %u B %s\n", pitch,
      pitch == w ? "(== width; no padding)" :
      pitch  > w ? "(> width; row padding present)" : "(< width?! unexpected)");
    P("  crop L/R/T/B    %u / %u / %u / %u\n",
      f->crop_left, f->crop_right, f->crop_top, f->crop_bottom);
    P("  visible         %u x %u  (pitch-cropLR x height-cropTB)\n", visw, vish);

    /* CPU-readability probe, guarded against a fault. */
    const char *readable = "NOT TESTED";
    if (f->data) {
        if (sigsetjmp(g_fault_jmp, 1) == 0) {
            g_fault_armed = 1;
            volatile uint32_t acc = 0;
            const uint8_t *p = (const uint8_t *)f->data;
            for (unsigned i = 0; i < 4096 && i < pitch * h; i += 64) acc += p[i];
            /* touch the presumed chroma plane too */
            const uint8_t *c = p + (size_t)pitch * h;
            for (unsigned i = 0; i < 1024; i += 64) acc += c[i];
            (void)acc;
            g_fault_armed = 0;
            readable = "yes (no fault on CPU read of luma + chroma)";
        } else {
            readable = "NO — faulted on CPU read (GPU-only mapping)";
        }
    }
    P("  cpu-readable     %s\n", readable);

    /* Rudimentary NV12 sanity: luma should have real spread, chroma should sit
     * near 128 for a typical frame. Only meaningful if CPU-readable. */
    if (readable[0] == 'y' && f->data && pitch && h) {
        const uint8_t *p = (const uint8_t *)f->data;
        const uint8_t *c = p + (size_t)pitch * h;
        unsigned ymin = 255, ymax = 0; long csum = 0; int cn = 0;
        for (unsigned row = 0; row < vish && row < 64; row++) {
            const uint8_t *lr = p + (size_t)(row + f->crop_top) * pitch + f->crop_left;
            for (unsigned x = 0; x < visw && x < 256; x++) {
                if (lr[x] < ymin) ymin = lr[x];
                if (lr[x] > ymax) ymax = lr[x];
            }
        }
        for (unsigned row = 0; row < vish / 2 && row < 32; row++) {
            const uint8_t *cr = c + (size_t)row * pitch;
            for (unsigned x = 0; x < 128; x++) { csum += cr[x]; cn++; }
        }
        P("  luma  min/max    %u / %u   %s\n", ymin, ymax,
          ymax > ymin + 8 ? "(has detail)" : "(flat — decode may be wrong)");
        if (cn) P("  chroma mean      %ld   %s\n", csum / cn,
                  (csum / cn > 96 && csum / cn < 160)
                      ? "(near 128 — consistent with NV12 UV)"
                      : "(far from 128 — not NV12 UV, or wrong plane offset)");
    }

    /* Dump planes for off-console inspection. */
    char dir[128], path[192];
    dump_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/frame0.nv12", dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0 && readable[0] == 'y') {
        size_t luma_sz   = (size_t)pitch * h;
        size_t chroma_sz = (size_t)pitch * (h / 2);
        ssize_t a = write(fd, f->data, luma_sz);
        ssize_t b = write(fd, (const uint8_t *)f->data + luma_sz, chroma_sz);
        P("  dumped           %s (%zd + %zd bytes)\n", path, a, b);
        close(fd);
    } else if (fd >= 0) {
        close(fd);
        P("  dump skipped     frame not CPU-readable\n");
    } else {
        P("  dump FAILED      could not open %s\n", path);
    }

    snprintf(path, sizeof path, "%s/frame0.txt", dir);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        char meta[512];
        int ml = snprintf(meta, sizeof meta,
            "width=%u height=%u pitch=%u\n"
            "crop_left=%u crop_right=%u crop_top=%u crop_bottom=%u\n"
            "visible=%ux%u timestamp_ms=%llu\n"
            "cpu_readable=%s\n"
            "layout: luma[pitch*height] then chroma[pitch*height/2] (NV12 assumed)\n",
            w, h, pitch, f->crop_left, f->crop_right, f->crop_top, f->crop_bottom,
            visw, vish, (unsigned long long)f->timestamp_ms, readable);
        write(fd, meta, (size_t)ml);
        close(fd);
    }
}

/* ================================ main =============================== */
int main(int argc, char **argv)
{
    const char *file = (argc > 1) ? argv[1] : AVPX_DEFAULT_FILE;

    P("\n==================================================\n");
    P("  avplayer_test — libSceAvPlayer native decode spike\n");
    P("  file: %s\n", file);
    P("==================================================\n\n");

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fault_handler;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    pthread_t wd;
    pthread_create(&wd, NULL, watchdog, NULL);

    evo_notify("avplayer_test: starting");

    /* --- resolve ------------------------------------------------------- */
    g_wd_stage = 1;
    sceSysmoduleLoadModuleInternal(0x80000018);   /* SceAvPlayer, best-effort */
    g_wd_stage = 2;
    struct av_api api;
    memset(&api, 0, sizeof api);
    if (resolve_all(&api) != 0) {
        evo_notify("avplayer_test: FAILED (symbol resolve)");
        _exit(2);
    }

    /* --- init -------------------------------------------------------- */
    g_wd_stage = 3;
    SceAvPlayerInitData init;
    memset(&init, 0, sizeof init);
    init.memory_replacement.allocate           = av_alloc;
    init.memory_replacement.deallocate         = av_free;
    init.memory_replacement.allocate_texture   = av_alloc_texture;
    init.memory_replacement.deallocate_texture = av_free_texture;
    init.file_replacement.open        = av_file_open;
    init.file_replacement.close       = av_file_close;
    init.file_replacement.read_offset = av_file_read_offset;
    init.file_replacement.size        = av_file_size;
    init.event_replacement.event_callback = av_event;
    init.debug_level = SCE_AVPLAYER_DEBUG_ALL;
    init.base_priority = 0;
    init.num_output_video_framebuffers = 8;
    init.auto_start = 0;

    P("[*] sceAvPlayerInit (sizeof InitData = %zu, expect 120)\n", sizeof init);
    void *player = api.Init(&init);
    P("    -> handle %p\n", player);
    if (!player) {
        evo_notify("avplayer_test: FAILED (Init returned NULL)");
        _exit(3);
    }

    /* --- add source ------------------------------------------------- */
    g_wd_stage = 4;
    int rc = api.AddSource(player, file);
    P("[*] sceAvPlayerAddSource(\"%s\") -> 0x%08x\n", file, rc);
    if (rc != 0) {
        evo_notify("avplayer_test: FAILED (AddSource 0x%08x)", rc);
        api.Close(player);
        _exit(4);
    }

    /* --- enumerate + enable streams ------------------------------- */
    g_wd_stage = 5;
    int count = 0;
    for (int t = 0; t < 100; t++) {           /* up to 10 s for the read thread */
        count = api.StreamCount ? api.StreamCount(player) : -1;
        if (count > 0) break;
        sceKernelUsleep(100 * 1000);
    }
    P("[*] sceAvPlayerStreamCount -> %d\n", count);

    int have_video = 0;
    if (count > 0 && api.GetStreamInfo && api.EnableStream) {
        int taken[4] = { 0, 0, 0, 0 };
        for (uint32_t i = 0; i < (uint32_t)count; i++) {
            SceAvPlayerStreamInfo si;
            memset(&si, 0, sizeof si);
            if (api.GetStreamInfo(player, i, &si) != 0) continue;
            P("    stream %u: type=%u duration=%llu ms", i, si.type,
              (unsigned long long)si.duration_ms);
            if (si.type == SCE_AVPLAYER_STREAM_VIDEO)
                P("  video %ux%u ar=%.3f\n",
                  si.details.video.width, si.details.video.height,
                  (double)si.details.video.aspect_ratio);
            else if (si.type == SCE_AVPLAYER_STREAM_AUDIO)
                P("  audio %u ch @ %u Hz\n",
                  si.details.audio.channel_count, si.details.audio.sample_rate);
            else
                P("\n");
            if (si.type < 4 && !taken[si.type]) {
                int er = api.EnableStream(player, i);
                P("      EnableStream(%u) -> 0x%08x\n", i, er);
                taken[si.type] = 1;
                if (si.type == SCE_AVPLAYER_STREAM_VIDEO) have_video = 1;
            }
        }
    }
    (void)have_video;

    /* --- start ---------------------------------------------------- */
    g_wd_stage = 6;
    if (api.SetLooping) api.SetLooping(player, 0);
    rc = api.Start(player);
    P("[*] sceAvPlayerStart -> 0x%08x\n", rc);
    evo_notify("avplayer_test: decode loop (streams=%d)", count);

    /* --- decode loop -------------------------------------------- */
    g_wd_stage = 7;
    int video_frames = 0, audio_frames = 0, polls = 0;
    uint64_t start = sceKernelGetProcessTime();
    while (sceKernelGetProcessTime() - start < 15ULL * 1000 * 1000) {
        SceAvPlayerFrameInfoEx vf;
        memset(&vf, 0, sizeof vf);
        if (api.GetVideoDataEx(player, &vf) && vf.data) {
            if (video_frames == 0)
                characterise_and_dump(&vf);
            video_frames++;
        }

        SceAvPlayerFrameInfo af;
        memset(&af, 0, sizeof af);
        if (api.GetAudioData && api.GetAudioData(player, &af) && af.data) {
            if (audio_frames == 0)
                P("\n[audio] first frame: %u ch @ %u Hz size=%u ts=%llu\n",
                  af.details.audio.channel_count, af.details.audio.sample_rate,
                  af.details.audio.size, (unsigned long long)af.timestamp_ms);
            audio_frames++;
        }

        if (!video_frames && !audio_frames && (++polls % 100) == 0) {
            int act = api.IsActive ? api.IsActive(player) : -1;
            P("    [poll %d] no frame yet (isActive=%d)\n", polls, act);
            if (act == 0 && polls >= 300) break;   /* player gave up */
        }
        sceKernelUsleep(5 * 1000);
    }

    P("\n[=] video frames: %d   audio frames: %d   tex high-water: %d\n",
      video_frames, audio_frames, g_tex_high);

    /* --- shutdown --------------------------------------------- */
    g_wd_stage = 8;
    if (api.Stop) { int s = api.Stop(player); P("[*] sceAvPlayerStop -> 0x%08x\n", s); }
    int c = api.Close(player);
    P("[*] sceAvPlayerClose -> 0x%08x\n", c);

    g_wd_stage = 9;
    if (video_frames > 0)
        evo_notify("avplayer_test: OK — %d video frame(s) decoded", video_frames);
    else
        evo_notify("avplayer_test: NO video frames (streams=%d) — see transcript",
                   count);

    P("\n[done]\n");
    return video_frames > 0 ? 0 : 1;
}
