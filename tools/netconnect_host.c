/*
 * tools/netconnect_host.c - host test for evo_net_connect_list() in
 * projects/evoplayer/addons/src/evo_net.c.
 * Built and run by tools/netconnect_host.sh.
 *
 * The failure it exists for (hardware, 2026-09-30): evo_net did a plain blocking
 * connect() to each address of a host in turn. One of raw.githubusercontent.com's
 * four addresses silently drops packets from some networks, and a blocking
 * connect() to such an address does not honour SO_SNDTIMEO - it waits out the
 * kernel's ~75 s SYN retry, with evo_net's single worker thread (and every request
 * queued behind it) stuck for the duration.
 *
 * A "blackhole" here is a listening socket with a full accept queue: the kernel
 * drops further SYNs, so connect() stays in SYN_SENT, which is the same thing on
 * the wire. Each listener has its own loopback address (127.0.0.2 ...) because
 * the module remembers a dead address by IP.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "evo_net.h"

static int g_fail, g_pass;

#define CHECK(cond, ...)                                            \
    do {                                                            \
        if (cond) { g_pass++; }                                     \
        else {                                                      \
            g_fail++;                                               \
            fprintf(stderr, "  FAIL %s:%d  ", __FILE__, __LINE__);  \
            fprintf(stderr, __VA_ARGS__);                           \
            fputc('\n', stderr);                                    \
        }                                                           \
    } while (0)

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

typedef struct { struct addrinfo ai; struct sockaddr_in sin; } entry_t;

static void make_entry(entry_t *e, const char *ip, int port)
{
    memset(e, 0, sizeof *e);
    e->sin.sin_family = AF_INET;
    e->sin.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, ip, &e->sin.sin_addr);
    e->ai.ai_family = AF_INET;
    e->ai.ai_socktype = SOCK_STREAM;
    e->ai.ai_protocol = IPPROTO_TCP;
    e->ai.ai_addrlen = sizeof e->sin;
    e->ai.ai_addr = (struct sockaddr *)&e->sin;
}

/* A listener on `ip`:ephemeral. Returns the fd, and the port through *port. */
static int listener(const char *ip, int backlog, int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &sin.sin_addr);
    if (bind(fd, (struct sockaddr *)&sin, sizeof sin) != 0) { perror("bind"); exit(2); }
    if (listen(fd, backlog) != 0) { perror("listen"); exit(2); }
    socklen_t len = sizeof sin;
    getsockname(fd, (struct sockaddr *)&sin, &len);
    *port = ntohs(sin.sin_port);
    return fd;
}

/* Fills the accept queue so the kernel drops further SYNs. The client fds are
 * kept so the queue stays full. */
static int g_fillers[8];
static int g_nfill;
static void fill_queue(const char *ip, int port)
{
    for (int i = 0; i < 4; i++) {
        int c = socket(AF_INET, SOCK_STREAM, 0);
        fcntl(c, F_SETFL, O_NONBLOCK);
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof sin);
        sin.sin_family = AF_INET;
        sin.sin_port = htons((uint16_t)port);
        inet_pton(AF_INET, ip, &sin.sin_addr);
        connect(c, (struct sockaddr *)&sin, sizeof sin);
        g_fillers[g_nfill++] = c;
    }
    usleep(100 * 1000);
}

static int is_blocking(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    return fl >= 0 && !(fl & O_NONBLOCK);
}

/* Did `lfd` receive a connection? */
static int accepted(int lfd)
{
    struct pollfd p = { lfd, POLLIN, 0 };
    if (poll(&p, 1, 300) <= 0) return 0;
    int c = accept(lfd, NULL, NULL);
    if (c < 0) return 0;
    close(c);
    return 1;
}

static void drain(int lfd)
{
    for (int i = 0; i < 8; i++) {
        struct pollfd p = { lfd, POLLIN, 0 };
        if (poll(&p, 1, 40) > 0) {
            int c = accept(lfd, NULL, NULL);
            if (c >= 0) close(c);
        }
    }
}

int main(void)
{
    const int T = 300;           /* per-address timeout for the test, ms */
    int p_good, p_hole, p_refused;
    int good = listener("127.0.0.3", 16, &p_good);
    int hole = listener("127.0.0.2", 0, &p_hole);
    int tmp = listener("127.0.0.4", 1, &p_refused);
    close(tmp);                                       /* nothing listens there now */

    entry_t e_good, e_hole, e_refused;
    make_entry(&e_good, "127.0.0.3", p_good);
    make_entry(&e_hole, "127.0.0.2", p_hole);
    make_entry(&e_refused, "127.0.0.4", p_refused);

    fill_queue("127.0.0.2", p_hole);

    /* Is the blackhole real on this kernel? If a fresh connect gets through
     * anyway, the timing checks would prove nothing, so say so and skip them. */
    int have_hole = 0;
    {
        int c = socket(AF_INET, SOCK_STREAM, 0);
        fcntl(c, F_SETFL, O_NONBLOCK);
        connect(c, e_hole.ai.ai_addr, e_hole.ai.ai_addrlen);
        struct pollfd pf = { c, POLLOUT, 0 };
        have_hole = (poll(&pf, 1, 250) == 0);
        close(c);
    }
    fprintf(stderr, "(blackhole %s on this kernel)\n",
            have_hole ? "works" : "does NOT stall - timing checks skipped");
    CHECK(have_hole, "the test needs a kernel that drops SYNs on a full accept queue");

    fprintf(stderr, "== the ordinary cases\n");
    {
        e_good.ai.ai_next = NULL;
        long t0 = now_ms();
        int s = evo_net_connect_list(&e_good.ai, T);
        CHECK(s >= 0, "one live address connects");
        CHECK(s >= 0 && is_blocking(s), "and the socket is handed back in blocking mode");
        CHECK(accepted(good), "the connection really landed on it");
        CHECK(now_ms() - t0 < 200, "quickly (%ld ms)", now_ms() - t0);
        if (s >= 0) close(s);
    }
    {
        e_refused.ai.ai_next = &e_good.ai;
        long t0 = now_ms();
        int s = evo_net_connect_list(&e_refused.ai, T);
        long ms = now_ms() - t0;
        CHECK(s >= 0 && accepted(good), "a refused first address falls through to the next");
        CHECK(ms < 200, "a refusal is immediate, not a timeout (%ld ms)", ms);
        if (s >= 0) close(s);
        e_refused.ai.ai_next = NULL;
    }
    {
        e_refused.ai.ai_next = NULL;
        CHECK(evo_net_connect_list(&e_refused.ai, T) == -1, "nothing listening: -1");
        CHECK(evo_net_connect_list(NULL, T) == -1, "an empty list: -1");
    }

    if (have_hole) {
        fprintf(stderr, "== a dead first address\n");
        e_hole.ai.ai_next = &e_good.ai;
        long t0 = now_ms();
        int s = evo_net_connect_list(&e_hole.ai, T);
        long first = now_ms() - t0;
        CHECK(s >= 0 && accepted(good), "connects to the live address behind it");
        CHECK(first >= T - 60 && first < T + 500,
              "having waited the per-address timeout, not a minute (%ld ms)", first);
        if (s >= 0) close(s);

        t0 = now_ms();
        s = evo_net_connect_list(&e_hole.ai, T);
        long second = now_ms() - t0;
        CHECK(s >= 0 && accepted(good), "the next request connects too");
        CHECK(second < 120,
              "and does not wait for the dead address again: it was remembered (%ld ms)", second);
        if (s >= 0) close(s);

        fprintf(stderr, "== a dead address is last, not never\n");
        e_hole.ai.ai_next = NULL;
        t0 = now_ms();
        s = evo_net_connect_list(&e_hole.ai, T);
        long alone = now_ms() - t0;
        CHECK(s == -1, "a host whose only address is dead fails");
        CHECK(alone >= T - 60 && alone < T + 500, "after one timeout, not a minute (%ld ms)", alone);

        fprintf(stderr, "== forgiven when it recovers\n");
        /* Drain the accept queue: the address answers again. */
        drain(hole);
        for (int i = 0; i < g_nfill; i++) close(g_fillers[i]);
        g_nfill = 0;
        usleep(100 * 1000);
        drain(hole);

        s = evo_net_connect_list(&e_hole.ai, T);
        CHECK(s >= 0, "the recovered address connects (tried last, but it was the only one)");
        if (s >= 0) close(s);
        drain(hole);
        e_hole.ai.ai_next = &e_good.ai;
        t0 = now_ms();
        s = evo_net_connect_list(&e_hole.ai, T);
        CHECK(s >= 0 && accepted(hole), "once it has answered it is first in line again");
        CHECK(now_ms() - t0 < 200, "and quick (%ld ms)", now_ms() - t0);
        if (s >= 0) close(s);
    }

    fprintf(stderr, "\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
