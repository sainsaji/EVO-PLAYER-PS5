#ifndef EVO_PHONE_REMOTE_H
#define EVO_PHONE_REMOTE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The phone as a remote (#238): the page at <ps5>:<log port>/remote
 * (evo_log_server.c) posts commands; the frame loop runs them. In every
 * build, not only --usb-remote ones.
 *
 * Commands (one per call, any thread):
 *   key <up|down|left|right|cross|circle|triangle|square|options|l1|r1|l2|r2>
 *   pause                 play / pause
 *   seek <+N|-N|N>        seconds, relative or absolute
 *   chapter <+1|-1>
 *   stop                  end playback, back to where it was opened from
 * Returns 0 if queued, -1 if the command is unknown or the queue is full.
 */
int evo_phone_remote_post(const char *cmd);

/* Main thread, once a frame: runs the queued commands and snapshots the
 * now-playing state for evo_phone_remote_state_json. */
void evo_phone_remote_poll(void);

/* Main thread: the synthetic button presses the page asked for (same mask as
 * the pad), OR'd into the pad state before input is read. */
unsigned int evo_phone_remote_take_buttons(void);

/* Any thread: {"active","paused","music","pos","dur","title"} from the last
 * poll. Returns the length written. */
size_t evo_phone_remote_state_json(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
