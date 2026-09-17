#include "evo_rmlui_render_agc.h"

#include <cmath>
#include "evo_rmlui_bundle.h"
#include "evo_direct_mem.h"
#include "evo_boot_log.h"

#include <algorithm>
#include <cstring>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../../stb_image.h"
#pragma clang diagnostic pop

EvoRenderInterfaceAGC::EvoRenderInterfaceAGC(int width, int height)
    : m_width(width), m_height(height), m_scissor_enabled(false)
{
    SetDimensions(width, height);

    /* Create 1x1 opaque white texture for untextured rendering */
    const uint32_t white_pixel = 0xffffffff;
    Rml::TextureHandle white_handle = CreateTextureInternal(&white_pixel, 1, 1);
    m_white_texture.reset(reinterpret_cast<EvoAgcTexture *>(white_handle));
}

EvoRenderInterfaceAGC::~EvoRenderInterfaceAGC()
{
    if (m_fullscreen_quad) {
        ReleaseGeometry(m_fullscreen_quad);
        m_fullscreen_quad = 0;
    }
    for (auto *layer : m_layer_stack) {
        if (layer)
            evo_agc_layer_release(layer);
    }
    m_layer_stack.clear();
    m_filters.clear();
    for (auto &pair : m_mem_textures) {
        if (pair.second.handle) {
            ReleaseTexture(pair.second.handle);
            pair.second.handle = 0;
        }
    }
}

void EvoRenderInterfaceAGC::SetDimensions(int w, int h)
{
    m_width = w;
    m_height = h;
    if (!m_scissor_enabled) {
        m_scissor_region = Rml::Rectanglei::FromSize({w, h});
    }
    m_projection = Rml::Matrix4f::FromColumns(
        Rml::Vector4f(2.0f / (float)m_width, 0.0f, 0.0f, 0.0f),
        Rml::Vector4f(0.0f, -2.0f / (float)m_height, 0.0f, 0.0f),
        Rml::Vector4f(0.0f, 0.0f, 1.0f, 0.0f),
        Rml::Vector4f(-1.0f, 1.0f, 0.0f, 1.0f)
    );
    SetTransform(nullptr);
}

void EvoRenderInterfaceAGC::FrameBegin()
{
    evo_agc_runtime_frame_begin();
    evo_agc_runtime_bind_pipeline(EVO_AGC_PIPE_UI);
    evo_agc_runtime_set_blend(EVO_AGC_BLEND_PREMULTIPLIED);
}

void EvoRenderInterfaceAGC::FrameEnd()
{
    /* Handled by evo_agc_runtime_present() */
}

/* -------------------------------------------------------------------------
 * Geometry Compilation & Rendering
 * ------------------------------------------------------------------------- */

Rml::CompiledGeometryHandle EvoRenderInterfaceAGC::CompileGeometry(
    Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
{
    if (vertices.empty() || indices.empty())
        return 0;

    auto *geom = new EvoAgcCompiledGeometry();
    geom->vertex_count = (uint32_t)vertices.size();
    geom->vertex_bytes = (vertices.size() * sizeof(Rml::Vertex) + 255u) & ~255u;
    geom->vertex_data = evo_direct_mem_alloc(geom->vertex_bytes);

    if (!geom->vertex_data) {
        delete geom;
        return 0;
    }
    memcpy(geom->vertex_data, vertices.data(), vertices.size() * sizeof(Rml::Vertex));

    /* Bounding box, for the clip-mask approximation in RenderToClipMask(). */
    geom->bb_min_x = geom->bb_max_x = vertices[0].position.x;
    geom->bb_min_y = geom->bb_max_y = vertices[0].position.y;
    for (size_t i = 1; i < vertices.size(); ++i) {
        const Rml::Vector2f p = vertices[i].position;
        if (p.x < geom->bb_min_x) geom->bb_min_x = p.x;
        if (p.x > geom->bb_max_x) geom->bb_max_x = p.x;
        if (p.y < geom->bb_min_y) geom->bb_min_y = p.y;
        if (p.y > geom->bb_max_y) geom->bb_max_y = p.y;
    }

    /*
     * Recover the corner radii from the tessellation. RmlUi builds a rounded
     * box whose straight top edge runs from (min_x + r_tl) to (max_x - r_tr),
     * so the arc meets the edge EXACTLY on y == min_y and the extreme x there
     * is one radius in. A square box has a vertex at the corner and yields 0.
     *
     * The match on y has to be exact. Allowing half a pixel of slop picks up
     * the next vertex around the arc instead, and near a circle's tangent
     * point x moves as sqrt(2*r*dy) - so 0.5px of slop cost ~4px of radius
     * (a 19px corner measured as 16), leaving every corner under-rounded and
     * content bleeding out past it.
     */
    {
        const float eps = 0.01f;
        float top_min_x = geom->bb_max_x, top_max_x = geom->bb_min_x;
        float bot_min_x = geom->bb_max_x, bot_max_x = geom->bb_min_x;
        for (size_t i = 0; i < vertices.size(); ++i) {
            const Rml::Vector2f p = vertices[i].position;
            if (p.y - geom->bb_min_y < eps) {
                if (p.x < top_min_x) top_min_x = p.x;
                if (p.x > top_max_x) top_max_x = p.x;
            }
            if (geom->bb_max_y - p.y < eps) {
                if (p.x < bot_min_x) bot_min_x = p.x;
                if (p.x > bot_max_x) bot_max_x = p.x;
            }
        }
        const float r[4] = {
            top_min_x - geom->bb_min_x,   /* top-left     */
            geom->bb_max_x - top_max_x,   /* top-right    */
            geom->bb_max_x - bot_max_x,   /* bottom-right */
            bot_min_x - geom->bb_min_x,   /* bottom-left  */
        };
        for (int i = 0; i < 4; ++i)
            geom->bb_radius[i] = (r[i] > 0.0f) ? r[i] : 0.0f;
    }
    /* This vertex buffer is read by the GPU (via the V# descriptor built
     * below) on every draw from now on, but the CPU only ever writes it once,
     * right here - flush it once now rather than every frame it's used. */
    evo_agc_runtime_cache_flush(geom->vertex_data, geom->vertex_bytes);

    geom->index_count = (uint32_t)indices.size();
    geom->index_bytes = (indices.size() * sizeof(uint16_t) + 255u) & ~255u;
    geom->index_data = evo_direct_mem_alloc(geom->index_bytes);

    if (!geom->index_data) {
        evo_direct_mem_free(geom->vertex_data);
        delete geom;
        return 0;
    }

    /* Convert 32-bit indices to 16-bit */
    auto *dst_indices = reinterpret_cast<uint16_t *>(geom->index_data);
    for (size_t i = 0; i < indices.size(); ++i) {
        dst_indices[i] = static_cast<uint16_t>(indices[i]);
    }
    /* Same one-time-write/many-reads case as vertex_data above - the GPU reads
     * this directly as the index buffer on every draw of this geometry. */
    evo_agc_runtime_cache_flush(geom->index_data, geom->index_bytes);

    /* Build GFX10.3 hardware vertex buffer descriptor (V#) */
    evo_agc_build_vsharp(geom->vsharp, (uint64_t)(uintptr_t)geom->vertex_data,
                         sizeof(Rml::Vertex), geom->vertex_count);

    return reinterpret_cast<Rml::CompiledGeometryHandle>(geom);
}

void EvoRenderInterfaceAGC::ReleaseGeometry(Rml::CompiledGeometryHandle geometry)
{
    auto *geom = reinterpret_cast<EvoAgcCompiledGeometry *>(geometry);
    if (!geom)
        return;

    if (geom->vertex_data) {
        evo_direct_mem_free(geom->vertex_data);
        geom->vertex_data = nullptr;
    }
    if (geom->index_data) {
        evo_direct_mem_free(geom->index_data);
        geom->index_data = nullptr;
    }
    delete geom;
}

void EvoRenderInterfaceAGC::RenderGeometry(Rml::CompiledGeometryHandle geometry,
                                          Rml::Vector2f translation,
                                          Rml::TextureHandle texture)
{
    auto *geom = reinterpret_cast<EvoAgcCompiledGeometry *>(geometry);
    if (!geom || geom->index_count == 0)
        return;

    SceAgcCommandBuffer *cb = evo_agc_runtime_get_current_cb();
    if (!cb)
        return;

    /* A prior pass (backdrop blur, video) may have bound its own pipeline;
     * geometry always renders through the UI pipe. No-op when already bound. */
    evo_agc_runtime_bind_pipeline(EVO_AGC_PIPE_UI);

    evo_agc_transient_ring_t *ring = evo_agc_runtime_get_transient_ring();
    uint32_t slot = evo_agc_runtime_get_current_slot();

    /* 1. Allocate ScreenConstants slice (80 bytes) in the transient ring */
    evo_agc_transient_slice_t const_slice;
    if (evo_agc_transient_ring_alloc(ring, slot, 128, 16, &const_slice) != EVO_AGC_TRANSIENT_OK) {
        evo_agc_runtime_note_drop(0);
        return;
    }

    /* Orthographic projection matrix: maps (0..width, 0..height) to (-1..1, 1..-1) */
    float *constants = reinterpret_cast<float *>(const_slice.cpu);
    memset(constants, 0, 128);

    /* mat4 projection / transform (column-major) */
    memcpy(constants, m_transform.data(), 16 * sizeof(float));

    /* vec4 translation */
    constants[16] = translation.x;
    constants[17] = translation.y;
    constants[18] = 0.0f;
    constants[19] = 0.0f;

    /* vec4 clip_rect + vec4 clip_params - the rounded-box clip the fragment
     * shader applies. Disabled leaves clip_params.y at zero from the memset. */
    if (m_clip_mask_enabled && m_clip_mask_valid) {
        constants[20] = m_clip_rect[0];
        constants[21] = m_clip_rect[1];
        constants[22] = m_clip_rect[2];
        constants[23] = m_clip_rect[3];
        constants[24] = 0.0f;
        constants[25] = 1.0f;
        constants[28] = m_clip_radius[0];
        constants[29] = m_clip_radius[1];
        constants[30] = m_clip_radius[2];
        constants[31] = m_clip_radius[3];
    }

    /* 2. Build constant V# descriptor in transient ring (16 bytes) */
    evo_agc_transient_slice_t desc_slice;
    if (evo_agc_transient_ring_alloc(ring, slot, 16, 16, &desc_slice) != EVO_AGC_TRANSIENT_OK) {
        evo_agc_runtime_note_drop(0);
        return;
    }

    evo_agc_build_constant_vsharp(reinterpret_cast<uint32_t *>(desc_slice.cpu),
                                  const_slice.gpu_addr, 128);

    /* 2b. Stage V# descriptor in transient ring (16 bytes) */
    evo_agc_transient_slice_t vsharp_slice;
    if (evo_agc_transient_ring_alloc(ring, slot, sizeof(geom->vsharp), 16, &vsharp_slice) != EVO_AGC_TRANSIENT_OK) {
        evo_agc_runtime_note_drop(0);
        return;
    }
    memcpy(vsharp_slice.cpu, geom->vsharp, sizeof(geom->vsharp));

    /* 3. Emit the user-SGPR blocks.
     *
     * Slot indices come from the compiled pipeline's PAL metadata, never from a
     * constant here. The old psbc path hardcoded "vertex table at dword 0, base
     * vertex at 1, constants at 2"; LLPC's [ResourceMapping] lays them out
     * differently, and writing a pointer to the wrong dword is invisible - the
     * shader reads its projection or its texture through an unset pointer,
     * draws nothing, and faults nothing. */
    const evo_agc_user_data_layout_t ud =
        evo_agc_runtime_get_user_data_layout(EVO_AGC_PIPE_UI);
    if (!ud.vs_count || ud.vs_const_table_dword < 0 ||
        ud.vs_vertex_table_dword < 0)
        return;

    EvoAgcTexture *tex = texture ? reinterpret_cast<EvoAgcTexture *>(texture)
                                 : m_white_texture.get();
    evo_agc_transient_slice_t tex_slice;
    if (evo_agc_transient_ring_alloc(ring, slot, sizeof(tex->descriptor), 16,
                                     &tex_slice) != EVO_AGC_TRANSIENT_OK) {
        evo_agc_runtime_note_drop(0);
        return;
    }
    memcpy(tex_slice.cpu, tex->descriptor, sizeof(tex->descriptor));

    uint32_t vs_user_data[16] = {0};
    if (ud.vs_count > 16)
        return;
    vs_user_data[ud.vs_const_table_dword]  = (uint32_t)desc_slice.gpu_addr;
    vs_user_data[ud.vs_vertex_table_dword] = (uint32_t)vsharp_slice.gpu_addr;
    evo_agc_writer_set_user_data_gs(cb, vs_user_data, ud.vs_count);

    if (ud.ps_count && ud.ps_texture_table_dword >= 0 && ud.ps_count <= 16) {
        uint32_t ps_user_data[16] = {0};
        ps_user_data[ud.ps_texture_table_dword] = (uint32_t)tex_slice.gpu_addr;
        evo_agc_writer_set_user_data_ps(cb, ps_user_data, ud.ps_count);
    }

    /* 5. Dispatch hardware draw call */
    evo_agc_writer_draw_index(cb, geom->index_count,
                              reinterpret_cast<const uint16_t *>(geom->index_data));
    /* This frame now has content, so it is worth presenting. */
    evo_agc_runtime_note_draw();
}

/* -------------------------------------------------------------------------
 * Scissoring & Transforms
 * ------------------------------------------------------------------------- */

/*
 * The hardware has one scissor rectangle but two things want to clip: RmlUi's
 * own scissor region, and the bounding-box stand-in for a clip mask. Both go
 * through here so neither can clobber the other - setting a scissor while a
 * mask is up used to drop the mask, which is exactly when a scrolling rounded
 * container needs both.
 */
void EvoRenderInterfaceAGC::ApplyScissorState()
{
    int left = 0, top = 0, right = m_width, bottom = m_height;

    if (m_scissor_enabled) {
        left   = m_scissor_region.Left();
        top    = m_scissor_region.Top();
        right  = m_scissor_region.Right();
        bottom = m_scissor_region.Bottom();
    }
    if (m_clip_mask_enabled && m_clip_mask_valid) {
        if (m_clip_mask_box.Left()   > left)   left   = m_clip_mask_box.Left();
        if (m_clip_mask_box.Top()    > top)    top    = m_clip_mask_box.Top();
        if (m_clip_mask_box.Right()  < right)  right  = m_clip_mask_box.Right();
        if (m_clip_mask_box.Bottom() < bottom) bottom = m_clip_mask_box.Bottom();
    }

    evo_agc_runtime_set_scissor(left, top, right - left, bottom - top);
}

void EvoRenderInterfaceAGC::EnableScissorRegion(bool enable)
{
    m_scissor_enabled = enable;
    ApplyScissorState();
}

void EvoRenderInterfaceAGC::SetScissorRegion(Rml::Rectanglei region)
{
    m_scissor_region = region;
    ApplyScissorState();
}

void EvoRenderInterfaceAGC::SetTransform(const Rml::Matrix4f *transform)
{
    if (transform) {
        m_transform = m_projection * (*transform);
        m_transform_active = true;
    } else {
        m_transform = m_projection;
        m_transform_active = false;
    }
}

void EvoRenderInterfaceAGC::EnableClipMask(bool enable)
{
    m_clip_mask_enabled = enable;
    if (!enable)
        m_clip_mask_valid = false;
    ApplyScissorState();
    evo_agc_runtime_set_clip_mask(enable ? 1 : 0);
}

void EvoRenderInterfaceAGC::RenderToClipMask(Rml::ClipMaskOperation operation,
                                             Rml::CompiledGeometryHandle geometry,
                                             Rml::Vector2f translation)
{
    const auto *geom = reinterpret_cast<const EvoAgcCompiledGeometry *>(geometry);
    if (!geom)
        return;

    /*
     * Bounding-box path (see m_clip_mask_box). Round outwards, so the box is
     * never smaller than the shape it stands in for - rounding inwards would
     * shave a pixel off every clipped edge in the UI.
     */
    Rml::Rectanglei box = Rml::Rectanglei::FromCorners(
        Rml::Vector2i((int)std::floor(geom->bb_min_x + translation.x),
                      (int)std::floor(geom->bb_min_y + translation.y)),
        Rml::Vector2i((int)std::ceil(geom->bb_max_x + translation.x),
                      (int)std::ceil(geom->bb_max_y + translation.y)));

    const float fx0 = geom->bb_min_x + translation.x;
    const float fy0 = geom->bb_min_y + translation.y;
    const float fx1 = geom->bb_max_x + translation.x;
    const float fy1 = geom->bb_max_y + translation.y;

    switch (operation) {
    case Rml::ClipMaskOperation::Set:
        m_clip_mask_box = box;
        m_clip_mask_valid = true;
        m_clip_rect[0] = fx0; m_clip_rect[1] = fy0;
        m_clip_rect[2] = fx1; m_clip_rect[3] = fy1;
        for (int i = 0; i < 4; ++i) m_clip_radius[i] = geom->bb_radius[i];
        break;
    case Rml::ClipMaskOperation::Intersect:
        if (m_clip_mask_valid) {
            m_clip_mask_box = m_clip_mask_box.IntersectIfValid(box);
            /* Two rounded rects intersected is not a rounded rect. Keep the
             * tighter extent and the larger radius: that is the intersection
             * wherever the two boxes share an edge, which is the nesting RmlUi
             * actually produces (a rounded card inside a rounded panel). */
            if (fx0 > m_clip_rect[0]) m_clip_rect[0] = fx0;
            if (fy0 > m_clip_rect[1]) m_clip_rect[1] = fy0;
            if (fx1 < m_clip_rect[2]) m_clip_rect[2] = fx1;
            if (fy1 < m_clip_rect[3]) m_clip_rect[3] = fy1;
            for (int i = 0; i < 4; ++i)
                if (geom->bb_radius[i] > m_clip_radius[i])
                    m_clip_radius[i] = geom->bb_radius[i];
        } else {
            m_clip_mask_box = box;
            m_clip_mask_valid = true;
            m_clip_rect[0] = fx0; m_clip_rect[1] = fy0;
            m_clip_rect[2] = fx1; m_clip_rect[3] = fy1;
            for (int i = 0; i < 4; ++i) m_clip_radius[i] = geom->bb_radius[i];
        }
        break;
    case Rml::ClipMaskOperation::SetInverse:
        /* "Everything outside this shape" is not a rectangle, so a scissor
         * cannot express it. Clipping to the box would hide exactly the wrong
         * half; clip to nothing and let the content draw unclipped. */
        m_clip_mask_valid = false;
        break;
    }
    ApplyScissorState();

    /*
     * The exact shape, when the stencil path is compiled in. Same shape as
     * RmlUi's own GL3 backend: switch the stencil to write mode with colour
     * writes off, draw the mask geometry through the normal path, then switch
     * back to testing against it.
     *
     * With the stencil compiled out, this geometry must NOT be drawn - it
     * describes a clipping shape, not something visible, and rendering it
     * normally covers the UI in opaque quads.
     */
    if (!evo_agc_runtime_clip_mask_supported())
        return;

    int op = 2; /* Intersect */
    if (operation == Rml::ClipMaskOperation::Set)
        op = 0;
    else if (operation == Rml::ClipMaskOperation::SetInverse)
        op = 1;

    evo_agc_runtime_clip_mask_begin(op);
    RenderGeometry(geometry, translation, 0);
    evo_agc_runtime_clip_mask_end();
}

/* -------------------------------------------------------------------------
 * Texture Management
 * ------------------------------------------------------------------------- */

Rml::TextureHandle EvoRenderInterfaceAGC::CreateTextureInternal(const uint32_t *rgba,
                                                               int width, int height)
{
    if (!rgba || width <= 0 || height <= 0)
        return 0;

    auto *tex = new EvoAgcTexture();
    tex->width = width;
    tex->height = height;

    /* GFX10.3 pitch alignment to 256 bytes */
    const uint32_t row_bytes = (uint32_t)width * 4u;
    tex->pitch = (row_bytes + 255u) & ~255u;
    tex->alloc_bytes = (size_t)height * (size_t)tex->pitch;

    /*
     * A GFX10 image descriptor stores the base as gpu_address >> 8, so a
     * texture MUST start on a 256-byte boundary or the low bits are simply
     * dropped and the GPU samples from up to 255 bytes before the real data.
     * evo_direct_mem_alloc only guarantees 64, so three allocations in four
     * landed misaligned - which is why some glyph atlases drew perfectly and
     * others came out scrambled. Over-allocate and align by hand, keeping the
     * raw pointer for the free.
     */
    tex->alloc_base = evo_direct_mem_alloc(tex->alloc_bytes + 255u);
    if (!tex->alloc_base) {
        evo_agc_runtime_note_drop(1);
        delete tex;
        return 0;
    }
    tex->pixels = (void *)(((uintptr_t)tex->alloc_base + 255u) & ~(uintptr_t)255);

    /* Copy row by row into pitched GPU layout */
    auto *dst = reinterpret_cast<uint8_t *>(tex->pixels);
    for (int y = 0; y < height; ++y) {
        memcpy(dst + (size_t)y * tex->pitch, rgba + (size_t)y * width, row_bytes);
    }
    /* One-time write, read by the GPU on every draw that uses this texture
     * from now on - flush it now, not per-frame. */
    evo_agc_runtime_cache_flush(tex->pixels, tex->alloc_bytes);

    if (evo_agc_build_tsharp_rgba8(tex->descriptor,
                                   (uint64_t)(uintptr_t)tex->pixels,
                                   (uint32_t)width, (uint32_t)height,
                                   (uint32_t)tex->pitch) != 0) {
        evo_boot_log("agc texture %dx%d pitch=%d addr=%p REJECTED by tsharp builder",
                     width, height, tex->pitch, tex->pixels);
        evo_agc_runtime_note_drop(1);
        evo_direct_mem_free(tex->alloc_base);
        delete tex;
        return 0;
    }

    evo_agc_build_ssharp(tex->descriptor + 8, 1 /* clamp */, 1 /* bilinear */);

    return reinterpret_cast<Rml::TextureHandle>(tex);
}

Rml::TextureHandle EvoRenderInterfaceAGC::GenerateTexture(Rml::Span<const Rml::byte> source,
                                                         Rml::Vector2i source_dimensions)
{
    return CreateTextureInternal(reinterpret_cast<const uint32_t *>(source.data()),
                                 source_dimensions.x, source_dimensions.y);
}

Rml::TextureHandle EvoRenderInterfaceAGC::LoadTexture(Rml::Vector2i &texture_dimensions,
                                                     const Rml::String &source)
{
    auto it = m_mem_textures.find(source);
    if (it != m_mem_textures.end()) {
        texture_dimensions = Rml::Vector2i(it->second.width, it->second.height);
        if (it->second.handle)
            return it->second.handle;
        it->second.handle = CreateTextureInternal(it->second.pixels.data(),
                                                  it->second.width, it->second.height);
        return it->second.handle;
    }

    std::string filename = source;
    size_t last_slash = source.find_last_of("/\\");
    if (last_slash != std::string::npos)
        filename = source.substr(last_slash + 1);

    int width = 0, height = 0, channels = 0;
    unsigned char *data = nullptr;

    const EvoRmlBundleFile *bundled = evo_rmlui_bundle_find(source.c_str());
    if (!bundled) bundled = evo_rmlui_bundle_find(("icons/" + filename).c_str());
    if (bundled) {
        data = stbi_load_from_memory(bundled->data, (int)bundled->size,
                                     &width, &height, &channels, 4);
    }

    if (!data) {
        std::vector<std::string> candidates = {
            source,
            "assets/icons/" + filename,
            "assets/rml/icons/" + filename,
            "assets/rml/" + source,
            "assets/" + source,
            "/app0/assets/icons/" + filename,
            "/app0/assets/rml/icons/" + filename,
            "/app0/assets/rml/" + source,
            "/app0/assets/" + source,
            "projects/evoplayer/assets/icons/" + filename,
            "projects/evoplayer/assets/rml/icons/" + filename,
            "projects/evoplayer/assets/rml/" + source,
            "projects/evoplayer/assets/" + source,
            "/workspace/projects/evoplayer/assets/icons/" + filename,
            "/workspace/projects/evoplayer/assets/rml/icons/" + filename,
            "/workspace/projects/evoplayer/assets/rml/" + source,
            "/workspace/projects/evoplayer/assets/" + source,
            "/mnt/usb0/assets/icons/" + filename,
            "/mnt/usb0/assets/rml/icons/" + filename
        };
        for (const auto &path : candidates) {
            data = stbi_load(path.c_str(), &width, &height, &channels, 4);
            if (data) break;
        }
    }

    if (!data) {
        texture_dimensions = Rml::Vector2i(0, 0);
        return 0;
    }

    /* Premultiply alpha for UI blending */
    std::vector<uint32_t> rgba((size_t)width * (size_t)height);
    for (int i = 0; i < width * height; ++i) {
        uint32_t r = data[i * 4 + 0];
        uint32_t g = data[i * 4 + 1];
        uint32_t b = data[i * 4 + 2];
        uint32_t a = data[i * 4 + 3];
        if (a < 255) {
            r = (r * a + 127) / 255;
            g = (g * a + 127) / 255;
            b = (b * a + 127) / 255;
        }
        rgba[i] = (a << 24) | (b << 16) | (g << 8) | r;
    }
    stbi_image_free(data);

    texture_dimensions = Rml::Vector2i(width, height);
    return CreateTextureInternal(rgba.data(), width, height);
}

void EvoRenderInterfaceAGC::ReleaseTexture(Rml::TextureHandle texture)
{
    auto *tex = reinterpret_cast<EvoAgcTexture *>(texture);
    if (!tex)
        return;

    if (tex->pixels) {
        evo_direct_mem_free(tex->alloc_base);
        tex->pixels = nullptr;
    }
    delete tex;
}

void EvoRenderInterfaceAGC::SetMemoryTexture(const std::string &key,
                                            const uint32_t *bgra, int w, int h)
{
    if (!bgra || w <= 0 || h <= 0)
        return;

    auto &entry = m_mem_textures[key];
    if (entry.handle) {
        ReleaseTexture(entry.handle);
        entry.handle = 0;
    }
    entry.width = w;
    entry.height = h;
    entry.pixels.resize((size_t)w * (size_t)h);

    /* Convert BGRA (0xAABBGGRR) to premultiplied RGBA */
    for (int i = 0; i < w * h; ++i) {
        uint32_t c = bgra[i];
        uint32_t a = (c >> 24) & 0xff;
        uint32_t b = (c >> 16) & 0xff;
        uint32_t g = (c >> 8)  & 0xff;
        uint32_t r =  c        & 0xff;
        if (a < 255) {
            r = (r * a + 127) / 255;
            g = (g * a + 127) / 255;
            b = (b * a + 127) / 255;
        }
        entry.pixels[i] = (a << 24) | (b << 16) | (g << 8) | r;
    }

    entry.handle = CreateTextureInternal(entry.pixels.data(), w, h);
}

void EvoRenderInterfaceAGC::DropMemoryTexture(const std::string &key)
{
    auto it = m_mem_textures.find(key);
    if (it != m_mem_textures.end()) {
        if (it->second.handle) {
            ReleaseTexture(it->second.handle);
        }
        m_mem_textures.erase(it);
    }
}

/* -------------------------------------------------------------------------
 * Layers & backdrop-filter: blur
 *
 * RmlUi's backdrop-filter: blur() is two CompositeLayers stages on top of a
 * pushed temp layer (ElementEffects.cpp):
 *
 *   1. CompositeLayers(backdrop_source, temp_layer, Blend, { blur_handle })
 *      - H and V separable Gaussian passes, source -> temp_layer.
 *   2. CompositeLayers(temp_layer, destination, Blend, {})
 *      - plain fullscreen copy, scissored + SDF-clipped to the element box.
 *
 * The blur passes run through the ui_backdrop_blur pipe, whose vertex shader
 * builds the fullscreen quad from gl_VertexIndex (BLUR_DRAW_MODIFIER), reads
 * BlurConstants from a PS-bound descriptor table and the backdrop source from
 * a PS-bound combined T#/S#. The copy reuses the UI pipe and RenderGeometry.
 * A floating source layer (scanout backbuffer = COMP_SWAP=ALT) is sampled
 * with the bgra8 T# swizzle; layer surfaces use standard RGBA so the rgba8
 * swizzle. Getting this backwards is a silent red/blue swap.
 * ------------------------------------------------------------------------- */

/* First-N entry breadcrumb. The frame-1 SIGSEGV landed somewhere in this
 * section with nothing logged, because every existing log sits AFTER the work
 * it describes - PushLayer's, in particular, is after a 33 MB surface clear. */
#define EVO_BD_TRACE(n, ...) do { static int s_bd_left = (n); if (s_bd_left > 0) { s_bd_left--; evo_log(__VA_ARGS__); evo_log_flush(); } } while (0)

Rml::LayerHandle EvoRenderInterfaceAGC::PushLayer()
{
    EVO_BD_TRACE(4, "backdrop: PushLayer enter has_layers=%d stack=%zu",
                 evo_agc_has_layers(), m_layer_stack.size());

    evo_agc_layer_surface_t *layer = nullptr;
    EVO_BD_TRACE(4, "backdrop: PushLayer -> acquire");
    const int acq = evo_agc_has_layers() ? evo_agc_layer_acquire(&layer) : -1;
    EVO_BD_TRACE(4, "backdrop: PushLayer acquire rc=%d layer=%p", acq, (void *)layer);
    if (acq == 0 && layer) {
        m_layer_stack.push_back(layer);
        EVO_BD_TRACE(4, "backdrop: PushLayer -> set_layer_target %ux%u pitch=%u base=%p",
                     layer->width, layer->height, layer->pitch_bytes,
                     (void *)layer->cpu_base);
        evo_agc_set_layer_target(layer);
        EVO_BD_TRACE(4, "backdrop: PushLayer set_layer_target done");
        static bool logged_ok = false;
        if (!logged_ok) {
            logged_ok = true;
            evo_log("backdrop: PushLayer acquired %p", (void *)layer);
            evo_log_flush();
        }
        return (Rml::LayerHandle)(uintptr_t)layer;
    }

    /* Pool exhausted: keep the stack balanced with a null marker so PopLayer
     * still restores the target, but hand RmlUi the base layer handle - the
     * UI draws, the backdrop blur just does not appear. */
    m_layer_stack.push_back(nullptr);
    static bool logged_degrade = false;
    if (!logged_degrade) {
        logged_degrade = true;
        evo_log("backdrop: PushLayer degraded (layer pool exhausted) -> base layer");
        evo_log_flush();
    }
    return 0;
}

void EvoRenderInterfaceAGC::PopLayer()
{
    EVO_BD_TRACE(4, "backdrop: PopLayer enter stack=%zu", m_layer_stack.size());

    if (m_layer_stack.empty())
        return;
    evo_agc_layer_surface_t *layer = m_layer_stack.back();
    m_layer_stack.pop_back();
    if (layer)
        evo_agc_layer_release(layer);
    /* Restore the render target to the new stack top (base layer when empty),
     * exactly as RmlUi expects after popping its pushed layer. */
    evo_agc_set_layer_target(m_layer_stack.empty() ? nullptr : m_layer_stack.back());
}

evo_agc_layer_surface_t *EvoRenderInterfaceAGC::ResolveLayer(Rml::LayerHandle handle) const
{
    if (!handle)
        return nullptr;
    for (auto *layer : m_layer_stack) {
        if (layer && (Rml::LayerHandle)(uintptr_t)layer == handle)
            return layer;
    }
    return nullptr;
}

Rml::CompiledFilterHandle EvoRenderInterfaceAGC::CompileFilter(const Rml::String &name,
                                                               const Rml::Dictionary &parameters)
{
    EVO_BD_TRACE(4, "backdrop: CompileFilter enter name=%s", name.c_str());

    if (name != "blur")
        return 0;
    const float sigma = Rml::Get(parameters, "sigma", 0.0f);
    if (sigma <= 0.0f)
        return 0;

    auto filter = std::make_unique<BlurFilter>();
    filter->sigma = sigma;
    const Rml::CompiledFilterHandle handle =
        (Rml::CompiledFilterHandle)(uintptr_t)filter.get();

    static bool logged = false;
    if (!logged) {
        logged = true;
        evo_log("backdrop: CompileFilter blur sigma=%.2f -> handle=%p", sigma, (void *)handle);
        evo_log_flush();
    }

    m_filters.push_back(std::move(filter));
    return handle;
}

void EvoRenderInterfaceAGC::ReleaseFilter(Rml::CompiledFilterHandle filter)
{
    EVO_BD_TRACE(4, "backdrop: ReleaseFilter enter h=%p", (void *)filter);

    if (!filter)
        return;
    for (auto it = m_filters.begin(); it != m_filters.end(); ++it) {
        if ((Rml::CompiledFilterHandle)(uintptr_t)it->get() == filter) {
            m_filters.erase(it);
            return;
        }
    }
}

/* Fullscreen quad spanning 0..width x 0..height in document pixels, built
 * once per render size. Its index stream {0,1,2,2,1,3} doubles as the blur
 * draw's index buffer (the blur VS reads gl_VertexIndex from it). */
void EvoRenderInterfaceAGC::EnsureFullscreenQuad()
{
    if (m_fullscreen_quad && m_fsquad_width == m_width && m_fsquad_height == m_height)
        return;

    if (m_fullscreen_quad) {
        ReleaseGeometry(m_fullscreen_quad);
        m_fullscreen_quad = 0;
    }

    const float w = (float)m_width;
    const float h = (float)m_height;
    const Rml::Vertex verts[4] = {
        {Rml::Vector2f(0, 0), Rml::ColourbPremultiplied(255, 255, 255, 255), Rml::Vector2f(0, 0)},
        {Rml::Vector2f(w, 0), Rml::ColourbPremultiplied(255, 255, 255, 255), Rml::Vector2f(1, 0)},
        {Rml::Vector2f(0, h), Rml::ColourbPremultiplied(255, 255, 255, 255), Rml::Vector2f(0, 1)},
        {Rml::Vector2f(w, h), Rml::ColourbPremultiplied(255, 255, 255, 255), Rml::Vector2f(1, 1)},
    };
    const int idx[6] = {0, 1, 2, 2, 1, 3};

    m_fullscreen_quad = CompileGeometry(Rml::Span<const Rml::Vertex>(verts, 4),
                                        Rml::Span<const int>(idx, 6));
    if (m_fullscreen_quad) {
        m_fsquad_width = m_width;
        m_fsquad_height = m_height;
    }
}

bool EvoRenderInterfaceAGC::DrawBlurPass(const evo_agc_layer_surface_t &src,
                                         bool src_is_base,
                                         float sigma, bool horizontal)
{
    SceAgcCommandBuffer *cb = evo_agc_runtime_get_current_cb();
    evo_agc_transient_ring_t *ring = evo_agc_runtime_get_transient_ring();
    uint32_t slot = evo_agc_runtime_get_current_slot();
    if (!cb || !ring)
        return false;

    const evo_agc_user_data_layout_t ud =
        evo_agc_runtime_get_user_data_layout(EVO_AGC_PIPE_UI_BLUR);
    if (ud.vs_count == 0 || ud.ps_count == 0 || ud.ps_count > 16 ||
        ud.ps_const_table_dword < 0 || ud.ps_texture_table_dword < 0)
        return false;

    const auto *quad = reinterpret_cast<const EvoAgcCompiledGeometry *>(m_fullscreen_quad);
    if (!quad || !quad->index_data || quad->index_count == 0)
        return false;

    /*
     * Taps and weights. RmlUi's FilterBlur extends the ink area by
     * 3*max(sigma,1), so we cover the same extent; the outermost tap lands at
     * 3*sigma. The weights are a truncated Gaussian (centre + symmetric
     * pairs), normalised so the output has unit sum.
     */
    const int taps = std::max(1, std::min(15, (int)std::ceil(3.0f * std::max(sigma, 1.0f))));
    const float radius = 3.0f * sigma;
    const float sigma2 = 2.0f * sigma * sigma;
    const float src_w = (float)src.width;
    const float src_h = (float)src.height;

    /* BlurConstants = uParams + uOffsets[16] + uWeights[16] = 33 vec4 = 528 B */
    evo_agc_transient_slice_t const_slice;
    if (evo_agc_transient_ring_alloc(ring, slot, 33u * 16u, 16, &const_slice) != EVO_AGC_TRANSIENT_OK) {
        evo_agc_runtime_note_drop(0);
        return false;
    }
    float *c = (float *)const_slice.cpu;
    memset(c, 0, 33u * 16u);
    c[0] = src_w > 0.0f ? 1.0f / src_w : 0.0f;   /* uParams.x */
    c[1] = src_h > 0.0f ? 1.0f / src_h : 0.0f;   /* uParams.y */
    c[2] = (float)taps;                           /* uParams.z */

    float weight_total = 1.0f; /* centre tap */
    float weights[16] = {0.0f};
    for (int i = 1; i <= taps; ++i) {
        const float d = radius * (float)i / (float)taps;
        weights[i] = expf(-(d * d) / sigma2);
        weight_total += 2.0f * weights[i];
    }

    c[68] = 1.0f / weight_total; /* uWeights[0].x (centre) */
    for (int i = 1; i <= taps; ++i) {
        const float d = radius * (float)i / (float)taps;
        const float w = weights[i] / weight_total;
        const float off = horizontal ? d / src_w : d / src_h;
        /* uOffsets[i].xy - direction baked in per pass */
        c[4 + 4 * i + 0] = horizontal ? off : 0.0f;
        c[4 + 4 * i + 1] = horizontal ? 0.0f : off;
        /* uWeights[i].x */
        c[68 + 4 * i + 0] = w;
    }

    /* Constant V# for the BlurConstants block (PS user data table slot). */
    evo_agc_transient_slice_t const_desc;
    if (evo_agc_transient_ring_alloc(ring, slot, 16u, 16, &const_desc) != EVO_AGC_TRANSIENT_OK) {
        evo_agc_runtime_note_drop(0);
        return false;
    }
    evo_agc_build_constant_vsharp((uint32_t *)const_desc.cpu, const_slice.gpu_addr, 33u * 16u);

    /* Combined T# + S# for the source texture (48 bytes). */
    evo_agc_transient_slice_t tex_desc;
    if (evo_agc_transient_ring_alloc(ring, slot, EVO_AGC_COMBINED_DESCRIPTOR_DWORDS * 4u, 16,
                                     &tex_desc) != EVO_AGC_TRANSIENT_OK) {
        evo_agc_runtime_note_drop(0);
        return false;
    }
    uint32_t *tdesc = (uint32_t *)tex_desc.cpu;
    memset(tdesc, 0, EVO_AGC_COMBINED_DESCRIPTOR_DWORDS * 4u);
    if (src_is_base)
        evo_agc_build_tsharp_bgra8(tdesc, src.gpu_addr, src.width, src.height, src.pitch_bytes);
    else
        evo_agc_build_tsharp_rgba8(tdesc, src.gpu_addr, src.width, src.height, src.pitch_bytes);
    evo_agc_build_ssharp(tdesc + EVO_AGC_TSHARP_DWORDS, 1, 1);

    /* User-SGPR blocks, from the compiled pipeline's PAL metadata. The blur VS
     * reads only gl_VertexIndex (no const/vertex tables), so its block is the
     * global-table dword alone; the PS block carries the constant V# and the
     * combined descriptor. */
    uint32_t vs_user[16] = {0};
    evo_agc_writer_set_user_data_gs(cb, vs_user, ud.vs_count);

    uint32_t ps_user[16] = {0};
    ps_user[ud.ps_const_table_dword] = (uint32_t)const_desc.gpu_addr;
    ps_user[ud.ps_texture_table_dword] = (uint32_t)tex_desc.gpu_addr;
    evo_agc_writer_set_user_data_ps(cb, ps_user, ud.ps_count);

    evo_agc_writer_draw_index_modifier(cb, quad->index_count,
                                       reinterpret_cast<const uint16_t *>(quad->index_data),
                                       evo_agc_runtime_get_pipe_draw_modifier(EVO_AGC_PIPE_UI_BLUR));
    evo_agc_runtime_note_draw();
    return true;
}

void EvoRenderInterfaceAGC::DrawCopyPass(const evo_agc_layer_surface_t &src,
                                         bool src_is_base,
                                         Rml::BlendMode blend_mode)
{
    EvoAgcTexture src_tex;
    src_tex.width = (int)src.width;
    src_tex.height = (int)src.height;
    src_tex.pitch = (int)src.pitch_bytes;
    if (src_is_base)
        evo_agc_build_tsharp_bgra8(src_tex.descriptor, src.gpu_addr,
                                   src.width, src.height, src.pitch_bytes);
    else
        evo_agc_build_tsharp_rgba8(src_tex.descriptor, src.gpu_addr,
                                   src.width, src.height, src.pitch_bytes);
    evo_agc_build_ssharp(src_tex.descriptor + EVO_AGC_TSHARP_DWORDS, 1, 1);

    evo_agc_runtime_set_blend(blend_mode == Rml::BlendMode::Replace
                                  ? EVO_AGC_BLEND_NONE
                                  : EVO_AGC_BLEND_PREMULTIPLIED);
    /* RenderGeometry binds the UI pipe itself and applies the active scissor
     * and SDF clip, which is exactly the composite RmlUi wants here. */
    RenderGeometry(m_fullscreen_quad, Rml::Vector2f(0, 0), (Rml::TextureHandle)&src_tex);
}

void EvoRenderInterfaceAGC::CompositeLayers(Rml::LayerHandle source_handle,
                                            Rml::LayerHandle destination_handle,
                                            Rml::BlendMode blend_mode,
                                            Rml::Span<const Rml::CompiledFilterHandle> filters)
{
    EVO_BD_TRACE(4, "backdrop: CompositeLayers enter src=%p dst=%p nfilters=%zu",
                 (void *)source_handle, (void *)destination_handle, filters.size());

    if (!evo_agc_has_layers())
        return;

    /* Handle 0 is the base (scanout) layer; anything else is a surface we
     * minted in PushLayer. A handle that no longer resolves means a degraded
     * (pool-exhausted) push - skip the composite so the backdrop stays absent
     * instead of faulting on a stale pointer. */
    evo_agc_layer_surface_t src_storage;
    bool src_is_base = false;
    const evo_agc_layer_surface_t *src = nullptr;
    if (source_handle == 0) {
        evo_agc_get_scanout_layer(&src_storage);
        src = &src_storage;
        src_is_base = true;
    } else if ((src = ResolveLayer(source_handle)) == nullptr) {
        return;
    }

    const evo_agc_layer_surface_t *dst = nullptr;
    if (destination_handle != 0) {
        dst = ResolveLayer(destination_handle);
        if (!dst)
            return;
    }

    const evo_agc_layer_surface_t *target_before =
        m_layer_stack.empty() ? nullptr : m_layer_stack.back();

    /* Strongest sigma among the non-zero handles. Only blur filters are ever
     * compiled here, so any non-zero handle is a blur. */
    float sigma = 0.0f;
    for (Rml::CompiledFilterHandle handle : filters) {
        if (!handle)
            continue;
        for (const auto &filter : m_filters) {
            if ((Rml::CompiledFilterHandle)(uintptr_t)filter.get() == handle) {
                sigma = std::max(sigma, filter->sigma);
                break;
            }
        }
    }

    static bool logged_call = false;
    if (!logged_call) {
        logged_call = true;
        evo_log("backdrop: CompositeLayers src=%u dst=%u blend=%d filters=%zu",
                (unsigned)(uintptr_t)source_handle, (unsigned)(uintptr_t)destination_handle,
                (int)blend_mode, filters.size());
        evo_log_flush();
    }

    EnsureFullscreenQuad();
    if (!m_fullscreen_quad)
        return;

    if (sigma > 0.0f) {
        /*
         * Blurred source -> destination, scissored to the backdrop region.
         *
         * Two passes need two layer surfaces (H writes, V reads), then the
         * result is composited onto destination with RmlUi's blend mode - the
         * same shape as RmlUi's GL3 backend (filter into a temp, composite the
         * temp). Pool exhaustion degrades to "no blur" rather than drawing
         * from an unwritten surface.
         */
        static bool logged_blur_path = false;
        if (!logged_blur_path) {
            logged_blur_path = true;
            evo_log("backdrop: taking blur H/V path sigma=%.2f", sigma);
            evo_log_flush();
        }
        evo_agc_layer_surface_t *h_scratch = nullptr, *v_scratch = nullptr;
        if (evo_agc_layer_acquire(&h_scratch) != 0 || !h_scratch) {
            static bool logged_no_scratch = false;
            if (!logged_no_scratch) {
                logged_no_scratch = true;
                evo_log("backdrop: blur path skipped - no layer surface (scratch H)");
                evo_log_flush();
            }
            return;
        }
        if (evo_agc_layer_acquire(&v_scratch) != 0 || !v_scratch) {
            static bool logged_no_vscratch = false;
            if (!logged_no_vscratch) {
                logged_no_vscratch = true;
                evo_log("backdrop: blur path skipped - no layer surface (scratch V)");
                evo_log_flush();
            }
            evo_agc_layer_release(h_scratch);
            return;
        }

        evo_agc_runtime_bind_pipeline(EVO_AGC_PIPE_UI_BLUR);
        evo_agc_runtime_set_blend(EVO_AGC_BLEND_NONE);

        /* The source may hold this frame's base draws or an earlier layer
         * composite - flush so the sampler sees them (event 45, all CBs). */
        evo_agc_flush_color_target();

        evo_agc_set_layer_target(h_scratch);
        DrawBlurPass(*src, src_is_base, sigma, true /* horizontal */);
        evo_agc_flush_color_target();

        evo_agc_set_layer_target(v_scratch);
        DrawBlurPass(*h_scratch, false, sigma, false /* vertical */);
        evo_agc_flush_color_target();

        evo_agc_layer_release(h_scratch);

        evo_agc_set_layer_target(dst);
        DrawCopyPass(*v_scratch, false, blend_mode);
        evo_agc_flush_color_target();

        evo_agc_layer_release(v_scratch);
    } else {
        /* No blur filter: plain composite source -> destination with RmlUi's
         * blend mode. Skip the degenerate base-onto-base self copy. */
        static bool logged_plain = false;
        if (!logged_plain) {
            logged_plain = true;
            evo_log("backdrop: plain composite (no blur filter) sigma=0");
            evo_log_flush();
        }
        if (!(src_is_base && dst == nullptr))
            DrawCopyPass(*src, src_is_base, blend_mode);
    }

    /* RmlUi has already pushed any layer it needed, and expects the stack top
     * - not the composite destination - to remain the render target. */
    evo_agc_set_layer_target(target_before);
}
