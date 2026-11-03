#ifndef EVO_RELEASE_NAME_HPP
#define EVO_RELEASE_NAME_HPP

#include <string>

namespace evo {

/* A space-separated file name made readable: release tags after the title go
 * ("Show S01E08 720p AMZN WEBRip x264-Group" -> "Show S01E08"), a year before
 * them is kept as "(2019)", a leading "www Site com -" goes, and a camera-style
 * "20261028 225403 ..." becomes "28 Oct 2026, 22:54". Empty when nothing would
 * be left. Used by MediaMetadataService::cleanMediaTitle. */
std::string tidyReleaseName(const std::string& spaced);

} // namespace evo

#endif // EVO_RELEASE_NAME_HPP
