/*
 * frame_test.c - a test engine host for spike S2b (#237, docs/ui/browser.md).
 *
 * A PS5 payload, not part of the app. The browser engine's broker spawns it
 * with EVO's end of a SEQPACKET socketpair at fd 3. It makes two SHM_ANON
 * frame buffers, hands them to EVO (HELLO + SCM_RIGHTS), then paints a moving
 * bar into whichever buffer EVO has released and reports only the rectangles
 * it changed (FRAME). A fixed red square, a green one and a blue one check the
 * byte order: if red shows blue in EVO, the formats disagree.
 *
 * It exits when EVO's end closes, after 10 minutes, or when EVO stops
 * releasing buffers for 5 s; the broker also kills it when EVO disconnects.
 * Built by tools/build_frame_test.sh; never embedded in EVO.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>

#include "evo_browser_wire.h"

#define W        1920
#define H        1080
#define STRIDE   (W * 4)
#define BAR_W    160
#define STEP     8
#define SOCK     3

static uint8_t *g_buf[2];

static void bg_px(uint8_t *p, int x, int y)
{
    const int grid = (x % 120) == 0 || (y % 120) == 0;
    p[0] = grid ? 255 : (uint8_t)(x * 255 / W);
    p[1] = grid ? 255 : (uint8_t)(y * 255 / H);
    p[2] = grid ? 255 : 96;
    p[3] = 255;
    /* byte-order check: red, green, blue squares, top left */
    if (y >= 100 && y < 300) {
        if (x >= 100 && x < 300)      { p[0] = 255; p[1] = 0;   p[2] = 0; }
        else if (x >= 350 && x < 550) { p[0] = 0;   p[1] = 255; p[2] = 0; }
        else if (x >= 600 && x < 800) { p[0] = 0;   p[1] = 0;   p[2] = 255; }
    }
}

static void paint_cols(uint8_t *b, int x0, int x1, int bar_x)
{
    if (x0 < 0) x0 = 0;
    if (x1 > W) x1 = W;
    for (int y = 0; y < H; ++y) {
        uint8_t *row = b + (size_t)y * STRIDE;
        for (int x = x0; x < x1; ++x) {
            uint8_t *p = row + x * 4;
            if (x >= bar_x && x < bar_x + BAR_W && y >= 400) {
                p[0] = 255; p[1] = 255; p[2] = 255; p[3] = 255;   /* the moving bar */
            } else {
                bg_px(p, x, y);
            }
        }
    }
}

static int send_msg(int type, const void *body, uint32_t len, const int *fds, int nfds)
{
    struct evob_hdr h = {EVOB_MAGIC, EVOB_VERSION, (uint16_t)type, len};
    uint8_t pkt[sizeof h + sizeof(struct evob_frame)];
    memcpy(pkt, &h, sizeof h);
    memcpy(pkt + sizeof h, body, len);
    struct iovec iov = {pkt, sizeof h + len};
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int) * EVOB_MAX_BUFS)]; } ctl;
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

/* One RELEASE from EVO: the buffer index, -1 on timeout, -2 when EVO is gone. */
static int recv_release(int flags)
{
    uint8_t pkt[64];
    const ssize_t n = recv(SOCK, pkt, sizeof pkt, flags);
    if (n == 0) return -2;
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? -1 : -2;
    struct evob_hdr h;
    if ((size_t)n < sizeof h) return -1;
    memcpy(&h, pkt, sizeof h);
    if (h.magic != EVOB_MAGIC) return -1;
    if (h.type == EVOB_BYE) return -2;
    if (h.type != EVOB_RELEASE || (size_t)n < sizeof h + sizeof(struct evob_release)) return -1;
    struct evob_release r;
    memcpy(&r, pkt + sizeof h, sizeof r);
    return r.buf < 2 ? (int)r.buf : -1;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    struct timeval tv = {1, 0};
    setsockopt(SOCK, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(SOCK, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    int fds[2];
    for (int i = 0; i < 2; ++i) {
        fds[i] = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);
        if (fds[i] < 0 || ftruncate(fds[i], (off_t)STRIDE * H) != 0) return 1;
        g_buf[i] = mmap(NULL, (size_t)STRIDE * H, PROT_READ | PROT_WRITE, MAP_SHARED, fds[i], 0);
        if (g_buf[i] == MAP_FAILED) return 1;
        paint_cols(g_buf[i], 0, W, -BAR_W);
    }
    const struct evob_hello hello = {W, H, STRIDE, EVOB_RGBA8_PREMUL, 2};
    if (send_msg(EVOB_HELLO, &hello, sizeof hello, fds, 2) != 0) return 2;
    close(fds[0]);
    close(fds[1]);

    int free_buf[2] = {1, 1}, last_bar[2] = {-1, -1}, first[2] = {1, 1};
    int bar = 0, starved_ms = 0;
    uint32_t seq = 0;
    for (int frame = 0; frame < 60 * 60 * 10; ++frame) {
        int r;
        while ((r = recv_release(MSG_DONTWAIT)) >= 0) free_buf[r] = 1;
        if (r == -2) return 0;
        const int b = free_buf[0] ? 0 : free_buf[1] ? 1 : -1;
        if (b < 0) {
            r = recv_release(0);                    /* up to 1 s */
            if (r == -2) return 0;
            if (r >= 0) { free_buf[r] = 1; starved_ms = 0; }
            else if ((starved_ms += 1000) >= 5000) return 3;
            continue;
        }
        struct evob_frame f;
        memset(&f, 0, sizeof f);
        f.buf = (uint32_t)b;
        f.seq = ++seq;
        if (first[b]) {
            paint_cols(g_buf[b], 0, W, bar);
            f.rects[f.nrects++] = (struct evob_rect){0, 0, W, H};
            first[b] = 0;
        } else {
            if (last_bar[b] >= 0) {
                paint_cols(g_buf[b], last_bar[b], last_bar[b] + BAR_W, bar);
                f.rects[f.nrects++] = (struct evob_rect){(uint16_t)last_bar[b], 0, BAR_W, H};
            }
            paint_cols(g_buf[b], bar, bar + BAR_W, bar);
            f.rects[f.nrects++] = (struct evob_rect){(uint16_t)bar, 0, BAR_W, H};
        }
        last_bar[b] = bar;
        free_buf[b] = 0;
        if (send_msg(EVOB_FRAME, &f, sizeof f, NULL, 0) != 0) return 0;
        bar = (bar + STEP) % (W - BAR_W);
        usleep(16000);
    }
    const uint32_t none = 0;
    send_msg(EVOB_BYE, &none, sizeof none, NULL, 0);
    return 0;
}
