/* Copyright (C) 2026 Mihawk-99 */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PS5_GPU_COMPLETION_H
#define PS5_GPU_COMPLETION_H
#include <stdint.h>
static inline void ps5_gpu_completion_packet(uint32_t words[8], uint64_t marker, uint32_t value)
{
    words[0] = 0xc0064900;
    words[1] = 0x0030c514;
    words[2] = 0x20000000;
    words[3] = (uint32_t)marker;
    words[4] = (uint32_t)(marker >> 32);
    words[5] = value;
    words[6] = words[7] = 0;
}

#endif
