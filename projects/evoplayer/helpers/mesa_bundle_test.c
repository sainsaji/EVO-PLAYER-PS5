/*
 * mesa_bundle_test.c - #237: the ProsperoWeb fork's Mesa path, outside WebKit.
 *
 * Built like the fork's payload (SDK v0.41.1, the fork's GL link stubs and
 * src/ps5/mesa/ps5_mesa.c, the prebuilt output/mesa_bundle/ps5_mesa_gpu.o) by
 * tools/build_mesa_bundle_test.sh. EVO's GPU spike spawns it as
 * /mnt/usb0/evo_gpu_client.elf and sends a broker endpoint as the first
 * message on fd 3. It then does what the WebProcess does: ps5_mesa_start(),
 * and WebKit's own EGL calls through the stubs (pbuffer config, ES 2
 * surfaceless context, a TextureMapper-style GLSL ES 1.00 shader).
 * Log: /data/prosperoweb/mesa.txt.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include "ps5_mesa.h"

static int recv_fd(int sock, int timeout_s)
{
    struct timeval tv = {timeout_s, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char byte = 0;
    struct iovec iov = {&byte, 1};
    union {
        struct cmsghdr h;
        char buf[CMSG_SPACE(sizeof(int))];
    } ctl;
    memset(&ctl, 0, sizeof ctl);
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = ctl.buf;
    m.msg_controllen = sizeof ctl.buf;
    if (recvmsg(sock, &m, 0) < 0)
        return -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int fd = -1;
            memcpy(&fd, CMSG_DATA(c), sizeof(int));
            return fd;
        }
    return -1;
}

/* As the fork's ps5_cpu_affinity.cpp: linked with --wrap=pthread_create, and
 * ps5_mesa_start() raises the minimum stack to 2 MiB. */
#include <pthread.h>
static size_t g_min_stack;
void ps5_thread_set_min_stack(size_t bytes) { g_min_stack = bytes; }
int __real_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);
int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
    pthread_attr_t local;
    int own = 0;
    size_t size = 0;
    if (g_min_stack) {
        if (!a) {
            if (pthread_attr_init(&local) == 0) {
                pthread_attr_setstacksize(&local, g_min_stack);
                a = &local;
                own = 1;
            }
        } else if (pthread_attr_getstacksize(a, &size) == 0 && size < g_min_stack) {
            pthread_attr_setstacksize((pthread_attr_t *)a, g_min_stack);
        }
    }
    const int rc = __real_pthread_create(t, a, fn, arg);
    if (own)
        pthread_attr_destroy(&local);
    ps5_mesa_log("bundle test: thread created (stack min %zu) -> %d", g_min_stack, rc);
    return rc;
}

int main(void);

static void on_crash(int sig, siginfo_t *si, void *ctx)
{
    const uint64_t *w = (const uint64_t *)ctx;   /* the console's context: rip word 28, rsp 31 */
    ps5_mesa_log("bundle test: CRASH signal %d addr %p rip %#llx rsp %#llx (main at %p)", sig,
                 si ? si->si_addr : NULL, (unsigned long long)(w ? w[28] : 0),
                 (unsigned long long)(w ? w[31] : 0), (void *)main);
    _exit(9);
}

static void on_exit_log(void)
{
    ps5_mesa_log("bundle test: exit() called");
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    atexit(on_exit_log);
    mkdir("/data/prosperoweb", 0777);
    close(open("/data/prosperoweb/mesa", O_WRONLY | O_CREAT, 0666));
    ps5_mesa_log("bundle test: pid %d", (int)getpid());
    const int fd = recv_fd(3, 30);
    ps5_mesa_log("bundle test: endpoint from EVO: fd %d", fd);
    if (fd < 0)
        return 1;
    char num[16];
    snprintf(num, sizeof num, "%d", fd);
    setenv("PROSPEROWEB_GPU_FD", num, 1);
    if (ps5_mesa_start() != 0) {
        ps5_mesa_log("bundle test: RESULT FAIL (start)");
        return 2;
    }

    /* WebKit's calls, through the fork's stubs. */
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
        ps5_mesa_log("bundle test: RESULT FAIL stub EGL init %#x", eglGetError());
        return 3;
    }
    const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                            EGL_STENCIL_SIZE, 8, EGL_NONE};
    EGLConfig config = 0;
    EGLint n = 0;
    eglChooseConfig(dpy, attrs, &config, 1, &n);
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cattrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext share = eglCreateContext(dpy, config, EGL_NO_CONTEXT, cattrs);
    EGLContext ctx = eglCreateContext(dpy, config, share, cattrs);
    const int current = ctx != EGL_NO_CONTEXT && eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
    ps5_mesa_log("bundle test: stubs EGL %d.%d, configs %d, shared ES2 contexts %p %p, surfaceless current %d (%#x)",
                 major, minor, n, share, ctx, current, eglGetError());
    if (!current) {
        ps5_mesa_log("bundle test: RESULT FAIL context");
        return 4;
    }
    const char *fs = "precision mediump float;\nuniform sampler2D s_sampler;\nvarying vec2 v_texCoord;\n"
                     "void main() { gl_FragColor = texture2D(s_sampler, v_texCoord); }\n";
    GLuint s = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(s, 1, &fs, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    ps5_mesa_log("bundle test: %s on %s, GLSL ES 1.00 compile %d", (const char *)glGetString(GL_VERSION),
                 (const char *)glGetString(GL_RENDERER), ok);
    ps5_mesa_log("bundle test: RESULT %s", ok ? "PASS" : "FAIL (shader)");
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return ok ? 0 : 5;
}
