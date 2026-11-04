/*
 * evo_shims.cpp - the kit's hui::sys functions the copied Prospero Browser
 * code calls (#237). EVO does not link the kit's platform/ps5/system.cpp (it
 * writes to stdout, which the app module does not have); its log goes to
 * evo.log instead.
 */
#include "platform/ps5/system.hpp"

#include "evo_boot_log.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace hui::sys
{

std::int64_t monotonic_us()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

void log(const char *format, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(buf, sizeof buf, format, ap);
    va_end(ap);
    EVO_LOGI("browser", "%s", buf);
}

}  // namespace hui::sys
