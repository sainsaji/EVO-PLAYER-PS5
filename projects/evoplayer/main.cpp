#include "evo/Application.hpp"
#include "evo_boot_log.h"
#include "evo_boot_trace.h"

#ifdef EVO_HAVE_BUILD_ID
#include "evo_build_id.h"
#endif

#if defined(EVO_APP_MODULE)
#if defined(EVO_GL_SMOKE)
#include "pp_gl_smoke.h"
#include "evo_jailbreak.h"
#include <unistd.h>
#endif

#if defined(EVO_GL_HDR_PROBE)
#include <EGL/egl.h>
#include <GL/gl.h>
#include "evo_jailbreak.h"
#include <unistd.h>
extern "C" {
extern int g_ps5_video_out_hdr;
extern int g_ps5_register_buffers2_rc;
}
#endif
#endif

int main(int argc, char* argv[]) {
#ifdef EVO_HAVE_BUILD_ID
    evo_bt("BUILD " EVO_BUILD_ID);   /* first thing on screen — catches a stale mount */
#endif
    evo_bt("main() entry");

#if defined(EVO_APP_MODULE)
#if defined(EVO_GL_SMOKE)
    {
        evo_bt("GL-1 smoke");
        int rc = pp_gl_smoke_run();
        evo_bt("GL-1 smoke done rc=%d", rc);
        evo_jailbreak_self();
        evo_boot_log_flush();
        for (;;) {
            evo_boot_log_flush();
            sleep(30);
        }
    }
#endif

#if defined(EVO_GL_HDR_PROBE)
    {
        evo_bt_("GL HDR probe: starting");
        g_ps5_video_out_hdr = 1;

        static const EGLint cfg_attr[] = {
            EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_RED_SIZE, 10, EGL_GREEN_SIZE, 10, EGL_BLUE_SIZE, 10, EGL_ALPHA_SIZE, 2,
            EGL_NONE,
        };
        static const EGLint ctx_attr[] = {
            EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
            EGL_CONTEXT_MINOR_VERSION_KHR, 3,
            EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
            EGL_NONE,
        };

        EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        EGLint egl_major = 0, egl_minor = 0;
        EGLBoolean init_ok = eglInitialize(dpy, &egl_major, &egl_minor);
        evo_bt_("GL HDR probe: eglInitialize -> %d (EGL %d.%d)", (int)init_ok, egl_major, egl_minor);

        EGLint cfg_count = 0;
        EGLConfig cfg = nullptr;
        eglBindAPI(EGL_OPENGL_API);
        EGLBoolean choose_ok = eglChooseConfig(dpy, cfg_attr, &cfg, 1, &cfg_count);
        evo_bt_("GL HDR probe: eglChooseConfig -> %d (count=%d err=0x%x)",
                (int)choose_ok, cfg_count, eglGetError());

        EGLSurface srf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)0, nullptr);
        evo_bt_("GL HDR probe: eglCreateWindowSurface -> %p (err=0x%x, register_rc=%d)",
                (void *)srf, eglGetError(), g_ps5_register_buffers2_rc);

        EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
        evo_bt_("GL HDR probe: eglCreateContext -> %p (err=0x%x)", (void *)ctx, eglGetError());

        EGLBoolean current_ok = eglMakeCurrent(dpy, srf, srf, ctx);
        evo_bt_("GL HDR probe: eglMakeCurrent -> %d (err=0x%x)", (int)current_ok, eglGetError());

        if (current_ok && srf != EGL_NO_SURFACE) {
            glViewport(0, 0, 1920, 1080);
            glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glFinish();

            EGLBoolean swap_ok = eglSwapBuffers(dpy, srf);
            evo_bt_("GL HDR probe: eglSwapBuffers -> %d (err=0x%x)", (int)swap_ok, eglGetError());
        } else {
            evo_bt_("GL HDR probe: context/surface failed, skipping draw/swap");
        }

        evo_bt_("GL HDR probe: complete (register_rc=%d). Unjailing and parking.",
                g_ps5_register_buffers2_rc);
        evo_jailbreak_self();
        evo_boot_log_flush();
        for (;;) {
            evo_boot_log_flush();
            sleep(30);
        }
    }
#endif
#endif /* EVO_APP_MODULE */

    auto& app = evo::Application::getInstance();
    if (!app.initialize(argc, argv)) {
        return 1;
    }
    return app.run();
}
