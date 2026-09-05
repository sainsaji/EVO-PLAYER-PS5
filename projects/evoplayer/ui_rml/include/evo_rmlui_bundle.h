#pragma once
/*
 * #60: the RmlUi asset set (assets/rml, assets/fonts, assets/icons), embedded
 * into the binary so the .ffpfsc is self-contained - see
 * tools/bundle_rml_assets.py for why (the /app0 sandbox EPERMs directory
 * traversal, so loose files inside the package can't be relied on).
 *
 * EVO_RML_BUNDLE / EVO_RML_BUNDLE_COUNT are defined in the GENERATED
 * evo_rmlui_bundle_data.cpp - do not hand-edit that file, re-run the
 * generator instead. evo_rmlui_bundle_find() (evo_rmlui_bundle.cpp, hand-
 * maintained) is what everything else calls.
 */
#include <string>

struct EvoRmlBundleFile {
    const char* path;              /* canonical key, e.g. "rml/launch.rml" */
    const unsigned char* data;
    unsigned int size;
};

extern const EvoRmlBundleFile EVO_RML_BUNDLE[];
extern const unsigned int EVO_RML_BUNDLE_COUNT;

/*
 * Looks up `raw_path` in the embedded bundle. Callers pass whatever prefixed
 * form they already use (/app0/assets/rml/launch.rml,
 * projects/evoplayer/assets/icons/icon_home.png, ...) - this strips the
 * known on-disk asset-directory prefixes down to the bundle's own
 * "<rml|fonts|icons>/<filename>" key before matching. Returns nullptr when
 * the path isn't bundled (an unrecognized prefix, or a file added to disk
 * but not yet re-bundled), so the caller can fall back to the filesystem.
 */
const EvoRmlBundleFile* evo_rmlui_bundle_find(const std::string& raw_path);
