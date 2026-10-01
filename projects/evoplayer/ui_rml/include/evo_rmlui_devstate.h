/*
 * evo_rmlui_devstate.h - #115: the UI state as text, for the dev remote.
 *
 * Driving EVO from tools/evo-remote.sh used to mean key -> screenshot -> look
 * at the picture to find out where the cursor went. RmlUi already knows: this
 * walks every visible document in every context (EVO's screens, the dialog,
 * the toast, the keyboard, a provider's own screen) and reports the screen,
 * the focused element, any modal and toast, as one JSON document.
 * evo_usb_remote.c writes it to /mnt/usb0/evo_ui.json when it changes.
 *
 * "Focused" is found two ways, because EVO uses both:
 *   - RmlUi's own focus (Context::GetFocusElement) - provider screens;
 *   - EVO's highlight classes: `focused`, `*-focused`, `*-cursor` - every
 *     native screen, which moves a class instead of RmlUi focus.
 *
 * Dev builds only (EVO_USB_REMOTE + EVO_APP_MODULE). Nothing here exists in a
 * release build.
 */
#ifndef EVO_RMLUI_DEVSTATE_H
#define EVO_RMLUI_DEVSTATE_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(EVO_USB_REMOTE) && defined(EVO_APP_MODULE)
/*
 * The current UI state as a JSON object (no trailing newline). The pointer is
 * to static storage, valid until the next call. Main thread only - it reads
 * the RmlUi DOM. `screen_id` is main.c's `screen`, `paused` the player state.
 */
const char* evo_rmlui_dev_ui_json(int screen_id, int paused);

/* From evo_rmlui_update_playback_params(): what the player OSD showed this
 * frame. The OSD is not an EVO screen of its own, so this is how the snapshot
 * knows whether the chrome is up. */
void evo_rmlui_dev_note_osd(int chrome_visible, int show_stats, int scrub_active);
#endif

#ifdef __cplusplus
}
#endif

#endif /* EVO_RMLUI_DEVSTATE_H */
