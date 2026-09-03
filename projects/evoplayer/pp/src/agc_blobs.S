/*
 * agc_blobs.S - the prebuilt sceAgc shader / resource blobs, embedded.
 *
 * Vendored from blackbearreloaded/ProsperoLight assets/private/ (GPL-3.0,
 * compatible with EVO's GPL-3.0-or-later). There is no reachable PSSL->.sb
 * compiler; these are ProsperoLight's hardware-proven binaries, used verbatim.
 * SHA256s in pp/blobs/SHA256SUMS. See docs/evo-pro/agc-implementation.md §2.
 *
 * .incbin paths are relative to make's CWD (projects/evoplayer/).
 */
#define BLOB(sym, file) \
    .section .rodata            ; \
    .global sym##_start         ; \
sym##_start:                    ; \
    .incbin file                ; \
    .global sym##_end           ; \
sym##_end:                      ; \
    .text

BLOB(pp_agc_geometry_header, "pp/blobs/geometry.header.bin")
BLOB(pp_agc_geometry_code,   "pp/blobs/geometry.text.bin")
BLOB(pp_agc_pixel_header,    "pp/blobs/pixel.header.bin")
BLOB(pp_agc_pixel_code,      "pp/blobs/pixel.text.linear-buffer.bin")
BLOB(pp_agc_pixel_hdr_code,  "pp/blobs/pixel.text.p010-passthrough.bin")
BLOB(pp_agc_resources,       "pp/blobs/netflix-video-resources.bin")
