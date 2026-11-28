/*
 * shim_ring_bench.c - host model of where the demux packet rings land.
 *
 * Builds the real tools/native-app/stubs/malloc_shim.c with its allocator
 * entry points renamed (so the host libc keeps its own malloc) and the
 * sceKernel* memory calls mocked as two budgets: direct memory (one
 * contiguous run, sized like the console's direct_largest_free) and flexible
 * memory. It then replays the ring allocations sizeReadAhead() makes for a
 * 120 s network buffer and prints what each pool lost.
 *
 *   before: rings from malloc() (the shim's direct-first, then flexible)
 *   after:  rings from evo_alloc_direct_only(), audio ring sized by bitrate
 *
 * The 1392 MB case is the evo.log boot of 2026-11-07 (direct_total=3072MB):
 * flex_avail fell 775 -> 389 MB when playback started.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

/* ---- mocked kernel ------------------------------------------------------ */
static size_t g_direct_free, g_flex_free;
static long   g_next_phys = 0x100000;

int sceKernelAllocateDirectMemory(long s, long e, size_t len, size_t align,
                                  int type, long *phys)
{
    (void)s; (void)e; (void)align; (void)type;
    if (len > g_direct_free) return -1;
    g_direct_free -= len;
    *phys = g_next_phys;
    g_next_phys += (long)len;
    return 0;
}
int sceKernelReleaseDirectMemory(long phys, size_t len) { (void)phys; g_direct_free += len; return 0; }
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags,
                             long phys, size_t align)
{
    (void)prot; (void)flags; (void)phys; (void)align;
    void *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return -1;
    *addr = p;
    return 0;
}
/* Flexible mappings remembered so an unmap can credit the budget. */
static struct { void *p; size_t len; } g_flex[4096];
int sceKernelMapNamedFlexibleMemory(void **addr, size_t len, int prot, int flags, const char *name)
{
    (void)prot; (void)flags; (void)name;
    if (len > g_flex_free) return -1;
    void *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return -1;
    for (int i = 0; i < 4096; i++)
        if (!g_flex[i].p) { g_flex[i].p = p; g_flex[i].len = len; break; }
    g_flex_free -= len;
    *addr = p;
    return 0;
}
int sceKernelMunmap(void *addr, size_t len)
{
    for (int i = 0; i < 4096; i++)
        if (g_flex[i].p == addr) { g_flex_free += g_flex[i].len; g_flex[i].p = 0; break; }
    return munmap(addr, len);
}
int sceKernelAvailableFlexibleMemorySize(size_t *out) { *out = g_flex_free; return 0; }

/* ---- the real shim, renamed ------------------------------------------- */
#define malloc              shim_malloc
#define free                shim_free
#define calloc              shim_calloc
#define realloc             shim_realloc
#define reallocarray        shim_reallocarray
#define posix_memalign      shim_posix_memalign
#define aligned_alloc       shim_aligned_alloc
#define memalign            shim_memalign
#define valloc              shim_valloc
#define malloc_usable_size  shim_malloc_usable_size
#include "../../tools/native-app/stubs/malloc_shim.c"
#undef malloc
#undef free

/* ---- the ring sizing, as PlaybackController.cpp does it ---------------- */
#define MB ((size_t)1 << 20)

static void *ring(void *(*alloc)(size_t), size_t bytes, size_t *got)
{
    void *p;
    for (;;) {                                  /* packet_queue_use_ring() */
        p = alloc(bytes);
        if (p || bytes < 64 * MB) break;
        bytes /= 2;
    }
    *got = p ? bytes : 0;
    return p;
}

static size_t audio_ring_after(long long abps, double secs, size_t full)
{                                               /* audioRingBytesFor() */
    if (abps <= 0) return full;
    double want = secs * (double)abps / 8.0 * 2.0;
    size_t b = want > (double)full ? full : (size_t)want;
    if (b < 16 * MB) b = 16 * MB;
    return b > full ? full : b;
}

static void run(const char *label, size_t direct_mb, long long abps, int after)
{
    g_direct_free = direct_mb * MB;
    g_flex_free   = 775 * MB;                   /* flex_avail at decode-open */
    const size_t flex0 = g_flex_free, dir0 = g_direct_free;
    const double secs = 120.0, scale = 4.0;     /* 120 s buffer: video x4 (cap 6), audio x4 */
    size_t vgot = 0, agot = 0;
    void *(*alloc)(size_t) = after ? evo_alloc_direct_only : shim_malloc;
    void *v = ring(alloc, (size_t)(512 * MB * scale), &vgot);
    size_t afull = (size_t)(96 * MB * scale);
    void *a = ring(alloc, after ? audio_ring_after(abps, secs, afull) : afull, &agot);
    printf("%-34s %-6s video_ring=%5zu MB audio_ring=%4zu MB  flex_lost=%4zu MB direct_lost=%5zu MB map_fail=%llu\n",
           label, after ? "after" : "before", vgot / MB, agot / MB,
           (flex0 - g_flex_free) / MB, (dir0 - g_direct_free) / MB,
           (unsigned long long)atomic_load(&g_map_fail));
    if (v) shim_free(v);
    if (a) shim_free(a);
    atomic_store(&g_map_fail, 0);
}

/* The cost of one large-block cycle (map, touch every page, unmap) on the
 * host, against a slab-sized one. evo.log's `large=` counter climbs ~20 per
 * second in steady playback, so this tells whether a block cache would pay. */
static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static void churn(size_t bytes, int n)
{
    g_flex_free = (size_t)1 << 40;
    g_direct_free = (size_t)1 << 40;
    double t0 = now_us();
    for (int i = 0; i < n; i++) {
        unsigned char *p = shim_malloc(bytes);
        for (size_t o = 0; o < bytes; o += 4096) p[o] = (unsigned char)i;
        shim_free(p);
    }
    printf("churn %7zu B: %.2f us per malloc+touch+free\n", bytes, (now_us() - t0) / n);
}

int main(void)
{
    churn(8192, 20000);        /* slab */
    churn(65536, 20000);       /* large, flexible */
    churn(262144, 5000);
    churn(2u << 20, 2000);     /* large, direct */
    /* AAC 2.0 at 256 kbit/s: the 2026-11-07 Jellyfin test file. */
    run("1392 MB direct free, AAC 256k", 1392, 256000, 0);
    run("1392 MB direct free, AAC 256k", 1392, 256000, 1);
    run("11288 MB direct free (Pro), AAC", 11288, 256000, 0);
    run("11288 MB direct free (Pro), AAC", 11288, 256000, 1);
    /* E-AC-3 7.1 at 1.5 Mbit/s */
    run("1392 MB direct free, E-AC-3 1.5M", 1392, 1536000, 0);
    run("1392 MB direct free, E-AC-3 1.5M", 1392, 1536000, 1);
    /* TrueHD in Matroska: no stated bitrate, full ring either way */
    run("1392 MB direct free, TrueHD (?)", 1392, 0, 0);
    run("1392 MB direct free, TrueHD (?)", 1392, 0, 1);
    /* Very little direct: the ring must give up rather than eat flexible */
    run("200 MB direct free, AAC 256k", 200, 256000, 0);
    run("200 MB direct free, AAC 256k", 200, 256000, 1);
    return 0;
}
