/*
 * evo_usb_remote.h — scriptable dev remote for the app module.
 *
 * When built with -DEVO_USB_REMOTE (scripts/package-app.sh --usb-remote) the
 * frame loop polls /mnt/usb0/evo_cmd for one-line commands and writes
 * /mnt/usb0/evo_status once a second. Lets tools/evo-remote.sh drive playback
 * (open a file, seek) and read state over FTP with no controller and no
 * rebuild between tests. Compiled out entirely otherwise.
 *
 * Commands (evo_cmd, consumed then deleted):
 *   play <path>        open <path> from the start
 *   seek <sec>         seek to absolute <sec>
 *   seek +<sec>        seek forward
 *   seek -<sec>        seek back
 *
 * Status line: build=<id> t=<s> scr=<n> be=<0|1> pos=<s> dur=<s> fps=<n>
 *              fatal=<0|1> eof=<0|1> active=<0|1>
 */
#ifndef EVO_USB_REMOTE_H
#define EVO_USB_REMOTE_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(EVO_USB_REMOTE) && defined(EVO_APP_MODULE)
/* Call once per frame from main()'s loop. Cheap: a stat() on the cmd file and,
 * at most once a second, a status write. */
void evo_usb_remote_poll(void);
#else
#define evo_usb_remote_poll() ((void)0)
#endif

/* Provided by the host (main.c): open <path> from the beginning, mirroring the
 * browse->select path (nav push, return screen, start_video_playback). */
void evo_open_media_path(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* EVO_USB_REMOTE_H */
