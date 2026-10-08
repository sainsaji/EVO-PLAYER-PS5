/*
 * hui_agc_batch.cpp - ps5-homebrew-ui draw lists on sceAgc. See the header.
 *
 * Per frame: every run of the list becomes one indexed draw through
 * EVO_AGC_PIPE_UI_SDF. The run's instances are written six times each into
 * the frame's transient-ring slot (the ring is flushed and recycled by the
 * runtime, so nothing here outlives the frame), a V# points at the run's
 * first vertex, and identity indices 0..n-1 drive it - the shader takes the
 * quad corner from gl_VertexIndex % 6. Indexed rather than auto-indexed
 * because DrawIndex is the packet every hardware-verified EVO draw uses.
 */
#include "hui_agc_batch.hpp"

#include "evo_agc_runtime.h"
#include "evo_agc_transient_ring.h"
#include "evo_agc_writer.h"
#include "evo_boot_log.h"
#include "evo_direct_mem.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace evo::hui_agc
{

namespace
{

using hui::gfx::Instance;
using hui::gfx::MeshVertex;

/* 16-bit indices: one draw covers at most this many vertices, so a run longer
 * than kMaxVertices / 6 instances is split. */
constexpr std::uint32_t kMaxVertices = 65532; /* a multiple of 6 */
constexpr float kMeshShape = 9.0f;            /* ui_sdf.pipe: a plain mesh vertex */
/* Frames a released texture is kept: the runtime has up to three frames in
 * flight, plus one for luck. */
constexpr int kFreeDelayFrames = 4;

} // namespace

struct AgcBatch::Texture
{
    int width = 0;
    int height = 0;
    int pitch = 0;
    int bytes_per_pixel = 4;
    void *pixels = nullptr;     /* 256-byte aligned: what the T# points at */
    void *alloc_base = nullptr; /* what must be freed */
    std::uint32_t descriptor[EVO_AGC_COMBINED_DESCRIPTOR_DWORDS] = {0};
};

AgcBatch::~AgcBatch()
{
    release();
}

bool AgcBatch::init()
{
    release();
    const evo_agc_user_data_layout_t ud = evo_agc_runtime_get_user_data_layout(EVO_AGC_PIPE_UI_SDF);
    if (!ud.vs_count || ud.vs_const_table_dword < 0 || ud.vs_vertex_table_dword < 0 ||
        ud.ps_texture_table_dword < 0)
    {
        evo_boot_log("hui: ui_sdf pipe unavailable, kit UI off");
        return false;
    }

    const std::size_t index_bytes = kMaxVertices * sizeof(std::uint16_t);
    indices_base_ = evo_direct_mem_alloc(index_bytes + 255u);
    if (!indices_base_)
        return false;
    indices_ = reinterpret_cast<std::uint16_t *>(
        (reinterpret_cast<std::uintptr_t>(indices_base_) + 255u) & ~static_cast<std::uintptr_t>(255));
    for (std::uint32_t i = 0; i < kMaxVertices; ++i)
        indices_[i] = static_cast<std::uint16_t>(i);
    evo_agc_runtime_cache_flush(indices_, index_bytes);

    const std::uint8_t white[4] = {255, 255, 255, 255};
    white_ = make_texture(1, 1, 4, white);
    if (!white_)
    {
        release();
        return false;
    }
    ok_ = true;
    return true;
}

void AgcBatch::release()
{
    /* Only ever called with the GPU idle (shutdown), so frees are immediate. */
    for (Pending &p : pending_free_)
        destroy(p.texture);
    pending_free_.clear();
    for (Texture *&t : textures_)
    {
        destroy(t);
        t = nullptr;
    }
    textures_.clear();
    for (Texture *&t : fonts_)
    {
        destroy(t);
        t = nullptr;
    }
    font_count_ = 0;
    destroy(white_);
    white_ = nullptr;
    if (indices_base_)
        evo_direct_mem_free(indices_base_);
    indices_base_ = nullptr;
    indices_ = nullptr;
    ok_ = false;
}

AgcBatch::Texture *AgcBatch::make_texture(int width, int height, int bytes_per_pixel,
                                          const std::uint8_t *pixels)
{
    if (width <= 0 || height <= 0 || !pixels)
        return nullptr;
    auto *tex = new Texture();
    tex->width = width;
    tex->height = height;
    tex->bytes_per_pixel = bytes_per_pixel;
    const std::uint32_t row_bytes = static_cast<std::uint32_t>(width * bytes_per_pixel);
    tex->pitch = static_cast<int>((row_bytes + 255u) & ~255u);
    const std::size_t bytes = static_cast<std::size_t>(height) * static_cast<std::size_t>(tex->pitch);
    /* A T# stores the base >> 8: the pixels must start on 256 bytes, and
     * evo_direct_mem_alloc only promises 64 (see CreateTextureInternal). */
    tex->alloc_base = evo_direct_mem_alloc(bytes + 255u);
    if (!tex->alloc_base)
    {
        evo_agc_runtime_note_drop(1);
        delete tex;
        return nullptr;
    }
    tex->pixels = reinterpret_cast<void *>(
        (reinterpret_cast<std::uintptr_t>(tex->alloc_base) + 255u) & ~static_cast<std::uintptr_t>(255));
    auto *dst = static_cast<std::uint8_t *>(tex->pixels);
    for (int y = 0; y < height; ++y)
        std::memcpy(dst + static_cast<std::size_t>(y) * tex->pitch,
                    pixels + static_cast<std::size_t>(y) * row_bytes, row_bytes);
    evo_agc_runtime_cache_flush(tex->pixels, bytes);

    const std::uint64_t gpu = reinterpret_cast<std::uintptr_t>(tex->pixels);
    const int rc = (bytes_per_pixel == 1)
                       ? evo_agc_build_tsharp_r8(tex->descriptor, gpu, static_cast<std::uint32_t>(width),
                                                 static_cast<std::uint32_t>(height),
                                                 static_cast<std::uint32_t>(tex->pitch))
                       : evo_agc_build_tsharp_rgba8(tex->descriptor, gpu, static_cast<std::uint32_t>(width),
                                                    static_cast<std::uint32_t>(height),
                                                    static_cast<std::uint32_t>(tex->pitch));
    if (rc != 0)
    {
        evo_boot_log("hui: texture %dx%d bpp=%d rejected by the T# builder", width, height,
                     bytes_per_pixel);
        evo_direct_mem_free(tex->alloc_base);
        delete tex;
        return nullptr;
    }
    evo_agc_build_ssharp(tex->descriptor + EVO_AGC_TSHARP_DWORDS, 1 /* clamp */, 1 /* bilinear */);
    return tex;
}

void AgcBatch::destroy(Texture *texture)
{
    if (!texture)
        return;
    if (texture->alloc_base)
        evo_direct_mem_free(texture->alloc_base);
    delete texture;
}

std::uint32_t AgcBatch::create_font_texture(const hui::gfx::Font &font)
{
    if (!ok_ || font_count_ >= hui::gfx::kFontSlots)
        return 0;
    Texture *tex = make_texture(font.atlas_width(), font.atlas_height(), 1, font.atlas().data());
    if (!tex)
        return 0;
    fonts_[font_count_] = tex;
    ++font_count_;
    return hui::gfx::kFontHandleBase | font_count_;
}

bool AgcBatch::update_font_texture(std::uint32_t handle, const hui::gfx::Font &font)
{
    if (!hui::gfx::is_font_handle(handle))
        return false;
    const std::uint32_t slot = (handle & 0xfu) - 1u;
    if (slot >= font_count_)
        return false;
    Texture *tex = make_texture(font.atlas_width(), font.atlas_height(), 1, font.atlas().data());
    if (!tex)
        return false;
    if (fonts_[slot])
        pending_free_.push_back({fonts_[slot], kFreeDelayFrames});
    fonts_[slot] = tex;
    return true;
}

std::uint32_t AgcBatch::create_texture(int width, int height, const std::uint8_t *rgba)
{
    if (!ok_)
        return 0;
    Texture *tex = make_texture(width, height, 4, rgba);
    if (!tex)
        return 0;
    for (std::size_t i = 0; i < textures_.size(); ++i)
    {
        if (!textures_[i])
        {
            textures_[i] = tex;
            return static_cast<std::uint32_t>(i + 1);
        }
    }
    textures_.push_back(tex);
    return static_cast<std::uint32_t>(textures_.size());
}

void AgcBatch::release_texture(std::uint32_t handle)
{
    if (handle == 0 || hui::gfx::is_font_handle(handle) || handle > textures_.size())
        return;
    Texture *&slot = textures_[handle - 1];
    if (slot)
        pending_free_.push_back({slot, kFreeDelayFrames});
    slot = nullptr;
}

void AgcBatch::end_frame()
{
    for (std::size_t i = 0; i < pending_free_.size();)
    {
        if (--pending_free_[i].frames_left <= 0)
        {
            destroy(pending_free_[i].texture);
            pending_free_[i] = pending_free_.back();
            pending_free_.pop_back();
        }
        else
        {
            ++i;
        }
    }
}

const std::uint32_t *AgcBatch::descriptor_for(std::uint32_t handle) const
{
    if (handle == 0)
        return white_->descriptor;
    if (hui::gfx::is_font_handle(handle))
    {
        const std::uint32_t slot = (handle & 0xfu) - 1u;
        return (slot < font_count_ && fonts_[slot]) ? fonts_[slot]->descriptor : white_->descriptor;
    }
    if (handle <= textures_.size() && textures_[handle - 1])
        return textures_[handle - 1]->descriptor;
    return white_->descriptor;
}

void AgcBatch::draw(const hui::gfx::DrawList &list, const hui::gfx::Viewport &viewport,
                    int surface_width, int surface_height)
{
    draw_calls_ = 0;
    if (!ok_ || list.empty())
        return;
    SceAgcCommandBuffer *cb = evo_agc_runtime_get_current_cb();
    if (!cb)
        return;

    evo_agc_runtime_bind_pipeline(EVO_AGC_PIPE_UI_SDF);
    evo_agc_runtime_set_blend(EVO_AGC_BLEND_PREMULTIPLIED);
    const evo_agc_user_data_layout_t ud = evo_agc_runtime_get_user_data_layout(EVO_AGC_PIPE_UI_SDF);
    if (!ud.vs_count || ud.vs_count > 16 || ud.ps_count > 16)
        return;
    const std::uint64_t modifier = evo_agc_runtime_get_pipe_draw_modifier(EVO_AGC_PIPE_UI_SDF);

    evo_agc_transient_ring_t *ring = evo_agc_runtime_get_transient_ring();
    const std::uint32_t slot = evo_agc_runtime_get_current_slot();
    auto alloc = [&](std::size_t bytes, std::size_t align, evo_agc_transient_slice_t *out) {
        if (evo_agc_transient_ring_alloc(ring, slot, bytes, align, out) != EVO_AGC_TRANSIENT_OK)
        {
            evo_agc_runtime_note_drop(0);
            return false;
        }
        return true;
    };

    /* Constants for the whole list, and their V#. */
    evo_agc_transient_slice_t consts, consts_desc;
    if (!alloc(32, 16, &consts) || !alloc(16, 16, &consts_desc))
        return;
    float *c = static_cast<float *>(consts.cpu);
    c[0] = viewport.scale;
    c[1] = viewport.offset_x;
    c[2] = viewport.offset_y;
    c[3] = 0.0f;
    c[4] = static_cast<float>(surface_width);
    c[5] = static_cast<float>(surface_height);
    c[6] = 0.0f;
    c[7] = 0.0f;
    evo_agc_build_constant_vsharp(static_cast<std::uint32_t *>(consts_desc.cpu), consts.gpu_addr, 32);

    /* The seven combined descriptors the pixel stage reads; slot 0 changes
     * per run, 1..6 are the font atlases for the whole list. */
    std::uint32_t textures[7][EVO_AGC_COMBINED_DESCRIPTOR_DWORDS];
    for (std::uint32_t f = 0; f < hui::gfx::kFontSlots; ++f)
    {
        const std::uint32_t *d = (f < font_count_ && fonts_[f]) ? fonts_[f]->descriptor : white_->descriptor;
        std::memcpy(textures[f + 1], d, sizeof(textures[0]));
    }

    const auto &instances = list.instances();
    const auto &mesh = list.mesh_vertices();
    bool scissor = false;
    std::uint32_t bound_texture = 0xffffffffu;
    std::uint64_t texture_table = 0;

    for (const hui::gfx::Run &run : list.runs())
    {
        if (run.count == 0)
            continue;

        if (run.clipped)
        {
            const float x0 = std::floor(run.clip.x * viewport.scale + viewport.offset_x);
            const float y0 = std::floor(run.clip.y * viewport.scale + viewport.offset_y);
            const float x1 = std::ceil((run.clip.x + run.clip.w) * viewport.scale + viewport.offset_x);
            const float y1 = std::ceil((run.clip.y + run.clip.h) * viewport.scale + viewport.offset_y);
            /* AGC scissors are top-left based, unlike glScissor. */
            evo_agc_runtime_set_scissor(static_cast<int>(x0), static_cast<int>(y0),
                                        static_cast<int>(std::max(0.0f, x1 - x0)),
                                        static_cast<int>(std::max(0.0f, y1 - y0)));
            scissor = true;
        }
        else if (scissor)
        {
            evo_agc_runtime_set_scissor(0, 0, surface_width, surface_height);
            scissor = false;
        }

        /* Fonts never change the run's own texture slot (they have theirs). */
        const std::uint32_t own = (run.texture != 0 && !hui::gfx::is_font_handle(run.texture)) ? run.texture : 0;
        if (own != bound_texture || texture_table == 0)
        {
            std::memcpy(textures[0], descriptor_for(own), sizeof(textures[0]));
            evo_agc_transient_slice_t table;
            if (!alloc(sizeof(textures), 16, &table))
                return;
            std::memcpy(table.cpu, textures, sizeof(textures));
            texture_table = table.gpu_addr;
            bound_texture = own;
        }

        /* Expand the run into vertices, one chunk per 16-bit draw. */
        const std::uint32_t total_vertices = run.mesh ? run.count : run.count * 6u;
        for (std::uint32_t done = 0; done < total_vertices;)
        {
            const std::uint32_t n = std::min(total_vertices - done, kMaxVertices);
            evo_agc_transient_slice_t verts, vsharp;
            if (!alloc(static_cast<std::size_t>(n) * sizeof(Instance), 256, &verts) ||
                !alloc(EVO_AGC_VSHARP_DWORDS * 4, 16, &vsharp))
                return;
            auto *out = static_cast<Instance *>(verts.cpu);
            if (run.mesh)
            {
                for (std::uint32_t i = 0; i < n; ++i)
                {
                    const MeshVertex &m = mesh[run.first + done + i];
                    Instance v{};
                    v.rect[0] = m.x;
                    v.rect[1] = m.y;
                    v.color_top[0] = m.r;
                    v.color_top[1] = m.g;
                    v.color_top[2] = m.b;
                    v.color_top[3] = m.a;
                    v.params[3] = kMeshShape;
                    out[i] = v;
                }
            }
            else
            {
                const std::uint32_t first = run.first + done / 6u;
                for (std::uint32_t i = 0; i < n / 6u; ++i)
                {
                    const Instance &src = instances[first + i];
                    for (int k = 0; k < 6; ++k)
                        out[i * 6u + static_cast<std::uint32_t>(k)] = src;
                }
            }
            evo_agc_build_vsharp(static_cast<std::uint32_t *>(vsharp.cpu), verts.gpu_addr,
                                 sizeof(Instance), n);

            std::uint32_t vs_user[16] = {0};
            vs_user[ud.vs_const_table_dword] = static_cast<std::uint32_t>(consts_desc.gpu_addr);
            vs_user[ud.vs_vertex_table_dword] = static_cast<std::uint32_t>(vsharp.gpu_addr);
            evo_agc_writer_set_user_data_gs(cb, vs_user, ud.vs_count);
            std::uint32_t ps_user[16] = {0};
            ps_user[ud.ps_texture_table_dword] = static_cast<std::uint32_t>(texture_table);
            evo_agc_writer_set_user_data_ps(cb, ps_user, ud.ps_count);

            evo_agc_writer_draw_index_modifier(cb, n, indices_, modifier);
            evo_agc_runtime_note_draw();
            ++draw_calls_;
            done += n;
        }
    }
    if (scissor)
        evo_agc_runtime_set_scissor(0, 0, surface_width, surface_height);
}

} // namespace evo::hui_agc
