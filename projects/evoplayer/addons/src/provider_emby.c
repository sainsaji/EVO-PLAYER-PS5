/*
 * provider_emby.c — Emby behind the evo_provider_t vtable.
 *
 * A thin vtable over the shared media-server client (addon_emby.c), which
 * provider_jellyfin.c uses too. Browsing is EVO's own screen
 * (rml/mediaserver.rml, bound to the host's data model); Emby's web client in
 * the system browser (#101) stays reachable as the "web version" from the
 * provider chooser.
 */
#include "evo_provider.h"
#include "addon_emby.h"

#define C ms_client(MS_EMBY)

static int  emby_p_init(void)          { return ms_load(C); }
static void emby_p_shutdown(void)      { }
static int  emby_p_configured(void)    { return ms_is_configured(C); }
static int  emby_p_needs_sign_in(void) { return ms_needs_sign_in(C); }

static int emby_p_suggest_user(void (*cb)(const char *, void *), void *ud)
{
    return ms_suggest_user(C, cb, ud);
}

static int emby_p_sign_in(const char *u, const char *p, evo_provider_auth_cb cb, void *ud)
{
    return ms_sign_in(C, u, p, cb, ud);
}

static int emby_p_list(const char *parent, int page, evo_provider_items_cb cb, void *ud)
{
    return ms_list_catalog(C, parent, page, cb, ud);
}

static int emby_p_search(const char *q, int page, evo_provider_items_cb cb, void *ud)
{
    return ms_search(C, q, page, cb, ud);
}

static int emby_p_resolve(const char *id, evo_provider_resolve_cb cb, void *ud)
{
    return ms_resolve(C, id, cb, ud);
}

static void emby_p_progress(const char *id, int64_t pos, int64_t dur, evo_provider_play_state_t st)
{
    ms_report(C, id, pos, dur, st);
}

static const char *emby_p_get_source(void)       { return ms_get_source(C); }
static int         emby_p_set_source(const char *v) { return ms_set_source(C, v); }

const evo_provider_t evo_provider_emby = {
    .id              = "emby",
    .name            = "Emby",
    .icon            = "icon_emby.png",
    .caps            = EVO_PROVIDER_CAP_CATALOG | EVO_PROVIDER_CAP_SEARCH |
                       EVO_PROVIDER_CAP_RESOLVE | EVO_PROVIDER_CAP_PROGRESS |
                       EVO_PROVIDER_CAP_CONFIG  | EVO_PROVIDER_CAP_WEBUI,
    .api_version     = EVO_PROVIDER_API_VERSION,
    .init            = emby_p_init,
    .shutdown        = emby_p_shutdown,
    .is_configured   = emby_p_configured,
    .list_catalog    = emby_p_list,
    .search          = emby_p_search,
    .resolve         = emby_p_resolve,
    .report_progress = emby_p_progress,
    .get_source      = emby_p_get_source,
    .set_source      = emby_p_set_source,
    .web_ui_url      = emby_p_get_source,
    .web_ui_path     = "/web/index.html",
    .needs_sign_in   = emby_p_needs_sign_in,
    .suggest_user    = emby_p_suggest_user,
    .sign_in         = emby_p_sign_in,
    .ui_embedded     = "mediaserver",
};
