/*
 * evo_net.h — Non-blocking HTTP/REST client and network task queue for EVO Player.
 *
 * Implements a lightweight, thread-safe HTTP/REST client over standard BSD sockets.
 * All asynchronous requests execute on a background worker thread and pump their
 * completion callbacks on the main UI thread via evo_net_poll(), preventing network
 * latency from dropping frames on the 60fps UI loop.
 */
#ifndef EVO_NET_H
#define EVO_NET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*evo_net_cb)(int success, int status_code, const char *body, size_t body_len, void *user_data);

/* Initialize background network worker and request queue. */
int  evo_net_init(void);

/* Shutdown background network worker and clean up. */
void evo_net_shutdown(void);

/*
 * Pump completed network callbacks on the calling (main/UI) thread.
 * Call this once per frame in the main render/event loop.
 */
void evo_net_poll(void);

/*
 * Asynchronous HTTP/REST request. Returns 0 on queued, negative on error.
 * Callback is invoked on the main thread during evo_net_poll().
 */
int  evo_net_request_async(const char *method,
                           const char *url,
                           const char *post_data,
                           const char **headers,
                           int header_count,
                           evo_net_cb callback,
                           void *user_data);

/*
 * Synchronous HTTP GET request. Blocks calling thread.
 * Caller must free(*out_body) if allocated.
 */
int  evo_net_http_get_sync(const char *url,
                          const char **headers,
                          int header_count,
                          char **out_body,
                          size_t *out_len,
                          int *out_status);

/*
 * Synchronous HTTP POST request with JSON or form payload. Blocks calling thread.
 * Caller must free(*out_body) if allocated.
 */
int  evo_net_http_post_sync(const char *url,
                           const char *post_data,
                           const char **headers,
                           int header_count,
                           char **out_body,
                           size_t *out_len,
                           int *out_status);

#ifdef __cplusplus
}
#endif

#endif /* EVO_NET_H */
