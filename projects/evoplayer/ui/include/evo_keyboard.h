/*
 * evo_keyboard.h — Global on-screen virtual keyboard modal for EVO Player.
 *
 * Provides a responsive, accessible controller-driven virtual keyboard with
 * layout switching (lowercase, uppercase, numbers/symbols), text editing,
 * shortcuts (Square = Backspace, Triangle = Done, Circle = Cancel), and
 * seamless integration with any text entry field across the application.
 */
#ifndef EVO_KEYBOARD_H
#define EVO_KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>
#include "evo_draw.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*evo_keyboard_cb)(const char *text, void *userdata);

/*
 * Open the global virtual keyboard modal.
 *
 * `title`: Prompt / description shown above the text field.
 * `initial_value`: Pre-filled text (can be NULL or empty).
 * `max_len`: Maximum allowable characters (excluding NUL).
 * `on_submit`: Callback invoked when user accepts with Done / Triangle.
 * `userdata`: Context pointer forwarded to `on_submit`.
 */
void evo_keyboard_open(const char *title,
                       const char *initial_value,
                       int max_len,
                       evo_keyboard_cb on_submit,
                       void *userdata);

/* Close the virtual keyboard without submitting. */
void evo_keyboard_close(void);

/* Check if the keyboard modal is currently active. */
int  evo_keyboard_is_open(void);

/* Retrieve current text in the keyboard buffer. */
const char *evo_keyboard_get_text(void);

/*
 * Handle controller pad events for the keyboard.
 * Returns 1 if input was handled/consumed by the keyboard modal, 0 otherwise.
 */
int  evo_keyboard_handle_input(uint32_t pressed);

/*
 * Render the keyboard overlay onto the framebuffer.
 * Call this after the underlying screen has been rendered.
 */
void evo_screen_keyboard(uint32_t *fb);

#ifdef __cplusplus
}
#endif

#endif /* EVO_KEYBOARD_H */
