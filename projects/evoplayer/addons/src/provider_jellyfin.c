/*
 * provider_jellyfin.c — Jellyfin behind the evo_provider_t vtable.
 *
 * The same shared media-server client as Emby (addon_emby.c), on its own
 * instance: no /emby path prefix, the `Authorization: MediaBrowser` header
 * Jellyfin 12 requires, and jellyfin.conf. Browsing is EVO's own screen
 * (rml/mediaserver.rml); Jellyfin's web client in the system browser (#101)
 * stays reachable as the "web version" from the provider chooser.
 */
#include "evo_provider.h"
#include "addon_emby.h"

#define C ms_client(MS_JELLYFIN)

static int  jf_init(void)          { return ms_load(C); }
static void jf_shutdown(void)      { }
static int  jf_configured(void)    { return ms_is_configured(C); }
static int  jf_needs_sign_in(void) { return ms_needs_sign_in(C); }
static int  jf_signed_in(void)     { return ms_is_signed_in(C); }
static void jf_sign_out(void)      { ms_sign_out(C); }

static int jf_suggest_user(void (*cb)(const char *, void *), void *ud)
{
    return ms_suggest_user(C, cb, ud);
}

static int jf_sign_in(const char *u, const char *p, evo_provider_auth_cb cb, void *ud)
{
    return ms_sign_in(C, u, p, cb, ud);
}

static int jf_qc_start(evo_provider_qc_code_cb cb, void *ud) { return ms_qc_start(C, cb, ud); }
static int jf_qc_poll(evo_provider_qc_cb cb, void *ud)        { return ms_qc_poll(C, cb, ud); }

static int jf_list(const char *parent, int page, evo_provider_items_cb cb, void *ud)
{
    return ms_list_catalog(C, parent, page, cb, ud);
}

static int jf_search(const char *q, int page, evo_provider_items_cb cb, void *ud)
{
    return ms_search(C, q, page, cb, ud);
}

static int jf_resolve(const char *id, evo_provider_resolve_cb cb, void *ud)
{
    return ms_resolve(C, id, cb, ud);
}

static void jf_progress(const char *id, int64_t pos, int64_t dur, evo_provider_play_state_t st)
{
    ms_report(C, id, pos, dur, st);
}

static void jf_chosen(const char *id, const evo_stream_choice_t *ch)
{
    ms_stream_chosen(C, id, ch);
}

static const char *jf_get_source(void)          { return ms_get_source(C); }
static int         jf_set_source(const char *v) { return ms_set_source(C, v); }

const evo_provider_t evo_provider_jellyfin = {
    .id              = "jellyfin",
    .name            = "Jellyfin",
    .icon            = "icon_emby.png",
    .caps            = EVO_PROVIDER_CAP_CATALOG | EVO_PROVIDER_CAP_SEARCH |
                       EVO_PROVIDER_CAP_RESOLVE | EVO_PROVIDER_CAP_PROGRESS |
                       EVO_PROVIDER_CAP_CONFIG  | EVO_PROVIDER_CAP_WEBUI |
                       EVO_PROVIDER_CAP_PICK,
    .api_version     = EVO_PROVIDER_API_VERSION,
    .init            = jf_init,
    .shutdown        = jf_shutdown,
    .is_configured   = jf_configured,
    .list_catalog    = jf_list,
    .search          = jf_search,
    .resolve         = jf_resolve,
    .report_progress = jf_progress,
    .stream_chosen   = jf_chosen,
    .get_source      = jf_get_source,
    .set_source      = jf_set_source,
    .web_ui_url      = jf_get_source,
    .web_ui_path     = "/web/index.html",
    .needs_sign_in   = jf_needs_sign_in,
    .suggest_user    = jf_suggest_user,
    .sign_in         = jf_sign_in,
    .qc_start        = jf_qc_start,
    .qc_poll         = jf_qc_poll,
    .ui_embedded     = "mediaserver",
    .is_signed_in    = jf_signed_in,
    .sign_out        = jf_sign_out,
};
