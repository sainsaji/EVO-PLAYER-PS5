/*
 * evo_page_engine.cpp - Prospero Browser's PageEngine on EVO's browser engine
 * seam (#237). See evo_page_engine.hpp.
 */
#include "evo_page_engine.hpp"

#include "evo_boot_log.h"
#include "evo_browser_engine.h"
#include "evo_browser_wire.h"

#include <cstring>

namespace evo::web {

namespace {

EvoPageEngine* g_current = nullptr;

using prospero::web::EngineEvent;

}  // namespace

/* The host has one web view; the newest page owns it. An older one (Prospero
 * restores a session by opening the saved tab, then closing its start tab)
 * goes quiet: its commands are dropped, so it cannot steer or hide the page
 * the newest one shows. */
#define EVO_ENGINE_LIVE_OR_RETURN() \
    do { if (g_current != this) return; } while (0)

EvoPageEngine::EvoPageEngine(uint32_t width, uint32_t height) : width_(width), height_(height)
{
    g_current = this;
    status_.progress = 0.0f;
    evo_browser_open();   /* starts the engine in the background unless it runs */
}

EvoPageEngine::~EvoPageEngine()
{
    if (g_current != this) return;
    /* The engine keeps running for the next page (it stops at EVO's soft
     * close); only stop painting a page nobody shows. */
    evo_browser_set_visible(0);
    g_current = nullptr;
}

void EvoPageEngine::install()
{
    prospero::web::page_engine_factory() = [](uint32_t w, uint32_t h)
        -> std::unique_ptr<prospero::web::PageEngine> {
        return std::make_unique<EvoPageEngine>(w, h);
    };
}

EvoPageEngine* EvoPageEngine::current() { return g_current; }

void EvoPageEngine::load_uri(const std::string& uri)
{
    EVO_ENGINE_LIVE_OR_RETURN();
    status_.uri = uri;
    status_.loading = true;
    status_.progress = 0.1f;
    evo_browser_load(uri.c_str());   /* queued until the engine is ready */
}

void EvoPageEngine::reload() { EVO_ENGINE_LIVE_OR_RETURN(); evo_browser_nav(EVOB_NAV_RELOAD); }
void EvoPageEngine::stop_loading() { EVO_ENGINE_LIVE_OR_RETURN(); evo_browser_nav(EVOB_NAV_STOP); }
void EvoPageEngine::go_back() { EVO_ENGINE_LIVE_OR_RETURN(); evo_browser_nav(EVOB_NAV_BACK); }
void EvoPageEngine::go_forward() { EVO_ENGINE_LIVE_OR_RETURN(); evo_browser_nav(EVOB_NAV_FORWARD); }

void EvoPageEngine::set_zoom_level(float zoom)
{
    (void)zoom;   /* the host has no zoom command yet */
}

void EvoPageEngine::resize(uint32_t width, uint32_t height)
{
    width_ = width;
    height_ = height;
    EVO_ENGINE_LIVE_OR_RETURN();
    if (setup_sent_) evo_browser_resize(static_cast<int>(width), static_cast<int>(height));
}

void EvoPageEngine::pointer_motion(int32_t x, int32_t y)
{
    EVO_ENGINE_LIVE_OR_RETURN();
    evo_browser_pointer(x, y, EVOB_PTR_MOVE, 0);
}

void EvoPageEngine::pointer_button(int32_t x, int32_t y, uint32_t button, bool pressed)
{
    EVO_ENGINE_LIVE_OR_RETURN();
    evo_browser_pointer(x, y, pressed ? EVOB_PTR_DOWN : EVOB_PTR_UP, static_cast<int>(button));
}

void EvoPageEngine::scroll(int32_t x, int32_t y, int32_t delta_x, int32_t delta_y)
{
    /* Prospero: positive delta_y scrolls towards the top. The host: positive
     * dy scrolls down the page (as the D-pad Down sent it). */
    EVO_ENGINE_LIVE_OR_RETURN();
    evo_browser_scroll(x, y, -delta_x, -delta_y);
}

void EvoPageEngine::key(uint32_t key_code, bool pressed, uint32_t modifiers)
{
    /* Both sides use DOM / Windows virtual key codes (13 Enter, 37..40). */
    EVO_ENGINE_LIVE_OR_RETURN();
    evo_browser_key(key_code, modifiers, pressed ? 1 : 0, "");
}

void EvoPageEngine::insert_text(const std::string& utf8)
{
    EVO_ENGINE_LIVE_OR_RETURN();
    if (!utf8.empty()) evo_browser_text(utf8.c_str());
}

bool EvoPageEngine::take_text_focus()
{
    const bool f = text_focus_;
    text_focus_ = false;
    return f;
}

void EvoPageEngine::set_visible(bool visible)
{
    visible_ = visible;
    EVO_ENGINE_LIVE_OR_RETURN();
    if (setup_sent_) evo_browser_set_visible(visible ? 1 : 0);
}

int EvoPageEngine::engine_state(std::string* why) const
{
    char buf[160] = {0};
    const int st = evo_browser_state(buf, sizeof buf);
    if (why) *why = buf;
    return st;
}

bool EvoPageEngine::take_frame(const FrameSink& sink)
{
    (void)sink;   /* the kit draws the host's buffers itself */
    pump();
    return false;
}

std::vector<EngineEvent> EvoPageEngine::take_events()
{
    pump();
    std::vector<EngineEvent> out;
    out.swap(events_);
    return out;
}

void EvoPageEngine::pump()
{
    EVO_ENGINE_LIVE_OR_RETURN();   /* the host's events belong to the live page */
    char why[160] = {0};
    const evo_browser_state_t st = evo_browser_state(why, sizeof why);
    if (st == EVO_BROWSER_READY && !setup_sent_) {
        setup_sent_ = true;
        evo_browser_resize(static_cast<int>(width_), static_cast<int>(height_));
        evo_browser_set_visible(visible_ ? 1 : 0);
    }
    if (st == EVO_BROWSER_FAILED && !failed_reported_) {
        failed_reported_ = true;
        EngineEvent e;
        e.type = EngineEvent::Type::ProcessTerminated;
        e.url = status_.uri;
        e.message = why[0] ? why : "The browser engine stopped";
        events_.push_back(e);
        status_.loading = false;
    }

    evo_browser_event_t ev;
    while (evo_browser_poll_event(&ev)) {
        switch (ev.kind) {
        case EVOB_EV_URL:
            status_.uri = ev.text;
            break;
        case EVOB_EV_TITLE:
            status_.title = ev.text;
            break;
        case EVOB_EV_LOAD_STARTED:
            status_.loading = true;
            status_.progress = 0.1f;
            break;
        case EVOB_EV_LOAD_FINISHED:
            status_.loading = false;
            status_.progress = 1.0f;
            break;
        case EVOB_EV_LOAD_FAILED: {
            status_.loading = false;
            status_.progress = 1.0f;
            EngineEvent e;
            e.type = EngineEvent::Type::LoadFailed;
            const char* nl = std::strchr(ev.text, '\n');
            e.url = nl ? std::string(ev.text, static_cast<size_t>(nl - ev.text)) : std::string(ev.text);
            e.message = nl ? nl + 1 : "";
            events_.push_back(e);
            break;
        }
        case EVOB_EV_NAV_STATE:
            status_.can_go_back = (ev.arg & 1u) != 0;
            status_.can_go_forward = (ev.arg & 2u) != 0;
            break;
        case EVOB_EV_TEXT_FOCUS:
            text_focus_ = ev.arg != 0;
            break;
        case EVOB_EV_NEW_WINDOW:
            /* one page at a time: a new window replaces this one */
            if (ev.text[0]) load_uri(ev.text);
            break;
        case EVOB_EV_CURSOR:
            hand_ = ev.arg == 1;
            break;
        case EVOB_EV_FULLSCREEN: {
            EngineEvent e;
            e.type = ev.arg ? EngineEvent::Type::FullscreenEntered : EngineEvent::Type::FullscreenExited;
            events_.push_back(e);
            break;
        }
        case EVOB_EV_ENGINE_ERROR: {
            EVO_LOGW("browser", "engine error: %s", ev.text);
            EngineEvent e;
            e.type = EngineEvent::Type::ProcessTerminated;
            e.url = status_.uri;
            e.message = ev.text;
            events_.push_back(e);
            break;
        }
        default:
            break;
        }
    }
}

}  // namespace evo::web
