/* EVO Player - system_info
 *
 * Milestone 2. Confirms libkernel works and, critically, reports the console's
 * actual firmware version so you can prove you are on 12.70 rather than
 * assuming it.
 *
 * The SDK encodes firmware as a packed BCD-ish word: 12.70 is 0x12700000.
 * crt/kernel.c switches on (kernel_get_fw_version() & 0xffff0000) to pick the
 * kernel offset table, and lists `case 0x12700000:` among the 12.xx group.
 * If this prints something else, the offsets in use are not the ones for your
 * console and kernel-touching payloads will misbehave.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "evo_ps5.h"

/* The firmware word packs the version into the top bits: 0xMMmm0000 where MM
 * is the major byte and mm the minor byte, both read as hex-coded decimal. */
static void
format_fw_version(uint32_t raw, char *out, size_t outlen)
{
    unsigned major = (raw >> 24) & 0xff;
    unsigned minor = (raw >> 16) & 0xff;
    snprintf(out, outlen, "%x.%02x", major, minor);
}

int
main(void)
{
    char fwstr[32];
    uint32_t fw = kernel_get_fw_version();

    format_fw_version(fw, fwstr, sizeof fwstr);

    printf("=== EVO Player system_info ===\n");
    printf("firmware raw        : 0x%08x\n", fw);
    printf("firmware version    : %s\n", fwstr);
    printf("offset table group  : 0x%08x\n", fw & 0xffff0000u);

    /* -- CPU ------------------------------------------------------------- */
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    long pagesz = sysconf(_SC_PAGESIZE);
    printf("cpus online         : %ld\n", ncpu);
    printf("page size           : %ld bytes\n", pagesz);

    /* -- Memory ---------------------------------------------------------- */
    size_t dmem = sceKernelGetDirectMemorySize();
    printf("direct memory total : %zu bytes (%.2f GiB)\n",
           dmem, (double)dmem / (1024.0 * 1024.0 * 1024.0));

    off_t  availStart = 0;
    size_t availSize  = 0;
    if (sceKernelAvailableDirectMemorySize(0, (off_t)dmem, 0,
                                           &availStart, &availSize) == 0) {
        printf("largest free block  : %zu bytes (%.2f MiB) at 0x%lx\n",
               availSize, (double)availSize / (1024.0 * 1024.0),
               (unsigned long)availStart);
    } else {
        printf("largest free block  : query failed\n");
    }

    /* -- Process --------------------------------------------------------- */
    printf("pid                 : %d\n", (int)getpid());
    printf("process time        : %lu us\n",
           (unsigned long)sceKernelGetProcessTime());

    /* -- Build ----------------------------------------------------------- */
    printf("built with clang    : %d.%d.%d\n",
           __clang_major__, __clang_minor__, __clang_patchlevel__);
    printf("built on            : %s %s\n", __DATE__, __TIME__);
    fflush(stdout);

    /* Surface the important bit on screen - the console is usually the only
     * display you have while testing. */
    if ((fw & 0xffff0000u) == 0x12700000u) {
        evo_notify("EVO system_info: firmware %s (12.70 target OK), %ld CPUs",
                   fwstr, ncpu);
    } else {
        evo_notify("EVO system_info: firmware %s - NOT the 12.70 target!",
                   fwstr);
    }

    return EXIT_SUCCESS;
}
