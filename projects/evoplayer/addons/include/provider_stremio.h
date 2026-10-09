/*
 * provider_stremio.h - the saved Stremio addon list, for the "Remove addon"
 * rows in the Addons options menu. Adding goes through the provider's
 * set_source; removing one needs the list, so it is exposed here.
 */
#ifndef PROVIDER_STREMIO_H
#define PROVIDER_STREMIO_H

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

#ifdef __cplusplus
}
#endif

#endif
