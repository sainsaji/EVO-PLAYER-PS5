/*
 * agc_probe.c - can a payload process drive the GPU itself? (#237, Mesa in
 * the WebProcess). The ProsperoWeb WebProcess is spawned through elfldr, not
 * as an app, so before porting a Mesa driver into it we need to know whether
 * a payload can (1) get GPU-mapped direct memory and how much, (2) init
 * libSceAgc, (3) submit its own command buffers and see them retire.
 *
 * No VideoOut anywhere (sceVideoOutOpen from a payload panics). The GPU work
 * is a DMA fill plus an end-of-pipe fence write, the smallest thing that
 * proves the queue.
 *
 * Logs to /data/prosperoweb/agc_probe.log (each line fsynced). Run with EVO
 * up (payloads see no direct memory without a foreground app). Optional
 * /data/prosperoweb/agc_probe_sizes: MB values to try, one per line.
 * Built by tools/build_glslim_probe.sh.
 */
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

int sceKernelLoadStartModule(const char *path, size_t argc, const void *argv, uint32_t flags, void *opt, int *res);
int64_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t align, int64_t *out_start,
                                       size_t *out_size);
int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align, int type, int64_t *phys);
int sceKernelReleaseDirectMemory(int64_t phys, size_t len);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t phys, size_t align);
int sceKernelMunmap(void *addr, size_t len);
int sceKernelMapFlexibleMemory(void **addr, size_t len, int prot, int flags);
int sceSysmoduleLoadModuleInternal(uint32_t id);

#define LOG_PATH "/data/prosperoweb/agc_probe.log"
#define SIZES_PATH "/data/prosperoweb/agc_probe_sizes"
#define MB (1024u * 1024u)
#define WB_ONION 12
#define PROT_CPU_GPU_RW 0x33
#define ALIGN_2M (2u * MB)
#define WORK_BYTES (4u * MB)
#define FILL_BYTES (1u * MB)
#define FRAMES 30

typedef struct {
    uint32_t *bottom, *top, *up, *down;
    uintptr_t callback;
    void *user_data;
    uint32_t reserved_dwords, padding;
} agc_cb;

typedef struct {
    const uint32_t *words;
    uint32_t count;
    uint8_t flag;
    uint8_t padding[3];
} agc_submit;

static int (*p_init)(uint32_t);
static int (*p_gnm_submit)(uint32_t, void **, uint32_t *, void **, uint32_t *);
static int (*p_gnm_done)(void);
static uint32_t *(*p_dma)(void *, uint32_t, uint32_t, uint32_t, uint64_t, uint32_t, uint32_t, uint64_t, uint32_t,
                          uint32_t, uint32_t, uint32_t);
static uint32_t *(*p_release_mem)(void *, uint8_t, int16_t, uint64_t, int8_t, void *, uint32_t, uint64_t, uint16_t,
                                  uint16_t, int8_t, int32_t);

static int g_log = -1;

static void logf_(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof buf - 2)
        n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    if (g_log >= 0) {
        (void)write(g_log, buf, (size_t)n);
        fsync(g_log);
    }
    (void)write(1, buf, (size_t)n);
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void clflush_range(const void *p, size_t n)
{
    const uint8_t *at = (const uint8_t *)((uintptr_t)p & ~(uintptr_t)63);
    const uint8_t *end = (const uint8_t *)p + n;
    for (; at < end; at += 64)
        __asm__ volatile("clflush (%0)" : : "r"(at) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

static void *watchdog(void *arg)
{
    sleep((unsigned)(uintptr_t)arg);
    logf_("watchdog: probe still running after %u s, exiting", (unsigned)(uintptr_t)arg);
    _exit(3);
    return NULL;
}

/* Console signal context: rip is word 28, rsp word 31 (the SDK ucontext_t
 * layout is wrong here; see ps5_glslim.c). */
static void on_crash(int sig, siginfo_t *si, void *ctx)
{
    const uint64_t *w = (const uint64_t *)ctx;
    logf_("CRASH signal %d addr %p rip %#llx rsp %#llx", sig, si ? si->si_addr : NULL,
          (unsigned long long)(w ? w[28] : 0), (unsigned long long)(w ? w[31] : 0));
    _exit(4);
}

static int cb_full(void *cb, uint32_t dwords, void *user)
{
    (void)cb;
    (void)dwords;
    (void)user;
    return 0;
}

static uint32_t load_module(const char *name)
{
    static const char *dirs[] = {"/system/common/lib/", "/system_ex/common_ex/lib/", "/system/priv/lib/"};
    uint32_t h = 0;
    logf_("handle lookup %s ...", name);
    if (kernel_dynlib_handle(getpid(), name, &h) == 0) {
        logf_("%s already loaded: handle %u", name, h);
        return h;
    }
    for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; ++i) {
        char path[128];
        snprintf(path, sizeof path, "%s%s", dirs[i], name);
        int res = 0;
        logf_("loading %s ...", path);
        int r = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &res);
        logf_("load %s -> %#x (res %d)", path, (unsigned)r, res);
        if (r >= 0)
            return (uint32_t)r;
    }
    return 0;
}

static void *resolve(uint32_t h, const char *nid, const char *name)
{
    void *p = (void *)kernel_dynlib_resolve(getpid(), h, nid);
    logf_("  %s (%s) -> %p", name, nid, p);
    return p;
}

/* Allocate + map `mb` MB of GPU-visible direct memory, time it, touch it,
 * release it. Returns 0 when the whole size worked. */
static int try_size(unsigned mb)
{
    size_t len = (size_t)mb * MB;
    int64_t phys = 0;
    uint64_t t0 = now_us();
    int r = sceKernelAllocateDirectMemory(0, (int64_t)1 << 36, len, ALIGN_2M, WB_ONION, &phys);
    if (r != 0) {
        logf_("size %u MB: allocate -> %#x", mb, (unsigned)r);
        return -1;
    }
    void *va = NULL;
    r = sceKernelMapDirectMemory(&va, len, PROT_CPU_GPU_RW, 0, phys, ALIGN_2M);
    uint64_t t1 = now_us();
    if (r != 0) {
        logf_("size %u MB: allocate ok (phys %#llx), map prot 0x33 -> %#x", mb, (unsigned long long)phys,
              (unsigned)r);
        sceKernelReleaseDirectMemory(phys, len);
        return -1;
    }
    memset(va, 0x5a, len);
    uint64_t t2 = now_us();
    logf_("size %u MB: ok va %p phys %#llx, alloc+map %llu us, memset %llu us", mb, va,
          (unsigned long long)phys, (unsigned long long)(t1 - t0), (unsigned long long)(t2 - t1));
    sceKernelMunmap(va, len);
    sceKernelReleaseDirectMemory(phys, len);
    return 0;
}

/* Flexible memory with GPU access, in case direct memory is closed to us. */
static void flex_test(void)
{
    static const unsigned mbs[] = {2, 16, 64, 256};
    for (unsigned i = 0; i < sizeof mbs / sizeof mbs[0]; ++i) {
        void *va = NULL;
        int r = sceKernelMapFlexibleMemory(&va, (size_t)mbs[i] * MB, PROT_CPU_GPU_RW, 0);
        logf_("flexible %u MB prot 0x33 -> %#x (%p)", mbs[i], (unsigned)r, va);
        if (r != 0)
            break;
        sceKernelMunmap(va, (size_t)mbs[i] * MB);
    }
}

int main(void)
{
    mkdir("/data/prosperoweb", 0777);
    g_log = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    pthread_t wd;
    pthread_create(&wd, NULL, watchdog, (void *)(uintptr_t)60);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);

    pid_t me = getpid();
    logf_("agc_probe pid %d authid %#llx", (int)me, (unsigned long long)kernel_get_ucred_authid(me));

    int64_t total = sceKernelGetDirectMemorySize(), start = 0;
    size_t avail = 0;
    int r = sceKernelAvailableDirectMemorySize(0, total > 0 ? total : (int64_t)1 << 36, ALIGN_2M, &start, &avail);
    logf_("direct memory: total %lld MB, largest free %zu MB at %#llx (rc %#x)", (long long)(total >> 20),
          avail >> 20, (unsigned long long)start, (unsigned)r);

    /* 1. How much GPU-mapped direct memory can a payload take? */
    unsigned sizes[16] = {2, 4, 8, 16, 32, 64, 128, 256, 512}, nsizes = 9;
    FILE *sf = fopen(SIZES_PATH, "r");
    if (sf) {
        nsizes = 0;
        unsigned v;
        while (nsizes < 16 && fscanf(sf, "%u", &v) == 1)
            sizes[nsizes++] = v;
        fclose(sf);
    }
    for (unsigned i = 0; i < nsizes; ++i)
        if (try_size(sizes[i]) != 0)
            break;
    flex_test();

    /* 1b. Process identity. /data/prosperoweb/agc_probe_evopid: EVO's pid,
     * to log its category word (proc +0x4b0 on 13.60) next to ours.
     * /data/prosperoweb/agc_probe_cat (hex): poke ours to that value before
     * anything GPU-related loads (the compositor accepted us after this). */
    {
        intptr_t myproc = kernel_get_proc(me);
        uint64_t mine = 0, theirs = 0;
        kernel_copyout(myproc + 0x4b0, &mine, 8);
        int evopid = 0;
        FILE *ef = fopen("/data/prosperoweb/agc_probe_evopid", "r");
        if (ef) {
            if (fscanf(ef, "%d", &evopid) != 1)
                evopid = 0;
            fclose(ef);
        }
        if (evopid > 0) {
            intptr_t ep = kernel_get_proc(evopid);
            if (ep)
                kernel_copyout(ep + 0x4b0, &theirs, 8);
            logf_("category: ours %#llx, EVO (pid %d, authid %#llx) %#llx", (unsigned long long)mine, evopid,
                  (unsigned long long)kernel_get_ucred_authid(evopid), (unsigned long long)theirs);
        } else {
            logf_("category: ours %#llx", (unsigned long long)mine);
        }
        unsigned long long want = 0;
        FILE *cf = fopen("/data/prosperoweb/agc_probe_cat", "r");
        if (cf) {
            if (fscanf(cf, "%llx", &want) == 1) {
                uint32_t v = (uint32_t)want;
                int pr = kernel_copyin(&v, myproc + 0x4b0, 4);
                kernel_copyout(myproc + 0x4b0, &mine, 8);
                logf_("category poke %#x -> rc %d, now %#llx", v, pr, (unsigned long long)mine);
            }
            fclose(cf);
        }
    }

    /* 1c. The GPU device itself: what libSceGnmDriver's init does first. */
    {
        int fd = open("/dev/gc", O_RDWR);
        int e = fd < 0 ? errno : 0;
        logf_("open /dev/gc -> %d (errno %d)", fd, e);
        if (fd >= 0) {
            uint64_t out = 0;
            errno = 0;
            int ir = ioctl(fd, 0xc008811bUL, &out);
            logf_("ioctl 0xc008811b -> %d (errno %d) out %#llx", ir, ir ? errno : 0, (unsigned long long)out);
            close(fd);
        }
    }

    /* 2. libSceAgc in a payload. A plain sceKernelLoadStartModule of it
     * killed the probe outright (no signal, no exit line); GLSlim gets it via
     * Sony's sysmodules, so preload those first. */
    static const uint32_t mods[] = {0x80000086, 0x8000008b, 0x80000004, 0x8000001f};
    for (unsigned i = 0; i < sizeof mods / sizeof mods[0]; ++i) {
        logf_("sysmodule %#x ...", mods[i]);
        logf_("sysmodule %#x -> %#x", mods[i], (unsigned)sceSysmoduleLoadModuleInternal(mods[i]));
    }
    static const char *names[] = {"libSceAgc.sprx", "libSceAgcDriver.sprx", "libSceGnmDriver.sprx",
                                  "libSceAgcVsh.sprx", "libSceAgcDriverVsh.sprx", "libSceGLSlimVSH.sprx"};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i) {
        uint32_t h = 0;
        int hr = kernel_dynlib_handle(getpid(), names[i], &h);
        logf_("after preload: %s handle rc %d h %u", names[i], hr, h);
    }
    /* /data/prosperoweb/agc_probe_load: load only the named module, report,
     * stop. Finds which driver module kills a payload. */
    FILE *lf = fopen("/data/prosperoweb/agc_probe_load", "r");
    if (lf) {
        char want[64] = {0};
        if (fscanf(lf, "%63s", want) == 1) {
            fclose(lf);
            uint32_t h = load_module(want);
            logf_("single load %s -> handle %u", want, h);
            if (h) {
                resolve(h, "UglJIZjGssM", "sceAgcDriverSubmitDcb");
                resolve(h, "kW3GLb7QfPg", "sceAgcInit");
                resolve(h, "zwY0YV91TTI", "sceGnmSubmitCommandBuffers");
                resolve(h, "xbxNatawohc", "sceGnmSubmitAndFlipCommandBuffers");
            }
            logf_("RESULT: single load done");
            return 0;
        }
        fclose(lf);
    }
    if (access("/data/prosperoweb/agc_probe_noload", F_OK) == 0) {
        logf_("agc_probe_noload set: stopping before any module load");
        return 0;
    }
    /* libSceAgc/libSceAgcDriver kill a payload on load (libSceAgcDriver is
     * the one). So: command builders from libSceAgcVsh (the sysmodules above
     * load it), submit through libSceGnmDriver, which loads fine. */
    uint32_t agc = 0;
    if (kernel_dynlib_handle(getpid(), "libSceAgcVsh.sprx", &agc) != 0)
        agc = 0;
    uint32_t gnm = load_module("libSceGnmDriver.sprx");
    if (!agc || !gnm) {
        logf_("FAIL: modules agcvsh %u gnm %u", agc, gnm);
        return 1;
    }
    p_init = (int (*)(uint32_t))resolve(agc, "kW3GLb7QfPg", "sceAgcInit");
    p_dma = resolve(agc, "WmAc2MEj6Io", "sceAgcDcbDmaData");
    p_release_mem = resolve(agc, "wr23dPKyWc0", "sceAgcCbReleaseMem");
    p_gnm_submit = resolve(gnm, "zwY0YV91TTI", "sceGnmSubmitCommandBuffers");
    p_gnm_done = resolve(gnm, "yvZ73uQUqrk", "sceGnmSubmitDone");
    if (!p_init || !p_dma || !p_release_mem || !p_gnm_submit || !p_gnm_done) {
        logf_("FAIL: missing symbols");
        return 1;
    }

    /* Working block: command buffer, fence, fill target. */
    int64_t phys = 0;
    void *base = NULL;
    r = sceKernelAllocateDirectMemory(0, (int64_t)1 << 36, WORK_BYTES, ALIGN_2M, WB_ONION, &phys);
    if (r == 0)
        r = sceKernelMapDirectMemory(&base, WORK_BYTES, PROT_CPU_GPU_RW, 0, phys, ALIGN_2M);
    if (r != 0) {
        logf_("FAIL: working block %u MB -> %#x", WORK_BYTES / MB, (unsigned)r);
        return 1;
    }
    memset(base, 0, WORK_BYTES);
    clflush_range(base, 2u * MB);
    uint32_t *dcb = (uint32_t *)base;                                 /* 1 MB */
    volatile uint32_t *fence = (volatile uint32_t *)((uint8_t *)base + MB); /* 256 B */
    uint8_t *fill = (uint8_t *)base + 2u * MB;                        /* 4 MB */

    uint64_t t0 = now_us();
    r = p_init(8);
    logf_("sceAgcInit(8) -> %#x (%llu us)", (unsigned)r, (unsigned long long)(now_us() - t0));
    if (r != 0) {
        logf_("FAIL: sceAgcInit");
        return 1;
    }

    /* 3. Own submissions: DMA fill 4 MB with a per-frame pattern, then an
     * end-of-pipe fence write. Spin on the fence with a 2 s limit. */
    unsigned ok = 0;
    uint64_t worst = 0, sum = 0;
    for (unsigned f = 1; f <= FRAMES; ++f) {
        agc_cb cb;
        memset(&cb, 0, sizeof cb);
        cb.bottom = dcb;
        cb.up = dcb;
        cb.top = dcb + MB / 4;
        cb.down = cb.top;
        cb.callback = (uintptr_t)cb_full;
        uint32_t pattern = 0xA5000000u | f;
        p_dma(&cb, 0u, 3u, 0u, (uint64_t)(uintptr_t)fill, 2u, 0u, pattern, FILL_BYTES, 0u, 0u, 1u);
        p_release_mem(&cb, 40, 0x30c, 0, 0, (void *)fence, 1, f, 0, 0, 0, 0);
        uint32_t dwords = (uint32_t)(cb.up - cb.bottom);
        clflush_range(dcb, (size_t)dwords * 4u);
        void *dcbs[1] = {dcb};
        uint32_t sizes_b[1] = {dwords * 4u};
        uint64_t ts = now_us();
        int sr = p_gnm_submit(1, dcbs, sizes_b, NULL, NULL);
        int sp = p_gnm_done();
        if (sr != 0) {
            logf_("frame %u: submit -> %#x (suspend %#x), stopping", f, (unsigned)sr, (unsigned)sp);
            break;
        }
        uint32_t seen = 0;
        uint64_t te;
        for (;;) {
            clflush_range((const void *)fence, 4);
            seen = *fence;
            te = now_us();
            if (seen == f || te - ts > 2000000u)
                break;
        }
        if (seen != f) {
            logf_("frame %u: fence TIMEOUT (fence %#x, dwords %u, suspend %#x), stopping", f, seen, dwords,
                  (unsigned)sp);
            break;
        }
        clflush_range(fill, 64);
        clflush_range(fill + FILL_BYTES - 64, 64);
        uint32_t first = *(volatile uint32_t *)fill;
        uint32_t last = *(volatile uint32_t *)(fill + FILL_BYTES - 4);
        uint64_t us = te - ts;
        if (f <= 3 || f == FRAMES || first != pattern || last != pattern)
            logf_("frame %u: dwords %u submit %#x suspend %#x fence ok in %llu us, fill first %#x last %#x (want %#x)",
                  f, dwords, (unsigned)sr, (unsigned)sp, (unsigned long long)us, first, last, pattern);
        if (first == pattern && last == pattern)
            ++ok;
        sum += us;
        if (us > worst)
            worst = us;
    }
    logf_("RESULT: %u/%u frames filled correctly, fence avg %llu us, worst %llu us", ok, FRAMES,
          (unsigned long long)(ok ? sum / ok : 0), (unsigned long long)worst);
    logf_(ok == FRAMES ? "PASS: payload owns GPU memory and its own queue" : "FAIL");

    sceKernelMunmap(base, WORK_BYTES);
    sceKernelReleaseDirectMemory(phys, WORK_BYTES);
    if (g_log >= 0)
        close(g_log);
    return ok == FRAMES ? 0 : 2;
}
