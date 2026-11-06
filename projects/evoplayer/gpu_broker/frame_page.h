/* Copyright (C) 2026 Mihawk-99 */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_FRAME_PAGE_H
#define PW_FRAME_PAGE_H
#include <stdint.h>

/* A client publishes composed BGRA windows in this shared page; the owner
 * shows the latest frame of the foreground client. sequence is odd while the
 * pixels are being written. A reader keeps its copy only if sequence stays
 * unchanged and even. x/y place the picture on the desktop for cursor scaling.
 * Legacy PW names and layout are retained for existing clients. */
enum { PW_FRAME_MAGIC = 0x50574652u, PW_FRAME_WIDTH = 1920, PW_FRAME_HEIGHT = 1080, PW_FRAME_HEADER = 16384,
       PW_FRAME_BYTES = 8323072 /* the header and 1920x1080 BGRA pixels, in whole 16 KiB pages */ };
typedef struct PwFramePage {
    uint32_t magic, sequence;
    uint32_t width, height, stride;
    int32_t x, y;
    uint32_t frames;
    uint64_t time_ns;
} PwFramePage;
static inline uint8_t *pw_frame_pixels(PwFramePage *page) { return (uint8_t *)page + PW_FRAME_HEADER; }
#ifdef __cplusplus
static_assert(PW_FRAME_HEADER + PW_FRAME_WIDTH * PW_FRAME_HEIGHT * 4 <= PW_FRAME_BYTES && !(PW_FRAME_BYTES % 16384),
              "the frame page holds a whole 1080p picture in 16 KiB pages");
#endif
#endif
