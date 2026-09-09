/*
 * evo_rmlui_render_gl.cpp - see evo_rmlui_render_gl.h.
 *
 * RmlUi's upstream RenderInterface_GL3 (src/rmlui_gl3/, vendored verbatim) plus
 * EVO's texture adapters. Everything geometry-, clip- and transform-related is
 * inherited unchanged.
 */
#include "evo_rmlui_render_gl.h"
#include "evo_rmlui_bundle.h"

/* GL entry points. Host (GL-2): glad declarations (definitions + loader live in
 * rmlui_gl3/RmlUi_Renderer_GL3.cpp, which #defines GLAD_GL_IMPLEMENTATION).
 * Device (GL-3, #79): RMLUI_GL3_CUSTOM_LOADER - ps5-opengl links GL directly, no
 * glad, so pull the same prototype header RmlUi_Renderer_GL3.cpp uses. */
#if defined(RMLUI_GL3_CUSTOM_LOADER)
#include RMLUI_GL3_CUSTOM_LOADER
#else
#include "rmlui_gl3/RmlUi_Include_GL3.h"
#endif

#include <algorithm>
#include <cstring>

/* Own private stb copy (STATIC => no clash with the one in evo_rmlui_render.cpp). */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../../stb_image.h"

EvoRenderInterfaceGL::EvoRenderInterfaceGL(int width, int height)
    : m_width(width), m_height(height)
{
    SetViewport(width, height);
}

EvoRenderInterfaceGL::~EvoRenderInterfaceGL() = default;

void EvoRenderInterfaceGL::SetDimensions(int w, int h)
{
    if (w == m_width && h == m_height)
        return;
    m_width = w;
    m_height = h;
    SetViewport(w, h);
}

/* ===================================================================== *
 *  Frame bracket - bind the FBO, read the composite back over m_target
 * ===================================================================== */

void EvoRenderInterfaceGL::FrameBegin()
{
#if defined(EVO_GL_DEVICE)
    /* Device (GL-3, #79): main.c clears fb 0 once per frame (evo_gl_frame_begin)
     * BEFORE the screen dispatch, then each screen / overlay pass composites
     * over it - so no per-pass fb 0 clear here. EndFrame() blits straight to
     * fb 0 and eglSwapBuffers presents it; there is no readback. */
    BeginFrame();
    return;
#else
    /* Host (GL-2): clear the pbuffer backbuffer. RenderInterface_GL3 clears its
     * own layer stack in BeginFrame() but NOT fb 0, and EndFrame() composites
     * onto fb 0 with premultiplied blending - so without this an overlay pass
     * (toast / dialog over a menu) that leaves most of the frame transparent
     * would show the *previous* frame's fb 0 underneath. We re-read fb 0 and
     * composite over m_target ourselves, so a clean transparent fb 0 is what we
     * want. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);

    BeginFrame();   /* RmlUi: binds + clears the layer stack to transparent black */
#endif
}

void EvoRenderInterfaceGL::FrameEnd()
{
    EndFrame();     /* RmlUi: resolves MSAA + blits the composite to fb 0 */

#if defined(EVO_GL_DEVICE)
    return;         /* composited straight to fb 0; main.c does eglSwapBuffers */
#else
    if (!m_target || m_width <= 0 || m_height <= 0)
        return;

    const size_t px = (size_t)m_width * (size_t)m_height;
    if (m_scratch.size() < px)
        m_scratch.resize(px);

    /* fb 0 is the pbuffer backbuffer (evo_gl_context_host.cpp). Its bytes are
     * R,G,B,A == EVO's 0xAABBGGRR in memory, premultiplied, y-up. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, m_scratch.data());

    /* Composite premultiplied UI over whatever the caller had in m_target
     * (a background fill for a full screen; the menu underneath for a toast /
     * dialog overlay), flipping y as we go. Matches EvoRenderInterface's
     * blend_premul convention. */
    for (int y = 0; y < m_height; y++) {
        const uint32_t* src = &m_scratch[(size_t)(m_height - 1 - y) * m_width];
        uint32_t* dst = &m_target[(size_t)y * m_width];
        for (int x = 0; x < m_width; x++) {
            uint32_t s = src[x];
            uint32_t a = (s >> 24) & 0xFF;
            if (a == 0)
                continue;
            if (a == 0xFF) {
                dst[x] = s;
                continue;
            }
            uint32_t inv = 255 - a;
            uint32_t d = dst[x];
            uint32_t sr = s & 0xFF, sg = (s >> 8) & 0xFF, sb = (s >> 16) & 0xFF;
            uint32_t dr = d & 0xFF, dg = (d >> 8) & 0xFF, db = (d >> 16) & 0xFF;
            uint32_t r = sr + (dr * inv + 127) / 255;
            uint32_t g = sg + (dg * inv + 127) / 255;
            uint32_t b = sb + (db * inv + 127) / 255;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            dst[x] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    }
#endif /* !EVO_GL_DEVICE */
}

/* ===================================================================== *
 *  Textures
 * ===================================================================== */

Rml::TextureHandle EvoRenderInterfaceGL::UploadRGBA(const uint32_t* rgba, int w, int h,
                                                    Rml::Vector2i& out_dims)
{
    out_dims = Rml::Vector2i(w, h);
    Rml::Span<const Rml::byte> span(reinterpret_cast<const Rml::byte*>(rgba),
                                    (size_t)w * (size_t)h * 4);
    return RenderInterface_GL3::GenerateTexture(span, out_dims);
}

void EvoRenderInterfaceGL::SetMemoryTexture(const std::string& key, const uint32_t* bgra,
                                           int w, int h)
{
    if (!bgra || w <= 0 || h <= 0) {
        m_mem_textures.erase(key);
        return;
    }
    MemImage& img = m_mem_textures[key];
    img.width = w;
    img.height = h;
    img.rgba.assign(bgra, bgra + (size_t)w * (size_t)h);

    /* Premultiply (usually a no-op - decoded artwork is opaque). Same as
     * EvoRenderInterface::SetMemoryTexture. Bytes are R,G,B,A. */
    for (uint32_t& px : img.rgba) {
        uint32_t a = (px >> 24) & 0xFF;
        if (a == 255) continue;
        uint32_t r = px & 0xFF, g = (px >> 8) & 0xFF, b = (px >> 16) & 0xFF;
        r = (r * a + 127) / 255;
        g = (g * a + 127) / 255;
        b = (b * a + 127) / 255;
        px = (a << 24) | (b << 16) | (g << 8) | r;
    }
}

void EvoRenderInterfaceGL::DropMemoryTexture(const std::string& key)
{
    m_mem_textures.erase(key);
}

Rml::TextureHandle EvoRenderInterfaceGL::LoadTexture(Rml::Vector2i& texture_dimensions,
                                                    const Rml::String& source)
{
    /* Runtime artwork: the memory registry, never disk. */
    if (source.compare(0, 8, "evo:mem/") == 0) {
        auto it = m_mem_textures.find(std::string(source));
        if (it == m_mem_textures.end()) {
            texture_dimensions = Rml::Vector2i(0, 0);
            return 0;
        }
        return UploadRGBA(it->second.rgba.data(), it->second.width, it->second.height,
                          texture_dimensions);
    }

    std::string filename = source;
    size_t last_slash = source.find_last_of("/\\");
    if (last_slash != std::string::npos)
        filename = source.substr(last_slash + 1);

    int width = 0, height = 0, channels = 0;
    unsigned char* data = nullptr;

    /* #60: embedded bundle first, same as evo_rmlui_render.cpp's CPU path. */
    const EvoRmlBundleFile* bundled = evo_rmlui_bundle_find(source);
    if (!bundled) bundled = evo_rmlui_bundle_find("icons/" + filename);
    if (bundled)
        data = stbi_load_from_memory(bundled->data, (int)bundled->size,
                                     &width, &height, &channels, 4);

    if (!data) {
        /* Disk fallback - the same prefixes evo_rmlui_render.cpp tries. */
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
        for (const auto& path : candidates) {
            data = stbi_load(path.c_str(), &width, &height, &channels, 4);
            if (data) break;
        }
    }

    if (!data) {
        texture_dimensions = Rml::Vector2i(0, 0);
        return 0;
    }

    /* Premultiply on the way in - identical to evo_rmlui_render.cpp. The atlas
     * and the GL3 shaders are premultiplied; a PNG on disk is not. */
    std::vector<uint32_t> rgba((size_t)width * (size_t)height);
    for (int i = 0; i < width * height; i++) {
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

    return UploadRGBA(rgba.data(), width, height, texture_dimensions);
}
