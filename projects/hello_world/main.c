/* EVO Player - hello_world
 *
 * Milestone 1. This proves the entire chain works before anything harder:
 *
 *     Windows -> Docker -> clang-18 -> PS5 Payload SDK -> ELF -> PS5 12.70
 *
 * It deliberately depends on almost nothing: libc stdio and one libkernel
 * call. If this runs and exits cleanly, the toolchain, the ELF format, the
 * loader and the network path are all good.
 */

#include <stdio.h>
#include <stdlib.h>

#include "evo_ps5.h"

int
main(int argc, char **argv)
{
    /* A recognisable message, per the milestone-8 requirement. The build
     * stamp makes it obvious whether the console is running the payload you
     * just deployed or a stale one still resident from an earlier attempt. */
    evo_notify("EVO Player: hello from PS5 (built %s %s)", __DATE__, __TIME__);

    printf("EVO Player hello_world\n");
    printf("  argc = %d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("  argv[%d] = %s\n", i, argv[i]);
    }
    printf("  compiler = clang %d.%d.%d\n",
           __clang_major__, __clang_minor__, __clang_patchlevel__);
    fflush(stdout);

    /* Exit cleanly. The loader reports a non-zero status as a failure, so
     * returning 0 is part of what this test verifies. */
    return EXIT_SUCCESS;
}
