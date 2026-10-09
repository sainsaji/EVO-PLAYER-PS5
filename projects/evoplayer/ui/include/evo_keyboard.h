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

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "evo_draw.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*evo_keyboard_cb)(const char *text, void *userdata);

enum {
    EVO_KEYBOARD_TYPE_VIRTUAL = 0,
    EVO_KEYBOARD_TYPE_NATIVE  = 1
};

/* Configure keyboard backend (0 = Custom Virtual Keyboard, 1 = Native PS5 IME Dialog) */
void evo_keyboard_set_type(int type);
int  evo_keyboard_get_type(void);

/*
 * #34: bring the native IME's subsystem up.
 *
 * MUST be called from main()'s PRE-UNJAIL slot, next to evo_vdec_probe() — the
 * credential swap in evo_jailbreak_self() poisons sceSysmoduleLoadModule the
 * same way it poisons it for libSceVideodec2 (#31) and libSceAgc. Loading the
 * IME dialog's system module lazily on first keyboard-open therefore fails, and
 * calling into libSceImeDialog with its module unloaded SIGSEGVs — which is
 * exactly how #34 presented.
 *
 * A failure here is not fatal: it latches the virtual keyboard for the session.
 * No-op on payload / host builds, which load the module on demand instead.
 */
void evo_keyboard_ime_probe(void);

/*
 * Open the global keyboard modal.
 *
 * `title`: Prompt / description shown above the text field.
 * `initial_value`: Pre-filled text (can be NULL or empty).
 * `max_len`: Maximum allowable characters (excluding NUL).
 * `on_submit`: Callback invoked when user accepts with Done / Triangle / Enter.
 * `userdata`: Context pointer forwarded to `on_submit`.
 */
void evo_keyboard_open(const char *title,
                       const char *initial_value,
                       int max_len,
                       evo_keyboard_cb on_submit,
                       void *userdata);

/*
 * Like evo_keyboard_open, for a long input (an address, a link) that is slow to
 * type with a pad. Always EVO's own keyboard, never the system IME, and the
 * title gains "phone: <ip>:9780/input". A phone browser on the same
 * network opens that page and sends the text, which is submitted as if typed.
 * Served by evo_log_server.c; without a network address it is a plain open.
 */
void evo_keyboard_open_phone(const char *title,
                             const char *initial_value,
                             int max_len,
                             evo_keyboard_cb on_submit,
                             void *userdata);

/* 1 and the prompt text while a phone-enabled keyboard is open, else 0. */
int  evo_keyboard_phone_info(char *title, size_t tcap);

/* Close the keyboard without submitting. */
void evo_keyboard_close(void);

/* Check if the keyboard modal is currently active. */
int  evo_keyboard_is_open(void);

/* Check if native PS5 IME dialog is the active backend. */
int  evo_keyboard_is_native_active(void);

/* Retrieve current text in the keyboard buffer. */
const char *evo_keyboard_get_text(void);

/*
 * Remote text entry (the --usb-remote dev remote: `evo-remote.sh type`).
 *
 * Both queue the request and return 1 if the virtual keyboard was open to
 * take it; evo_keyboard_update() applies it on the next frame, so the dev
 * remote's polling thread never touches g_kb directly.
 *
 * The NATIVE PS5 IME cannot be driven this way and these return 0 for it: it
 * is a system dialog that reads real HID, which nothing in EVO can reach.
 * Set KEYBOARD INPUT = VIRTUAL KEYBOARD before an unattended text test.
 */
int evo_keyboard_queue_text(const char *text);
int evo_keyboard_queue_submit(void);

/* Update keyboard lifecycle every frame (polls Native IME status if active) */
void evo_keyboard_update(void);

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

