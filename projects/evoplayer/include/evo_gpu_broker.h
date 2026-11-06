/*
 * evo_gpu_broker.h - EVO as the GPU owner for the browser engine (#237).
 *
 * A payload (the ProsperoWeb WebProcess) cannot drive the GPU itself, so EVO,
 * the app, runs PS5_Vulkan's GPU broker (vendored in gpu_broker/): a client
 * process builds RADV command buffers in memory EVO shares with it, and EVO
 * submits them. See docs/ui/browser.md, "GPU broker".
 *
 * Started when the Web section opens; switched off by
 * /data/evoplayer/evo_gpu_broker_off (or /mnt/usb0/evo_gpu_broker_off).
 */
#ifndef EVO_GPU_BROKER_H
#define EVO_GPU_BROKER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(EVO_AGC_DEVICE) || !EVO_AGC_DEVICE
/* The compile-check build has no GPU and no broker objects. */
static inline void evo_gpu_broker_reserve_early(void) {}
static inline void evo_gpu_broker_release_unused(void) {}
static inline void evo_gpu_broker_preinit(void) {}
static inline int evo_gpu_broker_start(void) { return -1; }
static inline int evo_gpu_broker_state(void) { return -1; }
static inline int evo_gpu_broker_open(int *cookie) { if (cookie) *cookie = -1; return -1; }
static inline void evo_gpu_broker_register(int cookie, int pid) { (void)cookie; (void)pid; }
static inline uint64_t evo_gpu_broker_frames(void) { return 0; }
static inline uint64_t evo_gpu_broker_last_frame(void *out, size_t cap, unsigned *w, unsigned *h,
                                                 unsigned *pitch, int *bgra)
{ (void)out; (void)cap; (void)w; (void)h; (void)pitch; (void)bgra; return 0; }
#else
/* At boot, before EVO maps anything big: hold the two address ranges broker
 * clients map shared memory at (0x240000000 + 512 MB, 0x3000000000 + 16 GB),
 * so EVO's own mappings stay out of them. No-op without the trigger file. */
void evo_gpu_broker_reserve_early(void);

/* Once the sandbox is open: gives the ranges back if the broker is switched off. */
void evo_gpu_broker_release_unused(void);

/* Before evo_jailbreak_self(): RADV's platform init (sceAgcInit), which like
 * EVO's other GPU-side inits may not work after the credential swap. */
void evo_gpu_broker_preinit(void);

/* Starts the broker on a worker thread (its warm-up submit can block).
 * 0 = started or already starting. */
int evo_gpu_broker_start(void);

/* 1 running, 0 not (yet), -1 failed. */
int evo_gpu_broker_state(void);

/* A client endpoint (fd to hand to the client process), or -1. *cookie is the
 * broker's id for it. */
int evo_gpu_broker_open(int *cookie);

/* The client process behind cookie: once this pid is gone, EVO closes the
 * client's slot and the broker frees its memory. Called again for the same
 * cookie, it moves the slot to a new pid (engine host -> its WebProcess). */
void evo_gpu_broker_register(int cookie, int pid);

/* Frames the broker's display received from clients (swapchain presents). */
uint64_t evo_gpu_broker_frames(void);

/* Copies the newest presented frame (BGRA when *bgra, rows of pitch bytes)
 * into out if it fits; returns its frame number, 0 if none. */
uint64_t evo_gpu_broker_last_frame(void *out, size_t cap, unsigned *w, unsigned *h, unsigned *pitch, int *bgra);
#endif

#ifdef __cplusplus
}
#endif

#endif
