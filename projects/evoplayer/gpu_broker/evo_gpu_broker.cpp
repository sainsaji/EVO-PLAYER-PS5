/*
 * evo_gpu_broker.cpp - EVO's side of the vendored GPU broker (#237): address
 * placeholders, start on a worker thread, client endpoints, and the
 * Ps5GpuDisplay the broker presents client frames to. EVO never opens
 * VideoOut here: a presented frame is copied into EVO's own buffer (the
 * client may free its buffer right after the present returns).
 */
#include "evo_gpu_broker.h"

#include "display.h"
#include "protocol.h"
#include "server.h"
extern "C" {
#include "radv_ps5_platform.h"
}

extern "C" {
#include "evo_boot_log.h"
}

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int sceKernelReserveVirtualRange(void **addr, size_t len, int flags, size_t align);

#define TRIGGER "/mnt/usb0/evo_gpu_broker"
#define BROKER_DIR "/data/evoplayer-gpu"

namespace {

const uintptr_t kHostWindow = 0x240000000ull;
const size_t kHostWindowBytes = 512ull << 20;
const uintptr_t kHostHigh = 0x3000000000ull;
const size_t kHostHighBytes = 16ull << 30;
const int kReserveFixed = 0x10, kReserveNoOverwrite = 0x80;

pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
bool g_held[2];
int g_state;   // 0 idle/starting, 1 running, -1 failed
bool g_starting;

struct Client {
    int cookie, pid;
};
Client g_clients[32];
int g_nclients;

// The display: the newest presented frame, EVO's own copy.
pthread_mutex_t g_frame_mu = PTHREAD_MUTEX_INITIALIZER;
void *g_pixels;
size_t g_cap;
unsigned g_w, g_h, g_pitch;
bool g_bgra;
uint64_t g_frames;

bool trigger_present()
{
    FILE *f = fopen(TRIGGER, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

void hold(int i, uintptr_t base, size_t bytes)
{
    void *at = (void *)base;
    int rc = sceKernelReserveVirtualRange(&at, bytes, kReserveFixed | kReserveNoOverwrite, 16384);
    g_held[i] = rc == 0 && at == (void *)base;
    if (!g_held[i] && rc == 0 && at)
        munmap(at, bytes);
    EVO_LOGI("gpu", "placeholder %#lx + %zu MB: rc %#x at %p -> %s", (unsigned long)base, bytes >> 20,
             (unsigned)rc, at, g_held[i] ? "held" : "NOT held");
}

void release_placeholders()
{
    if (g_held[0])
        munmap((void *)kHostWindow, kHostWindowBytes);
    if (g_held[1])
        munmap((void *)kHostHigh, kHostHighBytes);
    g_held[0] = g_held[1] = false;
}

// The broker frees a client's memory only once this says the process is gone.
bool client_absent(int token)
{
    if (token <= 0)
        return false;
    if (kill(token, 0) == -1 && errno == ESRCH)
        return true;
    return false;
}

void *start_body(void *)
{
    const long long t0 = evo_log_mono_ms();
    pthread_mutex_lock(&g_mu);
    release_placeholders();
    pthread_mutex_unlock(&g_mu);
    mkdir(BROKER_DIR, 0777);
    ps5_gpu_broker_set_host_limit(PW_GPU_LIMIT);
    EVO_LOGI("gpu", "broker starting (dir %s)", BROKER_DIR);
    evo_boot_log_flush();
    const bool ok = ps5_gpu_broker_start(BROKER_DIR, false, true, client_absent);
    EVO_LOGI("gpu", "broker start -> %d in %lld ms (log %s/gpu-broker-results.txt)", ok ? 1 : 0,
             evo_log_mono_ms() - t0, BROKER_DIR);
    evo_boot_log_flush();
    pthread_mutex_lock(&g_mu);
    g_state = ok ? 1 : -1;
    g_starting = false;
    pthread_mutex_unlock(&g_mu);
    return nullptr;
}

} // namespace

// ---- the broker's display (display.h), no VideoOut ----

int Ps5GpuDisplay::present(const void *pixels, unsigned width, unsigned height, unsigned pitch, FILE *log,
                           const char *capture, bool bgra, int cursor_x, int cursor_y)
{
    (void)capture;
    (void)cursor_x;
    (void)cursor_y;
    if (!pixels || !width || !height || pitch < width * 4u)
        return -1;
    const size_t bytes = (size_t)pitch * height;
    pthread_mutex_lock(&g_frame_mu);
    if (g_cap < bytes) {
        void *grown = realloc(g_pixels, bytes);
        if (!grown) {
            pthread_mutex_unlock(&g_frame_mu);
            return -1;
        }
        g_pixels = grown;
        g_cap = bytes;
    }
    memcpy(g_pixels, pixels, bytes);
    g_w = width;
    g_h = height;
    g_pitch = pitch;
    g_bgra = bgra;
    const uint64_t n = ++g_frames;
    const uint32_t first = *(const uint32_t *)pixels;
    const uint32_t mid = *(const uint32_t *)((const uint8_t *)pixels + (size_t)pitch * (height / 2) + (width / 2) * 4u);
    pthread_mutex_unlock(&g_frame_mu);
    ++flips;
    if (n <= 4 || n % 120 == 0) {
        EVO_LOGI("gpu", "client frame %llu: %ux%u pitch %u bgra %d, first px %#x centre px %#x",
                 (unsigned long long)n, width, height, pitch, bgra ? 1 : 0, first, mid);
        if (log) {
            fprintf(log, "evo display frame %llu %ux%u\n", (unsigned long long)n, width, height);
            fflush(log);
        }
    }
    return 0;
}

bool Ps5GpuDisplay::close(FILE *log)
{
    if (log) {
        fprintf(log, "evo display close flips=%llu\n", (unsigned long long)flips);
        fflush(log);
    }
    return true;
}

bool Ps5GpuDisplay::capture_last(const char *path)
{
    (void)path;
    return false;
}

// ---- EVO API ----

extern "C" void evo_gpu_broker_reserve_early(void)
{
    /* Runs at main() entry, before the sandbox opens, so the trigger file
     * cannot be read yet: hold the ranges always (address space only) and
     * let evo_gpu_broker_release_unused() give them back. */
    pthread_mutex_lock(&g_mu);
    hold(0, kHostWindow, kHostWindowBytes);
    hold(1, kHostHigh, kHostHighBytes);
    pthread_mutex_unlock(&g_mu);
}

extern "C" void evo_gpu_broker_preinit(void)
{
    const long long t0 = evo_log_mono_ms();
    const bool ok = radv_ps5_platform_init();
    EVO_LOGI("gpu", "RADV platform init (pre-unjail) -> %d in %lld ms", ok ? 1 : 0, evo_log_mono_ms() - t0);
}

extern "C" void evo_gpu_broker_release_unused(void)
{
    if (trigger_present())
        return;
    pthread_mutex_lock(&g_mu);
    const bool had = g_held[0] || g_held[1];
    release_placeholders();
    pthread_mutex_unlock(&g_mu);
    if (had)
        EVO_LOGI("gpu", "no %s: placeholders released", TRIGGER);
}

extern "C" int evo_gpu_broker_start(void)
{
    pthread_mutex_lock(&g_mu);
    if (g_state == 1 || g_starting) {
        pthread_mutex_unlock(&g_mu);
        return 0;
    }
    g_starting = true;
    g_state = 0;
    pthread_mutex_unlock(&g_mu);
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 1u << 20);
    const int rc = pthread_create(&th, &attr, start_body, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        pthread_mutex_lock(&g_mu);
        g_starting = false;
        g_state = -1;
        pthread_mutex_unlock(&g_mu);
        EVO_LOGW("gpu", "broker start thread not created (%d)", rc);
        return -1;
    }
    return 0;
}

extern "C" int evo_gpu_broker_state(void)
{
    pthread_mutex_lock(&g_mu);
    const int s = g_state;
    pthread_mutex_unlock(&g_mu);
    return s;
}

extern "C" int evo_gpu_broker_open(int *cookie)
{
    if (evo_gpu_broker_state() != 1)
        return -1;
    int c = -1;
    const int fd = ps5_gpu_broker_open(&c);
    EVO_LOGI("gpu", "client endpoint fd %d cookie %d", fd, c);
    if (cookie)
        *cookie = c;
    return fd;
}

extern "C" void evo_gpu_broker_register(int cookie, int pid)
{
    pthread_mutex_lock(&g_mu);
    if (g_nclients < 32)
        g_clients[g_nclients++] = Client{cookie, pid};
    pthread_mutex_unlock(&g_mu);
    EVO_LOGI("gpu", "client cookie %d is pid %d", cookie, pid);
}

extern "C" uint64_t evo_gpu_broker_frames(void)
{
    pthread_mutex_lock(&g_frame_mu);
    const uint64_t n = g_frames;
    pthread_mutex_unlock(&g_frame_mu);
    return n;
}

extern "C" uint64_t evo_gpu_broker_last_frame(void *out, size_t cap, unsigned *w, unsigned *h, unsigned *pitch,
                                              int *bgra)
{
    pthread_mutex_lock(&g_frame_mu);
    uint64_t n = 0;
    const size_t bytes = (size_t)g_pitch * g_h;
    if (g_frames && out && cap >= bytes) {
        memcpy(out, g_pixels, bytes);
        if (w) *w = g_w;
        if (h) *h = g_h;
        if (pitch) *pitch = g_pitch;
        if (bgra) *bgra = g_bgra ? 1 : 0;
        n = g_frames;
    }
    pthread_mutex_unlock(&g_frame_mu);
    return n;
}
