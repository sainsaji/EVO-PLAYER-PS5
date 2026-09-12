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

private:
    int m_width;
    int m_height;
    bool m_scissor_enabled;
    Rml::Rectanglei m_scissor_region;

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
