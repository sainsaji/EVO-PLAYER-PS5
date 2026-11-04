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
#include "evo_browser_wire.h"
#include "evo_boot_log.h"
#include "evo_elfldr.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>

#define SPIKE_TRIGGER     "/mnt/usb0/evo_browser_spike"
#define SPIKE_DEFAULT_ELF "/mnt/usb0/prosperoweb-broker.elf"
#define SPIKE_DEFAULT_URL "https://example.com/"
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
static int g_spike;                 /* started by the trigger file, not the UI */
static char g_engine_elf[256];      /* prosperoweb.elf, empty: none found */
static char g_url[512];             /* a page to load once the engine is ready */
static int g_url_pending;
static char g_fail[160];            /* why the engine is not available */

/* Page events for the main thread (evo_browser_poll_event). */
#define EVENT_RING 32
static evo_browser_event_t g_events[EVENT_RING];
static int g_ev_head, g_ev_count;

static void set_fail(const char *why)
{
    pthread_mutex_lock(&g_mu);
    if (!g_fail[0]) snprintf(g_fail, sizeof g_fail, "%s", why);
    pthread_mutex_unlock(&g_mu);
}

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

static int broker_send(int s, const char *req)
{
    const unsigned len = (unsigned)strlen(req);
    unsigned char hdr[4] = {
        (unsigned char)(len >> 24), (unsigned char)(len >> 16),
        (unsigned char)(len >> 8), (unsigned char)len,
    };
    return (evo_sock_send_all(s, hdr, 4) == 0 && evo_sock_send_all(s, req, len) == 0) ? 0 : -1;
}

/* The next reply frame as a NUL-terminated string; 0 on success. */
static int broker_recv(int s, char *reply, size_t cap)
{
    unsigned char hdr[4];
    if (evo_sock_recv_all(s, hdr, 4) != 0) return -1;
    const unsigned rlen = ((unsigned)hdr[0] << 24) | ((unsigned)hdr[1] << 16) |
                          ((unsigned)hdr[2] << 8) | (unsigned)hdr[3];
    if (rlen == 0 || rlen >= cap) return -1;
    if (evo_sock_recv_all(s, reply, rlen) != 0) return -1;
    reply[rlen] = 0;
    return 0;
}

/* One request/reply on the broker connection. 0 and the pid on success. */
static int broker_ping(int s, int *pid)
{
    char reply[512];
    if (broker_send(s, "{\"op\":\"ping\"}") != 0 || broker_recv(s, reply, sizeof reply) != 0)
        return -1;
    if (!strstr(reply, "\"ok\":true")) {
        EVO_LOGW("browser", "broker ping refused: %s", reply);
        return -1;
    }
    const char *p = strstr(reply, "\"pid\":");
    *pid = p ? atoi(p + 6) : 0;
    return 0;
}

/* ---- spike S2a: the frame transport, with the broker's own self-test ----
 *
 * EVO makes a SEQPACKET socketpair and names one end to the broker
 * (op ipc-selftest, src_pid + src_fd); the broker copies it with rdup, sends
 * "ping", takes an SHM_ANON object from EVO by SCM_RIGHTS and checks it, then
 * sends its own, which EVO maps and checks. That is the whole path a frame
 * buffer takes (docs/ui/browser.md). Patterns and size are the broker's
 * (third_party/ProsperoWeb/src/ps5/spawn/ipc_selftest.cpp). */
#define IPC_SHM_SIZE     0x4000
#define IPC_STEP_MS      6000      /* the broker waits 4 s per step */
#define IPC_EVO_PATTERN  "PRWB-UI-SHM"
#define IPC_PAYLOAD_PATTERN "PRWB-BROKER-SHM"

static int shm_with_pattern(const char *pattern)
{
    const int fd = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return -1;
    void *p = MAP_FAILED;
    if (ftruncate(fd, IPC_SHM_SIZE) == 0)
        p = mmap(NULL, IPC_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        return -1;
    }
    memcpy(p, pattern, strlen(pattern) + 1);
    munmap(p, IPC_SHM_SIZE);
    return fd;
}

static int send_fd(int sock, int fd)
{
    char byte = 'F';
    struct iovec iov = {&byte, 1};
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } ctl;
    memset(&ctl, 0, sizeof ctl);
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = ctl.buf;
    m.msg_controllen = sizeof ctl.buf;
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    return sendmsg(sock, &m, 0) == 1 ? 0 : -1;
}

/* The descriptor that came with the next message, or -1 (*why). */
static int recv_fd(int sock, const char **why)
{
    char byte = 0;
    struct iovec iov = {&byte, 1};
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } ctl;
    memset(&ctl, 0, sizeof ctl);
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = ctl.buf;
    m.msg_controllen = sizeof ctl.buf;
    if (recvmsg(sock, &m, 0) < 0) { *why = "recvmsg failed or timed out"; return -1; }
    if (m.msg_flags & MSG_CTRUNC) { *why = "control data truncated"; return -1; }
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int fd = -1;
            memcpy(&fd, CMSG_DATA(c), sizeof(int));
            return fd;
        }
    }
    *why = "message came without a descriptor";
    return -1;
}

static void ipc_selftest(int broker)
{
    const long long t0 = evo_log_mono_ms();
    int sv[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
        EVO_LOGW("browser", "S2a: socketpair(SEQPACKET) failed (errno %d)", errno);
        return;
    }
    struct timeval tv = {IPC_STEP_MS / 1000, 0};
    setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(sv[0], SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    const int own = shm_with_pattern(IPC_EVO_PATTERN);
    EVO_LOGI("browser", "S2a: shm_open(SHM_ANON)+mmap in EVO: %s", own >= 0 ? "ok" : "FAILED");

    char req[128];
    snprintf(req, sizeof req, "{\"op\":\"ipc-selftest\",\"src_pid\":%d,\"src_fd\":%d}",
             (int)getpid(), sv[1]);
    if (broker_send(broker, req) != 0) {
        EVO_LOGW("browser", "S2a: request to the broker failed");
        goto out;
    }
    /* The broker answers on the TCP connection only after the steps below,
     * so they run first, on the pair. */
    char ping[16] = {0};
    const ssize_t n = recv(sv[0], ping, sizeof ping - 1, 0);
    EVO_LOGI("browser", "S2a: broker -> EVO over the rdup'ed end: %s",
             n > 0 ? ping : "nothing (rdup failed or timed out)");
    if (n > 0) {
        EVO_LOGI("browser", "S2a: EVO -> broker SCM_RIGHTS (shm): %s",
                 own < 0 ? "no shm" : send_fd(sv[0], own) == 0 ? "sent" : "sendmsg FAILED");
        const char *why = "";
        const int got = recv_fd(sv[0], &why);
        if (got < 0) {
            EVO_LOGW("browser", "S2a: broker -> EVO SCM_RIGHTS: %s", why);
        } else {
            void *p = mmap(NULL, IPC_SHM_SIZE, PROT_READ, MAP_SHARED, got, 0);
            const int same = p != MAP_FAILED &&
                strncmp((const char *)p, IPC_PAYLOAD_PATTERN, strlen(IPC_PAYLOAD_PATTERN)) == 0;
            EVO_LOGI("browser", "S2a: broker -> EVO SCM_RIGHTS: fd %d, map %s",
                     got, p == MAP_FAILED ? "FAILED" : same ? "ok, pattern matches" : "ok, pattern DIFFERS");
            if (p != MAP_FAILED) munmap(p, IPC_SHM_SIZE);
            close(got);
        }
    }
    {
        char report[2048];
        if (broker_recv(broker, report, sizeof report) == 0)
            EVO_LOGI("browser", "S2a: broker report: %s", report);
        else
            EVO_LOGW("browser", "S2a: no broker report");
    }
    EVO_LOGI("browser", "S2a: done in %lld ms", evo_log_mono_ms() - t0);
out:
    if (own >= 0) close(own);
    close(sv[0]);
    close(sv[1]);
}

/* ---- spike S2b: frames from a test host (helpers/frame_test.c) ----
 *
 * When /mnt/usb0/evo_frame_test.elf exists, the broker spawns it with EVO's
 * end of a new socketpair at fd 3 (src_pid + src_fds, the same rdup route as
 * S2a and as the real engine host later). A thread here receives HELLO and
 * FRAME; the UI takes frames on the main thread (evo_browser_frame_*) and
 * draws them as a texture (ui_kit evo_hui_render_webframe).
 *
 * Phase 1 spike: when /mnt/usb0/prosperoweb.elf exists, the broker spawns
 * that instead, as `prosperoweb.elf --evo-host` (our ProsperoWeb fork). Its
 * WebKit then takes over the broker connection to spawn WebProcess and
 * NetworkProcess, so from then on fd 3 is what keeps the engine alive: EVO
 * closing its end ends the host, and the broker follows. After HELLO, EVO
 * sends LOAD with the trigger file's second line (default example.com),
 * logs the page events, and scrolls once when the first load finishes. */
#define FRAME_TEST_ELF  "/mnt/usb0/evo_frame_test.elf"
#define ENGINE_HOST_ELF "/mnt/usb0/prosperoweb.elf"
#define FRAME_RECV_MS   250        /* the receive thread checks the stop this often */

static int g_fsock = -1;
static unsigned char *g_fbuf[EVOB_MAX_BUFS];
static size_t g_fbuf_len;
static int g_fw, g_fh, g_fstride;
static int g_hello, g_fclosed, g_frame_busy;
static int g_engine_host;           /* the channel is the real engine, not the test */
static evo_browser_frame_t g_pending;
static int g_has_pending;
static long long g_frames_in, g_frames_drawn, g_copy_us, g_copy_bytes;

/* One packet: a fixed struct, then text (cut to fit EVOB_MAX_PACKET). */
static int frame_send_text(int s, int type, const void *body, uint32_t len,
                           const char *text, size_t text_len)
{
    unsigned char pkt[EVOB_MAX_PACKET];
    if (sizeof(struct evob_hdr) + len > sizeof pkt) return -1;
    const size_t room = sizeof pkt - sizeof(struct evob_hdr) - len;
    if (text_len > room) text_len = room;
    const struct evob_hdr h = {EVOB_MAGIC, EVOB_VERSION, (uint16_t)type, (uint32_t)(len + text_len)};
    memcpy(pkt, &h, sizeof h);
    if (len) memcpy(pkt + sizeof h, body, len);
    if (text_len) memcpy(pkt + sizeof h + len, text, text_len);
    const size_t n = sizeof h + len + text_len;
    /* Never waits: the main thread sends most of these, and the host reads
     * all the time. A full socket drops the packet. */
    const ssize_t r = send(s, pkt, n, MSG_DONTWAIT);
    if (r == (ssize_t)n) return 0;
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        EVO_LOGW("browser", "engine not reading: packet type %d dropped", type);
    return -1;
}

static int frame_send(int s, int type, const void *body, uint32_t len)
{
    return frame_send_text(s, type, body, len, NULL, 0);
}

static void handle_event(const unsigned char *body, size_t len)
{
    struct evob_event ev;
    if (len < sizeof ev) return;
    memcpy(&ev, body, sizeof ev);
    const char *text = (const char *)body + sizeof ev;
    int text_len = (int)(len - sizeof ev);
    if (text_len > 300) text_len = 300;   /* the log line, not the event */
    EVO_LOGI("browser", "engine event %u arg %u: %.*s", ev.kind, ev.arg, text_len, text);

    pthread_mutex_lock(&g_mu);
    evo_browser_event_t *e;
    if (g_ev_count == EVENT_RING) {   /* full: the oldest goes */
        g_ev_head = (g_ev_head + 1) % EVENT_RING;
        g_ev_count--;
    }
    e = &g_events[(g_ev_head + g_ev_count) % EVENT_RING];
    g_ev_count++;
    e->kind = (int)ev.kind;
    e->arg = ev.arg;
    size_t n = len - sizeof ev;
    if (n >= sizeof e->text) n = sizeof e->text - 1;
    memcpy(e->text, text, n);
    e->text[n] = 0;
    pthread_mutex_unlock(&g_mu);

    if (g_spike && ev.kind == EVOB_EV_LOAD_FINISHED) {
        static int scrolled;
        if (!scrolled) {   /* the input path, once: a wheel step down mid-page */
            scrolled = 1;
            const struct evob_scroll sc = {960, 540, 0, 400};
            EVO_LOGI("browser", "Phase 1: page loaded, scrolling once (%s)",
                     frame_send(g_fsock, EVOB_SCROLL, &sc, sizeof sc) == 0 ? "sent" : "send failed");
        }
    }
}

static void frame_log_stats(const char *when)
{
    pthread_mutex_lock(&g_mu);
    const long long in = g_frames_in, drawn = g_frames_drawn, us = g_copy_us, bytes = g_copy_bytes;
    pthread_mutex_unlock(&g_mu);
    EVO_LOGI("browser", "S2b %s: frames received %lld, drawn %lld, copy avg %lld us / %lld KB",
             when, in, drawn, drawn ? us / drawn : 0, drawn ? bytes / drawn / 1024 : 0);
}

static void handle_hello(const unsigned char *body, size_t len, const int *fds, int nfds)
{
    struct evob_hello h;
    if (len >= sizeof h) memcpy(&h, body, sizeof h);
    if (len >= sizeof h && h.nbuf == 0 && nfds == 0) {
        /* the answer to RESIZE: same buffers, a new page size */
        pthread_mutex_lock(&g_mu);
        const int ok = g_hello && (size_t)h.stride == (size_t)g_fstride &&
                       (size_t)h.stride * h.height <= g_fbuf_len && h.width * 4 <= h.stride;
        if (ok) {
            g_fw = (int)h.width;
            g_fh = (int)h.height;
        }
        pthread_mutex_unlock(&g_mu);
        EVO_LOGI("browser", "host page size %ux%u%s", h.width, h.height, ok ? "" : " refused");
        return;
    }
    if (len < sizeof h || nfds != EVOB_MAX_BUFS) {
        EVO_LOGW("browser", "S2b: bad HELLO (%zu bytes, %d fds)", len, nfds);
        return;
    }
    if (h.width == 0 || h.width > 3840 || h.height == 0 || h.height > 2160 ||
        h.stride < h.width * 4 || h.format != EVOB_RGBA8_PREMUL || h.nbuf != EVOB_MAX_BUFS) {
        EVO_LOGW("browser", "S2b: HELLO refused: %ux%u stride %u format %u nbuf %u",
                 h.width, h.height, h.stride, h.format, h.nbuf);
        return;
    }
    const size_t bytes = (size_t)h.stride * h.height;
    for (int i = 0; i < EVOB_MAX_BUFS; ++i) {
        void *p = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fds[i], 0);
        if (p == MAP_FAILED) {
            EVO_LOGW("browser", "S2b: map_fail on frame buffer %d (%zu bytes, errno %d)", i, bytes, errno);
            for (int j = 0; j < i; ++j) munmap(g_fbuf[j], bytes);
            return;
        }
        g_fbuf[i] = (unsigned char *)p;
    }
    pthread_mutex_lock(&g_mu);
    g_fbuf_len = bytes;
    g_fw = (int)h.width;
    g_fh = (int)h.height;
    g_fstride = (int)h.stride;
    g_hello = 1;
    pthread_mutex_unlock(&g_mu);
    EVO_LOGI("browser", "S2b: host HELLO %ux%u stride %u, %d buffers mapped (%zu KB each)",
             h.width, h.height, h.stride, EVOB_MAX_BUFS, bytes / 1024);
    if (g_engine_host) {
        char url[sizeof g_url];
        pthread_mutex_lock(&g_mu);
        const int pending = g_url_pending;
        g_url_pending = 0;
        snprintf(url, sizeof url, "%s", g_url);
        pthread_mutex_unlock(&g_mu);
        if (pending) {
            const int r = frame_send_text(g_fsock, EVOB_LOAD, NULL, 0, url, strlen(url));
            EVO_LOGI("browser", "LOAD %s %s", url, r == 0 ? "sent" : "send failed");
        }
    }
}

static void handle_frame(const unsigned char *body, size_t len)
{
    struct evob_frame f;
    if (len < sizeof f) return;
    memcpy(&f, body, sizeof f);
    if (f.buf >= EVOB_MAX_BUFS || f.nrects > EVOB_MAX_RECTS) return;
    evo_browser_frame_t out;
    memset(&out, 0, sizeof out);
    out.buf = (int)f.buf;
    out.seq = f.seq;
    out.nrects = (int)f.nrects;
    for (uint32_t i = 0; i < f.nrects; ++i) {
        out.rects[i][0] = f.rects[i].x;
        out.rects[i][1] = f.rects[i].y;
        out.rects[i][2] = f.rects[i].w;
        out.rects[i][3] = f.rects[i].h;
    }
    int superseded = -1;
    pthread_mutex_lock(&g_mu);
    if (!g_hello) {
        pthread_mutex_unlock(&g_mu);
        return;
    }
    out.pixels = g_fbuf[f.buf];
    out.width = g_fw;
    out.height = g_fh;
    out.stride = g_fstride;
    if (g_has_pending) superseded = g_pending.buf;
    g_pending = out;
    g_has_pending = 1;
    g_frames_in++;
    pthread_mutex_unlock(&g_mu);
    if (superseded >= 0) {   /* never drawn: the host may paint into it again */
        const struct evob_release r = {(uint32_t)superseded};
        frame_send(g_fsock, EVOB_RELEASE, &r, sizeof r);
    }
}

static void *frame_worker(void *arg)
{
    (void)arg;
    const int s = g_fsock;
    long long last_log = evo_log_mono_ms();
    const char *why = "stopped";
    while (!stop_requested()) {
        unsigned char pkt[EVOB_MAX_PACKET];
        struct iovec iov = {pkt, sizeof pkt};
        union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int) * EVOB_MAX_BUFS)]; } ctl;
        memset(&ctl, 0, sizeof ctl);
        struct msghdr m;
        memset(&m, 0, sizeof m);
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        m.msg_control = ctl.buf;
        m.msg_controllen = sizeof ctl.buf;
        const ssize_t n = recvmsg(s, &m, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (evo_log_mono_ms() - last_log >= 5000) {
                frame_log_stats("running");
                last_log = evo_log_mono_ms();
            }
            continue;
        }
        if (n <= 0) { why = n == 0 ? "host closed the channel" : "receive error"; break; }

        int fds[EVOB_MAX_BUFS], nfds = 0;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            const int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int i = 0; i < k && nfds < EVOB_MAX_BUFS; ++i)
                memcpy(&fds[nfds++], CMSG_DATA(c) + i * sizeof(int), sizeof(int));
        }
        struct evob_hdr h;
        if ((size_t)n >= sizeof h) {
            memcpy(&h, pkt, sizeof h);
            const size_t body = (size_t)n - sizeof h;
            if (h.magic != EVOB_MAGIC || h.len > body) {
                EVO_LOGW("browser", "S2b: bad packet (%zd bytes)", n);
            } else if (h.type == EVOB_HELLO) {
                handle_hello(pkt + sizeof h, h.len, fds, nfds);
            } else if (h.type == EVOB_FRAME) {
                handle_frame(pkt + sizeof h, h.len);
            } else if (h.type == EVOB_EVENT) {
                handle_event(pkt + sizeof h, h.len);
            } else if (h.type == EVOB_BYE) {
                why = "host said BYE";
                for (int i = 0; i < nfds; ++i) close(fds[i]);
                break;
            }
        }
        for (int i = 0; i < nfds; ++i) close(fds[i]);   /* mapped (or refused) by now */
        if (evo_log_mono_ms() - last_log >= 5000) {
            frame_log_stats("running");
            last_log = evo_log_mono_ms();
        }
    }
    EVO_LOGI("browser", "S2b: frame channel ends: %s", why);
    frame_log_stats("total");
    pthread_mutex_lock(&g_mu);
    g_fclosed = 1;
    g_hello = 0;
    g_has_pending = 0;
    const int stopping = g_stop;
    pthread_mutex_unlock(&g_mu);
    /* After a stop the main thread no longer draws (the stop runs on it), so
     * the buffers can go. A host that died on its own leaves them mapped:
     * the main thread might be copying from one right now. */
    if (stopping) {
        for (int i = 0; i < EVOB_MAX_BUFS; ++i)
            if (g_fbuf[i]) munmap(g_fbuf[i], g_fbuf_len);
        close(s);
    }
    pthread_mutex_lock(&g_mu);
    g_frame_busy = 0;
    pthread_mutex_unlock(&g_mu);
    evo_boot_log_flush();
    return NULL;
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void frame_test_start(int broker)
{
    g_engine_host = g_engine_elf[0] != 0;
    if (!g_engine_host && !(g_spike && file_exists(FRAME_TEST_ELF))) {
        set_fail("The browser engine (prosperoweb.elf) is not installed");
        return;
    }
    const char *what = g_engine_host ? "engine host" : "frame test";
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
        EVO_LOGW("browser", "S2b: socketpair failed (errno %d)", errno);
        return;
    }
    struct timeval rtv = {0, FRAME_RECV_MS * 1000}, stv = {1, 0};
    setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof rtv);
    setsockopt(sv[0], SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof stv);

    char req[384], reply[512];
    if (g_engine_host) {
        /* argv[0] is the ELF's absolute path: the host re-runs the same ELF
         * as its WebProcess and NetworkProcess (ProsperoWeb main.cpp,
         * export_self_path). */
        snprintf(req, sizeof req,
                 "{\"op\":\"spawn\",\"elf\":\"%s\",\"argv\":[\"%s\",\"--evo-host\"],"
                 "\"env\":[],\"nfds\":0,\"src_pid\":%d,\"src_fds\":[%d]}",
                 g_engine_elf, g_engine_elf, (int)getpid(), sv[1]);
    } else {
        snprintf(req, sizeof req,
                 "{\"op\":\"spawn\",\"elf\":\"" FRAME_TEST_ELF "\",\"argv\":[\"evo-frame-test\"],"
                 "\"env\":[],\"nfds\":0,\"src_pid\":%d,\"src_fds\":[%d]}",
                 (int)getpid(), sv[1]);
    }
    const long long t0 = evo_log_mono_ms();
    const int ok = broker_send(broker, req) == 0 && broker_recv(broker, reply, sizeof reply) == 0;
    close(sv[1]);   /* the child has its own copy (rdup) once the broker answered */
    if (!ok || !strstr(reply, "\"ok\":true")) {
        EVO_LOGW("browser", "S2b: spawn of the %s failed: %s", what, ok ? reply : "no answer");
        set_fail("The browser engine could not be started");
        close(sv[0]);
        return;
    }
    const char *p = strstr(reply, "\"pid\":");
    EVO_LOGI("browser", "S2b: %s spawned, pid %d, in %lld ms", what,
             p ? atoi(p + 6) : 0, evo_log_mono_ms() - t0);

    g_fsock = sv[0];
    pthread_mutex_lock(&g_mu);
    g_frame_busy = 1;
    pthread_mutex_unlock(&g_mu);
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    const int rc = pthread_create(&th, &attr, frame_worker, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        EVO_LOGW("browser", "S2b: frame thread not created (%d)", rc);
        pthread_mutex_lock(&g_mu);
        g_frame_busy = 0;
        pthread_mutex_unlock(&g_mu);
        close(sv[0]);
        g_fsock = -1;
    }
}

int evo_browser_frame_shown(void)
{
    pthread_mutex_lock(&g_mu);
    const int r = g_hello && !g_fclosed && !g_stop;
    pthread_mutex_unlock(&g_mu);
    return r;
}

int evo_browser_frame_acquire(evo_browser_frame_t *out)
{
    pthread_mutex_lock(&g_mu);
    const int r = g_has_pending && g_hello && !g_stop;
    if (r) {
        *out = g_pending;
        g_has_pending = 0;
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}

void evo_browser_frame_release(int buf)
{
    pthread_mutex_lock(&g_mu);
    const int open = !g_fclosed && !g_stop && g_fsock >= 0;
    pthread_mutex_unlock(&g_mu);
    if (!open || buf < 0 || buf >= EVOB_MAX_BUFS) return;
    const struct evob_release r = {(uint32_t)buf};
    frame_send(g_fsock, EVOB_RELEASE, &r, sizeof r);
}

void evo_browser_frame_note_copy(long long copy_us, long long bytes)
{
    pthread_mutex_lock(&g_mu);
    g_frames_drawn++;
    g_copy_us += copy_us;
    g_copy_bytes += bytes;
    pthread_mutex_unlock(&g_mu);
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
        set_fail("The browser engine (prosperoweb-broker.elf) is not installed");
        goto done;
    }
    if (evo_elfldr_send(elf, len, ELFLDR_SEND_MS) != 0) {
        EVO_LOGW("browser", "no ELF loader on 127.0.0.1:%d - the browser needs one", EVO_ELFLDR_PORT);
        set_fail("The browser needs an ELF loader (elfldr on port 9021)");
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
        set_fail("The browser engine did not start");
        goto done;
    }
    int pid = 0;
    if (broker_ping(s, &pid) != 0) {
        EVO_LOGW("browser", "broker on 127.0.0.1:%d did not answer the ping", EVO_BROWSER_BROKER_PORT);
        set_fail("The browser engine did not answer");
        close(s);   /* a closed client connection makes the broker exit */
        s = -1;
        goto done;
    }
    EVO_LOGI("browser", "broker pid %d answers on 127.0.0.1:%d, %lld ms after the start",
             pid, EVO_BROWSER_BROKER_PORT, evo_log_mono_ms() - t0);
    if (g_spike && !stop_requested())
        ipc_selftest(s);
    if (!stop_requested())
        frame_test_start(s);
    pthread_mutex_lock(&g_mu);
    if (g_stop) {
        close(s);
    } else {
        g_fd = s;
        g_running = 1;
    }
    pthread_mutex_unlock(&g_mu);

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
        set_fail("Another browser is running (close ProsperoWeb first)");
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
    /* line 2 (Phase 1): the page the engine host loads */
    if (!fgets(g_url, sizeof g_url, f)) g_url[0] = 0;
    fclose(f);
    path[strcspn(path, "\r\n")] = 0;
    g_url[strcspn(g_url, "\r\n")] = 0;
    if (!path[0]) snprintf(path, sizeof path, "%s", SPIKE_DEFAULT_ELF);
    if (!g_url[0]) snprintf(g_url, sizeof g_url, "%s", SPIKE_DEFAULT_URL);
    g_url_pending = 1;
    g_spike = 1;
    if (file_exists(ENGINE_HOST_ELF))
        snprintf(g_engine_elf, sizeof g_engine_elf, "%s", ENGINE_HOST_ELF);
    EVO_LOGI("browser", "spike S1: starting the broker from %s", path);
    evo_browser_engine_start(path);
}

int evo_browser_spike_overlay(void)
{
    return g_spike;
}

/* ---- Phase 2: the Browser section's calls ---- */

/* dir/name if it exists, else empty. */
static int find_elf(const char *name, char *out, size_t cap)
{
    static const char *const dirs[] = EVO_BROWSER_ELF_DIRS;
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; ++i) {
        snprintf(out, cap, "%s/%s", dirs[i], name);
        if (file_exists(out)) return 1;
    }
    out[0] = 0;
    return 0;
}

void evo_browser_open(void)
{
    pthread_mutex_lock(&g_mu);
    const int active = g_busy || g_fd >= 0 || g_stop || (g_hello && !g_fclosed);
    pthread_mutex_unlock(&g_mu);
    if (active) return;

    char broker[256];
    pthread_mutex_lock(&g_mu);
    g_fail[0] = 0;
    pthread_mutex_unlock(&g_mu);
    if (!find_elf("prosperoweb-broker.elf", broker, sizeof broker)) {
        set_fail("The browser engine (prosperoweb-broker.elf) is not installed");
        return;
    }
    if (!find_elf("prosperoweb.elf", g_engine_elf, sizeof g_engine_elf)) {
        set_fail("The browser engine (prosperoweb.elf) is not installed");
        return;
    }
    EVO_LOGI("browser", "opening: broker %s, engine %s", broker, g_engine_elf);
    evo_browser_engine_start(broker);
}

evo_browser_state_t evo_browser_state(char *why, int why_cap)
{
    pthread_mutex_lock(&g_mu);
    evo_browser_state_t st;
    if (g_fail[0])
        st = EVO_BROWSER_FAILED;
    else if (g_hello && !g_fclosed)
        st = EVO_BROWSER_READY;
    else if (g_fclosed)
        st = EVO_BROWSER_FAILED;
    else if (g_busy || g_fd >= 0)
        st = EVO_BROWSER_STARTING;
    else
        st = EVO_BROWSER_OFF;
    if (why && why_cap > 0) {
        snprintf(why, (size_t)why_cap, "%s",
                 g_fail[0] ? g_fail : (st == EVO_BROWSER_FAILED ? "The browser engine stopped" : ""));
    }
    pthread_mutex_unlock(&g_mu);
    return st;
}

/* The channel, if commands can go now. */
static int channel(void)
{
    pthread_mutex_lock(&g_mu);
    const int s = (g_hello && !g_fclosed && !g_stop && g_engine_host) ? g_fsock : -1;
    pthread_mutex_unlock(&g_mu);
    return s;
}

void evo_browser_load(const char *url)
{
    if (!url || !url[0]) return;
    const int s = channel();
    if (s >= 0) {
        frame_send_text(s, EVOB_LOAD, NULL, 0, url, strlen(url));
        EVO_LOGI("browser", "LOAD %s", url);
        return;
    }
    pthread_mutex_lock(&g_mu);
    snprintf(g_url, sizeof g_url, "%s", url);
    g_url_pending = 1;
    pthread_mutex_unlock(&g_mu);
}

void evo_browser_nav(int op)
{
    const int s = channel();
    const struct evob_nav n = {(uint32_t)op};
    if (s >= 0) frame_send(s, EVOB_NAV, &n, sizeof n);
}

void evo_browser_pointer(int x, int y, int action, int button)
{
    const int s = channel();
    const struct evob_pointer p = {x, y, (uint32_t)action, (uint32_t)button};
    if (s >= 0) frame_send(s, EVOB_POINTER, &p, sizeof p);
}

void evo_browser_scroll(int x, int y, int dx, int dy)
{
    const int s = channel();
    const struct evob_scroll sc = {x, y, dx, dy};
    if (s >= 0) frame_send(s, EVOB_SCROLL, &sc, sizeof sc);
}

void evo_browser_key(unsigned key_code, unsigned modifiers, int pressed, const char *text)
{
    const int s = channel();
    const struct evob_key k = {key_code, modifiers, pressed ? 1u : 0u};
    if (s >= 0) frame_send_text(s, EVOB_KEY, &k, sizeof k, text, text ? strlen(text) : 0);
}

void evo_browser_text(const char *utf8)
{
    const int s = channel();
    if (s >= 0 && utf8 && utf8[0]) frame_send_text(s, EVOB_TEXT, NULL, 0, utf8, strlen(utf8));
}

void evo_browser_resize(int width, int height)
{
    const int s = channel();
    const struct evob_size sz = {(uint32_t)width, (uint32_t)height};
    if (s >= 0) frame_send(s, EVOB_RESIZE, &sz, sizeof sz);
}

void evo_browser_set_visible(int visible)
{
    const int s = channel();
    const struct evob_flag f = {visible ? 1u : 0u};
    if (s >= 0) frame_send(s, EVOB_VISIBLE, &f, sizeof f);
}

int evo_browser_poll_event(evo_browser_event_t *ev)
{
    pthread_mutex_lock(&g_mu);
    const int r = g_ev_count > 0;
    if (r) {
        *ev = g_events[g_ev_head];
        g_ev_head = (g_ev_head + 1) % EVENT_RING;
        g_ev_count--;
    }
    pthread_mutex_unlock(&g_mu);
    return r;
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
    const int fsock = g_fclosed ? -1 : g_fsock;
    pthread_mutex_unlock(&g_mu);
    if (fsock >= 0) {
        /* The engine host owns the broker connection by now: ending fd 3 ends
         * it, and the broker follows. The frame thread closes the socket. */
        frame_send(fsock, EVOB_BYE, NULL, 0);
        shutdown(fsock, SHUT_RDWR);
    }
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
        const int busy = g_busy || g_frame_busy;
        pthread_mutex_unlock(&g_mu);
        if (!busy || waited >= timeout_ms) break;
        usleep(POLL_STEP_MS * 1000);
        waited += POLL_STEP_MS;
    }
    pthread_mutex_lock(&g_mu);
    const int started = g_started, busy = g_busy || g_frame_busy;
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
