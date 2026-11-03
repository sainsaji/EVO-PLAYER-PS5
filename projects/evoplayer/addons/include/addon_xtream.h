/*
 * addon_xtream.h — Xtream Codes IPTV provider client for EVO Player (#93).
 */
#ifndef ADDON_XTREAM_H
#define ADDON_XTREAM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "evo_addon.h"
#include "evo_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xtream_config {
    char host[128];
    int  port;
    char username[64];
    char password[64];
    bool use_https;
    char stream_format[16]; /* "m3u8" (default) or "ts" */
    bool is_connected;
    char status[32];        /* "Active", etc. */
    char exp_date[32];
    char server_name[64];
    char xmltv_url[256];    /* own guide; empty = the panel's xmltv.php */
} xtream_config_t;

/* Initialize Xtream client and load persistent credentials from xtream.conf */
int  xtream_init(void);

/* Save Xtream configuration to disk */
int  xtream_save_config(void);

/* Retrieve active configuration pointer */
xtream_config_t *xtream_get_config(void);

/* Update server connection settings */
void xtream_set_server(const char *host, int port, const char *username, const char *password, bool use_https);

/* Set preferred stream format ("m3u8" or "ts") */
void xtream_set_stream_format(const char *fmt);

typedef void (*xtream_auth_cb)(int success, const char *msg, void *userdata);

/* Authenticate with Xtream server asynchronously */
int  xtream_connect_async(xtream_auth_cb callback, void *userdata);

/* One object of an Xtream list reply (get_live_streams, get_vod_streams,
 * get_series), reduced to what search needs. */
typedef struct xtream_list_obj {
    char id[24];        /* stream_id or series_id */
    char name[256];
    char icon[512];     /* stream_icon or cover */
    char ext[12];       /* container_extension (VOD) */
} xtream_list_obj_t;
typedef void (*xtream_list_fn)(const xtream_list_obj_t *o, void *ud);

/* Walks a JSON array of objects without building a tree: a 100k-title VOD
 * list as a cJSON tree is several times its own size, which is what ran a
 * search out of memory. `id_key` / `icon_key` name the fields to read.
 * Returns the number of objects, or -1 when the body is not a JSON array. */
int xtream_scan_list(const char *body, size_t len, const char *id_key, const char *icon_key,
                     xtream_list_fn fn, void *ud);

/* Program guide (#149). The URL passed to load carries the account password:
 * `label` (the host) is what gets logged. lookup fills now/next for an
 * epg_channel_id and returns 1 when there is anything. */
enum { XT_EPG_NONE = 0, XT_EPG_LOADING, XT_EPG_OK, XT_EPG_EMPTY, XT_EPG_FAILED };
void        xtream_epg_load(const char *url, const char *label);
int         xtream_epg_state(void);
int         xtream_epg_lookup(const char *epg_id, char *now, size_t now_sz,
                              char *next, size_t next_sz);
const char *xtream_epg_status(void);

/* The user's own XMLTV address ("" = the panel's guide). Saved, reloads now. */
const char *xtream_get_xmltv(void);
void        xtream_set_xmltv(const char *url);

extern const evo_provider_t evo_provider_xtream;

#ifdef __cplusplus
}
#endif

#endif /* ADDON_XTREAM_H */
