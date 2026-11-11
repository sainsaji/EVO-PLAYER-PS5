/*
 * evo_cast_receiver.h - DIAL (DIscovery And Launch) cast receiver for EVO Player.
 *
 * Implements SSDP multicast discovery (UDP 1900 on 239.255.255.250) and
 * DIAL HTTP service (TCP 8765) matching CastDesk (Shyam-Sangeeth/CastDesk).
 * Allows the YouTube mobile app on Android/iOS to discover EVO Player and cast
 * to it.
 */
#ifndef EVO_CAST_RECEIVER_H
#define EVO_CAST_RECEIVER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVO_CAST_DEFAULT_PORT 8765
#define EVO_CAST_FALLBACK_COUNT 10

typedef void (*evo_cast_launch_cb)(const char *launch_data, void *userdata);
typedef void (*evo_cast_stop_cb)(void *userdata);

/* Start discovery and DIAL service. Returns 0 on success, -1 on failure. */
int evo_cast_receiver_start(const char *friendly_name, int preferred_port);

/* Stop receiver and multicast ssdp:byebye. */
void evo_cast_receiver_stop(void);

/* Status queries */
int evo_cast_receiver_is_running(void);
int evo_cast_receiver_port(void);
const char* evo_cast_receiver_device_id(void);
const char* evo_cast_receiver_friendly_name(void);

/* Update friendly name displayed in phone Cast menus */
void evo_cast_receiver_set_friendly_name(const char *name);

/* Register callbacks for launch and stop events from phone */
void evo_cast_receiver_set_callbacks(evo_cast_launch_cb on_launch, evo_cast_stop_cb on_stop, void *userdata);

/* Store additionalData reported by YouTube TV app (e.g. screenId, loungeToken) */
void evo_cast_receiver_set_additional_data(const char *key, const char *value);
void evo_cast_receiver_clear_additional_data(void);

/* Local IP discovery */
void evo_cast_receiver_get_local_ip(char *out, size_t cap);

/* Query whether an app is marked running */
int evo_cast_receiver_app_is_running(void);
void evo_cast_receiver_set_app_running(int running);

/* Helper to parse launch query/body parameters: "pairingCode=...&v=...&t=..." */
int evo_cast_parse_param(const char *data, const char *key, char *out, size_t cap);

/* Parse video ID and currentTime from YouTube Lounge event JSON stream */
int evo_cast_parse_lounge_video(const char *text, char *out_vid, size_t vid_cap, char *out_time, size_t time_cap);

/* Register TV pairing code from phone with YouTube Lounge pairing API */
int evo_cast_register_pairing_code(const char *pairing_code);

/* Store dynamic DASH MPD XML manifest for the current YouTube cast */
int evo_cast_receiver_set_stream_mpd(const char *mpd_xml, const char *title);
/* Store a direct stream URL (combined audio/video) */
int evo_cast_receiver_set_direct_stream(const char *url, const char *title);
/* Get current cast stream title */
const char* evo_cast_receiver_stream_title(void);

/* Events the receiver posts for the main thread; evo_cast_take_events()
 * returns and clears them. */
#define EVO_CAST_EV_PHONE_LEFT  0x1   /* Lounge remoteDisconnected on EVO's screen */
#define EVO_CAST_EV_PAGE_ENDED  0x2   /* the browser cast page finished the video */
int evo_cast_take_events(void);

/* Browser cast: watch the youtube.com/tv page in Sony's browser for the end
 * of `video_id`. Put a fresh code from evo_cast_new_pairing_code() in the page
 * URL as pairingCode=; the page registers it for its Lounge screen, EVO finds
 * the screen by it, joins as a silent remote and posts EVO_CAST_EV_PAGE_ENDED.
 * Stop is cheap and does not wait for the thread. */
void evo_cast_new_pairing_code(char *out, size_t cap);
void evo_cast_page_watch_start(const char *video_id, const char *pairing_code, const char *start_time);
void evo_cast_page_watch_stop(void);
int  evo_cast_page_watch_active(void);

/* Resolve a YouTube video ID via Innertube into a playable stream URL or local DASH MPD.
 * Returns 0 on success, -1 on failure. */
int evo_cast_resolve_youtube(const char *video_id, char *out_url, size_t url_cap, char *out_title, size_t title_cap);

#ifdef __cplusplus
}
#endif

#endif /* EVO_CAST_RECEIVER_H */
