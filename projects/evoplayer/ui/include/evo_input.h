/*
 * evo_input — raw pad bits in, semantic actions out.
 *
 * Two things this buys us.
 *
 * First, screens stop testing hardware bits. `pressed & PS5_PAD_BUTTON_CROSS`
 * appeared in a dozen places, each having to remember that CROSS confirms in
 * menus but toggles pause during playback. Actions are named for intent, and
 * the mapping lives in exactly one function.
 *
 * Second, key repeat. Holding DOWN did nothing: every list moved one item per
 * physical press, so reaching item 60 of a folder meant 60 presses. Repeat is
 * implemented here once rather than per screen, and it accelerates, so long
 * lists stay usable without making short ones feel slippery.
 */
#ifndef EVO_INPUT_H
#define EVO_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EVO_ACT_UP = 0,
    EVO_ACT_DOWN,
    EVO_ACT_LEFT,
    EVO_ACT_RIGHT,
    EVO_ACT_CONFIRM,     /* cross */
    EVO_ACT_CANCEL,      /* circle */
    EVO_ACT_CONTEXT,     /* triangle - favourite, view mode */
    EVO_ACT_ALT,         /* square  - media info */
    EVO_ACT_PAGE_UP,     /* L1 */
    EVO_ACT_PAGE_DOWN,   /* R1 */
    EVO_ACT_SHOULDER_L,  /* L2 */
    EVO_ACT_SHOULDER_R,  /* R2 */
    EVO_ACT_MENU,        /* options */
    EVO_ACT_STICK_L,     /* L3 */
    EVO_ACT_STICK_R,     /* R3 */
    EVO_ACT_COUNT
} evo_action;

typedef struct evo_input {
    uint32_t  raw;                    /* buttons held this frame */
    uint32_t  prev;                   /* buttons held last frame */
    uint32_t  edge;                   /* newly pressed this frame */

    /* Per-action repeat bookkeeping. */
    uint64_t  held_since_ms[EVO_ACT_COUNT];
    uint64_t  next_repeat_ms[EVO_ACT_COUNT];
    uint8_t   repeated[EVO_ACT_COUNT]; /* fired by repeat this frame */
    uint8_t   pressed[EVO_ACT_COUNT];  /* fired by a fresh press this frame */
} evo_input;

void evo_input_reset(evo_input *in);

/* Feed one frame of pad state. `buttons` is the raw scePad mask. */
void evo_input_update(evo_input *in, uint32_t buttons, uint64_t now_ms);

/* True on the frame the button went down. Use for anything destructive or
 * navigational-with-side-effects: confirm, cancel, delete. */
int evo_input_pressed(const evo_input *in, evo_action a);

/* True on a fresh press *or* an auto-repeat tick. Use for cursor movement. */
int evo_input_fired(const evo_input *in, evo_action a);

/* Still held down. */
int evo_input_held(const evo_input *in, evo_action a);

/*
 * Any action fired this frame - used to wake the playback control overlay
 * and to decide whether the frame needs feedback at all.
 */
int evo_input_any(const evo_input *in);

#ifdef __cplusplus
}
#endif

#endif /* EVO_INPUT_H */
