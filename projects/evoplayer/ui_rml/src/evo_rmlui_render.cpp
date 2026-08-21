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

/*
 * Composite a premultiplied source over the PS5 framebuffer (0xAABBGGRR).
 *
 * RmlUi's whole pipeline is premultiplied: vertex colours arrive as
 * `ColourbPremultiplied`, the font engine says as much where it builds the
 * glyph atlas ("we use premultiplied alpha, so copy the alpha into all four
 * channels"), and its own GL backend composites with
 * `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)` over `fragColor * texColor`.
 *
 * What was here instead was the textbook straight-alpha blend,
 * `src*a + dst*(1-a)`, which multiplies a colour that already carries its
 * alpha by that alpha a second time. At full opacity the two agree, so most
 * of the UI looked right and the bug stayed hidden; everywhere else it scales
 * the source towards black in proportion to how transparent it is. A 2/3
 * opacity border (#2a3b55aa) landed at green 35 instead of 48, and - because
 * an antialiased edge pixel is just a low-alpha source - every soft edge in
 * the UI was darkened by the square of its own coverage. That is a large part
 * of why curves read as thin and broken rather than smooth.
 */
static inline uint32_t blend_premul(uint32_t dst, uint32_t src, uint32_t a) {
    if (a == 0) return dst;
    if (a >= 255) return src | 0xFF000000;

    uint32_t inv = 255 - a;
    uint32_t r = ( src        & 0xFF) + (( dst        & 0xFF) * inv + 127) / 255;
    uint32_t g = ((src >>  8) & 0xFF) + (((dst >>  8) & 0xFF) * inv + 127) / 255;
    uint32_t b = ((src >> 16) & 0xFF) + (((dst >> 16) & 0xFF) * inv + 127) / 255;

    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;

    return 0xFF000000 | (b << 16) | (g << 8) | r;
}

/*
 * A read-only view of the active clip mask, handed to each drawing path.
 *
 * Null means nothing is clipped. When it is set, callers have already narrowed
 * their scissor to the mask's box, so every lookup lands in range.
 */
struct ClipMask {
    const uint8_t* pixels;
    int stride;
    int x0, y0, x1, y1;

    inline uint32_t at(int x, int y) const {
        if (x < x0 || x >= x1 || y < y0 || y >= y1) return 0;
        return pixels[(size_t)y * stride + x];
    }
};

/*
 * Scale a premultiplied source by the mask. False means the pixel is clipped
 * away entirely and should be skipped.
 *
 * All four channels scale together: in a premultiplied pixel the colour
 * already carries the alpha, so fading one without the others would change
 * the colour rather than the opacity.
 */
static inline bool apply_clip(const ClipMask* clip, int x, int y,
                              uint32_t& r, uint32_t& g, uint32_t& b, uint32_t& a)
{
    if (!clip) return a != 0;

    uint32_t m = clip->at(x, y);
    if (m == 0) return false;
    if (m < 255) {
        r = (r * m + 127) / 255;
        g = (g * m + 127) / 255;
        b = (b * m + 127) / 255;
        a = (a * m + 127) / 255;
    }
    return a != 0;
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

void EvoRenderInterface::SetMemoryTexture(const std::string& key, const uint32_t* bgra,
                                          int w, int h)
{
    if (!bgra || w <= 0 || h <= 0) {
        m_mem_textures.erase(key);
        return;
    }

    MemImage& img = m_mem_textures[key];
    img.width = w;
    img.height = h;
    img.pixels.assign(bgra, bgra + (size_t)w * (size_t)h);

    /* Same premultiplied convention as LoadTexture. Decoded artwork is opaque
     * in practice, so this is usually a no-op, but a poster that ever arrives
     * with real alpha would otherwise bloom the same way the icons did. */
    for (uint32_t& px : img.pixels) {
        uint32_t a = (px >> 24) & 0xFF;
        if (a == 255) continue;
        uint32_t r = px & 0xFF, g = (px >> 8) & 0xFF, b = (px >> 16) & 0xFF;
        r = (r * a + 127) / 255;
        g = (g * a + 127) / 255;
        b = (b * a + 127) / 255;
        px = (a << 24) | (b << 16) | (g << 8) | r;
    }
}

void EvoRenderInterface::DropMemoryTexture(const std::string& key)
{
    m_mem_textures.erase(key);
}

Rml::TextureHandle EvoRenderInterface::LoadTexture(Rml::Vector2i& texture_dimensions,
                                                  const Rml::String& source)
{
    /* Runtime artwork: served from the memory registry, never the disk. */
    if (source.compare(0, 8, "evo:mem/") == 0) {
        auto it = m_mem_textures.find(std::string(source));
        if (it == m_mem_textures.end()) {
            texture_dimensions = Rml::Vector2i(0, 0);
            return 0;
        }

        texture_dimensions = Rml::Vector2i(it->second.width, it->second.height);
        RmlTexture* tex = new RmlTexture();
        tex->width = it->second.width;
        tex->height = it->second.height;
        tex->pixels = it->second.pixels;
        return reinterpret_cast<Rml::TextureHandle>(tex);
    }

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

    /*
     * Premultiply on the way in. Everything downstream - the modulate against
     * the vertex colour, the coverage accumulator, blend_premul - works in
     * RmlUi's premultiplied convention, and the font atlas already arrives
     * that way. A PNG on disk does not: these icons carry a constant
     * (0, 205, 255) with only the alpha ramping across an edge, so composited
     * as if premultiplied every antialiased edge pixel painted full-strength
     * cyan instead of its share of it. The icons bloomed outward by a pixel
     * all round and the fine detail filled in - a gear read as a blob.
     */
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

        tex->pixels[i] = (a << 24) | (b << 16) | (g << 8) | r;
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

/* ===================================================================== *
 *  Texture sampling
 * ===================================================================== */

static inline uint32_t sample_bilinear(const RmlTexture* tex, float u, float v)
{
    float fx = u * (float)tex->width  - 0.5f;
    float fy = v * (float)tex->height - 0.5f;
    int ix0 = (int)std::floor(fx);
    int iy0 = (int)std::floor(fy);
    float fxf = fx - (float)ix0;
    float fyf = fy - (float)iy0;

    int ix1 = std::clamp(ix0 + 1, 0, tex->width  - 1);
    int iy1 = std::clamp(iy0 + 1, 0, tex->height - 1);
    ix0 = std::clamp(ix0, 0, tex->width  - 1);
    iy0 = std::clamp(iy0, 0, tex->height - 1);

    uint32_t p00 = tex->pixels[(size_t)iy0 * tex->width + ix0];
    uint32_t p10 = tex->pixels[(size_t)iy0 * tex->width + ix1];
    uint32_t p01 = tex->pixels[(size_t)iy1 * tex->width + ix0];
    uint32_t p11 = tex->pixels[(size_t)iy1 * tex->width + ix1];

    float if0 = 1.0f - fxf, if1 = fxf;
    float vf0 = 1.0f - fyf, vf1 = fyf;

    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        float c00 = (float)((p00 >> shift) & 0xFF);
        float c10 = (float)((p10 >> shift) & 0xFF);
        float c01 = (float)((p01 >> shift) & 0xFF);
        float c11 = (float)((p11 >> shift) & 0xFF);
        float c = (c00 * if0 + c10 * if1) * vf0
                + (c01 * if0 + c11 * if1) * vf1;
        out |= (uint32_t)std::clamp(c + 0.5f, 0.0f, 255.0f) << shift;
    }
    return out;
}

/*
 * Area average over the destination pixel's real footprint in the texture.
 *
 * Minification cannot be done with a fixed number of taps. The version this
 * replaces took round(texels) of them, centred by integer division, which at
 * the ratio the navbar icons are actually drawn at - 72px artwork in a 34px
 * box, 2.118 texels to the pixel - read two texels out of every 2.118 and
 * stepped straight over the remainder. Four texels in every row were never
 * read at all. On a photo that is invisible; on a gear or a star it is whole
 * teeth and whole limbs dropping out, and the icons came back shredded rather
 * than merely soft.
 *
 * Weighting each texel by how much of it the footprint covers is both correct
 * and self-scaling: nothing in range is skipped, and the partial texels at
 * the edges contribute in proportion to their overlap.
 */
static inline uint32_t sample_box(const RmlTexture* tex,
                                  float cx, float cy, float hx, float hy)
{
    float x_lo = cx - hx, x_hi = cx + hx;
    float y_lo = cy - hy, y_hi = cy + hy;

    int ix_lo = (int)std::floor(x_lo), ix_hi = (int)std::ceil(x_hi) - 1;
    int iy_lo = (int)std::floor(y_lo), iy_hi = (int)std::ceil(y_hi) - 1;
    if (ix_hi < ix_lo) ix_hi = ix_lo;
    if (iy_hi < iy_lo) iy_hi = iy_lo;

    float ar = 0.0f, ag = 0.0f, ab = 0.0f, aa = 0.0f, wsum = 0.0f;

    for (int sy = iy_lo; sy <= iy_hi; sy++) {
        float wy = std::min((float)sy + 1.0f, y_hi) - std::max((float)sy, y_lo);
        if (wy <= 0.0f) continue;

        const uint32_t* trow =
            &tex->pixels[(size_t)std::clamp(sy, 0, tex->height - 1) * tex->width];

        for (int sx = ix_lo; sx <= ix_hi; sx++) {
            float wx = std::min((float)sx + 1.0f, x_hi) - std::max((float)sx, x_lo);
            if (wx <= 0.0f) continue;

            float w = wx * wy;
            uint32_t px = trow[std::clamp(sx, 0, tex->width - 1)];
            ar += w * (float)( px        & 0xFF);
            ag += w * (float)((px >>  8) & 0xFF);
            ab += w * (float)((px >> 16) & 0xFF);
            aa += w * (float)((px >> 24) & 0xFF);
            wsum += w;
        }
    }

    if (wsum <= 0.0f) return 0;

    float inv = 1.0f / wsum;
    uint32_t out = 0;
    out |= (uint32_t)std::clamp(ar * inv + 0.5f, 0.0f, 255.0f);
    out |= (uint32_t)std::clamp(ag * inv + 0.5f, 0.0f, 255.0f) << 8;
    out |= (uint32_t)std::clamp(ab * inv + 0.5f, 0.0f, 255.0f) << 16;
    out |= (uint32_t)std::clamp(aa * inv + 0.5f, 0.0f, 255.0f) << 24;
    return out;
}

static inline float catmull_rom(float p0, float p1, float p2, float p3, float t)
{
    return 0.5f * ((2.0f * p1)
                 + (-p0 + p2) * t
                 + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t
                 + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t * t * t);
}

/*
 * Catmull-Rom, for magnification only.
 *
 * Bilinear between two texels is a straight ramp, so every pixel that lands
 * between them is a mixture of both and nothing in the enlarged image is ever
 * as sharp as the texel it came from. That is what makes the hero still look
 * soft: it is 960x540 artwork stretched across a 1698x312 card, so most
 * destination pixels are mixtures and none of them is a texel. Catmull-Rom
 * fits a curve through four texels instead of a line through two, restoring
 * the slope at the texel centres and with it the edges in the picture.
 *
 * It can overshoot either side of the texels it interpolates - that overshoot
 * is the sharpening - so every channel is clamped back into range.
 */
static inline uint32_t sample_bicubic(const RmlTexture* tex, float u, float v)
{
    float fx = u * (float)tex->width  - 0.5f;
    float fy = v * (float)tex->height - 0.5f;
    int ix = (int)std::floor(fx);
    int iy = (int)std::floor(fy);
    float tx = fx - (float)ix;
    float ty = fy - (float)iy;

    uint32_t px[4][4];
    for (int j = 0; j < 4; j++) {
        int sy = std::clamp(iy - 1 + j, 0, tex->height - 1);
        const uint32_t* trow = &tex->pixels[(size_t)sy * tex->width];
        for (int i = 0; i < 4; i++) {
            int sx = std::clamp(ix - 1 + i, 0, tex->width - 1);
            px[j][i] = trow[sx];
        }
    }

    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        float col[4];
        for (int j = 0; j < 4; j++) {
            col[j] = catmull_rom((float)((px[j][0] >> shift) & 0xFF),
                                 (float)((px[j][1] >> shift) & 0xFF),
                                 (float)((px[j][2] >> shift) & 0xFF),
                                 (float)((px[j][3] >> shift) & 0xFF), tx);
        }
        float c = catmull_rom(col[0], col[1], col[2], col[3], ty);
        out |= (uint32_t)std::clamp(c + 0.5f, 0.0f, 255.0f) << shift;
    }
    return out;
}

/* ===================================================================== *
 *  Coverage-accumulating rasterizer
 * ===================================================================== */

/*
 * Per-pixel sums for one draw call.
 *
 * A rounded corner, an arc, a gradient - anything that is not a plain
 * rectangle - arrives as a fan or strip of triangles that together tile one
 * shape. Blending each triangle into the framebuffer on its own cannot rebuild
 * that shape. Along an edge two triangles share, each covers part of the
 * pixel, and two partial blends do not compose back into one whole one:
 * antialias both sides and the seam shows as a spoke, antialias neither and
 * the outer silhouette goes hard. The previous code picked per edge which of
 * the two to accept, and had to exclude axis-aligned edges on top of that to
 * stop glyph quads being softened a second time.
 *
 * That compromise is what broke curves. Where a rounded border runs diagonally
 * its segments are shortest and its triangles are slivers, and a sliver whose
 * shared edge is hard-tested loses the pixels nearest that edge entirely - the
 * 2px outline measured 205 down its straight runs and a single pixel of 52
 * across the diagonal, which is the outline visibly coming apart.
 *
 * Summing coverage first and blending once fixes both halves. Two triangles
 * meeting on a shared edge contribute f and 1-f of a pixel, which adds back to
 * the whole pixel the shape really covers, so every edge can carry the
 * antialiasing falloff and none of them needs a special case.
 */
struct CovPixel {
    float w;        /* coverage                              */
    float a;        /* coverage * alpha                      */
    float r, g, b;  /* coverage * premultiplied colour       */
};

enum class CovTarget {
    Framebuffer,
    MaskSet,
    MaskInverse,
    MaskIntersect,
};

/*
 * Accumulator state. The buffer is left zeroed by whichever pass consumes it,
 * so it is reused across draw calls and frames and never cleared wholesale:
 * only the pixels a triangle actually wrote get revisited, tracked as one span
 * per row. Without that, a rounded border would cost a clear and a sweep of
 * its whole 1698x312 bounding box every frame just to draw a ring around the
 * edge of it.
 *
 * Rendering is driven from a single thread, so file-scope state is safe here.
 */
static std::vector<CovPixel> g_cov;
static std::vector<int> g_cov_lo;
static std::vector<int> g_cov_hi;

static void accumulate_triangle(int acc_w, int acc_x0, int acc_y0,
                                const Rml::Vertex& v0, const Rml::Vertex& v1, const Rml::Vertex& v2,
                                const RmlTexture* tex, const Rml::Vector2f& offset,
                                int clip_x0, int clip_y0, int clip_x1, int clip_y1)
{
    float x0 = v0.position.x + offset.x, y0 = v0.position.y + offset.y;
    float x1 = v1.position.x + offset.x, y1 = v1.position.y + offset.y;
    float x2 = v2.position.x + offset.x, y2 = v2.position.y + offset.y;

    /* One pixel of margin: the falloff below reaches half a pixel outside the
     * triangle, so the pixels just past each edge still take coverage. */
    int min_x = std::max(clip_x0,     (int)std::floor(std::min({x0, x1, x2})) - 1);
    int max_x = std::min(clip_x1 - 1, (int)std::ceil (std::max({x0, x1, x2})) + 1);
    int min_y = std::max(clip_y0,     (int)std::floor(std::min({y0, y1, y2})) - 1);
    int max_y = std::min(clip_y1 - 1, (int)std::ceil (std::max({y0, y1, y2})) + 1);

    if (min_x > max_x || min_y > max_y) return;

    float denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (std::abs(denom) < 0.0001f) return;
    float inv_denom = 1.0f / denom;

    /* Screen-space gradient of each edge function, in w-units per pixel -
     * dividing a w value by its own edge's gradient converts it into a signed
     * distance in pixels from that edge. */
    float dw0dx = (y1 - y2) * inv_denom, dw0dy = (x2 - x1) * inv_denom;
    float dw1dx = (y2 - y0) * inv_denom, dw1dy = (x0 - x2) * inv_denom;
    float dw2dx = -(dw0dx + dw1dx),      dw2dy = -(dw0dy + dw1dy);
    float grad0 = std::max(1e-6f, std::sqrt(dw0dx * dw0dx + dw0dy * dw0dy));
    float grad1 = std::max(1e-6f, std::sqrt(dw1dx * dw1dx + dw1dy * dw1dy));
    float grad2 = std::max(1e-6f, std::sqrt(dw2dx * dw2dx + dw2dy * dw2dy));

    for (int y = min_y; y <= max_y; y++) {
        float py = y + 0.5f;
        int row = y - acc_y0;
        CovPixel* arow = &g_cov[(size_t)row * acc_w];
        int touched_lo = g_cov_lo[row];
        int touched_hi = g_cov_hi[row];

        for (int x = min_x; x <= max_x; x++) {
            float px = x + 0.5f;

            float w0 = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) * inv_denom;
            float w1 = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) * inv_denom;
            float w2 = 1.0f - w0 - w1;

            /* How much of this pixel each edge leaves inside, from its signed
             * distance; the tightest of the three is the triangle's share. */
            float c0 = std::clamp(0.5f + w0 / grad0, 0.0f, 1.0f);
            float c1 = std::clamp(0.5f + w1 / grad1, 0.0f, 1.0f);
            float c2 = std::clamp(0.5f + w2 / grad2, 0.0f, 1.0f);
            float cov = std::min(c0, std::min(c1, c2));
            if (cov <= 0.001f) continue;

            /* Attributes are wanted at the pixel centre even where that centre
             * sits just outside the triangle, and a raw barycentric there
             * extrapolates past the vertices it interpolates between - on a
             * gradient that reads as a bright fringe along the edge. Clamping
             * first keeps the sample inside the triangle's own range. */
            float b0 = std::clamp(w0, 0.0f, 1.0f);
            float b1 = std::clamp(w1, 0.0f, 1.0f);
            float b2 = std::clamp(w2, 0.0f, 1.0f);
            float bsum = b0 + b1 + b2;
            if (bsum <= 0.0f) continue;
            float inv_b = 1.0f / bsum;
            b0 *= inv_b; b1 *= inv_b; b2 *= inv_b;

            float cr = b0 * v0.colour.red   + b1 * v1.colour.red   + b2 * v2.colour.red;
            float cg = b0 * v0.colour.green + b1 * v1.colour.green + b2 * v2.colour.green;
            float cb = b0 * v0.colour.blue  + b1 * v1.colour.blue  + b2 * v2.colour.blue;
            float ca = b0 * v0.colour.alpha + b1 * v1.colour.alpha + b2 * v2.colour.alpha;

            if (tex && tex->width > 0 && tex->height > 0) {
                float u = b0 * v0.tex_coord.x + b1 * v1.tex_coord.x + b2 * v2.tex_coord.x;
                float v = b0 * v0.tex_coord.y + b1 * v1.tex_coord.y + b2 * v2.tex_coord.y;
                uint32_t t = sample_bilinear(tex, u, v);
                cr = cr * (float)( t        & 0xFF) * (1.0f / 255.0f);
                cg = cg * (float)((t >>  8) & 0xFF) * (1.0f / 255.0f);
                cb = cb * (float)((t >> 16) & 0xFF) * (1.0f / 255.0f);
                ca = ca * (float)((t >> 24) & 0xFF) * (1.0f / 255.0f);
            }

            /* Premultiplied compositing is linear, so the contributions of
             * triangles that tile a pixel add directly - no normalising by
             * the accumulated alpha afterwards, which is what the straight
             * -alpha form needed. */
            CovPixel& p = arow[x - acc_x0];
            p.w += cov;
            p.a += cov * ca;
            p.r += cov * cr;
            p.g += cov * cg;
            p.b += cov * cb;

            if (x < touched_lo) touched_lo = x;
            if (x > touched_hi) touched_hi = x;
        }

        g_cov_lo[row] = touched_lo;
        g_cov_hi[row] = touched_hi;
    }
}

/*
 * Drain the accumulator: blend each touched pixel into the framebuffer exactly
 * once, or fold its coverage into the clip mask, and leave it zeroed behind us
 * for the next draw call.
 *
 * A mask has to visit every pixel of its region rather than only the touched
 * spans, because a pixel the mask geometry misses is one the mask must clear.
 */
static void emit_coverage(CovTarget target, uint32_t* fb, uint8_t* mask, int screen_w,
                          int acc_x0, int acc_y0, int acc_w, int acc_h,
                          const ClipMask* clip)
{
    if (target == CovTarget::Framebuffer) {
        for (int j = 0; j < acc_h; j++) {
            int lo = g_cov_lo[j];
            int hi = g_cov_hi[j];
            if (lo > hi) continue;

            CovPixel* arow = &g_cov[(size_t)j * acc_w];
            uint32_t* row = &fb[(size_t)(acc_y0 + j) * screen_w];

            for (int x = lo; x <= hi; x++) {
                CovPixel& p = arow[x - acc_x0];
                if (p.w > 0.0f && p.a > 0.0f) {
                    /* Triangles that overlap within one draw call can push the
                     * sum past a whole pixel; a pixel is still one pixel, so
                     * scale the whole premultiplied quad back down together. */
                    float s = std::min(p.w, 1.0f) / p.w;

                    uint32_t a = (uint32_t)std::clamp((int)(p.a * s + 0.5f), 0, 255);
                    uint32_t r = (uint32_t)std::clamp((int)(p.r * s + 0.5f), 0, 255);
                    uint32_t g = (uint32_t)std::clamp((int)(p.g * s + 0.5f), 0, 255);
                    uint32_t b = (uint32_t)std::clamp((int)(p.b * s + 0.5f), 0, 255);

                    if (apply_clip(clip, x, acc_y0 + j, r, g, b, a)) {
                        uint32_t col = (a << 24) | (b << 16) | (g << 8) | r;
                        row[x] = blend_premul(row[x], col, a);
                    }
                }
                p = CovPixel{};
            }
        }
        return;
    }

    for (int j = 0; j < acc_h; j++) {
        CovPixel* arow = &g_cov[(size_t)j * acc_w];
        uint8_t* mrow = &mask[(size_t)(acc_y0 + j) * screen_w + acc_x0];

        for (int i = 0; i < acc_w; i++) {
            CovPixel& p = arow[i];
            uint32_t cov = 0;
            if (p.w > 0.0f) {
                /* The mask records the shape, not what colour filled it. */
                cov = (uint32_t)std::clamp((int)(std::min(p.w, 1.0f) * 255.0f + 0.5f), 0, 255);
                p = CovPixel{};
            }

            switch (target) {
            case CovTarget::MaskSet:       mrow[i] = (uint8_t)cov; break;
            case CovTarget::MaskInverse:   mrow[i] = (uint8_t)(255 - cov); break;
            case CovTarget::MaskIntersect: mrow[i] = (uint8_t)((mrow[i] * cov + 127) / 255); break;
            default: break;
            }
        }
    }
}

/*
 * Rasterize a run of triangles through the accumulator, in horizontal strips so
 * the scratch stays bounded by the strip height rather than by the tallest
 * element on screen.
 */
static void render_triangles_accumulated(CovTarget target, uint32_t* fb, uint8_t* mask,
                                         int screen_w,
                                         const RmlCompiledGeo* geo,
                                         const int* tri_idx, size_t idx_count,
                                         const RmlTexture* tex,
                                         Rml::Vector2f translation,
                                         int bx0, int by0, int bx1, int by1,
                                         const ClipMask* clip)
{
    if (idx_count < 3 || bx0 > bx1 || by0 > by1) return;

    const int acc_w = bx1 - bx0 + 1;
    const int STRIP = 64;

    for (int sy0 = by0; sy0 <= by1; sy0 += STRIP) {
        int sy1 = std::min(by1, sy0 + STRIP - 1);
        int acc_h = sy1 - sy0 + 1;

        size_t need = (size_t)acc_w * acc_h;
        if (g_cov.size() < need) g_cov.resize(need);      /* grows zeroed */
        if ((int)g_cov_lo.size() < acc_h) { g_cov_lo.resize(acc_h); g_cov_hi.resize(acc_h); }
        for (int j = 0; j < acc_h; j++) { g_cov_lo[j] = bx1 + 1; g_cov_hi[j] = bx0 - 1; }

        for (size_t i = 0; i + 2 < idx_count; i += 3) {
            accumulate_triangle(acc_w, bx0, sy0,
                                geo->vertices[tri_idx[i + 0]],
                                geo->vertices[tri_idx[i + 1]],
                                geo->vertices[tri_idx[i + 2]],
                                tex, translation,
                                bx0, sy0, bx1 + 1, sy1 + 1);
        }

        emit_coverage(target, fb, mask, screen_w, bx0, sy0, acc_w, acc_h, clip);
    }
}

/* Screen-space bounds of the vertices a run of indices refers to, grown by the
 * one pixel of antialiasing margin and clipped to the given box. False if
 * nothing is left. */
static bool geometry_bounds(const RmlCompiledGeo* geo, const int* tri_idx, size_t idx_count,
                            Rml::Vector2f translation,
                            int clip_x0, int clip_y0, int clip_x1, int clip_y1,
                            int& bx0, int& by0, int& bx1, int& by1)
{
    float fx0 = 1e30f, fy0 = 1e30f, fx1 = -1e30f, fy1 = -1e30f;
    for (size_t i = 0; i < idx_count; i++) {
        int vi = tri_idx[i];
        if (vi < 0 || vi >= (int)geo->vertices.size()) return false;
        const Rml::Vertex& v = geo->vertices[vi];
        fx0 = std::min(fx0, v.position.x); fx1 = std::max(fx1, v.position.x);
        fy0 = std::min(fy0, v.position.y); fy1 = std::max(fy1, v.position.y);
    }

    bx0 = std::max(clip_x0,     (int)std::floor(fx0 + translation.x) - 1);
    by0 = std::max(clip_y0,     (int)std::floor(fy0 + translation.y) - 1);
    bx1 = std::min(clip_x1 - 1, (int)std::ceil (fx1 + translation.x) + 1);
    by1 = std::min(clip_y1 - 1, (int)std::ceil (fy1 + translation.y) + 1);
    return bx0 <= bx1 && by0 <= by1;
}

/* ===================================================================== *
 *  Clip mask
 * ===================================================================== */

void EvoRenderInterface::EnableClipMask(bool enable)
{
    m_clip_enabled = enable;
}

void EvoRenderInterface::RenderToClipMask(Rml::ClipMaskOperation operation,
                                          Rml::CompiledGeometryHandle geometry,
                                          Rml::Vector2f translation)
{
    if (!geometry || m_width <= 0 || m_height <= 0) return;

    RmlCompiledGeo* geo = reinterpret_cast<RmlCompiledGeo*>(geometry);
    if (geo->vertices.empty() || geo->indices.size() < 3) return;

    if (m_clip_mask.size() != (size_t)m_width * (size_t)m_height)
        m_clip_mask.assign((size_t)m_width * (size_t)m_height, 0);

    int gx0, gy0, gx1, gy1;
    if (!geometry_bounds(geo, geo->indices.data(), geo->indices.size(), translation,
                         0, 0, m_width, m_height, gx0, gy0, gx1, gy1))
    {
        m_mask_x0 = m_mask_y0 = m_mask_x1 = m_mask_y1 = 0;
        return;
    }

    /*
     * Where the mask ends up meaning anything. Set and Intersect both write
     * every pixel of that region, so nothing has to be cleared first: outside
     * it the mask reads as zero by definition. SetInverse is the exception -
     * its shape is the whole screen with a hole in it - and pays for a
     * full-screen pass, which is why it is worth not being the common case.
     */
    int nx0, ny0, nx1, ny1;
    CovTarget target;

    switch (operation) {
    case Rml::ClipMaskOperation::Set:
        nx0 = gx0; ny0 = gy0; nx1 = gx1; ny1 = gy1;
        target = CovTarget::MaskSet;
        break;

    case Rml::ClipMaskOperation::Intersect:
        /* Intersecting with nothing would clip everything away, and a mask
         * that hides the whole UI is a far worse failure than one that is
         * momentarily too permissive. RmlUi only intersects after a set, so
         * this is a guard rather than a path that is expected to be taken. */
        if (m_mask_x0 >= m_mask_x1 || m_mask_y0 >= m_mask_y1) {
            nx0 = gx0; ny0 = gy0; nx1 = gx1; ny1 = gy1;
            target = CovTarget::MaskSet;
            break;
        }
        nx0 = std::max(m_mask_x0, gx0);     ny0 = std::max(m_mask_y0, gy0);
        nx1 = std::min(m_mask_x1 - 1, gx1); ny1 = std::min(m_mask_y1 - 1, gy1);
        target = CovTarget::MaskIntersect;
        break;

    case Rml::ClipMaskOperation::SetInverse:
    default:
        nx0 = 0; ny0 = 0; nx1 = m_width - 1; ny1 = m_height - 1;
        target = CovTarget::MaskInverse;
        break;
    }

    if (nx0 > nx1 || ny0 > ny1) {
        m_mask_x0 = m_mask_y0 = m_mask_x1 = m_mask_y1 = 0;
        return;
    }

    render_triangles_accumulated(target, nullptr, m_clip_mask.data(), m_width,
                                 geo, geo->indices.data(), geo->indices.size(),
                                 nullptr, translation,
                                 nx0, ny0, nx1, ny1, nullptr);

    m_mask_x0 = nx0; m_mask_y0 = ny0;
    m_mask_x1 = nx1 + 1; m_mask_y1 = ny1 + 1;
}

/* ===================================================================== *
 *  Axis-aligned quad blitter
 * ===================================================================== */

// Fast-path for axis-aligned textured or untextured quads
static bool try_draw_fast_quad(uint32_t* fb, int screen_w, int screen_h,
                               const Rml::Vertex& v0, const Rml::Vertex& v1,
                               const Rml::Vertex& v2, const Rml::Vertex& v3,
                               const RmlTexture* tex, const Rml::Vector2f& offset,
                               int clip_x0, int clip_y0, int clip_x1, int clip_y1,
                               const ClipMask* clip)
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

    /*
     * Gradient decorators are vertex-coloured geometry, not shaders, so they
     * arrive here as an ordinary axis-aligned quad whose four corners differ.
     * This path only ever reads v0's colour, which flattened every gradient to
     * its start colour. Hand those quads to the coverage rasterizer, which
     * interpolates; flat quads - the overwhelming majority - still take the
     * fast path.
     */
    auto same_colour = [](const auto& a_, const auto& b_) {
        return a_.red == b_.red && a_.green == b_.green &&
               a_.blue == b_.blue && a_.alpha == b_.alpha;
    };
    if (!same_colour(v0.colour, v1.colour) ||
        !same_colour(v0.colour, v2.colour) ||
        !same_colour(v0.colour, v3.colour))
    {
        return false;
    }

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
                uint32_t pr = r, pg = g, pb = b, pa = a;
                if (!apply_clip(clip, x, y, pr, pg, pb, pa)) continue;
                uint32_t pc = clip ? ((pa << 24) | (pb << 16) | (pg << 8) | pr) : col;
                row[x] = blend_premul(row[x], pc, pa);
            }
        }
        return true;
    }

    float u0 = v0.tex_coord.x, v_t0 = v0.tex_coord.y;
    float u1 = v1.tex_coord.x, v_t1 = v2.tex_coord.y;
    float min_u = std::min(u0, u1), max_u = std::max(u0, u1);
    float min_v = std::min(v_t0, v_t1), max_v = std::max(v_t0, v_t1);

    float du_dx = (u1 - u0) / qw;
    float dv_dy = (v_t1 - v_t0) / qh;

    float fw = (float)tex->width;
    float fh = (float)tex->height;

    const float texels_x = std::abs(du_dx) * fw;
    const float texels_y = std::abs(dv_dy) * fh;

    /*
     * Text, and any sprite drawn at its native size, is one texel per pixel on
     * whole-pixel boundaries. Filtering that is all cost and no benefit: the
     * taps collapse back onto the single texel that was wanted, except where
     * rounding lands them a fraction off and a thin stem picks up a neighbour
     * it should not have. Copying the texel straight through is what the old
     * SDF UI did with its atlas, and it is why that UI's thin text held its
     * edge where this one's did not.
     *
     * The alignment has to be tested rather than assumed: RmlUi lays glyphs out
     * on integer advances, but the element they sit in can land anywhere, and a
     * run that starts half a pixel over has to keep filtering or it would
     * jitter against its own box.
     */
    if (du_dx > 0.0f && dv_dy > 0.0f &&
        std::abs(texels_x - 1.0f) < 0.002f && std::abs(texels_y - 1.0f) < 0.002f)
    {
        float sxf = u0 * fw,  syf = v_t0 * fh;
        float rsx = std::floor(sxf + 0.5f), rsy = std::floor(syf + 0.5f);
        float rdx = std::floor(x0  + 0.5f), rdy = std::floor(y0  + 0.5f);

        if (std::abs(sxf - rsx) < 0.01f && std::abs(syf - rsy) < 0.01f &&
            std::abs(x0  - rdx) < 0.01f && std::abs(y0  - rdy) < 0.01f)
        {
            int sox = (int)rsx - (int)rdx;
            int soy = (int)rsy - (int)rdy;

            for (int y = ry0; y < ry1; y++) {
                int sy = y + soy;
                if (sy < 0 || sy >= tex->height) continue;
                const uint32_t* trow = &tex->pixels[(size_t)sy * tex->width];
                uint32_t* row = &fb[y * screen_w];

                for (int x = rx0; x < rx1; x++) {
                    int sx = x + sox;
                    if (sx < 0 || sx >= tex->width) continue;
                    uint32_t t = trow[sx];
                    uint32_t ta = (t >> 24) & 0xFF;
                    if (ta == 0) continue;

                    uint32_t ma = (a * ta) / 255;
                    uint32_t mr = (r * ( t        & 0xFF)) / 255;
                    uint32_t mg = (g * ((t >>  8) & 0xFF)) / 255;
                    uint32_t mb = (b * ((t >> 16) & 0xFF)) / 255;
                    if (!apply_clip(clip, x, y, mr, mg, mb, ma)) continue;

                    uint32_t modulated = (ma << 24) | (mb << 16) | (mg << 8) | mr;
                    row[x] = blend_premul(row[x], modulated, ma);
                }
            }
            return true;
        }
    }

    /*
     * Minification: average the footprint.
     *
     * The icons are 72x72 and the controller glyphs 48x48, drawn at 20-34 px.
     * Bilinear reads four texels out of the five or more each destination
     * pixel actually covers, so which four you land on shifts with subpixel
     * position and the edges break up - the jagged small elements visible on
     * hardware but not in a half-scale screenshot, which resamples them away.
     *
     * Averaging the real footprint costs a few texels per pixel on elements
     * that are small by definition, and nothing at all when magnifying.
     */
    if (texels_x > 1.05f || texels_y > 1.05f) {
        const float hx = std::clamp(texels_x * 0.5f, 0.5f, 8.0f);
        const float hy = std::clamp(texels_y * 0.5f, 0.5f, 8.0f);

        for (int y = ry0; y < ry1; y++) {
            uint32_t* row = &fb[y * screen_w];
            float cur_v = std::clamp(v_t0 + (y + 0.5f - y0) * dv_dy, min_v, max_v);
            float cy = cur_v * fh;
            float cur_u = u0 + (rx0 + 0.5f - x0) * du_dx;

            for (int x = rx0; x < rx1; x++) {
                float cu = std::clamp(cur_u, min_u, max_u);
                cur_u += du_dx;

                uint32_t t = sample_box(tex, cu * fw, cy, hx, hy);
                uint32_t ta = (t >> 24) & 0xFF;
                if (ta == 0) continue;

                uint32_t ma = (a * ta) / 255;
                uint32_t mr = (r * ( t        & 0xFF)) / 255;
                uint32_t mg = (g * ((t >>  8) & 0xFF)) / 255;
                uint32_t mb = (b * ((t >> 16) & 0xFF)) / 255;
                if (!apply_clip(clip, x, y, mr, mg, mb, ma)) continue;

                uint32_t modulated = (ma << 24) | (mb << 16) | (mg << 8) | mr;
                row[x] = blend_premul(row[x], modulated, ma);
            }
        }
        return true;
    }

    /*
     * Magnifying by enough to see it: bicubic. Below this the two filters agree
     * to within a rounding step and bilinear is cheaper.
     */
    const bool magnifying = (texels_x < 0.85f || texels_y < 0.85f);

    for (int y = ry0; y < ry1; y++) {
        uint32_t* row = &fb[y * screen_w];
        float cur_v = std::clamp(v_t0 + (y + 0.5f - y0) * dv_dy, min_v, max_v);
        float cur_u = u0 + (rx0 + 0.5f - x0) * du_dx;

        for (int x = rx0; x < rx1; x++) {
            float cu = std::clamp(cur_u, min_u, max_u);
            cur_u += du_dx;

            uint32_t t = magnifying ? sample_bicubic(tex, cu, cur_v)
                                    : sample_bilinear(tex, cu, cur_v);
            uint32_t ta = (t >> 24) & 0xFF;
            if (ta == 0) continue;

            uint32_t ma = (a * ta) / 255;
            uint32_t mr = (r * ( t        & 0xFF)) / 255;
            uint32_t mg = (g * ((t >>  8) & 0xFF)) / 255;
            uint32_t mb = (b * ((t >> 16) & 0xFF)) / 255;
            if (!apply_clip(clip, x, y, mr, mg, mb, ma)) continue;

            uint32_t modulated = (ma << 24) | (mb << 16) | (mg << 8) | mr;
            row[x] = blend_premul(row[x], modulated, ma);
        }
    }

    return true;
}

/*
 * RmlUi batches a whole text run - and any repeated sprite - into a single
 * geometry of independent quads: vertices in groups of four laid out
 * top-left, top-right, bottom-right, bottom-left, with six indices per group
 * referring only to that group.
 *
 * The test is deliberately on which vertices a group uses rather than on the
 * order it lists them in. The check this replaces looked for the winding
 * (0,1,2) (0,2,3); RmlUi's MeshUtilities::GenerateQuad actually emits
 * (0,3,1) (1,3,2), so nothing ever matched and the whole blitter below was
 * unreachable. Every string, icon and poster on screen went through the
 * general triangle rasterizer instead - text resampled where it should have
 * been copied, artwork bilinear where it should have been bicubic, and the
 * box filter meant to stop small icons breaking up had never once run. A
 * winding-agnostic test cannot fail that way again: two triangles over the
 * same four corners are the same quad whichever way round they are wound.
 */
static bool is_quad_batch(const RmlCompiledGeo* geo)
{
    size_t n = geo->indices.size();
    if (n == 0 || n % 6 != 0) return false;

    size_t quads = n / 6;
    if (geo->vertices.size() != quads * 4) return false;

    for (size_t q = 0; q < quads; q++) {
        const int* ix = &geo->indices[q * 6];
        int base = (int)(q * 4);
        bool used[4] = { false, false, false, false };

        for (int k = 0; k < 6; k++) {
            int rel = ix[k] - base;
            if (rel < 0 || rel > 3) return false;
            used[rel] = true;
        }
        if (!used[0] || !used[1] || !used[2] || !used[3]) return false;
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

    /* Outside the mask's box everything is clipped away, so fold the box into
     * the scissor and never visit those pixels at all. */
    ClipMask mask_view;
    const ClipMask* clip = nullptr;
    if (m_clip_enabled && m_clip_mask.size() == (size_t)m_width * (size_t)m_height &&
        m_mask_x0 < m_mask_x1 && m_mask_y0 < m_mask_y1)
    {
        mask_view.pixels = m_clip_mask.data();
        mask_view.stride = m_width;
        mask_view.x0 = m_mask_x0; mask_view.y0 = m_mask_y0;
        mask_view.x1 = m_mask_x1; mask_view.y1 = m_mask_y1;
        clip = &mask_view;

        clip_x0 = std::max(clip_x0, m_mask_x0);
        clip_y0 = std::max(clip_y0, m_mask_y0);
        clip_x1 = std::min(clip_x1, m_mask_x1);
        clip_y1 = std::min(clip_y1, m_mask_y1);
    }

    if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1) return;

    RmlTexture* tex = reinterpret_cast<RmlTexture*>(texture);

    if (is_quad_batch(geo)) {
        /* Quads the blitter turns down - rotated, or vertex-coloured for a
         * gradient - are collected and rasterized together afterwards. */
        std::vector<int> leftover;
        size_t quads = geo->indices.size() / 6;

        for (size_t q = 0; q < quads; q++) {
            const Rml::Vertex* v = &geo->vertices[q * 4];
            if (!try_draw_fast_quad(m_fb, m_width, m_height,
                                    v[0], v[1], v[2], v[3],
                                    tex, translation,
                                    clip_x0, clip_y0, clip_x1, clip_y1, clip))
            {
                for (int k = 0; k < 6; k++)
                    leftover.push_back(geo->indices[q * 6 + k]);
            }
        }

        if (!leftover.empty()) {
            int bx0, by0, bx1, by1;
            if (geometry_bounds(geo, leftover.data(), leftover.size(), translation,
                                clip_x0, clip_y0, clip_x1, clip_y1, bx0, by0, bx1, by1))
            {
                render_triangles_accumulated(CovTarget::Framebuffer, m_fb, nullptr, m_width,
                                             geo, leftover.data(), leftover.size(),
                                             tex, translation, bx0, by0, bx1, by1, clip);
            }
        }
        return;
    }

    int bx0, by0, bx1, by1;
    if (!geometry_bounds(geo, geo->indices.data(), geo->indices.size(), translation,
                         clip_x0, clip_y0, clip_x1, clip_y1, bx0, by0, bx1, by1))
        return;

    render_triangles_accumulated(CovTarget::Framebuffer, m_fb, nullptr, m_width,
                                 geo, geo->indices.data(), geo->indices.size(),
                                 tex, translation, bx0, by0, bx1, by1, clip);
}
