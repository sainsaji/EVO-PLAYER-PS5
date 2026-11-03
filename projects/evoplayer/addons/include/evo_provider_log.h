/*
 * evo_provider_log.h — one diagnostic line, two destinations.
 *
 * The provider seam originally logged with fprintf(stderr, ...), which was
 * wrong twice over on the app module:
 *
 *   - stderr does not reach /mnt/usb0/evo.log. That file is written by
 *     evo_boot_log / evo_bt, so every provider diagnostic went nowhere and
 *     #90's "confirm the load lines in evo.log" was unsatisfiable.
 *   - nothing in the app module has ever written to stderr, and on the
 *     native-app CRT touching it dereferences null. The FFmpeg av_log fix
 *     landed earlier the same day for exactly this reason; the provider code
 *     walked straight back into it.
 *
 * So: evo.log on the device, plain stderr on the host, where the uiview
 * harness and the test runner both want to see it and stderr is a real stream.
 *
 * #216: runtime lines, so no "EVO boot:" prefix and no klog write (evo_bt was
 * both). PROV_WARN / PROV_ERR carry a severity the log viewer colours and
 * filters on: ERROR for a failure the user sees (server unreachable, login
 * refused, reply unreadable), WARN for one EVO works around.
 */
#ifndef EVO_PROVIDER_LOG_H
#define EVO_PROVIDER_LOG_H

#ifdef EVO_APP_MODULE

#include "evo_boot_log.h"
/* No fixed tag: provider lines start with their own ("net:", "iptv:",
 * "xtream:", "jellyfin:", ...), which the log viewer shows and filters as the
 * tag. Start a new line the same way. */
#define PROV_LOG(fmt, ...)  evo_log_level(0, fmt, ##__VA_ARGS__)
#define PROV_WARN(fmt, ...) evo_log_level(1, fmt, ##__VA_ARGS__)
#define PROV_ERR(fmt, ...)  evo_log_level(2, fmt, ##__VA_ARGS__)

#else /* host / payload */

#include <stdio.h>
#define PROV_LOG(fmt, ...)  fprintf(stderr, "[EVO provider] " fmt "\n", ##__VA_ARGS__)
#define PROV_WARN(fmt, ...) fprintf(stderr, "[EVO provider] WARN " fmt "\n", ##__VA_ARGS__)
#define PROV_ERR(fmt, ...)  fprintf(stderr, "[EVO provider] ERROR " fmt "\n", ##__VA_ARGS__)

#endif

#endif /* EVO_PROVIDER_LOG_H */
