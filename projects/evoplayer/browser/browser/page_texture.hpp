/*
 * EVO stand-in for Prospero Browser's browser/page_texture.hpp (#237).
 * Prospero draws engine frames into its own GL texture. EVO's kit copies the
 * engine host's shared buffers into AGC textures itself (WebFrame in
 * evo_hui_app.cpp), so this texture is never valid and holds nothing; the
 * browser UI asks evo_browser_page_texture() for the page instead.
 */
#pragma once

#include <cstdint>

typedef unsigned int GLuint;

namespace prospero::browser {

/* page_view.cpp clears the texture with GL before the built-in renderer draws;
 * with no texture that path returns first, these only let it compile. EVO
 * draws with sceAgc, never GL, here. */
#ifndef GL_COLOR_BUFFER_BIT
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100
#endif
inline void glClearColor(float, float, float, float) {}
inline void glClear(unsigned) {}

class PageTexture {
public:
    bool init(uint32_t width, uint32_t height)
    {
        width_ = width;
        height_ = height;
        return true;
    }
    void release() {}
    bool resize(uint32_t width, uint32_t height) { return init(width, height); }
    void update_pixels(const void*, uint32_t, uint32_t) {}
    void bind() {}
    void unbind() {}
    GLuint texture_id() const { return 0; }
    GLuint fbo_id() const { return 0; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool is_valid() const { return false; }

private:
    uint32_t width_{0};
    uint32_t height_{0};
};

}  // namespace prospero::browser
