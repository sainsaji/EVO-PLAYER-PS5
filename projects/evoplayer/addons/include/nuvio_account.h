/*
 * nuvio_account.h — a Nuvio account, for addon and watch-progress sync.
 *
 * Nuvio's backend is Supabase: GoTrue for sign-in, PostgREST for tables and
 * RPCs. NuvioTV reaches its official server with a URL and publishable key
 * that are injected at build time and are not in its source; this client
 * never carries those. It takes a server the user names:
 *
 *   server=https://my.nuvio.example   NuvioTV's self-hosted discovery,
 *                                     GET <server>/.well-known/nuvio ->
 *                                     backend_url + publishable_key
 *   backend_url= + publishable_key=   given directly instead
 *
 * Configured by FTP, in /data/evoplayer/providers/nuvio-native/account.conf:
 *
 *   server=https://nuvio.example.com
 *   email=me@example.com
 *   password=...        read once, exchanged for a refresh token, and then
 *                       removed from the file - it is never written back
 *   profile=1           optional, NuvioTV profile id (1 = primary)
 *
 * Every call is asynchronous, completes on the main thread through
 * evo_net_poll(), and fires its callback exactly once. A request that comes
 * back 401 refreshes the session once and is retried - NuvioTV's
 * withJwtRefreshRetry.
 */
#ifndef NUVIO_ACCOUNT_H
#define NUVIO_ACCOUNT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nuvio_account_cb)(int ok, const char *body, size_t len, void *ud);

/* Load account.conf from `path`. No network. Safe to call again. */
void nuvio_account_init(const char *path);

/* 1 when there is enough to try: a server (or backend + key), and either a
 * saved session or an email and password. */
int  nuvio_account_is_configured(void);

/* 1 once a sign-in has succeeded in this or an earlier session. */
int  nuvio_account_has_session(void);

const char *nuvio_account_status(void);   /* one line for the UI / log */
int  nuvio_account_profile(void);

/* Sign in or refresh as needed. cb's body is unused. */
int  nuvio_account_ensure(nuvio_account_cb cb, void *ud);

/* POST /rest/v1/rpc/<name> with a JSON body ("{}" for none). */
int  nuvio_account_rpc(const char *name, const char *json_body,
                       nuvio_account_cb cb, void *ud);

/* GET /rest/v1/<path_and_query>, e.g. "addons?select=url&user_id=eq.X". */
int  nuvio_account_select(const char *path_and_query, nuvio_account_cb cb, void *ud);

/* The origin client id NuvioTV sends as p_origin_client_id. */
const char *nuvio_account_client_id(void);

#ifdef __cplusplus
}
#endif

#endif /* NUVIO_ACCOUNT_H */
