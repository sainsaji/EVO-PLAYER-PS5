/*
 * evo_stremio_account - a Stremio account inside EVO.
 *
 * What the Stremio app keeps on its own account, EVO now reads from the same
 * place (https://api.strem.io, the protocol of Stremio/stremio-core):
 *
 *   sign-in      a 4-character code shown on the TV, entered at
 *                link.stremio.com on a phone or computer already signed in
 *                (link.stremio.com/api/v2/create, then .../read until it
 *                hands back the account's authKey). No password on the pad.
 *   addons       addonCollectionGet: the account's addons in its order, each
 *                with its manifest, so nothing has to be fetched per addon.
 *   library      datastoreGet on "libraryItem": what the user added, and
 *                what they were watching - Continue Watching is the items
 *                with a time offset.
 *   progress     datastorePut of one changed library item, so a film stopped
 *                on the PS5 resumes at the same second in Stremio and back.
 *
 * Everything is persisted under the data root (stremio_account.json,
 * stremio_addons.json, stremio_library.json) so EVO opens on the account's
 * shelves with no network at all, then refreshes.
 *
 * Main thread only: every request completes through evo_net_poll().
 */
#ifndef EVO_STREMIO_ACCOUNT_H
#define EVO_STREMIO_ACCOUNT_H

#include <stdint.h>

#include "cJSON.h"
#include "evo_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads what was saved. No network. */
void evo_sa_init(void);

int         evo_sa_signed_in(void);
const char *evo_sa_email(void);          /* "" until getUser answered */
void        evo_sa_sign_out(void);       /* forgets the key, addons and library */

/* Sign-in by code (evo_provider_t qc_* shape). */
int  evo_sa_link_start(evo_provider_qc_code_cb cb, void *ud);
int  evo_sa_link_poll(evo_provider_qc_cb cb, void *ud);
void evo_sa_link_cancel(void);

/*
 * Addons. cached() is the saved collection: an array of
 * {transportUrl, manifest, ...} in the account's order, or NULL. pull()
 * fetches it again; cb(ok, changed) tells whether the list of transport URLs
 * differs from the cached one.
 */
const cJSON *evo_sa_addons_cached(void);
int  evo_sa_addons_pull(void (*cb)(int ok, int changed, void *ud), void *ud);

/* Library. cached() is the array of library items, or NULL. */
const cJSON *evo_sa_library_cached(void);
int  evo_sa_library_pull(void (*cb)(int ok, void *ud), void *ud);
/* 1 while a pull of either is out. */
int  evo_sa_busy(void);

/*
 * Playback progress for one title. `lib_id` is the library item id ("tt0903747",
 * "kitsu:1"), `video_id` the episode ("tt0903747:1:2") or NULL for a film.
 * name/poster/type are used only when the title is not in the library yet (it
 * is added as a temporary item, which is how Stremio itself shows a title you
 * watched but never added in Continue Watching). Pushed at most once a minute
 * while playing, and at once when stopped.
 */
void evo_sa_progress(const char *lib_id, const char *type, const char *video_id,
                     const char *name, const char *poster,
                     int64_t pos_sec, int64_t dur_sec, int stopped);

/* Helpers, exposed for the host tests. */
/* RFC 3339 UTC with milliseconds, "2026-11-03T21:04:05.000Z". */
void evo_sa_iso8601(int64_t unix_sec, char out[32]);
/* Library id of an episode/video id: "tt123:1:2" -> "tt123", "kitsu:7:3" -> "kitsu:7". */
void evo_sa_lib_id_of(const char *video_id, char *out, int cap);
/* 1 when a library item belongs in Continue Watching (stremio-core's rule). */
int  evo_sa_in_continue_watching(const cJSON *item);

#ifdef __cplusplus
}
#endif

#endif
