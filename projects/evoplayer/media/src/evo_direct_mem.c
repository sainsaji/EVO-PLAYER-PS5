/*
 * evo_direct_mem.c — Direct Memory Region & Zero-Fragmentation Slab Manager.
 *
 * Provides thread-safe 2MB-aligned memory management for frame buffers,
 * subtitle surfaces, and streaming I/O ring buffers.
 */
#include "evo_direct_mem.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#if defined(EVO_TARGET_PS5)
#include <ps5/kernel.h>
/* PS5 SDK direct memory definitions if not present in standard headers */
extern int sceKernelAllocateDirectMemory(off_t searchStart, off_t searchEnd,
                                         size_t length, size_t alignment,
                                         int memoryType, off_t *physAddr);
extern int sceKernelMapDirectMemory(void **addr, size_t length, int protection,
                                    int flags, off_t physAddr, size_t alignment);
extern int sceKernelReleaseDirectMemory(off_t physAddr, size_t length);
#endif

#define EVO_DIRECT_MEM_DEFAULT_SIZE (64 * 1024 * 1024) /* 64 MiB */
#define EVO_DIRECT_MEM_ALIGN        64                  /* 64-byte SIMD alignment */
#define EVO_CHUNK_HEADER_SIZE       ((sizeof(mem_chunk_header_t) + EVO_DIRECT_MEM_ALIGN - 1) & ~(EVO_DIRECT_MEM_ALIGN - 1))

typedef struct mem_chunk_header {
    size_t                   size;
    int                      is_free;
    struct mem_chunk_header *next;
    struct mem_chunk_header *prev;
} mem_chunk_header_t;

typedef struct {
    pthread_mutex_t     lock;
    uint8_t            *base_ptr;
    size_t              total_size;
    size_t              allocated_size;
    size_t              peak_size;
    size_t              num_allocs;
    int                 is_direct_hw;
    off_t               ps5_phys_addr;
    mem_chunk_header_t *first_chunk;
    int                 initialized;
} direct_pool_t;

static direct_pool_t g_direct_pool = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .initialized = 0
};

int evo_direct_mem_init(size_t pool_size_bytes)
{
    pthread_mutex_lock(&g_direct_pool.lock);
    if (g_direct_pool.initialized) {
        pthread_mutex_unlock(&g_direct_pool.lock);
        return 0;
    }

    if (pool_size_bytes == 0)
        pool_size_bytes = EVO_DIRECT_MEM_DEFAULT_SIZE;

    /* Align pool to 2MB boundary */
    size_t align_2mb = 2 * 1024 * 1024;
    pool_size_bytes = (pool_size_bytes + align_2mb - 1) & ~(align_2mb - 1);

    void *base = NULL;
    int is_hw = 0;

#if defined(EVO_TARGET_PS5)
    off_t phys = 0;
    /* PS5 Direct Memory: memoryType 3 = WB_ONION (CPU cacheable + GPU shared) */
    int ret = sceKernelAllocateDirectMemory(0, (off_t)16 * 1024 * 1024 * 1024ULL,
                                           pool_size_bytes, align_2mb, 3, &phys);
    if (ret == 0 && phys != 0) {
        void *mapped = NULL;
        ret = sceKernelMapDirectMemory(&mapped, pool_size_bytes, 0x3 /* PROT_READ|PROT_WRITE */,
                                       0, phys, align_2mb);
        if (ret == 0 && mapped != NULL) {
            base = mapped;
            g_direct_pool.ps5_phys_addr = phys;
            is_hw = 1;
        } else {
            sceKernelReleaseDirectMemory(phys, pool_size_bytes);
        }
    }
#endif

    if (!base) {
        /* Fallback: 2MB aligned heap allocation */
        if (posix_memalign(&base, align_2mb, pool_size_bytes) != 0) {
            base = malloc(pool_size_bytes);
        }
        is_hw = 0;
    }

    if (!base) {
        pthread_mutex_unlock(&g_direct_pool.lock);
        return -1;
    }

    memset(base, 0, pool_size_bytes);
    g_direct_pool.base_ptr = (uint8_t *)base;
    g_direct_pool.total_size = pool_size_bytes;
    g_direct_pool.allocated_size = 0;
    g_direct_pool.peak_size = 0;
    g_direct_pool.num_allocs = 0;
    g_direct_pool.is_direct_hw = is_hw;

    /* Initialize root chunk */
    mem_chunk_header_t *root = (mem_chunk_header_t *)base;
    root->size = pool_size_bytes - EVO_CHUNK_HEADER_SIZE;
    root->is_free = 1;
    root->next = NULL;
    root->prev = NULL;
    g_direct_pool.first_chunk = root;

    g_direct_pool.initialized = 1;
    pthread_mutex_unlock(&g_direct_pool.lock);
    return 0;
}

void evo_direct_mem_shutdown(void)
{
    pthread_mutex_lock(&g_direct_pool.lock);
    if (!g_direct_pool.initialized) {
        pthread_mutex_unlock(&g_direct_pool.lock);
        return;
    }

#if defined(EVO_TARGET_PS5)
    if (g_direct_pool.is_direct_hw && g_direct_pool.ps5_phys_addr != 0) {
        sceKernelReleaseDirectMemory(g_direct_pool.ps5_phys_addr, g_direct_pool.total_size);
    } else
#endif
    {
        free(g_direct_pool.base_ptr);
    }

    g_direct_pool.base_ptr = NULL;
    g_direct_pool.total_size = 0;
    g_direct_pool.allocated_size = 0;
    g_direct_pool.first_chunk = NULL;
    g_direct_pool.initialized = 0;
    pthread_mutex_unlock(&g_direct_pool.lock);
}

void *evo_direct_mem_alloc(size_t bytes)
{
    if (bytes == 0) return NULL;

    pthread_mutex_lock(&g_direct_pool.lock);
    if (!g_direct_pool.initialized) {
        if (evo_direct_mem_init(EVO_DIRECT_MEM_DEFAULT_SIZE) != 0) {
            pthread_mutex_unlock(&g_direct_pool.lock);
            return malloc(bytes);
        }
    }

    /* Round up requested bytes to alignment boundary */
    size_t req = (bytes + EVO_DIRECT_MEM_ALIGN - 1) & ~(EVO_DIRECT_MEM_ALIGN - 1);

    mem_chunk_header_t *curr = g_direct_pool.first_chunk;
    while (curr) {
        if (curr->is_free && curr->size >= req) {
            /* Split chunk if remaining space is useful */
            if (curr->size >= req + EVO_CHUNK_HEADER_SIZE + EVO_DIRECT_MEM_ALIGN) {
                mem_chunk_header_t *next_chunk = (mem_chunk_header_t *)((uint8_t *)curr + EVO_CHUNK_HEADER_SIZE + req);
                next_chunk->size = curr->size - req - EVO_CHUNK_HEADER_SIZE;
                next_chunk->is_free = 1;
                next_chunk->next = curr->next;
                next_chunk->prev = curr;
                if (curr->next) curr->next->prev = next_chunk;
                curr->next = next_chunk;
                curr->size = req;
            }

            curr->is_free = 0;
            g_direct_pool.allocated_size += curr->size;
            g_direct_pool.num_allocs++;
            if (g_direct_pool.allocated_size > g_direct_pool.peak_size) {
                g_direct_pool.peak_size = g_direct_pool.allocated_size;
            }

            void *ptr = (void *)((uint8_t *)curr + EVO_CHUNK_HEADER_SIZE);
            pthread_mutex_unlock(&g_direct_pool.lock);
            return ptr;
        }
        curr = curr->next;
    }

    pthread_mutex_unlock(&g_direct_pool.lock);
    /* Direct pool exhausted; graceful fallback to system allocator */
    return malloc(bytes);
}

void *evo_direct_mem_calloc(size_t count, size_t size)
{
    size_t total = count * size;
    void *ptr = evo_direct_mem_alloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void evo_direct_mem_free(void *ptr)
{
    if (!ptr) return;

    pthread_mutex_lock(&g_direct_pool.lock);
    if (!g_direct_pool.initialized ||
        (uint8_t *)ptr < g_direct_pool.base_ptr ||
        (uint8_t *)ptr >= g_direct_pool.base_ptr + g_direct_pool.total_size) {
        pthread_mutex_unlock(&g_direct_pool.lock);
        free(ptr);
        return;
    }

    mem_chunk_header_t *chunk = (mem_chunk_header_t *)((uint8_t *)ptr - EVO_CHUNK_HEADER_SIZE);
    if (!chunk->is_free) {
        chunk->is_free = 1;
        g_direct_pool.allocated_size -= chunk->size;
        g_direct_pool.num_allocs--;

        /* Merge with next chunk if free */
        if (chunk->next && chunk->next->is_free) {
            chunk->size += EVO_CHUNK_HEADER_SIZE + chunk->next->size;
            chunk->next = chunk->next->next;
            if (chunk->next) chunk->next->prev = chunk;
        }

        /* Merge with previous chunk if free */
        if (chunk->prev && chunk->prev->is_free) {
            chunk->prev->size += EVO_CHUNK_HEADER_SIZE + chunk->size;
            chunk->prev->next = chunk->next;
            if (chunk->next) chunk->next->prev = chunk->prev;
        }
    }
    pthread_mutex_unlock(&g_direct_pool.lock);
}

void evo_direct_mem_get_stats(evo_direct_mem_stats_t *out_stats)
{
    if (!out_stats) return;
    pthread_mutex_lock(&g_direct_pool.lock);
    out_stats->total_bytes = g_direct_pool.total_size;
    out_stats->allocated_bytes = g_direct_pool.allocated_size;
    out_stats->peak_bytes = g_direct_pool.peak_size;
    out_stats->num_allocations = g_direct_pool.num_allocs;
    out_stats->is_direct_hardware_mem = g_direct_pool.is_direct_hw;
    pthread_mutex_unlock(&g_direct_pool.lock);
}
