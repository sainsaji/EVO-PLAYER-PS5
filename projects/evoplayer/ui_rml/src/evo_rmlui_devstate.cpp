/*
 * evo_rmlui_devstate.cpp - see evo_rmlui_devstate.h. Real body only under
 * EVO_USB_REMOTE + EVO_APP_MODULE.
 */
#include "evo_rmlui_devstate.h"

#if defined(EVO_USB_REMOTE) && defined(EVO_APP_MODULE)

#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementText.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

/* The player OSD, as last pushed by the bridge. */
long long g_osd_chrome_ms = -1;
bool      g_osd_stats = false;
bool      g_osd_scrub = false;

long long now_ms()
{
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* evo::ScreenId (core/include/evo/Common.hpp) - names for the JSON. */
const char* screen_name(int id)
{
    switch (id) {
    case 0:  return "MainMenu";
    case 1:  return "UsbBrowser";
    case 2:  return "Player";
    case 3:  return "ResumePrompt";
    case 10: return "Settings";
    case 11: return "ProfileSelect";
    case 12: return "RecentFiles";
    case 13: return "Favorites";
    case 14: return "AboutSupport";
    case 15: return "DeveloperTools";
    case 16: return "MediaInfo";
    case 17: return "PlaybackFinished";
    case 18: return "SubtitlePicker";
    case 19: return "Changelog";
    case 20: return "ExitConfirm";
    case 21: return "TextReader";
    case 22: return "EmbySetup";
    case 23: return "EmbyBrowse";
    case 24: return "SettingsPlayback";
    case 25: return "SettingsSubtitles";
    case 26: return "SettingsInterface";
    case 27: return "SettingsSystem";
    case 28: return "SurroundTest";
    case 29: return "ThemeSelect";
    case 30: return "ImageViewer";
    case 31: return "AudioTrackPicker";
    case 32: return "SafeToClose";
    default: return "?";
    }
}

void json_str(std::string& o, const std::string& s)
{
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\t': o += "\\t";  break;
        default:
            if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                o += b;
            } else {
                o += (char)c;
            }
        }
    }
    o += '"';
}

bool visible(Rml::Element* el)
{
    return el && el->IsVisible(true);
}

/* The visible text under `el`, whitespace collapsed, cut at `cap` bytes (on a
 * UTF-8 boundary). */
void collect_text(Rml::Element* el, std::string& out, size_t cap)
{
    if (out.size() >= cap || !el->IsVisible())
        return;
    if (el->GetTagName() == "#text") {
        const Rml::String& t = static_cast<Rml::ElementText*>(el)->GetText();
        for (char c : t) {
            bool ws = (c == ' ' || c == '\n' || c == '\r' || c == '\t');
            if (ws) {
                if (!out.empty() && out.back() != ' ') out += ' ';
            } else {
                out += c;
            }
        }
        if (!out.empty() && out.back() != ' ') out += ' ';
        return;
    }
    for (int i = 0, n = el->GetNumChildren(); i < n; ++i)
        collect_text(el->GetChild(i), out, cap);
}

/* First <img> under `el`: "../icons/icon_home.png" -> "icon_home". */
std::string icon_name(Rml::Element* el)
{
    if (el->GetTagName() == "img") {
        std::string s = el->GetAttribute<Rml::String>("src", "");
        size_t sl = s.find_last_of('/');
        if (sl != std::string::npos) s = s.substr(sl + 1);
        size_t dot = s.rfind('.');
        if (dot != std::string::npos) s.resize(dot);
        return s;
    }
    for (int i = 0, n = el->GetNumChildren(); i < n; ++i) {
        std::string s = icon_name(el->GetChild(i));
        if (!s.empty()) return s;
    }
    return std::string();
}

std::string text_of(Rml::Element* el, size_t cap = 80)
{
    std::string s;
    if (!el) return s;
    collect_text(el, s, cap);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.empty()) {
        /* Icon-only controls (the nav rail): name them by their icon. */
        std::string ic = icon_name(el);
        if (!ic.empty()) return "[" + ic + "]";
    }
    if (s.size() > cap) {
        size_t n = cap;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) --n;
        s.resize(n);
        s += "...";
    }
    return s;
}

/* `focused`, `*-focused` or `*-cursor`: the highlight classes EVO's screens
 * move instead of RmlUi focus (tile-focused, row-focused, rail-item-cursor,
 * the keyboard's `focused`, a provider bundle's op-focused, ...). */
bool is_focus_token(const Rml::String& tok)
{
    auto ends = [&](const char* suf) {
        size_t n = std::char_traits<char>::length(suf);
        return tok.size() >= n && tok.compare(tok.size() - n, n, suf) == 0;
    };
    return tok == "focused" || ends("-focused") || ends("-cursor");
}

std::vector<Rml::String> class_tokens(Rml::Element* el)
{
    std::vector<Rml::String> out;
    const Rml::String cls = el->GetClassNames();
    size_t i = 0;
    while (i < cls.size()) {
        size_t j = cls.find(' ', i);
        if (j == Rml::String::npos) j = cls.size();
        if (j > i) out.push_back(cls.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

bool has_focus_class(Rml::Element* el)
{
    for (const Rml::String& t : class_tokens(el))
        if (is_focus_token(t)) return true;
    return false;
}

/* The class that says what kind of item this is (`sb-section`, `brow`),
 * i.e. its first class that is not a highlight. Empty if it has none. */
Rml::String kind_class(Rml::Element* el)
{
    for (const Rml::String& t : class_tokens(el))
        if (!is_focus_token(t)) return t;
    return Rml::String();
}

/* Same list as `item`: same tag and, when `item` has one, same kind class -
 * so a section header next to the settings sections is not counted. */
bool same_kind(Rml::Element* c, Rml::Element* item, const Rml::String& kind)
{
    if (c->GetTagName() != item->GetTagName()) return false;
    return kind.empty() || c->IsClassSet(kind);
}

/* Collect the leaf-most visible highlighted elements under `el`: a highlighted
 * container (the settings rail as a whole) loses to the item inside it. */
bool find_highlighted(Rml::Element* el, std::vector<Rml::Element*>& out)
{
    if (!el->IsVisible())
        return false;
    bool below = false;
    for (int i = 0, n = el->GetNumChildren(); i < n; ++i)
        below |= find_highlighted(el->GetChild(i), out);
    if (!below && el->GetTagName() != "#text" && has_focus_class(el)) {
        out.push_back(el);
        return true;
    }
    return below;
}

std::string base_name(const Rml::String& url)
{
    size_t s = url.find_last_of('/');
    std::string b = (s == Rml::String::npos) ? url : url.substr(s + 1);
    size_t d = b.rfind(".rml");
    if (d != std::string::npos) b.resize(d);
    return b;
}

struct VisDoc {
    Rml::Context*         ctx;
    Rml::ElementDocument* doc;
    std::string           name;   /* source basename: "settings", "dialog" */
    int                   rank;   /* lower wins the focus */
};

int doc_rank(const std::string& ctx, const std::string& name)
{
    if (ctx == "keyboard_context") return 0;   /* modal over everything */
    if (name == "dialog")          return 1;
    if (name == "navbar")          return 2;   /* only highlights when the rail has focus */
    if (ctx != "main_context" && ctx != "toast_context") return 3;   /* a provider's screen */
    return 4;
}

struct Hit {
    Rml::Element* el;
    std::string   doc;
    int           rank;
};

void element_json(std::string& o, const Hit& h)
{
    Rml::Element* el = h.el;
    o += "{\"id\":";     json_str(o, el->GetId());
    o += ",\"tag\":";    json_str(o, el->GetTagName());
    o += ",\"class\":";  json_str(o, el->GetClassNames());
    o += ",\"text\":";   json_str(o, text_of(el));
    o += ",\"doc\":";    json_str(o, h.doc);

    /* Position among the visible siblings with the same tag: "item 3 of 12".
     * A highlighted element that is the only child of a wrapper (the rail's
     * nav-item inside nav-wrap) counts at the first ancestor level that has
     * siblings - that level is the list. */
    int index = 0, total = 0;
    std::vector<Rml::Element*> sibs;
    Rml::Element* item = el;
    for (int level = 0; level < 4 && item; ++level) {
        Rml::Element* p = item->GetParentNode();
        if (!p || p->GetTagName() == "body") break;
        index = total = 0;
        sibs.clear();
        const Rml::String kind = kind_class(item);
        for (int i = 0, n = p->GetNumChildren(); i < n; ++i) {
            Rml::Element* c = p->GetChild(i);
            if (!same_kind(c, item, kind) || !c->IsVisible()) continue;
            ++total;
            if (c == item) index = total;
            sibs.push_back(c);
        }
        if (total > 1) break;
        item = p;
    }
    if (total <= 1) {   /* nothing list-like above it: report it alone */
        index = total = 1;
        sibs.assign(1, el);
    }
    char b[160];
    std::snprintf(b, sizeof b, ",\"index\":%d,\"total\":%d", index, total);
    o += b;

    const Rml::Vector2f pos = el->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f sz  = el->GetBox().GetSize(Rml::BoxArea::Border);
    std::snprintf(b, sizeof b, ",\"rect\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
                  (int)pos.x, (int)pos.y, (int)sz.x, (int)sz.y);
    o += b;

    /* The siblings ARE the list/row/grid the D-pad moves through: what a
     * navigation sequence is planned against. */
    o += ",\"focusable\":[";
    const size_t cap = 60;
    for (size_t i = 0; i < sibs.size() && i < cap; ++i) {
        if (i) o += ',';
        o += "{\"id\":"; json_str(o, sibs[i]->GetId());
        o += ",\"text\":"; json_str(o, text_of(sibs[i], 48));
        o += '}';
    }
    o += "]}";
}

std::string g_json;

} // namespace

extern "C" void evo_rmlui_dev_note_osd(int chrome_visible, int show_stats, int scrub_active)
{
    if (chrome_visible)
        g_osd_chrome_ms = now_ms();
    g_osd_stats = show_stats != 0;
    g_osd_scrub = scrub_active != 0;
}

extern "C" const char* evo_rmlui_dev_ui_json(int screen_id, int paused)
{
    std::vector<VisDoc> docs;
    for (int c = 0, nc = Rml::GetNumContexts(); c < nc; ++c) {
        Rml::Context* ctx = Rml::GetContext(c);
        if (!ctx || ctx->GetName() == "debug_context") continue;
        for (int d = 0, nd = ctx->GetNumDocuments(); d < nd; ++d) {
            Rml::ElementDocument* doc = ctx->GetDocument(d);
            if (!doc || !doc->IsVisible()) continue;
            std::string name = base_name(doc->GetSourceURL());
            docs.push_back({ctx, doc, name, doc_rank(ctx->GetName(), name)});
        }
    }

    /* Every candidate, then the best-ranked one is "focused". RmlUi focus is
     * offered first within a document, the highlight classes after it. */
    std::vector<Hit> hits;
    for (const VisDoc& v : docs) {
        Rml::Element* f = v.ctx->GetFocusElement();
        if (f && f != v.doc && f->GetOwnerDocument() == v.doc && visible(f) &&
            f->GetTagName() != "body")
            hits.push_back({f, v.name, v.rank});
        std::vector<Rml::Element*> hl;
        find_highlighted(v.doc, hl);
        for (Rml::Element* e : hl)
            if (e != f) hits.push_back({e, v.name, v.rank});
    }
    const Hit* best = nullptr;
    for (const Hit& h : hits)
        if (!best || h.rank < best->rank) best = &h;

    std::string& o = g_json;
    o.clear();
    char b[96];
    std::snprintf(b, sizeof b, "{\"screen\":{\"id\":%d,\"name\":", screen_id);
    o += b;
    json_str(o, screen_name(screen_id));
    o += "},\"docs\":[";
    for (size_t i = 0; i < docs.size(); ++i) {
        if (i) o += ',';
        json_str(o, docs[i].name);
    }
    o += "],\"focused\":";
    if (best) element_json(o, *best);
    else      o += "null";

    /* Anything else that is highlighted - a remembered selection in a pane
     * that does not have the cursor, or a second highlight a bug left on. */
    o += ",\"also_highlighted\":[";
    bool first = true;
    for (const Hit& h : hits) {
        if (&h == best) continue;
        if (!first) o += ',';
        first = false;
        o += "{\"id\":"; json_str(o, h.el->GetId());
        o += ",\"text\":"; json_str(o, text_of(h.el, 48));
        o += ",\"doc\":"; json_str(o, h.doc);
        o += '}';
    }
    o += ']';

    const VisDoc* kb = nullptr; const VisDoc* dlg = nullptr; const VisDoc* toast = nullptr;
    for (const VisDoc& v : docs) {
        if (v.ctx->GetName() == "keyboard_context") kb = &v;
        else if (v.name == "dialog")                dlg = &v;
        else if (v.ctx->GetName() == "toast_context") toast = &v;
    }
    o += ",\"modal\":";
    const VisDoc* m = kb ? kb : dlg;
    if (m) {
        o += "{\"kind\":"; json_str(o, kb ? "keyboard" : "dialog");
        o += ",\"text\":"; json_str(o, text_of(m->doc, 200));
        o += '}';
    } else {
        o += "null";
    }
    o += ",\"toast\":";
    if (toast) json_str(o, text_of(toast->doc, 200));
    else       o += "null";

    o += ",\"native_player\":";
    if (screen_id == 2) {
        const bool osd = g_osd_chrome_ms >= 0 && now_ms() - g_osd_chrome_ms < 500;
        std::snprintf(b, sizeof b, "{\"osd_visible\":%s,\"paused\":%s,\"active_overlay\":",
                      osd ? "true" : "false", paused ? "true" : "false");
        o += b;
        if (osd && g_osd_scrub)      o += "\"scrub\"";
        else if (osd && g_osd_stats) o += "\"stats\"";
        else                         o += "null";
        o += '}';
    } else {
        o += "null";
    }
    o += '}';
    return o.c_str();
}

#endif /* EVO_USB_REMOTE && EVO_APP_MODULE */
