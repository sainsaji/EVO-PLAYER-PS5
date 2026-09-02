/*
 * evo_avplayer_probe.c - libSceAvPlayer native-decode gate, run from inside
 * the PPSA99039 app module. See evo_avplayer_probe.h and
 * docs/evo-pro/avplayer-abi.md. Compiled only under -DEVO_AVPLAYER_PROBE.
 *
 * libSceAvPlayer is now a link-time NEEDED dependency (PRX import stub —
 * tools/native-app/stubs/prx/libSceAvPlayer.syms), so the loader auto-loads
 * libSceAvPlayer.sprx at start and the sceAvPlayer* symbols are called
 * directly. No sceKernelLoadStartModule (which a fake-signed module can't do
 * for an undeclared PRX - the wall this probe first hit), no dlsym, no NID.
 *
 * App-module context: NO stdout to klog. Every result is a notification popup.
 */
#ifdef EVO_AVPLAYER_PROBE

#include "evo_avplayer_probe.h"
#include "sce/sce_avplayer.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>

extern int sceKernelSendNotificationRequest(int, void *, unsigned long, int);
extern unsigned int sceKernelUsleep(unsigned int us);
extern uint64_t sceKernelGetProcessTime(void);
extern int sceKernelAllocateMainDirectMemory(size_t, size_t, int, intptr_t *);
extern int sceKernelMapDirectMemory(void **, size_t, int, int, intptr_t, size_t);
extern int sceKernelReleaseDirectMemory(intptr_t, size_t);

struct av_note { char pad[45]; char msg[3075]; };

static void note(const char *fmt, ...)
{
    struct av_note n;
    va_list ap;
    memset(&n, 0, sizeof n);
    va_start(ap, fmt);
    vsnprintf(n.msg, sizeof n.msg, fmt, ap);
    va_end(ap);
    sceKernelSendNotificationRequest(0, &n, sizeof n, 0);
}

/* ---- watchdog: _exit()s EVO if a call hangs. A clean close the user can
 *      relaunch from is better than a wedged app slot with no remote kill. --- */
static volatile int g_stage = 0;
static volatile int g_stage_seen = -1;

static void *watchdog(void *arg)
{
    (void)arg;
    for (int i = 0; i < 45; i++) {
        sceKernelUsleep(1000 * 1000);
        if (g_stage != g_stage_seen) { g_stage_seen = g_stage; i = 0; }
    }
    note("EVO avplayer: WATCHDOG fired at stage %d - closing app", g_stage);
    _exit(90 + g_stage);
    return 0;
}

/* ---- SIGSEGV/SIGBUS guard. Armed around every call into libSceAvPlayer so a
 *      fault in the module is reported, not a bare crash-reporter popup. --- */
static sigjmp_buf g_fault_jmp;      /* top-level: any libSceAvPlayer call    */
static sigjmp_buf g_read_jmp;       /* the frame CPU-read test only          */
static volatile sig_atomic_t g_fault_armed = 0;   /* 0 none, 1 top, 2 read   */
static const char *volatile g_fault_where = "?";
static void fault_handler(int sig)
{
    int a = g_fault_armed;
    g_fault_armed = 0;
    if (a == 2) siglongjmp(g_read_jmp, sig);
    if (a == 1) siglongjmp(g_fault_jmp, sig);
    _exit(130 + sig);
}

/* ============================ memory callbacks ========================= */
static void *av_alloc(void *a, uint32_t align, uint32_t size)
{
    (void)a;
    void *p = 0;
    size_t al = align < sizeof(void *) ? sizeof(void *) : align;
    size_t pw = sizeof(void *);
    while (pw < al) pw <<= 1;
    if (posix_memalign(&p, pw, size) != 0) return 0;
    return p;
}
static void av_free(void *a, void *p) { (void)a; free(p); }

#define TEX_SLOTS 64
static struct { void *addr; intptr_t pa; size_t size; } g_tex[TEX_SLOTS];
static pthread_mutex_t g_tex_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_tex_hi = 0;
static volatile int g_tex_type = -1;   /* memory type that worked */

static void *av_alloc_texture(void *a, uint32_t align, uint32_t size)
{
    (void)a;
    size_t al = align <= 0x4000 ? 0x4000 : align;
    size_t pw = 0x4000;
    while (pw < al) pw <<= 1;
    size_t bytes = (size + pw - 1) & ~(pw - 1);

    /* SharpProspero's decode path types this MemoryTypeCachedShared (12);
     * EVO's own framebuffer uses WC_GARLIC (3). Try 12 then 3, record which. */
    intptr_t pa = 0;
    int mt = 12, rc = sceKernelAllocateMainDirectMemory(bytes, pw, 12, &pa);
    if (rc != 0) { mt = 3; rc = sceKernelAllocateMainDirectMemory(bytes, pw, 3, &pa); }
    if (rc != 0) return 0;

    void *addr = 0;
    rc = sceKernelMapDirectMemory(&addr, bytes, 0x33 /* CPU rw | GPU all */,
                                  0, pa, pw);
    if (rc != 0 || !addr) { sceKernelReleaseDirectMemory(pa, bytes); return 0; }

    pthread_mutex_lock(&g_tex_lock);
    int used = 0;
    for (int i = 0; i < TEX_SLOTS; i++) {
        if (!g_tex[i].addr) { g_tex[i].addr = addr; g_tex[i].pa = pa; g_tex[i].size = bytes; break; }
        used++;
    }
    if (used > g_tex_hi) g_tex_hi = used;
    g_tex_type = mt;
    pthread_mutex_unlock(&g_tex_lock);
    return addr;
}

static void av_free_texture(void *a, void *p)
{
    (void)a;
    if (!p) return;
    pthread_mutex_lock(&g_tex_lock);
    for (int i = 0; i < TEX_SLOTS; i++) {
        if (g_tex[i].addr == p) {
            sceKernelReleaseDirectMemory(g_tex[i].pa, g_tex[i].size);
            g_tex[i].addr = 0; g_tex[i].pa = 0; g_tex[i].size = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_tex_lock);
}

/* ============================ file callbacks ========================== */
static int g_fd = -1;
static uint64_t g_reads = 0;
static char g_openpath[256];

static int av_file_open(void *a, const char *path)
{
    (void)a;
    snprintf(g_openpath, sizeof g_openpath, "%s", path ? path : "(null)");
    g_fd = open(g_openpath, O_RDONLY);
    return g_fd >= 0 ? 0 : -1;
}
static int av_file_close(void *a)
{
    (void)a;
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    return 0;
}
static int av_file_read_offset(void *a, uint8_t *buf, uint64_t pos, uint32_t len)
{
    (void)a;
    g_reads++;
    if (g_fd < 0) return -1;
    if (lseek(g_fd, (off_t)pos, SEEK_SET) == (off_t)-1) return -1;
    return (int)read(g_fd, buf, len);
}
static uint64_t av_file_size(void *a)
{
    (void)a;
    struct stat st;
    if (g_fd < 0 || fstat(g_fd, &st) != 0) return 0;
    return (uint64_t)st.st_size;
}

/* ================================ probe =============================== */
static const char *const kCandidates[] = {
    "/data/probe.mp4",
    "/data/bunny.mp4",
    "/download0/evoplayer/probe.mp4",
    "/mnt/usb0/probe.mp4",
    "/mnt/usb0/bunny.mp4",
};

static const char *find_test_file(void)
{
    for (unsigned i = 0; i < sizeof kCandidates / sizeof *kCandidates; i++) {
        int fd = open(kCandidates[i], O_RDONLY);
        if (fd >= 0) { close(fd); return kCandidates[i]; }
    }
    return 0;
}

static void characterise(const SceAvPlayerFrameInfoEx *f)
{
    unsigned w = f->video_width, h = f->video_height, pitch = f->video_pitch;
    unsigned visw = pitch > f->crop_left + f->crop_right
                        ? pitch - f->crop_left - f->crop_right : 0;
    unsigned vish = h > f->crop_top + f->crop_bottom
                        ? h - f->crop_top - f->crop_bottom : 0;

    const char *readable = "untested";
    long cmean = -1; unsigned ymin = 0, ymax = 0;
    if (f->data && sigsetjmp(g_read_jmp, 1) == 0) {
        g_fault_armed = 2;
        const uint8_t *p = (const uint8_t *)f->data;
        const uint8_t *c = p + (size_t)pitch * h;
        volatile uint32_t acc = 0;
        ymin = 255; ymax = 0;
        for (unsigned r = 0; r < vish && r < 64; r++) {
            const uint8_t *lr = p + (size_t)(r + f->crop_top) * pitch + f->crop_left;
            for (unsigned x = 0; x < visw && x < 256; x++) {
                if (lr[x] < ymin) ymin = lr[x];
                if (lr[x] > ymax) ymax = lr[x];
            }
        }
        long csum = 0; int cn = 0;
        for (unsigned r = 0; r < vish / 2 && r < 32; r++) {
            const uint8_t *cr = c + (size_t)r * pitch;
            for (unsigned x = 0; x < 128; x++) { csum += cr[x]; cn++; acc += cr[x]; }
        }
        (void)acc;
        cmean = cn ? csum / cn : -1;
        readable = "YES";
    } else if (f->data) {
        readable = "NO (faulted)";
    }
    g_fault_armed = 1;   /* re-arm the top-level guard for the rest of the probe */

    /* dump the planes + metadata to the first writable dir */
    char path[128] = "(none)";
    const char *dirs[] = { "/download0/evoplayer", "/data", "/mnt/usb0" };
    for (unsigned i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        char meta[128];
        snprintf(meta, sizeof meta, "%s/avpx_frame0.txt", dirs[i]);
        int mf = open(meta, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (mf < 0) continue;                 /* dir not writable — try next */
        char m[400];
        int ml = snprintf(m, sizeof m,
            "w=%u h=%u pitch=%u crop=%u/%u/%u/%u visible=%ux%u ts=%llu\n"
            "cpu_readable=%s luma_min=%u luma_max=%u chroma_mean=%ld\n"
            "tex_mem_type=%d (12=CachedShared 3=WC_GARLIC)\n"
            "layout: luma[pitch*h] then chroma[pitch*h/2], NV12 assumed\n",
            w, h, pitch, f->crop_left, f->crop_right, f->crop_top, f->crop_bottom,
            visw, vish, (unsigned long long)f->timestamp_ms,
            readable, ymin, ymax, cmean, g_tex_type);
        write(mf, m, (size_t)ml);
        close(mf);

        snprintf(path, sizeof path, "%s/avpx_frame0.nv12", dirs[i]);
        if (readable[0] == 'Y') {
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd >= 0) {
                size_t ly = (size_t)pitch * h, ch = (size_t)pitch * (h / 2);
                write(fd, f->data, ly);
                write(fd, (const uint8_t *)f->data + ly, ch);
                close(fd);
            }
        }
        break;                                /* wrote to this dir — done */
    }

    note("EVO avplayer: FRAME %ux%u pitch=%u crop=%u/%u/%u/%u vis=%ux%u\n"
         "cpu-read=%s luma=%u..%u chroma~%ld texmem=%d -> %s",
         w, h, pitch, f->crop_left, f->crop_right, f->crop_top, f->crop_bottom,
         visw, vish, readable, ymin, ymax, cmean, g_tex_type, path);
}

void evo_avplayer_probe(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fault_handler;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGSYS,  &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);

    note("EVO avplayer: probe entry (libSceAvPlayer NEEDED via PRX stub)");

    /* Catch a fault anywhere in the probe body and report where we were. */
    if (sigsetjmp(g_fault_jmp, 1) != 0) {
        note("EVO avplayer: FAULT at [%s] (stage %d) - libSceAvPlayer call "
             "crashed. The .sprx loaded but is not usable from a fake-signed "
             "module.", g_fault_where, g_stage);
        _exit(70);
    }
    g_fault_armed = 1;

    pthread_t wd;
    pthread_create(&wd, NULL, watchdog, NULL);

    g_fault_where = "find_test_file";
    const char *file = find_test_file();
    if (!file) {
        note("EVO avplayer: libSceAvPlayer linked OK, but NO test file.\n"
             "Drop a small H.264 .mp4 at /data/probe.mp4 (FTP) or "
             "/mnt/usb0/probe.mp4 (USB), relaunch.");
        return;
    }
    note("EVO avplayer: test file = %s", file);

    /* --- init --- */
    g_stage = 3;
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
    init.debug_level = SCE_AVPLAYER_DEBUG_ALL;
    init.num_output_video_framebuffers = 6;
    init.auto_start = 0;

    g_fault_where = "sceAvPlayerInit";
    void *pl = sceAvPlayerInit(&init);
    if (!pl) {
        note("EVO avplayer: sceAvPlayerInit -> NULL (InitData=%zu B, want 120). "
             "Route A refused at Init.", sizeof init);
        return;
    }
    note("EVO avplayer: sceAvPlayerInit OK -> %p", pl);

    /* --- add source --- */
    g_stage = 4;
    g_fault_where = "sceAvPlayerAddSource";
    int rc = sceAvPlayerAddSource(pl, file);
    if (rc != 0) {
        note("EVO avplayer: AddSource(\"%s\") -> 0x%08x (opened=\"%s\" reads=%llu). "
             "Route A refused the file.",
             file, rc, g_openpath, (unsigned long long)g_reads);
        sceAvPlayerClose(pl);
        return;
    }

    /* --- streams --- */
    g_stage = 5;
    g_fault_where = "sceAvPlayerStreamCount/GetStreamInfo/EnableStream";
    int count = 0;
    for (int t = 0; t < 100; t++) {
        count = sceAvPlayerStreamCount(pl);
        if (count > 0) break;
        sceKernelUsleep(100 * 1000);
    }
    int vw = 0, vh = 0, taken[4] = {0,0,0,0};
    if (count > 0) {
        for (uint32_t i = 0; i < (uint32_t)count; i++) {
            SceAvPlayerStreamInfo si;
            memset(&si, 0, sizeof si);
            if (sceAvPlayerGetStreamInfo(pl, i, &si) != 0) continue;
            if (si.type == SCE_AVPLAYER_STREAM_VIDEO) {
                vw = (int)si.details.video.width;
                vh = (int)si.details.video.height;
            }
            if (si.type < 4 && !taken[si.type]) {
                sceAvPlayerEnableStream(pl, i);
                taken[si.type] = 1;
            }
        }
    }

    /* --- start + decode loop --- */
    g_stage = 6;
    g_fault_where = "sceAvPlayerSetLooping/Start";
    sceAvPlayerSetLooping(pl, 0);
    int srtc = sceAvPlayerStart(pl);
    note("EVO avplayer: Init OK, AddSource OK, streams=%d (video %dx%d), "
         "Start->0x%08x. Decoding...", count, vw, vh, srtc);

    g_stage = 7;
    g_fault_where = "sceAvPlayerGetVideoDataEx";
    int frames = 0, polls = 0;
    uint64_t t0 = sceKernelGetProcessTime();
    while (sceKernelGetProcessTime() - t0 < 15ULL * 1000 * 1000) {
        SceAvPlayerFrameInfoEx vf;
        memset(&vf, 0, sizeof vf);
        if (sceAvPlayerGetVideoDataEx(pl, &vf) && vf.data) {
            if (frames == 0) characterise(&vf);
            frames++;
        } else if (++polls % 200 == 0) {
            int act = sceAvPlayerIsActive(pl);
            if (act == 0 && polls >= 400) break;
        }
        sceKernelUsleep(5 * 1000);
    }

    /* --- shutdown --- */
    g_stage = 8;
    sceAvPlayerStop(pl);
    sceAvPlayerClose(pl);

    g_stage = 9;
    if (frames > 0)
        note("EVO avplayer: ROUTE A WORKS - %d video frame(s) hw-decoded "
             "(tex mem type %d, high-water %d). See /download0/evoplayer/avpx_frame0.*",
             frames, g_tex_type, g_tex_hi);
    else
        note("EVO avplayer: pipeline ran, %d frames. Start=0x%08x streams=%d. "
             "Route A context-limited (like Route B) - see avpx_frame0.txt",
             frames, srtc, count);
}

#endif /* EVO_AVPLAYER_PROBE */
