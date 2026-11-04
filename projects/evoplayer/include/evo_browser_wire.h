/*
 * The frame channel between EVO and a browser engine host (#237,
 * docs/ui/browser.md). AF_UNIX SOCK_SEQPACKET, one message per packet,
 * little-endian, plain C structs. Shared by EVO and the payloads that speak
 * it (helpers/frame_test.c now; the ProsperoWeb --evo-host mode later).
 */
#ifndef EVO_BROWSER_WIRE_H
#define EVO_BROWSER_WIRE_H

#include <stdint.h>

#define EVOB_MAGIC      0x424f5645u   /* "EVOB" */
#define EVOB_VERSION    1
#define EVOB_MAX_RECTS  16
#define EVOB_MAX_BUFS   2

enum evob_type {
    EVOB_HELLO   = 1,   /* host -> EVO; the buffer fds ride along (SCM_RIGHTS) */
    EVOB_FRAME   = 2,   /* host -> EVO: a buffer is complete */
    EVOB_RELEASE = 3,   /* EVO -> host: EVO has copied it, paint into it again */
    EVOB_BYE     = 4,   /* either way: orderly end */
};

enum evob_format {
    EVOB_RGBA8_PREMUL = 1,  /* bytes R, G, B, A in memory; EVO's texture order */
};

struct evob_hdr {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t len;       /* payload bytes after this header */
};

struct evob_hello {
    uint32_t width, height, stride, format, nbuf;
};

struct evob_rect {
    uint16_t x, y, w, h;
};

struct evob_frame {
    uint32_t buf, seq, nrects;
    struct evob_rect rects[EVOB_MAX_RECTS];
};

struct evob_release {
    uint32_t buf;
};

#endif
