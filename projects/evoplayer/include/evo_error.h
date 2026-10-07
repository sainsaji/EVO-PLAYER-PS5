/*
 * evo_error.h - the reason the last thing failed, in words the person at the
 * TV can act on.
 *
 * Screens used to say "Could not open the stream" or "Could not load the
 * catalog" and nothing else, so a wrong link, a refused login, a dead server and
 * a bad file all looked the same, and a bug report could not tell them apart
 * either (hardware, 2026-10: an IPTV link that was a playlist, not a stream,
 * failed 11 times in a row with the same generic toast).
 *
 * The code that sees the failure records the reason here (evo_error_set); the
 * screen that reports it takes it (evo_error_take) and shows it instead of its
 * own generic text, keeping that text as the fallback. One slot, newest wins,
 * and a reason older than 30 s is dropped so an old failure is never shown for a
 * new one. Every set also lands in evo.log as an ERROR line.
 *
 * Never put a full URL in a message: it carries the login. Use
 * evo_error_url_host().
 */
#ifndef EVO_ERROR_H
#define EVO_ERROR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void evo_error_set(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/* Copy the pending reason into out and clear it. 1 if there was one (set in the
 * last 30 s), else 0 and out is untouched. Safe from any thread. */
int evo_error_take(char *out, size_t cap);

/* As evo_error_take, but leave it pending. */
int evo_error_peek(char *out, size_t cap);

void evo_error_clear(void);

/* "https://user:pass@host.example:8080/path?x=1" -> "host.example". Never leaks
 * credentials, path or query. Empty string when there is no host. */
void evo_error_url_host(const char *url, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* EVO_ERROR_H */
