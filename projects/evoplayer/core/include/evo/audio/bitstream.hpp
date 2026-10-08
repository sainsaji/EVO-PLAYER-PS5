// evo/audio/bitstream.hpp - HDMI sink monitoring and format definitions.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "evo/audio/iec61937.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pt
{

// One format the TV or receiver lists in its HDMI EDID.
struct SinkFormat
{
    int coding = 0;   // CEA-861 audio coding type: 1 PCM, 2 AC-3, 6 AAC, 7 DTS, 10 E-AC-3, ...
    int channels = 0;
};

struct Sink
{
    bool ok = false;
    int rc = 0;       // what sceAudioOutSysGetHdmiMonitorInfo returned
    std::string name; // monitor name from the EDID
    std::vector<SinkFormat> formats;

    bool supports(int coding) const;
};

// CEA-861 coding type for a codec, to compare with Sink::formats.
int coding_for(Codec codec);
const char *coding_name(int coding);

// Queries HDMI sink capability list using sceAudioOutSysGetHdmiMonitorInfo.
Sink query_sink();

} // namespace pt
