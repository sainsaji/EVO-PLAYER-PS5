#pragma once
#include <RmlUi/Core/RenderInterface.h>
#include <vector>
#include <cstdint>
#include <string>
#include <map>

class EvoRenderInterface : public Rml::RenderInterface {
public:
    EvoRenderInterface(int width, int height);
    virtual ~EvoRenderInterface();

    void SetFramebuffer(uint32_t* fb) { m_fb = fb; }
    void SetDimensions(int w, int h) { m_width = w; m_height = h; }

    /*
     * Artwork the engine produces at runtime — decoded posters, the hero
     * still — has no file on disk to point an <img src> at, so it is
     * registered under a name in the "evo:mem/" namespace and LoadTexture
     * resolves that name out of this map instead of hitting the filesystem.
     * The pixels are copied because the caller's buffer is a rotating cache.
     */
    void SetMemoryTexture(const std::string& key, const uint32_t* bgra, int w, int h);
    void DropMemoryTexture(const std::string& key);

    void SetScissorRegion(Rml::Rectanglei region) override;
    void EnableScissorRegion(bool enable) override;

    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    void SetTransform(const Rml::Matrix4f* transform) override;

    void EnableClipMask(bool enable) override;
    void RenderToClipMask(Rml::ClipMaskOperation operation,
                          Rml::CompiledGeometryHandle geometry,
                          Rml::Vector2f translation) override;

private:
    int m_width;
    int m_height;
    uint32_t* m_fb;
    bool m_scissor_enabled;
    Rml::Rectanglei m_scissor_region;
    bool m_has_transform;
    Rml::Matrix4f m_transform;

    struct MemImage {
        std::vector<uint32_t> pixels;
        int width = 0;
        int height = 0;
    };
    std::map<std::string, MemImage> m_mem_textures;

    /*
     * Clip mask: 8-bit coverage, one byte per screen pixel.
     *
     * RmlUi asks for one whenever a clip cannot be expressed as a plain
     * rectangle - which is every card in this UI, because they all pair
     * `overflow: hidden` with a `border-radius`. Both hooks are optional in
     * the render interface and default to doing nothing, and that default is
     * silent: the geometry still draws, just unclipped, so the hero artwork
     * and every tile's poster painted square over the rounded corner they
     * were supposed to be cut by.
     *
     * Only the box below holds meaningful bytes. Outside it the mask reads as
     * zero - fully clipped - which is what lets Set and Intersect write just
     * the region they touch instead of clearing the whole screen each time.
     */
    std::vector<uint8_t> m_clip_mask;
    bool m_clip_enabled = false;
    int m_mask_x0 = 0, m_mask_y0 = 0, m_mask_x1 = 0, m_mask_y1 = 0;
};
