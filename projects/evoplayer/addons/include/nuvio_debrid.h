/*
 * nuvio_debrid.h — shared by the Torbox and Real-Debrid resolvers.
 *
 * Picking the right file out of a torrent is the part both services have in
 * common, and it is the part that decides whether a season pack plays the
 * episode the user chose. Ported from NuvioTV's selectDebridFile
 * (core/debrid/DebridFileSelection.kt), in the same order:
 *
 *   1. the filename the addon named (exact path, path suffix, then basename;
 *      case-sensitive first) - a single match wins, several is ambiguous
 *   2. the episode pattern: s01e02 / 1x02, not preceded by a letter or digit
 *      and not followed by a digit
 *   3. with neither hint given: the addon's fileIdx, then the largest video
 *
 * A hint that was given but matched nothing is a failure, not a fallback to
 * the largest file - playing S03E07 when S01E02 was asked for is worse than
 * saying the torrent did not have it.
 */
#ifndef NUVIO_DEBRID_H
#define NUVIO_DEBRID_H

#include <stdint.h>

#include "nuvio_stremio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nuvio_debrid_file {
    int64_t id;               /* the service's file id                  */
    char    path[512];        /* as the service reports it              */
    int64_t size;
    int     is_video;         /* by mimetype or extension               */
} nuvio_debrid_file_t;

/* Index into `files` of the file to play, or -1. */
int nuvio_debrid_select(const nuvio_debrid_file_t *files, int count,
                        const nuvio_magnet_hints_t *hints);

/* 1 when `name` ends in a video extension NuvioTV recognises. */
int nuvio_debrid_is_video_name(const char *name);

/* 1 when `name`'s basename carries season/episode in either form. */
int nuvio_debrid_episode_match(const char *name, int season, int episode);

#ifdef __cplusplus
}
#endif

#endif /* NUVIO_DEBRID_H */
