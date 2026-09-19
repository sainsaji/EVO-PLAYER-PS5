/*
 * evo_features.h — compile-time feature switches.
 *
 * One header so a feature can be turned off in every place that references it
 * without a build-system change, and turned back on by editing one line.
 */
#ifndef EVO_FEATURES_H
#define EVO_FEATURES_H

/*
 * Emby / media-server streaming.
 *
 * OFF since 0.10.0. The integration is not deleted - EmbyScreen,
 * EmbySetupScreen, the addon and addons/addon_emby.c are all still built and
 * still work - it is only unreachable from the UI while it is being worked on.
 * Turning this back to 1 restores the launch tile, the navigation rail section
 * and the rail icon; nothing else needs touching.
 *
 * Three places read it: the launch screen's library tiles, ScreenManager's
 * rail section table, and the RmlUi nav rail, which hides the icon so the
 * remaining sections do not sit next to a gap.
 */
#ifndef EVO_ENABLE_EMBY
#define EVO_ENABLE_EMBY 0
#endif

#endif /* EVO_FEATURES_H */
