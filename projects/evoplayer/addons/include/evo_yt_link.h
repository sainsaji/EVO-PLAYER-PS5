#ifndef EVO_YT_LINK_H
#define EVO_YT_LINK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The 11-character video id of a YouTube link (watch?v=, youtu.be/, shorts/,
 * embed/, live/, music.youtube.com, or a bare id). 0 on success. */
int evo_yt_link_video_id(const char *link, char *out_id, size_t cap);

/* Full-length YouTube playback without a PO token: the web_embedded player
 * API's HLS manifest (up to 1080p60), with its `n` challenge solved by
 * yt-dlp's solver (third_party/yt-dlp-ejs) running in QuickJS-NG. The
 * preprocessed player script is cached in the data dir per player version.
 * Blocking (several network round trips, a few seconds of JavaScript on a
 * cold cache): call it from a worker thread. 0 on success. */
int evo_yt_resolve_full(const char *video_id,
                        char *out_url, size_t url_cap,
                        char *out_title, size_t title_cap);

#ifdef __cplusplus
}
#endif

#endif
