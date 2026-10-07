/*
 * evo_playlist_sniff.h - is this reply a list of channels rather than a stream?
 *
 * A person pastes their provider's playlist link (".../get.php?...&type=m3u_plus")
 * where a channel address belongs, or saves it as the only entry of an .m3u.
 * EVO then tries to play the playlist itself as a video; the server answers in
 * a tenth of a second with text, FFmpeg refuses it as invalid data, and nothing
 * says why. When an open fails that way, the first few KB of the reply tell us
 * what it really was.
 *
 * A channel list is "#EXTM3U" with "#EXTINF" entries and none of HLS's own
 * "#EXT-X-" tags. HLS is also M3U, and FFmpeg plays it, so it must not match.
 * Pure text test: no FFmpeg, no I/O.
 */
#ifndef EVO_PLAYLIST_SNIFF_H
#define EVO_PLAYLIST_SNIFF_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if buf[0..n) looks like the start of an IPTV channel list. */
int evo_playlist_sniff_is_channel_list(const char *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* EVO_PLAYLIST_SNIFF_H */
