/*
 * provider_nuvio.c — Nuvio as a web-UI provider.
 *
 * Nuvio's TV web app (NuvioTVSmart, the Tizen/webOS build) is a Stremio-addon
 * client: catalogs from addons such as Cinemeta, streams from addons such as
 * Torrentio or AIOStreams, resolved through TorBox / Real-Debrid. EVO opens it
 * in the system browser beside the rail like Emby and Jellyfin; evo_webui.c's
 * "nuvio" hook profile catches its player (<video id="videoPlayer"> inside
 * #player) and hands the stream to EVO's player. So debrid services arrive
 * through Nuvio's addons, with no debrid code in EVO.
 *
 * The source string is the site's address: a self-hosted build
 * (http://<host>:<port>, e.g. the nuvio-test container) or the hosted app
 * (https://web.nuvioapp.space). Persisted as nuvio.conf.
 */
#include <stdio.h>
#include <string.h>

#include "evo_provider.h"
#include "evo_data_path.h"

#define NUVIO_CONF         "nuvio.conf"
#define NUVIO_DEFAULT_PORT 80

static char g_host[128];
static int  g_port = NUVIO_DEFAULT_PORT;
static int  g_https = 0;
static char g_source[176];

static int nv_init(void)
{
    g_host[0] = '\0';
    g_port = NUVIO_DEFAULT_PORT;
    g_https = 0;

    FILE *f = fopen(evo_data_path(NUVIO_CONF), "r");
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

static void nv_shutdown(void) { /* holds no resources */ }

static int nv_is_configured(void) { return g_host[0] ? 1 : 0; }

static const char *nv_get_source(void)
{
    if (!g_host[0]) return "";
    snprintf(g_source, sizeof g_source, "%s://%s:%d", g_https ? "https" : "http",
             g_host, g_port);
    return g_source;
}

static int nv_set_source(const char *value)
{
    char host[128];
    int port = NUVIO_DEFAULT_PORT, tls = 0;
    if (evo_provider_parse_web_source(value, host, sizeof host, &port, &tls,
                                      NUVIO_DEFAULT_PORT) != 0)
        return -1;

    FILE *f = fopen(evo_data_path(NUVIO_CONF), "w");
    if (!f) return -1;
    fprintf(f, "host=%s\nport=%d\nhttps=%d\n", host, port, tls);
    fclose(f);

    snprintf(g_host, sizeof g_host, "%s", host);
    g_port = port;
    g_https = tls;
    return 0;
}

static const char *nv_web_ui_url(void) { return nv_get_source(); }

const evo_provider_t evo_provider_nuvio = {
    .id            = "nuvio",
    .name          = "Nuvio",
    .icon          = "icon_emby.png",
    .caps          = EVO_PROVIDER_CAP_CONFIG | EVO_PROVIDER_CAP_WEBUI,
    .api_version   = EVO_PROVIDER_API_VERSION,
    .init          = nv_init,
    .shutdown      = nv_shutdown,
    .is_configured = nv_is_configured,
    .get_source    = nv_get_source,
    .set_source    = nv_set_source,
    .web_ui_url    = nv_web_ui_url,
    .web_ui_path   = "/",
    .web_ui_hook   = "nuvio",
};
