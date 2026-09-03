/*
 * evo_direct_mem.h — Direct Memory Region & Zero-Fragmentation Slab Manager.
 *
 * Implements high-performance, 2MB-aligned direct memory management for PS5
 * video frame buffers, subtitle texture surfaces, and streaming I/O buffers.
 */
#ifndef EVO_DIRECT_MEM_H
#define EVO_DIRECT_MEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Default pool size (#6). Sized for the 4K CPU video working set:
 *   pp_videoout linear staging   3 x 3840*2160*4  = ~100 MiB
 *   pp_playback display + back    (1080/V3 path)   =  ~16..66 MiB
 *   swscale rotate ring (fallback) 3 x frame       =  ~25..100 MiB
 * The common 4K path (V8 GPU present) only needs the staging buffers; the
 * rotate ring and display buffers are the 1080/V3/exotic-pixfmt paths and
 * spill gracefully to malloc() if they don't fit. evo_direct_mem_init steps
 * the request down (128/96/64 MiB) if the console won't give the full block.
 */
#define EVO_DIRECT_MEM_POOL_BYTES ((size_t)192 * 1024 * 1024)

typedef struct {
    size_t total_bytes;
    size_t allocated_bytes;
    size_t peak_bytes;
    size_t num_allocations;
    int    is_direct_hardware_mem;
} evo_direct_mem_stats_t;

/**
 * Initialize direct memory region manager.
 * Pre-allocates a 2MB-aligned direct memory pool (default 64 MiB).
 * On PS5: uses sceKernelAllocateDirectMemory + sceKernelMapDirectMemory (WB_ONION).
 * On host: uses 2MB-aligned virtual memory pool (posix_memalign / mmap).
 */
int evo_direct_mem_init(size_t pool_size_bytes);

/**
 * Shutdown direct memory pool and free backing direct memory.
 */
void evo_direct_mem_shutdown(void);

/**
 * Allocate a buffer from the direct memory region.
 * Guaranteed 64-byte alignment for fast SIMD/GPU/DMA access.
 */
void *evo_direct_mem_alloc(size_t bytes);

/**
 * Allocate zero-initialized memory from the direct memory region.
 */
void *evo_direct_mem_calloc(size_t count, size_t size);

/**
 * Free buffer back to direct memory region.
 */
void evo_direct_mem_free(void *ptr);

/**
 * Query current direct memory statistics.
 */
void evo_direct_mem_get_stats(evo_direct_mem_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* EVO_DIRECT_MEM_H */
