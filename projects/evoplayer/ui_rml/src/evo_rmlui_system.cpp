#include "evo_rmlui_system.h"
#include <chrono>
#include <iostream>

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
        std::cerr << "[RmlUi ERROR] " << message << std::endl;
    } else if (type == Rml::Log::LT_WARNING) {
        std::cerr << "[RmlUi WARN]  " << message << std::endl;
    }
    return true;
}
