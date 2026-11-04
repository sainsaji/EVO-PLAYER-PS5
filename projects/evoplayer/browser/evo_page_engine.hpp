/*
 * evo_page_engine.hpp - Prospero Browser's PageEngine on EVO's browser engine
 * seam (#237).
 *
 * The browser app and its kit UI are copied from Prospero Browser
 * (browser/browser, browser/ui). They drive a page through web::PageEngine;
 * this is that engine for EVO: every call goes to the out-of-process
 * ProsperoWeb host through evo_browser_engine.h, and the host's events come
 * back as Status and EngineEvents.
 *
 * One page at a time: the host shows one web view, so only one EvoPageEngine
 * exists; further tabs get none and use the built-in renderer.
 *
 * Frames do not pass through take_frame(): the kit copies the host's shared
 * buffers straight into its textures (WebFrame in evo_hui_app.cpp), and the
 * browser UI draws that texture in the page viewport.
 */
#pragma once

#include "web/page_engine.hpp"

#include <string>
#include <vector>

namespace evo::web {

class EvoPageEngine : public prospero::web::PageEngine {
public:
    EvoPageEngine(uint32_t width, uint32_t height);
    ~EvoPageEngine() override;

    /* Registers the factory with Prospero's browser (once, at startup). */
    static void install();
    /* The live engine, or nullptr. */
    static EvoPageEngine* current();

    void load_uri(const std::string& uri) override;
    void reload() override;
    void stop_loading() override;
    void go_back() override;
    void go_forward() override;
    void set_zoom_level(float zoom) override;
    void resize(uint32_t width, uint32_t height) override;

    void pointer_motion(int32_t x, int32_t y) override;
    void pointer_button(int32_t x, int32_t y, uint32_t button, bool pressed) override;
    void scroll(int32_t x, int32_t y, int32_t delta_x, int32_t delta_y) override;
    void key(uint32_t key_code, bool pressed, uint32_t modifiers) override;

    Status status() const override { return status_; }
    bool take_frame(const FrameSink& sink) override;
    std::vector<prospero::web::EngineEvent> take_events() override;
    void set_visible(bool visible) override;

    /* Text typed with EVO's keyboard, inserted into the focused field. */
    void insert_text(const std::string& utf8);
    /* 1 once after the page focused an editable field (EVO opens its keyboard). */
    bool take_text_focus();
    /* The engine's cursor is a hand (a link under the pointer). */
    bool hand() const { return hand_; }
    /* Starting, ready or failed, with a reason for the user when failed. */
    int engine_state(std::string* why) const;

private:
    void pump();

    uint32_t width_, height_;
    bool visible_ = true;
    bool setup_sent_ = false;   /* size + visibility sent once the host is ready */
    bool hand_ = false;
    bool text_focus_ = false;
    bool failed_reported_ = false;
    Status status_;
    std::vector<prospero::web::EngineEvent> events_;
};

}  // namespace evo::web
