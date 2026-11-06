/*
 * Copyright © 2026 Mihawk
 *
 * SPDX-License-Identifier: MIT
 */

#include "radv_ps5_platform.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "c11/threads.h"
#include "util/macros.h"
#include "util/os_time.h"
#include "util/simple_mtx.h"
#include "util/u_atomic.h"
#include "util/u_math.h"

#if defined(__PROSPERO__)
#include <ps5platform/agc.h>
#include <ps5platform/kernel.h>
#else
#include <sys/mman.h>
#endif

/* A first-fit allocator over a range of fixed-size granules, one bit each. The
 * console places buffers the GPU reaches through full addresses in a region it
 * maps at the address asked for (PS5_Vulkan R88); the host model places the
 * address window's buffers the same way. */
struct radv_ps5_granules {
   simple_mtx_t lock;
   uint64_t base;
   uint64_t granule_bytes;
   uint32_t count;
   uint64_t *used;
};

static bool
radv_ps5_granules_init(struct radv_ps5_granules *g, uint64_t base, uint64_t bytes, uint64_t granule_bytes)
{
   simple_mtx_init(&g->lock, mtx_plain);
   g->base = base;
   g->granule_bytes = granule_bytes;
   g->count = (uint32_t)(bytes / granule_bytes);
   g->used = calloc(DIV_ROUND_UP(g->count, 64), sizeof(uint64_t));
   return g->used != NULL;
}

static void
radv_ps5_granules_mark(struct radv_ps5_granules *g, uint32_t first, uint32_t count, bool used)
{
   for (uint32_t i = first; i < first + count; i++) {
      const uint64_t bit = UINT64_C(1) << (i % 64);
      if (used)
         g->used[i / 64] |= bit;
      else
         g->used[i / 64] &= ~bit;
   }
}

/* count free granules in a row, aligned to align_granules; UINT32_MAX if none. */
static uint32_t
radv_ps5_granules_take(struct radv_ps5_granules *g, uint32_t count, uint32_t align_granules)
{
   if (count == 0 || count > g->count)
      return UINT32_MAX;
   align_granules = MAX2(align_granules, 1);
   simple_mtx_lock(&g->lock);
   uint32_t start = 0;
   while (start + count <= g->count) {
      uint32_t run = 0;
      while (run < count && !(g->used[(start + run) / 64] & (UINT64_C(1) << ((start + run) % 64))))
         run++;
      if (run == count) {
         radv_ps5_granules_mark(g, start, count, true);
         simple_mtx_unlock(&g->lock);
         return start;
      }
      start = align(start + run + 1, align_granules);
   }
   simple_mtx_unlock(&g->lock);
   return UINT32_MAX;
}

static bool
radv_ps5_granules_free(const struct radv_ps5_granules *g, uint32_t first, uint32_t count)
{
   for (uint32_t i = first; i < first + count; i++) {
      if (g->used[i / 64] & (UINT64_C(1) << (i % 64)))
         return false;
   }
   return true;
}

/* The same from the top down: where captured buffers go, away from everything
 * else, so their addresses are still free when a replay asks for them. */
static uint32_t
radv_ps5_granules_take_top(struct radv_ps5_granules *g, uint32_t count, uint32_t align_granules)
{
   if (count == 0 || count > g->count)
      return UINT32_MAX;
   align_granules = MAX2(align_granules, 1);
   simple_mtx_lock(&g->lock);
   for (uint32_t start = (g->count - count) / align_granules * align_granules;;
        start -= align_granules) {
      if (radv_ps5_granules_free(g, start, count)) {
         radv_ps5_granules_mark(g, start, count, true);
         simple_mtx_unlock(&g->lock);
         return start;
      }
      if (start < align_granules)
         break;
   }
   simple_mtx_unlock(&g->lock);
   return UINT32_MAX;
}

/* Exactly the granules from first on, if they are all free. */
static bool
radv_ps5_granules_take_at(struct radv_ps5_granules *g, uint32_t first, uint32_t count)
{
   if (count == 0 || first >= g->count || count > g->count - first)
      return false;
   simple_mtx_lock(&g->lock);
   const bool free = radv_ps5_granules_free(g, first, count);
   if (free)
      radv_ps5_granules_mark(g, first, count, true);
   simple_mtx_unlock(&g->lock);
   return free;
}

static void
radv_ps5_granules_give(struct radv_ps5_granules *g, uint32_t first, uint32_t count)
{
   if (count == 0)
      return;
   simple_mtx_lock(&g->lock);
   radv_ps5_granules_mark(g, first, count, false);
   simple_mtx_unlock(&g->lock);
}

/* The device-memory region: 256 GiB at 0x4000000000, handed out in 2 MiB
 * granules. The GPU reads and writes the whole direct-memory pool there
 * (PS5_Vulkan R86-R88). */
#define RADV_PS5_REGION_BASE UINT64_C(0x4000000000)
#define RADV_PS5_REGION_BYTES (UINT64_C(256) << 30)

static struct radv_ps5_granules radv_ps5_region;
static once_flag radv_ps5_once = ONCE_FLAG_INIT;
static bool radv_ps5_ready;

uint64_t
radv_ps5_now_ns(void)
{
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

void
radv_ps5_sleep_us(unsigned microseconds)
{
   const struct timespec delay = {
      .tv_sec = microseconds / 1000000u,
      .tv_nsec = (long)(microseconds % 1000000u) * 1000l,
   };
   nanosleep(&delay, NULL);
}

void
radv_ps5_cpu_flush(const void *address, size_t bytes)
{
   if (bytes == 0)
      return;
   const uintptr_t line = 64;
   uintptr_t at = (uintptr_t)address & ~(line - 1);
   const uintptr_t end = (uintptr_t)address + bytes;
   /* CLFLUSHOPT (Zen 2 has it): the lines are written back and dropped as
    * CLFLUSH does, but not one after another; the closing MFENCE orders them
    * before every later store and load (a submission, or a read of what the
    * GPU wrote). A submission's words took one serialising CLFLUSH a line. */
   __builtin_ia32_mfence();
   for (; at < end; at += line)
      __asm__ volatile("clflushopt %0" : "+m"(*(volatile char *)at));
   __builtin_ia32_mfence();
}

static bool
radv_ps5_window_contains(uint64_t address, uint64_t bytes)
{
   return address >= RADV_PS5_WINDOW_BASE && bytes <= RADV_PS5_WINDOW_BYTES &&
          address - RADV_PS5_WINDOW_BASE <= RADV_PS5_WINDOW_BYTES - bytes;
}

#if defined(__PROSPERO__)

/* ------------------------------------------------------------------ console */

/* GPU-visible direct memory as ps5vk and the test runner allocate it: type 12,
 * mapped for CPU and GPU read and write. */
#define RADV_PS5_DIRECT_TYPE 12
#define RADV_PS5_PROTECTION                                                                        \
   (PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | PS5_KERNEL_PROT_GPU_READ |              \
    PS5_KERNEL_PROT_GPU_WRITE)

static int32_t radv_ps5_agc_result = -1;

/* The top of the window, kept for the replayable window buffers (a ray tracing
 * pipeline's shader arenas when group handles are captured): the kernel
 * places every other window mapping, so an address captured among them may be
 * taken when it is replayed. Here the port places them, captures from the top
 * down and replays at their captured addresses; the range stays reserved
 * where nothing is mapped. */
#define RADV_PS5_WINDOW_REPLAY_BYTES (UINT64_C(256) << 20)
#define RADV_PS5_WINDOW_REPLAY_BASE  (RADV_PS5_WINDOW_BASE + RADV_PS5_WINDOW_BYTES - RADV_PS5_WINDOW_REPLAY_BYTES)
static struct radv_ps5_granules radv_ps5_window_replay;
static bool radv_ps5_window_replay_ready;

static void
radv_ps5_window_replay_init(void)
{
   void *at = (void *)(uintptr_t)RADV_PS5_WINDOW_REPLAY_BASE;
   if (sceKernelReserveVirtualRange(&at, RADV_PS5_WINDOW_REPLAY_BYTES, 0, RADV_PS5_LARGE_BYTES) != 0)
      return;
   if (at != (void *)(uintptr_t)RADV_PS5_WINDOW_REPLAY_BASE ||
       !radv_ps5_granules_init(&radv_ps5_window_replay, RADV_PS5_WINDOW_REPLAY_BASE, RADV_PS5_WINDOW_REPLAY_BYTES,
                               RADV_PS5_LARGE_BYTES)) {
      sceKernelMunmap(at, RADV_PS5_WINDOW_REPLAY_BYTES);
      return;
   }
   radv_ps5_window_replay_ready = true;
}

bool
radv_ps5_window_replayable(void)
{
   return radv_ps5_window_replay_ready;
}

static void
radv_ps5_platform_once(void)
{
   /* RADV's shader cache in the title's own folder, as ps5vk kept its compiled
    * shaders there (ps5vk-shader-cache), in the database form, which needs no
    * file mapping. A title may choose otherwise, as on Linux. */
   setenv("MESA_SHADER_CACHE_DIR", "/app0/radv-shader-cache", 0);
   setenv("MESA_DISK_CACHE_DATABASE", "1", 0);

   radv_ps5_agc_result = sceAgcInit(PS5_AGC_INIT_VERSION);
   if (radv_ps5_agc_result != 0)
      fprintf(stderr, "radv/ps5: sceAgcInit(%u) failed: 0x%08x\n", PS5_AGC_INIT_VERSION,
              (unsigned)radv_ps5_agc_result);
   radv_ps5_ready = radv_ps5_agc_result == 0 &&
                    radv_ps5_granules_init(&radv_ps5_region, RADV_PS5_REGION_BASE, RADV_PS5_REGION_BYTES,
                                           RADV_PS5_LARGE_BYTES);
   if (radv_ps5_ready)
      radv_ps5_window_replay_init();
}

bool
radv_ps5_platform_runs_gpu(void)
{
   return true;
}

uint64_t
radv_ps5_memory_pool_bytes(void)
{
   const int64_t bytes = sceKernelGetDirectMemorySize();
   return bytes > 0 ? (uint64_t)bytes : 0;
}

uint64_t
radv_ps5_memory_available_bytes(void)
{
   const int64_t pool = sceKernelGetDirectMemorySize();
   int64_t start = -1;
   size_t available = 0;
   if (pool <= 0 || sceKernelAvailableDirectMemorySize(0, pool, RADV_PS5_PAGE_BYTES, &start, &available) != 0)
      return 0;
   return available;
}

bool
radv_ps5_memory_alloc(uint64_t bytes, uint64_t alignment, bool window32, struct radv_ps5_memory *out)
{
   *out = (struct radv_ps5_memory){.physical = -1};
   bytes = align64(MAX2(bytes, 1), RADV_PS5_PAGE_BYTES);
   alignment = MAX2(alignment, PS5_KERNEL_DIRECT_ALIGNMENT);
   if (bytes >= RADV_PS5_LARGE_BYTES)
      alignment = MAX2(alignment, RADV_PS5_LARGE_BYTES);
   alignment = util_next_power_of_two64(alignment);

   int64_t physical = -1;
   if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes, alignment,
                                     RADV_PS5_DIRECT_TYPE, &physical) != 0)
      return false;

   /* A window buffer goes where the kernel puts a mapping it is given no
    * address for, which is the window (PS5_Vulkan R86); anything else goes in
    * the device-memory region, at the address asked for. */
   void *hint = NULL;
   uint32_t granule = 0, granules = 0;
   if (!window32) {
      granules = (uint32_t)DIV_ROUND_UP(bytes, RADV_PS5_LARGE_BYTES);
      granule = radv_ps5_granules_take(&radv_ps5_region, granules,
                                       (uint32_t)MAX2(alignment / RADV_PS5_LARGE_BYTES, 1));
      if (granule == UINT32_MAX)
         granules = 0;
      else
         hint = (void *)(uintptr_t)(RADV_PS5_REGION_BASE + (uint64_t)granule * RADV_PS5_LARGE_BYTES);
   }

   void *address = hint;
   int32_t result = sceKernelMapDirectMemory(&address, bytes, RADV_PS5_PROTECTION, 0, physical, alignment);
   if (result != 0 && hint != NULL) {
      radv_ps5_granules_give(&radv_ps5_region, granule, granules);
      granules = 0;
      address = NULL;
      result = sceKernelMapDirectMemory(&address, bytes, RADV_PS5_PROTECTION, 0, physical, alignment);
   }
   if (result == 0 && address != hint) {
      radv_ps5_granules_give(&radv_ps5_region, granule, granules);
      granules = 0;
   }

   const uint64_t va = (uint64_t)(uintptr_t)address;
   const bool placed = result == 0 && address != NULL &&
                       (window32 ? radv_ps5_window_contains(va, bytes)
                                 : va < RADV_PS5_GPU_ADDRESS_LIMIT && bytes <= RADV_PS5_GPU_ADDRESS_LIMIT - va);
   if (!placed) {
      if (result == 0 && address != NULL)
         sceKernelMunmap(address, bytes);
      radv_ps5_granules_give(&radv_ps5_region, granule, granules);
      sceKernelReleaseDirectMemory(physical, bytes);
      return false;
   }

   *out = (struct radv_ps5_memory){
      .cpu = address,
      .bytes = bytes,
      .physical = physical,
      .granule = granule,
      .granules = granules,
   };
   return true;
}

static bool radv_ps5_map_fixed(uint8_t *at, uint64_t bytes, int64_t physical);

/* A replayable window buffer, in the window's replay range: captured from its
 * top, replayed at the captured address. */
static bool
radv_ps5_memory_alloc_window_replay(uint64_t bytes, uint64_t alignment, uint64_t replay_va,
                                    struct radv_ps5_memory *out)
{
   if (!radv_ps5_window_replay_ready)
      return false;
   alignment = util_next_power_of_two64(MAX2(alignment, RADV_PS5_LARGE_BYTES));
   const uint32_t granules = (uint32_t)DIV_ROUND_UP(bytes, RADV_PS5_LARGE_BYTES);
   uint32_t granule;
   if (replay_va) {
      if (replay_va < RADV_PS5_WINDOW_REPLAY_BASE || (replay_va - RADV_PS5_WINDOW_BASE) % alignment ||
          !radv_ps5_window_contains(replay_va, (uint64_t)granules * RADV_PS5_LARGE_BYTES))
         return false;
      granule = (uint32_t)((replay_va - RADV_PS5_WINDOW_REPLAY_BASE) / RADV_PS5_LARGE_BYTES);
      if (!radv_ps5_granules_take_at(&radv_ps5_window_replay, granule, granules))
         return false;
   } else {
      granule = radv_ps5_granules_take_top(&radv_ps5_window_replay, granules,
                                           (uint32_t)(alignment / RADV_PS5_LARGE_BYTES));
      if (granule == UINT32_MAX)
         return false;
   }

   const uint64_t span = (uint64_t)granules * RADV_PS5_LARGE_BYTES;
   uint8_t *const at = (uint8_t *)(uintptr_t)(RADV_PS5_WINDOW_REPLAY_BASE + (uint64_t)granule * RADV_PS5_LARGE_BYTES);
   int64_t physical = -1;
   if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), span, RADV_PS5_LARGE_BYTES,
                                     RADV_PS5_DIRECT_TYPE, &physical) != 0) {
      radv_ps5_granules_give(&radv_ps5_window_replay, granule, granules);
      return false;
   }
   if (!radv_ps5_map_fixed(at, span, physical)) {
      sceKernelReleaseDirectMemory(physical, span);
      radv_ps5_granules_give(&radv_ps5_window_replay, granule, granules);
      return false;
   }
   *out = (struct radv_ps5_memory){
      .cpu = at,
      .bytes = span,
      .physical = physical,
      .granule = granule,
      .granules = granules,
      .window_replay = true,
   };
   return true;
}

/* A window buffer is replayed where the kernel agrees to put it: at the
 * captured address given as a hint, if nothing took it since. */
static UNUSED bool
radv_ps5_memory_replay_window(uint64_t bytes, uint64_t alignment, uint64_t replay_va, struct radv_ps5_memory *out)
{
   alignment = util_next_power_of_two64(MAX2(alignment, PS5_KERNEL_DIRECT_ALIGNMENT));
   if (replay_va % alignment || !radv_ps5_window_contains(replay_va, bytes))
      return false;
   int64_t physical = -1;
   if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes, alignment, RADV_PS5_DIRECT_TYPE,
                                     &physical) != 0)
      return false;
   void *const hint = (void *)(uintptr_t)replay_va;
   void *address = hint;
   const int32_t result = sceKernelMapDirectMemory(&address, bytes, RADV_PS5_PROTECTION, 0, physical, alignment);
   if (result != 0 || address != hint) {
      if (result == 0 && address != NULL)
         sceKernelMunmap(address, bytes);
      sceKernelReleaseDirectMemory(physical, bytes);
      return false;
   }
   *out = (struct radv_ps5_memory){.cpu = address, .bytes = bytes, .physical = physical};
   return true;
}

bool
radv_ps5_memory_alloc_replayable(uint64_t bytes, uint64_t alignment, bool window32, uint64_t replay_va,
                                 struct radv_ps5_memory *out)
{
   *out = (struct radv_ps5_memory){.physical = -1};
   bytes = align64(MAX2(bytes, 1), RADV_PS5_PAGE_BYTES);
   if (window32)
      return radv_ps5_memory_alloc_window_replay(bytes, alignment, replay_va, out);
   alignment = util_next_power_of_two64(MAX2(alignment, RADV_PS5_LARGE_BYTES));
   const uint32_t granules = (uint32_t)DIV_ROUND_UP(bytes, RADV_PS5_LARGE_BYTES);
   const uint32_t align_granules = (uint32_t)(alignment / RADV_PS5_LARGE_BYTES);

   uint32_t granule;
   if (replay_va) {
      if (replay_va < RADV_PS5_REGION_BASE || (replay_va - RADV_PS5_REGION_BASE) % alignment ||
          (replay_va - RADV_PS5_REGION_BASE) / RADV_PS5_LARGE_BYTES > UINT32_MAX)
         return false;
      granule = (uint32_t)((replay_va - RADV_PS5_REGION_BASE) / RADV_PS5_LARGE_BYTES);
      if (!radv_ps5_granules_take_at(&radv_ps5_region, granule, granules))
         return false;
   } else {
      granule = radv_ps5_granules_take_top(&radv_ps5_region, granules, align_granules);
      if (granule == UINT32_MAX)
         return false;
   }

   int64_t physical = -1;
   if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes, alignment, RADV_PS5_DIRECT_TYPE,
                                     &physical) != 0) {
      radv_ps5_granules_give(&radv_ps5_region, granule, granules);
      return false;
   }
   /* The address is a hint the kernel takes only where nothing is mapped:
    * anything else it chose is given back. */
   void *const hint = (void *)(uintptr_t)(RADV_PS5_REGION_BASE + (uint64_t)granule * RADV_PS5_LARGE_BYTES);
   void *address = hint;
   const int32_t result = sceKernelMapDirectMemory(&address, bytes, RADV_PS5_PROTECTION, 0, physical, alignment);
   if (result != 0 || address != hint) {
      if (result == 0 && address != NULL)
         sceKernelMunmap(address, bytes);
      radv_ps5_granules_give(&radv_ps5_region, granule, granules);
      sceKernelReleaseDirectMemory(physical, bytes);
      return false;
   }

   *out = (struct radv_ps5_memory){
      .cpu = address,
      .bytes = bytes,
      .physical = physical,
      .granule = granule,
      .granules = granules,
   };
   return true;
}

/* ------------------------------------------------------------ sparse ranges */

/* A sparse resource is a range of the device-memory region. A bound piece is
 * a buffer's direct memory mapped there at a fixed address; an unbound piece
 * is the shared zero block, so an access to it neither faults nor reaches any
 * buffer. What such a read returns is whatever was written there before,
 * which Vulkan allows without residencyNonResidentStrict. */
static simple_mtx_t radv_ps5_zero_lock = SIMPLE_MTX_INITIALIZER;
static int64_t radv_ps5_zero_physical = -1;

static int64_t
radv_ps5_zero_block(void)
{
   simple_mtx_lock(&radv_ps5_zero_lock);
   if (radv_ps5_zero_physical < 0) {
      int64_t physical = -1;
      if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), RADV_PS5_LARGE_BYTES,
                                        RADV_PS5_LARGE_BYTES, RADV_PS5_DIRECT_TYPE, &physical) == 0) {
         void *cpu = NULL;
         if (sceKernelMapDirectMemory(&cpu, RADV_PS5_LARGE_BYTES, RADV_PS5_PROTECTION, 0, physical,
                                      RADV_PS5_LARGE_BYTES) == 0) {
            memset(cpu, 0, RADV_PS5_LARGE_BYTES);
            radv_ps5_cpu_flush(cpu, RADV_PS5_LARGE_BYTES);
            sceKernelMunmap(cpu, RADV_PS5_LARGE_BYTES);
            radv_ps5_zero_physical = physical;
         } else {
            sceKernelReleaseDirectMemory(physical, RADV_PS5_LARGE_BYTES);
         }
      }
   }
   const int64_t zero = radv_ps5_zero_physical;
   simple_mtx_unlock(&radv_ps5_zero_lock);
   return zero;
}

static bool
radv_ps5_map_fixed(uint8_t *at, uint64_t bytes, int64_t physical)
{
   void *address = at;
   return sceKernelMapDirectMemory(&address, bytes, RADV_PS5_PROTECTION, PS5_KERNEL_MAP_FIXED, physical,
                                   RADV_PS5_PAGE_BYTES) == 0 &&
          address == at;
}

bool
radv_ps5_vrange_unbind(void *at, uint64_t bytes)
{
   const int64_t zero = radv_ps5_zero_block();
   if (zero < 0 || (uintptr_t)at % RADV_PS5_PAGE_BYTES || bytes % RADV_PS5_PAGE_BYTES)
      return false;
   for (uint64_t done = 0; done < bytes;) {
      const uint64_t piece = MIN2(bytes - done, RADV_PS5_LARGE_BYTES);
      if (!radv_ps5_map_fixed((uint8_t *)at + done, piece, zero))
         return false;
      done += piece;
   }
   return true;
}

bool
radv_ps5_vrange_bind(void *at, uint64_t bytes, const struct radv_ps5_memory *memory, uint64_t offset)
{
   if (memory->physical < 0 || offset > memory->bytes || bytes > memory->bytes - offset ||
       (uintptr_t)at % RADV_PS5_PAGE_BYTES || offset % RADV_PS5_PAGE_BYTES || bytes % RADV_PS5_PAGE_BYTES)
      return false;
   return radv_ps5_map_fixed(at, bytes, memory->physical + (int64_t)offset);
}

/* A range in the window: a reservation where the kernel puts one it is given
 * no address for, or at the captured address if nothing took it since, as
 * window buffers are placed (radv_ps5_memory_alloc). */
static bool
radv_ps5_vrange_reserve_window(uint64_t bytes, uint64_t replay_va, struct radv_ps5_memory *out)
{
   const uint64_t span = align64(MAX2(bytes, 1), RADV_PS5_LARGE_BYTES);
   if (replay_va && (replay_va % RADV_PS5_LARGE_BYTES || !radv_ps5_window_contains(replay_va, span)))
      return false;
   void *at = (void *)(uintptr_t)replay_va;
   if (sceKernelReserveVirtualRange(&at, span, 0, RADV_PS5_LARGE_BYTES) != 0)
      return false;
   if ((replay_va && at != (void *)(uintptr_t)replay_va) || !radv_ps5_window_contains((uint64_t)(uintptr_t)at, span) ||
       !radv_ps5_vrange_unbind(at, span)) {
      sceKernelMunmap(at, span);
      return false;
   }
   *out = (struct radv_ps5_memory){.cpu = at, .bytes = span, .physical = -1};
   return true;
}

bool
radv_ps5_vrange_reserve(uint64_t bytes, bool window32, bool replayable, uint64_t replay_va,
                        struct radv_ps5_memory *out)
{
   *out = (struct radv_ps5_memory){.physical = -1};
   if (window32)
      return radv_ps5_vrange_reserve_window(bytes, replay_va, out);
   const uint32_t granules = (uint32_t)DIV_ROUND_UP(MAX2(bytes, 1), RADV_PS5_LARGE_BYTES);
   /* Placed as radv_ps5_memory_alloc_replayable places buffers. */
   uint32_t granule;
   if (replay_va) {
      if (replay_va < RADV_PS5_REGION_BASE || (replay_va - RADV_PS5_REGION_BASE) % RADV_PS5_LARGE_BYTES ||
          (replay_va - RADV_PS5_REGION_BASE) / RADV_PS5_LARGE_BYTES > UINT32_MAX)
         return false;
      granule = (uint32_t)((replay_va - RADV_PS5_REGION_BASE) / RADV_PS5_LARGE_BYTES);
      if (!radv_ps5_granules_take_at(&radv_ps5_region, granule, granules))
         return false;
   } else {
      granule = replayable ? radv_ps5_granules_take_top(&radv_ps5_region, granules, 1)
                           : radv_ps5_granules_take(&radv_ps5_region, granules, 1);
      if (granule == UINT32_MAX)
         return false;
   }
   uint8_t *const va = (uint8_t *)(uintptr_t)(RADV_PS5_REGION_BASE + (uint64_t)granule * RADV_PS5_LARGE_BYTES);
   const uint64_t span = (uint64_t)granules * RADV_PS5_LARGE_BYTES;
   if (!radv_ps5_vrange_unbind(va, span)) {
      sceKernelMunmap(va, span);
      radv_ps5_granules_give(&radv_ps5_region, granule, granules);
      return false;
   }
   *out = (struct radv_ps5_memory){.cpu = va, .bytes = span, .physical = -1, .granule = granule, .granules = granules};
   return true;
}

void
radv_ps5_vrange_release(struct radv_ps5_memory *range)
{
   if (!range->cpu)
      return;
   sceKernelMunmap(range->cpu, range->bytes);
   radv_ps5_granules_give(&radv_ps5_region, range->granule, range->granules);
   *range = (struct radv_ps5_memory){.physical = -1};
}

uint64_t
radv_ps5_vrange_space_bytes(void)
{
   return RADV_PS5_REGION_BYTES;
}

void
radv_ps5_memory_free(struct radv_ps5_memory *memory)
{
   if (!memory->cpu)
      return;
   if (memory->window_replay) {
      /* Back to a reservation, for the next capture or replay. */
      void *at = memory->cpu;
      if (sceKernelReserveVirtualRange(&at, memory->bytes, PS5_KERNEL_MAP_FIXED, RADV_PS5_PAGE_BYTES) != 0 ||
          at != memory->cpu)
         fprintf(stderr, "radv/ps5: the window's replay range at %p could not be reserved again\n",
                 (void *)memory->cpu);
      radv_ps5_granules_give(&radv_ps5_window_replay, memory->granule, memory->granules);
      sceKernelReleaseDirectMemory(memory->physical, memory->bytes);
      *memory = (struct radv_ps5_memory){.physical = -1};
      return;
   }
   const int32_t unmapped = sceKernelMunmap(memory->cpu, memory->bytes);
   if (unmapped != 0)
      fprintf(stderr, "radv/ps5: sceKernelMunmap(%p, %" PRIu64 ") failed: 0x%08x\n", (void *)memory->cpu,
              memory->bytes, (unsigned)unmapped);
   radv_ps5_granules_give(&radv_ps5_region, memory->granule, memory->granules);
   if (memory->physical >= 0)
      sceKernelReleaseDirectMemory(memory->physical, memory->bytes);
   *memory = (struct radv_ps5_memory){.physical = -1};
}

/* Where radv_ps5_submit's time went (radv_ps5_submit_times); [2] is the kick
 * thread's. */
static uint64_t radv_ps5_submit_ns[3];

/* Without a suspend point after it the console starts a submission up to a
 * refresh late (PS5_Vulkan R68), and the suspend point took about 0.3 ms a
 * submission of the submitting thread (RPCS3's renderer: 2,000 of them in
 * 10 s). A thread of its own makes it instead, as soon as a submission is
 * made; submissions made while it is at one share the next. */
static mtx_t radv_ps5_kick_lock;
static cnd_t radv_ps5_kick_wanted;
static bool radv_ps5_kick_pending;
static once_flag radv_ps5_kick_once = ONCE_FLAG_INIT;

static int
radv_ps5_kick_body(void *unused)
{
   (void)unused;
   mtx_lock(&radv_ps5_kick_lock);
   for (;;) {
      while (!radv_ps5_kick_pending)
         cnd_wait(&radv_ps5_kick_wanted, &radv_ps5_kick_lock);
      radv_ps5_kick_pending = false;
      mtx_unlock(&radv_ps5_kick_lock);
      const uint64_t started = os_time_get_nano();
      const int32_t result = sceAgcSuspendPoint();
      p_atomic_add(&radv_ps5_submit_ns[2], os_time_get_nano() - started);
      if (result != 0)
         fprintf(stderr, "radv/ps5: sceAgcSuspendPoint failed: 0x%08x\n", (unsigned)result);
      mtx_lock(&radv_ps5_kick_lock);
   }
   return 0;
}

static void
radv_ps5_kick_start(void)
{
   mtx_init(&radv_ps5_kick_lock, mtx_plain);
   cnd_init(&radv_ps5_kick_wanted);
   thrd_t thread;
   if (thrd_create(&thread, radv_ps5_kick_body, NULL) == thrd_success)
      thrd_detach(thread);
}

int
radv_ps5_submit(uint32_t *words, uint32_t count, volatile uint32_t *marker, uint32_t marker_value)
{
   (void)marker;
   (void)marker_value;
   const uint64_t started = os_time_get_nano();
   radv_ps5_cpu_flush(words, (size_t)count * sizeof(uint32_t));
   const uint64_t flushed = os_time_get_nano();
   struct ps5_agc_submit_description description = {
      .words = words,
      .word_count = count,
   };
   int32_t result = sceAgcDriverSubmitDcb(&description);
   const uint64_t submitted = os_time_get_nano();
   radv_ps5_submit_ns[0] += flushed - started;
   radv_ps5_submit_ns[1] += submitted - flushed;
   if (result != 0)
      return result;
   call_once(&radv_ps5_kick_once, radv_ps5_kick_start);
   mtx_lock(&radv_ps5_kick_lock);
   radv_ps5_kick_pending = true;
   cnd_signal(&radv_ps5_kick_wanted);
   mtx_unlock(&radv_ps5_kick_lock);
   return 0;
}

void
radv_ps5_submit_times(uint64_t times[3])
{
   for (unsigned i = 0; i < 3; i++)
      times[i] = p_atomic_xchg(&radv_ps5_submit_ns[i], 0);
}

int
radv_ps5_set_tess_factor_ring(uint64_t va, uint32_t size)
{
   return sceAgcDriverSetTFRing((uintptr_t)va, size);
}

int
radv_ps5_set_hs_offchip_param(uint32_t granularity, uint32_t buffering)
{
   return sceAgcDriverSetHsOffchipParam(granularity, buffering);
}

bool
radv_ps5_memory_map_at(const struct radv_ps5_memory *memory, void *address)
{
   if (memory->physical < 0)
      return false;
   void *at = address;
   const int32_t result = sceKernelMapDirectMemory(&at, memory->bytes, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE,
                                                   PS5_KERNEL_MAP_FIXED, memory->physical, PS5_KERNEL_PAGE_SIZE);
   if (result == 0 && at != address) {
      sceKernelMunmap(at, memory->bytes);
      return false;
   }
   return result == 0;
}

bool
radv_ps5_memory_grant_gpu(void *address, uint64_t bytes)
{
   /* A title's anonymous memory takes GPU access this way, and a shader then
    * writes it through the CPU's address (HARDWARE_FINDINGS.md, 2026-09-27). */
   return sceKernelMprotect(address, bytes,
                            PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | PS5_KERNEL_PROT_GPU_READ |
                               PS5_KERNEL_PROT_GPU_WRITE) == 0;
}

void
radv_ps5_memory_unmap_at(void *address, uint64_t bytes, bool reserve)
{
   if (reserve) {
      /* A reservation laid over the mapping replaces it, as a fixed mapping does. */
      void *at = address;
      if (sceKernelReserveVirtualRange(&at, bytes, PS5_KERNEL_MAP_FIXED, PS5_KERNEL_PAGE_SIZE) == 0 && at == address)
         return;
      fprintf(stderr, "radv/ps5: a placed mapping at %p could not be left reserved\n", address);
   }
   sceKernelMunmap(address, bytes);
}

#else

/* --------------------------------------------------------------- host model */

/* The window is reserved at the console's address, so 32-bit pointers carry
 * the same high word on both; buffers outside it are ordinary mappings. */
static struct radv_ps5_granules radv_ps5_window;
#define RADV_PS5_WINDOW_GRANULE UINT64_C(0x10000)

static void
radv_ps5_platform_once(void)
{
   void *const window = mmap((void *)(uintptr_t)RADV_PS5_WINDOW_BASE, RADV_PS5_WINDOW_BYTES, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
   if (window != (void *)(uintptr_t)RADV_PS5_WINDOW_BASE) {
      fprintf(stderr, "radv/ps5: the host model could not reserve the address window: %s\n", strerror(errno));
      if (window != MAP_FAILED)
         munmap(window, RADV_PS5_WINDOW_BYTES);
      return;
   }
   radv_ps5_ready = radv_ps5_granules_init(&radv_ps5_window, RADV_PS5_WINDOW_BASE, RADV_PS5_WINDOW_BYTES,
                                           RADV_PS5_WINDOW_GRANULE);
}

/* The Vulkan loader may unload the driver once its instances are gone and load
 * it again (the CTS does): the window goes with the library, or the next load
 * finds it taken. */
static void __attribute__((destructor))
radv_ps5_platform_unload(void)
{
   if (!radv_ps5_ready)
      return;
   munmap((void *)(uintptr_t)RADV_PS5_WINDOW_BASE, RADV_PS5_WINDOW_BYTES);
   free(radv_ps5_window.used);
   simple_mtx_destroy(&radv_ps5_window.lock);
}

bool
radv_ps5_platform_runs_gpu(void)
{
   return false;
}

uint64_t
radv_ps5_memory_pool_bytes(void)
{
   /* The console's pool is about 12 GiB; the model reports the same. */
   return UINT64_C(12) << 30;
}

uint64_t
radv_ps5_memory_available_bytes(void)
{
   return radv_ps5_memory_pool_bytes();
}

bool
radv_ps5_memory_alloc(uint64_t bytes, uint64_t alignment, bool window32, struct radv_ps5_memory *out)
{
   *out = (struct radv_ps5_memory){.physical = -1};
   bytes = align64(MAX2(bytes, 1), RADV_PS5_PAGE_BYTES);
   if (window32) {
      const uint32_t count = (uint32_t)DIV_ROUND_UP(bytes, RADV_PS5_WINDOW_GRANULE);
      const uint32_t first = radv_ps5_granules_take(
         &radv_ps5_window, count, (uint32_t)MAX2(util_next_power_of_two64(alignment) / RADV_PS5_WINDOW_GRANULE, 1));
      if (first == UINT32_MAX)
         return false;
      uint8_t *const cpu = (uint8_t *)(uintptr_t)(RADV_PS5_WINDOW_BASE + (uint64_t)first * RADV_PS5_WINDOW_GRANULE);
      const uint64_t span = (uint64_t)count * RADV_PS5_WINDOW_GRANULE;
      if (mprotect(cpu, span, PROT_READ | PROT_WRITE) != 0) {
         radv_ps5_granules_give(&radv_ps5_window, first, count);
         return false;
      }
      *out = (struct radv_ps5_memory){.cpu = cpu, .bytes = span, .physical = -1, .granule = first, .granules = count};
      return true;
   }
   alignment = util_next_power_of_two64(MAX2(alignment, RADV_PS5_PAGE_BYTES));
   /* Over-allocate so the start can be aligned, then trim. */
   const uint64_t span = bytes + alignment;
   uint8_t *const raw = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
   if (raw == MAP_FAILED)
      return false;
   uint8_t *const cpu = (uint8_t *)align64((uint64_t)(uintptr_t)raw, alignment);
   if (cpu > raw)
      munmap(raw, cpu - raw);
   const uint8_t *const end = raw + span;
   if (cpu + bytes < end)
      munmap(cpu + bytes, end - (cpu + bytes));
   *out = (struct radv_ps5_memory){.cpu = cpu, .bytes = bytes, .physical = -1};
   return true;
}

bool
radv_ps5_memory_alloc_replayable(uint64_t bytes, uint64_t alignment, bool window32, uint64_t replay_va,
                                 struct radv_ps5_memory *out)
{
   if (!replay_va)
      return radv_ps5_memory_alloc(bytes, alignment, window32, out);
   *out = (struct radv_ps5_memory){.physical = -1};
   bytes = align64(MAX2(bytes, 1), RADV_PS5_PAGE_BYTES);
   if (window32) {
      const uint32_t count = (uint32_t)DIV_ROUND_UP(bytes, RADV_PS5_WINDOW_GRANULE);
      if (!radv_ps5_window_contains(replay_va, bytes) || (replay_va - RADV_PS5_WINDOW_BASE) % RADV_PS5_WINDOW_GRANULE)
         return false;
      const uint32_t first = (uint32_t)((replay_va - RADV_PS5_WINDOW_BASE) / RADV_PS5_WINDOW_GRANULE);
      if (!radv_ps5_granules_take_at(&radv_ps5_window, first, count))
         return false;
      uint8_t *const cpu = (uint8_t *)(uintptr_t)replay_va;
      const uint64_t span = (uint64_t)count * RADV_PS5_WINDOW_GRANULE;
      if (mprotect(cpu, span, PROT_READ | PROT_WRITE) != 0) {
         radv_ps5_granules_give(&radv_ps5_window, first, count);
         return false;
      }
      *out = (struct radv_ps5_memory){.cpu = cpu, .bytes = span, .physical = -1, .granule = first, .granules = count};
      return true;
   }
   if (replay_va % RADV_PS5_PAGE_BYTES)
      return false;
   void *const cpu = mmap((void *)(uintptr_t)replay_va, bytes, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
   if (cpu == MAP_FAILED)
      return false;
   if (cpu != (void *)(uintptr_t)replay_va) {
      munmap(cpu, bytes);
      return false;
   }
   *out = (struct radv_ps5_memory){.cpu = cpu, .bytes = bytes, .physical = -1};
   return true;
}

/* The model places every window buffer itself (radv_ps5_window). */
bool
radv_ps5_window_replayable(void)
{
   return true;
}

bool
radv_ps5_memory_grant_gpu(void *address, uint64_t bytes)
{
   /* The model runs no GPU work: the memory stays the application's. */
   (void)address;
   (void)bytes;
   return true;
}

/* Sparse ranges in the model: plain memory, which binds leave alone (the
 * model runs no GPU work to read through them). */
bool
radv_ps5_vrange_reserve(uint64_t bytes, bool window32, bool replayable, uint64_t replay_va,
                        struct radv_ps5_memory *out)
{
   const uint64_t span = align64(MAX2(bytes, 1), RADV_PS5_LARGE_BYTES);
   return replayable ? radv_ps5_memory_alloc_replayable(span, RADV_PS5_LARGE_BYTES, window32, replay_va, out)
                     : radv_ps5_memory_alloc(span, RADV_PS5_LARGE_BYTES, window32, out);
}

void
radv_ps5_vrange_release(struct radv_ps5_memory *range)
{
   radv_ps5_memory_free(range);
}

bool
radv_ps5_vrange_bind(void *at, uint64_t bytes, const struct radv_ps5_memory *memory, uint64_t offset)
{
   (void)at;
   return offset <= memory->bytes && bytes <= memory->bytes - offset;
}

bool
radv_ps5_vrange_unbind(void *at, uint64_t bytes)
{
   (void)at;
   (void)bytes;
   return true;
}

uint64_t
radv_ps5_vrange_space_bytes(void)
{
   return UINT64_C(256) << 30;
}

void
radv_ps5_memory_free(struct radv_ps5_memory *memory)
{
   if (!memory->cpu)
      return;
   if (radv_ps5_window_contains((uint64_t)(uintptr_t)memory->cpu, memory->bytes)) {
      /* Back to reserved, and zero when it is handed out again. */
      mmap(memory->cpu, memory->bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
      radv_ps5_granules_give(&radv_ps5_window, memory->granule, memory->granules);
   } else {
      munmap(memory->cpu, memory->bytes);
   }
   *memory = (struct radv_ps5_memory){.physical = -1};
}

int
radv_ps5_submit(uint32_t *words, uint32_t count, volatile uint32_t *marker, uint32_t marker_value)
{
   (void)words;
   (void)count;
   /* Nothing runs the words on a PC; the submission completes at once. */
   if (marker)
      *marker = marker_value;
   return 0;
}

void
radv_ps5_submit_times(uint64_t times[3])
{
   times[0] = times[1] = times[2] = 0;
}

int
radv_ps5_set_tess_factor_ring(uint64_t va, uint32_t size)
{
   (void)va;
   (void)size;
   return 0;
}

int
radv_ps5_set_hs_offchip_param(uint32_t granularity, uint32_t buffering)
{
   (void)granularity;
   (void)buffering;
   return 0;
}

bool
radv_ps5_memory_map_at(const struct radv_ps5_memory *memory, void *address)
{
   /* The host model's memory is anonymous: nothing maps it twice. */
   (void)memory;
   (void)address;
   return false;
}

void
radv_ps5_memory_unmap_at(void *address, uint64_t bytes, bool reserve)
{
   (void)address;
   (void)bytes;
   (void)reserve;
}

#endif

bool
radv_ps5_platform_init(void)
{
   call_once(&radv_ps5_once, radv_ps5_platform_once);
   return radv_ps5_ready;
}
