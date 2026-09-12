#pragma once
#include <RmlUi/Core/RenderInterface.h>
#include "evo_rmlui_render_bridge.h"
#include "evo_agc_runtime.h"

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>

struct EvoAgcTexture {
    int      width = 0;
    int      height = 0;
    int      pitch = 0;
    void    *pixels = nullptr;      /* 256-byte aligned; what the T# points at */
    void    *alloc_base = nullptr;  /* raw allocation, what must be freed */
    size_t   alloc_bytes = 0;
    uint32_t descriptor[EVO_AGC_COMBINED_DESCRIPTOR_DWORDS] = {0};
};

struct EvoAgcCompiledGeometry {
    void    *vertex_data = nullptr;
    size_t   vertex_bytes = 0;
    uint32_t vertex_count = 0;

    void    *index_data = nullptr;
    size_t   index_bytes = 0;
    uint32_t index_count = 0;

    uint32_t vsharp[EVO_AGC_VSHARP_DWORDS] = {0};

    /* Untranslated bounding box of the vertices, in document pixels. Only used
     * when this geometry is handed to RenderToClipMask: a clip mask we cannot
     * render into the stencil is approximated by scissoring to this box. */
    float    bb_min_x = 0.0f, bb_min_y = 0.0f;
    float    bb_max_x = 0.0f, bb_max_y = 0.0f;
    /* Corner radii recovered from the tessellation, in RmlUi's corner order:
     * top-left, top-right, bottom-right, bottom-left. Zero for a square box.
     * Only meaningful for clip-mask geometry. */
    float    bb_radius[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

class EvoRenderInterfaceAGC : public Rml::RenderInterface, public EvoRenderBridge {
public:
    EvoRenderInterfaceAGC(int width, int height);
    virtual ~EvoRenderInterfaceAGC();

    Rml::RenderInterface* AsRml() override { return this; }
    bool IsGpu() const override { return true; }
    bool Ok() const { return m_white_texture != nullptr; }

    void SetFramebuffer(uint32_t* fb) override { (void)fb; }
    void SetDimensions(int w, int h) override;

    void SetMemoryTexture(const std::string& key, const uint32_t* bgra, int w, int h) override;
    void DropMemoryTexture(const std::string& key) override;

    void FrameBegin() override;
    void FrameEnd() override;

    /* Rml::RenderInterface implementation */
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry,
                        Rml::Vector2f translation,
                        Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions,
                                  const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
                                      Rml::Vector2i source_dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;

    void SetTransform(const Rml::Matrix4f* transform) override;

    /* Clip masks. RmlUi uses these for border-radius clipping and masked
     * overlays; leaving them unimplemented means children are never clipped to
     * a rounded container - square thumbnail corners, gradient masks in the
     * wrong place. Backed by the stencil buffer in evo_agc_runtime.c. */
    void EnableClipMask(bool enable) override;
    void RenderToClipMask(Rml::ClipMaskOperation operation,
                          Rml::CompiledGeometryHandle geometry,
                          Rml::Vector2f translation) override;

private:
    int m_width;
    int m_height;
    bool m_scissor_enabled;
    Rml::Rectanglei m_scissor_region;

    Rml::Matrix4f m_projection;
    Rml::Matrix4f m_transform;
    bool m_transform_active = false;

    /*
     * Bounding-box approximation of a clip mask.
     *
     * RmlUi only reaches for a clip mask when the clip is not a plain
     * rectangle - a rounded container, most often. The exact shape needs the
     * stencil buffer; the *extent* does not, and scissoring to the mask's
     * bounding box gets everything except the corner radius right. Without it
     * a rounded, overflow:hidden container does not clip its children at all:
     * the thumbnail art spills past its frame and paints over the focus border
     * the parent drew underneath it.
     *
     * The effective scissor is always the intersection of RmlUi's own scissor
     * region and this box - see ApplyScissorState().
     */
    bool m_clip_mask_enabled = false;
    bool m_clip_mask_valid = false;
    Rml::Rectanglei m_clip_mask_box;

    /* The same clip, in the form the fragment shader takes: document-pixel
     * rect plus corner radius. This is what actually does the clipping; the
     * scissor box above only culls. */
    float m_clip_rect[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float m_clip_radius[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    void ApplyScissorState();

    std::unique_ptr<EvoAgcTexture> m_white_texture;

    struct MemImage {
        std::vector<uint32_t> pixels;
        int width = 0;
        int height = 0;
        Rml::TextureHandle handle = 0;
    };
    std::map<std::string, MemImage> m_mem_textures;

    Rml::TextureHandle CreateTextureInternal(const uint32_t* rgba, int width, int height);
};
