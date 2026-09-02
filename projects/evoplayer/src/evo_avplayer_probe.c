/*
 * evo_avplayer_probe.c - libSceAvPlayer native-decode gate, run from inside
 * the PPSA99039 app module. See evo_avplayer_probe.h and
 * docs/evo-pro/avplayer-abi.md. Compiled only under -DEVO_AVPLAYER_PROBE.
 *
 * App-module context: NO kernel R/W, NO stdout to klog. Module load + symbol
 * resolution go through sceKernelLoadStartModule + sceKernelDlsym-by-NID (NID
 * computed here from OpenSSL SHA1, exactly as evo_agc_probe.c does). Every
 * result comes back as a system-notification popup.
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

#include <openssl/sha.h>

/* --- app-module module loader + dlsym (userland; no stub header in EVO) --- */
extern int sceKernelLoadStartModule(const char *name, unsigned long argc,
                                    const void *argv, unsigned int flags,
                                    void *opt, int *res);
extern int sceKernelDlsym(int handle, const char *symbol, void **addr);
extern int sceKernelSendNotificationRequest(int, void *, unsigned long, int);
extern unsigned int sceKernelUsleep(unsigned int us);
extern uint64_t sceKernelGetProcessTime(void);

/* direct memory — same entry points main.c uses for the app-module framebuffer */
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

/* Sony NID: SHA1(name + salt), first 8 bytes reversed, base64 w/ custom
 * alphabet, keep 11 chars. Verified byte-for-byte against prospero-nid — this
 * is a straight copy of evo_agc_probe.c's nid_of(). */
static void nid_of(const char *sym, char out[12])
{
    static const uint8_t salt[16] = {
        0x51,0x8D,0x64,0xA6,0x35,0xDE,0xD8,0xC1,
        0xE6,0xB0,0x39,0xB1,0xC3,0xE5,0x52,0x30};
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";

    uint8_t buf[256], hash[SHA_DIGEST_LENGTH], d[9] = {0};
    size_t n = strlen(sym);
    if (n > sizeof(buf) - sizeof(salt)) n = sizeof(buf) - sizeof(salt);
    memcpy(buf, sym, n);
    memcpy(buf + n, salt, sizeof(salt));
    SHA1(buf, n + sizeof(salt), hash);

    for (int i = 0; i < 8; i++) d[i] = hash[7 - i];

    char tmp[12];
    int p = 0;
    for (int i = 0; i < 9; i += 3) {
        int abc = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
        tmp[p++] = b64[(abc >> 18) & 0x3f];
        tmp[p++] = b64[(abc >> 12) & 0x3f];
        tmp[p++] = b64[(abc >> 6) & 0x3f];
        tmp[p++] = b64[abc & 0x3f];
    }
    memcpy(out, tmp, 11);
    out[11] = '\0';
}

static int load_module(const char *basename)
{
    static const char *const dirs[] = {
        "/system/common/lib/", "/system/priv/lib/", "/system_ex/common_ex/lib/",
    };
    char path[256];
    for (unsigned i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        int res = 0;
        snprintf(path, sizeof path, "%s%s", dirs[i], basename);
        int id = sceKernelLoadStartModule(path, 0, 0, 0, 0, &res);
        if (id >= 0)
            return id;
    }
    return -1;
}

static void *resolve(int mod, const char *name)
{
    void *a = 0;
    char nid[12];
    nid_of(name, nid);
    if (sceKernelDlsym(mod, nid, &a) == 0 && a) return a;
    if (sceKernelDlsym(mod, name, &a) == 0 && a) return a;
    return 0;
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

/* ---- SIGSEGV/SIGBUS guard for the "is the frame CPU-readable" probe ---- */
static sigjmp_buf g_fault_jmp;
static volatile sig_atomic_t g_fault_armed = 0;
static void fault_handler(int sig)
{
    if (g_fault_armed) { g_fault_armed = 0; siglongjmp(g_fault_jmp, sig); }
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
struct api {
    void   *(*Init)(const SceAvPlayerInitData *);
    int32_t (*AddSource)(void *, const char *);
    int32_t (*Start)(void *);
    int32_t (*Stop)(void *);
    int32_t (*IsActive)(void *);
    int32_t (*SetLooping)(void *, int32_t);
    int32_t (*StreamCount)(void *);
    int32_t (*GetStreamInfo)(void *, uint32_t, SceAvPlayerStreamInfo *);
    int32_t (*EnableStream)(void *, uint32_t);
    int32_t (*GetVideoDataEx)(void *, SceAvPlayerFrameInfoEx *);
    int32_t (*Close)(void *);
};

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
    if (f->data && sigsetjmp(g_fault_jmp, 1) == 0) {
        g_fault_armed = 1;
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
        g_fault_armed = 0;
        cmean = cn ? csum / cn : -1;
        readable = "YES";
    } else if (f->data) {
        readable = "NO (faulted)";
    }

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
    sigaction(SIGBUS, &sa, NULL);

    pthread_t wd;
    pthread_create(&wd, NULL, watchdog, NULL);

    /* --- module + symbols --- */
    g_stage = 1;
    int mod = load_module("libSceAvPlayer.sprx");
    if (mod < 0) {
        note("EVO avplayer: libSceAvPlayer.sprx load FAILED - Route A blocked");
        return;
    }

    g_stage = 2;
    struct api A;
    memset(&A, 0, sizeof A);
    A.Init           = resolve(mod, "sceAvPlayerInit");
    A.AddSource      = resolve(mod, "sceAvPlayerAddSource");
    A.Start          = resolve(mod, "sceAvPlayerStart");
    A.Stop           = resolve(mod, "sceAvPlayerStop");
    A.IsActive       = resolve(mod, "sceAvPlayerIsActive");
    A.SetLooping     = resolve(mod, "sceAvPlayerSetLooping");
    A.StreamCount    = resolve(mod, "sceAvPlayerStreamCount");
    A.GetStreamInfo  = resolve(mod, "sceAvPlayerGetStreamInfo");
    A.EnableStream   = resolve(mod, "sceAvPlayerEnableStream");
    A.GetVideoDataEx = resolve(mod, "sceAvPlayerGetVideoDataEx");
    A.Close          = resolve(mod, "sceAvPlayerClose");

    int core = (A.Init && A.AddSource && A.Start && A.GetVideoDataEx && A.Close);
    if (!core) {
        note("EVO avplayer: modid=0x%x but core NIDs unresolved "
             "(Init=%p AddSrc=%p Start=%p GetVidEx=%p Close=%p)",
             mod, (void *)A.Init, (void *)A.AddSource, (void *)A.Start,
             (void *)A.GetVideoDataEx, (void *)A.Close);
        return;
    }

    const char *file = find_test_file();
    if (!file) {
        note("EVO avplayer: modid=0x%x, all NIDs OK, but NO test file.\n"
             "Drop a small H.264 .mp4 at /data/probe.mp4 (FTP) or "
             "/mnt/usb0/probe.mp4 (USB), relaunch.");
        return;
    }

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

    void *pl = A.Init(&init);
    if (!pl) {
        note("EVO avplayer: sceAvPlayerInit -> NULL (InitData=%zu B, want 120). "
             "Route A refused at Init.", sizeof init);
        return;
    }

    /* --- add source --- */
    g_stage = 4;
    int rc = A.AddSource(pl, file);
    if (rc != 0) {
        note("EVO avplayer: AddSource(\"%s\") -> 0x%08x (opened=\"%s\" reads=%llu). "
             "Route A refused the file.",
             file, rc, g_openpath, (unsigned long long)g_reads);
        A.Close(pl);
        return;
    }

    /* --- streams --- */
    g_stage = 5;
    int count = 0;
    for (int t = 0; t < 100; t++) {
        count = A.StreamCount ? A.StreamCount(pl) : -1;
        if (count > 0) break;
        sceKernelUsleep(100 * 1000);
    }
    int vw = 0, vh = 0, taken[4] = {0,0,0,0};
    if (count > 0 && A.GetStreamInfo && A.EnableStream) {
        for (uint32_t i = 0; i < (uint32_t)count; i++) {
            SceAvPlayerStreamInfo si;
            memset(&si, 0, sizeof si);
            if (A.GetStreamInfo(pl, i, &si) != 0) continue;
            if (si.type == SCE_AVPLAYER_STREAM_VIDEO) {
                vw = (int)si.details.video.width;
                vh = (int)si.details.video.height;
            }
            if (si.type < 4 && !taken[si.type]) {
                A.EnableStream(pl, i);
                taken[si.type] = 1;
            }
        }
    }

    /* --- start + decode loop --- */
    g_stage = 6;
    if (A.SetLooping) A.SetLooping(pl, 0);
    int srtc = A.Start(pl);
    note("EVO avplayer: Init OK, AddSource OK, streams=%d (video %dx%d), "
         "Start->0x%08x. Decoding...", count, vw, vh, srtc);

    g_stage = 7;
    int frames = 0, polls = 0;
    uint64_t t0 = sceKernelGetProcessTime();
    while (sceKernelGetProcessTime() - t0 < 15ULL * 1000 * 1000) {
        SceAvPlayerFrameInfoEx vf;
        memset(&vf, 0, sizeof vf);
        if (A.GetVideoDataEx(pl, &vf) && vf.data) {
            if (frames == 0) characterise(&vf);
            frames++;
        } else if (++polls % 200 == 0) {
            int act = A.IsActive ? A.IsActive(pl) : -1;
            if (act == 0 && polls >= 400) break;
        }
        sceKernelUsleep(5 * 1000);
    }

    /* --- shutdown --- */
    g_stage = 8;
    if (A.Stop) A.Stop(pl);
    A.Close(pl);

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
