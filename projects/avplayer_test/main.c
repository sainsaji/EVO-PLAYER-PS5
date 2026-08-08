/* EVO Player - avplayer_test
 *
 * RESEARCH SCAFFOLD - placeholder, deliberately not implemented.
 *
 * Reserved for experiments with Sony's own media pipeline
 * (libSceAvPlayer / libSceAvPlayer.native / libSceAvPlayerStreaming), which
 * would give hardware demux + decode + A/V sync in one API if it turns out to
 * be reachable from a payload.
 *
 * It is kept as its own project, separate from prosperoplayer and from
 * decoder_test, precisely so that a dead end here costs nothing elsewhere.
 *
 * Before writing code here, run decoder_test on the console: if
 * libSceAvPlayer.sprx does not even map into the payload's process, there is
 * nothing to call and the stub-generation route (docs/proprietary.md) is the
 * only way forward.
 */

#include <stdio.h>
#include <stdlib.h>

#include "evo_ps5.h"

int
main(void)
{
    printf("=== EVO Player avplayer_test ===\n");
    printf("Not implemented yet - this is a reserved research slot.\n");
    printf("Run decoder_test first to find out whether libSceAvPlayer is\n");
    printf("reachable on this firmware, then build on top of that result.\n");

    evo_notify("EVO avplayer_test: placeholder (not implemented)");
    return EXIT_SUCCESS;
}
