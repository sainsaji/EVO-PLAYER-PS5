/*
 * pp_agc.c - see pp_agc.h. Ported from ProsperoLight native_agc_present.cpp.
 * Real body only under EVO_APP_MODULE.
 */
#include "pp_agc.h"
#include "evo_boot_log.h"

#ifdef EVO_APP_MODULE

#include <string.h>

/* --- sceAgc / libkernel: stub-linked (package-app.sh step 6b) ------------- */
extern int32_t  sceAgcInit(void *state, uint32_t defaults_revision);
extern int32_t  sceAgcCreateShader(void **shader, void *header, void *code);
extern int32_t  sceAgcLinkShaders(void *cx, void *uc, void *reserved,
                                  void *vertex_shader, void *pixel_shader,
                                  uint32_t primitive_type);

extern int64_t  sceKernelGetDirectMemorySize(void);
extern int32_t  sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t *);
extern int32_t  sceKernelMapDirectMemory(void **, size_t, int, int, int64_t, size_t);
extern int32_t  sceKernelMunmap(void *, size_t);
extern int32_t  sceKernelReleaseDirectMemory(int64_t, size_t);

/* --- embedded blobs (agc_blobs.S) --------------------------------------- */
extern const uint8_t pp_agc_geometry_header_start[], pp_agc_geometry_header_end[];
extern const uint8_t pp_agc_geometry_code_start[],   pp_agc_geometry_code_end[];
extern const uint8_t pp_agc_pixel_header_start[],    pp_agc_pixel_header_end[];
extern const uint8_t pp_agc_pixel_code_start[],      pp_agc_pixel_code_end[];
extern const uint8_t pp_agc_pixel_hdr_code_start[],  pp_agc_pixel_hdr_code_end[];
extern const uint8_t pp_agc_resources_start[],       pp_agc_resources_end[];

/* --- constants (ProsperoLight) ----------------------------------------- */
#define SHADER_MEMORY_BYTES  0x0d0000u
#define DIRECT_MEMORY_TYPE   12
#define MAP_PROTECTION       0x33
#define SHADER_ALIGN         0x4000u

/* shader_memory fixed layout (agc-implementation.md §3):
 *   0x0000 geometry header   0x1000 pixel header   0x2000 pixel code
 *   0x3700 geometry code     0x5000/0x6000 link scratch   0xc000 resources   */
#define OFF_GEO_HDR   0x0000u
#define OFF_PIX_HDR   0x1000u
#define OFF_PIX_CODE  0x2000u
#define OFF_GEO_CODE  0x3700u
#define OFF_LINK_A    0x5000u
#define OFF_LINK_B    0x6000u
#define OFF_RESOURCES 0xc000u

static struct {
    int       ready;
    int       tried;
    uint64_t  state;
    uint8_t  *mem;
    int64_t   mem_start;
    void     *vs;
    void     *ps;
    uint32_t  w, h;
} g_agc;

static int copy_asset(void *dst, size_t cap, const uint8_t *s, const uint8_t *e)
{
    if (e < s)
        return -1;
    size_t n = (size_t)(e - s);
    if (n > cap)
        return -1;
    memcpy(dst, s, n);
    return 0;
}

/* prepare_resources: verbatim from ProsperoLight - rebase the NGR1 descriptor
 * table's pointers to the mapped `resources` address, and (SDR) swap the
 * limited-range YUV coefficients for full-range. */
static int prepare_resources(uint8_t *resources, int hdr)
{
    uint32_t *header = (uint32_t *)resources;
    uint32_t table_offsets[2] = { header[1], header[3] };
    static const uint32_t table_counts[2] = { 2, 8 };
    uint32_t *limited_offset = (uint32_t *)(resources + 0x500);
    uint32_t *limited_scale  = (uint32_t *)(resources + 0x600);
    uint32_t *sample_scale   = (uint32_t *)(resources + 0x700);

    if (limited_offset[0] != 0x3d802008u || limited_offset[1] != 0x3d802008u ||
        limited_offset[2] != 0x3d802008u || limited_scale[0]  != 0x3f957abdu ||
        limited_scale[1]  != 0x3f922492u || limited_scale[2]  != 0x3f922492u ||
        sample_scale[0]   != 0x42801f88u)
        return -1;

    if (!hdr) {
        limited_offset[0] = limited_offset[1] = limited_offset[2] = 0x3d808081u;
        limited_scale[0] = 0x3f950a85u;
        limited_scale[1] = limited_scale[2] = 0x3f91b6dbu;
        sample_scale[0]  = 0x3f800000u;
    }

    for (uint32_t t = 0; t < 2; ++t) {
        uint32_t *entry = (uint32_t *)(resources + table_offsets[t]);
        for (uint32_t i = 0; i < table_counts[t]; ++i, entry += 4) {
            uintptr_t addr = (uintptr_t)resources + entry[0];
            entry[0] = (uint32_t)addr;
            entry[1] = (entry[1] & 0xffff0000u) | (uint32_t)(addr >> 32);
        }
    }
    return 0;
}

int pp_agc_init(uint32_t width, uint32_t height, int hdr)
{
    if (g_agc.ready)
        return 0;
    if (g_agc.tried)
        return -1;
    g_agc.tried = 1;
    g_agc.mem_start = -1;

    int32_t rc = sceAgcInit(&g_agc.state, 8);
    if (rc != 0) {
        evo_boot_log("pp_agc: sceAgcInit=0x%08x", (unsigned)rc);
        return -1;
    }

    int64_t limit = sceKernelGetDirectMemorySize();
    rc = sceKernelAllocateDirectMemory(0, limit, SHADER_MEMORY_BYTES, SHADER_ALIGN,
                                       DIRECT_MEMORY_TYPE, &g_agc.mem_start);
    if (rc == 0)
        rc = sceKernelMapDirectMemory((void **)&g_agc.mem, SHADER_MEMORY_BYTES,
                                      MAP_PROTECTION, 0, g_agc.mem_start, SHADER_ALIGN);
    if (rc != 0 || !g_agc.mem) {
        evo_boot_log("pp_agc: shader mem alloc/map=0x%08x", (unsigned)rc);
        pp_agc_shutdown();
        return -1;
    }
    memset(g_agc.mem, 0, SHADER_MEMORY_BYTES);

    const uint8_t *px_s = hdr ? pp_agc_pixel_hdr_code_start : pp_agc_pixel_code_start;
    const uint8_t *px_e = hdr ? pp_agc_pixel_hdr_code_end   : pp_agc_pixel_code_end;
    if (copy_asset(g_agc.mem + OFF_GEO_HDR,   0x1000, pp_agc_geometry_header_start, pp_agc_geometry_header_end) != 0 ||
        copy_asset(g_agc.mem + OFF_GEO_CODE,  0x1000, pp_agc_geometry_code_start,   pp_agc_geometry_code_end)   != 0 ||
        copy_asset(g_agc.mem + OFF_PIX_HDR,   0x1000, pp_agc_pixel_header_start,    pp_agc_pixel_header_end)     != 0 ||
        copy_asset(g_agc.mem + OFF_PIX_CODE,  0x1000, px_s, px_e)                                               != 0 ||
        copy_asset(g_agc.mem + OFF_RESOURCES, 0x1000, pp_agc_resources_start,       pp_agc_resources_end)       != 0 ||
        prepare_resources(g_agc.mem + OFF_RESOURCES, hdr) != 0) {
        evo_boot_log("pp_agc: blob copy / prepare_resources failed");
        pp_agc_shutdown();
        return -1;
    }

    rc = sceAgcCreateShader(&g_agc.vs, g_agc.mem + OFF_GEO_HDR, g_agc.mem + OFF_GEO_CODE);
    if (rc == 0)
        rc = sceAgcCreateShader(&g_agc.ps, g_agc.mem + OFF_PIX_HDR, g_agc.mem + OFF_PIX_CODE);
    int32_t link = -1;
    if (rc == 0)
        link = sceAgcLinkShaders(g_agc.mem + OFF_LINK_A, g_agc.mem + OFF_LINK_B, 0,
                                 g_agc.vs, g_agc.ps, 6 /* PrimitiveTriangleStrip */);

    evo_boot_log("pp_agc: create=0x%08x link=0x%08x vs=%p ps=%p  %ux%u hdr=%d",
                 (unsigned)rc, (unsigned)link, g_agc.vs, g_agc.ps, width, height, hdr);
    if (rc != 0 || link != 0) {
        pp_agc_shutdown();
        return -1;
    }

    g_agc.w = width;
    g_agc.h = height;
    /* render_frame not yet ported -> ready stays 0; pp_agc_available() 0,
     * player uses the CPU path. Flip to 1 when pp_agc_present_nv12 works. */
    evo_boot_log("pp_agc: shaders linked - present path still TODO (#27)");
    return 0;
}

int pp_agc_available(void)
{
    return g_agc.ready;
}

int pp_agc_present_nv12(const void *nv12, uint32_t pitch_bytes, uint32_t coded_height,
                        uint32_t vis_w, uint32_t vis_h, int64_t flip_marker)
{
    (void)nv12; (void)pitch_bytes; (void)coded_height;
    (void)vis_w; (void)vis_h; (void)flip_marker;
    return -1;   /* render_frame not ported yet */
}

void pp_agc_shutdown(void)
{
    if (g_agc.mem) {
        sceKernelMunmap(g_agc.mem, SHADER_MEMORY_BYTES);
        g_agc.mem = 0;
    }
    if (g_agc.mem_start >= 0) {
        sceKernelReleaseDirectMemory(g_agc.mem_start, SHADER_MEMORY_BYTES);
        g_agc.mem_start = -1;
    }
    g_agc.ready = 0;
    g_agc.vs = g_agc.ps = 0;
}

#else  /* host / payload */

int  pp_agc_init(uint32_t w, uint32_t h, int hdr) { (void)w; (void)h; (void)hdr; return -1; }
int  pp_agc_available(void) { return 0; }
int  pp_agc_present_nv12(const void *n, uint32_t p, uint32_t c, uint32_t vw, uint32_t vh, int64_t m)
{ (void)n; (void)p; (void)c; (void)vw; (void)vh; (void)m; return -1; }
void pp_agc_shutdown(void) {}

#endif /* EVO_APP_MODULE */
