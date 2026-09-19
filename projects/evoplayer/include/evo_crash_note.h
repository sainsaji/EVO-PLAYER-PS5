/*
 * evo_crash_note — remember what we were doing when the process died.
 *
 * Some work cannot be made safe from inside this process. Thumbnail
 * extraction runs a media file through libavcodec's software decoders, and a
 * stream that makes one of them fail an internal allocation walks into its own
 * error path and dereferences null: SIGSEGV, no return value to check, nothing
 * to catch. `EVO_TEST_hevc8_4k.mp4` does exactly that to the HEVC decoder,
 * while playing back perfectly on the hardware decoder.
 *
 * That alone would be survivable - one file, no poster. What is not survivable
 * is the loop: the browser re-derives posters for the visible page on every
 * launch, so the same file takes the process down again the moment the user
 * scrolls back to it, forever.
 *
 * So the crash handler leaves a note. The extractor records the path it is
 * about to work on (a memcpy into a static buffer - no I/O, nothing in the
 * steady-state path), and if the process dies while that path is set, the
 * handler appends it to a quarantine file. On the next launch the extractor
 * reads the quarantine and refuses those paths, so the browser comes back and
 * the only thing lost is one poster.
 *
 * Everything the handler touches has to be async-signal-safe, hence the raw
 * open/write/close and the path resolved up front rather than inside it.
 */
#ifndef EVO_CRASH_NOTE_H
#define EVO_CRASH_NOTE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve and cache the quarantine file's path. Call once, after the data root
 * is known and before any extraction. Safe to call again; later calls are
 * ignored so the signal handler always sees a stable buffer. */
void evo_crash_note_init(void);

/* The quarantine file, or NULL before init. */
const char *evo_crash_note_path(void);

/* Mark work as in flight. `path` NULL or empty clears the mark. Copies at most
 * 511 bytes. No allocation, no I/O. */
void evo_crash_note_set(const char *path);

/* Append the in-flight mark, if any, to the quarantine file. Called only from
 * the crash handler: raw syscalls, no stdio, no allocation. */
void evo_crash_note_commit(void);

/* Whether `path` is quarantined. Reads the file on first call and caches it. */
int evo_crash_note_is_quarantined(const char *path);

/* Forget every quarantined path (deletes the file and drops the cache), so a
 * file can be retried after the underlying bug is fixed. */
void evo_crash_note_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* EVO_CRASH_NOTE_H */
