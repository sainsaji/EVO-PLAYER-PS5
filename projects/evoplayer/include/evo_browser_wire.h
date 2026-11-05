/*
 * The channel between EVO and a browser engine host (#237,
 * docs/ui/browser.md). AF_UNIX SOCK_SEQPACKET, one message per packet,
 * little-endian, plain C structs. Shared by EVO and the payloads that speak
 * it (helpers/frame_test.c, and the ProsperoWeb --evo-host mode, which keeps
 * an identical copy in src/frontend/ps5/evo_browser_wire.h of our fork).
 *
 * A message is a struct evob_hdr, then `len` payload bytes. Strings travel
 * as raw UTF-8 bytes at the end of the payload, without a terminating NUL.
 * A packet is at most EVOB_MAX_PACKET bytes (the kernel's SEQPACKET limit
 * is 8 KB by default); a longer string is cut by the sender and logged.
 */
#ifndef EVO_BROWSER_WIRE_H
#define EVO_BROWSER_WIRE_H

#include <stdint.h>

#define EVOB_MAGIC      0x424f5645u   /* "EVOB" */
#define EVOB_VERSION    1
#define EVOB_MAX_RECTS  16
#define EVOB_MAX_BUFS   2
#define EVOB_MAX_PACKET 8192

enum evob_type {
    /* frames (S2) */
    EVOB_HELLO   = 1,   /* host -> EVO; the buffer fds ride along (SCM_RIGHTS) */
    EVOB_FRAME   = 2,   /* host -> EVO: a buffer is complete */
    EVOB_RELEASE = 3,   /* EVO -> host: EVO has copied it, paint into it again */
    EVOB_BYE     = 4,   /* either way: orderly end */
    /* control, EVO -> host (Phase 1) */
    EVOB_LOAD    = 5,   /* payload: the URL */
    EVOB_NAV     = 6,   /* struct evob_nav */
    EVOB_POINTER = 7,   /* struct evob_pointer */
    EVOB_SCROLL  = 8,   /* struct evob_scroll */
    EVOB_KEY     = 9,   /* struct evob_key, then the text the key types */
    EVOB_TEXT    = 10,  /* payload: text to insert into the focused field */
    EVOB_RESIZE  = 11,  /* struct evob_size; the host answers with a HELLO */
    EVOB_VISIBLE = 12,  /* struct evob_flag: 0 pauses painting (playback) */
    EVOB_JS      = 13,  /* payload: a script to run in the main frame */
    /* events, host -> EVO (Phase 1) */
    EVOB_EVENT   = 14,  /* struct evob_event, then its text */
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

/* nbuf 2: new buffers, their fds attached. nbuf 0 (answer to RESIZE): the
 * same buffers, a new page size; width and height never exceed what the
 * buffers were made for, and stride stays the same. */
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

enum evob_nav_op {
    EVOB_NAV_BACK    = 1,
    EVOB_NAV_FORWARD = 2,
    EVOB_NAV_RELOAD  = 3,
    EVOB_NAV_STOP    = 4,
};

struct evob_nav {
    uint32_t op;
};

enum evob_pointer_action {
    EVOB_PTR_MOVE = 0,
    EVOB_PTR_DOWN = 1,
    EVOB_PTR_UP   = 2,
};

/* Page pixels. button: 1 left, 2 middle, 3 right (unused for MOVE). */
struct evob_pointer {
    int32_t  x, y;
    uint32_t action, button;
};

/* A wheel step at (x, y); positive dy scrolls the content down. */
struct evob_scroll {
    int32_t x, y, dx, dy;
};

/* key_code is a DOM key code (13 Enter, 8 Backspace, 37..40 arrows ...);
 * modifiers: 1 shift, 2 control, 4 alt, 8 meta. */
struct evob_key {
    uint32_t key_code, modifiers, pressed;
};

/* The page in EVO's UI pixels. scale (optional: an 8-byte RESIZE, or 0,
 * means 1) is how many buffer pixels per UI pixel EVO wants: the host's view
 * is width*scale x height*scale at page zoom `scale`, the HELLO answer gives
 * that buffer size, and POINTER / SCROLL stay in UI pixels. */
struct evob_size {
    uint32_t width, height;
    uint32_t scale;
};
#define EVOB_SIZE_V1_BYTES 8u   /* width and height only */

struct evob_flag {
    uint32_t value;
};

enum evob_event_kind {
    EVOB_EV_URL           = 1,   /* text: the page URL */
    EVOB_EV_TITLE         = 2,   /* text: the page title */
    EVOB_EV_LOAD_STARTED  = 3,
    EVOB_EV_LOAD_FINISHED = 4,
    EVOB_EV_LOAD_FAILED   = 5,   /* text: url, a newline, the error */
    EVOB_EV_NAV_STATE     = 6,   /* arg: bit 0 can go back, bit 1 can go forward */
    EVOB_EV_TEXT_FOCUS    = 7,   /* arg: 1 an editable field has focus, 0 not */
    EVOB_EV_SCRIPT        = 8,   /* text: a page script message, as JSON */
    EVOB_EV_NEW_WINDOW    = 9,   /* text: the URL window.open() asked for */
    EVOB_EV_CURSOR        = 10,  /* arg: 0 pointer, 1 hand, 2 none */
    EVOB_EV_FULLSCREEN    = 11,  /* arg: 1 entered, 0 left */
    EVOB_EV_ENGINE_ERROR  = 12,  /* text: why a web process could not start */
};

struct evob_event {
    uint32_t kind, arg;
};

#endif
