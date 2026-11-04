/*
 * evo_online_subs - subtitles from Stremio addons. See evo_online_subs.h.
 */
#include "evo_online_subs.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "evo_net.h"
#include "evo_data_path.h"
#include "evo_langpref.h"
#include "evo_langname.h"

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char       g_key[512];
static evo_osub_t g_subs[EVO_OSUB_MAX];
static int        g_count;
static int        g_pending;
static int        g_active = -1;
static char       g_kept_path[512];   /* the active online subtitle, copied aside */
static char       g_kept_label[96];

/* picker download state */
static int  g_fetch_busy;
static int  g_fetch_index = -1;
static int  g_fetch_done;          /* 1 ok, -1 failed, 0 nothing to take */
static char g_fetch_path[512];
static char g_fetch_url[EVO_OSUB_URL];

static int key_is(const char *k)
{
    return k && !strcmp(k, g_key);
}

void evo_osubs_begin(const char *item_key)
{
    pthread_mutex_lock(&g_mu);
    snprintf(g_key, sizeof g_key, "%s", item_key ? item_key : "");
    g_count = 0;
    g_pending = 0;
    g_active = -1;
    g_kept_path[0] = 0;
    g_kept_label[0] = 0;
    pthread_mutex_unlock(&g_mu);
}

void evo_osubs_remember(const char *path, const char *label)
{
    char keep[512];
    snprintf(keep, sizeof keep, "%s", evo_data_path("online_sub_active.srt"));
    int ok = 0;
    if (path && strcmp(path, keep)) {
        FILE *in = fopen(path, "rb");
        if (in) {
            FILE *out = fopen(keep, "wb");
            if (out) {
                char buf[8192];
                size_t r;
                ok = 1;
                while ((r = fread(buf, 1, sizeof buf, in)) > 0)
                    if (fwrite(buf, 1, r, out) != r) { ok = 0; break; }
                fclose(out);
            }
            fclose(in);
        }
    } else if (path) {
        ok = 1;
    }
    pthread_mutex_lock(&g_mu);
    snprintf(g_kept_path, sizeof g_kept_path, "%s", ok ? keep : "");
    snprintf(g_kept_label, sizeof g_kept_label, "%s", label ? label : "");
    pthread_mutex_unlock(&g_mu);
}

int evo_osubs_remembered(char *path, int path_cap, char *label, int label_cap)
{
    pthread_mutex_lock(&g_mu);
    int ok = g_kept_path[0] != 0;
    if (ok && path && path_cap > 0)
        snprintf(path, (size_t)path_cap, "%s", g_kept_path);
    if (ok && label && label_cap > 0)
        snprintf(label, (size_t)label_cap, "%s", g_kept_label);
    pthread_mutex_unlock(&g_mu);
    return ok;
}

void evo_osubs_add(const char *item_key, const char *url, const char *lang_tag,
                   const char *source)
{
    if (!url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)))
        return;
    if (strlen(url) >= EVO_OSUB_URL)
        return;
    pthread_mutex_lock(&g_mu);
    if (key_is(item_key) && g_count < EVO_OSUB_MAX) {
        int dup = 0;
        for (int i = 0; i < g_count && !dup; ++i)
            dup = !strcmp(g_subs[i].url, url);
        if (!dup) {
            evo_osub_t *s = &g_subs[g_count++];
            memset(s, 0, sizeof *s);
            snprintf(s->url, sizeof s->url, "%s", url);
            snprintf(s->lang_tag, sizeof s->lang_tag, "%s", lang_tag ? lang_tag : "");
            if (!evo_lang_normalize(lang_tag, s->lang))
                s->lang[0] = 0;
            snprintf(s->source, sizeof s->source, "%s", source && source[0] ? source : "Addon");
        }
    }
    pthread_mutex_unlock(&g_mu);
}

void evo_osubs_pending(const char *item_key, int delta)
{
    pthread_mutex_lock(&g_mu);
    if (key_is(item_key)) {
        g_pending += delta;
        if (g_pending < 0)
            g_pending = 0;
    }
    pthread_mutex_unlock(&g_mu);
}

int evo_osubs_wait(const char *item_key, int timeout_ms)
{
    for (int waited = 0;; waited += 50) {
        pthread_mutex_lock(&g_mu);
        const int mine = key_is(item_key);
        const int busy = mine && g_pending > 0;
        const int n = mine ? g_count : 0;
        pthread_mutex_unlock(&g_mu);
        if (!busy || waited >= timeout_ms)
            return n;
        usleep(50 * 1000);
    }
}

int evo_osubs_count(void)
{
    pthread_mutex_lock(&g_mu);
    int n = g_count;
    pthread_mutex_unlock(&g_mu);
    return n;
}

int evo_osubs_get(int i, evo_osub_t *out)
{
    int ok = 0;
    pthread_mutex_lock(&g_mu);
    if (i >= 0 && i < g_count && out) {
        *out = g_subs[i];
        ok = 1;
    }
    pthread_mutex_unlock(&g_mu);
    return ok;
}

void evo_osubs_key(char *out, int cap)
{
    if (!out || cap <= 0)
        return;
    pthread_mutex_lock(&g_mu);
    snprintf(out, (size_t)cap, "%s", g_key);
    pthread_mutex_unlock(&g_mu);
}

int evo_osubs_best(int allow_fallback, int *rank_out)
{
    int best = -1, best_rank = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_count; ++i) {
        const int r = evo_langpref_rank(g_subs[i].lang[0] ? g_subs[i].lang : g_subs[i].lang_tag, 's');
        if (r > best_rank) {          /* first one of the best rank: addon order */
            best_rank = r;
            best = i;
        }
    }
    pthread_mutex_unlock(&g_mu);
    if (best_rank < 2 && !allow_fallback)
        best = -1;
    if (best < 0)
        best_rank = 0;
    if (rank_out)
        *rank_out = best_rank;
    return best;
}

void evo_osubs_set_active(int i)
{
    pthread_mutex_lock(&g_mu);
    g_active = i;
    pthread_mutex_unlock(&g_mu);
}

int evo_osubs_active(void)
{
    pthread_mutex_lock(&g_mu);
    int a = g_active;
    pthread_mutex_unlock(&g_mu);
    return a;
}

void evo_osubs_label(int i, char *out, int cap)
{
    if (!out || cap <= 0)
        return;
    evo_osub_t s;
    if (!evo_osubs_get(i, &s)) {
        snprintf(out, (size_t)cap, "ONLINE");
        return;
    }
    const char *name = evo_language_native_name(s.lang[0] ? s.lang : s.lang_tag);
    snprintf(out, (size_t)cap, "%s", name ? name : (s.lang_tag[0] ? s.lang_tag : "Unknown language"));
}

/* Download `url` to `path`. 0 on success. */
static int fetch_to(const char *url, const char *path)
{
    char *body = NULL;
    size_t len = 0;
    int status = 0;
    const char *h[1] = { "Accept: */*" };
    int rc = evo_net_http_get_sync(url, h, 1, &body, &len, &status);
    int ok = (rc == 0 && body && len > 16 && status == 200);
    if (ok) {
        FILE *f = fopen(path, "wb");
        if (f) {
            ok = fwrite(body, 1, len, f) == len;
            fclose(f);
        } else {
            ok = 0;
        }
    }
    free(body);
    return ok ? 0 : -1;
}

const char *evo_osubs_fetch_sync(int i)
{
    static _Thread_local char path[512];
    evo_osub_t s;
    if (!evo_osubs_get(i, &s))
        return NULL;
    snprintf(path, sizeof path, "%s", evo_data_path("online_sub.srt"));
    return fetch_to(s.url, path) == 0 ? path : NULL;
}

static void *fetch_worker(void *arg)
{
    (void)arg;
    char url[EVO_OSUB_URL], path[512];
    pthread_mutex_lock(&g_mu);
    snprintf(url, sizeof url, "%s", g_fetch_url);
    pthread_mutex_unlock(&g_mu);
    snprintf(path, sizeof path, "%s", evo_data_path("online_sub_pick.srt"));
    const int rc = fetch_to(url, path);
    pthread_mutex_lock(&g_mu);
    snprintf(g_fetch_path, sizeof g_fetch_path, "%s", path);
    g_fetch_done = rc == 0 ? 1 : -1;
    g_fetch_busy = 0;
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

int evo_osubs_fetch_async(int i)
{
    evo_osub_t s;
    if (!evo_osubs_get(i, &s))
        return -1;
    pthread_mutex_lock(&g_mu);
    if (g_fetch_busy) {
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    g_fetch_busy = 1;
    g_fetch_done = 0;
    g_fetch_index = i;
    snprintf(g_fetch_url, sizeof g_fetch_url, "%s", s.url);
    pthread_mutex_unlock(&g_mu);

    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&th, &attr, fetch_worker, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        pthread_mutex_lock(&g_mu);
        g_fetch_busy = 0;
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    return 0;
}

int evo_osubs_fetching(void)
{
    pthread_mutex_lock(&g_mu);
    int b = g_fetch_busy;
    pthread_mutex_unlock(&g_mu);
    return b;
}

const char *evo_osubs_take_ready(int *index_out, int *failed_out)
{
    static char path[512];
    const char *ret = NULL;
    pthread_mutex_lock(&g_mu);
    if (failed_out)
        *failed_out = 0;
    if (g_fetch_done == 1) {
        snprintf(path, sizeof path, "%s", g_fetch_path);
        ret = path;
        if (index_out)
            *index_out = g_fetch_index;
    } else if (g_fetch_done == -1 && failed_out) {
        *failed_out = 1;
        if (index_out)
            *index_out = g_fetch_index;
    }
    g_fetch_done = 0;
    pthread_mutex_unlock(&g_mu);
    return ret;
}
