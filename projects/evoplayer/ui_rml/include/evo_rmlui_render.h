#pragma once
#include <RmlUi/Core/RenderInterface.h>
#include <vector>
#include <cstdint>

class EvoRenderInterface : public Rml::RenderInterface {
public:
    EvoRenderInterface(int width, int height);
    virtual ~EvoRenderInterface();

    void SetFramebuffer(uint32_t* fb) { m_fb = fb; }
    void SetDimensions(int w, int h) { m_width = w; m_height = h; }

    void SetScissorRegion(Rml::Rectanglei region) override;
    void EnableScissorRegion(bool enable) override;

    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    void SetTransform(const Rml::Matrix4f* transform) override;

private:
    int m_width;
    int m_height;
    uint32_t* m_fb;
    bool m_scissor_enabled;
    Rml::Rectanglei m_scissor_region;
    bool m_has_transform;
    Rml::Matrix4f m_transform;
};
