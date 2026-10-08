/*
 * evo_hui_app.cpp - runs EVO's ps5-homebrew-ui screens on the console.
 *
 * Owns the AGC batch, the fonts and the screen objects. Started lazily on the
 * first kit render, which is always inside an AGC frame on the main thread, so
 * the runtime and ui_sdf.pipe are known to be up. If anything fails the kit
 * stays off for the session and every screen keeps its RmlUi version.
 */
#include "evo_hui.h"

#include "evo_hui_browser.hpp"
#include "evo_hui_extra.hpp"
#include "evo_hui_misc.hpp"
#include "evo_hui_modals.hpp"
#include "evo_hui_osd.hpp"
#include "evo_hui_screens.hpp"
#include "evo_hui_settings.hpp"
#include "hui_agc_batch.hpp"

#include "evo_agc_runtime.h"
#include "evo_boot_log.h"
#include "evo_rmlui_bundle.h"

#include "gfx/font.hpp"
#include "ui/fonts.hpp"

#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#include <string>

namespace
{

using evo::kit::Context;

class BundleTextures final : public evo::kit::TextureHost
{
  public:
    explicit BundleTextures(evo::hui_agc::AgcBatch &batch) : batch_(batch) {}
    std::uint32_t create_texture(int w, int h, const std::uint8_t *rgba) override
    {
        return batch_.create_texture(w, h, rgba);
    }
    void release_texture(std::uint32_t handle) override
    {
        batch_.release_texture(handle);
    }
    bool read_asset(const std::string &path, std::string *bytes) override
    {
        /* Screens use the RmlUi documents' relative paths ("../icons/x.png");
         * bundle keys are relative to assets/. */
        std::string key = path;
        while (key.compare(0, 3, "../") == 0)
            key.erase(0, 3);
        const EvoRmlBundleFile *f = evo_rmlui_bundle_find(key);
        if (!f)
            return false;
        bytes->assign(reinterpret_cast<const char *>(f->data), f->size);
        return true;
    }

  private:
    evo::hui_agc::AgcBatch &batch_;
};

double now_seconds()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

enum class Screen
{
    none,
    home,
    list,
    browser,
    settings,
    osd,
    subtitles,
    mediainfo,
    about,
    changelog,
    closed,
    reader,
    image,
    dialog, /* overlays from here on */
    toast,
    keyboard,
    count,
};

const char *doc_name(Screen s)
{
    switch (s)
    {
    case Screen::home:
        return "hui_home";
    case Screen::list:
        return "hui_list";
    case Screen::browser:
        return "hui_browser";
    case Screen::settings:
        return "hui_settings";
    case Screen::osd:
        return "hui_osd";
    case Screen::subtitles:
        return "hui_subtitles";
    case Screen::mediainfo:
        return "hui_mediainfo";
    case Screen::about:
        return "hui_about";
    case Screen::changelog:
        return "hui_changelog";
    case Screen::closed:
        return "hui_closed";
    case Screen::reader:
        return "hui_reader";
    case Screen::image:
        return "hui_image";
    default:
        return "hui";
    }
}

struct Kit
{
    enum State
    {
        untried,
        up,
        failed,
    } state = untried;

    evo::hui_agc::AgcBatch batch;
    std::unique_ptr<BundleTextures> host;
    std::unique_ptr<evo::kit::TextureCache> textures;
    hui::gfx::Font font_regular, font_semibold, font_display, font_mono;
    hui::ui::Fonts fonts;
    evo::kit::Palette palette;

    evo::kit::NavRail rail;
    evo::kit::HomeScreen home;
    evo::kit::ListScreen list;
    evo::kit::BrowserScreen browser;
    evo::kit::SettingsScreen settings;
    evo::kit::OsdScreen osd;
    evo::kit::SubtitlePicker subtitles;
    evo::kit::MediaInfoCard mediainfo;
    evo::kit::AboutScreen about;
    evo::kit::ChangelogScreen changelog;
    evo::kit::ClosedScreen closed;
    evo::kit::DialogOverlay dialog;
    evo::kit::ToastOverlay toast;
    evo::kit::ReaderScreen reader;
    evo::kit::ImageViewer image;
    evo::kit::KeyboardOverlay keyboard;
    bool keyboard_visible = false;
    /* Frame bookkeeping: one dt per presented frame, however many kit draws
     * it has (a screen, then a dialog over it). */
    unsigned frame_serial = 0;
    unsigned stepped_serial = ~0u;
    float frame_dt = 0.0f;
    /* Frame each overlay last drew in; far in the past to begin with. */
    unsigned overlay_serial[static_cast<int>(Screen::count)] = {};
    bool dialog_up = false;

    Screen current = Screen::none;
    Screen drawn_last_frame = Screen::none;
    int idle_frames = 0;
    double last_time = 0.0;
    float clock = 0.0f;

    bool load_font(const char *path, hui::gfx::Font *font, hui::ui::FontRef *ref)
    {
        const EvoRmlBundleFile *f = evo_rmlui_bundle_find(path);
        if (!f || !font->load(std::string_view(reinterpret_cast<const char *>(f->data), f->size)))
        {
            evo_boot_log("hui: font %s missing or unreadable", path);
            return false;
        }
        ref->font = font;
        ref->texture = batch.create_font_texture(*font);
        if (!ref->texture)
        {
            evo_boot_log("hui: font %s upload failed", path);
            return false;
        }
        return true;
    }

    bool start()
    {
        if (state != untried)
            return state == up;
        state = failed;
        for (unsigned &s : overlay_serial)
            s = 0x80000000u;
        if (access("/mnt/usb0/evo_no_hui", F_OK) == 0)
        {
            evo_boot_log("hui: /mnt/usb0/evo_no_hui present, kit screens off");
            return false;
        }
        if (!evo_agc_runtime_is_active() || !batch.init())
            return false;
        if (!load_font("hui/fonts/inter-regular.huifont", &font_regular, &fonts.regular) ||
            !load_font("hui/fonts/inter-semibold.huifont", &font_semibold, &fonts.semibold) ||
            !load_font("hui/fonts/montserrat-medium.huifont", &font_display, &fonts.display) ||
            !load_font("hui/fonts/dejavu-sans-mono.huifont", &font_mono, &fonts.mono))
        {
            batch.release();
            return false;
        }
        fonts.pixel = fonts.mono;
        fonts.hand = fonts.regular;
        host = std::make_unique<BundleTextures>(batch);
        textures = std::make_unique<evo::kit::TextureCache>(*host);
        last_time = now_seconds();
        state = up;
        evo_boot_log("hui: kit screens up");
        return true;
    }

    /* Advances the clock once per frame, however many kit draws it has. */
    float step()
    {
        if (stepped_serial == frame_serial)
            return frame_dt;
        stepped_serial = frame_serial;
        const double t = now_seconds();
        float dt = static_cast<float>(t - last_time);
        last_time = t;
        if (dt < 0.0f)
            dt = 0.0f;
        if (dt > 0.1f)
            dt = 0.1f; /* a stall must not throw springs across the screen */
        clock += dt;
        frame_dt = dt;
        return dt;
    }

    /* Records `screen` into the current AGC frame. */
    template <typename DrawFn>
    int render(Screen screen, int width, int height, DrawFn &&draw_screen, bool with_rail = true)
    {
        if (!start())
            return 0;
        if (current != screen)
        {
            current = screen;
            if (screen == Screen::home)
                home.enter();
            else if (screen == Screen::list)
                list.enter();
            else if (screen == Screen::browser)
                browser.enter();
            else if (screen == Screen::settings)
                settings.enter();
            else if (screen == Screen::osd)
                osd.enter();
            else if (screen == Screen::subtitles)
                subtitles.enter();
            else if (screen == Screen::mediainfo)
                mediainfo.enter();
            else if (screen == Screen::about)
                about.enter();
            else if (screen == Screen::changelog)
                changelog.enter();
            else if (screen == Screen::closed)
                closed.enter();
            else if (screen == Screen::reader)
                reader.enter();
            else if (screen == Screen::image)
                image.enter();
        }
        const float dt = step();
        rail.update(dt);
        drawn_last_frame = screen;
        idle_frames = 0;

        evo_agc_runtime_frame_begin();
        Context ctx{fonts, *textures, palette, clock};
        hui::gfx::DrawList &list = scratch;
        list.clear();
        draw_screen(dt, list, ctx);
        if (with_rail)
            rail.draw(list, ctx);
        batch.draw(list, hui::gfx::fit_viewport(width, height), width, height);
        return 1;
    }

    /* An overlay over this frame's screen: leaves `current` alone, and runs
     * the overlay's entrance when it was not up on the previous frame. */
    template <typename DrawFn>
    int render_overlay(Screen overlay, int width, int height, DrawFn &&draw_overlay)
    {
        if (!start())
            return 0;
        const int slot = static_cast<int>(overlay);
        /* Not drawn this frame or the one before: it has just appeared. */
        const bool fresh = overlay_serial[slot] != frame_serial && overlay_serial[slot] + 1 != frame_serial;
        if (fresh)
        {
            if (overlay == Screen::dialog)
                dialog.enter();
            else if (overlay == Screen::toast)
                toast.enter();
            else if (overlay == Screen::keyboard)
                keyboard.enter();
        }
        overlay_serial[slot] = frame_serial;
        const float dt = step();
        if (overlay == Screen::dialog)
            dialog_up = true;

        evo_agc_runtime_frame_begin();
        Context ctx{fonts, *textures, palette, clock};
        hui::gfx::DrawList &list = overlay_scratch;
        list.clear();
        draw_overlay(dt, list, ctx);
        batch.draw(list, hui::gfx::fit_viewport(width, height), width, height);
        return 1;
    }

    hui::gfx::DrawList scratch;
    hui::gfx::DrawList overlay_scratch;
};

Kit &kit()
{
    /* Never destroyed: shutdown is explicit, like EvoRmlApp's. */
    static Kit *k = new Kit();
    return *k;
}

} // namespace

extern "C" {

void evo_hui_shutdown(void)
{
    Kit &k = kit();
    if (k.state != Kit::up)
        return;
    k.textures.reset();
    k.host.reset();
    k.batch.release();
    k.state = Kit::failed;
}

int evo_hui_wants_frame(void)
{
    Kit &k = kit();
    return (k.state == Kit::up && k.drawn_last_frame != Screen::none) ? 1 : 0;
}

void evo_hui_end_frame(void)
{
    Kit &k = kit();
    if (k.state != Kit::up)
        return;
    k.batch.end_frame();
    ++k.frame_serial;
    k.textures->tick();
    /* A frame in which no kit screen drew means an RmlUi screen took over. */
    if (++k.idle_frames > 1)
    {
        k.drawn_last_frame = Screen::none;
        k.current = Screen::none;
    }
}

void evo_hui_set_theme(const evo_rmlui_theme_t *theme)
{
    if (theme)
        kit().palette = evo::kit::Palette::from(*theme);
}

void evo_hui_update_nav(const evo_rmlui_nav_params_t *params)
{
    if (params)
        kit().rail.set(*params);
}

const char *evo_hui_dev_focus_json(const char **screen_doc)
{
    Kit &k = kit();
    if (k.state != Kit::up || k.drawn_last_frame == Screen::none)
        return nullptr;
    const char *doc = doc_name(k.drawn_last_frame);
    if (screen_doc)
        *screen_doc = doc;

    evo::kit::FocusInfo f;
    const bool dialog_shown = k.overlay_serial[static_cast<int>(Screen::dialog)] + 1 >= k.frame_serial;
    const bool kb_shown = k.overlay_serial[static_cast<int>(Screen::keyboard)] + 1 >= k.frame_serial;
    bool has = kb_shown && k.keyboard.focus(&f);
    if (has && screen_doc)
        *screen_doc = "hui_keyboard";
    if (!has)
        has = dialog_shown && k.dialog.focus(&f);
    if (has && screen_doc)
        *screen_doc = "hui_dialog";
    if (!has)
        has = k.rail.focus(&f);
    if (!has && k.drawn_last_frame == Screen::home)
        has = k.home.focus(&f);
    else if (!has && k.drawn_last_frame == Screen::list)
        has = k.list.focus(&f);
    else if (!has && k.drawn_last_frame == Screen::browser)
        has = k.browser.focus(&f);
    else if (!has && k.drawn_last_frame == Screen::settings)
        has = k.settings.focus(&f);
    else if (!has && k.drawn_last_frame == Screen::osd)
        has = k.osd.focus(&f);
    else if (!has && k.drawn_last_frame == Screen::subtitles)
        has = k.subtitles.focus(&f);
    else if (!has && k.drawn_last_frame == Screen::mediainfo)
        has = k.mediainfo.focus(&f);
    else if (!has && k.drawn_last_frame == Screen::about)
        has = k.about.focus(&f);
    else if (!has && k.drawn_last_frame == Screen::changelog)
        has = k.changelog.focus(&f);
    else if (!has && k.drawn_last_frame == Screen::reader)
        has = k.reader.focus(&f);
    else if (!has && k.drawn_last_frame == Screen::image)
        has = k.image.focus(&f);
    static std::string o;
    o.clear();
    if (!has)
    {
        o = "null";
        return o.c_str();
    }
    auto str = [&](const std::string &s) {
        o += '"';
        for (unsigned char c : s)
        {
            if (c == '"' || c == '\\')
            {
                o += '\\';
                o += static_cast<char>(c);
            }
            else if (c < 0x20)
            {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                o += b;
            }
            else
            {
                o += static_cast<char>(c);
            }
        }
        o += '"';
    };
    o += "{\"id\":";
    str(f.id);
    o += ",\"tag\":\"hui\",\"class\":\"focused\",\"text\":";
    str(f.text);
    o += ",\"doc\":";
    str(doc);
    char b[160];
    std::snprintf(b, sizeof b,
                  ",\"index\":%d,\"total\":%d,\"rect\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
                  f.index, f.total, static_cast<int>(f.rect.x), static_cast<int>(f.rect.y),
                  static_cast<int>(f.rect.w), static_cast<int>(f.rect.h));
    o += b;
    o += ",\"focusable\":[";
    for (std::size_t i = 0; i < f.focusable.size(); ++i)
    {
        if (i)
            o += ',';
        o += "{\"id\":";
        str(f.focusable[i].first);
        o += ",\"text\":";
        str(f.focusable[i].second);
        o += '}';
    }
    o += "]}";
    return o.c_str();
}

void evo_hui_update_launch(const evo_rmlui_launch_params_t *params)
{
    if (params)
        kit().home.set(*params);
}

int evo_hui_render_launch(int width, int height)
{
    Kit &k = kit();
    return k.render(Screen::home, width, height,
                    [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                        k.home.update(dt);
                        k.home.draw(list, ctx);
                    });
}

void evo_hui_update_list(const evo_rmlui_list_params_t *params)
{
    if (params)
        kit().list.set(*params);
}

int evo_hui_render_list(int width, int height)
{
    Kit &k = kit();
    return k.render(Screen::list, width, height,
                    [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                        k.list.update(dt);
                        k.list.draw(list, ctx);
                    });
}

void evo_hui_update_browser(const evo_rmlui_browser_params_t *params)
{
    if (params)
        kit().browser.set(*params);
}

int evo_hui_render_browser(int width, int height)
{
    Kit &k = kit();
    return k.render(Screen::browser, width, height,
                    [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                        k.browser.update(dt);
                        k.browser.draw(list, ctx);
                    });
}

void evo_hui_update_settings(const evo_rmlui_settings_params_t *params)
{
    if (params)
        kit().settings.set(*params);
}

int evo_hui_render_settings(int width, int height)
{
    Kit &k = kit();
    return k.render(Screen::settings, width, height,
                    [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                        k.settings.update(dt);
                        k.settings.draw(list, ctx);
                    });
}

void evo_hui_update_playback(const evo_playback_osd_params_t *params)
{
    if (params)
        kit().osd.set(*params);
}

void evo_hui_update_perf_hud(const evo_perf_hud_t *hud)
{
    if (hud)
        kit().osd.set_hud(*hud);
}

int evo_hui_render_playback_osd(int width, int height)
{
    Kit &k = kit();
    if (!k.start() || !k.osd.can_draw(k.fonts))
        return 0;
    /* Over the video: no rail, no backdrop. */
    return k.render(
        Screen::osd, width, height,
        [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
            k.osd.update(dt);
            k.osd.draw(list, ctx);
        },
        false);
}

void evo_hui_update_subtitles(const evo_rmlui_subtitles_params_t *params)
{
    if (params)
        kit().subtitles.set(*params);
}

int evo_hui_render_subtitles(int width, int height)
{
    Kit &k = kit();
    return k.render(
        Screen::subtitles, width, height,
        [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
            k.subtitles.update(dt);
            k.subtitles.draw(list, ctx);
        },
        false);
}

void evo_hui_update_mediainfo(const evo_rmlui_mediainfo_params_t *params)
{
    if (params)
        kit().mediainfo.set(*params);
}

int evo_hui_render_mediainfo(int width, int height)
{
    Kit &k = kit();
    return k.render(
        Screen::mediainfo, width, height,
        [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
            k.mediainfo.update(dt);
            k.mediainfo.draw(list, ctx);
        },
        false);
}

void evo_hui_update_about(const evo_rmlui_about_params_t *params)
{
    if (params)
        kit().about.set(*params);
}

int evo_hui_render_about(int width, int height)
{
    Kit &k = kit();
    return k.render(Screen::about, width, height,
                    [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                        k.about.update(dt);
                        k.about.draw(list, ctx);
                    });
}

void evo_hui_update_changelog(const evo_rmlui_changelog_params_t *params)
{
    if (params)
        kit().changelog.set(*params);
}

int evo_hui_render_changelog(int width, int height)
{
    Kit &k = kit();
    return k.render(Screen::changelog, width, height,
                    [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                        k.changelog.update(dt);
                        k.changelog.draw(list, ctx);
                    });
}

int evo_hui_render_closed(int width, int height)
{
    Kit &k = kit();
    return k.render(
        Screen::closed, width, height,
        [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
            k.closed.update(dt);
            k.closed.draw(list, ctx);
        },
        false);
}

void evo_hui_update_dialog(const evo_rmlui_dialog_params_t *params)
{
    if (params)
        kit().dialog.set(*params);
}

int evo_hui_render_dialog(int width, int height)
{
    Kit &k = kit();
    return k.render_overlay(Screen::dialog, width, height,
                            [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                                k.dialog.update(dt);
                                k.dialog.draw(list, ctx);
                            });
}

void evo_hui_update_toast(const evo_rmlui_toast_params_t *params)
{
    if (params)
        kit().toast.set(*params);
}

int evo_hui_render_toast(int width, int height)
{
    Kit &k = kit();
    return k.render_overlay(Screen::toast, width, height,
                            [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                                k.toast.update(dt);
                                k.toast.draw(list, ctx);
                            });
}

void evo_hui_update_reader(const evo_rmlui_reader_params_t *params)
{
    if (params)
        kit().reader.set(*params);
}

int evo_hui_render_reader(int width, int height)
{
    Kit &k = kit();
    return k.render(Screen::reader, width, height,
                    [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                        k.reader.update(dt);
                        k.reader.draw(list, ctx);
                    });
}

void evo_hui_update_image(const evo_rmlui_image_params_t *params)
{
    if (params)
        kit().image.set(*params);
}

int evo_hui_render_image(int width, int height)
{
    Kit &k = kit();
    return k.render(
        Screen::image, width, height,
        [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
            k.image.update(dt);
            k.image.draw(list, ctx);
        },
        false);
}

void evo_hui_update_keyboard(const evo_keyboard_params_t *params)
{
    if (!params)
        return;
    Kit &k = kit();
    k.keyboard_visible = params->visible != 0;
    k.keyboard.set(*params);
}

int evo_hui_render_keyboard(int width, int height)
{
    Kit &k = kit();
    if (!k.keyboard_visible)
        return 0; /* nothing to draw: RmlUi's keyboard is hidden too */
    return k.render_overlay(Screen::keyboard, width, height,
                            [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                                k.keyboard.update(dt);
                                k.keyboard.draw(list, ctx);
                            });
}

} /* extern "C" */
