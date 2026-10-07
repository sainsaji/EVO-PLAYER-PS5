/* evo_error.c - see evo_error.h. */
#include "evo_error.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef EVO_APP_MODULE
#include "evo_boot_log.h"
#endif

#define EVO_ERROR_MAX_AGE_MS 30000

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char            g_msg[256];
static long long       g_at_ms;

static long long now_ms_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

void evo_error_set(const char *fmt, ...)
{
    char buf[sizeof g_msg];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_mu);
    memcpy(g_msg, buf, sizeof g_msg);
    g_at_ms = now_ms_mono();
    pthread_mutex_unlock(&g_mu);

#ifdef EVO_APP_MODULE
    evo_log_error("error: %s", buf);
#endif
}

static int copy_out(char *out, size_t cap, int clear)
{
    int have = 0;
    pthread_mutex_lock(&g_mu);
    if (g_msg[0] && now_ms_mono() - g_at_ms <= EVO_ERROR_MAX_AGE_MS) {
        if (out && cap) snprintf(out, cap, "%s", g_msg);
        have = 1;
    }
    if (clear || !have)
        g_msg[0] = 0;
    pthread_mutex_unlock(&g_mu);
    return have;
}

int  evo_error_take(char *out, size_t cap) { return copy_out(out, cap, 1); }
int  evo_error_peek(char *out, size_t cap) { return copy_out(out, cap, 0); }

void evo_error_clear(void)
{
    pthread_mutex_lock(&g_mu);
    g_msg[0] = 0;
    pthread_mutex_unlock(&g_mu);
}

void evo_error_url_host(const char *url, char *out, size_t cap)
{
    if (!out || !cap) return;
    out[0] = 0;
    if (!url) return;
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t end = strcspn(p, "/?#");
    const char *at = NULL;                      /* skip "user:pass@" */
    for (size_t i = 0; i < end; ++i)
        if (p[i] == '@') at = p + i;
    if (at) { end -= (size_t)(at + 1 - p); p = at + 1; }
    size_t n = end;
    const char *colon = memchr(p, ':', n);      /* drop the port */
    if (colon && p[0] != '[') n = (size_t)(colon - p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
}
