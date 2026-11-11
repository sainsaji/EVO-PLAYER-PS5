/*
 * web_stubs.cpp - the browser engine calls for the host preview (#237).
 * There is no engine host on the PC: the engine stays "starting" and sends
 * nothing, so the Web screens show Prospero Browser's own pages (speed dial,
 * menus, address keyboard) the way they look before the first web page.
 */
#include "evo_browser_engine.h"

#include <cstdint>

extern "C" {

/* Settings > Experimental > Web browser (06b4c76): on in the preview, so the rail shows Web. */
int evo_settings_web_browser_enabled(void) { return 1; }
void evo_browser_open(void) {}
evo_browser_state_t evo_browser_state(char *why, int why_cap)
{
    if (why && why_cap > 0)
        why[0] = 0;
    return EVO_BROWSER_STARTING;
}
void evo_browser_load(const char *) {}
void evo_browser_nav(int) {}
void evo_browser_pointer(int, int, int, int) {}
void evo_browser_scroll(int, int, int, int) {}
void evo_browser_key(unsigned, unsigned, int, const char *) {}
void evo_browser_text(const char *) {}
void evo_browser_resize(int, int) {}
void evo_browser_set_visible(int) {}
int evo_browser_poll_event(evo_browser_event_t *) { return 0; }
int evo_browser_page_texture(std::uint32_t *, float *, float *) { return 0; }

}  // extern "C"
