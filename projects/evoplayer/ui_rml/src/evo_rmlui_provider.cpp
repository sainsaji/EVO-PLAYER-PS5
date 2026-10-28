/*
 * evo_rmlui_provider.cpp — render a provider's own document (#90).
 *
 * See evo_rmlui_provider.h for what this is and why it is separate from
 * EvoRmlApp. This file is the first use of RmlUi data binding and of RmlUi's
 * own focus navigation anywhere in the codebase; EVO's existing screens are
 * untouched and keep working exactly as before alongside it.
 */
#include <chrono>
#include "evo_rmlui_provider.h"
#include "evo_rmlui_app.h"
#include "evo_rmlui_render_bridge.h"
#include "evo_rmlui_bundle.h"
#if defined(EVO_AGC_DEVICE)
#include "evo_hui.h"
#endif
#include <algorithm>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Types.h>

#include <cctype>
#include <cstdio>
#include <cstring>

extern "C" {
/* PROV_LOG: evo_bt on the device (so provider lines actually reach
 * /mnt/usb0/evo.log), plain stderr on the host. See evo_provider_log.h for why
 * this is not fprintf(stderr, ...). */
#include "evo_provider_log.h"
#include "evo_net.h"   /* evo_failure_reason */

__attribute__((weak)) const char *provider_iptv_epg_status(void);
__attribute__((weak)) int provider_iptv_epg_needs_setup(void);
__attribute__((weak)) void toast(const char *title, const char *msg);   /* ui/evo_keyboard.c */
}

/* ------------------------------------------------------------------------- */
/* Pending selection                                                         */
/* ------------------------------------------------------------------------- */
/*
 * The activated item is handed to the player through a one-slot mailbox rather
 * than by calling into PlaybackController from inside an Rml event handler.
 * Starting playback tears down and rebuilds a great deal of state, including
 * the VideoOut configuration; doing that from underneath Context::Update(),
 * which is still walking the element tree that raised the event, is the shape
 * of bug that took two console cycles to find in #32.
 */
static evo_provider_selection_t g_selection;
static bool g_selection_pending = false;
static bool g_resume_enabled = true;      /* the Resume playback setting, for the detail page's Restart button */

/*
 * The data model name the embedded fallback skin binds to.
 *
 * A constant, because assets/rml/provider_fallback.rml is authored once and
 * serves every provider - it cannot know a provider's own model name. A
 * downloaded bundle declares its own in manifest.json instead.
 */
static const char* const kFallbackModelName = "provider";

extern "C" int evo_rmlui_provider_take_selection(evo_provider_selection_t *out)
{
    if (!g_selection_pending || !out) return 0;
    *out = g_selection;
    g_selection_pending = false;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static Rml::String format_duration(int64_t secs)
{
    if (secs <= 0) return Rml::String();
    long long h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
    char buf[32];
    if (h > 0) snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", h, m, s);
    else       snprintf(buf, sizeof buf, "%lld:%02lld", m, s);
    return Rml::String(buf);
}

EvoRmlProviderHost& EvoRmlProviderHost::Instance()
{
    /* Never destroyed, same reasoning as EvoRmlApp::Instance(): its teardown
     * is explicit (Close()), and letting it unwind at exit would run Rml
     * teardown after Rml::Shutdown(). */
    static EvoRmlProviderHost* p = new EvoRmlProviderHost();
    return *p;
}

void EvoRmlProviderHost::SetStatus(const std::string& msg, bool error)
{
    m_model.status = Rml::String(msg.c_str());
    m_model.has_error = error;
    if (m_model_handle) {
        m_model_handle.DirtyVariable("status");
        m_model_handle.DirtyVariable("has_error");
    }
    m_dirty = true;
}

void EvoRmlProviderHost::SetLoading(bool loading, const std::string& status)
{
    m_model.loading = loading;
    if (!loading) {
        m_model.tuning = false;
    }
    if (m_model_handle) {
        m_model_handle.DirtyVariable("loading");
        m_model_handle.DirtyVariable("tuning");
    }
    if (!status.empty()) {
        m_model.status = Rml::String(status.c_str());
        m_model.has_error = false;
        if (m_model_handle) {
            m_model_handle.DirtyVariable("status");
            m_model_handle.DirtyVariable("has_error");
        }
    } else if (!loading && !m_model.has_error) {
        m_model.status = "";
        if (m_model_handle) {
            m_model_handle.DirtyVariable("status");
        }
    }
    m_dirty = true;
}

void EvoRmlProviderHost::SetTuning(bool tuning)
{
    m_model.tuning = tuning;
    if (tuning) {
        m_model.loading = true;
    }
    if (m_model_handle) {
        m_model_handle.DirtyVariable("tuning");
        m_model_handle.DirtyVariable("loading");
    }
    m_dirty = true;
}

/* ------------------------------------------------------------------------- */
/* Data model                                                                */
/* ------------------------------------------------------------------------- */

bool EvoRmlProviderHost::RegisterDataModel()
{
    Rml::DataModelConstructor c = m_context->CreateDataModel(m_model_name);
    if (!c) {
        PROV_LOG("CreateDataModel('%s') FAILED", m_model_name.c_str());
        return false;
    }

    /* The row struct has to be registered before the array that holds it. */
    if (auto row = c.RegisterStruct<EvoProviderRow>()) {
        row.RegisterMember("id",        &EvoProviderRow::id);
        row.RegisterMember("title",     &EvoProviderRow::title);
        row.RegisterMember("subtitle",  &EvoProviderRow::subtitle);
        row.RegisterMember("overview",  &EvoProviderRow::overview);
        row.RegisterMember("art",       &EvoProviderRow::art);
        row.RegisterMember("now",       &EvoProviderRow::now);
        row.RegisterMember("next",      &EvoProviderRow::next);
        row.RegisterMember("duration",  &EvoProviderRow::duration);
        row.RegisterMember("initial",   &EvoProviderRow::initial);
        row.RegisterMember("is_folder", &EvoProviderRow::is_folder);
        row.RegisterMember("is_live",   &EvoProviderRow::is_live);
        row.RegisterMember("index",     &EvoProviderRow::index);
        row.RegisterMember("progress",  &EvoProviderRow::progress);
        row.RegisterMember("progress_w", &EvoProviderRow::progress_w);
        row.RegisterMember("played",    &EvoProviderRow::played);
    } else {
        return false;
    }
    c.RegisterArray<std::vector<EvoProviderRow>>();

    if (auto pr = c.RegisterStruct<EvoPanelRow>()) {
        pr.RegisterMember("id",         &EvoPanelRow::id);
        pr.RegisterMember("title",      &EvoPanelRow::title);
        pr.RegisterMember("detail",     &EvoPanelRow::detail);
        pr.RegisterMember("badge",      &EvoPanelRow::badge);
        pr.RegisterMember("icon",       &EvoPanelRow::icon);
        pr.RegisterMember("radio",      &EvoPanelRow::radio);
        pr.RegisterMember("on",         &EvoPanelRow::on);
        pr.RegisterMember("warn",       &EvoPanelRow::warn);
        pr.RegisterMember("chevron",    &EvoPanelRow::chevron);
        pr.RegisterMember("badge_live", &EvoPanelRow::badge_live);
        pr.RegisterMember("focused",    &EvoPanelRow::focused);
    } else {
        return false;
    }
    c.RegisterArray<std::vector<EvoPanelRow>>();

    c.Bind("provider_name", &m_model.provider_name);
    c.Bind("breadcrumb",    &m_model.breadcrumb);
    c.Bind("status",        &m_model.status);
    c.Bind("query",         &m_model.query);
    c.Bind("loading",       &m_model.loading);
    c.Bind("tuning",        &m_model.tuning);
    c.Bind("has_error",     &m_model.has_error);
    c.Bind("empty",         &m_model.empty);
    c.Bind("is_folder_level", &m_model.is_folder_level);
    c.Bind("in_folder",     &m_model.in_folder);
    c.Bind("count",         &m_model.count);
    c.Bind("rows",          &m_model.rows);

    c.Bind("page_info",          &m_model.page_info);
    c.Bind("page_current",       &m_model.page_current);
    c.Bind("page_count",         &m_model.page_count);
    c.Bind("has_multiple_pages", &m_model.has_multiple_pages);

    c.Bind("selected_id",        &m_model.selected_id);
    c.Bind("selected_title",     &m_model.selected_title);
    c.Bind("selected_subtitle",  &m_model.selected_subtitle);
    c.Bind("selected_art",       &m_model.selected_art);
    c.Bind("selected_initial",   &m_model.selected_initial);
    c.Bind("selected_now",       &m_model.selected_now);
    c.Bind("selected_next",      &m_model.selected_next);
    c.Bind("epg_status",         &m_model.epg_status);
    c.Bind("epg_setup",          &m_model.epg_setup);
    c.Bind("setup_configured",   &m_model.setup_configured);
    c.Bind("setup_account",      &m_model.setup_account);
    c.Bind("panel_open",         &m_model.panel_open);
    c.Bind("panel_crumb",        &m_model.panel_crumb);
    c.Bind("panel_eyebrow",      &m_model.panel_eyebrow);
    c.Bind("panel_title",        &m_model.panel_title);
    c.Bind("panel_sub",          &m_model.panel_sub);
    c.Bind("panel_note_b",       &m_model.panel_note_b);
    c.Bind("panel_note",         &m_model.panel_note);
    c.Bind("panel_accept",       &m_model.panel_accept);
    c.Bind("panel_back",         &m_model.panel_back);
    c.Bind("panel_rows",         &m_model.panel_rows);
    c.Bind("selected_num",       &m_model.selected_num);
    c.Bind("selected_tech",      &m_model.selected_tech);
    c.Bind("has_selected",       &m_model.has_selected);
    c.Bind("selected_is_folder", &m_model.selected_is_folder);
    c.Bind("selected_overview",  &m_model.selected_overview);
    c.Bind("selected_resume",    &m_model.selected_resume);
    c.Bind("selected_progress",  &m_model.selected_progress);
    c.Bind("selected_progress_w", &m_model.selected_progress_w);
    c.Bind("selected_played",    &m_model.selected_played);

    /*
     * The one event a bundle raises to start something.
     *
     * It takes NO arguments, and the row it refers to is read back off the
     * element that raised it - via a `rowid` attribute the bundle binds with
     * data-attr-rowid="row.id".
     *
     * That indirection is not stylistic. DataControllerEvent::Initialize
     * parses its expression ONCE and resolves the variable addresses at that
     * moment, so inside a data-for the alias is baked to the first iteration:
     * `activate(row.id)` hands back row[0]'s id no matter which row was
     * clicked. Measured, not assumed - every card reported 'g:Kids'. Views
     * (data-attr, data-if, {{ }}) re-resolve per clone and are unaffected,
     * which is why the attribute carries it instead.
     *
     * The activation is recorded and acted on in Tick(), never here: this runs
     * inside Context::Update(), which is still walking the element tree that
     * raised the event, and both outcomes (a folder push that rebuilds `rows`,
     * or starting playback) destroy what it is walking.
     */
    c.BindEventCallback("activate",
        [this](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) {
            Rml::String id;
            if (Rml::Element* el = ev.GetCurrentElement())
                id = el->GetAttribute<Rml::String>("rowid", Rml::String());
            /* A bundle that passes the id explicitly still works - it is just
             * subject to the alias limitation above, so the attribute wins. */
            if (id.empty() && !args.empty())
                id = args[0].Get<Rml::String>();
            if (id.empty()) return;
            m_pending_activation = std::string(id.c_str());
        });

    c.BindEventCallback("activate_selected",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (!m_model.selected_id.empty()) {
                m_pending_activation = std::string(m_model.selected_id.c_str());
            }
        });

    c.BindEventCallback("page_up",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            HandleKey(KeyPageUp);
        });

    c.BindEventCallback("page_down",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            HandleKey(KeyPageDown);
        });

    /* Paging, for a bundle that wants an explicit "more" affordance rather
     * than relying on the host to append. */
    c.BindEventCallback("load_more",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            /* Rows already fetched but held back by the render budget come
             * first - asking the provider for another page while this level
             * still has unshown rows would fetch what is already in hand. */
            if (m_row_window < m_all_rows.size()) {
                m_row_window += EVO_PROVIDER_ROW_WINDOW;
                PublishRowWindow();
                PushArtRequests();
                return;
            }
            if (!m_has_more || m_model.loading) return;
            RequestPage(m_stack.empty() ? "" : m_stack.back().c_str(), ++m_page);
        });

    c.BindEventCallback("go_back",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            HandleKey(KeyBack);
        });

    c.BindEventCallback("setup_url",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            m_pending_action = EVO_PROVIDER_ACTION_SETUP_URL;
        });

    c.BindEventCallback("setup_usb",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            m_pending_action = EVO_PROVIDER_ACTION_SETUP_USB;
        });

    /* The one place a configured source is removed on purpose. */
    c.BindEventCallback("setup_signout",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (!m_provider) return;
            if (m_provider->sign_out) m_provider->sign_out();
            else if (m_provider->set_source) m_provider->set_source("");
            else return;
            PROV_LOG("'%s' signed out by the user", m_provider_id.c_str());
            if (toast) toast(m_provider->name, "Signed out");
            ShowSetupScreen();
        });

    m_model_handle = c.GetModelHandle();
    return (bool)m_model_handle;
}

/* ------------------------------------------------------------------------- */
/* Context and documents                                                     */
/* ------------------------------------------------------------------------- */

bool EvoRmlProviderHost::BuildContext(int width, int height)
{
    /* A distinct name per provider so two contexts can never collide, and so
     * a leaked one is identifiable in an Rml log. */
    std::string ctx_name = "provider_" + m_provider_id + "_context";
    m_context = Rml::CreateContext(ctx_name, Rml::Vector2i(width, height));
    if (!m_context) {
        PROV_LOG("CreateContext FAILED");
        return false;
    }
    m_context->SetDensityIndependentPixelRatio(EvoRmlApp::Instance().DpRatio());
    return true;
}

bool EvoRmlProviderHost::LoadEntryDocument(const std::string& rml_path)
{
    /*
     * A plain absolute path into the provider's cache directory. No file
     * interface change is needed: EvoRmlFileInterface::Open tries the embedded
     * bundle first and then falls back to fopen(), which is what makes a
     * downloaded document loadable at all.
     *
     * The path itself came from evo_bundle_path(), which is the only thing in
     * this codebase allowed to build one - see evo_provider_bundle.h.
     */
    m_doc = m_context->LoadDocument(rml_path);
    if (!m_doc) {
        PROV_LOG("LoadDocument('%s') FAILED", rml_path.c_str());
        return false;
    }
    if (!EnforceDomCap()) {
        m_doc->Close();
        m_doc = nullptr;
        return false;
    }
    m_doc->Show();
    return true;
}

bool EvoRmlProviderHost::LoadFallbackSkin()
{
    /*
     * The embedded skin. It is in the binary's asset bundle (#60), so it works
     * on first run, offline, and under uiview.sh with no server anywhere -
     * which is what makes a provider screen developable at all.
     */
    static const char* kPrefixes[] = {
        "/app0/assets/rml/",
        "assets/rml/",
        "projects/evoplayer/assets/rml/",
    };
    for (const char* p : kPrefixes) {
        m_doc = m_context->LoadDocument(std::string(p) + "provider_fallback.rml");
        if (m_doc) break;
    }
    if (!m_doc) {
        PROV_LOG("fallback skin FAILED to load");
        return false;
    }
    m_using_fallback = true;
    m_doc->Show();
    return true;
}

/*
 * DOM node cap. A bundle with a runaway data-for, or simply an enormous
 * hand-written document, would otherwise grow the element tree until layout
 * cost dominates the frame - the failure looks like the console hanging, not
 * like a bad bundle, which is exactly the diagnosis this prevents.
 */
bool EvoRmlProviderHost::EnforceDomCap()
{
    int counted = 0;
    /* Iterative walk: a deep document would otherwise recurse as deep as it
     * is nested, on a thread whose stack is not ours to spend. */
    std::vector<Rml::Element*> stack;
    stack.push_back(m_doc);
    while (!stack.empty()) {
        Rml::Element* e = stack.back();
        stack.pop_back();
        if (++counted > EVO_BUNDLE_MAX_DOM_NODES) {
            PROV_LOG("document exceeds %d DOM nodes - refused",
                     EVO_BUNDLE_MAX_DOM_NODES);
            return false;
        }
        for (int i = 0; i < e->GetNumChildren(); ++i)
            stack.push_back(e->GetChild(i));
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Open / Close                                                              */
/* ------------------------------------------------------------------------- */

bool EvoRmlProviderHost::Open(const char* provider_id, int width, int height)
{
    if (IsOpen()) Close();

    m_provider = evo_provider_find(provider_id);
    if (!m_provider) {
        PROV_LOG("unknown provider '%s'", provider_id ? provider_id : "(null)");
        return false;
    }
    /* A provider with no source yet is disabled by default, but its own setup
     * page is the only way to give it one - so it may open to be set up. Only
     * a configured provider the user switched off stays shut. (Refusing both
     * made IPTV unreachable on every fresh install.) */
    const bool configured = m_provider->is_configured && m_provider->is_configured();
    if (!evo_provider_is_enabled(provider_id) && configured) {
        PROV_LOG("'%s' is disabled", provider_id);
        return false;
    }
    if (!configured)
        PROV_LOG("'%s' is not set up yet - opening for setup", provider_id);

    m_provider_id = provider_id;
    /* Another provider's shelves must never show under this one. */
    m_shelves.clear();
    m_shelves_on = false;
    m_shelf_wait_root = false;
    m_shelf_row = 0;
    ++m_shelf_gen;
    m_using_fallback = false;
    m_stack.clear();
    m_pos_stack.clear();
    m_crumbs.clear();
    m_page = 0;
    m_has_more = false;
    m_model = EvoProviderModel{};
    m_model.provider_name = Rml::String(m_provider->name);
    m_model.breadcrumb = Rml::String(m_provider->name);

    if (!BuildContext(width, height)) return false;

    std::string model_name = m_provider_id;
    bool loaded = false;
    evo_bundle_status_t st = EVO_BUNDLE_ERR_NO_URL;

    /*
     * Check if this provider has an embedded UI document in the application bundle
     * (e.g. "rml/iptv.rml"). If present, it loads instantly from memory with zero
     * network or disk dependency, completely self-contained.
     */
    /* A provider family that shares a document (Emby + Jellyfin: "mediaserver")
     * names it; the data model takes the document's name, since the document
     * declares it. */
    const std::string doc_name = m_provider->ui_embedded ? m_provider->ui_embedded : m_provider_id;
    std::string embedded_path = "rml/" + doc_name + ".rml";
    if (evo_rmlui_bundle_find(embedded_path)) {
        model_name = doc_name;
        m_bundle_version = "embedded";
        m_model_name = model_name;
        if (RegisterDataModel()) {
            loaded = LoadEntryDocument(embedded_path);
        }
        if (loaded) st = EVO_BUNDLE_OK;
    }

    /*
     * If not embedded, try the cached bundle before the network.
     */
    if (!loaded) {
        evo_bundle_manifest_t man;
        st = evo_bundle_load_cached(m_provider_id.c_str(), &man);

        if (st == EVO_BUNDLE_OK) {
            char path[512];
            if (evo_bundle_path(m_provider_id.c_str(), man.entry, path, sizeof path) == 0) {
                model_name = man.data_model[0] ? man.data_model : m_provider_id;
                m_bundle_version = man.version;
                m_model_name = model_name;
                /* The model has to exist before the document is parsed: RmlUi
                 * resolves a document's data-model attribute at load time and a
                 * document naming a model that is not there binds nothing, silently. */
                if (RegisterDataModel())
                    loaded = LoadEntryDocument(path);
            }
            if (!loaded) st = EVO_BUNDLE_ERR_INCOMPLETE;
        }
    }

    PROV_LOG("'%s' cached/embedded bundle: %s", m_provider_id.c_str(),
             evo_bundle_status_str(st));

    if (!loaded) {
        /*
         * The fallback skin binds to the model called "provider".
         *
         * It has to be a constant: the skin is authored once, compiled into
         * the binary, and used for every provider, so it cannot name the
         * provider's own model. Registering the model under any other name
         * here leaves the document parsed but bound to nothing - which renders
         * as literal "{{ provider_name }}" text with every data-if branch
         * showing at once, and no warning anywhere. That is exactly what it
         * did before this comment existed.
         */
        if (!m_model_name.empty() && m_model_name != kFallbackModelName) {
            m_context->RemoveDataModel(m_model_name);
            m_model_handle = Rml::DataModelHandle();
        }
        m_model_name = kFallbackModelName;
        if (!m_model_handle && !RegisterDataModel()) { Close(); return false; }
        if (!LoadFallbackSkin()) { Close(); return false; }

        if (st != EVO_BUNDLE_OK)
            SetStatus(std::string(m_provider->name) + ": " +
                      evo_bundle_status_str(st), st != EVO_BUNDLE_OK);
    }

    /*
     * Kick a refresh either way. A bundle that has never been fetched gets one
     * now; a cached one checks its version and usually downloads nothing.
     */
    if ((m_provider->caps & EVO_PROVIDER_CAP_UI) && m_provider->ui_bundle_url) {
        const char* url = m_provider->ui_bundle_url();
        if (url && *url)
            evo_bundle_refresh_async(m_provider_id.c_str(), url,
                                      &EvoRmlProviderHost::BundleCallback, this);
    }

    PROV_LOG("'%s' host open: %s  model='%s' version='%s'",
             m_provider_id.c_str(),
             m_using_fallback ? "FALLBACK SKIN" : "provider bundle",
             m_model_name.c_str(),
             m_bundle_version.empty() ? "-" : m_bundle_version.c_str());

    /* And the first page of the catalog. */
    PROV_LOG("open: calling RequestPage");
    RequestPage("", 0);
    PROV_LOG("open: RequestPage finished, returning true");

    m_dirty = true;
    return true;
}

void EvoRmlProviderHost::Close()
{
    /* Bump the generation so any catalog reply still in evo_net's queue is
     * dropped rather than writing into a model whose context is gone. */
    m_request_generation++;

    if (m_doc) { m_doc->Close(); m_doc = nullptr; }
    if (m_context) {
        if (!m_model_name.empty()) m_context->RemoveDataModel(m_model_name);
        Rml::RemoveContext(m_context->GetName());
        m_context = nullptr;
    }
    m_model_handle = Rml::DataModelHandle();
    m_model_name.clear();
    m_model = EvoProviderModel{};
    m_provider = nullptr;
    m_provider_id.clear();
    m_stack.clear();
    m_pos_stack.clear();
    m_crumbs.clear();
    m_art_urls.clear();
    m_art_keys.clear();
    m_all_rows.clear();
    m_row_window = 0;
    m_row_offset = 0;
    m_needs_initial_focus = false;
    m_pending_activation.clear();
    m_detail = DetailState();
    ++m_detail_gen;
    m_detail_bypass = false;
    m_pending_action = 0;
    m_is_usb_picker = false;
    m_saved_usb_playlists.clear();
    m_bundle_version.clear();
    m_using_fallback = false;
    g_selection_pending = false;
    m_dirty = true;

    /*
     * Drop RmlUi's parsed-stylesheet cache.
     *
     * It is keyed by FILE PATH, and a refreshed bundle writes its new .rcss
     * over the same cache path it had before. Without this, a reopen onto an
     * updated bundle re-used the stylesheet parsed from the PREVIOUS version's
     * bytes: the new .rml loaded, the old rules styled it, and the result was
     * a screen that rendered rather than failed - new markup with no rules
     * matching it, laid out by whatever old selectors happened to still hit.
     * Measured on hardware: the cache on disk hashed correctly against the new
     * manifest while the focus ring on screen was still the previous version's
     * colour.
     *
     * Clearing on Close() rather than at the reopen covers the other order too
     * - leave the screen, the bundle updates, come back - and costs nothing:
     * EVO's own documents are loaded once at init and hold their stylesheets
     * by shared pointer, so this only forces the NEXT load to reparse.
     *
     * Textures are deliberately NOT released here. `Rml::ReleaseTextures()` is
     * process-wide, so it would drop EVO's own icon atlas and force a re-upload
     * through the sceAgc interface mid-session. A bundle that ships its own
     * images and changes them is the case that would need it; none does yet.
     */
    Rml::Factory::ClearStyleSheetCache();
}

/* ------------------------------------------------------------------------- */
/* Catalog                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * The callback context. Heap-allocated per request and carrying the generation
 * it was issued under, because evo_net's queue can outlive the screen: the
 * reply for a folder the user opened and then backed out of still arrives, and
 * without the generation it would repopulate a screen that has moved on.
 */
struct ItemsCtx {
    EvoRmlProviderHost* host;
    unsigned generation;
};

void EvoRmlProviderHost::RequestPage(const char* parent_id, int page)
{
    m_setup_shown = false;
    /* The root of a media server is a row of shelves, not one level. */
    m_shelf_wait_root = page == 0 && (!parent_id || !*parent_id) && m_model.query.empty() && ShelfCapable();
    if (!ShelfCapable()) { m_shelves.clear(); m_shelves_on = false; }
    /* Further pages of a search come from search(), not the catalog. */
    if (page > 0 && !m_model.query.empty() && m_provider && m_provider->search) {
        m_model.loading = true;
        if (m_model_handle) m_model_handle.DirtyVariable("loading");
        ItemsCtx* sctx = new ItemsCtx{this, ++m_request_generation};
        if (m_provider->search(m_model.query.c_str(), page, &EvoRmlProviderHost::ItemsCallback, sctx) != 0) {
            delete sctx;
            m_model.loading = false;
            if (m_model_handle) m_model_handle.DirtyVariable("loading");
        }
        return;
    }
    if (!m_provider || !(m_provider->caps & EVO_PROVIDER_CAP_CATALOG)) {
        SetStatus(std::string(m_provider ? m_provider->name : "Provider") +
                  " has no catalog", false);
        return;
    }

    m_is_usb_picker = false;
    if (page == 0) {
        m_model.rows.clear();
        m_all_rows.clear();
        m_row_window = 0;
        m_row_offset = 0;
        m_art_urls.clear();
        m_art_keys.clear();
        m_model.count = 0;
        if (m_model_handle) {
            m_model_handle.DirtyVariable("rows");
            m_model_handle.DirtyVariable("count");
        }
    }

    m_model.loading = true;
    m_model.empty = false;
    if (m_model_handle) {
        m_model_handle.DirtyVariable("loading");
        m_model_handle.DirtyVariable("empty");
    }

    /* Rebuild the breadcrumb from the names pushed on the way down. */
    std::string crumb = m_provider->name;
    for (const std::string& c : m_crumbs) crumb += " / " + c;
    m_model.breadcrumb = Rml::String(crumb.c_str());
    if (m_model_handle) m_model_handle.DirtyVariable("breadcrumb");

    m_dirty = true;

    PROV_LOG("RequestPage parent='%s' page=%d", parent_id ? parent_id : "", page);
    ItemsCtx* ctx = new ItemsCtx{this, ++m_request_generation};
    PROV_LOG("RequestPage calling list_catalog (provider=%p)", (void*)m_provider);
    int rc = m_provider->list_catalog(parent_id, page,
                                      &EvoRmlProviderHost::ItemsCallback, ctx);
    PROV_LOG("RequestPage list_catalog returned rc=%d", rc);
    if (rc != 0) {
        /* Not accepted, so the callback will never fire and this owns ctx. */
        delete ctx;
        m_model.loading = false;
        m_model.empty = m_all_rows.empty();
        if (m_model_handle) {
            m_model_handle.DirtyVariable("loading");
            m_model_handle.DirtyVariable("empty");
        }
        if (m_doc) m_needs_initial_focus = true;
        PROV_LOG("'%s' list_catalog REFUSED rc=%d configured=%d",
                 m_provider_id.c_str(), rc, m_provider->is_configured());
        if (m_provider->is_configured()) {
            char why[256];
            SetStatus(evo_failure_reason(why, sizeof why) ? why : "Could not reach the provider",
                      true);
        } else {
            SetStatus("", false);
        }
    }
}

void EvoRmlProviderHost::ItemsCallback(int ok, const evo_provider_item_t* items,
                                       int count, int has_more, void* ud)
{
    ItemsCtx* ctx = (ItemsCtx*)ud;
    EvoRmlProviderHost* self = ctx->host;
    unsigned gen = ctx->generation;
    delete ctx;

    /* The screen moved on, or closed, while this was in flight. */
    if (!self->IsOpen() || gen != self->m_request_generation) return;

    self->m_model.loading = false;
    if (self->m_model_handle) self->m_model_handle.DirtyVariable("loading");

    if (!ok) {
        self->m_model.empty = self->m_all_rows.empty();
        if (self->m_model_handle) self->m_model_handle.DirtyVariable("empty");
        if (self->m_doc) self->m_needs_initial_focus = true;
        char why[256];
        self->SetStatus(evo_failure_reason(why, sizeof why) ? why : "Could not load the catalog",
                        true);
        return;
    }

    if (self->m_shelf_wait_root) {
        self->m_shelf_wait_root = false;
        if (self->BeginShelves(items, count)) return;
        /* Not a list of rows (a flat playlist, nothing yet): the plain level. */
        self->m_shelves.clear();
        self->m_shelves_on = false;
    }
    self->m_has_more = has_more != 0;
    self->ApplyItems(items, count, has_more);

    PROV_LOG("'%s' catalog parent='%s' -> %d rows (folder_level=%d has_more=%d total=%d)",
             self->m_provider_id.c_str(),
             self->m_stack.empty() ? "" : self->m_stack.back().c_str(),
             count, self->m_model.is_folder_level ? 1 : 0, has_more,
             self->m_model.count);
}

/*
 * Provider text as the UI font can draw it.
 *
 * Catalogs decorate names with emoji ("⚽ BRASILEIRAO", "CANAL ✨ REALITY").
 * LatoLatin has none of them, so each one rendered as a box or a stray mark in
 * front of the real name. Dropped here: pictographs (U+1F000+), the symbol and
 * dingbat blocks (U+2190-U+2BFF), variation selectors and the zero-width
 * joiner that glue emoji together, and private-use glyphs. Letters in any
 * script, digits and ordinary punctuation are kept. Runs of spaces left behind
 * collapse to one and the ends are trimmed.
 */
/*
 * Styled letters LatoLatin cannot draw ("ᵁᴴᴰ ³⁸⁴⁰ᴾ", small caps, fullwidth),
 * mapped to the plain ASCII they stand for. 0 when there is no mapping.
 */
static char fancy_to_ascii(unsigned cp)
{
    if (cp >= 0xFF01 && cp <= 0xFF5E) return (char)(cp - 0xFF01 + 0x21);   /* fullwidth */
    if (cp >= 0x2074 && cp <= 0x2079) return (char)('4' + (cp - 0x2074));  /* ⁴..⁹ */
    if (cp >= 0x2080 && cp <= 0x2089) return (char)('0' + (cp - 0x2080));  /* ₀..₉ */
    switch (cp) {
    case 0x00B2: return '2';
    case 0x00B3: return '3';
    case 0x00B9: return '1';
    case 0x2070: return '0';
    case 0x2071: return 'i';
    case 0x207F: return 'n';
    case 0x207A: return '+';
    case 0x207B: return '-';
    }
    static const struct { unsigned short cp; char c; } tbl[] = {
        /* modifier capitals */
        {0x1D2C,'A'},{0x1D2E,'B'},{0x1D30,'D'},{0x1D31,'E'},{0x1D33,'G'},{0x1D34,'H'},
        {0x1D35,'I'},{0x1D36,'J'},{0x1D37,'K'},{0x1D38,'L'},{0x1D39,'M'},{0x1D3A,'N'},
        {0x1D3C,'O'},{0x1D3E,'P'},{0x1D3F,'R'},{0x1D40,'T'},{0x1D41,'U'},{0x1D42,'W'},
        /* modifier lowercase */
        {0x1D43,'a'},{0x1D47,'b'},{0x1D9C,'c'},{0x1D48,'d'},{0x1D49,'e'},{0x1DA0,'f'},
        {0x1D4D,'g'},{0x02B0,'h'},{0x02B2,'j'},{0x1D4F,'k'},{0x02E1,'l'},{0x1D50,'m'},
        {0x1D52,'o'},{0x1D56,'p'},{0x02B3,'r'},{0x02E2,'s'},{0x1D57,'t'},{0x1D58,'u'},
        {0x1D5B,'v'},{0x02B7,'w'},{0x02E3,'x'},{0x02B8,'y'},{0x1DBB,'z'},
        /* small caps */
        {0x1D00,'A'},{0x0299,'B'},{0x1D04,'C'},{0x1D05,'D'},{0x1D07,'E'},{0xA730,'F'},
        {0x0262,'G'},{0x029C,'H'},{0x026A,'I'},{0x1D0A,'J'},{0x1D0B,'K'},{0x029F,'L'},
        {0x1D0D,'M'},{0x0274,'N'},{0x1D0F,'O'},{0x1D18,'P'},{0x0280,'R'},{0xA731,'S'},
        {0x1D1B,'T'},{0x1D1C,'U'},{0x1D20,'V'},{0x1D21,'W'},{0x028F,'Y'},{0x1D22,'Z'},
    };
    for (const auto& e : tbl)
        if (e.cp == cp) return e.c;
    return 0;
}

static Rml::String display_text(const char* in)
{
    std::string out;
    const unsigned char* p = (const unsigned char*)(in ? in : "");
    while (*p) {
        unsigned cp = *p;
        int n = 1;
        if (cp >= 0xF0 && p[1] && p[2] && p[3]) {
            cp = ((cp & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            n = 4;
        } else if (cp >= 0xE0 && p[1] && p[2]) {
            cp = ((cp & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            n = 3;
        } else if (cp >= 0xC0 && p[1]) {
            cp = ((cp & 0x1F) << 6) | (p[1] & 0x3F);
            n = 2;
        }
        bool drop = cp >= 0x1F000 ||
                    (cp >= 0x2190 && cp <= 0x2BFF) ||
                    (cp >= 0xFE00 && cp <= 0xFE0F) ||
                    cp == 0x200D || cp == 0x20E3 ||
                    (cp >= 0xE000 && cp <= 0xF8FF);
        if (drop) {
            if (!out.empty() && out.back() != ' ') out.push_back(' ');
        } else if (*p == ' ') {
            if (!out.empty() && out.back() != ' ') out.push_back(' ');
        } else if (n > 1 && fancy_to_ascii(cp)) {
            out.push_back(fancy_to_ascii(cp));
        } else {
            out.append((const char*)p, (size_t)n);
        }
        p += n;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    /* A name that was nothing but symbols keeps them: boxes beat a blank card. */
    if (out.empty()) return Rml::String(in ? in : "");
    return Rml::String(out.c_str());
}

extern "C" void evo_home_art_capture(const char*, const char*, const char*, const char*);

void EvoRmlProviderHost::ApplyItems(const evo_provider_item_t* items, int count,
                                    int has_more)
{
    (void)has_more;

    for (int i = 0; i < count; ++i) {
        const evo_provider_item_t& s = items[i];
        EvoProviderRow r;
        r.id        = Rml::String(s.id);
        r.title     = display_text(s.title);
        r.subtitle  = display_text(s.subtitle);
        r.overview  = display_text(s.overview);
        r.now       = display_text(s.now_title);
        r.next      = display_text(s.next_title);
        r.duration  = s.is_live ? Rml::String() : format_duration(s.duration_sec);
        /* ASCII-only on purpose: taking the first BYTE of a UTF-8 title would
         * emit half a codepoint, so a non-ASCII title gets no initial rather
         * than a broken glyph. */
        const char* t0 = r.title.c_str();
        if (t0[0] >= 0x20 && (unsigned char)t0[0] < 0x80) {
            char ini[2] = { t0[0], 0 };
            if (ini[0] >= 'a' && ini[0] <= 'z') ini[0] = (char)(ini[0] - 'a' + 'A');
            r.initial = Rml::String(ini);
        }
        r.is_folder = s.is_folder != 0;
        r.is_live   = s.is_live != 0;
        r.index     = (int)m_all_rows.size();
        r.played    = s.played != 0;
        r.favorite  = s.favorite != 0;
        r.resume_sec = s.resume_pos_sec > 0 ? (long long)s.resume_pos_sec : 0;
        r.rating = s.rating;
        r.cert = s.cert;
        r.facts = s.facts;
        r.art_url = s.art_url;
        r.duration_sec = s.duration_sec;
        r.detail = s.detail;
        if (s.resume_pos_sec > 0 && s.duration_sec > 0) {
            long long pct = (long long)s.resume_pos_sec * 100 / (long long)s.duration_sec;
            r.progress = (int)(pct < 1 ? 1 : pct > 99 ? 99 : pct);
            /* Published ready to use: an expression building "37%" from an
             * int did not apply on hardware (the bar stayed empty). */
            char w[8];
            snprintf(w, sizeof w, "%d%%", r.progress);
            r.progress_w = Rml::String(w);
        }
        /* Art starts empty and is filled in by the art queue as posters
         * arrive. A bundle that wants something behind the gap paints a
         * background colour on the element, which is why this is "" and not a
         * placeholder image the provider did not choose. */
        /* Home shows what was played, so keep the art of anything started or part-watched
         * (Plex keeps every video it has browsed, as it always did). */
        if (!s.is_folder && !s.is_live &&
            (s.resume_pos_sec > 0 || s.played || (m_provider_id == "plex" && !std::strncmp(s.id, "video:", 6)))) {
            evo_home_art_capture(m_provider_id.c_str(), s.id, s.art_url, s.backdrop_url);
        }
        m_all_rows.push_back(std::move(r));
        evo_provider_art_set_fallback(s.art_url, s.art_alt_url);
        m_art_urls.emplace_back(s.art_url);
        m_art_keys.emplace_back();
    }

    m_model.count = (int)m_all_rows.size();
    m_model.empty = m_all_rows.empty();
    /* A level is a folder level when its first row is a folder. Providers do
     * not mix the two at one level - a group list is groups, a group's contents
     * are channels - and taking the first row rather than requiring every row
     * to agree keeps a provider that does mix them from flipping the header. */
    m_model.is_folder_level = !m_all_rows.empty() && m_all_rows[0].is_folder;

    m_model.in_folder = !m_stack.empty();
    if (m_model_handle) m_model_handle.DirtyVariable("in_folder");

    m_focus_slot = 0;
    if (m_saved_offset > 0 || m_saved_slot > 0) {
        size_t off = m_saved_offset;
        int slot = m_saved_slot;
        m_saved_offset = 0;
        m_saved_slot = -1;
        SetPageOffset(off, slot);
    } else {
        PublishRowWindow();
    }

    if (m_model_handle) {
        m_model_handle.DirtyVariable("count");
        m_model_handle.DirtyVariable("empty");
        m_model_handle.DirtyVariable("is_folder_level");
    }

    SetStatus("", false);

    /* A Down/R1 past the last loaded page fetched this page: finish the move
     * now that the cards exist. */
    if (m_advance_after_load) {
        m_advance_after_load = false;
        if (m_row_offset + EVO_PROVIDER_PAGE_SIZE < m_all_rows.size())
            SetPageOffset(m_row_offset + EVO_PROVIDER_PAGE_SIZE, m_advance_col);
    }

    /* Deferred to Render(), which is where Context::Update() creates the
     * elements data-for is going to produce. See m_needs_initial_focus. */
    if (m_doc)
        m_needs_initial_focus = true;

    PushArtRequests();
    m_dirty = true;
}

void EvoRmlProviderHost::Search(const char* query)
{
    if (!m_provider) return;

    std::string q = query ? query : "";
    size_t b = q.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        q.clear();
    } else {
        size_t e = q.find_last_not_of(" \t\r\n");
        q = q.substr(b, e - b + 1);
    }

    if (q.empty()) {
        m_model.query = "";
        if (m_model_handle) m_model_handle.DirtyVariable("query");
        /* Restore current level */
        m_page = 0;
        RequestPage(m_stack.empty() ? "" : m_stack.back().c_str(), 0);
        return;
    }

    if (!(m_provider->caps & EVO_PROVIDER_CAP_SEARCH) || !m_provider->search) {
        SetStatus("Provider does not support search", true);
        return;
    }

    m_model.query = Rml::String(q.c_str());
    m_model.rows.clear();
    m_all_rows.clear();
    m_row_window = 0;
    m_row_offset = 0;
    m_art_urls.clear();
    m_art_keys.clear();
    m_model.count = 0;
    m_model.loading = true;
    m_model.empty = false;
    m_model.is_folder_level = false;

    std::string crumb = std::string(m_provider->name) + " / Search: " + q;
    m_model.breadcrumb = Rml::String(crumb.c_str());

    if (m_model_handle) {
        m_model_handle.DirtyVariable("query");
        m_model_handle.DirtyVariable("rows");
        m_model_handle.DirtyVariable("count");
        m_model_handle.DirtyVariable("loading");
        m_model_handle.DirtyVariable("empty");
        m_model_handle.DirtyVariable("is_folder_level");
        m_model_handle.DirtyVariable("breadcrumb");
    }

    m_dirty = true;
    PROV_LOG("Search query='%s'", q.c_str());
    m_page = 0;
    ItemsCtx* ctx = new ItemsCtx{this, ++m_request_generation};
    int rc = m_provider->search(q.c_str(), 0, &EvoRmlProviderHost::ItemsCallback, ctx);
    if (rc != 0) {
        delete ctx;
        m_model.loading = false;
        if (m_model_handle) m_model_handle.DirtyVariable("loading");
        SetStatus("Search failed", true);
    }
}

/* ------------------------------------------------------------------------- */
/* The row window - see EVO_PROVIDER_ROW_WINDOW                              */
/* ------------------------------------------------------------------------- */

void EvoRmlProviderHost::PublishRowWindow()
{
    const size_t total = m_all_rows.size();
    size_t want = EVO_PROVIDER_PAGE_SIZE;
    if (m_row_offset >= total && total > 0) {
        m_row_offset = ((total - 1) / EVO_PROVIDER_PAGE_SIZE) * EVO_PROVIDER_PAGE_SIZE;
    }
    if (m_row_offset + want > total) {
        want = (total > m_row_offset) ? (total - m_row_offset) : 0;
    }
    m_row_window = want;

    const long first = (long)m_row_offset;
    m_model.rows.assign(m_all_rows.begin() + first,
                        m_all_rows.begin() + first + (long)want);

    for (size_t i = 0; i < m_model.rows.size(); ++i) {
        m_model.rows[i].index = (int)i;
    }

    m_model.page_count = total ? (int)((total + EVO_PROVIDER_PAGE_SIZE - 1) / EVO_PROVIDER_PAGE_SIZE) : 1;
    m_model.page_current = total ? (int)(m_row_offset / EVO_PROVIDER_PAGE_SIZE + 1) : 1;
    m_model.has_multiple_pages = (m_model.page_count > 1);

    char pbuf[64];
    std::snprintf(pbuf, sizeof(pbuf), "PAGE %d OF %d", m_model.page_current, m_model.page_count);
    m_model.page_info = Rml::String(pbuf);

    if (m_model_handle) {
        m_model_handle.DirtyVariable("rows");
        m_model_handle.DirtyVariable("page_info");
        m_model_handle.DirtyVariable("page_current");
        m_model_handle.DirtyVariable("page_count");
        m_model_handle.DirtyVariable("has_multiple_pages");
    }
    m_dirty = true;
    UpdateSelectedPreview();
}

int EvoRmlProviderHost::FocusedRowIndex() const
{
    if (!m_context) return -1;
    Rml::Element* e = m_context->GetFocusElement();
    while (e) {
        const Rml::String id = e->GetAttribute<Rml::String>("rowid", Rml::String());
        if (!id.empty()) {
            for (size_t i = 0; i < m_model.rows.size(); ++i)
                if (m_model.rows[i].id == id) return (int)i;
            return -1;
        }
        e = e->GetParentNode();
    }
    return -1;
}

const EvoProviderRow* EvoRmlProviderHost::GetFocusedRow() const
{
    if (ShelvesActive()) return ShelfFocusedRow();
    const int idx = FocusedRowIndex();
    if (idx >= 0 && idx < (int)m_model.rows.size()) {
        return &m_model.rows[idx];
    }
    return nullptr;
}

void EvoRmlProviderHost::SetPageOffset(size_t new_offset, int target_slot)
{
    if (m_all_rows.empty()) {
        m_row_offset = 0;
        PublishRowWindow();
        return;
    }
    if (new_offset >= m_all_rows.size()) {
        new_offset = ((m_all_rows.size() - 1) / EVO_PROVIDER_PAGE_SIZE) * EVO_PROVIDER_PAGE_SIZE;
    }
    m_row_offset = new_offset;
    PublishRowWindow();
    PushArtRequests();

    if (target_slot >= 0) {
        if (target_slot >= (int)m_model.rows.size()) {
            target_slot = (int)m_model.rows.size() - 1;
        }
        /* The cards for the new page do not exist until the next
         * Context::Update(), so focusing one now would hit the old page. */
        m_focus_slot = target_slot < 0 ? 0 : target_slot;
        m_needs_initial_focus = true;
    }
    UpdateSelectedPreview();
}

void EvoRmlProviderHost::UpdateSelectedPreview()
{
    const EvoProviderRow* row = GetFocusedRow();
    if (!row && !m_model.rows.empty()) {
        row = &m_model.rows[0];
    }

    if (row) {
        m_model.has_selected = true;
        m_model.selected_id = row->id;
        m_model.selected_title = row->title;
        m_model.selected_subtitle = row->subtitle;
        m_model.selected_art = row->art;
        m_model.selected_initial = row->initial;
        m_model.selected_now = row->now;
        m_model.selected_next = row->next;
        m_model.epg_status = (m_provider && std::strcmp(m_provider->id, "iptv") == 0 && provider_iptv_epg_status)
                           ? provider_iptv_epg_status() : "Live Broadcast";
        m_model.epg_setup = m_provider && std::strcmp(m_provider->id, "iptv") == 0 &&
                            provider_iptv_epg_needs_setup && provider_iptv_epg_needs_setup();
        m_model.selected_is_folder = row->is_folder;
        m_model.selected_overview = row->overview;
        m_model.selected_progress = row->progress;
        m_model.selected_progress_w = row->progress_w;
        m_model.selected_played = row->played;
        if (row->resume_sec > 0) {
            Rml::String t = format_duration(row->resume_sec);
            m_model.selected_resume = Rml::String("Resume from ") + t;
        } else {
            m_model.selected_resume = "";
        }

        int abs_idx = (int)m_row_offset + row->index;
        char num_buf[32];
        std::snprintf(num_buf, sizeof(num_buf), "CH %d", abs_idx + 1);
        m_model.selected_num = Rml::String(num_buf);

        if (row->is_folder) {
            m_model.selected_tech = "CHANNEL GROUP";
        } else if (row->is_live) {
            m_model.selected_tech = "LIVE HLS STREAM • 1080p • 60 FPS";
        } else {
            m_model.selected_tech = row->duration.empty() ? "VOD STREAM" : row->duration;
        }
    } else {
        m_model.has_selected = false;
        m_model.selected_id = "";
        m_model.selected_title = "";
        m_model.selected_subtitle = "";
        m_model.selected_art = "";
        m_model.selected_initial = "";
        m_model.selected_now = "";
        m_model.selected_next = "";
        m_model.selected_num = "";
        m_model.selected_tech = "";
        m_model.selected_is_folder = false;
        m_model.selected_overview = "";
        m_model.selected_resume = "";
        m_model.selected_progress = 0;
        m_model.selected_progress_w = "";
        m_model.selected_played = false;
    }

    if (m_model_handle) {
        m_model_handle.DirtyVariable("has_selected");
        m_model_handle.DirtyVariable("selected_id");
        m_model_handle.DirtyVariable("selected_title");
        m_model_handle.DirtyVariable("selected_subtitle");
        m_model_handle.DirtyVariable("selected_art");
        m_model_handle.DirtyVariable("selected_initial");
        m_model_handle.DirtyVariable("selected_now");
        m_model_handle.DirtyVariable("selected_next");
        m_model_handle.DirtyVariable("epg_status");
        m_model_handle.DirtyVariable("epg_setup");
        m_model_handle.DirtyVariable("selected_num");
        m_model_handle.DirtyVariable("selected_tech");
        m_model_handle.DirtyVariable("selected_is_folder");
        m_model_handle.DirtyVariable("selected_overview");
        m_model_handle.DirtyVariable("selected_resume");
        m_model_handle.DirtyVariable("selected_progress");
        m_model_handle.DirtyVariable("selected_progress_w");
        m_model_handle.DirtyVariable("selected_played");
    }
}

bool EvoRmlProviderHost::FocusPublishedRow(int local_idx)
{
    if (!m_doc || local_idx < 0) return false;

    /* The cards are whatever data-for instanced with a `rowid`, in document
     * order - found from the document, not from the focused element, which
     * may be the WATCH button or nothing at all. data-for's template element
     * has no bound rowid, so it never counts. */
    std::vector<Rml::Element*> cards;
    std::vector<Rml::Element*> todo{m_doc};
    while (!todo.empty()) {
        Rml::Element* e = todo.back();
        todo.pop_back();
        if (!e->GetAttribute<Rml::String>("rowid", Rml::String()).empty() && e->IsVisible()) {
            cards.push_back(e);
            continue;
        }
        for (int i = e->GetNumChildren() - 1; i >= 0; --i)
            todo.push_back(e->GetChild(i));
    }
    if (cards.empty()) return false;
    if (local_idx >= (int)cards.size()) local_idx = (int)cards.size() - 1;
    cards[local_idx]->Focus();
    m_dirty = true;
    return true;
}

/* Focus m_focus_slot's card; a level with no cards (setup, empty) falls back
 * to setup cards or KI_TAB so its own controls still get focus. Call after Context::Update(). */
bool EvoRmlProviderHost::SeedFocus()
{
    m_needs_initial_focus = false;
    const int slot = m_focus_slot;
    m_focus_slot = 0;
    if (FocusPublishedRow(slot)) {
        UpdateSelectedPreview();
        return true;
    }

    /* Setup cards (IPTV / Xtream setup screen) */
    if (m_doc) {
        if (auto* card = m_doc->GetElementById("tv-card-setup-url")) {
            if (card->IsVisible(true)) {
                card->Focus();
                m_dirty = true;
                return true;
            }
        }
        if (auto* card = m_doc->GetElementById("tv-card-setup-usb")) {
            if (card->IsVisible(true)) {
                card->Focus();
                m_dirty = true;
                return true;
            }
        }
        Rml::ElementList setup_cards;
        m_doc->GetElementsByClassName(setup_cards, "tv-setup-card");
        for (auto* sc : setup_cards) {
            if (sc && sc->IsVisible(true)) {
                sc->Focus();
                m_dirty = true;
                return true;
            }
        }
        if (!m_context->GetFocusElement()) {
            m_doc->Focus();
        }
    }

    m_context->ProcessKeyDown(Rml::Input::KI_TAB, 0);
    m_context->ProcessKeyUp(Rml::Input::KI_TAB, 0);
    return m_context->GetFocusElement() != nullptr;
}

/* ------------------------------------------------------------------------- */
/* Artwork                                                                   */
/* ------------------------------------------------------------------------- */

void EvoRmlProviderHost::PushArtRequests()
{
    /*
     * Ask for the posters that are not drawable yet, and adopt the ones that
     * have become drawable since the last pass.
     *
     * Prioritize a window around current focus so visible cards populate first.
     * When the queue fills (-2), preserve the URLs and stop asking this tick.
     */
    bool changed = false;
    const size_t n = m_model.rows.size();
    if (n == 0) return;

    int focus_idx = FocusedRowIndex();
    if (focus_idx < 0) focus_idx = 0;

    (void)focus_idx;
    size_t start = 0;
    size_t end = n;                                  /* a page is what is on screen */

    bool queue_full = false;

    /* `i` is a PUBLISHED index; the art vectors are parallel to m_all_rows, so
     * every one of them is read at m_row_offset + i. */
    auto try_row = [&](size_t i) {
        if (queue_full || i >= n) return;
        const size_t a = m_row_offset + i;
        if (a >= m_art_urls.size()) return;
        if (!m_model.rows[i].art.empty()) return;      /* already showing */
        if (m_art_urls[a].empty()) return;             /* row has no art   */

        if (!m_art_keys[a].empty()) {
            /* Promised a key earlier - has it arrived? */
            if (evo_provider_art_ready(m_art_keys[a].c_str())) {
                m_model.rows[i].art = Rml::String(m_art_keys[a].c_str());
                m_all_rows[a].art   = m_model.rows[i].art;
                changed = true;
            }
            return;
        }

        char key[128];
        int rc = evo_provider_art_request(m_provider_id.c_str(),
                                          m_art_urls[a].c_str(),
                                          key, sizeof key);
        if (rc == -2) {
            /* In-flight queue is full. Keep URL to retry next tick, stop asking this tick. */
            queue_full = true;
            return;
        }
        if (rc < 0) {
            /* Unusable URL. Clear it so this row stops asking. */
            m_art_urls[a].clear();
            return;
        }
        m_art_keys[a] = key;
        if (rc == 1) {
            m_model.rows[i].art = Rml::String(key);
            m_all_rows[a].art   = m_model.rows[i].art;
            changed = true;
        }
    };

    /* Pass 1: prioritize window around current focus */
    for (size_t i = start; i < end; ++i) {
        try_row(i);
    }
    /* Pass 2: remaining rows if queue didn't fill */
    for (size_t i = 0; i < start && !queue_full; ++i) {
        try_row(i);
    }
    for (size_t i = end; i < n && !queue_full; ++i) {
        try_row(i);
    }

    if (changed && m_model_handle) {
        m_model_handle.DirtyVariable("rows");
        UpdateSelectedPreview();
        m_dirty = true;
    }
}

/* ------------------------------------------------------------------------- */
/* Input                                                                     */
/* ------------------------------------------------------------------------- */

bool EvoRmlProviderHost::HandleKey(Key k)
{
    if (!IsOpen() || !m_context) return false;

    /* The detail page owns every key while it is up (#221). */
    if (m_detail.open) return DetailKey(k);

    /* The side panel owns every key while it is up: the grid behind it must not
     * move, and Left must not wander off to the navigation rail. */
    if (m_model.panel_open) {
        switch (k) {
        case KeyUp:   PanelMove(-1); break;
        case KeyDown: PanelMove(+1); break;
        case KeyAccept:
            if (m_panel_focus >= 0 && m_panel_focus < (int)m_model.panel_rows.size()) {
                m_choice = m_model.panel_rows[m_panel_focus].id.c_str();
                m_pending_action = EVO_PROVIDER_ACTION_CHOICE;
            }
            break;
        case KeyBack:
            m_choice = "panel:back";
            m_pending_action = EVO_PROVIDER_ACTION_CHOICE;
            break;
        default: break;
        }
        m_dirty = true;
        return true;
    }

    if (ShelvesActive()) return ShelfKey(k);

    switch (k) {
    case KeyPageUp: {
        int idx = FocusedRowIndex();
        if (m_row_offset >= EVO_PROVIDER_PAGE_SIZE) {
            SetPageOffset(m_row_offset - EVO_PROVIDER_PAGE_SIZE, idx >= 0 ? idx : 0);
            return true;
        }
        return true;
    }

    case KeyPageDown: {
        int idx = FocusedRowIndex();
        if (m_row_offset + EVO_PROVIDER_PAGE_SIZE < m_all_rows.size()) {
            SetPageOffset(m_row_offset + EVO_PROVIDER_PAGE_SIZE, idx >= 0 ? idx : 0);
            return true;
        }
        FetchMoreAndAdvance(idx >= 0 ? idx : 0);
        return true;
    }

    case KeyUp:
    case KeyDown:
    case KeyLeft:
    case KeyRight: {
        int idx = FocusedRowIndex();

        /* Deterministic 2x8 grid paging for Up/Down */
        const int cols = (int)EVO_PROVIDER_COLS;
        if (idx >= 0 && k == KeyDown) {
            const int n = (int)m_model.rows.size();
            /* Top row over a SHORT bottom row (6 cards, focus on column 3):
             * nothing sits directly below, so take the last card rather than
             * swallowing the press. */
            if (idx < cols && idx + cols >= n && n > cols) {
                FocusPublishedRow(n - 1);
                m_dirty = true;
                UpdateSelectedPreview();
                return true;
            }
            /* If on the bottom row (slot >= cols or slot + cols exceeds visible cards), press Down flips page */
            if (idx >= cols || idx + cols >= (int)m_model.rows.size()) {
                if (m_row_offset + EVO_PROVIDER_PAGE_SIZE < m_all_rows.size()) {
                    int target_col = idx % cols;
                    SetPageOffset(m_row_offset + EVO_PROVIDER_PAGE_SIZE, target_col);
                    return true;
                }
                /* At the bottom of what is loaded: the provider may have more. */
                FetchMoreAndAdvance(idx % cols);
                return true;
            }
        } else if (idx >= 0 && k == KeyUp) {
            /* If on the top row (slot < cols), press Up flips to previous page bottom row */
            if (idx < cols) {
                if (m_row_offset >= EVO_PROVIDER_PAGE_SIZE) {
                    int target_col = idx + cols;
                    SetPageOffset(m_row_offset - EVO_PROVIDER_PAGE_SIZE, target_col);
                    return true;
                }
                /* Already on page 0: stay on top row */
                return true;
            }
        }

        Rml::Input::KeyIdentifier id =
            (k == KeyUp)    ? Rml::Input::KI_UP :
            (k == KeyDown)  ? Rml::Input::KI_DOWN :
            (k == KeyLeft)  ? Rml::Input::KI_LEFT : Rml::Input::KI_RIGHT;

        /* A press that beats the first render: seed focus onto a row first,
         * otherwise this direction is swallowed and the screen reads as hung
         * for one press. */
        if (m_needs_initial_focus || !m_context->GetFocusElement()) {
            m_context->Update();
            SeedFocus();
            m_context->Update();
        }

        Rml::Element* before = m_context->GetFocusElement();
        if (!before) {
            SeedFocus();
            before = m_context->GetFocusElement();
        }

        m_context->ProcessKeyDown(id, 0);
        m_context->ProcessKeyUp(id, 0);
        m_dirty = true;
        UpdateSelectedPreview();

        if (k == KeyLeft && m_context->GetFocusElement() == before)
            return false;   /* -> the rail takes it */
        return true;
    }

    case KeyAccept: {
        if (m_needs_initial_focus || !m_context->GetFocusElement()) {
            m_context->Update();
            SeedFocus();
            m_context->Update();
        }

        /*
         * Click the focused element. RmlUi synthesises a click from Return for
         * form controls only, and a provider's rows are divs - so the click is
         * dispatched here, on whatever has focus, and the bundle's
         * data-event-click does the rest.
         */
        Rml::Element* f = m_context->GetFocusElement();
        if (!f) return true;
        Rml::Dictionary params;
        params["button"] = 0;
        f->DispatchEvent(Rml::EventId::Click, params);
        m_dirty = true;
        return true;
    }

    case KeyBack: {
        /* If USB playlist picker is active, Back returns to initial setup screen */
        if (m_is_usb_picker) {
            ShowSetupScreen();
            return true;
        }

        /* If search is active, Back clears search and restores current folder */
        if (!m_model.query.empty()) {
            Search("");
            return true;
        }

        /* Deeper than root: pop a folder level */
        if (!m_stack.empty()) {
            m_stack.pop_back();
            if (!m_crumbs.empty()) m_crumbs.pop_back();
            if (!m_pos_stack.empty()) {
                m_saved_offset = m_pos_stack.back().offset;
                m_saved_slot = m_pos_stack.back().slot;
                m_pos_stack.pop_back();
            }
            m_page = 0;
            RequestPage(m_stack.empty() ? "" : m_stack.back().c_str(), 0);
            return true;
        }

        /* At the root of a loaded source, Back leaves the provider. It used to
         * open the setup screen (and the USB list before that), and setup no
         * longer clears the source - so Back there returns here, and the two
         * would trap the user. Setup is under Options. */
        if (!m_model.empty && !m_setup_shown)
            return false;

        /* On the setup screen of a provider that still has its source (setup no
         * longer clears it): Back returns to that source instead of leaving. */
        if (m_setup_shown && m_provider->is_configured && m_provider->is_configured()) {
            m_page = 0;
            RequestPage("", 0);
            return true;
        }

        /* Already on the setup screen: do not consume, allow caller to exit to Main Menu */
        return false;
    }

    case KeySearch:
        if (!(m_provider->caps & EVO_PROVIDER_CAP_SEARCH)) return false;
        /* The keyboard context owns text entry; the screen manager opens it
         * and calls back with the term. Nothing to do here yet. */
        return false;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Frame                                                                     */
/* ------------------------------------------------------------------------- */

void EvoRmlProviderHost::BundleCallback(evo_bundle_status_t st,
                                        const evo_bundle_manifest_t* m, void* ud)
{
    EvoRmlProviderHost* self = (EvoRmlProviderHost*)ud;
    if (!self->IsOpen()) return;

    PROV_LOG("'%s' bundle refresh: %s  version='%s' files=%d bytes=%llu",
             self->m_provider_id.c_str(), evo_bundle_status_str(st),
             (m && m->version[0]) ? m->version : "-",
             m ? m->entry_count : 0,
             m ? (unsigned long long)m->total_bytes : 0ULL);

    if (st != EVO_BUNDLE_OK) {
        /*
         * A refresh failure is only worth surfacing when there is nothing else
         * on screen. If the cached bundle is already rendering, saying "download
         * failed" over a working screen is noise.
         */
        if (self->m_using_fallback)
            self->SetStatus(std::string("UI: ") + evo_bundle_status_str(st), true);
        return;
    }

    /*
     * Reopen ONLY when there is something new to show: the screen is on the
     * fallback skin, or the version that just landed differs from the one it is
     * rendering. "The cache was already current" also reports OK, and
     * reopening on that put the screen in a teardown/rebuild loop, because each
     * reopen kicks another refresh.
     */
    const bool same_version = m && m->version[0] &&
                              self->m_bundle_version == m->version;
    if (self->m_using_fallback || (m && m->version[0] && !same_version)) {
        std::string id = self->m_provider_id;
        int w = 1920, h = 1080;
        if (self->m_context) {
            Rml::Vector2i d = self->m_context->GetDimensions();
            w = d.x; h = d.y;
        }
        self->Close();
        self->Open(id.c_str(), w, h);
    }
}

/*
 * Act on a row the user activated. Called from Tick(), one frame after the Rml
 * event handler recorded it - see m_pending_activation.
 */
void EvoRmlProviderHost::ApplyPendingActivation()
{
    if (m_pending_activation.empty()) return;
    const std::string id = m_pending_activation;
    m_pending_activation.clear();

    const EvoProviderRow* hit = nullptr;
    for (const EvoProviderRow& r : m_model.rows)
        if (id == r.id.c_str()) { hit = &r; break; }
    if (!hit && ShelvesActive()) {
        const EvoProviderRow* sf = ShelfFocusedRow();
        if (sf && id == sf->id.c_str()) hit = sf;
    }
    /* The rows were replaced between the press and now - a refresh landing on
     * the same frame. Dropping it is right: acting on a stale id would open
     * something the user is no longer looking at. */
    if (!hit) return;

    /* If USB playlist picker is active, activating a row selects that playlist */
    if (m_is_usb_picker) {
        if (m_provider && m_provider->set_source &&
            m_provider->set_source(hit->id.c_str()) != 0) {
            /* Refused (Xtream wants an account, and a plain M3U has none). Stay
             * on the list and say why, rather than reloading a provider that is
             * still unconfigured - that bounced the user back to setup. */
            PROV_LOG("'%s' refused USB playlist '%s'", m_provider_id.c_str(), hit->id.c_str());
            if (toast)
                toast(m_provider->name, std::strcmp(m_provider->id, "xtream") == 0
                      ? "Not an Xtream playlist - open it under IPTV"
                      : "That playlist could not be opened");
            return;
        }
        m_is_usb_picker = false;
        if (m_provider && m_provider->set_source) {
            evo_provider_set_enabled(m_provider_id.c_str(), 1);
            m_stack.clear();
            m_pos_stack.clear();
            m_crumbs.clear();
            m_page = 0;
            RequestPage("", 0);
        }
        return;
    }

    /* A movie or a show opens its detail page; Play and Restart there come back as the selection below. */
    const bool bypass = m_detail_bypass;
    m_detail_bypass = false;
    if (!bypass && WantsDetail(*hit)) {
        OpenDetail(*hit);
        return;
    }

    PROV_LOG("'%s' activate '%s' (%s) title='%s'", m_provider_id.c_str(),
             hit->id.c_str(), hit->is_folder ? "folder" : "playable",
             hit->title.c_str());

    if (hit->is_folder) {
        m_pos_stack.push_back({m_row_offset, hit->index});
        m_stack.push_back(std::string(hit->id.c_str()));
        m_crumbs.push_back(std::string(hit->title.c_str()));
        m_page = 0;
        RequestPage(m_stack.back().c_str(), 0);
        return;
    }

    /* Playable: keep its art for Home (it is about to be in the history), then hand it to the player. */
    {
        const EvoShelfCard* sc = nullptr;
        if (ShelvesActive() && m_shelf_row >= 0 && m_shelf_row < (int)m_shelves.size()) {
            const EvoShelf& sh = m_shelves[m_shelf_row];
            if (sh.col >= 0 && sh.col < (int)sh.cards.size() && sh.cards[sh.col].id == hit->id)
                sc = &sh.cards[sh.col];
        }
        if (sc) evo_home_art_capture(m_provider_id.c_str(), hit->id.c_str(), sc->art_url.c_str(), sc->backdrop_url.c_str());
        else {
            const size_t a = m_row_offset + hit->index;
            if (a < m_art_urls.size())
                evo_home_art_capture(m_provider_id.c_str(), hit->id.c_str(), m_art_urls[a].c_str(), "");
        }
    }
    {
        std::string art = hit->art_url;
        const size_t a = m_row_offset + hit->index;
        if (art.empty() && a < m_art_urls.size()) art = m_art_urls[a];
        RememberPickContext(hit->title.c_str(), !hit->facts.empty() ? hit->facts : std::string(hit->subtitle.c_str()), art);
    }
    memset(&g_selection, 0, sizeof g_selection);
    snprintf(g_selection.provider_id, sizeof g_selection.provider_id,
             "%s", m_provider_id.c_str());
    snprintf(g_selection.item_id, sizeof g_selection.item_id,
             "%s", hit->id.c_str());
    snprintf(g_selection.title, sizeof g_selection.title,
             "%s", hit->title.c_str());
    g_selection.is_live = hit->is_live ? 1 : 0;
    g_selection.resume_sec = hit->resume_sec;
    g_selection_pending = true;
}

/* The last loaded page is on screen and the provider said there is more:
 * fetch the next page, and move onto it once it lands (ApplyItems). */
void EvoRmlProviderHost::FetchMoreAndAdvance(int col)
{
    if (!m_has_more || m_model.loading) return;
    m_advance_after_load = true;
    m_advance_col = col;
    RequestPage(m_stack.empty() ? "" : m_stack.back().c_str(), ++m_page);
}

void EvoRmlProviderHost::Tick()
{
    if (!IsOpen()) return;
    evo_bundle_poll();
    evo_provider_art_poll();
    /* Adopt whatever finished decoding, and ask for the next batch. */
    if (m_detail.open) PushDetailArt();
    if (ShelvesActive()) {
        PushShelfArt();
        PushShelfBackdrop();
        PushPanelArt();
        ApplyPendingActivation();
        return;
    }
    PushArtRequests();
    PushPanelArt();
    UpdateSelectedPreview();
    ApplyPendingActivation();
}

/*
 * NOTHING in here may log.
 *
 * This runs every frame, and PROV_LOG -> evo_bt writes to /mnt/usb0/evo.log
 * AND sceKernelDebugOutText on every call, with a 3 KB struct on the stack.
 * Nine such calls per frame produced 265,846 of 266,884 lines in one hardware
 * run - a 16.9 MB log, ~480 USB writes a second, in the render path. It buried
 * every other diagnostic and cost frame rate.
 *
 * Trace the once-per-open events (Open, RequestPage, the bundle refresh)
 * instead; those tell the same story and cost nothing.
 */
bool EvoRmlProviderHost::Render(uint32_t* framebuffer, int width, int height)
{
    if (!IsOpen() || !m_context) return false;

    EvoRenderBridge* r = EvoRmlApp::Instance().RenderBridge();
    if (!r) return false;

    /*
     * Own context, so nothing here touches EvoRmlApp's cached menu surface or
     * any other document's visibility - the same isolation the toast and
     * keyboard contexts rely on (#75).
     */
    r->SetFramebuffer(framebuffer);
    r->SetDimensions(width, height);
    m_context->Update();

    /*
     * The data-for elements exist now, so this is the first moment focus can
     * land on a row. One KI_TAB, which ElementDocument turns into
     * FindNextTabElement - the host never names an element. Then Update again,
     * so the :focus styling the provider's RCSS applies is in THIS frame
     * rather than the next one.
     */
    if (m_needs_initial_focus) {
        SeedFocus();
        m_context->Update();
    }

#if defined(EVO_AGC_DEVICE)
    /* EVO's own provider screens (the embedded documents and the fallback skin) are drawn by the kit. RmlUi stays the
     * logic here (data model, focus, events) and still lays the document out;
     * it just does not rasterise it. */
    if ((m_bundle_version == "embedded" || m_using_fallback) && RenderKit(width, height))
        return true;
#endif

    r->FrameBegin();
    m_context->Render();
    r->FrameEnd();
    return false;
}

#if defined(EVO_AGC_DEVICE)
bool EvoRmlProviderHost::RenderKit(int width, int height)
{
    EvoRenderBridge* r = EvoRmlApp::Instance().RenderBridge();
    if (!r || !EvoRmlApp::Instance().GlActive()) return false;

    static evo_hui_provider_params_t p;
    std::memset(&p, 0, sizeof(p));

    if (m_using_fallback)                    p.variant = EVO_HUI_PROVIDER_GENERIC;
    else if (m_model_name == "iptv")         p.variant = EVO_HUI_PROVIDER_LIVETV;
    else if (m_model_name == "xtream")       p.variant = EVO_HUI_PROVIDER_XTREAM;
    else if (m_model_name == "mediaserver")  p.variant = EVO_HUI_PROVIDER_MEDIASERVER;
    else                                     p.variant = EVO_HUI_PROVIDER_GENERIC;

    p.provider_name = m_model.provider_name.c_str();
    static std::string icon;
    icon = (m_provider && m_provider->icon && m_provider->icon[0])
         ? std::string("../icons/") + m_provider->icon : std::string();
    p.provider_icon = icon.c_str();
    /* In-provider OPTIONS hint */
    if (m_provider_id == "addons" || m_provider_id == "nuvio")
        p.options_hint = "Add & manage addons";
    else if (m_provider_id == "emby" || m_provider_id == "jellyfin")
        p.options_hint = "Edit server";
    else if (m_provider_id == "iptv")
        p.options_hint = "Playlist & guide";
    else if (m_provider_id == "xtream")
        p.options_hint = "Account";
    else
        p.options_hint = "";
    p.breadcrumb = m_model.breadcrumb.c_str();
    p.status = m_model.status.c_str();
    p.query = m_model.query.c_str();
    p.page_info = m_model.page_info.c_str();
    p.loading = m_model.loading;
    p.tuning = m_model.tuning;
    p.has_error = m_model.has_error;
    p.empty = m_model.empty;
    p.has_multiple_pages = m_model.has_multiple_pages;
    p.is_folder_level = m_model.is_folder_level;
    p.in_folder = m_model.in_folder;
    p.count = m_model.count;
    p.row_count = (int)std::min<size_t>(m_model.rows.size(), EVO_HUI_PROVIDER_ROWS);
    for (int i = 0; i < p.row_count; ++i) {
        const EvoProviderRow& row = m_model.rows[i];
        evo_hui_provider_row_t& o = p.rows[i];
        o.title = row.title.c_str();
        o.subtitle = row.subtitle.c_str();
        o.initial = row.initial.c_str();
        o.art_key = row.art.c_str();
        if (!row.art.empty()) o.art = r->MemoryTexturePixels(row.art, &o.art_w, &o.art_h);
        o.is_folder = row.is_folder;
        o.played = row.played;
        o.progress = row.progress;
        o.is_live = row.is_live;
        o.now = row.now.c_str();
        o.next = row.next.c_str();
    }
    p.focus = FocusedRowIndex();
    p.has_selected = m_model.has_selected;
    p.selected_is_folder = m_model.selected_is_folder;
    p.selected_played = m_model.selected_played;
    p.selected_progress = m_model.selected_progress;
    p.selected_title = m_model.selected_title.c_str();
    p.selected_subtitle = m_model.selected_subtitle.c_str();
    p.selected_initial = m_model.selected_initial.c_str();
    p.selected_overview = m_model.selected_overview.c_str();
    p.selected_resume = m_model.selected_resume.c_str();
    p.selected_art_key = m_model.selected_art.c_str();
    if (!m_model.selected_art.empty())
        p.selected_art = r->MemoryTexturePixels(m_model.selected_art, &p.selected_art_w, &p.selected_art_h);
    p.selected_num = m_model.selected_num.c_str();
    p.selected_tech = m_model.selected_tech.c_str();
    p.selected_now = m_model.selected_now.c_str();
    p.selected_next = m_model.selected_next.c_str();
    p.epg_status = m_model.epg_status.c_str();
    p.epg_setup = m_model.epg_setup;

    /* The setup cards are plain focusable elements: read which has the cursor. */
    p.setup_configured = m_model.setup_configured;
    p.setup_account = m_model.setup_account.c_str();
    p.setup_focus = -1;
    if (m_context) {
        if (Rml::Element* f = m_context->GetFocusElement()) {
            const Rml::String id = f->GetId();
            if (id == "tv-card-setup-url")          p.setup_focus = 0;
            else if (id == "tv-card-setup-usb")     p.setup_focus = 1;
            else if (id == "tv-card-setup-signout") p.setup_focus = 2;
        }
    }

    p.panel_open = m_model.panel_open;
    if (m_model.panel_open) {
        p.panel_crumb = m_model.panel_crumb.c_str();
        p.panel_eyebrow = m_model.panel_eyebrow.c_str();
        p.panel_title = m_model.panel_title.c_str();
        p.panel_sub = m_model.panel_sub.c_str();
        p.panel_note_b = m_model.panel_note_b.c_str();
        p.panel_note = m_model.panel_note.c_str();
        p.panel_accept = m_model.panel_accept.c_str();
        p.panel_back = m_model.panel_back.c_str();
        p.panel_row_count = (int)std::min<size_t>(m_model.panel_rows.size(), EVO_HUI_PROVIDER_PANEL_ROWS);
        for (int i = 0; i < p.panel_row_count; ++i) {
            const EvoPanelRow& pr = m_model.panel_rows[i];
            evo_hui_panel_row_t& o = p.panel_rows[i];
            o.title = pr.title.c_str();
            o.detail = pr.detail.c_str();
            o.badge = pr.badge.c_str();
            o.icon = pr.icon.c_str();
            o.radio = pr.radio;
            o.on = pr.on;
            o.warn = pr.warn;
            o.chevron = pr.chevron;
            o.badge_live = pr.badge_live;
            o.focused = pr.focused;
            o.initial = pr.initial.c_str();
            o.art_key = pr.art_key.c_str();
            if (!pr.art_key.empty() && evo_provider_art_ready(pr.art_key.c_str()))
                o.art = r->MemoryTexturePixels(Rml::String(pr.art_key.c_str()), &o.art_w, &o.art_h);
        }
    }

    if (ShelvesActive()) FillShelfParams(p, r);
    if (m_detail.open) FillDetailParams(p.detail, r);
    else if (m_pick_fresh) FillPickParams(p.detail, r);

    evo_hui_update_provider(&p);
    if (!evo_hui_render_provider(width, height)) return false;
    EvoRmlApp::Instance().NoteExternalDraw();
    return true;
}
#else
bool EvoRmlProviderHost::RenderKit(int, int) { return false; }
#endif

int EvoRmlProviderHost::TakeAction()
{
    int act = m_pending_action;
    m_pending_action = EVO_PROVIDER_ACTION_NONE;
    return act;
}

void EvoRmlProviderHost::ShowUsbPlaylists(const std::vector<std::string>& paths)
{
    m_saved_usb_playlists = paths;
    m_all_rows.clear();
    m_model.rows.clear();
    m_stack.clear();
    m_pos_stack.clear();
    m_crumbs.clear();
    m_art_urls.clear();
    m_art_keys.clear();
    m_is_usb_picker = true;
    m_setup_shown = false;

    for (const auto& path : paths) {
        size_t last_slash = path.find_last_of("/\\");
        std::string fname = (last_slash != std::string::npos) ? path.substr(last_slash + 1) : path;
        std::string fdir = (last_slash != std::string::npos) ? path.substr(0, last_slash) : "";

        EvoProviderRow r;
        r.id = Rml::String(path.c_str());
        r.title = Rml::String(fname.c_str());
        r.subtitle = Rml::String(fdir.c_str());
        r.is_folder = true;
        r.art = "";
        r.initial = "M3U";
        r.is_live = false;
        m_all_rows.push_back(r);
        m_art_urls.emplace_back("");
        m_art_keys.emplace_back();
    }

    m_model.is_folder_level = false;
    m_model.count = (int)paths.size();
    std::string crumb = std::string(m_provider ? m_provider->name : "IPTV") + " / USB Playlists";
    m_model.breadcrumb = Rml::String(crumb.c_str());
    m_model.loading = false;
    m_model.empty = false;
    m_model.query = "";
    m_row_window = paths.size();
    m_row_offset = 0;

    PublishRowWindow();
    SetStatus("Select an M3U playlist from USB to load channels", false);

    if (m_model_handle) {
        m_model_handle.DirtyVariable("empty");
        m_model_handle.DirtyVariable("rows");
        m_model_handle.DirtyVariable("count");
        m_model_handle.DirtyVariable("breadcrumb");
        m_model_handle.DirtyVariable("is_folder_level");
        m_model_handle.DirtyVariable("loading");
        m_model_handle.DirtyVariable("query");
    }

    m_needs_initial_focus = true;
    m_dirty = true;
}

void EvoRmlProviderHost::ShowPanel(const EvoProviderModel& page, int focus)
{
    m_model.panel_crumb   = page.panel_crumb;
    m_model.panel_eyebrow = page.panel_eyebrow;
    m_model.panel_title   = page.panel_title;
    m_model.panel_sub     = page.panel_sub;
    m_model.panel_note_b  = page.panel_note_b;
    m_model.panel_note    = page.panel_note;
    m_model.panel_accept  = page.panel_accept;
    m_model.panel_back    = page.panel_back;
    m_model.panel_rows    = page.panel_rows;
    m_model.panel_open    = true;
    m_choice.clear();
    m_panel_focus = (focus >= 0 && focus < (int)m_model.panel_rows.size()) ? focus : 0;
    PublishPanel();
}

void EvoRmlProviderHost::HidePanel()
{
    if (!m_model.panel_open) return;
    m_model.panel_open = false;
    m_model.panel_rows.clear();
    PublishPanel();
}

void EvoRmlProviderHost::PanelMove(int dir)
{
    int n = (int)m_model.panel_rows.size();
    if (n == 0) return;
    int f = m_panel_focus + dir;
    if (f < 0 || f >= n) return;                  /* no wrap, like Settings */
    m_panel_focus = f;
    PublishPanel();
}

void EvoRmlProviderHost::PublishPanel()
{
    for (int i = 0; i < (int)m_model.panel_rows.size(); ++i)
        m_model.panel_rows[i].focused = (i == m_panel_focus);
    if (m_model_handle) {
        m_model_handle.DirtyVariable("panel_open");
        m_model_handle.DirtyVariable("panel_crumb");
        m_model_handle.DirtyVariable("panel_eyebrow");
        m_model_handle.DirtyVariable("panel_title");
        m_model_handle.DirtyVariable("panel_sub");
        m_model_handle.DirtyVariable("panel_note_b");
        m_model_handle.DirtyVariable("panel_note");
        m_model_handle.DirtyVariable("panel_accept");
        m_model_handle.DirtyVariable("panel_back");
        m_model_handle.DirtyVariable("panel_rows");
    }
    m_dirty = true;
}

std::string EvoRmlProviderHost::TakeChoice()
{
    std::string c = m_choice;
    m_choice.clear();
    return c;
}

void EvoRmlProviderHost::ShowSetupScreen()
{
    m_saved_usb_playlists.clear();
    m_stack.clear();
    m_pos_stack.clear();
    m_crumbs.clear();
    m_all_rows.clear();
    m_model.rows.clear();
    m_art_urls.clear();
    m_art_keys.clear();
    m_row_window = 0;
    m_row_offset = 0;
    m_is_usb_picker = false;
    HidePanel();
    m_model.query = "";
    m_model.loading = false;
    m_model.tuning = false;
    m_model.empty = true;
    m_model.count = 0;
    m_model.is_folder_level = false;
    /* The stack was just cleared: without this, Options pressed inside a
     * group kept in_folder set and drew "NO CHANNELS HERE" over setup. */
    m_model.in_folder = false;
    m_model.breadcrumb = Rml::String(m_provider ? m_provider->name : "IPTV");
    SetStatus("", false);

    /*
     * Showing the choices must not remove anything. This used to call
     * set_source("") here, which for Xtream deletes the account: pressing Back at
     * the top level, or Options, signed the user out before they had chosen
     * anything. The source now changes only when a new one is entered or picked,
     * or on an explicit Sign out.
     */
    m_setup_shown = true;
    m_model.setup_configured = m_provider && m_provider->is_signed_in &&
                               m_provider->is_signed_in();
    m_model.setup_account = "";
    if (m_provider && m_provider->is_configured && m_provider->is_configured() &&
        m_provider->get_source) {
        /* Host only: get_source() carries the username and password. */
        std::string src = m_provider->get_source();
        size_t b = src.find("://");
        b = (b == std::string::npos) ? 0 : b + 3;
        size_t at = src.find('@', b), sl = src.find('/', b);
        if (at != std::string::npos && (sl == std::string::npos || at < sl)) b = at + 1;
        size_t e = src.find_first_of("/?", b);
        std::string host = src.substr(b, e == std::string::npos ? std::string::npos : e - b);
        if (src[0] == '/') {
            size_t s2 = src.find_last_of('/');
            host = src.substr(s2 + 1);
        }
        if (!host.empty())
            m_model.setup_account = Rml::String(("Using " + host + " - Circle to go back to it").c_str());
    }

    if (m_model_handle) {
        m_model_handle.DirtyVariable("setup_configured");
        m_model_handle.DirtyVariable("setup_account");
        m_model_handle.DirtyVariable("rows");
        m_model_handle.DirtyVariable("query");
        m_model_handle.DirtyVariable("loading");
        m_model_handle.DirtyVariable("tuning");
        m_model_handle.DirtyVariable("empty");
        m_model_handle.DirtyVariable("count");
        m_model_handle.DirtyVariable("is_folder_level");
        m_model_handle.DirtyVariable("in_folder");
        m_model_handle.DirtyVariable("breadcrumb");
    }

    m_needs_initial_focus = true;
    m_dirty = true;

    /* Update immediately so the DOM reflects query == '' && !in_folder
     * and displays #tv-setup right away, then seed focus onto the first card. */
    if (m_context) {
        m_context->Update();
        SeedFocus();
        m_context->Update();
    }
}

void EvoRmlProviderHost::OpenLevel(const char* id, const char* title)
{
    if (!IsOpen() || !id || !*id) return;
    const int idx = FocusedRowIndex();
    m_pos_stack.push_back({m_row_offset, idx >= 0 ? idx : 0});
    m_stack.push_back(std::string(id));
    m_crumbs.push_back(std::string(title ? title : ""));
    m_page = 0;
    RequestPage(m_stack.back().c_str(), 0);
}

void EvoRmlProviderHost::ReloadCurrentLevel()
{
    m_saved_offset = m_row_offset;
    m_saved_slot = FocusedRowIndex();
    const char* cur = m_stack.empty() ? "" : m_stack.back().c_str();
    RequestPage(cur, 0);
}

/* ------------------------------------------------------------------------- */
/* C entry points                                                            */
/* ------------------------------------------------------------------------- */

extern "C" {

void evo_rmlui_provider_show_setup(void)
{
    EvoRmlProviderHost::Instance().ShowSetupScreen();
}

void evo_rmlui_provider_show_panel(const evo_panel_t *p)
{
    if (!p) return;
    auto S = [](const char* s) { return Rml::String(s ? s : ""); };
    EvoProviderModel page;
    page.panel_crumb   = S(p->crumb);
    page.panel_eyebrow = S(p->eyebrow);
    page.panel_title   = S(p->title);
    page.panel_sub     = S(p->sub);
    page.panel_note_b  = S(p->note_b);
    page.panel_note    = S(p->note);
    page.panel_accept  = S(p->accept);
    page.panel_back    = S(p->back);
    for (int i = 0; i < p->n; ++i) {
        const evo_panel_row_t& r = p->rows[i];
        EvoPanelRow row;
        row.id = S(r.id); row.title = S(r.title); row.detail = S(r.detail);
        row.badge = S(r.badge); row.icon = S(r.icon);
        row.radio = r.radio != 0; row.on = r.on != 0; row.warn = r.warn != 0;
        row.chevron = r.chevron != 0; row.badge_live = r.badge_live != 0;
        row.art_url = r.art_url ? r.art_url : "";
        row.initial = r.initial ? r.initial : "";
        page.panel_rows.push_back(row);
    }
    EvoRmlProviderHost::Instance().ShowPanel(page, p->focus);
}

void evo_rmlui_provider_hide_panel(void)
{
    EvoRmlProviderHost::Instance().HidePanel();
}

int evo_rmlui_provider_panel_open(void)
{
    return EvoRmlProviderHost::Instance().PanelOpen() ? 1 : 0;
}

int evo_rmlui_provider_take_choice(char *out, size_t out_sz)
{
    std::string c = EvoRmlProviderHost::Instance().TakeChoice();
    if (c.empty() || !out || !out_sz) return 0;
    snprintf(out, out_sz, "%s", c.c_str());
    return 1;
}

int evo_rmlui_provider_take_action(void)
{
    return EvoRmlProviderHost::Instance().TakeAction();
}

int evo_rmlui_provider_open(const char *provider_id, int width, int height)
{
    return EvoRmlProviderHost::Instance().Open(provider_id, width, height) ? 1 : 0;
}

void evo_rmlui_provider_close(void)
{
    EvoRmlProviderHost::Instance().Close();
}

int evo_rmlui_provider_is_open(void)
{
    return EvoRmlProviderHost::Instance().IsOpen() ? 1 : 0;
}

void evo_rmlui_provider_tick(void)
{
    EvoRmlProviderHost::Instance().Tick();
}

int evo_rmlui_provider_render(uint32_t *framebuffer, int width, int height)
{
    return EvoRmlProviderHost::Instance().Render(framebuffer, width, height) ? 1 : 0;
}

int evo_rmlui_provider_needs_frame(void)
{
    return EvoRmlProviderHost::Instance().NeedsFrame() ? 1 : 0;
}

void evo_rmlui_provider_clear_frame(void)
{
    EvoRmlProviderHost::Instance().ClearFrameFlag();
}

int evo_rmlui_provider_key(int key)
{
    if (key < EvoRmlProviderHost::KeyUp || key > EvoRmlProviderHost::KeyPageDown)
        return 0;
    return EvoRmlProviderHost::Instance().HandleKey(
               (EvoRmlProviderHost::Key)key) ? 1 : 0;
}

void evo_rmlui_provider_set_loading(int loading, const char *status)
{
    EvoRmlProviderHost::Instance().SetLoading(loading != 0, status ? status : "");
}

void evo_rmlui_provider_set_tuning(int tuning)
{
    EvoRmlProviderHost::Instance().SetTuning(tuning != 0);
}

void evo_rmlui_provider_set_status(const char *status, int error)
{
    EvoRmlProviderHost::Instance().SetStatus(status ? status : "", error != 0);
}

void evo_rmlui_provider_search(const char *query)
{
    EvoRmlProviderHost::Instance().Search(query);
}

const char* evo_rmlui_provider_get_query(void)
{
    return EvoRmlProviderHost::Instance().CurrentQuery();
}

const char* evo_rmlui_provider_get_focused_title(void)
{
    const auto* r = EvoRmlProviderHost::Instance().GetFocusedRow();
    return r ? r->title.c_str() : "";
}

const char* evo_rmlui_provider_get_focused_id(void)
{
    const auto* r = EvoRmlProviderHost::Instance().GetFocusedRow();
    return r ? r->id.c_str() : "";
}

int evo_rmlui_provider_get_focused_is_folder(void)
{
    const auto* r = EvoRmlProviderHost::Instance().GetFocusedRow();
    return r ? (r->is_folder ? 1 : 0) : 0;
}

void evo_rmlui_provider_get_focused_flags(int *played, int *favorite)
{
    const auto* r = EvoRmlProviderHost::Instance().GetFocusedRow();
    if (played)   *played   = r && r->played   ? 1 : 0;
    if (favorite) *favorite = r && r->favorite ? 1 : 0;
}

const char* evo_rmlui_provider_current_parent(void)
{
    return EvoRmlProviderHost::Instance().CurrentParent();
}

void evo_rmlui_provider_open_level(const char *id, const char *title)
{
    EvoRmlProviderHost::Instance().OpenLevel(id, title);
}

void evo_rmlui_provider_reload(void)
{
    EvoRmlProviderHost::Instance().ReloadCurrentLevel();
}

} /* extern "C" */

/* ------------------------------------------------------------------------- */
/* Shelves: the media-server home as rows of posters                          */
/* ------------------------------------------------------------------------- */
/*
 * The root of Emby / Jellyfin is a list of "rows" (Continue Watching, Next Up,
 * Recently Added, Favourites, one per library). Shown as one folder level it
 * made the user open each. Here each becomes a shelf: its first page of posters
 * is fetched, in turn, and the screen is a stack of horizontal rows. RmlUi is
 * not used for the focus - the host keeps (shelf, card) itself and the kit draws
 * it. Opening a card (or a shelf's "See all") pushes a normal level, so Back
 * returns to the shelves.
 */

static const size_t kShelfCards = 24;      /* posters per shelf before "See all" */
static const int    kMaxShelves = 24;      /* real shelves; the rest of the folders gather in "More" */

bool EvoRmlProviderHost::ShelfCapable() const
{
    /* Every provider with a catalog: one layout, only the rows differ. */
    return m_provider && m_provider->list_catalog && (m_provider->caps & EVO_PROVIDER_CAP_CATALOG);
}

bool EvoRmlProviderHost::ShelvesActive() const
{
    return m_shelves_on && m_stack.empty() && m_model.query.empty() && !m_setup_shown &&
           !m_is_usb_picker && ShelvesWithCards() > 0;
}

int EvoRmlProviderHost::ShelvesWithCards() const
{
    int n = 0;
    for (const EvoShelf& s : m_shelves) if (!s.cards.empty()) ++n;
    return n;
}

int EvoRmlProviderHost::NextShelf(int from, int dir) const
{
    for (int i = from + dir; i >= 0 && i < (int)m_shelves.size(); i += dir)
        if (!m_shelves[i].cards.empty()) return i;
    return -1;
}

/*
 * The root reply. Returns false when it is not a list of rows to turn into
 * shelves (no folders, or mostly channels), so the caller shows the plain level.
 * Plex's root is its servers: each is opened first, and the libraries inside
 * become the shelves.
 */
bool EvoRmlProviderHost::BeginShelves(const evo_provider_item_t* items, int count)
{
    int folders = 0;
    for (int i = 0; i < count; ++i) if (items[i].is_folder) ++folders;
    if (folders == 0 || folders < count - folders) return false;

    if (m_provider_id == "plex" && !std::strncmp(items[0].id, "srv:", 4)) {
        m_expand_servers.clear();
        m_expand_items.clear();
        for (int i = 0; i < count; ++i)
            if (items[i].is_folder && !std::strncmp(items[i].id, "srv:", 4)) m_expand_servers.push_back(items[i].id);
        m_expand_gen = ++m_shelf_gen;
        m_model.loading = true;
        ExpandNextServer();
        return true;
    }
    /* Xtream's root is Live / Movies / Series: open each and make its categories the shelves. */
    if (m_provider_id == "xtream" && !std::strcmp(items[0].id, "live")) {
        m_expand_servers.clear();
        m_expand_items.clear();
        for (int i = 0; i < count; ++i)
            if (items[i].is_folder) m_expand_servers.push_back(items[i].id);
        m_expand_gen = ++m_shelf_gen;
        m_model.loading = true;
        ExpandNextServer();
        return true;
    }
    BuildShelves(items, count);
    return true;
}

void EvoRmlProviderHost::ExpandNextServer()
{
    struct Ctx { EvoRmlProviderHost* host; unsigned gen; std::string section; };
    while (!m_expand_servers.empty()) {
        const std::string id = m_expand_servers.front();
        m_expand_servers.erase(m_expand_servers.begin());
        Ctx* c = new Ctx{this, m_expand_gen, id};
        if (m_provider->list_catalog(id.c_str(), 0, &EvoRmlProviderHost::ExpandCallback, c) == 0) return;
        delete c;
    }
    FinishExpand();
}

void EvoRmlProviderHost::ExpandCallback(int ok, const evo_provider_item_t* items, int count, int, void* ud)
{
    struct Ctx { EvoRmlProviderHost* host; unsigned gen; std::string section; };
    Ctx* c = static_cast<Ctx*>(ud);
    EvoRmlProviderHost* self = c->host;
    const unsigned gen = c->gen;
    const std::string section = c->section;
    delete c;
    if (!self->IsOpen() || gen != self->m_expand_gen) return;
    if (self->m_provider_id == "xtream") {
        /* The first few categories of each section become channel rows; one closing row
         * of the section's category cards (with "See all") keeps the rest reachable. */
        static const int kPerSection = 8;
        const char* label = section == "live" ? "Live" : section == "vod" ? "Movies" : "Series";
        int taken = 0;
        if (ok)
            for (int i = 0; i < count && taken < kPerSection; ++i) {
                if (!items[i].is_folder) continue;
                evo_provider_item_t it = items[i];
                std::snprintf(it.title, sizeof it.title, "%s: %s", label, items[i].title);
                self->m_expand_items.push_back(it);
                ++taken;
            }
        if (ok && count > taken) {
            evo_provider_item_t more;
            std::memset(&more, 0, sizeof more);
            std::snprintf(more.id, sizeof more.id, "%s", section.c_str());
            std::snprintf(more.title, sizeof more.title, "%s: all categories", label);
            more.is_folder = 1;
            more.kind = EVO_MEDIA_FOLDER;
            self->m_expand_items.push_back(more);
        }
        self->ExpandNextServer();
        return;
    }
    if (ok)
        for (int i = 0; i < count; ++i)
            if (items[i].is_folder) self->m_expand_items.push_back(items[i]);
    self->ExpandNextServer();
}

void EvoRmlProviderHost::FinishExpand()
{
    m_model.loading = false;
    if (m_expand_items.empty()) {
        m_shelves.clear();
        m_shelves_on = false;
        m_model.empty = true;
        if (m_model_handle) { m_model_handle.DirtyVariable("loading"); m_model_handle.DirtyVariable("empty"); }
        SetStatus("No libraries found", false);
        m_dirty = true;
        return;
    }
    std::vector<evo_provider_item_t> libs;
    libs.swap(m_expand_items);
    BuildShelves(libs.data(), (int)libs.size());
}

void EvoRmlProviderHost::BuildShelves(const evo_provider_item_t* items, int count)
{
    std::vector<EvoShelf> fresh;
    std::vector<int> spill;                             /* folders past the cap */
    for (int i = 0; i < count; ++i) {
        if (!items[i].is_folder) continue;
        if ((int)fresh.size() >= kMaxShelves) { spill.push_back(i); continue; }
        EvoShelf s;
        s.parent = items[i].id;
        s.title = display_text(items[i].title);
        /* Two rows with one name (a catalog for movies and one for series): add what tells them apart. */
        int same = 0;
        for (int j = 0; j < count; ++j)
            if (items[j].is_folder && !std::strcmp(items[j].title, items[i].title)) ++same;
        const char* dot = std::strrchr(items[i].subtitle, '\xC2');
        if (same > 1 && dot && dot[1] == '\xB7' && dot[2] == ' ')
            s.title += Rml::String(" - ") + display_text(dot + 3);
        /* The same shelf as before keeps its posters and place until the new ones land. */
        for (const EvoShelf& o : m_shelves) {
            if (o.parent == s.parent) {
                s.cards = o.cards;
                s.col = o.col;
                s.first = o.first;
                break;
            }
        }
        fresh.push_back(std::move(s));
    }
    if (!spill.empty()) {
        /* One closing shelf of the remaining folders, so every one stays reachable. */
        std::vector<evo_provider_item_t> more;
        for (int i : spill) more.push_back(items[i]);
        EvoShelf s;
        s.title = "More";
        s.fresh = true;
        for (const EvoShelf& o : m_shelves) if (o.parent.empty()) { s.col = o.col; s.first = o.first; }
        AddShelfCards(s, more.data(), (int)more.size(), 0);
        fresh.push_back(std::move(s));
    }
    const std::string focused = (m_shelf_row >= 0 && m_shelf_row < (int)m_shelves.size())
                              ? m_shelves[m_shelf_row].parent : std::string();
    m_shelves.swap(fresh);
    m_shelves_on = true;
    m_shelf_moved = !focused.empty();      /* coming back keeps the place; a fresh home follows the first row to load */
    m_shelf_row = 0;
    for (int i = 0; i < (int)m_shelves.size(); ++i)
        if (!focused.empty() && m_shelves[i].parent == focused) m_shelf_row = i;
    if (m_shelf_row < (int)m_shelves.size() && m_shelves[m_shelf_row].cards.empty()) {
        int n = NextShelf(m_shelf_row, +1);
        if (n < 0) n = NextShelf(m_shelf_row, -1);
        if (n >= 0) m_shelf_row = n;
    }
    ++m_shelf_gen;
    m_model.empty = m_shelves.empty();
    if (m_model_handle) m_model_handle.DirtyVariable("empty");
    SetStatus("", false);
    SyncShelfFocus();
    LoadNextShelf();
    m_dirty = true;
}

/* Shelves asked for at once. One at a time, a home of eight shelves on a server
 * that takes a second per answer stood empty for eight seconds. */
static constexpr int kShelfFetches = 4;

void EvoRmlProviderHost::LoadNextShelf()
{
    struct Ctx { EvoRmlProviderHost* host; unsigned gen; int index; };
    /* Counted afresh each time: a provider that answers inside list_catalog has
     * already finished (and started the next) by the time the call returns. */
    auto out = [this] {
        int n = 0;
        for (const EvoShelf& s : m_shelves) if (s.loading) ++n;
        return n;
    };
    const unsigned gen = m_shelf_gen;
    /* Xtream has a shelf per category, a request each: fetch only the ones near the focus
     * (all of them while nothing has landed yet, so an empty run cannot stall the home). */
    const bool lazy = m_provider_id == "xtream" && ShelvesWithCards() > 0;
    const int last = m_shelf_row + 3;
    for (int i = 0; i < (int)m_shelves.size() && out() < kShelfFetches; ++i) {
        if (lazy && i > last) break;
        if (m_shelves[i].fresh || m_shelves[i].loading || m_shelves[i].parent.empty()) continue;
        Ctx* c = new Ctx{this, gen, i};
        m_model.loading = true;
        m_shelves[i].loading = true;                    /* before the call: a provider may answer inside it */
        const std::string parent = m_shelves[i].parent;
        evo_provider_set_row_limit((int)kShelfCards);
        const int rc = m_provider->list_catalog(parent.c_str(), 0, &EvoRmlProviderHost::ShelfCallback, c);
        evo_provider_set_row_limit(0);
        if (rc == 0) {
            if (gen != m_shelf_gen) return;             /* answered inside and the home changed */
            continue;                                   /* its callback loads the next */
        }
        delete c;
        if (gen != m_shelf_gen || i >= (int)m_shelves.size()) return;
        m_shelves[i].loading = false;
        m_shelves[i].fresh = true;                      /* refused: leave it as it was */
    }
    if (out() > 0) return;
    m_model.loading = false;
    if (m_model_handle) m_model_handle.DirtyVariable("loading");
    if (!m_shelves.empty() && ShelvesWithCards() == 0) {
        m_model.empty = true;
        if (m_model_handle) m_model_handle.DirtyVariable("empty");
    }
    m_dirty = true;
}

void EvoRmlProviderHost::ShelfCallback(int ok, const evo_provider_item_t* items, int count,
                                       int has_more, void* ud)
{
    struct Ctx { EvoRmlProviderHost* host; unsigned gen; int index; };
    Ctx* c = static_cast<Ctx*>(ud);
    EvoRmlProviderHost* self = c->host;
    const unsigned gen = c->gen;
    const int index = c->index;
    delete c;
    if (!self->IsOpen() || gen != self->m_shelf_gen || index >= (int)self->m_shelves.size()) return;

    EvoShelf& s = self->m_shelves[index];
    s.fresh = true;
    s.loading = false;
    if (ok) self->AddShelfCards(s, items, count, has_more);
    if (!self->m_shelf_moved) {
        const int first = self->NextShelf(-1, +1);
        if (first >= 0) self->m_shelf_row = first;
    }
    if (self->m_shelf_row >= 0 && self->m_shelf_row < (int)self->m_shelves.size() &&
        self->m_shelves[self->m_shelf_row].cards.empty()) {
        int n = self->NextShelf(self->m_shelf_row, +1);
        if (n < 0) n = self->NextShelf(self->m_shelf_row, -1);
        if (n >= 0) self->m_shelf_row = n;
    }
    self->SyncShelfFocus();
    self->m_dirty = true;
    self->LoadNextShelf();
}

void EvoRmlProviderHost::AddShelfCards(EvoShelf& s, const evo_provider_item_t* items, int count, int has_more)
{
    s.cards.clear();
    for (int i = 0; i < count && s.cards.size() < kShelfCards; ++i) {
        const evo_provider_item_t& it = items[i];
        EvoShelfCard c;
        c.id = Rml::String(it.id);
        c.title = display_text(it.title);
        c.subtitle = display_text(it.subtitle);
        c.overview = display_text(it.overview);
        c.rating = it.rating;
        c.cert = it.cert;
        c.facts = it.facts;
        c.art_url = it.art_url;
        evo_provider_art_set_fallback(it.art_url, it.art_alt_url);
        if (!it.is_folder && !it.is_live && (it.resume_pos_sec > 0 || it.played))
            evo_home_art_capture(m_provider_id.c_str(), it.id, it.art_url, it.backdrop_url);
        c.backdrop_url = it.backdrop_url;
        c.logo_url = it.logo_url;
        c.is_folder = it.is_folder != 0;
        c.is_live = it.is_live != 0;
        c.detail = it.detail;
        c.now = display_text(it.now_title);
        c.next = display_text(it.next_title);
        c.played = it.played != 0;
        c.favorite = it.favorite != 0;
        c.resume_sec = it.resume_pos_sec > 0 ? (long long)it.resume_pos_sec : 0;
        c.duration_sec = it.duration_sec;
        if (it.resume_pos_sec > 0 && it.duration_sec > 0) {
            long long pct = (long long)it.resume_pos_sec * 100 / (long long)it.duration_sec;
            c.progress = (int)(pct < 1 ? 1 : (pct > 99 ? 99 : pct));
        }
        const char* t0 = c.title.c_str();
        if (t0[0] >= 0x20 && (unsigned char)t0[0] < 0x80) {
            char ini[2] = { t0[0], 0 };
            if (ini[0] >= 'a' && ini[0] <= 'z') ini[0] = (char)(ini[0] - 'a' + 'A');
            c.initial = Rml::String(ini);
        }
        s.cards.push_back(std::move(c));
    }
    /* A library, or a row with more behind it, ends in "See all". */
    if (!s.cards.empty() && !s.parent.empty() && (has_more || count > (int)kShelfCards || s.parent.compare(0, 4, "lib:") == 0)) {
        EvoShelfCard all;
        all.id = Rml::String(s.parent.c_str());
        all.title = s.title;
        all.is_folder = true;
        all.see_all = true;
        s.cards.push_back(std::move(all));
    }
    if (s.col >= (int)s.cards.size()) s.col = s.cards.empty() ? 0 : (int)s.cards.size() - 1;
    if (s.first > s.col) s.first = s.col;
}

/* The focused card, as the row type the rest of the host (and the screen's
 * Triangle menu) already understands. */
const EvoProviderRow* EvoRmlProviderHost::ShelfFocusedRow() const
{
    if (m_shelf_row < 0 || m_shelf_row >= (int)m_shelves.size()) return nullptr;
    const EvoShelf& s = m_shelves[m_shelf_row];
    if (s.col < 0 || s.col >= (int)s.cards.size()) return nullptr;
    const EvoShelfCard& c = s.cards[s.col];
    EvoProviderRow& r = m_shelf_focus_row;
    /* Callers hold the c_str() of id and title across the next call (the Triangle menu
     * reads the id, then the title), so a string is only touched when the card changed. */
    if (r.id != c.id) r.id = c.id;
    if (r.title != c.title) r.title = c.title;
    if (r.subtitle != c.subtitle) r.subtitle = c.subtitle;
    if (r.overview != c.overview) r.overview = c.overview;
    r.is_folder = c.is_folder;
    r.is_live = c.is_live;
    if (r.now != c.now) r.now = c.now;
    if (r.next != c.next) r.next = c.next;
    r.played = c.played;
    r.favorite = c.favorite;
    r.progress = c.progress;
    r.resume_sec = c.resume_sec;
    r.rating = c.rating;
    r.cert = c.cert;
    r.facts = c.facts;
    r.art_url = c.art_url;
    r.duration_sec = c.duration_sec;
    r.detail = c.detail;
    return &r;
}

void EvoRmlProviderHost::SyncShelfFocus()
{
    if (m_shelf_row < 0 || m_shelf_row >= (int)m_shelves.size()) return;
    EvoShelf& s = m_shelves[m_shelf_row];
    const int visible = 8;
    if (s.col < s.first) s.first = s.col;
    if (s.col >= s.first + visible) s.first = s.col - visible + 1;
    if (s.first < 0) s.first = 0;
}

bool EvoRmlProviderHost::ShelfKey(Key k)
{
    if (m_shelf_row < 0 || m_shelf_row >= (int)m_shelves.size()) return k != KeyBack;
    EvoShelf& s = m_shelves[m_shelf_row];
    if (k != KeyBack && k != KeySearch) m_shelf_moved = true;
    switch (k) {
    case KeyUp: {
        const int n = NextShelf(m_shelf_row, -1);
        if (n >= 0) { m_shelf_row = n; SyncShelfFocus(); }
        break;
    }
    case KeyDown: {
        const int n = NextShelf(m_shelf_row, +1);
        if (n >= 0) { m_shelf_row = n; SyncShelfFocus(); LoadNextShelf(); }
        break;
    }
    case KeyLeft:
        if (s.col <= 0) return false;                  /* the rail takes it */
        --s.col;
        SyncShelfFocus();
        break;
    case KeyRight:
        if (s.col + 1 < (int)s.cards.size()) { ++s.col; SyncShelfFocus(); }
        break;
    case KeyPageUp:
        s.col = std::max(0, s.col - 6);
        SyncShelfFocus();
        break;
    case KeyPageDown:
        s.col = std::min((int)s.cards.size() - 1, s.col + 6);
        SyncShelfFocus();
        break;
    case KeyAccept:
        if (const EvoProviderRow* f = ShelfFocusedRow()) m_pending_activation = std::string(f->id.c_str());
        break;
    case KeyBack:
    case KeySearch:
        return false;
    }
    m_dirty = true;
    return true;
}

/* The headshots of an open details panel, through the same art queue as the posters. */
void EvoRmlProviderHost::PushPanelArt()
{
    if (!m_model.panel_open) return;
    for (EvoPanelRow& r : m_model.panel_rows) {
        if (r.art_url.empty()) continue;
        if (!r.art_key.empty()) continue;
        char key[128];
        const int rc = evo_provider_art_request(m_provider_id.c_str(), r.art_url.c_str(), key, sizeof key);
        if (rc == -2) return;                  /* queue full: ask again next tick */
        if (rc < 0) { r.art_url.clear(); continue; }   /* refused: the letter stays */
        r.art_key = key;
        m_dirty = true;
    }
    for (const EvoPanelRow& r : m_model.panel_rows)     /* a headshot that has just landed */
        if (!r.art_key.empty() && evo_provider_art_ready(r.art_key.c_str())) { m_dirty = true; break; }
}

void EvoRmlProviderHost::PushShelfArt()
{
    bool queue_full = false;
    bool changed = false;
    auto want = [&](EvoShelf& s, int from, int to) {
        for (int i = std::max(0, from); i < to && i < (int)s.cards.size() && !queue_full; ++i) {
            EvoShelfCard& c = s.cards[i];
            if (!c.art.empty() || c.art_url.empty() || c.see_all) continue;
            if (!c.art_key.empty()) {
                if (evo_provider_art_ready(c.art_key.c_str())) { c.art = Rml::String(c.art_key.c_str()); changed = true; }
                continue;
            }
            char key[128];
            const int rc = evo_provider_art_request(m_provider_id.c_str(), c.art_url.c_str(), key, sizeof key);
            if (rc == -2) { queue_full = true; return; }
            if (rc < 0) { c.art_url.clear(); continue; }
            c.art_key = key;
            if (rc == 1) { c.art = Rml::String(key); changed = true; }
        }
    };
    /* The focused shelf first, then the ones around it. */
    static const int order[] = { 0, 1, -1, 2 };
    for (int d : order) {
        const int r = m_shelf_row + d;
        if (r < 0 || r >= (int)m_shelves.size()) continue;
        EvoShelf& s = m_shelves[r];
        want(s, s.first - 1, s.first + 11);
    }
    if (changed) m_dirty = true;
}

/*
 * The wide image (and title logo) behind the shelves, for the focused card. It
 * is asked for only after the cursor has rested ~0.45 s on a card, so flicking
 * along a row does not queue a download per card; the last one that finished
 * stays on screen until the next lands, so the background never blinks off.
 */
void EvoRmlProviderHost::PushShelfBackdrop()
{
    using namespace std::chrono;
    const long long now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    const EvoShelfCard* c = nullptr;
    if (m_shelf_row >= 0 && m_shelf_row < (int)m_shelves.size()) {
        const EvoShelf& s = m_shelves[m_shelf_row];
        if (s.col >= 0 && s.col < (int)s.cards.size()) c = &s.cards[s.col];
    }
    const std::string id = c ? std::string(c->id.c_str()) + "|" + std::to_string(m_shelf_row) : std::string();
    if (id != m_bd_id) {
        m_bd_id = id;
        m_bd_since_ms = now;
        m_bd_url = c ? c->backdrop_url : std::string();
        m_logo_url = c ? c->logo_url : std::string();
        m_bd_key.clear();
        m_logo_key.clear();
        if (m_bd_url.empty()) m_bd_art.clear();
        if (m_logo_url.empty()) m_logo_art.clear();
        return;
    }
    if (now - m_bd_since_ms < 450) return;
    auto step = [&](const std::string& url, std::string& key, Rml::String& art) {
        if (url.empty()) return;
        if (!key.empty()) {
            if (evo_provider_art_ready(key.c_str()) && art != Rml::String(key.c_str())) {
                art = Rml::String(key.c_str());
                m_dirty = true;
            }
            return;
        }
        char k[128];
        const int rc = evo_provider_art_request(m_provider_id.c_str(), url.c_str(), k, sizeof k);
        if (rc < 0) return;                    /* full or refused: no background for this one */
        key = k;
        if (rc == 1) { art = Rml::String(k); m_dirty = true; }
    };
    step(m_bd_url, m_bd_key, m_bd_art);
    step(m_logo_url, m_logo_key, m_logo_art);

    /* The cast is not in a catalog list: ask the provider's details panel once the cursor rests. */
    if (c && c->cast_state == 0 && m_provider && m_provider->panel) {
        struct Ctx { EvoRmlProviderHost* host; unsigned gen; int shelf; std::string id; };
        EvoShelfCard& card = m_shelves[m_shelf_row].cards[m_shelves[m_shelf_row].col];
        card.cast_state = 1;
        Ctx* x = new Ctx{this, m_shelf_gen, m_shelf_row, std::string(card.id.c_str())};
        if (m_provider->panel(x->id.c_str(), "details", &EvoRmlProviderHost::CastCallback, x) != 0) {
            card.cast_state = 2;
            delete x;
        }
    }
}

void EvoRmlProviderHost::CastCallback(int ok, const evo_provider_row_t* rows, int count, void* ud)
{
    struct Ctx { EvoRmlProviderHost* host; unsigned gen; int shelf; std::string id; };
    Ctx* x = static_cast<Ctx*>(ud);
    EvoRmlProviderHost* self = x->host;
    const unsigned gen = x->gen;
    const int shelf = x->shelf;
    const std::string id = x->id;
    delete x;
    if (!self->IsOpen() || gen != self->m_shelf_gen || shelf < 0 || shelf >= (int)self->m_shelves.size()) return;
    for (EvoShelfCard& c : self->m_shelves[shelf].cards) {
        if (id != c.id.c_str()) continue;
        c.cast_state = 2;
        if (!ok) break;
        /* The rows that are facts have these titles; every other row is a person. */
        static const char* const kFacts[] = { "Rating", "Genres", "Studio", "Director", "Tagline" };
        std::string cast;
        int n = 0;
        for (int i = 0; i < count && n < 5; ++i) {
            bool fact = false;
            for (const char* f : kFacts) fact = fact || !std::strcmp(rows[i].title, f);
            if (fact || !rows[i].title[0]) continue;
            cast += (n++ ? ", " : "");
            cast += rows[i].title;
        }
        c.cast = cast;
        self->m_dirty = true;
        break;
    }
}

void EvoRmlProviderHost::FillShelfParams(evo_hui_provider_params_t& p, EvoRenderBridge* r)
{
    p.shelves_active = 1;
    p.item_menu = m_provider && (m_provider->item_action || m_provider->panel) ? 1 : 0;
    p.shelf_total = ShelvesWithCards();
    p.empty = 0;

    /* The window: the focused shelf, the one above, and two below. */
    std::vector<int> have;                 /* indices of shelves with cards */
    int focus_abs = 0;
    for (int i = 0; i < (int)m_shelves.size(); ++i) {
        if (m_shelves[i].cards.empty()) continue;
        if (i == m_shelf_row) focus_abs = (int)have.size();
        have.push_back(i);
    }
    p.shelf_focus_abs = focus_abs;
    int from = std::max(0, focus_abs - 1);
    for (int a = from; a < (int)have.size() && p.shelf_count < EVO_HUI_SHELVES; ++a) {
        const EvoShelf& s = m_shelves[have[a]];
        evo_hui_shelf_t& o = p.shelves[p.shelf_count++];
        o.title = s.title.c_str();
        o.abs_index = a;
        o.total_cards = (int)s.cards.size() - ((!s.cards.empty() && s.cards.back().see_all) ? 1 : 0);
        o.first_card = std::max(0, s.first - 1);
        o.scroll_card = s.first;
        o.focus_card = (have[a] == m_shelf_row) ? s.col : -1;
        for (int k = o.first_card; k < (int)s.cards.size() && o.card_count < EVO_HUI_SHELF_CARDS; ++k) {
            const EvoShelfCard& c = s.cards[k];
            evo_hui_provider_row_t& d = o.cards[o.card_count++];
            d.title = c.title.c_str();
            d.subtitle = c.subtitle.c_str();
            d.initial = c.initial.c_str();
            d.art_key = c.art.c_str();
            if (!c.art.empty()) d.art = r->MemoryTexturePixels(c.art, &d.art_w, &d.art_h);
            d.is_folder = c.is_folder;
            d.played = c.played;
            d.progress = c.progress;
            d.see_all = c.see_all;
            d.is_live = c.is_live;
            d.now = c.now.c_str();
            d.next = c.next.c_str();
        }
    }

    if (!m_bd_art.empty())
        p.backdrop = r->MemoryTexturePixels(m_bd_art, &p.backdrop_w, &p.backdrop_h);
    p.backdrop_key = m_bd_art.c_str();
    if (!m_logo_art.empty())
        p.logo = r->MemoryTexturePixels(m_logo_art, &p.logo_w, &p.logo_h);
    p.logo_key = m_logo_art.c_str();

    /* The focused card, for the title line. */
    static std::string t, sub, resume;
    if (const EvoProviderRow* f = ShelfFocusedRow()) {
        t = f->title.c_str();
        sub = f->subtitle.c_str();
        if (f->is_live && !f->now.empty()) {
            sub = std::string("Now: ") + f->now.c_str();
            if (!f->next.empty()) sub += std::string("   Next: ") + f->next.c_str();
        }
        resume = f->resume_sec > 0 ? std::string("Resume from ") + format_duration(f->resume_sec).c_str() : std::string();
        p.has_selected = 1;
        p.selected_title = t.c_str();
        p.selected_subtitle = sub.c_str();
        p.selected_resume = resume.c_str();
        /* the Details block (the kit draws it when there is a description) */
        const EvoShelfCard& card = m_shelves[m_shelf_row].cards[m_shelves[m_shelf_row].col];
        p.selected_overview = card.overview.c_str();
        p.selected_rating = card.rating.c_str();
        p.selected_cert = card.cert.c_str();
        p.selected_cast = card.cast.c_str();
        if (!card.facts.empty()) p.selected_subtitle = card.facts.c_str();
        p.selected_is_folder = f->is_folder;
        p.selected_played = f->played;
        p.selected_progress = f->progress;
    }
}

/* ------------------------------------------------------------------------- */
/* The movie / show detail page (#221)                                       */
/* ------------------------------------------------------------------------- */
/*
 * Cross on a movie or a show opens this instead of playing. The page is built
 * from the card that was focused (overview, rating, facts, poster); the cast
 * comes from the provider's "details" panel and the versions from the same
 * resolve the stream picker uses, each asked for once. Play and Restart go out
 * as the selection a card press would make, so the player starts through
 * ProviderHostScreen::startSelected like any other item.
 */

bool EvoRmlProviderHost::WantsDetail(const EvoProviderRow& r) const
{
    /* A provider with no facts to show (IPTV) keeps opening straight into playback. */
    if (!m_provider || !m_provider->panel || r.is_live) return false;
    return r.detail == 1 || r.detail == 2;
}

void EvoRmlProviderHost::OpenDetail(const EvoProviderRow& r)
{
    m_detail = DetailState();
    DetailState& d = m_detail;
    d.open = true;
    d.gen = ++m_detail_gen;
    d.item_id = r.id.c_str();
    d.title = r.title.c_str();
    d.meta = !r.facts.empty() ? r.facts : std::string(r.subtitle.c_str());
    d.rating = r.rating;
    d.cert = r.cert;
    d.overview = r.overview.c_str();
    d.art_url = r.art_url;
    d.is_show = r.detail == 2;
    d.resume_sec = r.resume_sec;
    if (d.art_url.empty() && !m_art_urls.empty()) {
        const size_t a = m_row_offset + (size_t)r.index;
        if (a < m_art_urls.size()) d.art_url = m_art_urls[a];
    }
    if (ShelvesActive() && m_shelf_row >= 0 && m_shelf_row < (int)m_shelves.size()) {
        const EvoShelf& sh = m_shelves[m_shelf_row];
        if (sh.col >= 0 && sh.col < (int)sh.cards.size() && sh.cards[sh.col].id == r.id)
            d.backdrop_url = sh.cards[sh.col].backdrop_url;
    }

    /* A show has nothing to resume or restart from here: its one button opens the episodes. */
    if (d.is_show || !g_resume_enabled || d.resume_sec <= 5) d.resume_sec = 0;
    if (d.resume_sec > 0) d.resume = std::string("Resume from ") + format_duration(d.resume_sec).c_str();

    PROV_LOG("'%s' detail page '%s' (%s)", m_provider_id.c_str(), d.item_id.c_str(), d.is_show ? "show" : "movie");

    if (m_provider && m_provider->panel) {
        struct Ctx { EvoRmlProviderHost* host; unsigned gen; };
        d.cast_state = EVO_HUI_LOAD_BUSY;
        Ctx* x = new Ctx{this, d.gen};
        if (m_provider->panel(d.item_id.c_str(), "details", &EvoRmlProviderHost::DetailCastCallback, x) != 0) {
            d.cast_state = EVO_HUI_LOAD_FAILED;
            delete x;
        }
    }
    m_dirty = true;
}

void EvoRmlProviderHost::CloseDetail()
{
    m_detail = DetailState();
    ++m_detail_gen;               /* a reply still in flight is dropped */
    m_dirty = true;
}

void EvoRmlProviderHost::DetailCastCallback(int ok, const evo_provider_row_t* rows, int count, void* ud)
{
    struct Ctx { EvoRmlProviderHost* host; unsigned gen; };
    Ctx* x = static_cast<Ctx*>(ud);
    EvoRmlProviderHost* self = x->host;
    const unsigned gen = x->gen;
    delete x;
    if (!self->IsOpen() || !self->m_detail.open || self->m_detail.gen != gen) return;
    DetailState& d = self->m_detail;
    d.cast.clear();
    d.versions.clear();
    for (int i = 0; ok && i < count; ++i) {
        /* the Details tab: every fact row the provider sent (rating, genres, director, studio, tagline) */
        if (std::strcmp(rows[i].group, "cast") != 0) {
            if (rows[i].title[0] && rows[i].detail[0] && (int)d.versions.size() < EVO_HUI_DETAIL_VERSIONS)
                d.versions.push_back({display_text(rows[i].title).c_str(), display_text(rows[i].detail).c_str()});
            continue;
        }
        if (!rows[i].title[0] || (int)d.cast.size() >= EVO_HUI_DETAIL_CAST) continue;
        DetailPerson p;
        p.name = display_text(rows[i].title).c_str();
        p.role = std::strcmp(rows[i].detail, "Cast") ? display_text(rows[i].detail).c_str() : "";
        p.art_url = rows[i].art_url;
        d.cast.push_back(std::move(p));
    }
    d.cast_state = ok ? EVO_HUI_LOAD_DONE : EVO_HUI_LOAD_FAILED;
    d.versions_state = d.cast_state;
    self->m_dirty = true;
}

/* The poster and the headshots: asked for once, shown when they land. */
void EvoRmlProviderHost::PushDetailArt()
{
    DetailState& d = m_detail;
    bool waiting = false;
    auto step = [&](const std::string& url, std::string& key) {
        if (url.empty()) return;
        if (key.empty()) {
            char k[128];
            const int rc = evo_provider_art_request(m_provider_id.c_str(), url.c_str(), k, sizeof k);
            if (rc < 0) return;                 /* full or refused: the circle stays a letter */
            key = k;
        }
        if (!evo_provider_art_ready(key.c_str())) waiting = true;
    };
    step(d.art_url, d.poster_key);
    for (DetailPerson& p : d.cast) step(p.art_url, p.art_key);
    /* Keep frames coming until the pictures land, but give up on one that never does. */
    if (waiting && d.art_wait < 900) { ++d.art_wait; m_dirty = true; }
}

bool EvoRmlProviderHost::DetailKey(Key k)
{
    DetailState& d = m_detail;
    const int tabs = d.is_show ? 4 : 3;
    const int buttons = d.is_show ? 1 : (d.resume_sec > 0 ? 3 : 2);   /* PLAY, RESTART when a position is saved, CHOOSE STREAM */
    m_dirty = true;

    if (k == KeyBack) {
        CloseDetail();
        return true;
    }
    if (d.zone == EVO_HUI_DETAIL_ZONE_BUTTONS) {
        switch (k) {
        case KeyUp:    if (d.button > 0) --d.button; break;
        case KeyDown:  if (d.button < buttons - 1) ++d.button; break;
        case KeyRight: d.zone = EVO_HUI_DETAIL_ZONE_TABS; break;
        case KeyAccept: DetailPlay(d.is_show || d.button == 0 ? 0 : (d.resume_sec > 0 && d.button == 1 ? 1 : 2)); break;
        default: break;
        }
        return true;
    }
    if (d.zone == EVO_HUI_DETAIL_ZONE_TABS) {
        switch (k) {
        case KeyLeft:
            if (d.tab > 0) --d.tab;
            else d.zone = EVO_HUI_DETAIL_ZONE_BUTTONS;
            break;
        case KeyRight: if (d.tab < tabs - 1) ++d.tab; break;
        case KeyDown:  if (d.tab == EVO_HUI_DETAIL_TAB_EPISODES) d.zone = EVO_HUI_DETAIL_ZONE_CONTENT; break;
        default: break;
        }
        return true;
    }
    /* the Episodes tab's open row */
    if (k == KeyUp) d.zone = EVO_HUI_DETAIL_ZONE_TABS;
    else if (k == KeyAccept) DetailPlay(0);
    return true;
}

/* A show's button opens its episodes (the level the card press would have opened);
 * a movie's Play and Restart queue the selection the player starts from. */
void EvoRmlProviderHost::DetailPlay(int mode)
{
    const DetailState d = m_detail;
    if (d.is_show) {
        m_detail_bypass = true;
        m_pending_activation = d.item_id;
        CloseDetail();
        return;
    }
    evo_home_art_capture(m_provider_id.c_str(), d.item_id.c_str(), d.art_url.c_str(), d.backdrop_url.c_str());
    RememberPickContext(d.title, d.meta, d.art_url);
    memset(&g_selection, 0, sizeof g_selection);
    snprintf(g_selection.provider_id, sizeof g_selection.provider_id, "%s", m_provider_id.c_str());
    snprintf(g_selection.item_id, sizeof g_selection.item_id, "%s", d.item_id.c_str());
    snprintf(g_selection.title, sizeof g_selection.title, "%s", d.title.c_str());
    g_selection.resume_sec = mode == 1 ? 0 : d.resume_sec;
    g_selection.force_pick = mode == 2;
    g_selection.skip_prompt = 1;
    g_selection_pending = true;
    CloseDetail();
}

void EvoRmlProviderHost::FillDetailParams(evo_hui_detail_t& o, EvoRenderBridge* r)
{
    const DetailState& d = m_detail;
    o.open = 1;
    o.title = d.title.c_str();
    o.meta = d.meta.c_str();
    o.rating = d.rating.c_str();
    o.cert = d.cert.c_str();
    o.overview = d.overview.c_str();
    o.resume = d.resume.c_str();
    o.is_show = d.is_show;
    o.has_restart = d.resume_sec > 0;
    o.zone = d.zone;
    o.button = d.button;
    o.tab = d.tab;
    o.poster_key = d.poster_key.c_str();
    if (!d.poster_key.empty() && evo_provider_art_ready(d.poster_key.c_str()))
        o.poster = r->MemoryTexturePixels(Rml::String(d.poster_key.c_str()), &o.poster_w, &o.poster_h);
    o.cast_count = (int)std::min<size_t>(d.cast.size(), EVO_HUI_DETAIL_CAST);
    for (int i = 0; i < o.cast_count; ++i) {
        const DetailPerson& p = d.cast[i];
        evo_hui_cast_t& c = o.cast[i];
        static char initials[EVO_HUI_DETAIL_CAST][2];
        initials[i][0] = (p.name[0] >= 0x20 && (unsigned char)p.name[0] < 0x80)
                             ? (char)std::toupper((unsigned char)p.name[0]) : '\0';
        initials[i][1] = '\0';
        c.name = p.name.c_str();
        c.role = p.role.c_str();
        c.initial = initials[i];
        c.art_key = p.art_key.c_str();
        if (!p.art_key.empty() && evo_provider_art_ready(p.art_key.c_str()))
            c.art = r->MemoryTexturePixels(Rml::String(p.art_key.c_str()), &c.art_w, &c.art_h);
    }
    o.cast_state = d.cast_state;
    o.version_count = (int)std::min<size_t>(d.versions.size(), EVO_HUI_DETAIL_VERSIONS);
    for (int i = 0; i < o.version_count; ++i) {
        o.versions[i].label = d.versions[i].label.c_str();
        o.versions[i].facts = d.versions[i].facts.c_str();
    }
    o.versions_state = d.versions_state;
}

/* The "Resume playback" setting, told to us by ProviderHostScreen (this layer has no settings access). */
extern "C" void evo_rmlui_provider_set_resume_enabled(int on)
{
    g_resume_enabled = on != 0;
}

extern "C" int evo_rmlui_provider_detail_open(void)
{
    return EvoRmlProviderHost::Instance().DetailOpen() ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* The stream picker in the detail page's look                               */
/* ------------------------------------------------------------------------- */

void EvoRmlProviderHost::RememberPickContext(const std::string& title, const std::string& meta, const std::string& art_url)
{
    m_pick_title = title;
    m_pick_meta = meta;
    if (art_url != m_pick_art_url) m_pick_poster_key.clear();
    m_pick_art_url = art_url;
}

void EvoRmlProviderHost::SetPick(const evo_hui_detail_t* d)
{
    if (!d) { m_pick_fresh = false; return; }
    m_pick = *d;
    m_pick_fresh = true;
}

void EvoRmlProviderHost::FillPickParams(evo_hui_detail_t& d, EvoRenderBridge* r)
{
    d = m_pick;
    d.open = 1;
    d.pick = 1;
    d.title = m_pick_title.c_str();
    d.meta = m_pick_meta.c_str();
    if (m_pick_poster_key.empty() && !m_pick_art_url.empty()) {
        char k[128];
        if (evo_provider_art_request(m_provider_id.c_str(), m_pick_art_url.c_str(), k, sizeof k) >= 0) m_pick_poster_key = k;
    }
    d.poster_key = m_pick_poster_key.c_str();
    if (!m_pick_poster_key.empty() && evo_provider_art_ready(m_pick_poster_key.c_str()))
        d.poster = r->MemoryTexturePixels(Rml::String(m_pick_poster_key.c_str()), &d.poster_w, &d.poster_h);
    m_pick_fresh = false;
}

extern "C" void evo_rmlui_provider_pick_set(const evo_hui_detail_t* d)
{
    EvoRmlProviderHost::Instance().SetPick(d);
}
