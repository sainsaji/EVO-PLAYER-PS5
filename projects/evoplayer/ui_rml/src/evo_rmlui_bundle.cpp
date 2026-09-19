#include "evo_rmlui_bundle.h"
#include <cstring>

namespace {

/*
 * Every prefix the codebase has ever glued onto an asset filename, longest
 * first so e.g. "/workspace/projects/evoplayer/assets/" is stripped whole
 * rather than partially matching a shorter prefix first. Once stripped, what
 * remains is exactly a bundle key: "rml/launch.rml", "fonts/Lato-Bold.ttf",
 * "icons/icon_home.png".
 *
 * NOTE: this list is intentionally the same "wherever a dev might be running
 * this from" set evo_rmlui_app.cpp and evo_rmlui_render.cpp already tried one
 * at a time before #60 - it is not new surface area, just centralized.
 */
const char* const kPrefixes[] = {
    "/workspace/projects/evoplayer/assets/",
    "/data/homebrew/EVOPlayer/assets/",
    "projects/evoplayer/assets/",
    "/mnt/usb0/assets/",
    "/app0/assets/",
    "assets/",
};

std::string Normalize(const std::string& raw_path) {
    for (const char* prefix : kPrefixes) {
        size_t n = std::strlen(prefix);
        if (raw_path.compare(0, n, prefix) == 0)
            return raw_path.substr(n);
    }
    /* Not one of the known prefixes - maybe already a bare bundle key
     * ("rml/launch.rml"), maybe an unrelated path. Either way, try it as-is;
     * the table lookup below is the real filter. */
    return raw_path;
}

} // namespace

const EvoRmlBundleFile* evo_rmlui_bundle_find(const std::string& raw_path) {
    const std::string key = Normalize(raw_path);
    for (unsigned i = 0; i < EVO_RML_BUNDLE_COUNT; i++) {
        if (key == EVO_RML_BUNDLE[i].path)
            return &EVO_RML_BUNDLE[i];
    }
    return nullptr;
}
