/*
 * EVO Player - Phase 1b task 4: malloc interposer for the app module.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The clean-room libc.prx installs its own bounded heap mspace (via
 * _sceKernelRtldSetApplicationHeapAPI). Once EVO's cumulative allocations
 * (FFmpeg contexts, RmlUi, fonts, ...) fill it, plain malloc() returns NULL
 * even with hundreds of MB of flexible memory free - verified on hardware
 * 2026-09-02: malloc(8 MB) fails in pp_videoout_init while mmap(8 MB) succeeds.
 *
 * This replaces the process allocator entirely with an mmap-backed one:
 *   - requests >= 16 KiB : one anonymous mmap each (page-rounded, munmap'd)
 *   - smaller requests   : size-classed slabs carved from 2 MiB anonymous
 *                          mmap arenas, with per-class free lists
 * Every block carries a 32-byte header so free()/realloc() route correctly
 * and user pointers are 32-byte aligned (enough for AVX / SIMD / the C++
 * runtime, which asks for 32).
 *
 * Compiled INTO eboot.bin, earliest in link order so these definitions win.
 */

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off);
extern int   munmap(void *addr, size_t len);

#define PROT_RW        0x03
#define MAP_ANON_PRIV  0x1002          /* MAP_PRIVATE | MAP_ANONYMOUS (FreeBSD) */
#define PAGE           0x4000ULL       /* 16 KiB */
#define ARENA          (2ULL << 20)    /* 2 MiB slab arena */
#define SMALL_MAX      (16 * 1024)     /* >= this gets its own mmap */
#define HDR            32
#define MAGIC_LARGE    UINT64_C(0x4F56454752414C45)   /* large block */
#define MAGIC_SLAB     UINT64_C(0x4F56534C41425F5F)   /* slab block  */

typedef struct {
    uint64_t magic;
    uint64_t info;     /* large: total mmap len. slab: class index. */
    uint64_t pad[2];
} hdr_t;

_Static_assert(sizeof(hdr_t) == HDR, "header must be 32 bytes");

/* size classes, all multiples of 32 */
static const uint32_t kClassSize[] = {
    32, 64, 96, 128, 192, 256, 384, 512, 768, 1024,
    1536, 2048, 3072, 4096, 6144, 8192, 12288
};
#define NCLASS ((int)(sizeof(kClassSize) / sizeof(kClassSize[0])))

typedef struct slot { struct slot *next; } slot_t;

static struct {
    slot_t     *free_list[NCLASS];
    uint8_t    *cur;
    uint8_t    *end;
    atomic_flag lock;
} g = { .lock = ATOMIC_FLAG_INIT };

static void lock(void)   { while (atomic_flag_test_and_set_explicit(&g.lock, memory_order_acquire)) { } }
static void unlock(void) { atomic_flag_clear_explicit(&g.lock, memory_order_release); }

#define SLAB_MAX (kClassSize[NCLASS - 1])   /* 12288 - above this -> large path */

static int class_for(size_t n)
{
    for (int i = 0; i < NCLASS; i++)
        if (n <= kClassSize[i])
            return i;
    return NCLASS - 1;   /* unreachable: malloc() gates on SLAB_MAX first */
}

/* caller holds g.lock */
static void *arena_carve(size_t need)
{
    if ((size_t)(g.end - g.cur) < need) {
        uint8_t *a = (uint8_t *)mmap(0, ARENA, PROT_RW, MAP_ANON_PRIV, -1, 0);
        if (a == (uint8_t *)-1 || a == 0)
            return 0;
        g.cur = a;                    /* mmap is page-aligned -> 32-aligned */
        g.end = a + ARENA;
    }
    void *p = g.cur;
    g.cur += need;
    return p;
}

/* Large blocks reserve a whole leading PAGE for bookkeeping, so the user
 * pointer is PAGE-aligned (covers every posix_memalign request up to PAGE).
 * The tag lives at user-HDR, same as slabs, so free()/usable() find it the
 * same way regardless of path. */
static void *large_alloc(size_t user)
{
    size_t total = (PAGE + user + (PAGE - 1)) & ~(PAGE - 1);
    uint8_t *base = (uint8_t *)mmap(0, total, PROT_RW, MAP_ANON_PRIV, -1, 0);
    if (base == (uint8_t *)-1 || base == 0)
        return 0;
    hdr_t *tag = (hdr_t *)(base + PAGE - HDR);
    tag->magic = MAGIC_LARGE;
    tag->info  = total;
    return base + PAGE;
}

static void *small_alloc(size_t user)
{
    int c = class_for(user);
    size_t slot = HDR + kClassSize[c];
    lock();
    hdr_t *h = (hdr_t *)g.free_list[c];
    if (h)
        g.free_list[c] = ((slot_t *)h)->next;
    else
        h = (hdr_t *)arena_carve(slot);
    unlock();
    if (!h)
        return 0;
    h->magic = MAGIC_SLAB;
    h->info  = (uint64_t)c;
    return (uint8_t *)h + HDR;
}

void *malloc(size_t n)
{
    if (n == 0)
        n = 1;
    return (n > SLAB_MAX) ? large_alloc(n) : small_alloc(n);
}

void free(void *p)
{
    if (!p)
        return;
    hdr_t *h = (hdr_t *)((uint8_t *)p - HDR);
    if (h->magic == MAGIC_LARGE) {
        munmap((uint8_t *)p - PAGE, (size_t)h->info);
    } else if (h->magic == MAGIC_SLAB) {
        int c = (int)h->info;
        lock();
        ((slot_t *)h)->next = g.free_list[c];
        g.free_list[c] = (slot_t *)h;
        unlock();
    }
    /* unknown magic: a platform-allocator block from before this interposer
     * was linked in - leak rather than corrupt. */
}

static size_t usable(void *p)
{
    hdr_t *h = (hdr_t *)((uint8_t *)p - HDR);
    if (h->magic == MAGIC_LARGE)
        return (size_t)h->info - PAGE;
    if (h->magic == MAGIC_SLAB)
        return kClassSize[(int)h->info];
    return 0;
}

size_t malloc_usable_size(void *p) { return p ? usable(p) : 0; }

void *calloc(size_t nmemb, size_t size)
{
    size_t n = nmemb * size;
    if (size && n / size != nmemb)
        return 0;
    void *p = malloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p)
        return malloc(n);
    if (n == 0) {
        free(p);
        return 0;
    }
    size_t old = usable(p);
    if (old && old >= n)
        return p;
    void *q = malloc(n);
    if (q && old) {
        memcpy(q, p, old < n ? old : n);
        free(p);
    } else if (q && !old) {
        /* pre-interposer block: cannot know its size; copy a conservative
         * page and accept the leak. Extremely rare in practice. */
        memcpy(q, p, n < PAGE ? n : (size_t)PAGE);
    }
    return q;
}

void *reallocarray(void *p, size_t nmemb, size_t size)
{
    size_t n = nmemb * size;
    if (size && n / size != nmemb)
        return 0;
    return realloc(p, n);
}

int posix_memalign(void **out, size_t align, size_t size)
{
    if (align <= HDR) {
        void *p = malloc(size);
        if (!p)
            return 12;                 /* ENOMEM */
        *out = p;
        return 0;
    }
    if (align > PAGE || (align & (align - 1))) {
        *out = 0;
        return 22;                     /* EINVAL */
    }
    /* Force the large path: its user pointer sits one PAGE-rounded header in,
     * i.e. PAGE-aligned, which covers every align in (HDR, PAGE]. */
    void *p = large_alloc(size);
    if (!p)
        return 12;
    *out = p;
    return 0;
}

void *aligned_alloc(size_t align, size_t size)
{
    void *p = 0;
    return posix_memalign(&p, align, size) == 0 ? p : 0;
}

void *memalign(size_t align, size_t size)
{
    void *p = 0;
    return posix_memalign(&p, align, size) == 0 ? p : 0;
}

void *valloc(size_t size)
{
    void *p = 0;
    return posix_memalign(&p, PAGE, size) == 0 ? p : 0;
}
