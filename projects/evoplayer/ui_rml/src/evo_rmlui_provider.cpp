/*
 * evo_rmlui_provider.cpp — render a provider's own document (#90).
 *
 * See evo_rmlui_provider.h for what this is and why it is separate from
 * EvoRmlApp. This file is the first use of RmlUi data binding and of RmlUi's
 * own focus navigation anywhere in the codebase; EVO's existing screens are
 * untouched and keep working exactly as before alongside it.
 */
#include "evo_rmlui_provider.h"
#include "evo_rmlui_app.h"
#include "evo_rmlui_render_bridge.h"
#include "evo_rmlui_bundle.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Types.h>

#include <cstdio>
#include <cstring>

extern "C" {
/* PROV_LOG: evo_bt on the device (so provider lines actually reach
 * /mnt/usb0/evo.log), plain stderr on the host. See evo_provider_log.h for why
 * this is not fprintf(stderr, ...). */
#include "evo_provider_log.h"
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
    } else {
        return false;
    }
    c.RegisterArray<std::vector<EvoProviderRow>>();

    c.Bind("provider_name", &m_model.provider_name);
    c.Bind("breadcrumb",    &m_model.breadcrumb);
    c.Bind("status",        &m_model.status);
    c.Bind("query",         &m_model.query);
    c.Bind("loading",       &m_model.loading);
    c.Bind("tuning",        &m_model.tuning);
    c.Bind("has_error",     &m_model.has_error);
    c.Bind("empty",         &m_model.empty);
    c.Bind("is_folder_level", &m_model.is_folder_level);
    c.Bind("count",         &m_model.count);
    c.Bind("rows",          &m_model.rows);

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
    if (!evo_provider_is_enabled(provider_id)) {
        PROV_LOG("'%s' is disabled", provider_id);
        return false;
    }

    m_provider_id = provider_id;
    m_using_fallback = false;
    m_stack.clear();
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
    std::string embedded_path = "rml/" + m_provider_id + ".rml";
    if (evo_rmlui_bundle_find(embedded_path)) {
        model_name = m_provider_id;
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
    m_crumbs.clear();
    m_art_urls.clear();
    m_art_keys.clear();
    m_all_rows.clear();
    m_row_window = 0;
    m_row_offset = 0;
    m_needs_initial_focus = false;
    m_pending_activation.clear();
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
            SetStatus("Could not reach the provider", true);
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
        self->SetStatus("Could not load the catalog", true);
        return;
    }

    self->m_has_more = has_more != 0;
    self->ApplyItems(items, count, has_more);

    PROV_LOG("'%s' catalog parent='%s' -> %d rows (folder_level=%d has_more=%d total=%d)",
             self->m_provider_id.c_str(),
             self->m_stack.empty() ? "" : self->m_stack.back().c_str(),
             count, self->m_model.is_folder_level ? 1 : 0, has_more,
             self->m_model.count);
}

void EvoRmlProviderHost::ApplyItems(const evo_provider_item_t* items, int count,
                                    int has_more)
{
    (void)has_more;

    for (int i = 0; i < count; ++i) {
        const evo_provider_item_t& s = items[i];
        EvoProviderRow r;
        r.id        = Rml::String(s.id);
        r.title     = Rml::String(s.title);
        r.subtitle  = Rml::String(s.subtitle);
        r.overview  = Rml::String(s.overview);
        r.now       = Rml::String(s.now_title);
        r.next      = Rml::String(s.next_title);
        r.duration  = s.is_live ? Rml::String() : format_duration(s.duration_sec);
        /* ASCII-only on purpose: taking the first BYTE of a UTF-8 title would
         * emit half a codepoint, so a non-ASCII title gets no initial rather
         * than a broken glyph. */
        if (s.title[0] >= 0x20 && (unsigned char)s.title[0] < 0x80) {
            char ini[2] = { s.title[0], 0 };
            if (ini[0] >= 'a' && ini[0] <= 'z') ini[0] = (char)(ini[0] - 'a' + 'A');
            r.initial = Rml::String(ini);
        }
        r.is_folder = s.is_folder != 0;
        r.is_live   = s.is_live != 0;
        r.index     = (int)m_all_rows.size();
        /* Art starts empty and is filled in by the art queue as posters
         * arrive. A bundle that wants something behind the gap paints a
         * background colour on the element, which is why this is "" and not a
         * placeholder image the provider did not choose. */
        m_all_rows.push_back(std::move(r));
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

    PublishRowWindow();

    if (m_model_handle) {
        m_model_handle.DirtyVariable("count");
        m_model_handle.DirtyVariable("empty");
        m_model_handle.DirtyVariable("is_folder_level");
    }

    if (m_all_rows.empty())
        SetStatus("", false);
    else
        SetStatus("", false);

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
    size_t want = m_row_window ? m_row_window : EVO_PROVIDER_ROW_WINDOW;
    if (want > EVO_PROVIDER_ROW_WINDOW_MAX) want = EVO_PROVIDER_ROW_WINDOW_MAX;
    if (want > m_all_rows.size()) want = m_all_rows.size();
    m_row_window = want;

    /* Keep the window inside the level. */
    if (m_row_offset + want > m_all_rows.size())
        m_row_offset = m_all_rows.size() - want;

    /*
     * Copied wholesale rather than appended because the art keys PushArtRequests
     * wrote live in BOTH vectors - it updates m_all_rows and the published copy
     * together, so a re-publish from m_all_rows never loses a poster that has
     * already arrived.
     *
     * Unconditional, with no early-out on an unchanged size: a slide keeps the
     * size identical and changes only the contents, which is exactly the case
     * the old `rows.size() == want` guard would have skipped.
     */
    const long first = (long)m_row_offset;
    m_model.rows.assign(m_all_rows.begin() + first,
                        m_all_rows.begin() + first + (long)want);
    if (m_model_handle) m_model_handle.DirtyVariable("rows");
    m_dirty = true;
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

void EvoRmlProviderHost::ExtendRowWindow()
{
    const int idx = FocusedRowIndex();
    if (idx < 0) return;

    /* Room left below the focused card inside the published window. */
    const bool near_end =
        (size_t)idx + EVO_PROVIDER_ROW_WINDOW_MARGIN >= m_row_window;
    const bool near_start =
        m_row_offset > 0 && (size_t)idx < EVO_PROVIDER_ROW_WINDOW_MARGIN;

    /* Still below the cap: grow. Cheap, and it does not disturb focus. */
    if (near_end &&
        m_row_window < EVO_PROVIDER_ROW_WINDOW_MAX &&
        m_row_offset + m_row_window < m_all_rows.size()) {
        m_row_window += EVO_PROVIDER_ROW_WINDOW_STEP;
        PublishRowWindow();
        PushArtRequests();
        PROV_LOG("row window -> %zu of %zu (focus row %d)",
                 m_row_window, m_all_rows.size(), idx);
        return;
    }

    /* At the cap. Slide to keep a margin of runway on the side focus is
     * heading for, and stop at either end of the level. */
    if (near_end && m_row_offset + m_row_window < m_all_rows.size()) {
        size_t room = m_all_rows.size() - (m_row_offset + m_row_window);
        size_t step = room < EVO_PROVIDER_ROW_WINDOW_STEP
                          ? room : EVO_PROVIDER_ROW_WINDOW_STEP;
        SlideRowWindow(m_row_offset + step, idx);
    } else if (near_start) {
        size_t step = m_row_offset < EVO_PROVIDER_ROW_WINDOW_STEP
                          ? m_row_offset : EVO_PROVIDER_ROW_WINDOW_STEP;
        SlideRowWindow(m_row_offset - step, idx);
    }
}

void EvoRmlProviderHost::SlideRowWindow(size_t new_offset, int focused_local)
{
    if (new_offset == m_row_offset) return;

    const bool forward = new_offset > m_row_offset;
    const size_t distance = forward ? (new_offset - m_row_offset)
                                    : (m_row_offset - new_offset);
    m_row_offset = new_offset;

    PublishRowWindow();
    PushArtRequests();

    /*
     * The element that had focus is still the same element - nothing was
     * re-instanced - but it now renders a row `distance` further along, so
     * focus has effectively jumped. Move it back by the same distance to leave
     * the user on the row they were actually on.
     */
    const int target = forward ? focused_local - (int)distance
                               : focused_local + (int)distance;
    FocusPublishedRow(target);

    PROV_LOG("row window slide -> [%zu,%zu) of %zu (focus %d -> %d)",
             m_row_offset, m_row_offset + m_row_window, m_all_rows.size(),
             focused_local, target);
}

bool EvoRmlProviderHost::FocusPublishedRow(int local_idx)
{
    if (!m_context || local_idx < 0) return false;

    /* The cards are the siblings data-for instanced, so walk up from whatever
     * has focus to the card, then index its parent's children. Only those
     * carrying `rowid` count - the parent also holds data-for's own template
     * element and whatever else the bundle put beside the list. */
    Rml::Element* focused = m_context->GetFocusElement();
    Rml::Element* card = nullptr;
    for (Rml::Element* e = focused; e; e = e->GetParentNode()) {
        if (!e->GetAttribute<Rml::String>("rowid", Rml::String()).empty()) {
            card = e;
            break;
        }
    }
    if (!card) return false;

    Rml::Element* parent = card->GetParentNode();
    if (!parent) return false;

    int seen = 0;
    for (int i = 0; i < parent->GetNumChildren(); ++i) {
        Rml::Element* child = parent->GetChild(i);
        if (child->GetAttribute<Rml::String>("rowid", Rml::String()).empty())
            continue;
        if (seen++ != local_idx) continue;
        child->Focus();
        m_dirty = true;
        return true;
    }
    return false;
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

    size_t start = (focus_idx > 4) ? (size_t)(focus_idx - 4) : 0;
    size_t end = (start + 16 < n) ? (start + 16) : n;

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
        m_dirty = true;
    }
}

/* ------------------------------------------------------------------------- */
/* Input                                                                     */
/* ------------------------------------------------------------------------- */

bool EvoRmlProviderHost::HandleKey(Key k)
{
    if (!IsOpen() || !m_context) return false;

    switch (k) {
    case KeyUp:
    case KeyDown:
    case KeyLeft:
    case KeyRight: {
        /*
         * Straight into RmlUi. ElementDocument's default action turns
         * KI_UP/DOWN/LEFT/RIGHT into a spatial-navigation move driven by the
         * `nav-up`/`nav-down`/... RCSS properties, and sets :focus and
         * :focus-visible on the result.
         *
         * This is the whole of scope 11: EVO does not know where the rows are,
         * how many there are per line, or which one is selected. The provider's
         * stylesheet decides, and the focus ring it draws is its own.
         */
        Rml::Input::KeyIdentifier id =
            (k == KeyUp)    ? Rml::Input::KI_UP :
            (k == KeyDown)  ? Rml::Input::KI_DOWN :
            (k == KeyLeft)  ? Rml::Input::KI_LEFT : Rml::Input::KI_RIGHT;

        /* A press that beats the first render: seed focus onto a row first,
         * otherwise this direction is swallowed and the screen reads as hung
         * for one press. */
        if (m_needs_initial_focus) {
            m_needs_initial_focus = false;
            m_context->Update();
            m_context->ProcessKeyDown(Rml::Input::KI_TAB, 0);
            m_context->ProcessKeyUp(Rml::Input::KI_TAB, 0);
        }

        /*
         * Who ends up with focus is RmlUi's business, but whether the key was
         * USED is EVO's, and the rail depends on the answer.
         *
         * ProviderHostScreen hands Left to the document first and focuses the
         * navigation rail if the document declines it - the leftmost column is
         * the only place that can know it is the leftmost column, and the rail
         * is a document in EVO's MAIN context that this one cannot reach. That
         * contract was written on the screen side but never honoured here: this
         * returned true for every direction, so Left was always consumed and
         * the rail was unreachable from a provider screen by d-pad at all.
         *
         * Focus not moving is the signal. Nothing else distinguishes "moved to
         * the card on the left" from "there was nothing to move to".
         */
        Rml::Element* before = m_context->GetFocusElement();

        m_context->ProcessKeyDown(id, 0);
        m_context->ProcessKeyUp(id, 0);
        m_dirty = true;

        if (k == KeyLeft && m_context->GetFocusElement() == before)
            return false;   /* -> the rail takes it */
        return true;
    }

    case KeyAccept: {
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
            m_page = 0;
            RequestPage(m_stack.empty() ? "" : m_stack.back().c_str(), 0);
            return true;
        }

        /* At root: if we arrived via USB playlist picker with multiple playlists, return to USB list */
        if (!m_saved_usb_playlists.empty()) {
            ShowUsbPlaylists(m_saved_usb_playlists);
            return true;
        }

        /* At root: if a playlist is loaded, Back returns to the Select Playlist Source screen */
        if (!m_model.empty) {
            ShowSetupScreen();
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
    /* The rows were replaced between the press and now - a refresh landing on
     * the same frame. Dropping it is right: acting on a stale id would open
     * something the user is no longer looking at. */
    if (!hit) return;

    /* If USB playlist picker is active, activating a row selects that playlist */
    if (m_is_usb_picker) {
        m_is_usb_picker = false;
        if (m_provider && m_provider->set_source) {
            m_provider->set_source(hit->id.c_str());
            evo_provider_set_enabled(m_provider_id.c_str(), 1);
            m_stack.clear();
            m_crumbs.clear();
            m_page = 0;
            RequestPage("", 0);
        }
        return;
    }

    PROV_LOG("'%s' activate '%s' (%s) title='%s'", m_provider_id.c_str(),
             hit->id.c_str(), hit->is_folder ? "folder" : "playable",
             hit->title.c_str());

    if (hit->is_folder) {
        m_stack.push_back(std::string(hit->id.c_str()));
        m_crumbs.push_back(std::string(hit->title.c_str()));
        m_page = 0;
        RequestPage(m_stack.back().c_str(), 0);
        return;
    }

    /* Playable: hand it to the player through the mailbox. */
    memset(&g_selection, 0, sizeof g_selection);
    snprintf(g_selection.provider_id, sizeof g_selection.provider_id,
             "%s", m_provider_id.c_str());
    snprintf(g_selection.item_id, sizeof g_selection.item_id,
             "%s", hit->id.c_str());
    snprintf(g_selection.title, sizeof g_selection.title,
             "%s", hit->title.c_str());
    g_selection.is_live = hit->is_live ? 1 : 0;
    g_selection_pending = true;
}

void EvoRmlProviderHost::Tick()
{
    if (!IsOpen()) return;
    evo_bundle_poll();
    evo_provider_art_poll();
    /* Adopt whatever finished decoding, and ask for the next batch. */
    PushArtRequests();
    /* Grow the render window if focus has walked to the end of it. Cheap: it
     * returns immediately unless the window is both partial and nearly used
     * up, and it logs only on the rare frame where it actually grows. */
    ExtendRowWindow();
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
void EvoRmlProviderHost::Render(uint32_t* framebuffer, int width, int height)
{
    if (!IsOpen() || !m_context) return;

    EvoRenderBridge* r = EvoRmlApp::Instance().RenderBridge();
    if (!r) return;

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
        m_needs_initial_focus = false;
        m_context->ProcessKeyDown(Rml::Input::KI_TAB, 0);
        m_context->ProcessKeyUp(Rml::Input::KI_TAB, 0);
        m_context->Update();
    }

    r->FrameBegin();
    m_context->Render();
    r->FrameEnd();
}

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
    m_crumbs.clear();
    m_art_urls.clear();
    m_art_keys.clear();
    m_is_usb_picker = true;

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

void EvoRmlProviderHost::ShowSetupScreen()
{
    m_saved_usb_playlists.clear();
    m_stack.clear();
    m_crumbs.clear();
    m_all_rows.clear();
    m_model.rows.clear();
    m_art_urls.clear();
    m_art_keys.clear();
    m_row_window = 0;
    m_row_offset = 0;
    m_is_usb_picker = false;
    m_model.query = "";
    m_model.loading = false;
    m_model.tuning = false;
    m_model.empty = true;
    m_model.count = 0;
    m_model.is_folder_level = false;
    m_model.breadcrumb = Rml::String(m_provider ? m_provider->name : "IPTV");
    SetStatus("", false);

    if (m_provider && m_provider->set_source) {
        m_provider->set_source("");
    }

    if (m_model_handle) {
        m_model_handle.DirtyVariable("rows");
        m_model_handle.DirtyVariable("query");
        m_model_handle.DirtyVariable("loading");
        m_model_handle.DirtyVariable("tuning");
        m_model_handle.DirtyVariable("empty");
        m_model_handle.DirtyVariable("count");
        m_model_handle.DirtyVariable("is_folder_level");
        m_model_handle.DirtyVariable("breadcrumb");
    }

    m_needs_initial_focus = true;
    m_dirty = true;
}

/* ------------------------------------------------------------------------- */
/* C entry points                                                            */
/* ------------------------------------------------------------------------- */

extern "C" {

void evo_rmlui_provider_show_setup(void)
{
    EvoRmlProviderHost::Instance().ShowSetupScreen();
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

void evo_rmlui_provider_render(uint32_t *framebuffer, int width, int height)
{
    EvoRmlProviderHost::Instance().Render(framebuffer, width, height);
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
    if (key < EvoRmlProviderHost::KeyUp || key > EvoRmlProviderHost::KeySearch)
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

} /* extern "C" */
