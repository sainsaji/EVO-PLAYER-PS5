/*
 * evo_feedback — what the controller does when you touch the UI.
 *
 * Two channels, one semantic API:
 *
 *   sound     the existing synthesised blips in main.c, reached through a
 *             callback so the audio engine does not have to move
 *   lightbar  tinted to the active theme accent, with a brief flash on
 *             confirm and a red pulse on error
 *
 * Callers name the *event*, not the effect: evo_feedback(EVO_FB_CONFIRM).
 * That way a screen never has to know which channel does what, and turning
 * one off is a single branch here rather than fifteen at the call sites.
 *
 * There is deliberately no haptics channel. It was built, and it does not
 * work on this platform: on firmware 12.70, launched via hbldr, every
 * vibration entry point in libScePad either reports success and produces
 * nothing or rejects the call outright, while scePadSetLightBar succeeds on
 * the very same handle. The evidence and the full probe results are in
 * docs/ui-handoff.md - read that before adding it back.
 */
#ifndef EVO_FEEDBACK_H
#define EVO_FEEDBACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EVO_FB_MOVE = 0,   /* cursor stepped to a neighbouring item      */
    EVO_FB_WRAP,       /* cursor wrapped past the end of a list      */
    EVO_FB_BOUNDARY,   /* pushed against the end of a clamped list   */
    EVO_FB_CONFIRM,    /* selection accepted                         */
    EVO_FB_OPEN,       /* navigated into a section or folder         */
    EVO_FB_CANCEL,     /* backed out                                 */
    EVO_FB_TOGGLE,     /* a value changed in place                   */
    EVO_FB_ERROR,      /* refused - nothing to open, load failed     */
    EVO_FB_COUNT
} evo_feedback_kind;

/*
 * `sfx` is main.c's evo_sfx_play(); it takes the EVO_SFX_* enum. Passing NULL
 * leaves the sound channel silent. `pad_handle` is the scePadOpen handle, or
 * negative to disable the lightbar.
 */
void evo_feedback_init(int pad_handle, void (*sfx)(int sfx_kind));

/* The pad handle can arrive after init (it does, on a cold boot). */
void evo_feedback_set_pad(int pad_handle);

/* Fire the feedback for `kind`. Cheap and non-blocking. */
void evo_feedback(evo_feedback_kind kind);

/*
 * Let the lightbar flash decay back to the resting accent colour. Call once
 * per frame with a monotonic millisecond clock.
 */
void evo_feedback_tick(uint64_t now_ms);

void evo_feedback_set_sound(int enabled);
void evo_feedback_set_lightbar(int enabled);

int  evo_feedback_sound_enabled(void);
int  evo_feedback_lightbar_enabled(void);

/*
 * Repaint the lightbar from the current theme accent. Call after a theme
 * change; the pad is part of the theme surface too.
 */
void evo_feedback_refresh_lightbar(void);

#ifdef __cplusplus
}
#endif

#endif /* EVO_FEEDBACK_H */
