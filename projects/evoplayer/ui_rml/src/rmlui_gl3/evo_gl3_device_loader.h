#pragma once
/*
 * evo_gl3_device_loader.h - RMLUI_GL3_CUSTOM_LOADER for the PS5 device build
 * (render-overhaul GL-3, #79).
 *
 * ps5-opengl links the GL 3.3 core entry points directly into the eboot
 * (GL_GLEXT_PROTOTYPES), so there is no runtime loader to run: glad's
 * gladLoaderLoadGL() dlopen("libGL.so.1")s, which does not exist on the PS5, so
 * the default RmlUi_Include_GL3.h path fails. With RMLUI_GL3_CUSTOM_LOADER set
 * to this header, RmlUi_Renderer_GL3.cpp skips glad entirely, RmlGL3::Initialize
 * becomes a no-op, and every gl* call binds at link time against the
 * ps5-opengl-core33 archives - the same way pp_gl_smoke.c did in GL-1.
 *
 * Host builds (EVO_RML_GL_HOST) keep the glad loader: Mesa's libGL is present
 * there and glad resolves against it.
 */
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include <GL/gl.h>
#include <GL/glext.h>
