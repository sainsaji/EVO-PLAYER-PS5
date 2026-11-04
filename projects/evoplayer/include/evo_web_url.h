/*
 * What the Web section's address field turns typed text into (#237).
 * Pure; host-tested in tests/test_web_url.cpp.
 */
#ifndef EVO_WEB_URL_H
#define EVO_WEB_URL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    EVO_WEB_INPUT_EMPTY  = 0,  /* nothing typed: out is "" */
    EVO_WEB_INPUT_URL    = 1,  /* an address, with a scheme added if missing */
    EVO_WEB_INPUT_SEARCH = 2,  /* anything else: a DuckDuckGo search */
};

/*
 * "example.com" -> "https://example.com", "192.168.0.5:8096" ->
 * "http://192.168.0.5:8096" (a LAN server, most of them speak plain HTTP),
 * "https://x/y" stays, "cats and dogs" -> "https://duckduckgo.com/?q=cats+and+dogs".
 * Leading and trailing blanks are dropped. out is always NUL-terminated;
 * a result that does not fit is cut.
 */
int evo_web_normalize_input(const char *in, char *out, size_t cap);

/* The host of a URL, for a short label: "https://en.m.wikipedia.org/wiki/X"
 * -> "en.m.wikipedia.org". "" when there is none. */
void evo_web_url_host(const char *url, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
