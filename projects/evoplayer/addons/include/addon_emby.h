/*
 * addon_emby.h — the Emby & Jellyfin media-server client.
 *
 * One client, two instances. The two servers share their REST API almost
 * entirely; what differs is the path prefix (Emby serves under /emby,
 * Jellyfin at the root), the auth header (Jellyfin 12 answers the legacy
 * X-Emby-Token with 401 and wants `Authorization: MediaBrowser ..., Token=`),
 * and the conf file. provider_emby.c and provider_jellyfin.c are thin vtables
 * over ms_client(MS_EMBY) and ms_client(MS_JELLYFIN).
 *
 * The emby_* functions at the bottom are the old single-instance API, kept as
 * wrappers over the Emby instance for the host tests.
 */
#ifndef ADDON_EMBY_H
#define ADDON_EMBY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "evo_addon.h"
#include "evo_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EMBY_MAX_ITEMS 64

typedef struct emby_config {
    char host[128];
    int  port;
    char username[64];
    char password[64];
    /* A session token. Stock servers hand out 32 hex chars; a Jellyfin-
     * compatible service behind a proxy (AIOStreams) hands out a 378-char
     * JWT, which a 128-byte field cut in half - every request after sign-in
     * then came back 401 (hardware, 2026-10-01). */
    char token[1024];
    char user_id[64];
    char server_name[64];
    char server_version[32];
    /* https rather than http for every request, including the stream URL that
     * FFmpeg opens. A reverse-proxied Emby is the common deployment and was
     * simply unreachable before this existed (#90). */
    bool use_https;
    bool is_connected;
    /* The path the server lives under behind a reverse proxy ("/jellyfin"),
     * "" at the root. Every request, image and stream URL is built under it. */
    char path[96];
} emby_config_t;

typedef enum { MS_EMBY = 0, MS_JELLYFIN = 1 } ms_kind_t;

typedef struct ms_client ms_client_t;

typedef void (*emby_auth_cb)(int success, const char *msg, void *userdata);

ms_client_t   *ms_client(ms_kind_t kind);
emby_config_t *ms_config(ms_client_t *c);

/* Load / save the instance's conf (emby.conf, jellyfin.conf). */
int  ms_load(ms_client_t *c);
int  ms_save(ms_client_t *c);

/* A server address is set. */
int  ms_is_configured(ms_client_t *c);
/* No session yet (or the server rejected the last one). */
int  ms_needs_sign_in(ms_client_t *c);
/* A session token is stored; sign_out forgets it and keeps the address. */
int  ms_is_signed_in(ms_client_t *c);
void ms_sign_out(ms_client_t *c);

/* The address as http[s]://host:port. set_source re-points the instance and
 * drops its session: a new address needs a new sign-in. */
const char *ms_get_source(ms_client_t *c);
int  ms_set_source(ms_client_t *c, const char *value);

/* The first user the server lists publicly, "" when it hides them. */
int  ms_suggest_user(ms_client_t *c, void (*cb)(const char *name, void *ud), void *ud);
/* AuthenticateByName; on success the token and user id are saved. The
 * password is never written to disk. */
int  ms_sign_in(ms_client_t *c, const char *user, const char *password,
                evo_provider_auth_cb cb, void *ud);

/* Jellyfin Quick Connect: a code to approve in another signed-in app.
 * Emby has no such flow; its provider leaves the slots empty. */
int  ms_qc_start(ms_client_t *c, evo_provider_qc_code_cb cb, void *ud);
int  ms_qc_poll(ms_client_t *c, evo_provider_qc_cb cb, void *ud);

/* The provider seam, implemented once for both servers. */
int  ms_list_catalog(ms_client_t *c, const char *parent_id, int page,
                     evo_provider_items_cb cb, void *ud);
int  ms_search(ms_client_t *c, const char *query, int page,
               evo_provider_items_cb cb, void *ud);
int  ms_resolve(ms_client_t *c, const char *item_id,
                evo_provider_resolve_cb cb, void *ud);
void ms_report(ms_client_t *c, const char *item_id, int64_t pos_sec,
               int64_t dur_sec, evo_provider_play_state_t state);

/* The version about to play (the provider's stream_chosen): later reports
 * carry its MediaSourceId and PlaySessionId. */
void ms_stream_chosen(ms_client_t *c, const char *item_id, const evo_stream_choice_t *choice);

/* A PlaybackInfo reply -> one choice per MediaSource, server order. Returns
 * the count, 0 when the body lists none. Pure apart from the client's address
 * and token, which go into the URLs; exposed for the host tests. */
int  ms_parse_sources(ms_client_t *c, const char *item_id, const char *body,
                      evo_stream_choice_t *out, int max);

/* Direct stream URL (Static=true, the file as it is) for a raw item id. */
int  ms_build_stream_url(ms_client_t *c, const char *item_id, char *out, size_t cap);

/* ---- the old single-instance API, over the Emby instance ---- */
int  emby_init(void);
int  emby_save_config(void);
emby_config_t *emby_get_config(void);
void emby_set_server(const char *host, int port, const char *username, const char *password);
void emby_disconnect(void);
int  emby_build_stream_url(const char *item_id, char *out_url, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* ADDON_EMBY_H */
