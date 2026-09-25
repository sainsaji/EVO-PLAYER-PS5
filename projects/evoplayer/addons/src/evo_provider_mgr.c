/*
 * evo_provider_mgr.c — the provider registry and the resolver chain (#90).
 *
 * A static table of compiled-in providers, their per-provider enable flags,
 * and the chain that turns an unplayable catalog link into a playable URL.
 *
 * There is no dynamic registration and no plugin loading. Providers are C
 * compiled into the app module; what is fetched at runtime is their markup,
 * never their code. Adding a provider means adding a line to PROVIDERS below
 * and a file to ADDON_SRCS in the Makefile - that is the whole extension
 * mechanism, and it is deliberately that small.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "evo_provider.h"
#include "evo_provider_log.h"
#include "evo_data_path.h"

/* ------------------------------------------------------------------------- */
/* The table                                                                 */
/* ------------------------------------------------------------------------- */
/*
 * Each provider TU exports one of these. They are extern rather than
 * registered at startup so the table is a link-time fact: a provider that
 * fails to compile fails the build instead of silently not appearing.
 */
extern const evo_provider_t evo_provider_iptv;
extern const evo_provider_t evo_provider_emby;
extern const evo_provider_t evo_provider_jellyfin;

/*
 * Emby is in the table unconditionally, even while EVO_ENABLE_EMBY is 0.
 *
 * That flag was always about the SCREENS, not the logic, and #90's "done when"
 * is explicit that the seam has to be proven against two providers with Emby's
 * UI still off. What keeps an unconfigured Emby out of the way is the same
 * thing that keeps an unconfigured IPTV out of the way: is_configured() is
 * false, so load_state() leaves it disabled and nothing calls its vtable.
 */
static const evo_provider_t *const PROVIDERS[] = {
    &evo_provider_iptv,
    &evo_provider_emby,
    &evo_provider_jellyfin,     /* #101: web UI only */
};

#define PROVIDER_COUNT ((int)(sizeof(PROVIDERS) / sizeof(PROVIDERS[0])))

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

static int g_initialised = 0;
static int g_registered  = 0;
/* Parallel to PROVIDERS. Not a field on evo_provider_t because that is const
 * static data owned by each provider's TU. */
static int g_enabled[PROVIDER_COUNT ? PROVIDER_COUNT : 1];

#define STATE_FILE "providers.conf"

/* ------------------------------------------------------------------------- */
/* Id validation                                                             */
/* ------------------------------------------------------------------------- */

int evo_provider_id_valid(const char *id)
{
    if (!id || !*id) return 0;
    size_t n = strlen(id);
    if (n >= EVO_PROVIDER_MAX_ID) return 0;
    for (size_t i = 0; i < n; ++i) {
        char c = id[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '-' || c == '_';
        if (!ok) return 0;
    }
    /* A leading '-' or '_' would make an awkward directory name and buys
     * nothing; a trailing one is the same. */
    if (id[0] == '-' || id[0] == '_') return 0;
    if (id[n - 1] == '-' || id[n - 1] == '_') return 0;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Vtable validation                                                         */
/* ------------------------------------------------------------------------- */
/*
 * A capability bit with a NULL slot behind it is a null call at the worst
 * possible moment - inside a screen the user just opened. Catch it at startup
 * instead, where it is a log line and a disabled provider rather than a crash.
 */
static int vtable_ok(const evo_provider_t *p, const char **why)
{
    if (!p)                        { *why = "null vtable";        return 0; }
    if (!evo_provider_id_valid(p->id)) { *why = "invalid id";      return 0; }
    if (!p->name || !*p->name)     { *why = "no name";            return 0; }
    if (p->api_version != EVO_PROVIDER_API_VERSION) {
                                     *why = "api_version";        return 0; }
    if (!p->init || !p->shutdown || !p->is_configured) {
                                     *why = "missing lifecycle";  return 0; }

    if ((p->caps & EVO_PROVIDER_CAP_AUTH)     && !p->auth) {
                                     *why = "CAP_AUTH no auth";   return 0; }
    if ((p->caps & EVO_PROVIDER_CAP_CATALOG)  && !p->list_catalog) {
                                     *why = "CAP_CATALOG no list_catalog"; return 0; }
    if ((p->caps & EVO_PROVIDER_CAP_SEARCH)   && !p->search) {
                                     *why = "CAP_SEARCH no search"; return 0; }
    if ((p->caps & EVO_PROVIDER_CAP_RESOLVE)  && !p->resolve) {
                                     *why = "CAP_RESOLVE no resolve"; return 0; }
    if ((p->caps & EVO_PROVIDER_CAP_PROGRESS) && !p->report_progress) {
                                     *why = "CAP_PROGRESS no report_progress"; return 0; }
    if ((p->caps & EVO_PROVIDER_CAP_UI)       && !p->ui_bundle_url) {
                                     *why = "CAP_UI no ui_bundle_url"; return 0; }
    if ((p->caps & EVO_PROVIDER_CAP_WEBUI)    && !p->web_ui_url) {
                                     *why = "CAP_WEBUI no web_ui_url"; return 0; }
    if ((p->caps & EVO_PROVIDER_CAP_CONFIG)  && (!p->get_source || !p->set_source)) {
                                     *why = "CAP_CONFIG no get/set_source"; return 0; }

    /* A provider that can do nothing at all is a table mistake. */
    if (p->caps == 0)              { *why = "no capabilities";    return 0; }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Persisted enable flags                                                    */
/* ------------------------------------------------------------------------- */
/*
 * One line per provider, "<id>=0|1". Follows emby_save_config's shape rather
 * than inventing a format: the store is a handful of key=value files and
 * staying consistent with that is worth more than a better format.
 *
 * A provider absent from the file defaults to enabled if it is configured and
 * disabled if it is not, so a fresh install shows nothing half-set-up.
 */
static void load_state(void)
{
    for (int i = 0; i < PROVIDER_COUNT; ++i)
        g_enabled[i] = PROVIDERS[i]->is_configured ? PROVIDERS[i]->is_configured() : 0;

    FILE *f = fopen(evo_data_path(STATE_FILE), "r");
    if (!f) return;

    char line[128];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        int   val = atoi(eq + 1);
        /* strip trailing whitespace from the key */
        size_t kl = strlen(key);
        while (kl && (key[kl - 1] == ' ' || key[kl - 1] == '\t')) key[--kl] = '\0';
        for (int i = 0; i < PROVIDER_COUNT; ++i) {
            if (strcmp(PROVIDERS[i]->id, key) == 0) {
                g_enabled[i] = val ? 1 : 0;
                break;
            }
        }
    }
    fclose(f);
}

int evo_provider_save_state(void)
{
    FILE *f = fopen(evo_data_path(STATE_FILE), "w");
    if (!f) return -1;
    for (int i = 0; i < PROVIDER_COUNT; ++i)
        fprintf(f, "%s=%d\n", PROVIDERS[i]->id, g_enabled[i] ? 1 : 0);
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

int evo_provider_mgr_init(void)
{
    if (g_initialised) return g_registered;

    g_registered = 0;
    for (int i = 0; i < PROVIDER_COUNT; ++i) {
        const char *why = "";
        if (!vtable_ok(PROVIDERS[i], &why)) {
            PROV_LOG("[%d] REFUSED (%s)", i, why);
            g_enabled[i] = 0;
            continue;
        }
        /* Duplicate ids would make two providers share a cache directory. */
        int dup = 0;
        for (int j = 0; j < i; ++j)
            if (strcmp(PROVIDERS[i]->id, PROVIDERS[j]->id) == 0) dup = 1;
        if (dup) {
            PROV_LOG("'%s' REFUSED (duplicate id)", PROVIDERS[i]->id);
            g_enabled[i] = 0;
            continue;
        }
        if (PROVIDERS[i]->init() != 0) {
            PROV_LOG("'%s' init FAILED", PROVIDERS[i]->id);
            g_enabled[i] = 0;
            continue;
        }
        g_registered++;
    }

    load_state();
    g_initialised = 1;

    /*
     * The success path has to say something. Without this a boot where
     * everything worked is indistinguishable from one where the table was
     * empty, and #90's hardware checklist asks for exactly these lines.
     */
    PROV_LOG("registry up: %d/%d registered", g_registered, PROVIDER_COUNT);
    for (int i = 0; i < PROVIDER_COUNT; ++i) {
        const evo_provider_t *p = PROVIDERS[i];
        PROV_LOG("  %-6s caps=0x%02x configured=%d enabled=%d",
                 p->id, (unsigned)p->caps,
                 p->is_configured ? p->is_configured() : 0, g_enabled[i]);
    }
    return g_registered;
}

void evo_provider_mgr_rebind(void)
{
    if (!g_initialised) { evo_provider_mgr_init(); return; }

    /*
     * init() again, deliberately. Each provider's init() is a config load and
     * nothing else - that is a documented part of the vtable contract, exactly
     * so this is safe to repeat when the data root changes underneath it.
     */
    for (int i = 0; i < PROVIDER_COUNT; ++i)
        if (PROVIDERS[i] && PROVIDERS[i]->init) PROVIDERS[i]->init();

    load_state();

    PROV_LOG("rebind after data-root change:");
    for (int i = 0; i < PROVIDER_COUNT; ++i)
        PROV_LOG("  %-6s configured=%d enabled=%d", PROVIDERS[i]->id,
                 PROVIDERS[i]->is_configured ? PROVIDERS[i]->is_configured() : 0,
                 g_enabled[i]);
}

void evo_provider_mgr_shutdown(void)
{
    if (!g_initialised) return;
    for (int i = 0; i < PROVIDER_COUNT; ++i)
        if (PROVIDERS[i] && PROVIDERS[i]->shutdown) PROVIDERS[i]->shutdown();
    g_initialised = 0;
}

int evo_provider_count(void) { return PROVIDER_COUNT; }

const evo_provider_t *evo_provider_at(int index)
{
    if (index < 0 || index >= PROVIDER_COUNT) return NULL;
    return PROVIDERS[index];
}

const evo_provider_t *evo_provider_find(const char *id)
{
    if (!id) return NULL;
    for (int i = 0; i < PROVIDER_COUNT; ++i)
        if (strcmp(PROVIDERS[i]->id, id) == 0) return PROVIDERS[i];
    return NULL;
}

static int index_of(const char *id)
{
    if (!id) return -1;
    for (int i = 0; i < PROVIDER_COUNT; ++i)
        if (strcmp(PROVIDERS[i]->id, id) == 0) return i;
    return -1;
}

int evo_provider_is_enabled(const char *id)
{
    int i = index_of(id);
    return (i < 0) ? 0 : g_enabled[i];
}

void evo_provider_set_enabled(const char *id, int enabled)
{
    int i = index_of(id);
    if (i < 0) return;
    g_enabled[i] = enabled ? 1 : 0;
    evo_provider_save_state();
}

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

int evo_provider_parse_web_source(const char *value, char *host, size_t host_cap,
                                  int *port, int *tls, int default_port)
{
    if (!value || !host || host_cap == 0 || !port || !tls) return -1;
    while (*value == ' ') value++;
    int https = 0;
    if (!strncmp(value, "https://", 8))     { https = 1; value += 8; }
    else if (!strncmp(value, "http://", 7)) { value += 7; }
    size_t n = strcspn(value, ":/?# ");
    if (n == 0 || n >= host_cap) return -1;
    for (size_t i = 0; i < n; ++i) {                    /* a hostname or an IPv4 */
        char c = value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return -1;
    }
    int p = https ? 443 : default_port;
    if (value[n] == ':') {
        p = 0;
        for (const char *c = value + n + 1; *c >= '0' && *c <= '9'; ++c)
            p = p * 10 + (*c - '0');
        if (p <= 0 || p > 65535) return -1;
    }
    memcpy(host, value, n);
    host[n] = 0;
    *port = p;
    *tls = https;
    return 0;
}

void evo_provider_item_clear(evo_provider_item_t *it)
{
    if (!it) return;
    memset(it, 0, sizeof *it);
    it->kind = EVO_MEDIA_VIDEO;
}

void evo_provider_stream_choice_clear(evo_stream_choice_t *c)
{
    if (!c) return;
    memset(c, 0, sizeof *c);
}

int evo_provider_url_escape(const char *in, char *out, size_t out_sz)
{
    static const char HEX[] = "0123456789ABCDEF";
    if (!in || !out || out_sz == 0) return -1;

    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
        unsigned char c = *p;
        /* RFC 3986 unreserved. Everything else is escaped, including '/' and
         * ':' - this escapes a query VALUE, where a bare '/' is legal but a
         * bare '&' or '=' changes the meaning of the request. */
        int unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') ||
                         c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            if (o + 1 >= out_sz) return -2;
            out[o++] = (char)c;
        } else {
            if (o + 3 >= out_sz) return -2;
            out[o++] = '%';
            out[o++] = HEX[(c >> 4) & 0xF];
            out[o++] = HEX[c & 0xF];
        }
    }
    out[o] = '\0';
    return (int)o;
}

/* ------------------------------------------------------------------------- */
/* Resolver chain                                                            */
/* ------------------------------------------------------------------------- */
/*
 * Three states, driven by callbacks rather than a loop, because every step is
 * asynchronous and the whole thing has to unwind through evo_net_poll():
 *
 *   1. resolve the item with its own provider
 *   2. if the best choice needs a resolver, hand its url to the next enabled
 *      RESOLVE-capable provider (skipping the originating one)
 *   3. first playable answer wins; running out of resolvers is a failure
 *
 * The chain state is heap-allocated and freed on exactly one path: the
 * terminal `finish()`. Every early return either has not allocated yet or
 * goes through finish().
 */
typedef struct chain {
    char origin[EVO_PROVIDER_MAX_ID];
    char pending_url[EVO_PROVIDER_MAX_URL];
    int  next_resolver;            /* index into PROVIDERS to try next */
    int  hops;                     /* guards a resolver that resolves to itself */
    evo_provider_resolve_cb cb;
    void *ud;
} chain_t;

#define CHAIN_MAX_HOPS 4

static void chain_step(chain_t *st);

static void finish(chain_t *st, int ok,
                   const evo_stream_choice_t *choices, int count)
{
    evo_provider_resolve_cb cb = st->cb;
    void *ud = st->ud;
    free(st);
    if (cb) cb(ok, choices, count, ud);
}

/* Pick the choice the chain should act on: the first playable one if there is
 * one at all, otherwise the first that needs resolving. */
static int pick(const evo_stream_choice_t *c, int n, int *out_needs)
{
    int first_needing = -1;
    for (int i = 0; i < n; ++i) {
        if (!c[i].needs_resolver) { *out_needs = 0; return i; }
        if (first_needing < 0) first_needing = i;
    }
    *out_needs = 1;
    return first_needing;
}

static void on_resolved(int ok, const evo_stream_choice_t *choices, int count,
                        void *ud)
{
    chain_t *st = (chain_t *)ud;

    if (!ok || count <= 0) {
        /* This link in the chain had nothing. Try the next resolver rather
         * than giving up: a debrid service that does not hold this particular
         * magnet is a normal outcome, not an error. */
        if (st->pending_url[0]) { chain_step(st); return; }
        finish(st, 0, NULL, 0);
        return;
    }

    int needs = 0;
    int idx = pick(choices, count, &needs);
    if (idx < 0) { finish(st, 0, NULL, 0); return; }

    if (!needs) {
        /* Playable. Hand back the whole array so the caller can still offer a
         * quality picker - the chain only decided that it is done. */
        finish(st, 1, choices, count);
        return;
    }

    if (++st->hops > CHAIN_MAX_HOPS) { finish(st, 0, NULL, 0); return; }

    snprintf(st->pending_url, sizeof st->pending_url, "%s", choices[idx].url);
    chain_step(st);
}

static void chain_step(chain_t *st)
{
    /* Find the next enabled RESOLVE-capable provider that is not the one the
     * item came from - asking a catalog to resolve its own unresolvable link
     * again just loops. */
    while (st->next_resolver < PROVIDER_COUNT) {
        int i = st->next_resolver++;
        const evo_provider_t *p = PROVIDERS[i];
        if (!g_enabled[i]) continue;
        if (!(p->caps & EVO_PROVIDER_CAP_RESOLVE)) continue;
        if (strcmp(p->id, st->origin) == 0) continue;

        if (p->resolve(st->pending_url, on_resolved, st) == 0)
            return;                /* its callback drives the next step */
        /* Not accepted - try the one after it. */
    }
    finish(st, 0, NULL, 0);
}

int evo_provider_resolve_chain(const char *provider_id, const char *item_id,
                               evo_provider_resolve_cb cb, void *ud)
{
    const evo_provider_t *p = evo_provider_find(provider_id);
    if (!p || !item_id || !*item_id) return -1;
    if (!(p->caps & EVO_PROVIDER_CAP_RESOLVE)) return -1;
    if (!evo_provider_is_enabled(provider_id)) return -1;

    chain_t *st = (chain_t *)calloc(1, sizeof *st);
    if (!st) return -2;
    snprintf(st->origin, sizeof st->origin, "%s", provider_id);
    st->cb = cb;
    st->ud = ud;

    if (p->resolve(item_id, on_resolved, st) != 0) {
        free(st);
        return -3;
    }
    return 0;
}
