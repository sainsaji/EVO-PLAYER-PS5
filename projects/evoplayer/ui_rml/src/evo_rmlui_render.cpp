#include "evo_rmlui_render.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>

struct RmlTexture {
    std::vector<uint32_t> pixels; // BGRA 0xAABBGGRR
    int width = 0;
    int height = 0;
};

struct RmlCompiledGeo {
    std::vector<Rml::Vertex> vertices;
    std::vector<int> indices;
};

/* Fast BGRA Alpha Blending for PS5 Native Linear Framebuffer (0xAABBGGRR) */
static inline uint32_t blend_bgra(uint32_t dst, uint32_t src, uint32_t a) {
    if (a == 0) return dst;
    if (a >= 255) return src | 0xFF000000;

    uint32_t inv = 255 - a;
    uint32_t dr = dst & 0xFF;
    uint32_t dg = (dst >> 8) & 0xFF;
    uint32_t db = (dst >> 16) & 0xFF;

    uint32_t sr = src & 0xFF;
    uint32_t sg = (src >> 8) & 0xFF;
    uint32_t sb = (src >> 16) & 0xFF;

    uint32_t r = (sr * a + dr * inv + 127) / 255;
    uint32_t g = (sg * a + dg * inv + 127) / 255;
    uint32_t b = (sb * a + db * inv + 127) / 255;

    return 0xFF000000 | (b << 16) | (g << 8) | r;
}

EvoRenderInterface::EvoRenderInterface(int width, int height)
    : m_width(width), m_height(height), m_fb(nullptr),
      m_scissor_enabled(false),
      m_has_transform(false)
{
    m_scissor_region = Rml::Rectanglei::FromPositionSize({0, 0}, {width, height});
}

EvoRenderInterface::~EvoRenderInterface() {
}

void EvoRenderInterface::SetScissorRegion(Rml::Rectanglei region) {
    m_scissor_region = region;
}

void EvoRenderInterface::EnableScissorRegion(bool enable) {
    m_scissor_enabled = enable;
}

void EvoRenderInterface::SetTransform(const Rml::Matrix4f* transform) {
    if (transform) {
        m_transform = *transform;
        m_has_transform = true;
    } else {
        m_has_transform = false;
    }
}

Rml::TextureHandle EvoRenderInterface::GenerateTexture(Rml::Span<const Rml::byte> source,
                                                       Rml::Vector2i source_dimensions)
{
    if (source.empty() || source_dimensions.x <= 0 || source_dimensions.y <= 0)
        return 0;

    RmlTexture* tex = new RmlTexture();
    tex->width = source_dimensions.x;
    tex->height = source_dimensions.y;
    tex->pixels.resize(tex->width * tex->height);

    // Source is RGBA bytes from font engine, pack to PS5 BGRA 0xAABBGGRR
    const uint8_t* src = (const uint8_t*)source.data();
    for (int i = 0; i < tex->width * tex->height; i++) {
        uint8_t r = src[i * 4 + 0];
        uint8_t g = src[i * 4 + 1];
        uint8_t b = src[i * 4 + 2];
        uint8_t a = src[i * 4 + 3];
        tex->pixels[i] = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
    }

    return reinterpret_cast<Rml::TextureHandle>(tex);
}

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../../stb_image.h"

Rml::TextureHandle EvoRenderInterface::LoadTexture(Rml::Vector2i& texture_dimensions,
                                                  const Rml::String& source)
{
    std::string filename = source;
    size_t last_slash = source.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        filename = source.substr(last_slash + 1);
    }

    std::vector<std::string> candidates = {
        source,
        "assets/icons/" + filename,
        "assets/rml/icons/" + filename,
        "assets/rml/" + source,
        "assets/" + source,
        "/data/evoplayer/app/assets/icons/" + filename,
        "/data/evoplayer/app/assets/rml/icons/" + filename,
        "/data/evoplayer/app/assets/rml/" + source,
        "/data/evoplayer/app/assets/" + source,
        "/data/homebrew/EVOPlayer/assets/icons/" + filename,
        "/data/homebrew/EVOPlayer/assets/rml/icons/" + filename,
        "/data/homebrew/EVOPlayer/assets/rml/" + source,
        "/data/homebrew/EVOPlayer/assets/" + source,
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

    int width = 0, height = 0, channels = 0;
    unsigned char* data = nullptr;

    for (const auto& path : candidates) {
        data = stbi_load(path.c_str(), &width, &height, &channels, 4);
        if (data) {
            break;
        }
    }

    if (!data) {
        texture_dimensions = Rml::Vector2i(0, 0);
        return 0;
    }

    texture_dimensions = Rml::Vector2i(width, height);
    RmlTexture* tex = new RmlTexture();
    tex->width = width;
    tex->height = height;
    tex->pixels.resize(width * height);

    for (int i = 0; i < width * height; i++) {
        uint8_t r = data[i * 4 + 0];
        uint8_t g = data[i * 4 + 1];
        uint8_t b = data[i * 4 + 2];
        uint8_t a = data[i * 4 + 3];
        tex->pixels[i] = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
    }

    stbi_image_free(data);
    return reinterpret_cast<Rml::TextureHandle>(tex);
}

void EvoRenderInterface::ReleaseTexture(Rml::TextureHandle texture) {
    if (texture) {
        RmlTexture* tex = reinterpret_cast<RmlTexture*>(texture);
        delete tex;
    }
}

Rml::CompiledGeometryHandle EvoRenderInterface::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                               Rml::Span<const int> indices)
{
    RmlCompiledGeo* geo = new RmlCompiledGeo();
    geo->vertices.assign(vertices.begin(), vertices.end());
    geo->indices.assign(indices.begin(), indices.end());
    return reinterpret_cast<Rml::CompiledGeometryHandle>(geo);
}

void EvoRenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) {
    if (geometry) {
        RmlCompiledGeo* geo = reinterpret_cast<RmlCompiledGeo*>(geometry);
        delete geo;
    }
}

// 2D Triangle Rasterizer with barycentric interpolation
static void draw_triangle(uint32_t* fb, int screen_w, int screen_h,
                          const Rml::Vertex& v0, const Rml::Vertex& v1, const Rml::Vertex& v2,
                          const RmlTexture* tex, const Rml::Vector2f& offset,
                          int clip_x0, int clip_y0, int clip_x1, int clip_y1)
{
    float x0 = v0.position.x + offset.x, y0 = v0.position.y + offset.y;
    float x1 = v1.position.x + offset.x, y1 = v1.position.y + offset.y;
    float x2 = v2.position.x + offset.x, y2 = v2.position.y + offset.y;

    int min_x = std::max(clip_x0, (int)std::floor(std::min({x0, x1, x2})));
    int max_x = std::min(clip_x1 - 1, (int)std::ceil(std::max({x0, x1, x2})));
    int min_y = std::max(clip_y0, (int)std::floor(std::min({y0, y1, y2})));
    int max_y = std::min(clip_y1 - 1, (int)std::ceil(std::max({y0, y1, y2})));

    if (min_x > max_x || min_y > max_y) return;

    float denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (std::abs(denom) < 0.0001f) return;
    float inv_denom = 1.0f / denom;

    for (int y = min_y; y <= max_y; y++) {
        float py = y + 0.5f;
        uint32_t* row = &fb[y * screen_w];

        for (int x = min_x; x <= max_x; x++) {
            float px = x + 0.5f;

            float w0 = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) * inv_denom;
            float w1 = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) * inv_denom;
            float w2 = 1.0f - w0 - w1;

            if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                int r = (int)(w0 * v0.colour.red + w1 * v1.colour.red + w2 * v2.colour.red);
                int g = (int)(w0 * v0.colour.green + w1 * v1.colour.green + w2 * v2.colour.green);
                int b = (int)(w0 * v0.colour.blue + w1 * v1.colour.blue + w2 * v2.colour.blue);
                int a = (int)(w0 * v0.colour.alpha + w1 * v1.colour.alpha + w2 * v2.colour.alpha);

                r = std::clamp(r, 0, 255);
                g = std::clamp(g, 0, 255);
                b = std::clamp(b, 0, 255);
                a = std::clamp(a, 0, 255);

                uint32_t col = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;

                if (tex && tex->width > 0 && tex->height > 0) {
                    float u = w0 * v0.tex_coord.x + w1 * v1.tex_coord.x + w2 * v2.tex_coord.x;
                    float v = w0 * v0.tex_coord.y + w1 * v1.tex_coord.y + w2 * v2.tex_coord.y;

                    int tx = std::clamp((int)(u * tex->width), 0, tex->width - 1);
                    int ty = std::clamp((int)(v * tex->height), 0, tex->height - 1);
                    uint32_t tex_pixel = tex->pixels[ty * tex->width + tx];

                    uint32_t tr = tex_pixel & 0xFF;
                    uint32_t tg = (tex_pixel >> 8) & 0xFF;
                    uint32_t tb = (tex_pixel >> 16) & 0xFF;
                    uint32_t ta = (tex_pixel >> 24) & 0xFF;

                    uint32_t mr = (r * tr) / 255;
                    uint32_t mg = (g * tg) / 255;
                    uint32_t mb = (b * tb) / 255;
                    uint32_t ma = (a * ta) / 255;

                    uint32_t modulated = (ma << 24) | (mb << 16) | (mg << 8) | mr;
                    row[x] = blend_bgra(row[x], modulated, ma);
                } else {
                    row[x] = blend_bgra(row[x], col, a);
                }
            }
        }
    }
}

// Fast-path for axis-aligned textured or untextured quads
static bool try_draw_fast_quad(uint32_t* fb, int screen_w, int screen_h,
                               const Rml::Vertex& v0, const Rml::Vertex& v1,
                               const Rml::Vertex& v2, const Rml::Vertex& v3,
                               const RmlTexture* tex, const Rml::Vector2f& offset,
                               int clip_x0, int clip_y0, int clip_x1, int clip_y1)
{
    float x0 = v0.position.x + offset.x, y0 = v0.position.y + offset.y;
    float x1 = v1.position.x + offset.x, y1 = v1.position.y + offset.y;
    float x2 = v2.position.x + offset.x, y2 = v2.position.y + offset.y;
    float x3 = v3.position.x + offset.x, y3 = v3.position.y + offset.y;

    // Check if quad is axis-aligned: (x0,y0)-(x1,y0)-(x2,y2)-(x0,y2)
    if (std::abs(y0 - y1) > 0.05f || std::abs(x1 - x2) > 0.05f ||
        std::abs(y2 - y3) > 0.05f || std::abs(x3 - x0) > 0.05f)
    {
        return false;
    }

    int rx0 = std::max(clip_x0, (int)std::floor(x0));
    int ry0 = std::max(clip_y0, (int)std::floor(y0));
    int rx1 = std::min(clip_x1, (int)std::ceil(x1));
    int ry1 = std::min(clip_y1, (int)std::ceil(y2));

    if (rx0 >= rx1 || ry0 >= ry1) return true;

    float qw = x1 - x0;
    float qh = y2 - y0;
    if (qw <= 0.001f || qh <= 0.001f) return true;

    uint32_t r = v0.colour.red;
    uint32_t g = v0.colour.green;
    uint32_t b = v0.colour.blue;
    uint32_t a = v0.colour.alpha;

    if (!tex || tex->width <= 0 || tex->height <= 0) {
        uint32_t col = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
        for (int y = ry0; y < ry1; y++) {
            uint32_t* row = &fb[y * screen_w];
            for (int x = rx0; x < rx1; x++) {
                row[x] = blend_bgra(row[x], col, a);
            }
        }
        return true;
    }

    // Textured quad (Font glyphs or icons)
    float u0 = v0.tex_coord.x, v_t0 = v0.tex_coord.y;
    float u1 = v1.tex_coord.x, v_t1 = v2.tex_coord.y;
    float min_u = std::min(u0, u1), max_u = std::max(u0, u1);
    float min_v = std::min(v_t0, v_t1), max_v = std::max(v_t0, v_t1);

    float du_dx = (u1 - u0) / qw;
    float dv_dy = (v_t1 - v_t0) / qh;

    for (int y = ry0; y < ry1; y++) {
        uint32_t* row = &fb[y * screen_w];
        float cur_v = v_t0 + (y + 0.5f - y0) * dv_dy;
        cur_v = std::clamp(cur_v, min_v, max_v);
        int ty = std::clamp((int)(cur_v * tex->height), 0, tex->height - 1);
        const uint32_t* tex_row = &tex->pixels[ty * tex->width];

        float cur_u = u0 + (rx0 + 0.5f - x0) * du_dx;

        for (int x = rx0; x < rx1; x++) {
            float clamped_u = std::clamp(cur_u, min_u, max_u);
            int tx = std::clamp((int)(clamped_u * tex->width), 0, tex->width - 1);
            cur_u += du_dx;

            uint32_t tex_pixel = tex_row[tx];
            uint32_t ta = (tex_pixel >> 24) & 0xFF;
            if (ta == 0) continue;

            uint32_t tr = tex_pixel & 0xFF;
            uint32_t tg = (tex_pixel >> 8) & 0xFF;
            uint32_t tb = (tex_pixel >> 16) & 0xFF;

            uint32_t mr = (r * tr) / 255;
            uint32_t mg = (g * tg) / 255;
            uint32_t mb = (b * tb) / 255;
            uint32_t ma = (a * ta) / 255;

            uint32_t modulated = (ma << 24) | (mb << 16) | (mg << 8) | mr;
            row[x] = blend_bgra(row[x], modulated, ma);
        }
    }

    return true;
}

void EvoRenderInterface::RenderGeometry(Rml::CompiledGeometryHandle geometry,
                                        Rml::Vector2f translation,
                                        Rml::TextureHandle texture)
{
    if (!m_fb || !geometry) return;

    RmlCompiledGeo* geo = reinterpret_cast<RmlCompiledGeo*>(geometry);
    if (geo->vertices.empty() || geo->indices.empty()) return;

    int clip_x0 = 0, clip_y0 = 0, clip_x1 = m_width, clip_y1 = m_height;
    if (m_scissor_enabled) {
        clip_x0 = std::max(0, m_scissor_region.Left());
        clip_y0 = std::max(0, m_scissor_region.Top());
        clip_x1 = std::min(m_width, m_scissor_region.Right());
        clip_y1 = std::min(m_height, m_scissor_region.Bottom());
    }

    RmlTexture* tex = reinterpret_cast<RmlTexture*>(texture);

    // Fast-path for standard 2-triangle quads: (0, 1, 2) + (0, 2, 3)
    if (geo->indices.size() == 6 && geo->vertices.size() >= 4) {
        int i0 = geo->indices[0], i1 = geo->indices[1], i2 = geo->indices[2];
        int i3 = geo->indices[3], i4 = geo->indices[4], i5 = geo->indices[5];

        if (i0 == 0 && i1 == 1 && i2 == 2 && i3 == 0 && i4 == 2 && i5 == 3) {
            if (try_draw_fast_quad(m_fb, m_width, m_height,
                                   geo->vertices[0], geo->vertices[1],
                                   geo->vertices[2], geo->vertices[3],
                                   tex, translation,
                                   clip_x0, clip_y0, clip_x1, clip_y1))
            {
                return;
            }
        }
    }

    for (size_t i = 0; i + 2 < geo->indices.size(); i += 3) {
        int i0 = geo->indices[i + 0];
        int i1 = geo->indices[i + 1];
        int i2 = geo->indices[i + 2];

        if (i0 < (int)geo->vertices.size() &&
            i1 < (int)geo->vertices.size() &&
            i2 < (int)geo->vertices.size())
        {
            draw_triangle(m_fb, m_width, m_height,
                          geo->vertices[i0], geo->vertices[i1], geo->vertices[i2],
                          tex, translation,
                          clip_x0, clip_y0, clip_x1, clip_y1);
        }
    }
}
