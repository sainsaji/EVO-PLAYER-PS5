/*
 * hui_agc_batch.hpp - draws ps5-homebrew-ui draw lists through sceAgc.
 *
 * The kit (third_party/ps5-homebrew-ui) records every shape, glyph and image
 * of a frame into a hui::gfx::DrawList and hands it to an OpenGL backend
 * (gfx/gl_batch.cpp). EVO has no OpenGL: the AGC runtime owns the GPU. This is
 * the same backend for the AGC runtime, drawing through ui_sdf.pipe - the
 * kit's own SDF shader in LLPC form. The host preview keeps the kit's GL
 * backend, so both draw the same list with the same shader.
 *
 * Textures are opaque uint32 handles, as the kit expects: font atlases get the
 * kit's font-slot handles (kFontHandleBase | 1..6), everything else a small
 * index into this batch's table. A released texture is freed a few frames
 * later, once the GPU can no longer be reading it.
 *
 * Main thread only, like the rest of the UI. draw() must run between
 * evo_agc_runtime_frame_begin() and the frame's present.
 */
#ifndef EVO_HUI_AGC_BATCH_HPP
#define EVO_HUI_AGC_BATCH_HPP

#include "gfx/draw_list.hpp"
#include "gfx/font.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace evo::hui_agc
{

class AgcBatch
{
  public:
    AgcBatch() = default;
    AgcBatch(const AgcBatch &) = delete;
    AgcBatch &operator=(const AgcBatch &) = delete;
    ~AgcBatch();

    /* False when the AGC runtime is down or ui_sdf.pipe did not load. */
    bool init();
    void release();
    bool ok() const { return ok_; }

    /* Uploads a font atlas (R8) into the next font slot and returns the kit's
     * slot handle. 0 when all six slots are taken or memory ran out. */
    std::uint32_t create_font_texture(const hui::gfx::Font &font);
    /* Re-uploads a slot's atlas after the font grew (the dynamic fallback
     * font adds glyphs at run time). */
    bool update_font_texture(std::uint32_t handle, const hui::gfx::Font &font);

    /* RGBA8, straight alpha, top row first: the byte order of EVO's
     * 0xAABBGGRR pixels. 0 on failure. */
    std::uint32_t create_texture(int width, int height, const std::uint8_t *rgba);
    void release_texture(std::uint32_t handle);

    /* Records the list into the current AGC frame. */
    void draw(const hui::gfx::DrawList &list, const hui::gfx::Viewport &viewport,
              int surface_width, int surface_height);

    std::size_t last_draw_calls() const { return draw_calls_; }

    /* Call once per presented frame: advances the deferred-free queue. */
    void end_frame();

  private:
    struct Texture;

    Texture *make_texture(int width, int height, int bytes_per_pixel, const std::uint8_t *pixels);
    void destroy(Texture *texture);
    const std::uint32_t *descriptor_for(std::uint32_t handle) const;

    bool ok_ = false;
    std::uint16_t *indices_ = nullptr; /* identity 0..kMaxVertices-1, GPU-visible */
    void *indices_base_ = nullptr;
    Texture *white_ = nullptr;
    Texture *fonts_[hui::gfx::kFontSlots] = {};
    std::uint32_t font_count_ = 0;
    std::vector<Texture *> textures_; /* handle - 1 -> texture; nullptr = free */
    struct Pending
    {
        Texture *texture;
        int frames_left;
    };
    std::vector<Pending> pending_free_;
    std::size_t draw_calls_ = 0;
};

} // namespace evo::hui_agc

#endif /* EVO_HUI_AGC_BATCH_HPP */
