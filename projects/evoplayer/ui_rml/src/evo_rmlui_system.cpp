#include "evo_rmlui_system.h"
#include <chrono>
#include <cstdio>

/* No <iostream>: on the app module its static init (ios_base::Init ->
 * std::locale::locale()) crashes in the custom CRT's _init() pass, layout-
 * sensitively (#71). stderr via C stdio has no C++ static-init dependency. */

static auto g_start_time = std::chrono::steady_clock::now();

EvoSystemInterface::EvoSystemInterface() {
}

EvoSystemInterface::~EvoSystemInterface() {
}

double EvoSystemInterface::GetElapsedTime() {
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> diff = now - g_start_time;
    return diff.count();
}

bool EvoSystemInterface::LogMessage(Rml::Log::Type type, const Rml::String& message) {
    if (type == Rml::Log::LT_ERROR) {
        fprintf(stderr, "[RmlUi ERROR] %s\n", message.c_str());
    } else if (type == Rml::Log::LT_WARNING) {
        fprintf(stderr, "[RmlUi WARN]  %s\n", message.c_str());
    }
    return true;
}
