/*
 * gpu_client_spike.cpp - #237 GPU broker spike, the client side.
 *
 * A PS5 payload (like the browser engine's WebProcess), spawned by the
 * ProsperoWeb broker with EVO's socket end at fd 3. EVO is the GPU owner: it
 * runs PS5_Vulkan's broker and sends us an endpoint as the first message on
 * fd 3. We then run real Mesa (EGL 1.5 + GLES 3 over Zink on the broker-client
 * RADV): every command buffer we build is submitted by EVO, every buffer lives
 * in memory EVO shares with us. No VideoOut, no AGC driver in this process.
 *
 * Test: a GLSL ES 3.00 shader compiled at run time draws a spinning triangle
 * into an FBO; each frame is read back into one of two SHM buffers and sent
 * to EVO over the normal browser wire (HELLO/FRAME), so it shows in EVO's Web
 * page view. Also measures how much texture memory it can get.
 *
 * Log: /data/prosperoweb/gpu_client.log. Built by tools/build_gpu_client.sh.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

extern "C" {
#include "evo_browser_wire.h"
#include "client.h"
int sceKernelReserveVirtualRange(void **addr, size_t len, int flags, size_t align);
}

#define SOCK 3
#define W 1280
#define H 720
#define STRIDE (W * 4)
#define FRAMES 300
#define LOG_PATH "/data/prosperoweb/gpu_client.log"

static int g_log = -1;

static void logf_(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof buf - 2)
        n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    if (g_log >= 0) {
        (void)write(g_log, buf, (size_t)n);
        fsync(g_log);
    }
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void *watchdog(void *arg)
{
    sleep((unsigned)(uintptr_t)arg);
    logf_("watchdog: still running after %u s, exiting", (unsigned)(uintptr_t)arg);
    _exit(3);
    return NULL;
}

static void on_crash(int sig, siginfo_t *si, void *ctx)
{
    const uint64_t *w = (const uint64_t *)ctx;
    logf_("CRASH signal %d addr %p rip %#llx rsp %#llx", sig, si ? si->si_addr : NULL,
          (unsigned long long)(w ? w[28] : 0), (unsigned long long)(w ? w[31] : 0));
    _exit(4);
}

/* The broker's clients map shared memory at fixed addresses: hold them before
 * anything else maps (protocol.h host ranges). */
static bool reserve_ranges(void)
{
    static const uintptr_t base[2] = {0x240000000ull, 0x3000000000ull};
    static const size_t bytes[2] = {512ull << 20, 16ull << 30};
    for (int i = 0; i < 2; ++i) {
        void *at = (void *)base[i];
        int rc = sceKernelReserveVirtualRange(&at, bytes[i], 0x10 | 0x80, 16384);
        logf_("reserve %#lx + %zu MB -> %#x at %p", (unsigned long)base[i], bytes[i] >> 20, (unsigned)rc, at);
        if (rc != 0 || at != (void *)base[i])
            return false;
    }
    return true;
}

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

static int send_msg(int type, const void *body, uint32_t len, const int *fds, int nfds)
{
    struct evob_hdr h = {EVOB_MAGIC, EVOB_VERSION, (uint16_t)type, len};
    uint8_t pkt[sizeof h + sizeof(struct evob_frame)];
    memcpy(pkt, &h, sizeof h);
    memcpy(pkt + sizeof h, body, len);
    struct iovec iov = {pkt, sizeof h + len};
    union {
        struct cmsghdr h;
        char buf[CMSG_SPACE(sizeof(int) * EVOB_MAX_BUFS)];
    } ctl;
    memset(&ctl, 0, sizeof ctl);
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    if (nfds > 0) {
        m.msg_control = ctl.buf;
        m.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds);
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)nfds);
    }
    return sendmsg(SOCK, &m, 0) == (ssize_t)(sizeof h + len) ? 0 : -1;
}

/* One RELEASE from EVO: buffer index, -1 nothing yet, -2 EVO gone. */
static int recv_release(int flags)
{
    uint8_t pkt[64];
    const ssize_t n = recv(SOCK, pkt, sizeof pkt, flags);
    if (n == 0)
        return -2;
    if (n < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? -1 : -2;
    struct evob_hdr h;
    if ((size_t)n < sizeof h)
        return -1;
    memcpy(&h, pkt, sizeof h);
    if (h.magic != EVOB_MAGIC)
        return -1;
    if (h.type == EVOB_BYE)
        return -2;
    if (h.type != EVOB_RELEASE || (size_t)n < sizeof h + sizeof(struct evob_release))
        return -1;
    struct evob_release r;
    memcpy(&r, pkt + sizeof h, sizeof r);
    return r.buf < 2 ? (int)r.buf : -1;
}

/* GL entry points through eglGetProcAddress (static Mesa). */
#define GL_FUNCS(X)                                                                                                \
    X(PFNGLGETSTRINGPROC, glGetString)                                                                             \
    X(PFNGLGETERRORPROC, glGetError)                                                                               \
    X(PFNGLGENTEXTURESPROC, glGenTextures)                                                                         \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures)                                                                   \
    X(PFNGLBINDTEXTUREPROC, glBindTexture)                                                                         \
    X(PFNGLTEXSTORAGE2DPROC, glTexStorage2D)                                                                       \
    X(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D)                                                                     \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri)                                                                     \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                                                                 \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                                                                 \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)                                                       \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)                                                   \
    X(PFNGLVIEWPORTPROC, glViewport)                                                                               \
    X(PFNGLCLEARCOLORPROC, glClearColor)                                                                           \
    X(PFNGLCLEARPROC, glClear)                                                                                     \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                                                       \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                                                       \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                                                                     \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                                                         \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                                                               \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                                                     \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                                                       \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                                                         \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                                                       \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                                                             \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                                                           \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                                                           \
    X(PFNGLUNIFORM1FPROC, glUniform1f)                                                                             \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays)                                                                           \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                                                                 \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                                                                 \
    X(PFNGLFINISHPROC, glFinish)                                                                                   \
    X(PFNGLREADPIXELSPROC, glReadPixels)                                                                           \
    X(PFNGLGETINTEGERVPROC, glGetIntegerv)

struct Gl {
#define DECL(t, n) t n;
    GL_FUNCS(DECL)
#undef DECL
    bool load()
    {
        bool ok = true;
#define LOAD(t, n)                                                                                                 \
    n = (t)eglGetProcAddress(#n);                                                                                  \
    if (!n) {                                                                                                      \
        logf_("missing GL entry point %s", #n);                                                                    \
        ok = false;                                                                                                \
    }
        GL_FUNCS(LOAD)
#undef LOAD
        return ok;
    }
};

static GLuint compile(Gl &gl, GLenum type, const char *src)
{
    GLuint s = gl.glCreateShader(type);
    gl.glShaderSource(s, 1, &src, NULL);
    const double t0 = now_ms();
    gl.glCompileShader(s);
    GLint ok = 0;
    gl.glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    char info[512] = {0};
    gl.glGetShaderInfoLog(s, sizeof info, NULL, info);
    logf_("compile %s shader -> %d in %.1f ms %s", type == GL_VERTEX_SHADER ? "vertex" : "fragment", ok,
          now_ms() - t0, info);
    return ok ? s : 0;
}

/* Textures of 4096x4096 RGBA8 (64 MB each) until one fails: how much GPU
 * memory a page could get. Each one is touched by a clear through an FBO. */
static void memory_test(Gl &gl)
{
    GLuint tex[64] = {0}, fbo = 0;
    gl.glGenFramebuffers(1, &fbo);
    int n = 0;
    const int max = access("/data/prosperoweb/gpu_client_mem16", F_OK) == 0 ? 64 : 16;
    for (; n < max; ++n) {
        gl.glGenTextures(1, &tex[n]);
        gl.glBindTexture(GL_TEXTURE_2D, tex[n]);
        gl.glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4096, 4096);
        gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[n], 0);
        gl.glClearColor(0.f, 0.f, 0.f, 1.f);
        gl.glClear(GL_COLOR_BUFFER_BIT);
        gl.glFinish();
        GLenum e = gl.glGetError();
        if (e != GL_NO_ERROR) {
            logf_("memory: texture %d (%d MB total) -> GL error %#x", n + 1, (n + 1) * 64, e);
            ++n;
            break;
        }
        if ((n + 1) % 4 == 0)
            logf_("memory: %d MB of textures ok", (n + 1) * 64);
    }
    logf_("memory: stopped at %d textures (%d MB)", n, n * 64);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.glDeleteTextures(n, tex);
    gl.glFinish();
}

int main(void)
{
    mkdir("/data/prosperoweb", 0777);
    g_log = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    const bool reserved = reserve_ranges();
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    pthread_t wd;
    pthread_create(&wd, NULL, watchdog, (void *)(uintptr_t)180);
    logf_("gpu client pid %d, ranges reserved %d", (int)getpid(), reserved ? 1 : 0);

    const int gpu_fd = recv_fd(SOCK, 30);
    logf_("endpoint from EVO: fd %d", gpu_fd);
    if (gpu_fd < 0)
        return 1;
    const double tb = now_ms();
    const int bind = ps5_gpu_client_bind(gpu_fd, reserved);
    logf_("ps5_gpu_client_bind -> %d (%.1f ms)", bind, now_ms() - tb);
    if (bind != 0)
        return 2;

    setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
    setenv("GALLIUM_THREAD", "0", 1);
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);

    double t0 = now_ms();
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
        logf_("EGL init failed: %#x", eglGetError());
        return 3;
    }
    logf_("EGL %d.%d (%s) in %.1f ms", major, minor, eglQueryString(dpy, EGL_VENDOR), now_ms() - t0);
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        logf_("context failed: %#x", eglGetError());
        return 4;
    }
    Gl gl;
    if (!gl.load())
        return 5;
    GLint max_tex = 0;
    gl.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    logf_("GL_VERSION %s | GL_RENDERER %s | GLSL %s | max texture %d", (const char *)gl.glGetString(GL_VERSION),
          (const char *)gl.glGetString(GL_RENDERER), (const char *)gl.glGetString(GL_SHADING_LANGUAGE_VERSION),
          max_tex);

    memory_test(gl);

    /* Render target. */
    GLuint tex = 0, fbo = 0;
    gl.glGenTextures(1, &tex);
    gl.glBindTexture(GL_TEXTURE_2D, tex);
    gl.glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
    gl.glGenFramebuffers(1, &fbo);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    logf_("FBO status %#x", gl.glCheckFramebufferStatus(GL_FRAMEBUFFER));

    /* Run-time GLSL: a triangle from gl_VertexID, spun by a uniform. */
    const char *vs_src = "#version 300 es\n"
                         "uniform float u_t;\n"
                         "out vec3 v_c;\n"
                         "void main() {\n"
                         "  float a = u_t + float(gl_VertexID) * 2.0943951;\n"
                         "  gl_Position = vec4(0.6 * cos(a) * 0.5625, 0.6 * sin(a), 0.0, 1.0);\n"
                         "  v_c = vec3(gl_VertexID == 0, gl_VertexID == 1, gl_VertexID == 2);\n"
                         "}\n";
    const char *fs_src = "#version 300 es\n"
                         "precision mediump float;\n"
                         "in vec3 v_c;\n"
                         "out vec4 o;\n"
                         "void main() { o = vec4(v_c, 1.0); }\n";
    GLuint vs = compile(gl, GL_VERTEX_SHADER, vs_src), fs = compile(gl, GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs)
        return 6;
    GLuint prog = gl.glCreateProgram();
    gl.glAttachShader(prog, vs);
    gl.glAttachShader(prog, fs);
    gl.glLinkProgram(prog);
    GLint linked = 0;
    gl.glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    logf_("link -> %d", linked);
    if (!linked)
        return 7;
    const GLint u_t = gl.glGetUniformLocation(prog, "u_t");
    GLuint vao = 0;
    gl.glGenVertexArrays(1, &vao);
    gl.glBindVertexArray(vao);

    /* Two shared frame buffers for EVO. */
    int fds[2];
    uint8_t *buf[2];
    for (int i = 0; i < 2; ++i) {
        fds[i] = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);
        if (fds[i] < 0 || ftruncate(fds[i], (off_t)STRIDE * H) != 0)
            return 8;
        buf[i] = (uint8_t *)mmap(NULL, (size_t)STRIDE * H, PROT_READ | PROT_WRITE, MAP_SHARED, fds[i], 0);
        if (buf[i] == MAP_FAILED)
            return 8;
    }
    const struct evob_hello hello = {W, H, STRIDE, EVOB_RGBA8_PREMUL, 2};
    struct timeval stv = {1, 0};
    setsockopt(SOCK, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof stv);
    setsockopt(SOCK, SOL_SOCKET, SO_RCVTIMEO, &stv, sizeof stv);
    if (send_msg(EVOB_HELLO, &hello, sizeof hello, fds, 2) != 0)
        return 9;
    close(fds[0]);
    close(fds[1]);

    uint8_t *rows = (uint8_t *)malloc((size_t)STRIDE * H);
    int free_buf[2] = {1, 1};
    double sum_draw = 0, sum_read = 0, worst = 0;
    int done = 0;
    uint32_t seq = 0;
    for (int f = 0; f < FRAMES; ++f) {
        int r;
        while ((r = recv_release(MSG_DONTWAIT)) >= 0)
            free_buf[r] = 1;
        if (r == -2)
            break;
        int b = free_buf[0] ? 0 : free_buf[1] ? 1 : -1;
        if (b < 0) {
            r = recv_release(0);
            if (r == -2)
                break;
            if (r >= 0)
                free_buf[r] = 1;
            --f;
            continue;
        }
        const double ta = now_ms();
        gl.glViewport(0, 0, W, H);
        gl.glClearColor(0.05f, 0.08f, 0.15f, 1.f);
        gl.glClear(GL_COLOR_BUFFER_BIT);
        gl.glUseProgram(prog);
        gl.glUniform1f(u_t, f * 0.05f);
        gl.glDrawArrays(GL_TRIANGLES, 0, 3);
        gl.glFinish();
        const double tr = now_ms();
        gl.glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rows);
        for (int y = 0; y < H; ++y) /* GL row 0 is the bottom */
            memcpy(buf[b] + (size_t)y * STRIDE, rows + (size_t)(H - 1 - y) * STRIDE, STRIDE);
        const double te = now_ms();
        sum_draw += tr - ta;
        sum_read += te - tr;
        if (te - ta > worst)
            worst = te - ta;
        ++done;
        if (f < 3 || f % 60 == 0) {
            const uint32_t c = *(const uint32_t *)(buf[b] + (size_t)(H / 2) * STRIDE + (W / 2) * 4);
            logf_("frame %d: draw+finish %.2f ms, readback+copy %.2f ms, centre px %#x, GL error %#x", f, tr - ta,
                  te - tr, c, gl.glGetError());
        }
        struct evob_frame fr;
        memset(&fr, 0, sizeof fr);
        fr.buf = (uint32_t)b;
        fr.seq = ++seq;
        fr.rects[fr.nrects++] = (struct evob_rect){0, 0, W, H};
        free_buf[b] = 0;
        if (send_msg(EVOB_FRAME, &fr, sizeof fr, NULL, 0) != 0)
            break;
        usleep(8000);
    }
    logf_("RESULT: %d frames, draw+finish avg %.2f ms, readback+copy avg %.2f ms, worst frame %.2f ms", done,
          done ? sum_draw / done : 0, done ? sum_read / done : 0, worst);

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    const uint32_t none = 0;
    send_msg(EVOB_BYE, &none, sizeof none, NULL, 0);
    logf_("bye");
    return done == FRAMES ? 0 : 10;
}
