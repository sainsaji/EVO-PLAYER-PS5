#ifndef EVO_DATA_PATH_H
#define EVO_DATA_PATH_H

/*
 * evo_data_path - where EVO's persistent data lives.
 *
 * As an elfldr payload EVO runs unsandboxed and writes to /data/evoplayer/.
 * As the PPSA99039 app module (scripts/package-app.sh defines EVO_APP_MODULE)
 * the process is sandboxed: /data is ENOENT, only /app0 (RO) and /download0
 * (RW, because param.json sets downloadDataSize > 0) exist. So settings, the
 * recent/favorites DBs, the last-folder cfg, the Emby config and custom themes
 * all move under /download0/evoplayer/.
 *
 * See docs/evo-pro/phase-1b-app-module.md section 5 (Step B.1).
 *
 * EVO_DATA_DIR is a compile-time string literal (no trailing slash) so the
 * existing "#define FOO DIR \"/foo.cfg\"" sites stay compile-time constants.
 * evo_data_path() is the runtime join helper for paths built at runtime.
 */

#ifdef EVO_APP_MODULE
#define EVO_DATA_DIR "/download0/evoplayer"
#else
#define EVO_DATA_DIR "/data/evoplayer"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The data root, no trailing slash. Same value as EVO_DATA_DIR - a function
 * form so it can be logged / breadcrumbed without stringify games. */
const char *evo_data_dir(void);

/*
 * Join `leaf` onto the data root: evo_data_path("emby.conf") ->
 * "/download0/evoplayer/emby.conf". A leading '/' on `leaf` is ignored.
 *
 * Returns a pointer to a per-thread static buffer - use it immediately (as an
 * fopen()/open() argument, say); do not stash it or pass two results of this
 * call into the same expression. On overflow it returns evo_data_dir().
 */
const char *evo_data_path(const char *leaf);

#ifdef __cplusplus
}
#endif

#endif /* EVO_DATA_PATH_H */
