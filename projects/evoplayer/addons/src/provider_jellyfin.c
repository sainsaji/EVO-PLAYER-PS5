/*
 * provider_jellyfin.c — Jellyfin as a web-UI provider (#101).
 *
 * Nothing but an address. The UI is Jellyfin's own web client, opened in the
 * system browser beside the rail by the provider screen (evo_webui.c
 * reverse-proxies it on loopback and reroutes its player to EVO's), so this
 * provider has no catalog, no auth and no resolve of its own: the user signs
 * in and browses inside Jellyfin, and a Play lands in EVO.
 *
 * The source string is the server's address, http[s]://<host>:<port>,
 * persisted as jellyfin.conf in the data store in emby.conf's key=value shape.
 */
#include <stdio.h>
#include <string.h>

#include "evo_provider.h"
#include "evo_data_path.h"

#define JELLYFIN_CONF         "jellyfin.conf"
#define JELLYFIN_DEFAULT_PORT 8096

static char g_host[128];
static int  g_port = JELLYFIN_DEFAULT_PORT;
static int  g_https = 0;
static char g_source[176];

static int jf_init(void)
{
    g_host[0] = '\0';
    g_port = JELLYFIN_DEFAULT_PORT;
    g_https = 0;

    FILE *f = fopen(evo_data_path(JELLYFIN_CONF), "r");
    if (!f) return 0;                   /* not set up yet - not an error */
    char line[160];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "host=", 5) == 0) {
            snprintf(g_host, sizeof g_host, "%s", line + 5);
        } else if (strncmp(line, "port=", 5) == 0) {
            int p = 0;
            if (sscanf(line + 5, "%d", &p) == 1 && p > 0 && p <= 65535) g_port = p;
        } else if (strncmp(line, "https=", 6) == 0) {
            g_https = line[6] == '1';
        }
    }
    fclose(f);
    return 0;
}

static void jf_shutdown(void) { /* holds no resources */ }

static int jf_is_configured(void) { return g_host[0] ? 1 : 0; }

static const char *jf_get_source(void)
{
    if (!g_host[0]) return "";
    snprintf(g_source, sizeof g_source, "%s://%s:%d", g_https ? "https" : "http",
             g_host, g_port);
    return g_source;
}

static int jf_set_source(const char *value)
{
    char host[128];
    int port = JELLYFIN_DEFAULT_PORT, tls = 0;
    if (evo_provider_parse_web_source(value, host, sizeof host, &port, &tls,
                                      JELLYFIN_DEFAULT_PORT) != 0)
        return -1;

    FILE *f = fopen(evo_data_path(JELLYFIN_CONF), "w");
    if (!f) return -1;
    fprintf(f, "host=%s\nport=%d\nhttps=%d\n", host, port, tls);
    fclose(f);

    snprintf(g_host, sizeof g_host, "%s", host);
    g_port = port;
    g_https = tls;
    return 0;
}

static const char *jf_web_ui_url(void) { return jf_get_source(); }

const evo_provider_t evo_provider_jellyfin = {
    .id            = "jellyfin",
    .name          = "Jellyfin",
    .icon          = "icon_emby.png",
    .caps          = EVO_PROVIDER_CAP_CONFIG | EVO_PROVIDER_CAP_WEBUI,
    .api_version   = EVO_PROVIDER_API_VERSION,
    .init          = jf_init,
    .shutdown      = jf_shutdown,
    .is_configured = jf_is_configured,
    .get_source    = jf_get_source,
    .set_source    = jf_set_source,
    .web_ui_url    = jf_web_ui_url,
    .web_ui_path   = "/web/index.html",
};
