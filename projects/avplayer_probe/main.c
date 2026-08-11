/* EVO Player - avplayer_probe
 *
 * ROUTE A, as an instrument rather than a fallback.
 *
 * RUN IT WITH ./scripts/install-homebrew.sh --run, NOT ./scripts/deploy.sh.
 * And run projects/slotcheck first - via deploy.sh - to confirm the app slot is
 * clear. That check exists because this probe can hang, and a hung payload
 * holds the slot.
 *
 * ---------------------------------------------------------------------------
 * THE QUESTION THIS EXISTS TO ANSWER
 *
 * Route B walked sceVideodec2Decode through five gates and stopped at the last
 * one: the hardware submit, ioctl(fd, _IOW(0x83, 23, 24), ...), refused with
 * driver errno 5200. Two hypotheses remain and Route B cannot separate them:
 *
 *   (a) the decode job we build is still malformed - which is exactly what the
 *       previous driver errno in this chain, 5031, turned out to be (an
 *       unaligned length, one line to fix); or
 *   (b) the driver requires arbitration, and a payload cannot have it.
 *
 * (b) looked testable until it wasn't. sceVideoDecoderArbitrationInitialize's
 * ABI was read and confirmed 7/7 by controls - and then the real call BLOCKED,
 * indefinitely, on what looks like an IPC to a service that will not answer a
 * process with no user session. Route B cannot ask the question.
 *
 * libSceAvPlayer can. It decodes video on this console today, through these
 * same modules, and it calls arbitration Initialize/Enable before it decodes
 * anything. So:
 *
 *   IF sceAvPlayerInit RETURNS, arbitration is reachable from this process
 *   after all, and Route B's hang was our sequence rather than our privileges.
 *
 *   IF IT HANGS IN THE SAME PLACE, that is the wall - and "hardware decode
 *   needs a capability homebrew does not have" is a real finding that stops
 *   the next person spending fifteen deploys discovering it.
 *
 * The discriminator is cheap and does not need AvPlayer to play anything:
 * after Init returns, call sceVideoDecoderArbitrationInitialize ourselves. If
 * AvPlayer brought arbitration up, our call returns 0x81570002 ("already
 * initialised") INSTEAD OF HANGING. A call that previously blocked forever
 * returning an error code is about as unambiguous as this work gets.
 *
 * ---------------------------------------------------------------------------
 * WHY EVERY CALLBACK IS INSTRUMENTED
 *
 * docs/hardware-decode-next-steps.md, Route A: "instrument the allocator and
 * file callbacks BEFORE the first call, so they log their arguments and return
 * something benign. A failed sceAvPlayerInit with instrumented callbacks yields
 * the struct layout, the allocation pattern and the file-access model in one
 * deploy. Without them it yields an error code."
 *
 * So every one of the nine callbacks logs its arguments. Even if Init fails,
 * the run reports how AvPlayer wanted to allocate, in what order, with what
 * alignments, and how it reads a file. None of that is recoverable from an
 * error code, and all of it is needed by Phase 10 whichever route wins.
 *
 * ---------------------------------------------------------------------------
 * THE INIT STRUCT
 *
 * sceAvPlayerInit takes ONE argument and does not fault on NULL. [E] Two of its
 * fields are known from the disassembly and they anchor the whole layout:
 * [rdi+0x60] is range-checked 1..4, and [rdi+0x64] is clamped into 637..767 and
 * used to derive several thread priorities - debugLevel and basePriority, at
 * exactly the offsets the PS4 SceAvPlayerInitData puts them. [E]/[I] The rest of
 * the layout below is the PS4 one [H]; those two confirmed offsets are what
 * make it credible, and the callbacks will show immediately if it is wrong,
 * because a misaligned struct means AvPlayer calls a garbage pointer instead of
 * our allocator.
 *
 * That is also why the first thing this probe does is a NULL Init as a control:
 * findings say NULL returns 0 rather than faulting, so if that stops being true
 * the struct reading is not the problem, the module is.
 *
 * ---------------------------------------------------------------------------
 * SAFETY
 *   - Ordered cheapest-first. Module load, entry-point resolution, the NULL
 *     control and the whole callback wiring are logged before Init is called.
 *   - Every allocation the callbacks make is tracked and freed at the end.
 *   - The file callbacks serve an MP4 linked into .rodata, so nothing is copied
 *     onto the console and no filesystem is touched.
 *   - AddSource and the playback loop are opt-in (`--args "eboot.elf play"`).
 *     Init alone answers the arbitration question, and Init alone is the risky
 *     part; there is no reason to spend the first run on both.
 *   - Log flushed after every line and written to /mnt/usb0. No watchdog: it
 *     does not fire on 12.70. The guard is `timeout` on the PC.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/nid.h>

#include "evo_ps5.h"

#define USB_DIR  "/mnt/usb0"
#define LOG_PATH USB_DIR "/evo_avplayer_log.txt"

#define MAX_ALLOCS 256

extern const uint8_t  test_clip_mp4[];
extern const uint64_t test_clip_mp4_len;

/* AvPlayer needs its own dependencies; VdecCore reaches sysmodule, and the
 * Phase 4/5 lessons about loading every module in the chain apply here too. */
static const char *const kModules[] = {
    "libSceVdecCore.sprx",
    "libSceVideoDecoderArbitration.sprx",
    "libSceVideodec2.sprx",
    "libSceVdecwrap.sprx",
    "libSceVdecShevc.sprx",
    "libSceGnmDriver.sprx",
    "libSceSysmodule.sprx",
    "libSceAjm.sprx",
    "libSceAvPlayer.sprx",
};

/* ------------------------------------------------------------------------- */
/* Logging                                                                   */
/* ------------------------------------------------------------------------- */

static FILE *g_log;

static void LOG(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void
LOG(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);

    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

/* ------------------------------------------------------------------------- */
/* SceAvPlayerInitData                                                       */
/* ------------------------------------------------------------------------- */

typedef struct {
    void *objectPointer;
    void *(*allocate)(void *p, uint32_t alignment, uint32_t size);
    void  (*deallocate)(void *p, void *memory);
    void *(*allocateTexture)(void *p, uint32_t alignment, uint32_t size);
    void  (*deallocateTexture)(void *p, void *memory);
} SceAvPlayerMemAllocator;

typedef struct {
    void    *objectPointer;
    int      (*open)(void *p, const char *filename);
    int      (*close)(void *p);
    int      (*readOffset)(void *p, uint8_t *buf, uint64_t pos, uint32_t len);
    uint64_t (*size)(void *p);
} SceAvPlayerFileReplacement;

typedef struct {
    void *objectPointer;
    void  (*eventCallback)(void *p, int32_t eventId, int32_t sourceId,
                           void *eventData);
} SceAvPlayerEventReplacement;

typedef struct {
    SceAvPlayerMemAllocator     memoryReplacement;    /* +0x00 */
    SceAvPlayerFileReplacement  fileReplacement;      /* +0x28 */
    SceAvPlayerEventReplacement eventReplacement;     /* +0x50 */
    int32_t  debugLevel;                              /* +0x60  1..4  [E]   */
    uint32_t basePriority;                            /* +0x64  637..767 [E]*/
    int32_t  numOutputVideoFrameBuffers;              /* +0x68 */
    uint8_t  autoStart;                               /* +0x6c */
    uint8_t  reserved[3];
    const char *defaultLanguage;                      /* +0x70 */
} SceAvPlayerInitData;                                /* 0x78 */

/* The two offsets the disassembly pinned down. If either of these is wrong the
 * whole struct is wrong and AvPlayer will call a garbage function pointer, so
 * they are worth asserting at compile time rather than discovering on a
 * console. */
_Static_assert(__builtin_offsetof(SceAvPlayerInitData, debugLevel) == 0x60,
               "debugLevel is range-checked at +0x60");
_Static_assert(__builtin_offsetof(SceAvPlayerInitData, basePriority) == 0x64,
               "basePriority is clamped at +0x64");
_Static_assert(sizeof(SceAvPlayerInitData) == 0x78, "init data is 0x78 bytes");

typedef void *(*avplayer_init_fn)(SceAvPlayerInitData *);
typedef int    (*avplayer_addsource_fn)(void *handle, const char *path);
typedef int    (*avplayer_isactive_fn)(void *handle);
typedef int    (*avplayer_getvideo_fn)(void *handle, void *frameInfo);
typedef int    (*avplayer_close_fn)(void *handle);
typedef int    (*avplayer_start_fn)(void *handle);
typedef int    (*avplayer_streamcount_fn)(void *handle);
typedef int    (*avplayer_streaminfo_fn)(void *handle, uint32_t id, void *info);
typedef int    (*avplayer_enablestream_fn)(void *handle, uint32_t id);

typedef struct {
    uint64_t thisSize;
    uint32_t priority;
    uint32_t pad;
    uint64_t count;
} ArbParams;

typedef int (*arb_init_fn)(const ArbParams *);

/* ------------------------------------------------------------------------- */
/* Instrumented callbacks                                                    */
/* ------------------------------------------------------------------------- */

static struct {
    void   *ptr;
    size_t  size;
    int     texture;
} g_allocs[MAX_ALLOCS];

static int    g_nallocs;
static size_t g_total_alloc, g_total_texture;
static int    g_calls_alloc, g_calls_free, g_calls_open, g_calls_read;
static int    g_calls_size, g_calls_close, g_calls_event;
static uint64_t g_bytes_read;

static void
track(void *p, size_t size, int texture)
{
    if (g_nallocs < MAX_ALLOCS) {
        g_allocs[g_nallocs].ptr     = p;
        g_allocs[g_nallocs].size    = size;
        g_allocs[g_nallocs].texture = texture;
        g_nallocs++;
    }
    if (texture)
        g_total_texture += size;
    else
        g_total_alloc += size;
}

static void *
cb_allocate(void *p, uint32_t alignment, uint32_t size)
{
    void *r = NULL;

    g_calls_alloc++;
    if (posix_memalign(&r, alignment < sizeof(void *) ? sizeof(void *)
                                                      : alignment, size) != 0)
        r = NULL;
    LOG("  [alloc  %3d] obj=%p align=%-6u size=%-10u -> %p\n",
        g_calls_alloc, p, alignment, size, r);
    if (r)
        track(r, size, 0);
    return r;
}

static void
cb_deallocate(void *p, void *memory)
{
    g_calls_free++;
    LOG("  [free   %3d] obj=%p mem=%p\n", g_calls_free, p, memory);
    for (int i = 0; i < g_nallocs; i++)
        if (g_allocs[i].ptr == memory)
            g_allocs[i].ptr = NULL;
    free(memory);
}

static void *
cb_allocate_texture(void *p, uint32_t alignment, uint32_t size)
{
    void *r = NULL;

    g_calls_alloc++;
    if (posix_memalign(&r, alignment < sizeof(void *) ? sizeof(void *)
                                                      : alignment, size) != 0)
        r = NULL;
    LOG("  [alloc-T%3d] obj=%p align=%-6u size=%-10u -> %p  (texture)\n",
        g_calls_alloc, p, alignment, size, r);
    if (r)
        track(r, size, 1);
    return r;
}

static void
cb_deallocate_texture(void *p, void *memory)
{
    g_calls_free++;
    LOG("  [free-T %3d] obj=%p mem=%p\n", g_calls_free, p, memory);
    for (int i = 0; i < g_nallocs; i++)
        if (g_allocs[i].ptr == memory)
            g_allocs[i].ptr = NULL;
    free(memory);
}

/* The file callbacks serve the MP4 out of .rodata. AvPlayer never learns it is
 * not a file, and we learn its exact access pattern. */
static int
cb_open(void *p, const char *filename)
{
    g_calls_open++;
    LOG("  [open      ] obj=%p name=\"%s\" -> 0 (serving the embedded clip,"
        " %llu bytes)\n",
        p, filename ? filename : "(null)",
        (unsigned long long)test_clip_mp4_len);
    return 0;
}

static int
cb_close(void *p)
{
    g_calls_close++;
    LOG("  [close     ] obj=%p -> 0\n", p);
    return 0;
}

static int
cb_read_offset(void *p, uint8_t *buf, uint64_t pos, uint32_t len)
{
    uint64_t avail;

    g_calls_read++;
    if (pos >= test_clip_mp4_len) {
        LOG("  [read   %3d] pos=%-10llu len=%-8u -> 0 (past end)\n",
            g_calls_read, (unsigned long long)pos, len);
        return 0;
    }
    avail = test_clip_mp4_len - pos;
    if (avail > len)
        avail = len;
    memcpy(buf, test_clip_mp4 + pos, (size_t)avail);
    g_bytes_read += avail;

    /* Only the first handful are logged in full: AvPlayer reads a lot, and a
     * log line per read would bury everything else. */
    if (g_calls_read <= 24)
        LOG("  [read   %3d] pos=%-10llu len=%-8u -> %llu\n", g_calls_read,
            (unsigned long long)pos, len, (unsigned long long)avail);
    else if (g_calls_read == 25)
        LOG("  [read      ] ... further reads summarised at the end\n");
    return (int)avail;
}

static uint64_t
cb_size(void *p)
{
    g_calls_size++;
    LOG("  [size      ] obj=%p -> %llu\n", p,
        (unsigned long long)test_clip_mp4_len);
    return test_clip_mp4_len;
}

/* AvPlayer is asynchronous: AddSource returns immediately and the container is
 * parsed on its own demux thread. Run 3 queried StreamCount straight after
 * AddSource, got 0 because parsing had not finished, and then deadlocked in
 * sceAvPlayerStart. So the state is latched here and waited on instead.
 *
 * Observed ids: 0x2 arrives first, then 0x1 once it stops - which matches the
 * PS4 state enum, STOP = 1 and READY = 2. */
#define AVP_STATE_STOP  1
#define AVP_STATE_READY 2

static volatile int32_t g_last_event = -1;
static volatile int     g_seen_ready;

static void
cb_event(void *p, int32_t eventId, int32_t sourceId, void *eventData)
{
    g_calls_event++;
    g_last_event = eventId;
    if (eventId == AVP_STATE_READY)
        g_seen_ready = 1;
    LOG("  [event  %3d] id=0x%x source=%d data=%p%s\n", g_calls_event, eventId,
        sourceId, eventData,
        eventId == AVP_STATE_READY ? "  (READY)"
      : eventId == AVP_STATE_STOP  ? "  (STOP)" : "");
}

/* ------------------------------------------------------------------------- */

static intptr_t
resolve(uint32_t dynh, intptr_t base, const char *name, unsigned expect_off)
{
    char     nid[12] = {0};
    intptr_t addr;

    nid_encode(name, nid);
    addr = kernel_dynlib_resolve(getpid(), dynh, nid);

    if (!addr) {
        LOG("  %-38s %s  DID NOT RESOLVE\n", name, nid);
        return 0;
    }
    LOG("  %-38s %s  +0x%-6lx %s\n", name, nid,
        (unsigned long)(addr - base),
        (unsigned long)(addr - base) == expect_off ? "ok" : "*** MOVED ***");
    return addr;
}

int
main(int argc, char **argv)
{
    int   want_play = 1;   /* the prize: does AvPlayer decode? */
    int   want_arb  = 0;   /* opt-in: it hung on run 1 */
    int   want_start = 0;  /* opt-in: it deadlocked on run 3 */
    void *handle = NULL;
    int   arb_answered = 0;

    for (int i = 1; i < argc; i++)
    {
        if (!argv[i])
            continue;
        if (strcmp(argv[i], "no-play") == 0)
            want_play = 0;
        else if (strcmp(argv[i], "arb") == 0)
            want_arb = 1;
        else if (strcmp(argv[i], "start") == 0)
            want_start = 1;
    }

    g_log = fopen(LOG_PATH, "w");

    LOG("=== EVO Player avplayer_probe - Route A ===\n");
    LOG("firmware : 0x%08x\n", kernel_get_fw_version());
    LOG("pid      : %d\n", getpid());
    LOG("clip     : %llu bytes of MP4 in .rodata\n",
        (unsigned long long)test_clip_mp4_len);
    LOG("playback : %s\n\n", want_play ? "ENABLED (argv)"
                                       : "off - pass \"play\" to enable");

    LOG("The question: sceVideoDecoderArbitrationInitialize BLOCKS when Route B\n"
        "calls it. AvPlayer calls arbitration before it decodes. If Init here\n"
        "returns at all, arbitration is reachable from this process and Route\n"
        "B's hang was our sequence, not our privileges. If it blocks in the\n"
        "same place, that is the wall.\n");

    for (size_t i = 0; i < sizeof kModules / sizeof *kModules; i++) {
        char path[256];
        int  res = 0;
        int  modid;

        snprintf(path, sizeof path, "/system/common/lib/%s", kModules[i]);
        modid = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &res);
        LOG("load %-38s modid=0x%x res=0x%x\n", kModules[i], modid, res);
    }

    uint32_t dynh = 0;
    if (kernel_dynlib_handle(getpid(), "libSceAvPlayer.sprx", &dynh) != 0) {
        LOG("\nFATAL: no dynlib handle for libSceAvPlayer.sprx\n");
        return EXIT_FAILURE;
    }

    intptr_t base = kernel_dynlib_mapbase_addr(getpid(), dynh);
    LOG("\nlibSceAvPlayer base 0x%lx\n", (unsigned long)base);

    intptr_t a_init   = resolve(dynh, base, "sceAvPlayerInit", 0xd00);
    intptr_t a_add    = resolve(dynh, base, "sceAvPlayerAddSource", 0x20e0);
    intptr_t a_active = resolve(dynh, base, "sceAvPlayerIsActive", 0x2d30);
    intptr_t a_video  = resolve(dynh, base, "sceAvPlayerGetVideoData", 0x3040);
    intptr_t a_close  = resolve(dynh, base, "sceAvPlayerClose", 0x1e50);
    intptr_t a_start  = resolve(dynh, base, "sceAvPlayerStart", 0xcd0);
    intptr_t a_scount = resolve(dynh, base, "sceAvPlayerStreamCount", 0x890);
    intptr_t a_sinfo  = resolve(dynh, base, "sceAvPlayerGetStreamInfo", 0xb30);
    intptr_t a_senab  = resolve(dynh, base, "sceAvPlayerEnableStream", 0xc80);

    if (!a_init) {
        LOG("\nFATAL: sceAvPlayerInit did not resolve\n");
        return EXIT_FAILURE;
    }

    avplayer_init_fn      av_init   = (avplayer_init_fn)a_init;
    avplayer_addsource_fn av_add    = (avplayer_addsource_fn)a_add;
    avplayer_isactive_fn  av_active = (avplayer_isactive_fn)a_active;
    avplayer_getvideo_fn  av_video  = (avplayer_getvideo_fn)a_video;
    avplayer_close_fn     av_close  = (avplayer_close_fn)a_close;

    /* -- CONTROL: NULL Init ------------------------------------------------ *
     * findings.md says NULL returns 0 rather than faulting. If that has
     * stopped being true, the struct reading below is not the problem. */
    LOG("\n--- control: sceAvPlayerInit(NULL) ---\n");
    {
        void *r = av_init(NULL);

        LOG("  -> %p  %s\n", r,
            r == NULL ? "NULL, as read - no fault"
                      : "*** returned a handle for NULL input ***");
    }

    /* -- the real Init ----------------------------------------------------- */
    LOG("\n--- sceAvPlayerInit, fully instrumented ---\n");
    LOG("    Every callback below logs its arguments. Even if Init fails, the\n"
        "    allocation pattern and file-access model are the result.\n\n");
    {
        SceAvPlayerInitData init;

        memset(&init, 0, sizeof init);

        init.memoryReplacement.objectPointer     = (void *)0xA110C;
        init.memoryReplacement.allocate          = cb_allocate;
        init.memoryReplacement.deallocate        = cb_deallocate;
        init.memoryReplacement.allocateTexture   = cb_allocate_texture;
        init.memoryReplacement.deallocateTexture = cb_deallocate_texture;

        init.fileReplacement.objectPointer = (void *)0xF11E;
        init.fileReplacement.open          = cb_open;
        init.fileReplacement.close         = cb_close;
        init.fileReplacement.readOffset    = cb_read_offset;
        init.fileReplacement.size          = cb_size;

        init.eventReplacement.objectPointer = (void *)0xEBE47;
        init.eventReplacement.eventCallback = cb_event;

        init.debugLevel                 = 4;      /* max; 1..4 [E]. Its own
                                                   * log is the instrument */
        init.basePriority               = 700;    /* clamped 637..767   [E] */
        init.numOutputVideoFrameBuffers = 2;
        /* Run 2 left this at 0 and then never called Start, so the player went
         * READY (event 2) and straight back to STOP (event 1) without ever
         * reading the mdat. 811 bytes read is a container parse, not a decode.
         * Both fixes are applied: autoStart here, and an explicit Start below,
         * which is harmless if this already did it. */
        init.autoStart                  = 1;
        init.defaultLanguage            = NULL;

        LOG("  debugLevel %d  basePriority %u  frameBuffers %d\n",
            init.debugLevel, init.basePriority,
            init.numOutputVideoFrameBuffers);
        LOG("  calling sceAvPlayerInit ... if the log stops here, Init blocked\n"
            "  the way arbitration did for Route B, and that is the answer.\n\n");

        handle = av_init(&init);

        LOG("\n  sceAvPlayerInit -> %p  %s\n", handle,
            handle ? "*** RETURNED A HANDLE ***" : "returned NULL");
    }

    /* -- AddSource and playback: the actual prize ------------------------- */
    if (handle && want_play && a_add && a_active && a_video) {
        LOG("\n--- AddSource + playback ---\n");
        {
            int rc = av_add(handle, "embedded.mp4");

            LOG("  sceAvPlayerAddSource -> 0x%08x\n", rc);

            /* Wait for the demuxer. AddSource returns before the container is
             * parsed - run 3 proved it by getting StreamCount 0 and then
             * deadlocking in Start. */
            if (rc == 0) {
                int waited = 0;

                LOG("  waiting for READY ...\n");
                while (!g_seen_ready && waited < 300) {
                    usleep(10000);
                    waited++;
                }
                LOG("  %s after %d ms (last event id 0x%x)\n",
                    g_seen_ready ? "READY" : "*** no READY event ***",
                    waited * 10, g_last_event);
            }

            /* What did the demuxer find? StreamCount and GetStreamInfo cost
             * nothing and they say whether the MP4 was understood, separately
             * from whether it plays. */
            if (rc == 0 && a_scount) {
                int n = ((avplayer_streamcount_fn)a_scount)(handle);

                LOG("  sceAvPlayerStreamCount -> %d\n", n);

                for (int s = 0; s < n && s < 8; s++) {
                    uint8_t info[0x40] = {0};

                    if (a_sinfo &&
                        ((avplayer_streaminfo_fn)a_sinfo)(handle,
                                                          (uint32_t)s,
                                                          info) == 0) {
                        LOG("    stream %d raw:", s);
                        for (size_t k = 0; k < 0x20; k++)
                            LOG(" %02x", info[k]);
                        LOG("\n");
                    }
                    if (a_senab) {
                        int er = ((avplayer_enablestream_fn)a_senab)(
                                     handle, (uint32_t)s);
                        LOG("    sceAvPlayerEnableStream(%d) -> 0x%08x\n",
                            s, er);
                    }
                }
            }

            /* autoStart is 1, so AvPlayer starts itself once ready. Run 3
             * called Start explicitly on top of that and blocked; it is opt-in
             * now rather than removed, because if autoStart turns out not to
             * work we will want it back. */
            if (rc == 0 && want_start && a_start) {
                int sr = ((avplayer_start_fn)a_start)(handle);

                LOG("  sceAvPlayerStart -> 0x%08x  %s\n", sr,
                    sr == 0 ? "playing" : "refused");
            } else if (rc == 0) {
                LOG("  sceAvPlayerStart not called - autoStart is 1, and run 3\n"
                    "  deadlocked calling it explicitly. Pass \"start\" to force.\n");
            }

            if (rc == 0) {
                for (int i = 0; i < 400; i++) {
                    uint8_t frame[0x28];
                    int     active = av_active(handle);

                    memset(frame, 0, sizeof frame);
                    if (av_video(handle, frame)) {
                        LOG("\n  *** GetVideoData RETURNED A FRAME (iter %d) ***\n",
                            i);
                        for (size_t k = 0; k < sizeof frame; k += 8)
                            LOG("    +0x%02zx  %02x %02x %02x %02x %02x %02x %02x %02x\n",
                                k, frame[k], frame[k+1], frame[k+2], frame[k+3],
                                frame[k+4], frame[k+5], frame[k+6], frame[k+7]);
                        break;
                    }
                    if (!active && i > 60) {
                        LOG("  player went inactive after %d polls\n", i);
                        break;
                    }
                    usleep(20000);
                }
            }
        }
    } else if (handle && want_play) {
        LOG("\n  playback requested but an entry point did not resolve\n");
    }

    /* -- the arbitration discriminator: LAST, and opt-in ------------------ *
     * If AvPlayer brought arbitration up, our own Initialize returns
     * 0x81570002 "already initialised" instead of blocking. A call that
     * previously hung forever returning an error code settles it.
     *
     * It runs after playback because run 1 ran it before, and that could only
     * ever hang: AvPlayer initialises perfectly well without arbitration, so
     * it must bring it up at AddSource or first decode. Ordering error, mine. */
    if (want_arb) {
        uint32_t dynh_arb = 0;

        LOG("\n--- the arbitration discriminator ---\n");

        if (kernel_dynlib_handle(getpid(),
                                 "libSceVideoDecoderArbitration.sprx",
                                 &dynh_arb) == 0) {
            intptr_t arb_base = kernel_dynlib_mapbase_addr(getpid(), dynh_arb);
            intptr_t a_arb    = resolve(dynh_arb, arb_base,
                                        "sceVideoDecoderArbitrationInitialize",
                                        0xf0);

            if (a_arb) {
                ArbParams p;
                int rc;

                memset(&p, 0, sizeof p);
                p.thisSize = 0x18;
                p.priority = 700;
                p.count    = 1;

                LOG("  calling sceVideoDecoderArbitrationInitialize.\n"
                    "  Route B hung here. If AvPlayer already brought it up we\n"
                    "  get 0x81570002; if it returns 0 it was reachable all\n"
                    "  along; if the log stops, the wall is real.\n");

                rc = ((arb_init_fn)a_arb)(&p);
                arb_answered = 1;

                LOG("  -> 0x%08x  %s\n", rc,
                    (uint32_t)rc == 0x81570002
                        ? "*** ALREADY INITIALISED - AvPlayer brought it up ***"
                    : rc == 0
                        ? "*** RETURNED 0 - reachable after all ***"
                        : "an error code, but IT RETURNED - not a hang");
            }
        } else {
            LOG("  no dynlib handle for the arbitration module\n");
        }
    } else {
        LOG("\n--- arbitration discriminator skipped ---\n"
            "    Run 1 put this BEFORE playback, which was the wrong place.\n"
            "    AvPlayer initialises fine without arbitration, so it must\n"
            "    bring it up later - at AddSource or first decode. Probing\n"
            "    before playback could only ever hang, and it did. Pass\n"
            "    \"arb\" to run it after playback, where the answer lives.\n");
    }

    /* -- teardown ---------------------------------------------------------- */
    if (handle && a_close) {
        int rc = av_close(handle);
        LOG("\n  sceAvPlayerClose -> 0x%08x\n", rc);
    }

    LOG("\n=== what the callbacks saw ===\n");
    LOG("  allocate calls    : %d   (%zu bytes plain, %zu bytes texture)\n",
        g_calls_alloc, g_total_alloc, g_total_texture);
    LOG("  deallocate calls  : %d\n", g_calls_free);
    LOG("  open / close      : %d / %d\n", g_calls_open, g_calls_close);
    LOG("  size queries      : %d\n", g_calls_size);
    LOG("  reads             : %d  (%llu bytes)\n", g_calls_read,
        (unsigned long long)g_bytes_read);
    LOG("  events            : %d\n", g_calls_event);

    {
        int leaked = 0;

        for (int i = 0; i < g_nallocs; i++)
            if (g_allocs[i].ptr) {
                leaked++;
                free(g_allocs[i].ptr);
            }
        LOG("  still-held allocations freed at exit: %d\n", leaked);
    }

    LOG("\n=== verdict ===\n");
    if (arb_answered)
        LOG("  ARBITRATION ANSWERED. It does not inherently block in this\n"
            "  process, so Route B's hang was about our call sequence rather\n"
            "  than our privileges. Re-read what AvPlayer does before\n"
            "  Initialize and mirror it.\n");
    else
        LOG("  arbitration was not reached this run.\n");

    if (!handle)
        LOG("  sceAvPlayerInit returned NULL. The callback log above still\n"
            "  shows what it wanted; a NULL return with zero allocate calls\n"
            "  means it refused before trying, which points at the init\n"
            "  struct rather than at the environment.\n");

    LOG("\ndone\n");
    if (g_log)
        fclose(g_log);

    evo_notify("EVO avplayer_probe: init %s, %d allocs, arb %s",
               handle ? "OK" : "NULL", g_calls_alloc,
               arb_answered ? "ANSWERED" : "not reached");
    return EXIT_SUCCESS;
}
