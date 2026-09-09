#pragma once

/*
 * evo_rmlui_render_gl.h - EVO's OpenGL RmlUi render interface (render-overhaul
 * GL-2, #78). Host-proven first (tools/uiview_playback_rml); the device path is
 * GL-3.
 *
 * It is RmlUi's upstream RenderInterface_GL3 (vendored verbatim under
 * src/rmlui_gl3/) with EVO's texture adapters grafted on:
 *
 *   - LoadTexture       resolves embedded-bundle paths (#60) and the runtime
 *                       "evo:mem/" artwork namespace before touching disk, and
 *                       premultiplies on the way in - matching
 *                       EvoRenderInterface (the CPU rasteriser) exactly.
 *   - SetMemoryTexture  registers decoded posters / hero art that have no file.
 *
 * Geometry, clip masks (stencil), transforms (MVP) and scissor are inherited
 * from RenderInterface_GL3 unchanged.
 *
 * The rest of EVO still hands it a plain BGRA framebuffer pointer: FrameBegin
 * binds the GL FBO, FrameEnd reads the composite back and blends it over that
 * buffer. A GL context must already be current (evo_gl_context_create()).
 */

#include "rmlui_gl3/RmlUi_Renderer_GL3.h"
#include "evo_rmlui_render_bridge.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

class EvoRenderInterfaceGL final : public RenderInterface_GL3, public EvoRenderBridge {
public:
    EvoRenderInterfaceGL(int width, int height);
    ~EvoRenderInterfaceGL() override;

    /* True if the base renderer constructed (shaders compiled). */
    bool Ok() const { return static_cast<bool>(static_cast<const RenderInterface_GL3&>(*this)); }

    /* -- EvoRenderBridge -- */
    Rml::RenderInterface* AsRml() override { return static_cast<RenderInterface_GL3*>(this); }
    void SetFramebuffer(uint32_t* fb) override { m_target = fb; }
    void SetDimensions(int w, int h) override;
    void SetMemoryTexture(const std::string& key, const uint32_t* bgra, int w, int h) override;
    void DropMemoryTexture(const std::string& key) override;
    void SetAgcSink(EvoAgcGeoSink*) override {}   /* retired in GL-3 */
    bool IsGpu() const override { return true; }
    void FrameBegin() override;
    void FrameEnd() override;

    /* -- Rml::RenderInterface override (bundle + evo:mem/ + premultiply) -- */
    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions,
                                   const Rml::String& source) override;

private:
    int m_width;
    int m_height;
    uint32_t* m_target = nullptr;   /* non-owning BGRA readback destination */

    std::vector<uint32_t> m_scratch;   /* glReadPixels staging (RGBA, y-up) */

    struct MemImage {
        std::vector<uint32_t> rgba;    /* premultiplied, 0xAABBGGRR-in-memory == RGBA bytes */
        int width = 0;
        int height = 0;
    };
    std::map<std::string, MemImage> m_mem_textures;

    Rml::TextureHandle UploadRGBA(const uint32_t* rgba, int w, int h,
                                  Rml::Vector2i& out_dims);
};
