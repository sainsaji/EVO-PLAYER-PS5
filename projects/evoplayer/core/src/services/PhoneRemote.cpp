/*
 * PhoneRemote.cpp - the phone as a remote (#238). See evo_phone_remote.h.
 *
 * The log server's thread only queues text commands here; everything that
 * touches the player runs on the main thread in evo_phone_remote_poll(), the
 * same thread the pad drives it from. Keys become one-frame button presses,
 * exactly like the dev remote's `key`, so menus react as to a real tap.
 */
#include "evo_phone_remote.h"
#include "evo_usb_remote.h"   /* evo_player_position_s, evo_stop_media_playback */
#include "evo_boot_log.h"
#include "evo/Application.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace
{

std::mutex g_mx;
constexpr int kQueue = 16;
std::string g_queue[kQueue];
int g_count = 0;

unsigned int g_buttons = 0;  /* main thread only */

struct Snapshot
{
    int active = 0, paused = 0, music = 0;
    double pos = 0.0, dur = 0.0;
    std::string title;
};
Snapshot g_snap;             /* guarded by g_mx */

/* Pad masks, as in evo_usb_remote.c (kept as literals: that file is C). */
const struct { const char *name; unsigned int mask; } kKeys[] = {
    {"up", 0x0010}, {"right", 0x0020}, {"down", 0x0040}, {"left", 0x0080},
    {"l2", 0x0100}, {"r2", 0x0200}, {"l1", 0x0400}, {"r1", 0x0800},
    {"triangle", 0x1000}, {"circle", 0x2000}, {"cross", 0x4000}, {"square", 0x8000},
    {"options", 0x0008},
};

unsigned int key_mask(const char *name)
{
    for (const auto &k : kKeys)
        if (std::strcmp(name, k.name) == 0)
            return k.mask;
    return 0;
}

bool known(const char *c)
{
    if (std::strncmp(c, "key ", 4) == 0)
        return key_mask(c + 4) != 0;
    if (std::strncmp(c, "seek ", 5) == 0)
        return c[5] != '\0';
    if (std::strncmp(c, "chapter ", 8) == 0)
        return true;
    return std::strcmp(c, "pause") == 0 || std::strcmp(c, "stop") == 0;
}

void run(const std::string &cmd)
{
    const char *c = cmd.c_str();
    auto *pc = evo::Application::getInstance().getPlaybackController();
    if (std::strncmp(c, "key ", 4) == 0)
    {
        g_buttons |= key_mask(c + 4);
        return;
    }
    if (!pc || !pc->isActive())
        return;  /* the player commands need something playing */
    if (std::strcmp(c, "pause") == 0)
        pc->togglePause();
    else if (std::strcmp(c, "stop") == 0)
        evo_stop_media_playback();
    else if (std::strncmp(c, "chapter ", 8) == 0)
        pc->jumpChapter(std::atoi(c + 8) < 0 ? -1 : 1);
    else if (std::strncmp(c, "seek ", 5) == 0)
    {
        const char *a = c + 5;
        double t = (a[0] == '+' || a[0] == '-') ? evo_player_position_s() + std::atof(a) : std::atof(a);
        const double dur = pc->getDurationSeconds();
        if (t < 0.0)
            t = 0.0;
        if (dur > 1.0 && t > dur - 1.0)
            t = dur - 1.0;
        pc->seekTo(t);
    }
}

void json_str(std::string &out, const std::string &s)
{
    out += '"';
    for (unsigned char ch : s)
    {
        if (ch == '"' || ch == '\\')
        {
            out += '\\';
            out += static_cast<char>(ch);
        }
        else if (ch < 0x20)
            out += ' ';
        else
            out += static_cast<char>(ch);
    }
    out += '"';
}

} // namespace

extern "C" int evo_phone_remote_post(const char *cmd)
{
    if (!cmd)
        return -1;
    std::string c(cmd);
    while (!c.empty() && (c.back() == '\n' || c.back() == '\r' || c.back() == ' '))
        c.pop_back();
    if (c.empty() || c.size() > 64 || !known(c.c_str()))
        return -1;
    std::lock_guard<std::mutex> lock(g_mx);
    if (g_count >= kQueue)
        return -1;
    g_queue[g_count++] = c;
    return 0;
}

extern "C" void evo_phone_remote_poll(void)
{
    std::string todo[kQueue];
    int n = 0;
    {
        std::lock_guard<std::mutex> lock(g_mx);
        for (int i = 0; i < g_count; ++i)
            todo[n++] = std::move(g_queue[i]);
        g_count = 0;
    }
    for (int i = 0; i < n; ++i)
        run(todo[i]);

    Snapshot s;
    if (auto *pc = evo::Application::getInstance().getPlaybackController())
    {
        s.active = pc->isActive() ? 1 : 0;
        if (s.active)
        {
            s.paused = pc->isPaused() ? 1 : 0;
            s.music = pc->isMusicMode() ? 1 : 0;
            s.pos = evo_player_position_s();
            s.dur = pc->getDurationSeconds();
            s.title = pc->getDisplayTitle();
        }
    }
    std::lock_guard<std::mutex> lock(g_mx);
    g_snap = std::move(s);
}

extern "C" unsigned int evo_phone_remote_take_buttons(void)
{
    const unsigned int m = g_buttons;
    g_buttons = 0;
    return m;
}

extern "C" size_t evo_phone_remote_state_json(char *out, size_t cap)
{
    if (!out || cap == 0)
        return 0;
    Snapshot s;
    {
        std::lock_guard<std::mutex> lock(g_mx);
        s = g_snap;
    }
    std::string j = "{\"active\":" + std::to_string(s.active) + ",\"paused\":" + std::to_string(s.paused) +
                    ",\"music\":" + std::to_string(s.music);
    char num[64];
    std::snprintf(num, sizeof num, ",\"pos\":%.1f,\"dur\":%.1f,\"title\":", s.pos, s.dur);
    j += num;
    json_str(j, s.title);
    j += "}";
    const size_t n = j.size() < cap - 1 ? j.size() : cap - 1;
    std::memcpy(out, j.data(), n);
    out[n] = '\0';
    return n;
}
