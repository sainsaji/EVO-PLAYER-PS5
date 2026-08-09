#include "evo_input.h"

#include <string.h>

/*
 * Pad masks. Duplicated from main.c deliberately: this module is meant to be
 * the only place that knows them, and main.c's copies are on their way out.
 *
 * L3/R3 are 0x2/0x4. The values inherited from upstream (0x20000/0x40000)
 * matched no button at all, which is why R3 subtitle delay never worked
 * despite being documented - see the note above the defines in main.c.
 */
#define PAD_L3        0x00000002u
#define PAD_R3        0x00000004u
#define PAD_OPTIONS   0x00000008u
#define PAD_UP        0x00000010u
#define PAD_RIGHT     0x00000020u
#define PAD_DOWN      0x00000040u
#define PAD_LEFT      0x00000080u
#define PAD_L2        0x00000100u
#define PAD_R2        0x00000200u
#define PAD_L1        0x00000400u
#define PAD_R1        0x00000800u
#define PAD_TRIANGLE  0x00001000u
#define PAD_CIRCLE    0x00002000u
#define PAD_CROSS     0x00004000u
#define PAD_SQUARE    0x00008000u

static const uint32_t ACTION_MASK[EVO_ACT_COUNT] = {
    PAD_UP,       /* EVO_ACT_UP         */
    PAD_DOWN,     /* EVO_ACT_DOWN       */
    PAD_LEFT,     /* EVO_ACT_LEFT       */
    PAD_RIGHT,    /* EVO_ACT_RIGHT      */
    PAD_CROSS,    /* EVO_ACT_CONFIRM    */
    PAD_CIRCLE,   /* EVO_ACT_CANCEL     */
    PAD_TRIANGLE, /* EVO_ACT_CONTEXT    */
    PAD_SQUARE,   /* EVO_ACT_ALT        */
    PAD_L1,       /* EVO_ACT_PAGE_UP    */
    PAD_R1,       /* EVO_ACT_PAGE_DOWN  */
    PAD_L2,       /* EVO_ACT_SHOULDER_L */
    PAD_R2,       /* EVO_ACT_SHOULDER_R */
    PAD_OPTIONS,  /* EVO_ACT_MENU       */
    PAD_L3,       /* EVO_ACT_STICK_L    */
    PAD_R3        /* EVO_ACT_STICK_R    */
};

/*
 * Only directional actions and page steps repeat. A held CROSS must not fire
 * twice - that would launch a file and then immediately re-launch it.
 */
static int action_repeats(evo_action a)
{
    switch (a) {
        case EVO_ACT_UP:
        case EVO_ACT_DOWN:
        case EVO_ACT_LEFT:
        case EVO_ACT_RIGHT:
        case EVO_ACT_PAGE_UP:
        case EVO_ACT_PAGE_DOWN:
            return 1;
        default:
            return 0;
    }
}

/*
 * Repeat curve. 380ms before the first repeat, so a deliberate single press
 * is never doubled; then 110ms, tightening to 45ms once the direction has
 * been held for a second and a half and the intent is clearly "keep going".
 */
#define REPEAT_DELAY_MS   380u
#define REPEAT_SLOW_MS    110u
#define REPEAT_FAST_MS     45u
#define REPEAT_RAMP_MS   1500u

static uint64_t repeat_interval(uint64_t held_for_ms)
{
    if (held_for_ms < REPEAT_RAMP_MS) return REPEAT_SLOW_MS;
    return REPEAT_FAST_MS;
}

void evo_input_reset(evo_input *in)
{
    memset(in, 0, sizeof(*in));
}

void evo_input_update(evo_input *in, uint32_t buttons, uint64_t now_ms)
{
    int a;

    in->prev = in->raw;
    in->raw  = buttons;
    in->edge = buttons & ~in->prev;

    for (a = 0; a < EVO_ACT_COUNT; a++) {
        uint32_t mask = ACTION_MASK[a];
        int down      = (buttons & mask) != 0;

        in->pressed[a]  = 0;
        in->repeated[a] = 0;

        if (!down) {
            in->held_since_ms[a]  = 0;
            in->next_repeat_ms[a] = 0;
            continue;
        }

        if ((in->edge & mask) != 0) {
            in->pressed[a]        = 1;
            in->held_since_ms[a]  = now_ms;
            in->next_repeat_ms[a] = now_ms + REPEAT_DELAY_MS;
            continue;
        }

        if (!action_repeats((evo_action)a)) continue;

        if (now_ms >= in->next_repeat_ms[a]) {
            uint64_t held = now_ms - in->held_since_ms[a];

            in->repeated[a]       = 1;
            in->next_repeat_ms[a] = now_ms + repeat_interval(held);
        }
    }
}

int evo_input_pressed(const evo_input *in, evo_action a)
{
    if (a < 0 || a >= EVO_ACT_COUNT) return 0;
    return in->pressed[a];
}

int evo_input_fired(const evo_input *in, evo_action a)
{
    if (a < 0 || a >= EVO_ACT_COUNT) return 0;
    return in->pressed[a] || in->repeated[a];
}

int evo_input_held(const evo_input *in, evo_action a)
{
    if (a < 0 || a >= EVO_ACT_COUNT) return 0;
    return (in->raw & ACTION_MASK[a]) != 0;
}

int evo_input_any(const evo_input *in)
{
    return in->edge != 0;
}
