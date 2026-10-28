#ifndef EVO_NET_ART_SIDECAR_HPP
#define EVO_NET_ART_SIDECAR_HPP

#include <string>
#include <vector>

namespace evo {

struct SidecarEntry {
    std::string name;
    std::string url;
    bool isDirectory = false;
};

// Given the media file's name (with extension) and the other entries of the SAME folder listing,
// return the index of the best cover image, or -1. Pure function, no I/O.
// Priority (case-insensitive name compare, extensions .jpg .jpeg .png only):
//   1. <name>-poster.<ext>   2. <name>.<ext>   (<name> = file name without its last extension)
//   3. poster.<ext>  4. folder.<ext>  5. cover.<ext>
// Never pick a directory; never pick the media file itself. Within one tier prefer .jpg, .jpeg, .png.
int pickSidecar(const std::string& mediaName, const std::vector<SidecarEntry>& siblings);

} // namespace evo

#endif // EVO_NET_ART_SIDECAR_HPP
