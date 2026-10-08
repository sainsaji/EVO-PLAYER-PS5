// evo/audio/bitstream.cpp - HDMI sink capability querying via libSceAudioOut.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "evo/audio/bitstream.hpp"

#include <cstring>

extern "C"
{
    int sceAudioOutSysGetHdmiMonitorInfo(int type, void *out, unsigned size); // Tf9-yOJwF-A
}

namespace pt
{

bool Sink::supports(int coding) const
{
    for (const SinkFormat &f : formats)
        if (f.coding == coding)
            return true;
    return false;
}

int coding_for(Codec codec)
{
    switch (codec)
    {
    case Codec::ac3:
        return 2;  // AC-3
    case Codec::dts:
        return 7;  // DTS
    case Codec::aac:
        return 6;  // AAC
    case Codec::eac3:
        return 10; // E-AC-3
    case Codec::truehd:
        return 12; // Dolby TrueHD (MAT)
    case Codec::unknown:
        break;
    }
    return 0;
}

const char *coding_name(int coding)
{
    switch (coding)
    {
    case 1:
        return "PCM";
    case 2:
        return "Dolby Digital";
    case 6:
        return "AAC";
    case 7:
        return "DTS";
    case 10:
        return "Dolby Digital Plus";
    case 11:
        return "DTS-HD";
    case 12:
        return "Dolby TrueHD";
    default:
        return "Other";
    }
}

Sink query_sink()
{
    Sink info;
    std::uint8_t buffer[0x180] = {};
    info.rc = sceAudioOutSysGetHdmiMonitorInfo(1 /* HDMI */, buffer, sizeof(buffer));
    if (info.rc < 0)
        return info;

    info.ok = true;
    char name[14] = {};
    std::memcpy(name, buffer + 0x0B, 13);
    for (char &c : name)
        if (c != 0 && (c < 0x20 || c > 0x7E))
            c = 0;
    info.name = name;
    while (!info.name.empty() && info.name.back() == ' ')
        info.name.pop_back();

    std::uint32_t count = 0;
    std::memcpy(&count, buffer + 0x94, sizeof(count));
    for (std::uint32_t i = 0; i < count && 0x98 + (i + 1) * 8 <= sizeof(buffer); ++i)
    {
        const std::uint8_t *entry = buffer + 0x98 + i * 8;
        if (entry[0] == 0 || entry[0] > 15)
            break;
        info.formats.push_back({entry[0], entry[1]});
    }
    return info;
}

} // namespace pt
