/*
 * provider_stremio.h - the saved Stremio addon list, for the "Remove addon"
 * rows in the Addons options menu. Adding goes through the provider's
 * set_source; removing one needs the list, so it is exposed here.
 */
#ifndef PROVIDER_STREMIO_H
#define PROVIDER_STREMIO_H

#include "evo_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

int         evo_stremio_addon_count(void);
/* The manifest URL, or "" when i is out of range. */
const char *evo_stremio_addon_url(int i);
/* The manifest's name once loaded, else the URL without scheme or /manifest.json. */
const char *evo_stremio_addon_name(int i);
/* Drops entry i, shifts the rest down and saves. 0 ok, -1 bad index. */
int         evo_stremio_addon_remove(int i);
/* Host tests: the rank provider_stremio.c gives one addon stream (JSON object). */
int         evo_stremio_rank_json(const char *json, evo_stream_choice_t *out);

/*
 * Nuvio's native screens run on the same addon engine, over the addons the
 * Nuvio web app has installed (provider_stremio.c reads them from its saved
 * page storage). provider_nuvio.c's vtable calls these.
 */
int evo_nuvio_list_catalog(const char *parent_id, int page, evo_provider_items_cb cb, void *ud);
int evo_nuvio_search(const char *query, int page, evo_provider_items_cb cb, void *ud);
int evo_nuvio_resolve(const char *item_id, evo_provider_resolve_cb cb, void *ud);
int evo_nuvio_panel(const char *item_id, const char *kind, evo_provider_rows_cb cb, void *ud);
/*
 * Add and remove Nuvio addons from EVO. They edit the same saved page-storage
 * keys the web version reads, for the active profile.
 * count: the saved addons that can be removed (0 while Nuvio is on its Cinemeta
 * default). add: a manifest URL, base URL or stremio:// link; 0 ok, -1 not an
 * address, -2 not saved. remove: drops entry i, 0 ok.
 */
int         evo_nuvio_addon_count(void);
const char *evo_nuvio_addon_name(int i);
int         evo_nuvio_addon_add(const char *value);
int         evo_nuvio_addon_remove(int i);
/* In provider_nuvio.c: the page-storage file of the Nuvio site in use,
 * relative to the data root ("webui/<host>_<port>.json"). */
const char *evo_nuvio_store_file(void);

#ifdef __cplusplus
}
#endif

#endif
