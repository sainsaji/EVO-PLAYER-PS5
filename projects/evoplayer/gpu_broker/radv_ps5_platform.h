/*
 * Copyright © 2026 Mihawk
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef RADV_PS5_PLATFORM_H
#define RADV_PS5_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What the PS5 winsys needs from the console: direct memory the CPU and the GPU
 * share at one address, submission of PM4 words, and the CPU cache control the
 * GPU's view of that memory requires. On the console these are the exported
 * kernel and AGC functions (ps5platform/kernel.h, ps5platform/agc.h); a host
 * build models them, so the driver runs on a PC for everything that does not
 * need the GPU to execute (device creation, pipeline compilation, API tests).
 */

/* The high word the shaders' 32-bit pointers carry. Everything RADV reaches
 * through one -- descriptor sets, push constants, vertex-buffer descriptors,
 * shader code -- is a RADEON_FLAG_32BIT buffer in this word's 4 GiB window
 * (PS5_Vulkan R86-R88: resources the GPU is handed a full address for may lie
 * anywhere below 2^47). */
#define RADV_PS5_ADDRESS32_HI 2u
#define RADV_PS5_WINDOW_BASE ((uint64_t)RADV_PS5_ADDRESS32_HI << 32)
#define RADV_PS5_WINDOW_BYTES (UINT64_C(1) << 32)

/* The GPU reaches full addresses below this, and so does the user half of the
 * CPU's address space. */
#define RADV_PS5_GPU_ADDRESS_LIMIT (UINT64_C(1) << 47)

/* Direct memory is granted in 16 KiB pages; allocations of at least 2 MiB get
 * 2 MiB alignment, which is also the granule of the device-memory region. */
#define RADV_PS5_PAGE_BYTES UINT64_C(0x4000)
#define RADV_PS5_LARGE_BYTES UINT64_C(0x200000)

struct radv_ps5_memory {
   /* The CPU mapping, which is also the GPU address. */
   uint8_t *cpu;
   uint64_t bytes;
   /* The direct-memory allocation behind it; -1 on the host. */
   int64_t physical;
   /* The device-memory region granules the mapping took, if any, or the
    * window's replay range's (window_replay). */
   uint32_t granule;
   uint32_t granules;
   bool window_replay;
};

/* Initialises AGC once per process. It has to run from the title's own
 * executable, which the driver archive is linked into. */
bool radv_ps5_platform_init(void);

/* Whether a replayable window buffer can be placed at its captured address
 * again, whatever else the process mapped since (radv_ps5_memory_alloc_replayable). */
bool radv_ps5_window_replayable(void);

/* Whether submitted words run on a GPU. The host model completes a submission
 * without running it. */
bool radv_ps5_platform_runs_gpu(void);

bool radv_ps5_memory_alloc(uint64_t bytes, uint64_t alignment, bool window32,
                           struct radv_ps5_memory *out);
void radv_ps5_memory_free(struct radv_ps5_memory *memory);
/* Memory for capture and replay. A capture outside the window goes at the
 * top of the device-memory region, away from everything else; a replay goes
 * at replay_va exactly or not at all (in the window, where the kernel places
 * buffers, only if nothing took the address since). */
bool radv_ps5_memory_alloc_replayable(uint64_t bytes, uint64_t alignment, bool window32, uint64_t replay_va,
                                      struct radv_ps5_memory *out);

/* Host memory the application allocated, given GPU read and write access
 * where the CPU keeps its own: the GPU then reaches it at the CPU's address
 * (VK_EXT_external_memory_host). Whole pages. */
bool radv_ps5_memory_grant_gpu(void *address, uint64_t bytes);

/* Sparse resources: a range reserved in the device-memory region, or in the
 * window for one the shaders reach through a 32-bit pointer (a descriptor
 * buffer), unbound (the shared zero block) until pieces of a buffer's memory
 * are bound into it. Addresses, sizes and offsets are whole pages. */
/* Placed for capture and replay as radv_ps5_memory_alloc_replayable places
 * buffers when replayable or replay_va is set. */
bool radv_ps5_vrange_reserve(uint64_t bytes, bool window32, bool replayable, uint64_t replay_va,
                             struct radv_ps5_memory *out);
void radv_ps5_vrange_release(struct radv_ps5_memory *range);
bool radv_ps5_vrange_bind(void *at, uint64_t bytes, const struct radv_ps5_memory *memory, uint64_t offset);
bool radv_ps5_vrange_unbind(void *at, uint64_t bytes);
uint64_t radv_ps5_vrange_space_bytes(void);

/* The direct-memory pool, which the CPU draws on too, and what it has left. */
uint64_t radv_ps5_memory_pool_bytes(void);
uint64_t radv_ps5_memory_available_bytes(void);

/* Writes back and invalidates the CPU cache lines of a range, and orders the
 * evictions before what follows. */
void radv_ps5_cpu_flush(const void *address, size_t bytes);

/* Submits count PM4 words at words (GPU-visible memory whose CPU lines have
 * been flushed). The words end with a packet that writes marker_value to
 * marker once they have run; the host model writes it at once. */
int radv_ps5_submit(uint32_t *words, uint32_t count, volatile uint32_t *marker, uint32_t marker_value);

/* The nanoseconds radv_ps5_submit spent since the last call, in the cache
 * flush, in sceAgcDriverSubmitDcb and in sceAgcSuspendPoint (called with the
 * submit lock held, as radv_ps5_submit is). */
void radv_ps5_submit_times(uint64_t times[3]);
/* The tessellation factor ring the GPU uses from the next submission on (AGC
 * owns its registers); 0 or the system software's error. */
int radv_ps5_set_tess_factor_ring(uint64_t va, uint32_t size);

/* Maps memory's direct memory a second time for the CPU, at address, replacing
 * whatever the range held (a placed mapping). */
bool radv_ps5_memory_map_at(const struct radv_ps5_memory *memory, void *address);
/* Undoes it, leaving the range reserved if reserve is set. */
void radv_ps5_memory_unmap_at(void *address, uint64_t bytes, bool reserve);
/* VGT_HS_OFFCHIP_PARAM's fields for the next submissions (AGC owns it too);
 * 0 or the system software's error. */
int radv_ps5_set_hs_offchip_param(uint32_t granularity, uint32_t buffering);

uint64_t radv_ps5_now_ns(void);
void radv_ps5_sleep_us(unsigned microseconds);

#endif /* RADV_PS5_PLATFORM_H */
