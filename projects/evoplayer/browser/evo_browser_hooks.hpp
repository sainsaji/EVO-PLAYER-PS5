/*
 * evo_browser_hooks.hpp - what the copied Prospero Browser code needs from EVO
 * (#237). Kept to the few places the copies had to change; each is marked
 * "EVO:" in the copied file.
 */
#pragma once

#include <cstdint>

/* The engine page as the kit holds it: texture handle and the UV corner of
 * the page inside it (the texture rows can be wider than the page). 0 when no
 * frame has arrived yet. Implemented in ui_kit/src/evo_hui_app.cpp. */
extern "C" int evo_browser_page_texture(std::uint32_t* tex, float* u1, float* v1);
