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

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Types.h>

#include <cstdio>
#include <cstring>

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

/* ------------------------------------------------------------------------- */
/* Data model                                                                */
/* ------------------------------------------------------------------------- */

bool EvoRmlProviderHost::RegisterDataModel()
{
    Rml::DataModelConstructor c = m_context->CreateDataModel(m_model_name);
    if (!c) {
        fprintf(stderr, "[EVO provider] CreateDataModel('%s') failed\n",
                m_model_name.c_str());
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
    c.Bind("has_error",     &m_model.has_error);
    c.Bind("empty",         &m_model.empty);
    c.Bind("is_folder_level", &m_model.is_folder_level);
    c.Bind("count",         &m_model.count);
    c.Bind("rows",          &m_model.rows);

    /*
     * The one event a bundle can raise. It takes the row index rather than an
     * item id because a data-event attribute's arguments come through as
     * variants from the markup, and an index cannot be forged into a path -
     * whatever the bundle passes is looked up in `rows` and anything out of
     * range is ignored.
     */
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
            if (!m_has_more || m_model.loading) return;
            RequestPage(m_stack.empty() ? "" : m_stack.back().c_str(), ++m_page);
        });

    c.BindEventCallback("go_back",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            HandleKey(KeyBack);
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
        fprintf(stderr, "[EVO provider] CreateContext failed\n");
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
        fprintf(stderr, "[EVO provider] LoadDocument('%s') failed\n",
                rml_path.c_str());
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
        fprintf(stderr, "[EVO provider] fallback skin failed to load\n");
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
            fprintf(stderr, "[EVO provider] document exceeds %d DOM nodes\n",
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
        fprintf(stderr, "[EVO provider] unknown provider '%s'\n",
                provider_id ? provider_id : "(null)");
        return false;
    }
    if (!evo_provider_is_enabled(provider_id)) {
        fprintf(stderr, "[EVO provider] '%s' is disabled\n", provider_id);
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

    /*
     * Try the cached bundle before the network. A provider screen must open on
     * the frame the user pressed the button, not after a round trip - so the
     * cache renders immediately and the refresh lands behind it.
     */
    evo_bundle_manifest_t man;
    evo_bundle_status_t st = evo_bundle_load_cached(m_provider_id.c_str(), &man);

    std::string model_name = m_provider_id;
    bool loaded = false;

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

    /* And the first page of the catalog. */
    RequestPage("", 0);

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
    m_needs_initial_focus = false;
    m_pending_activation.clear();
    m_bundle_version.clear();
    m_using_fallback = false;
    g_selection_pending = false;
    m_dirty = true;
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

    if (page == 0) {
        m_model.rows.clear();
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

    ItemsCtx* ctx = new ItemsCtx{this, ++m_request_generation};
    int rc = m_provider->list_catalog(parent_id, page,
                                      &EvoRmlProviderHost::ItemsCallback, ctx);
    if (rc != 0) {
        /* Not accepted, so the callback will never fire and this owns ctx. */
        delete ctx;
        m_model.loading = false;
        if (m_model_handle) m_model_handle.DirtyVariable("loading");
        SetStatus(m_provider->is_configured()
                      ? "Could not reach the provider"
                      : std::string(m_provider->name) + " is not set up yet",
                  true);
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
        self->SetStatus("Could not load the catalog", true);
        return;
    }

    self->m_has_more = has_more != 0;
    self->ApplyItems(items, count, has_more);
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
        r.index     = (int)m_model.rows.size();
        /* Art starts empty and is filled in by the art queue as posters
         * arrive. A bundle that wants something behind the gap paints a
         * background colour on the element, which is why this is "" and not a
         * placeholder image the provider did not choose. */
        m_model.rows.push_back(std::move(r));
        m_art_urls.emplace_back(s.art_url);
        m_art_keys.emplace_back();
    }

    m_model.count = (int)m_model.rows.size();
    m_model.empty = m_model.rows.empty();
    /* A level is a folder level when its first row is a folder. Providers do
     * not mix the two at one level - a group list is groups, a group's contents
     * are channels - and taking the first row rather than requiring every row
     * to agree keeps a provider that does mix them from flipping the header. */
    m_model.is_folder_level = !m_model.rows.empty() && m_model.rows[0].is_folder;

    if (m_model_handle) {
        m_model_handle.DirtyVariable("rows");
        m_model_handle.DirtyVariable("count");
        m_model_handle.DirtyVariable("empty");
        m_model_handle.DirtyVariable("is_folder_level");
    }

    if (m_model.rows.empty())
        SetStatus("Nothing here", false);
    else
        SetStatus("", false);

    /* Deferred to Render(), which is where Context::Update() creates the
     * elements data-for is going to produce. See m_needs_initial_focus. */
    if (m_doc && !m_model.rows.empty())
        m_needs_initial_focus = true;

    PushArtRequests();
    m_dirty = true;
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
     * Called from ApplyItems and again every Tick(), which is what fills a
     * poster wall in over a few frames rather than blocking the rows on it.
     * The art layer bounds itself - a fixed in-flight limit and an LRU under a
     * byte cap - so this can afford to be naive about how many rows there are.
     */
    bool changed = false;
    const size_t n = m_model.rows.size();

    for (size_t i = 0; i < n && i < m_art_urls.size(); ++i) {
        if (!m_model.rows[i].art.empty()) continue;      /* already showing */
        if (m_art_urls[i].empty()) continue;             /* row has no art   */

        if (!m_art_keys[i].empty()) {
            /* Promised a key earlier - has it arrived? */
            if (evo_provider_art_ready(m_art_keys[i].c_str())) {
                m_model.rows[i].art = Rml::String(m_art_keys[i].c_str());
                changed = true;
            }
            continue;
        }

        char key[128];
        int rc = evo_provider_art_request(m_provider_id.c_str(),
                                          m_art_urls[i].c_str(),
                                          key, sizeof key);
        if (rc < 0) {
            /* Unusable URL, or the queue is full. Clear the URL so this row
             * stops asking; a later navigation rebuilds the rows and tries
             * again with a queue that has drained. */
            m_art_urls[i].clear();
            continue;
        }
        m_art_keys[i] = key;
        if (rc == 1) {
            m_model.rows[i].art = Rml::String(key);
            changed = true;
        }
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

        m_context->ProcessKeyDown(id, 0);
        m_context->ProcessKeyUp(id, 0);
        m_dirty = true;
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
        /*
         * The exit contract. Deeper than the root: pop a level, consume the
         * press. At the root: do NOT consume, and the caller returns to the
         * rail. A provider screen that swallowed Back at its root would be a
         * screen the user cannot leave, on a device whose only other option is
         * the PS button - which is the close path that panicked the console.
         */
        if (m_stack.empty()) return false;
        m_stack.pop_back();
        if (!m_crumbs.empty()) m_crumbs.pop_back();
        m_page = 0;
        RequestPage(m_stack.empty() ? "" : m_stack.back().c_str(), 0);
        return true;
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
    ApplyPendingActivation();
}

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

/* ------------------------------------------------------------------------- */
/* C entry points                                                            */
/* ------------------------------------------------------------------------- */

extern "C" {

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

} /* extern "C" */
