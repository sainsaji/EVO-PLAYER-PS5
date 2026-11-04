/*
 * ProsperoWeb backend of the browser engine seam - spike S1 (#237): start the
 * broker through elfldr, hold its connection, drop it on soft close.
 * See include/evo_browser_engine.h.
 *
 * Broker protocol (third_party/ProsperoWeb/src/ps5/spawn/broker_proto.cpp):
 * frames are a 4-byte big-endian length and a JSON object. {"op":"ping"}
 * answers {"ok":true,"pid":N}. Before its first client the broker exits after
 * 30 s alone; after it, it lives exactly as long as that client's connection.
 */
#include "evo_browser_engine.h"
#include "evo_boot_log.h"
#include "evo_elfldr.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SPIKE_TRIGGER     "/mnt/usb0/evo_browser_spike"
#define SPIKE_DEFAULT_ELF "/mnt/usb0/prosperoweb-broker.elf"
#define BROKER_MAX_ELF    (64u << 20)
#define ELFLDR_SEND_MS    10000     /* the loader reads the whole ELF first */
#define BROKER_UP_MS      10000     /* broker binds 17771 after it starts */
#define BROKER_PING_MS    5000
#define POLL_STEP_MS      100

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_fd = -1;               /* the broker connection EVO holds */
static int g_started;               /* EVO sent a broker this run */
static int g_busy;                  /* the start worker is still running */
static int g_stop;                  /* soft close asked for the stop */
static int g_running;               /* ping answered, connection held */
static char g_path[256];

static int stop_requested(void)
{
    pthread_mutex_lock(&g_mu);
    const int s = g_stop;
    pthread_mutex_unlock(&g_mu);
    return s;
}

static unsigned char *read_elf(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char *buf = NULL;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    if (n > 4 && (unsigned long)n <= BROKER_MAX_ELF && fseek(f, 0, SEEK_SET) == 0) {
        buf = (unsigned char *)malloc((size_t)n);
        if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
            free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    if (buf && memcmp(buf, "\x7f" "ELF", 4) != 0) {
        free(buf);
        buf = NULL;
    }
    if (buf) *len = (size_t)n;
    return buf;
}

/* One request/reply on the broker connection. 0 and the pid on success. */
static int broker_ping(int s, int *pid)
{
    static const char req[] = "{\"op\":\"ping\"}";
    const unsigned len = (unsigned)(sizeof req - 1);
    unsigned char hdr[4] = {
        (unsigned char)(len >> 24), (unsigned char)(len >> 16),
        (unsigned char)(len >> 8), (unsigned char)len,
    };
    if (evo_sock_send_all(s, hdr, 4) != 0 || evo_sock_send_all(s, req, len) != 0)
        return -1;
    if (evo_sock_recv_all(s, hdr, 4) != 0) return -1;
    const unsigned rlen = ((unsigned)hdr[0] << 24) | ((unsigned)hdr[1] << 16) |
                          ((unsigned)hdr[2] << 8) | (unsigned)hdr[3];
    char reply[512];
    if (rlen == 0 || rlen >= sizeof reply) return -1;
    if (evo_sock_recv_all(s, reply, rlen) != 0) return -1;
    reply[rlen] = 0;
    if (!strstr(reply, "\"ok\":true")) {
        EVO_LOGW("browser", "broker ping refused: %s", reply);
        return -1;
    }
    const char *p = strstr(reply, "\"pid\":");
    *pid = p ? atoi(p + 6) : 0;
    return 0;
}

static void *start_worker(void *arg)
{
    (void)arg;
    const long long t0 = evo_log_mono_ms();
    size_t len = 0;
    unsigned char *elf = read_elf(g_path, &len);
    int s = -1;

    if (!elf) {
        EVO_LOGW("browser", "broker ELF %s missing, too big or not an ELF", g_path);
        goto done;
    }
    if (evo_elfldr_send(elf, len, ELFLDR_SEND_MS) != 0) {
        EVO_LOGW("browser", "no ELF loader on 127.0.0.1:%d - the browser needs one", EVO_ELFLDR_PORT);
        goto done;
    }
    pthread_mutex_lock(&g_mu);
    g_started = 1;
    pthread_mutex_unlock(&g_mu);
    EVO_LOGI("browser", "broker sent to the ELF loader (%zu bytes) in %lld ms",
             len, evo_log_mono_ms() - t0);

    for (int waited = 0; waited < BROKER_UP_MS && !stop_requested(); waited += POLL_STEP_MS) {
        s = evo_loopback_connect(EVO_BROWSER_BROKER_PORT, BROKER_PING_MS);
        if (s >= 0) break;
        usleep(POLL_STEP_MS * 1000);
    }
    if (s < 0) {
        if (!stop_requested())
            EVO_LOGW("browser", "broker did not answer on 127.0.0.1:%d within %d ms",
                     EVO_BROWSER_BROKER_PORT, BROKER_UP_MS);
        goto done;
    }
    int pid = 0;
    if (broker_ping(s, &pid) != 0) {
        EVO_LOGW("browser", "broker on 127.0.0.1:%d did not answer the ping", EVO_BROWSER_BROKER_PORT);
        close(s);   /* a closed client connection makes the broker exit */
        s = -1;
        goto done;
    }
    pthread_mutex_lock(&g_mu);
    if (g_stop) {
        close(s);
    } else {
        g_fd = s;
        g_running = 1;
    }
    pthread_mutex_unlock(&g_mu);
    EVO_LOGI("browser", "broker pid %d answers on 127.0.0.1:%d, %lld ms after the start",
             pid, EVO_BROWSER_BROKER_PORT, evo_log_mono_ms() - t0);

done:
    free(elf);
    pthread_mutex_lock(&g_mu);
    g_busy = 0;
    pthread_mutex_unlock(&g_mu);
    evo_boot_log_flush();
    return NULL;
}

int evo_browser_engine_start(const char *broker_elf_path)
{
    pthread_mutex_lock(&g_mu);
    if (g_busy || g_fd >= 0 || g_stop) {
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    g_busy = 1;
    pthread_mutex_unlock(&g_mu);

    /* Someone else's broker (a standalone ProsperoWeb) must not be taken
     * over: connecting would replace its client and kill its children. */
    if (evo_loopback_port_open(EVO_BROWSER_BROKER_PORT)) {
        EVO_LOGW("browser", "something already answers on 127.0.0.1:%d - not starting a broker",
                 EVO_BROWSER_BROKER_PORT);
        pthread_mutex_lock(&g_mu);
        g_busy = 0;
        pthread_mutex_unlock(&g_mu);
        return -1;
    }

    snprintf(g_path, sizeof g_path, "%s", broker_elf_path);
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    const int rc = pthread_create(&th, &attr, start_worker, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        pthread_mutex_lock(&g_mu);
        g_busy = 0;
        pthread_mutex_unlock(&g_mu);
        EVO_LOGW("browser", "start worker not created (%d)", rc);
        return -1;
    }
    return 0;
}

void evo_browser_engine_boot(void)
{
    FILE *f = fopen(SPIKE_TRIGGER, "r");
    if (!f) return;
    char path[256] = {0};
    if (!fgets(path, sizeof path, f)) path[0] = 0;
    fclose(f);
    path[strcspn(path, "\r\n")] = 0;
    if (!path[0]) snprintf(path, sizeof path, "%s", SPIKE_DEFAULT_ELF);
    EVO_LOGI("browser", "spike S1: starting the broker from %s", path);
    evo_browser_engine_start(path);
}

int evo_browser_engine_running(void)
{
    pthread_mutex_lock(&g_mu);
    const int r = g_running && g_fd >= 0;
    pthread_mutex_unlock(&g_mu);
    return r;
}

void evo_browser_engine_stop(void)
{
    pthread_mutex_lock(&g_mu);
    g_stop = 1;
    const int fd = g_fd;
    g_fd = -1;
    g_running = 0;
    const int started = g_started || g_busy;
    pthread_mutex_unlock(&g_mu);
    if (fd >= 0) close(fd);   /* the broker terminates its children and exits */
    if (started)
        EVO_LOGI("browser", "stop: broker connection %s", fd >= 0 ? "closed" : "was not held");
}

int evo_browser_engine_wait_stopped(int timeout_ms)
{
    const long long t0 = evo_log_mono_ms();
    int waited = 0;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        const int busy = g_busy;
        pthread_mutex_unlock(&g_mu);
        if (!busy || waited >= timeout_ms) break;
        usleep(POLL_STEP_MS * 1000);
        waited += POLL_STEP_MS;
    }
    pthread_mutex_lock(&g_mu);
    const int started = g_started, busy = g_busy;
    pthread_mutex_unlock(&g_mu);
    if (!started && !busy) return 0;

    /* A broker that never got EVO as its client (the stop came while EVO was
     * still waiting for it) exits on the probe's own connect-and-close. */
    while (evo_loopback_port_open(EVO_BROWSER_BROKER_PORT)) {
        if (waited >= timeout_ms) {
            EVO_LOGW("browser", "broker still answers on 127.0.0.1:%d after %lld ms%s",
                     EVO_BROWSER_BROKER_PORT, evo_log_mono_ms() - t0,
                     busy ? " (start worker still running)" : "");
            return -1;
        }
        usleep(POLL_STEP_MS * 1000);
        waited += POLL_STEP_MS;
    }
    EVO_LOGI("browser", "broker gone (127.0.0.1:%d refused) after %lld ms",
             EVO_BROWSER_BROKER_PORT, evo_log_mono_ms() - t0);
    return busy ? -1 : 0;
}
