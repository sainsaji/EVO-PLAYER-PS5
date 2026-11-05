/*
 * glslim_probe.c - does Sony's GLES library (libSceGLSlimVSH, "Piglet") work
 * from a payload process? (#237, the browser's GPU compositing plan,
 * docs/ui/browser.md). Sony's own browser (NKWebProcess) composites through
 * it with offscreen EGL contexts; no homebrew has used it on the PS5.
 *
 * Sent to elfldr (:9021) with EVO closed. Every step is logged to
 * /data/glslim_probe.log BEFORE it runs, so a crash shows where it stopped:
 *   1. scePigletGetConfigurationVSH: the default configuration, as hex
 *   2. eglGetDisplay / eglInitialize, the EGL strings, every config
 *   3. a GLES2 pbuffer context
 *   4. GL strings, then a 3584x1888 render target (EVO's 2x page) with a
 *      13-tap blur drawn over it, timed; glReadPixels of it, timed; a
 *      1792x944 texture upload, timed
 * Built by tools/build_glslim_probe.sh; never embedded in EVO.
 *
 * The compositor server keeps a client's slot until the client says Quit, and
 * a slot left by a dead probe needs a reboot to clear. So once GLSlim's init
 * may have connected, every exit path (normal end, crash signals, exit()
 * inside Sony's libraries, the watchdog) sends Quit itself: see
 * compositor_bye().
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <math.h>

#include <ps5/kernel.h>
#include <ps5/mdbg.h>

/* Sony's precompiled NK WebKit shaders, generated (not in git):
 * python3 output/re/scripts/nkshaders.py <libSceNKWebKitRequirements.sprx> output/re/nkshaders
 * python3 output/re/scripts/nkshaders_h.py output/re/nkshaders output/glslim_nk_shaders.h \
 *   616_texmap_v1.vert 616_texmap_f1.frag 616_texmap_v16384.vert 616_texmap_f16384.frag */
#include "../../../output/glslim_nk_shaders.h"

int scePigletGetConfigurationVSH(void *config);
int scePigletSetConfigurationVSH(const void *config);
int sceSysmoduleLoadModuleInternal(uint32_t id);
int64_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t align, int64_t *out_start,
                                       size_t *out_size);
int sceKernelAvailableFlexibleMemorySize(size_t *out);
int sceKernelGetAppInfo(int pid, void *info);
int sceKernelGetAppCategoryType(int pid, uint32_t *category);
int sceKernelGetIpcPath(const char *name, char *path);
int sceKernelPollEventFlag(void *ef, uint64_t pattern, uint32_t mode, uint64_t *result);

static FILE *g_log;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_log) {
        vfprintf(g_log, fmt, ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
    va_end(ap);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void hexdump(const unsigned char *p, int n)
{
    char line[128];
    for (int i = 0; i < n; i += 16) {
        int k = snprintf(line, sizeof line, "  %03x:", i);
        for (int j = 0; j < 16 && i + j < n; ++j)
            k += snprintf(line + k, sizeof line - k, " %02x", p[i + j]);
        logf_("%s", line);
    }
}

/* Read-only look at the AgcCompositor server (13.60 /system/sys/AgcCompositor.elf):
 * find its process by p_comm (struct proc +0x5e4), its load address from its
 * memory map (the 0x12c000-byte text mapping), then its 16 client slots
 * (base+0x1c8620, 0x2090 each: +0 index, +0xa0 pid, +0xa8 category, system
 * pool at +0xd0 and video pool at +0x910, each with its block count at
 * +0x28, owner pid at +0x830, memory type at +0x838). */
static pid_t g_spid;
static intptr_t g_sbase;
static volatile int g_watch_stop;

/* Polls the server's client slots for this process's pid while the main
 * thread runs GLSlim's init, and logs every new state of our slot. */
static void *watch_slot(void *arg)
{
    (void)arg;
    pid_t me = getpid();
    uint32_t last[8] = {0xffffffff};
    int seen = 0;
    unsigned long polls = 0;
    while (!g_watch_stop) {
        polls++;
        for (int i = 0; i < 16; ++i) {
            uint32_t pid_ = (uint32_t)mdbg_getint(g_spid, g_sbase + 0x1c8620 + (intptr_t)i * 0x2090 + 0xa0);
            if (pid_ != (uint32_t)me)
                continue;
            unsigned char t[0x2090];
            mdbg_copyout(g_spid, g_sbase + 0x1c8620 + (intptr_t)i * 0x2090, t, sizeof t);
            uint32_t v[8];
            memcpy(&v[0], t + 0xa8, 4);
            memcpy(&v[1], t + 0xd0 + 0x28, 4);
            memcpy(&v[2], t + 0x910 + 0x28, 4);
            memcpy(&v[3], t + 0xd0 + 0x838, 4);
            memcpy(&v[4], t + 0x910 + 0x838, 4);
            memcpy(&v[5], t + 0xa4, 4);
            memcpy(&v[6], t + 4, 4);
            v[7] = t[0];
            if (memcmp(v, last, sizeof v) != 0) {
                memcpy(last, v, sizeof v);
                logf_("watch: slot %d idx %u cat %#x sys %u video %u types %#x/%#x +0xa4 %#x +4 %#x", i, v[7],
                      v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
                if (seen++ == 0)
                    hexdump(t, 0x100);
            }
        }
    }
    logf_("watch: %lu polls, our slot seen %d times", polls, seen);
    return NULL;
}

static void find_compositor(void)
{
    intptr_t p = 0;
    kernel_copyout(KERNEL_ADDRESS_ALLPROC, &p, sizeof p);
    pid_t spid = 0;
    intptr_t sproc = 0;
    for (int guard = 0; p && guard < 2000; ++guard) {
        char comm[32] = {0};
        kernel_copyout(p + 0x5e4, comm, sizeof comm - 1);
        if (strstr(comm, "AgcCompositor")) {
            kernel_copyout(p + KERNEL_OFFSET_PROC_P_PID, &spid, sizeof spid);
            sproc = p;
            break;
        }
        intptr_t next = 0;
        kernel_copyout(p, &next, sizeof next);
        p = next;
    }
    logf_("server: AgcCompositor pid %d proc %#lx", (int)spid, (long)sproc);
    if (!sproc)
        return;
    intptr_t vm = 0, ent = 0, base = 0;
    kernel_copyout(sproc + KERNEL_OFFSET_PROC_P_VMSPACE, &vm, sizeof vm);
    kernel_copyout(vm + 8, &ent, sizeof ent);
    for (int i = 0; ent && ent != vm && i < 400; ++i) {
        uint64_t se[2] = {0, 0};
        kernel_copyout(ent + 0x20, se, sizeof se);
        if (se[1] - se[0] == 0x12c000 && !base)
            base = (intptr_t)se[0];
        if (i < 6)
            logf_("server: map %#llx..%#llx", (unsigned long long)se[0], (unsigned long long)se[1]);
        intptr_t next = 0;
        kernel_copyout(ent + 8, &next, sizeof next);
        ent = next;
    }
    logf_("server: base %#lx", (long)base);
    if (!base)
        return;
    g_spid = spid;
    g_sbase = base;
    unsigned char flag = 0;
    int r = mdbg_copyout(spid, base + 0x37e9f8, &flag, 1);
    logf_("server: flag 0x37e9f8 = %d (rc %d)", flag, r);
    for (int i = 0; i < 16; ++i) {
        unsigned char s[0x2090];
        if (mdbg_copyout(spid, base + 0x1c8620 + (intptr_t)i * 0x2090, s, sizeof s) != 0) {
            logf_("server: slot %d unreadable", i);
            break;
        }
        uint32_t spid_, cat, sysn, vidn, sysown, vidown, systype, vidtype;
        memcpy(&spid_, s + 0xa0, 4);
        memcpy(&cat, s + 0xa8, 4);
        memcpy(&sysn, s + 0xd0 + 0x28, 4);
        memcpy(&vidn, s + 0x910 + 0x28, 4);
        memcpy(&sysown, s + 0xd0 + 0x830, 4);
        memcpy(&vidown, s + 0x910 + 0x830, 4);
        memcpy(&systype, s + 0xd0 + 0x838, 4);
        memcpy(&vidtype, s + 0x910 + 0x838, 4);
        if (spid_ || sysn || vidn)
            logf_("server: slot %2d idx %u pid %u cat %#x sys blocks %u (owner %u type %#x) video blocks %u (owner %u type %#x)",
                  i, s[0], spid_, cat, sysn, sysown, systype, vidn, vidown, vidtype);
    }
}

/* --- Always disconnect from the compositor ---------------------------------
 * libSceComposite talks to the server over a Unix socket, one connection per
 * request (0x5670 in the 13.60 libSceComposite.sprx): path from
 * sceKernelGetIpcPath("GnmCompositor"), a 16-byte header {1, command,
 * payload bytes, status}, the payload, then a reply of the same shape.
 * sceCompositorQuit (0x5ea0) sends command 1 with no payload, but only when
 * the library's init got all the way through; after a failed map it does
 * nothing and the slot stays. The server knows the client by the socket's
 * peer, so this process sends that same Quit itself.
 *
 * Everything below may run in a signal handler: plain syscalls, no stdio,
 * no malloc. The socket address is built at startup. */
static struct sockaddr_un g_comp_addr;
static int g_comp_addr_ok;
static volatile sig_atomic_t g_comp_armed; /* GLSlim's init may have connected */
static volatile sig_atomic_t g_comp_done;
static int g_log_fd = -1;

static void raw_log(const char *s)
{
    if (g_log_fd >= 0)
        (void)write(g_log_fd, s, strlen(s));
}

static void raw_log_hex(const char *label, unsigned long long v)
{
    char b[24];
    int i = sizeof b;
    b[--i] = 0;
    do {
        b[--i] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v && i > 3);
    b[--i] = 'x';
    b[--i] = '0';
    raw_log(label);
    raw_log(b + i);
}

/* 1: our pid holds a slot in the server, 0: it does not, -1: can't tell
 * (server not found). */
static int our_slot(void)
{
    if (!g_sbase)
        return -1;
    pid_t me = getpid();
    for (int i = 0; i < 16; ++i)
        if ((uint32_t)mdbg_getint(g_spid, g_sbase + 0x1c8620 + (intptr_t)i * 0x2090 + 0xa0) == (uint32_t)me)
            return 1;
    return 0;
}

static void compositor_addr_init(void)
{
    static char path[256];
    memset(path, 0, sizeof path);
    int r = sceKernelGetIpcPath("GnmCompositor", path);
    memset(&g_comp_addr, 0, sizeof g_comp_addr);
    g_comp_addr.sun_family = AF_UNIX;
    strncpy(g_comp_addr.sun_path, path, sizeof g_comp_addr.sun_path - 1);
    g_comp_addr_ok = r == 0 && path[0];
    logf_("compositor: ipc path \"%s\" (rc %#x)", path, (unsigned)r);
}

/* Sends Quit if our slot is (or may be) still in the server, then checks it
 * is gone. Once per process. */
static void compositor_bye(const char *why)
{
    if (!g_comp_armed || g_comp_done)
        return;
    g_comp_done = 1;
    raw_log("bye: ");
    raw_log(why);
    int had = our_slot();
    raw_log(had > 0 ? ", our slot is in the server" : had == 0 ? ", no slot of ours" : ", server not found");
    if (had == 0) {
        raw_log(", nothing to release\n");
        return;
    }
    if (!g_comp_addr_ok) {
        raw_log(", no ipc path: CANNOT release\n");
        return;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        raw_log(", socket failed: CANNOT release\n");
        return;
    }
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    if (connect(fd, (const struct sockaddr *)&g_comp_addr, sizeof g_comp_addr) != 0) { /* connect-ok: local AF_UNIX compositor socket */
        close(fd);
        raw_log(", connect failed: CANNOT release\n");
        return;
    }
    const uint32_t hdr[4] = {1, 1, 0, 0}; /* Quit */
    ssize_t w = write(fd, hdr, sizeof hdr);
    uint32_t rep[4] = {0, 0, 0, 0};
    ssize_t got = 0;
    struct pollfd p = {fd, POLLIN, 0};
    while (w == (ssize_t)sizeof hdr && got < (ssize_t)sizeof rep && poll(&p, 1, 2000) > 0) {
        ssize_t n = read(fd, (char *)rep + got, sizeof rep - got);
        if (n <= 0)
            break;
        got += n;
    }
    close(fd);
    raw_log(w == (ssize_t)sizeof hdr ? ", Quit sent" : ", Quit write FAILED");
    raw_log_hex(", reply bytes ", (unsigned long long)got);
    raw_log_hex(" status ", rep[3]);
    /* The server releases asynchronously; give it up to a second. */
    int left = our_slot();
    for (int i = 0; i < 20 && left > 0; ++i) {
        usleep(50000);
        left = our_slot();
    }
    raw_log(left > 0 ? ", slot STILL THERE\n" : left == 0 ? ", slot released\n" : ", can't check\n");
}

static void on_fatal(int sig)
{
    raw_log_hex("signal ", (unsigned)sig);
    raw_log("\n");
    compositor_bye("fatal signal");
    _exit(128 + sig);
}

static void on_exit_(void)
{
    compositor_bye("exit()");
}

/* A hang (a library call that never returns) is an exit path too. */
static void *watchdog(void *arg)
{
    unsigned secs = (unsigned)(uintptr_t)arg;
    sleep(secs);
    raw_log_hex("watchdog: no end after seconds ", secs);
    raw_log("\n");
    compositor_bye("watchdog");
    _exit(124);
    return NULL;
}

static void compositor_guard_install(void)
{
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS, SIGTERM, SIGINT, SIGHUP};
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fatal;
    sa.sa_flags = SA_RESETHAND; /* a second fault in the handler kills us */
    sigemptyset(&sa.sa_mask);
    for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; ++i)
        sigaction(sigs[i], &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    atexit(on_exit_);
    pthread_t t;
    if (pthread_create(&t, NULL, watchdog, (void *)(uintptr_t)90) == 0)
        pthread_detach(t);
    else
        logf_("compositor: watchdog thread FAILED");
}

/* A program from two precompiled blobs. The binary format WebKit passes is
 * the shader's own type (glGetShaderiv GL_SHADER_TYPE), so do the same. */
static GLuint bin_program(const char *tag, const unsigned char *vb, int vn, const unsigned char *fb, int fn)
{
    GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderBinary(1, &vs, GL_VERTEX_SHADER, vb, vn);
    GLenum ev = glGetError();
    glShaderBinary(1, &fs, GL_FRAGMENT_SHADER, fb, fn);
    GLenum ef = glGetError();
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_vertex");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    char msg[256] = {0};
    if (!ok)
        glGetProgramInfoLog(p, sizeof msg - 1, NULL, msg);
    logf_("  program %s: binary errors 0x%x/0x%x, linked %d, a_vertex at %d, error 0x%x %s", tag, ev, ef, ok,
          glGetAttribLocation(p, "a_vertex"), glGetError(), msg);
    if (ok) {
        GLint nu = 0;
        glGetProgramiv(p, GL_ACTIVE_UNIFORMS, &nu);
        for (GLint i = 0; i < nu && i < 32; ++i) {
            char name[64] = {0};
            GLint size = 0;
            GLenum type = 0;
            glGetActiveUniform(p, (GLuint)i, sizeof name - 1, NULL, &size, &type, name);
            logf_("    uniform %d: %s type 0x%x size %d location %d", i, name, type, size,
                  glGetUniformLocation(p, name));
        }
        logf_("    %d active uniforms, error 0x%x", nu, glGetError());
    }
    return ok ? p : 0;
}

/* libSceComposite's client ring (libSceCompositeExt.sprx, which GLSlim links;
 * 13.60 offsets, module base from the kernel's dynlib list): the shared control block pointer is at +0x18030, our write index
 * at +0x1814c. Logs both and the block's first 0x20 bytes. */
static int (*g_comp_flush)(void);
static unsigned char *g_cb; /* libSceCompositeExt's base */
static int (*g_comp_wait_eor)(void); /* sceCompositorWaitEndOfRendering */

static void ring_state(const char *tag)
{
    static unsigned char *cb;
    if (!cb) {
        static const char *names[] = {"libSceComposite.sprx", "libSceComposite", "libSceCompositeExt.sprx",
                                      "libSceCompositeExt", "libSceComposite.native.sprx", "libSceGLSlimVSH.sprx"};
        unsigned char *gb = (unsigned char *)((uintptr_t)&eglCreatePbufferSurface - 0xd920);
        logf_("  GLSlim's init import -> %#lx", (unsigned long)*(uintptr_t *)(gb + 0x152a48));
        for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i) {
            uint32_t h = 0;
            int hr = kernel_dynlib_handle(getpid(), names[i], &h);
            if (hr != 0)
                continue;
            intptr_t mb = kernel_dynlib_mapbase_addr(getpid(), h);
            intptr_t ci = kernel_dynlib_resolve(getpid(), h, "IUlpGnuoR1c");
            logf_("  module %s: h %u base %#lx, init NID at %#lx", names[i], h, (unsigned long)mb, (unsigned long)ci);
            if (ci && !cb && ci - mb == 0x5180) {
                cb = (unsigned char *)mb;
                g_cb = cb;
                g_comp_flush = (int (*)(void))kernel_dynlib_resolve(getpid(), h, "qZNF03+ghLI");
                g_comp_wait_eor = (int (*)(void))kernel_dynlib_resolve(getpid(), h, "R00q9P3BKAc");
                logf_("  sceCompositorFlush at %p", (void *)g_comp_flush);
            }
        }
        if (!cb)
            return;
    }
    unsigned char *shm = *(unsigned char **)(cb + 0x18030);
    unsigned widx = *(unsigned *)(cb + 0x1814c);
    if (!shm) {
        logf_("  ring %s: no shm (write idx %u)", tag, widx);
        return;
    }
    const unsigned *w = (const unsigned *)shm;
    logf_("  ring %s: write idx %u, shm %08x %08x %08x %08x %08x %08x %08x %08x", tag, widx, w[0], w[1], w[2],
          w[3], w[4], w[5], w[6], w[7]);
}

/* GLSlim's submit (0xdd640) flushes the context buffer and only then writes
 * the swap's PostEvent (0x15) and EndOfFrame (6) commands, unflushed. The
 * server ends a client's pass at EndOfFrame and stops at an unflushed
 * placeholder, so without a flush of our own every frame finishes one swap
 * late. The server takes one frame per 60 Hz tick; the shared block's u32[2]
 * goes up by 0x10 when it has read one (CPU side: the GPU finishes later).
 * More than one frame ahead of that tick and GLSlim hangs in
 * sceCompositorWaitEndOfRendering. wait: 0 = flush only; 1 = also spin until
 * the counter moves. Returns ms from swap to the counter change, or -1. */
static double frame_end_w(EGLDisplay dpy, EGLSurface surf, const char *tag, int wait, int quiet)
{
    volatile unsigned *shm = g_cb ? *(volatile unsigned **)(g_cb + 0x18030) : NULL;
    unsigned before = shm ? shm[2] : 0, now = before;
    double t0 = now_ms();
    EGLBoolean sw = eglSwapBuffers(dpy, surf);
    double t1 = now_ms();
    int fr = g_comp_flush ? g_comp_flush() : -1;
    double t2 = now_ms(), hit = -1;
    while (wait && shm && now_ms() - t0 < 300) {
        now = shm[2];
        if (now != before) {
            hit = now_ms() - t0;
            break;
        }
        sched_yield();
    }
    if (!quiet)
        logf_("  frame %s: swap %d %.2f ms, flush %#x %.2f ms, counter %#x -> %#x at %.3f ms", tag, sw, t1 - t0,
              (unsigned)fr, t2 - t1, before, now, hit);
    return wait ? hit : t2 - t0;
}

/* GLSlim's current context (libSceGLSlimVSH 0x13920, 13.60) and its frame
 * slot: slots at ctx+0x8e8 (0x200 each), index ctx+0x864; in a slot, +0x1d0
 * the label the GPU writes at the end of the frame, +0x1d8 the value it will
 * write, +0x1e0 state (3 = submitted). GLSlim's frame begin (0xdd520) waits
 * on that label through sceCompositorWaitEndOfRendering, which never fires
 * for us, so we wait for it here first. Returns ms, -1 on timeout, -2 when
 * nothing is known. */
static void *(*g_slim_ctx)(void);

/* The slot this frame is being recorded into: take it before the swap, since
 * the swap moves ctx+0x864 on to the next slot (two slots, alternating). */
static unsigned char *slot_cur(unsigned *idx_out)
{
    if (!g_slim_ctx) {
        uint32_t h = 0;
        if (kernel_dynlib_handle(getpid(), "libSceGLSlimVSH.sprx", &h) == 0)
            g_slim_ctx = (void *(*)(void))(kernel_dynlib_mapbase_addr(getpid(), h) + 0x13920);
        if (!g_slim_ctx)
            return NULL;
    }
    unsigned char *ctx = g_slim_ctx();
    if (!ctx)
        return NULL;
    unsigned idx = *(unsigned *)(ctx + 0x864);
    if (idx_out)
        *idx_out = idx;
    return *(unsigned char **)(ctx + 0x8e8) + (size_t)idx * 0x200;
}

static double slot_wait_s(unsigned char *s, unsigned idx, const char *tag)
{
    if (!s)
        return -2;
    volatile uint64_t *lab = *(volatile uint64_t **)(s + 0x1d0);
    uint64_t want = *(uint64_t *)(s + 0x1d8);
    unsigned st = *(unsigned *)(s + 0x1e0);
    double t0 = now_ms(), r = -1;
    uint64_t got = lab ? *lab : 0;
    if (st == 3 && lab) {
        while (now_ms() - t0 < 300) {
            got = *lab;
            if (got >= want) {
                r = now_ms() - t0;
                break;
            }
            sched_yield();
        }
    } else {
        r = 0;
    }
    if (tag)
        logf_("  slot %s: %p idx %u state %u label %p %#llx want %#llx, %.3f ms", tag, s, idx, st, (void *)lab,
              (unsigned long long)got, (unsigned long long)want, r);
    return r;
}

/* The working frame end (2026-11-05): take the slot, swap, flush, then wait
 * for the slot's GPU label. Also notes when the server tick lands (shared
 * counter moves) and returns label time minus tick time = GPU time of the
 * frame (-1 on timeout). */
static double frame_sync(EGLDisplay dpy, EGLSurface surf, const char *tag)
{
    volatile unsigned *shm = g_cb ? *(volatile unsigned **)(g_cb + 0x18030) : NULL;
    unsigned c0 = shm ? shm[2] : 0;
    unsigned idx = 0;
    unsigned char *s = slot_cur(&idx);
    double t0 = now_ms();
    eglSwapBuffers(dpy, surf);
    if (g_comp_flush)
        g_comp_flush();
    if (!s)
        return -1;
    volatile uint64_t *lab = *(volatile uint64_t **)(s + 0x1d0);
    uint64_t want = *(uint64_t *)(s + 0x1d8);
    double tick = -1, done = -1;
    while (lab && now_ms() - t0 < 500) {
        double t = now_ms() - t0;
        if (tick < 0 && shm && shm[2] != c0)
            tick = t;
        if (*lab >= want) {
            done = t;
            break;
        }
    }
    if (tick < 0)
        tick = done;
    if (tag)
        logf_("  sync %s: slot %u, tick at %.3f ms, label at %.3f ms, GPU %.3f ms", tag, idx, tick, done,
              done - tick);
    return done < 0 ? -1 : done - tick;
}

/* Left half red, right half green. GLSlim seems to apply clears lazily: a
 * frame holding only clears was not seen by a later frame's sampling, so seed
 * in the same frame as the draw that reads it. */
static void seed_halves(GLuint fbo, int W, int H)
{
    /* Upload, no clears: GL_TEXTURE_2D bound to unit 0 must be fbo's texture. */
    static unsigned char *pat;
    if (!pat && (pat = malloc((size_t)W * H * 4)) != NULL)
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                unsigned char *p = pat + ((size_t)y * W + x) * 4;
                p[0] = x < W / 2 ? 0xff : 0;
                p[1] = x < W / 2 ? 0 : 0xff;
                p[2] = 0;
                p[3] = 0xff;
            }
    if (pat) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pat);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, W, H);
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, W, H);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, W / 2, H);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(W / 2, 0, W - W / 2, H);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

/* Reads the middle row of the bound framebuffer and logs pixels around the
 * vertical centre line and at the quarters. */
static void row_dump(const char *tag, int W, int H)
{
    unsigned char row[64 * 4];
    unsigned char q[2][4];
    glReadPixels(W / 2 - 32, H / 2, 64, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
    glReadPixels(W / 4, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q[0]);
    glReadPixels(3 * W / 4, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q[1]);
    char buf[64 * 8 + 1];
    int n = 0;
    for (int i = 0; i < 64; i += 2)
        n += snprintf(buf + n, sizeof buf - n, "%02x%02x ", row[i * 4], row[i * 4 + 1]);
    logf_("  %s: quarter %02x%02x%02x%02x / %02x%02x%02x%02x, error 0x%x", tag, q[0][0], q[0][1], q[0][2], q[0][3],
          q[1][0], q[1][1], q[1][2], q[1][3], glGetError());
    logf_("  %s: row x %d..%d step 2 (rg): %s", tag, W / 2 - 32, W / 2 + 31, buf);
}

static double frame_end(EGLDisplay dpy, EGLSurface surf, const char *tag)
{
    return frame_end_w(dpy, surf, tag, 1, 0);
}

int main(void)
{
    g_log = fopen("/data/glslim_probe.log", "w");
    logf_("glslim probe: start");
    /* Sony's libraries report errors with fprintf(stderr): into the log. */
    if (g_log) {
        dup2(fileno(g_log), 1);
        dup2(fileno(g_log), 2);
        g_log_fd = fileno(g_log);
    }
    compositor_addr_init();
    compositor_guard_install();
    /* The compositor server refuses unknown clients (0x80d40009) and a
     * failed init cannot be retried in the same process (0x80d40003). Under
     * another identity GLSlim cannot load its system modules (AGC_VSH,
     * 0x80020002). So: load those modules as ourselves, then switch to
     * /data/glslim_authid (hex) before GLSlim's first init. */
    {
        pid_t pid = getpid();
        logf_("identity: pid %d authid %#llx", (int)pid, (unsigned long long)kernel_get_ucred_authid(pid));
        {
            int64_t start = 0;
            size_t avail = 0;
            int64_t total = sceKernelGetDirectMemorySize();
            int r = sceKernelAvailableDirectMemorySize(0, total, 2u * 1024u * 1024u, &start, &avail);
            size_t flex = 0;
            sceKernelAvailableFlexibleMemorySize(&flex);
            logf_("memory: direct total %lld MB, available %zu MB (rc %#x), flexible available %zu MB",
                  (long long)(total >> 20), avail >> 20, (unsigned)r, flex >> 20);
        }
        /* GLSlim's three, and the one sceCompositorInitWithProcessOrder loads. */
        static const uint32_t mods[] = {0x80000086, 0x8000008b, 0x80000004, 0x8000001f};
        for (unsigned i = 0; i < 4; ++i)
            logf_("identity: sysmodule %#x load = %#x", mods[i], (unsigned)sceSysmoduleLoadModuleInternal(mods[i]));
        unsigned long long want = 0;
        FILE *f = fopen("/data/glslim_authid", "r");
        if (f) {
            if (fscanf(f, "%llx", &want) != 1)
                want = 0;
            fclose(f);
        }
        if (want) {
            int r = kernel_set_ucred_authid(pid, want);
            logf_("identity: set authid %#llx (%d), now %#llx", want, r,
                  (unsigned long long)kernel_get_ucred_authid(pid));
        }
        /* /data/glslim_refpid: copy that process's ucred caps and attrs
         * (the compositor's memory map is refused with EPERM otherwise). */
        int ref = 0;
        char what[8] = "ca";  /* c: caps, a: attrs */
        FILE *rf = fopen("/data/glslim_refpid", "r");
        if (rf) {
            if (fscanf(rf, "%d %7s", &ref, what) < 1)
                ref = 0;
            fclose(rf);
        }
        uint8_t caps[16], attrs[32], rcaps[16], rattrs[32];
        kernel_get_ucred_caps(pid, caps);
        kernel_get_ucred_attrs(pid, attrs);
        logf_("identity: our caps %02x%02x%02x%02x%02x%02x%02x%02x.. attrs %02x%02x%02x%02x%02x%02x%02x%02x..",
              caps[0], caps[1], caps[2], caps[3], caps[4], caps[5], caps[6], caps[7], attrs[0], attrs[1],
              attrs[2], attrs[3], attrs[4], attrs[5], attrs[6], attrs[7]);
        if (ref > 0) {
            kernel_get_ucred_caps(ref, rcaps);
            kernel_get_ucred_attrs(ref, rattrs);
            logf_("identity: pid %d authid %#llx caps %02x%02x%02x%02x%02x%02x%02x%02x.. attrs %02x%02x%02x%02x%02x%02x%02x%02x..",
                  ref, (unsigned long long)kernel_get_ucred_authid(ref), rcaps[0], rcaps[1], rcaps[2], rcaps[3],
                  rcaps[4], rcaps[5], rcaps[6], rcaps[7], rattrs[0], rattrs[1], rattrs[2], rattrs[3], rattrs[4],
                  rattrs[5], rattrs[6], rattrs[7]);
            /* Keep ours and add theirs: OR, byte by byte. */
            char hc[64] = {0}, ha[96] = {0};
            for (int i = 0; i < 16; ++i) {
                caps[i] |= rcaps[i];
                snprintf(hc + i * 2, 3, "%02x", caps[i]);
            }
            for (int i = 0; i < 32; ++i) {
                attrs[i] |= rattrs[i];
                snprintf(ha + i * 2, 3, "%02x", attrs[i]);
            }
            int r1 = strchr(what, 'c') ? kernel_set_ucred_caps(pid, caps) : -99;
            int r2 = strchr(what, 'a') ? kernel_set_ucred_attrs(pid, attrs) : -99;
            logf_("identity: caps now %s (%d), attrs now %s (%d)", hc, r1, ha, r2);
        }
        /* The app info the compositor reads (sceKernelGetAppInfo; category
         * at +0x50) lives in struct proc at +0x460 on 13.60, so the category
         * is proc+0x4b0: 0xffffffff "unknown" for a payload, 0x01000000
         * "common_dialog" for Sony's NKWebProcess. /data/glslim_poke lines
         * "<hex proc offset> <hex value> <bytes>" are written into OUR proc. */
        static unsigned char ai[0x150];
        memset(ai, 0, sizeof ai);
        int ar = sceKernelGetAppInfo(pid, ai);
        logf_("appinfo (rc %#x) category %#x, first 0x60 bytes:", (unsigned)ar, *(uint32_t *)(ai + 0x50));
        hexdump(ai, 0x60);
        intptr_t proc = kernel_get_proc(pid);
        FILE *pf = fopen("/data/glslim_poke", "r");
        unsigned long long off, val;
        unsigned nb;
        while (pf && fscanf(pf, "%llx %llx %u", &off, &val, &nb) == 3) {
            if (off >= 0x1400 || (nb != 1 && nb != 2 && nb != 4 && nb != 8))
                continue;
            uint64_t old = 0;
            kernel_copyout(proc + (intptr_t)off, &old, nb);
            int r = kernel_copyin(&val, proc + (intptr_t)off, nb);
            logf_("poke proc+%#llx (%u): %#llx -> %#llx (rc %d)", off, nb, (unsigned long long)old, val, r);
        }
        if (pf) {
            fclose(pf);
            memset(ai, 0, sizeof ai);
            ar = sceKernelGetAppInfo(pid, ai);
            logf_("appinfo after poke (rc %#x) category %#x", (unsigned)ar, *(uint32_t *)(ai + 0x50));
        }
        {
            /* What the compositor's admission check asks (AgcCompositor 0x6380). */
            uint32_t c1 = 0, c2 = 0;
            int q1 = sceKernelGetAppCategoryType(pid, &c1);
            int q2 = sceKernelGetAppCategoryType(1230, &c2);
            logf_("category type: ours %#x (rc %#x), pid 1230 %#x (rc %#x)", c1, (unsigned)q1, c2, (unsigned)q2);
        }
    }

    /* 1. The default configuration. Its layout on the PS5 is unknown, so it
     * is only dumped; the size field (the first word on the PS4) is set to
     * the buffer's size in case the call checks it. */
    /* The configuration as NKWebProcess builds it (0x3f30 in its text, mode
     * 1 of its 4-row size table at 0x9db8): 0xa8 bytes. Its allocator
     * callbacks (+0x60..+0x88) are left out; the flags word (+0x04, 0x78 in
     * Sony's) comes from /data/glslim_flags (hex) so variants need no rebuild. */
    static unsigned char cfg[0xa8];
    memset(cfg, 0, sizeof cfg);
    uint32_t flags = 0;
    {
        FILE *f = fopen("/data/glslim_flags", "r");
        if (f) {
            if (fscanf(f, "%x", &flags) != 1)
                flags = 0;
            fclose(f);
        }
    }
    *(uint32_t *)(cfg + 0x00) = sizeof cfg;
    *(uint32_t *)(cfg + 0x04) = flags;
    *(uint32_t *)(cfg + 0x08) = 1;
    *(uint64_t *)(cfg + 0x18) = 0x200000;     /* system shared memory */
    *(uint64_t *)(cfg + 0x28) = 0x2400000;    /* video shared memory */
    {
        /* /data/glslim_sizes: "<system hex> <video hex>" overrides the two. */
        FILE *f = fopen("/data/glslim_sizes", "r");
        unsigned long long s = 0, v = 0;
        if (f) {
            if (fscanf(f, "%llx %llx", &s, &v) == 2) {
                *(uint64_t *)(cfg + 0x18) = s;
                *(uint64_t *)(cfg + 0x28) = v;
                logf_("step 1: sizes from file: system %#llx video %#llx", s, v);
            }
            fclose(f);
        }
    }
    *(uint64_t *)(cfg + 0x30) = 0xa000000;    /* max mapped flexible memory */
    *(uint32_t *)(cfg + 0x40) = 1920;
    *(uint32_t *)(cfg + 0x44) = 1080;
    *(uint64_t *)(cfg + 0x54) = 0x10000000c0000ull; /* draw command buffer, lcue */
    *(uint32_t *)(cfg + 0x5c) = 2;
    logf_("step 1: scePigletSetConfigurationVSH, flags 0x%x", flags);
    int rc = scePigletSetConfigurationVSH(cfg);
    logf_("  rc 0x%08x", (unsigned)rc);
    (void)hexdump;

    find_compositor();
    pthread_t watcher;
    int watching = g_sbase && pthread_create(&watcher, NULL, watch_slot, NULL) == 0;
    /* eglGetDisplay runs GLSlim's library init, which connects to the
     * compositor: from here on every exit sends Quit. */
    g_comp_armed = 1;
    logf_("step 2: eglGetDisplay");
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (watching) {
        usleep(200000);
        g_watch_stop = 1;
        pthread_join(watcher, NULL);
    }
    logf_("  display %p, error 0x%x", (void *)dpy, eglGetError());
    if (dpy == EGL_NO_DISPLAY)
        goto out;
    {
        unsigned char *gb = (unsigned char *)((uintptr_t)&eglCreatePbufferSurface - 0xd920);
        logf_("  library inited %d", gb[0x156560]);
    }
    EGLint maj = 0, min = 0;
    logf_("step 2: eglInitialize");
    EGLBoolean ok = eglInitialize(dpy, &maj, &min);
    logf_("  ok %d, EGL %d.%d, error 0x%x", ok, maj, min, eglGetError());
    if (!ok)
        goto out;
    logf_("  vendor: %s", eglQueryString(dpy, EGL_VENDOR));
    logf_("  version: %s", eglQueryString(dpy, EGL_VERSION));
    logf_("  apis: %s", eglQueryString(dpy, EGL_CLIENT_APIS));
    logf_("  extensions: %s", eglQueryString(dpy, EGL_EXTENSIONS));

    EGLConfig all[64];
    EGLint n = 0;
    eglGetConfigs(dpy, all, 64, &n);
    logf_("  %d configs", n);
    for (int i = 0; i < n; ++i) {
        EGLint r, g, b, a, dep, st, surf, rt;
        eglGetConfigAttrib(dpy, all[i], EGL_RED_SIZE, &r);
        eglGetConfigAttrib(dpy, all[i], EGL_GREEN_SIZE, &g);
        eglGetConfigAttrib(dpy, all[i], EGL_BLUE_SIZE, &b);
        eglGetConfigAttrib(dpy, all[i], EGL_ALPHA_SIZE, &a);
        eglGetConfigAttrib(dpy, all[i], EGL_DEPTH_SIZE, &dep);
        eglGetConfigAttrib(dpy, all[i], EGL_STENCIL_SIZE, &st);
        eglGetConfigAttrib(dpy, all[i], EGL_SURFACE_TYPE, &surf);
        eglGetConfigAttrib(dpy, all[i], EGL_RENDERABLE_TYPE, &rt);
        logf_("  config %d: rgba %d%d%d%d depth %d stencil %d surface 0x%x renderable 0x%x",
              i, r, g, b, a, dep, st, surf, rt);
    }

    logf_("step 3: GLES2 pbuffer context");
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint want[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                           EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                           EGL_NONE};
    EGLConfig cfgs[1];
    EGLint nc = 0;
    ok = eglChooseConfig(dpy, want, cfgs, 1, &nc);
    logf_("  choose ok %d, %d match, error 0x%x", ok, nc, eglGetError());
    if (!ok || nc < 1)
        goto out;
    /* A pbuffer per config and size, to see which (if any) the library makes. */
    EGLSurface surf = EGL_NO_SURFACE;
    EGLConfig surf_cfg = cfgs[0];
    {
        static const EGLint sizes[][2] = {{64, 64}, {1920, 1080}, {16, 16}};
        for (int i = 0; i < n && surf == EGL_NO_SURFACE; ++i)
            for (int s = 0; s < 3; ++s) {
                const EGLint pb[] = {EGL_WIDTH, sizes[s][0], EGL_HEIGHT, sizes[s][1], EGL_NONE};
                EGLSurface t = eglCreatePbufferSurface(dpy, all[i], pb);
                logf_("  pbuffer config %d %dx%d: %p, error 0x%x", i, sizes[s][0], sizes[s][1],
                      (void *)t, eglGetError());
                if (t != EGL_NO_SURFACE) {
                    surf = t;
                    surf_cfg = all[i];
                    break;
                }
            }
    }
    const EGLint ctxa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, surf_cfg, EGL_NO_CONTEXT, ctxa);
    logf_("  context %p, error 0x%x", (void *)ctx, eglGetError());
    if (ctx == EGL_NO_CONTEXT)
        goto out;
    ok = EGL_FALSE;
    if (surf != EGL_NO_SURFACE) {
        ok = eglMakeCurrent(dpy, surf, surf, ctx);
        logf_("  make current with the pbuffer: ok %d, error 0x%x", ok, eglGetError());
    }
    if (!ok) {
        ok = eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
        logf_("  make current surfaceless: ok %d, error 0x%x", ok, eglGetError());
    }
    if (!ok)
        goto out;

    logf_("step 4: GL");
    logf_("  GL_VENDOR %s", (const char *)glGetString(GL_VENDOR));
    logf_("  GL_RENDERER %s", (const char *)glGetString(GL_RENDERER));
    logf_("  GL_VERSION %s", (const char *)glGetString(GL_VERSION));
    logf_("  GLSL %s", (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
    logf_("  GL_EXTENSIONS %s", (const char *)glGetString(GL_EXTENSIONS));
    GLint maxtex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
    logf_("  max texture %d", maxtex);

    /* EVO's 1x page: two of them fit the payload's 48 MB grant (a 2x page,
     * 27 MB each, does not). */
    const int W = 1792, H = 944;
    GLuint tex[2], fbo[2];
    glGenTextures(2, tex);
    glGenFramebuffers(2, fbo);
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[i], 0);
        logf_("  target %d: fbo status 0x%x, error 0x%x", i, glCheckFramebufferStatus(GL_FRAMEBUFFER),
              glGetError());
    }

    /* First GPU draw, no shaders needed: clear target 0 on the GPU, read it
     * back, check the colour. Then time a full-page clear. */
    double t0, t1;
    {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
        glViewport(0, 0, W, H);
        logf_("  before clears: error 0x%x, slot %d", glGetError(), our_slot());
        ring_state("before");
        /* Three colours, each cleared, finished and read back whole: if the
         * GPU runs every submit, each readback shows its own colour. */
        static const float cols[3][3] = {{0.2f, 0.4f, 0.6f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.5f}};
        unsigned char *rb = malloc((size_t)W * H * 4);
        for (int k = 0; k < 3 && rb; ++k) {
            /* Clear 0 is whole; 1 and 2 are scissored (left, right half), which
             * cannot be metadata-only fast clears. */
            if (k > 0) {
                glEnable(GL_SCISSOR_TEST);
                glScissor(k == 1 ? 0 : W / 2, 0, W / 2, H);
            }
            glClearColor(cols[k][0], cols[k][1], cols[k][2], 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
            double c0 = now_ms();
            char tag[16];
            snprintf(tag, sizeof tag, "clear%d", k);
            frame_end(dpy, surf, tag);
            double c1 = now_ms();
            memset(rb, 0x11, (size_t)W * H * 4);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rb);
            double c2 = now_ms();
            const unsigned char *m = rb + ((size_t)(H / 2) * W + W / 2) * 4, *z = rb + ((size_t)W * H - 1) * 4;
            logf_("  clear %d: finish %.2f ms, read %.2f ms, error 0x%x, first %02x%02x%02x%02x centre "
                  "%02x%02x%02x%02x last %02x%02x%02x%02x, slot %d",
                  k, c1 - c0, c2 - c1, glGetError(), rb[0], rb[1], rb[2], rb[3], m[0], m[1], m[2], m[3], z[0], z[1],
                  z[2], z[3], our_slot());
            ring_state("after");
        }
        free(rb);
        unsigned char c[4] = {0, 0, 0, 0};
        glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
        logf_("  1x1 read at centre: %02x %02x %02x %02x, error 0x%x", c[0], c[1], c[2], c[3], glGetError());
        /* Ten frames back to back, each ended by frame_end(): every one
         * should land, and the last readback must show the last colour. */
        /* Loop B: 30 frames, each waits for the frame counter, then reads
         * the centre pixel back and checks it is this frame's colour. */
        t0 = now_ms();
        int misses = 0, stale = 0;
        double wmax = 0, wsum = 0;
        for (int k = 0; k < 30; ++k) {
            int r = (k * 37) % 256;
            glClearColor(r / 255.0f, 0.5f, 0.25f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            char tag[16];
            snprintf(tag, sizeof tag, "B%d", k);
            double w = frame_end_w(dpy, surf, tag, 1, k >= 4);
            if (w < 0)
                ++misses;
            else {
                wsum += w;
                if (w > wmax)
                    wmax = w;
            }
            glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
            if (c[0] != r) {
                ++stale;
                if (stale <= 2)
                    logf_("  B%d stale: read r %02x, want %02x", k, c[0], r);
            }
        }
        t1 = now_ms();
        logf_("  loop B (wait for counter, 1 px check): 30 frames %.2f ms (%.2f per frame), wait avg %.3f max %.3f, "
              "%d timeouts, %d stale",
              t1 - t0, (t1 - t0) / 30, wsum / (30 - misses ? 30 - misses : 1), wmax, misses, stale);
        ring_state("after B");
        /* Loop C: after the counter moves, spin-read the centre pixel until
         * it is this frame's colour: how long the GPU takes after the tick. */
        {
            double lmax = 0, lsum = 0;
            int never = 0;
            for (int k = 0; k < 20; ++k) {
                int r = (k * 53 + 7) % 256;
                glClearColor(r / 255.0f, 0.75f, 0.25f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                frame_end_w(dpy, surf, "C", 1, 1);
                double s = now_ms(), l = -1;
                int reads = 0;
                while (now_ms() - s < 50) {
                    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
                    ++reads;
                    if (c[0] == r) {
                        l = now_ms() - s;
                        break;
                    }
                }
                if (l < 0)
                    ++never;
                else {
                    lsum += l;
                    if (l > lmax)
                        lmax = l;
                }
                if (k < 3)
                    logf_("  C%d: fresh after %.3f ms (%d reads)", k, l, reads);
            }
            logf_("  loop C: GPU done after the tick avg %.3f max %.3f ms, %d never fresh in 50 ms",
                  lsum / (20 - never ? 20 - never : 1), lmax, never);
        }
        /* (sceCompositorWaitEndOfRendering never fires for this client: 60 s
         * timeout, 2026-11-05. GLSlim's frame begin falls back on it when the
         * slot's label is not done, which is the back-to-back hang.)
         * Loop E: swap, flush, then wait for GLSlim's own slot label (the
         * GPU's release-mem write), no counter wait; read and check. Loop F:
         * the same with glFinish instead of the pbuffer swap. */
        for (int pass = 0; pass < 2; ++pass) {
            double wmx = 0, wsm = 0, t_0 = now_ms();
            int bad = 0, to = 0;
            for (int k = 0; k < 30; ++k) {
                int r = (k * 71 + 11 + pass * 5) % 256;
                glClearColor(r / 255.0f, 0.25f, 0.5f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                unsigned sidx = 0;
                unsigned char *sl = slot_cur(&sidx);
                if (k < 2 && sl)
                    logf_("  before %s%d: slot %p idx %u state %u want %#llx", pass ? "F" : "E", k, sl, sidx,
                          *(unsigned *)(sl + 0x1e0), *(unsigned long long *)(sl + 0x1d8));
                if (pass == 0) {
                    eglSwapBuffers(dpy, surf);
                } else {
                    glFinish();
                }
                if (g_comp_flush)
                    g_comp_flush();
                double w = slot_wait_s(sl, sidx, k < 2 ? (pass ? "F" : "E") : NULL);
                if (w < 0)
                    ++to;
                else {
                    wsm += w;
                    if (w > wmx)
                        wmx = w;
                }
                glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
                if (c[0] != r) {
                    ++bad;
                    if (bad <= 3)
                        logf_("  %s%d stale: read r %02x want %02x", pass ? "F" : "E", k, c[0], r);
                }
            }
            logf_("  loop %s (%s + flush + slot label): 30 frames %.2f ms, label wait avg %.3f max %.3f, %d timeouts, "
                  "%d stale",
                  pass ? "F" : "E", pass ? "glFinish" : "swap", now_ms() - t_0, wsm / 30, wmx, to, bad);
        }
        ring_state("after E/F");
    }

    /* No runtime compiler (glShaderSource is a stub), so: NK WebKit's own
     * precompiled TextureMapper shaders (AMDGPU ELFs from
     * libSceNKWebKitRequirements, output/glslim_nk_shaders.h). Option 1 =
     * textured copy, 16384 = Gaussian blur filter. */
    {
        static const float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
        static const float mv[16] = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, -1, -1, 0, 1};
        static const float id4[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        static const float id3[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        GLuint pcopy = bin_program("copy", nk_v1_vert, sizeof nk_v1_vert, nk_f1_frag, sizeof nk_f1_frag);
        GLuint pblur =
            bin_program("blur", nk_v16384_vert, sizeof nk_v16384_vert, nk_f16384_frag, sizeof nk_f16384_frag);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
        glEnableVertexAttribArray(0);
        GLuint progs[2] = {pcopy, pblur};
        for (int i = 0; i < 2; ++i) {
            if (!progs[i])
                continue;
            glUseProgram(progs[i]);
            GLenum e[6];
            glUniformMatrix4fv(glGetUniformLocation(progs[i], "u_modelViewMatrix"), 1, GL_FALSE, mv);
            e[0] = glGetError();
            glUniformMatrix4fv(glGetUniformLocation(progs[i], "u_projectionMatrix"), 1, GL_FALSE, id4);
            e[1] = glGetError();
            glUniformMatrix4fv(glGetUniformLocation(progs[i], "u_textureSpaceMatrix"), 1, GL_FALSE, id4);
            e[2] = glGetError();
            glUniform1i(glGetUniformLocation(progs[i], "s_sampler"), 0);
            e[3] = glGetError();
            GLint cs = glGetUniformLocation(progs[i], "u_textureColorSpaceMatrix");
            e[4] = e[5] = 0;
            if (cs >= 0) {
                glUniformMatrix3fv(cs, 1, GL_FALSE, id3);
                e[4] = glGetError();
                if (e[4]) {
                    glUniformMatrix4fv(cs, 1, GL_FALSE, id4);
                    e[5] = glGetError();
                }
            }
            logf_("  %s uniforms: mv 0x%x proj 0x%x texspace 0x%x sampler 0x%x colorspace mat3 0x%x mat4 0x%x",
                  i ? "blur" : "copy", e[0], e[1], e[2], e[3], e[4], e[5]);
        }
        if (pcopy) {
            /* Seed target 0, copy 0 -> 1 with the shader (same frame), read 1. */
            glBindTexture(GL_TEXTURE_2D, tex[0]);
            seed_halves(fbo[0], W, H);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]);
            glClearColor(0, 0, 1, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glUseProgram(pcopy);
            glBindTexture(GL_TEXTURE_2D, tex[0]);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            logf_("  copy draw error 0x%x", glGetError());
            frame_sync(dpy, surf, "copy");
            row_dump("copy", W, H);
        }
        if (pblur) {
            /* WebKit's kernel: weights for offsets 0..half-1 (normalised over
             * both sides), offsets in texels; direction in texture space. */
            enum { HALF = 6 }; /* u_gaussianKernel[6] in this build (glGetActiveUniform) */
            float kern[HALF], offs[HALF], sum = 0;
            const float sigma = 2.0f;
            for (int i = 0; i < HALF; ++i) {
                kern[i] = expf(-(float)(i * i) / (2 * sigma * sigma));
                offs[i] = (float)i;
                sum += i ? 2 * kern[i] : kern[i];
            }
            for (int i = 0; i < HALF; ++i)
                kern[i] /= sum;
            glUseProgram(pblur);
            glUniform1fv(glGetUniformLocation(pblur, "u_gaussianKernel"), HALF, kern);
            glUniform1fv(glGetUniformLocation(pblur, "u_gaussianKernelOffset"), HALF, offs);
            GLint hs = glGetUniformLocation(pblur, "u_gaussianKernelHalfSize");
            glUniform1i(hs, HALF);
            GLenum e1 = glGetError();
            if (e1)
                glUniform1f(hs, (float)HALF);
            glUniform2f(glGetUniformLocation(pblur, "u_texelSize"), 1.0f / W, 1.0f / H);
            GLint bd = glGetUniformLocation(pblur, "u_blurDirection");
            logf_("  blur uniforms: halfsize as int error 0x%x, then 0x%x; locations kern %d offs %d half %d dir %d "
                  "texel %d",
                  e1, glGetError(), glGetUniformLocation(pblur, "u_gaussianKernel"),
                  glGetUniformLocation(pblur, "u_gaussianKernelOffset"), hs, bd,
                  glGetUniformLocation(pblur, "u_texelSize"));
            /* Seed, then one two-pass blur 0 -> 1 -> 0 in the same frame; read
             * a row across the edge. */
            glBindTexture(GL_TEXTURE_2D, tex[0]);
            seed_halves(fbo[0], W, H);
            for (int pass = 0; pass < 2; ++pass) {
                glBindFramebuffer(GL_FRAMEBUFFER, fbo[pass ? 0 : 1]);
                glBindTexture(GL_TEXTURE_2D, tex[pass ? 1 : 0]);
                glUniform2f(bd, pass ? 0.0f : 1.0f / W, pass ? 1.0f / H : 0.0f);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                logf_("  blur pass %d draw error 0x%x", pass, glGetError());
                frame_sync(dpy, surf, pass ? "blur-v" : "blur-h");
                row_dump(pass ? "blur-v (in 0)" : "blur-h (in 1)", W, H);
            }
            /* Same, direction scaled x8 (in case the shader wants pixels). */
            glBindTexture(GL_TEXTURE_2D, tex[0]);
            seed_halves(fbo[0], W, H);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]);
            glUniform2f(bd, 8.0f, 0.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            frame_sync(dpy, surf, "blur-h8");
            row_dump("blur-h dir 8.0 (in 1)", W, H);
            {
                /* Identity kernel, one pass: must reproduce its input. Re-seed
                 * target 0 first (the blur above overwrote it). */
                float k1[HALF] = {1}, o1[HALF] = {0};
                glBindTexture(GL_TEXTURE_2D, tex[0]);
                seed_halves(fbo[0], W, H);
                glUniform1fv(glGetUniformLocation(pblur, "u_gaussianKernel"), HALF, k1);
                glUniform1fv(glGetUniformLocation(pblur, "u_gaussianKernelOffset"), HALF, o1);
                glUniform1i(hs, 1);
                glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]);
                glBindTexture(GL_TEXTURE_2D, tex[0]);
                glUniform2f(bd, 1.0f / W, 0.0f);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                frame_sync(dpy, surf, "blur-id");
                row_dump("blur-id", W, H);
                glUniform1fv(glGetUniformLocation(pblur, "u_gaussianKernel"), HALF, kern);
                glUniform1fv(glGetUniformLocation(pblur, "u_gaussianKernelOffset"), HALF, offs);
                glUniform1i(hs, HALF);
            }
            /* GPU cost: N two-pass full-page blurs in one frame. */
            int ns[3] = {1, 5, 20};
            for (int j = 0; j < 3; ++j) {
                double gsum = 0;
                for (int rep = 0; rep < 3; ++rep) {
                    for (int k = 0; k < ns[j]; ++k)
                        for (int pass = 0; pass < 2; ++pass) {
                            glBindFramebuffer(GL_FRAMEBUFFER, fbo[pass ? 0 : 1]);
                            glBindTexture(GL_TEXTURE_2D, tex[pass ? 1 : 0]);
                            glUniform2f(bd, pass ? 0.0f : 1.0f / W, pass ? 1.0f / H : 0.0f);
                            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                        }
                    gsum += frame_sync(dpy, surf, rep == 0 ? "blurN" : NULL);
                }
                logf_("  %2d two-pass 22-tap blurs %dx%d per frame: GPU after tick avg %.3f ms (%.3f per blur), "
                      "error 0x%x",
                      ns[j], W, H, gsum / 3, gsum / 3 / ns[j], glGetError());
            }
        }
        glUseProgram(0);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
    unsigned char *px = malloc((size_t)W * H * 4);
    if (px) {
        t0 = now_ms();
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
        t1 = now_ms();
        logf_("  glReadPixels %dx%d: %.1f ms, error 0x%x, pixel0 %02x %02x %02x %02x", W, H, t1 - t0,
              glGetError(), px[0], px[1], px[2], px[3]);
        glBindTexture(GL_TEXTURE_2D, tex[1]);
        t0 = now_ms();
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1792, 944, GL_RGBA, GL_UNSIGNED_BYTE, px);
        frame_end(dpy, surf, "upload");
        t1 = now_ms();
        logf_("  glTexSubImage2D 1792x944: %.1f ms, error 0x%x", t1 - t0, glGetError());
        free(px);
    }
    logf_("  before terminate: slot %d", our_slot());
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglDestroySurface(dpy, surf);
    eglTerminate(dpy);

out:
    compositor_bye("end of run");
    logf_("glslim probe: end");
    if (g_log)
        fclose(g_log);
    return 0;
}
