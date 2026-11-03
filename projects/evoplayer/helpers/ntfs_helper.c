/*
 * ntfs_helper.c - root-side disk reader for EVO's NTFS support (#217).
 *
 * A PS5 payload, not part of the app. The app sandbox hides USB disk nodes
 * (/dev/da1 is not in EVO's /dev even after the jailbreak), so EVO sends this
 * small program to the console's ELF loader; it runs as root, opens a disk
 * read-only and hands EVO raw blocks over a socket on 127.0.0.1. EVO parses
 * NTFS itself (src/evo_ntfs.c) - this side knows nothing about filesystems.
 *
 * Protocol on 127.0.0.1:EVO_NTFS_HELPER_PORT, one request per connection:
 *   "LIST\n"     -> "da1 da2\n"   whole USB disks (daN), then close
 *   "OPEN daN\n" -> "OK\n" or "ERR <errno>\n"; then any number of reads:
 *        request  u64 offset, u32 length (little-endian, 4 KiB aligned,
 *                 length <= 1 MiB + 4 KiB)
 *        reply    i32 n (bytes, or -errno), then n bytes
 *   Only /dev/daN and its slices/partitions open; never for writing.
 *
 * One instance: a second copy cannot bind the port and exits. It exits by
 * itself 30 s after its last client is gone, so it does not outlive EVO.
 * Built by tools/gen_ntfs_helper.sh and embedded in EVO.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef EVO_NTFS_HELPER_PORT
#define EVO_NTFS_HELPER_PORT 9786
#endif
#define MAX_READ  ((1u << 20) + 4096u)
#define IDLE_EXIT_SECS 30

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static int g_clients;
static time_t g_last_active;

static void touch(int delta)
{
    pthread_mutex_lock(&g_mx);
    g_clients += delta;
    g_last_active = time(NULL);
    pthread_mutex_unlock(&g_mx);
}

static int send_all(int s, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n) {
        ssize_t w = send(s, b, n, 0);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

static int recv_all(int s, void *p, size_t n)
{
    uint8_t *b = (uint8_t *)p;
    while (n) {
        ssize_t r = recv(s, b, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

static int read_line(int s, char *out, size_t cap)
{
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        ssize_t r = recv(s, &c, 1, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        if (c == '\n') break;
        out[n++] = c;
    }
    out[n] = 0;
    return 0;
}

/* daN, daNsM, daNpM - a USB disk or one of its partitions, nothing else. */
static int disk_name_ok(const char *n, int whole_only)
{
    if (strncmp(n, "da", 2) != 0) return 0;
    const char *p = n + 2;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') p++;
    if (!*p) return 1;
    if (whole_only || (*p != 's' && *p != 'p')) return 0;
    p++;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') p++;
    return *p == 0;
}

static void do_list(int s)
{
    char out[512];
    size_t n = 0;
    out[0] = 0;
    DIR *d = opendir("/dev");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!disk_name_ok(e->d_name, 1)) continue;
            int w = snprintf(out + n, sizeof out - n, "%s%s", n ? " " : "", e->d_name);
            if (w < 0 || (size_t)w >= sizeof out - n) break;
            n += (size_t)w;
        }
        closedir(d);
    }
    if (n + 1 < sizeof out) out[n++] = '\n';
    send_all(s, out, n);
}

static void do_open(int s, const char *name)
{
    char path[64], reply[32];
    if (!disk_name_ok(name, 0)) {
        send_all(s, "ERR 22\n", 7);
        return;
    }
    snprintf(path, sizeof path, "/dev/%s", name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        int n = snprintf(reply, sizeof reply, "ERR %d\n", errno);
        send_all(s, reply, (size_t)n);
        return;
    }
    send_all(s, "OK\n", 3);
    uint8_t *buf = (uint8_t *)malloc(MAX_READ);
    uint8_t req[12];
    while (buf && recv_all(s, req, sizeof req) == 0) {
        uint64_t off = 0;
        uint32_t len = 0;
        for (int i = 0; i < 8; ++i) off |= (uint64_t)req[i] << (8 * i);
        for (int i = 0; i < 4; ++i) len |= (uint32_t)req[8 + i] << (8 * i);
        int32_t got = 0;
        if (len > MAX_READ || (off & 4095) || (len & 4095)) {
            got = -EINVAL;
        } else if (lseek(fd, (off_t)off, SEEK_SET) != (off_t)off) {
            got = -errno;
        } else {
            while ((uint32_t)got < len) {
                ssize_t r = read(fd, buf + got, len - (uint32_t)got);
                if (r < 0 && errno == EINTR) continue;
                if (r < 0) { if (!got) got = -errno; break; }
                if (r == 0) break;
                got += (int32_t)r;
            }
        }
        uint8_t hdr[4] = {(uint8_t)got, (uint8_t)(got >> 8), (uint8_t)(got >> 16), (uint8_t)(got >> 24)};
        if (send_all(s, hdr, 4) != 0) break;
        if (got > 0 && send_all(s, buf, (size_t)got) != 0) break;
        touch(0);
    }
    free(buf);
    close(fd);
}

static void *client(void *arg)
{
    int s = (int)(intptr_t)arg;
    char line[64];
    if (read_line(s, line, sizeof line) == 0) {
        if (!strcmp(line, "LIST")) do_list(s);
        else if (!strncmp(line, "OPEN ", 5)) do_open(s, line + 5);
    }
    close(s);
    touch(-1);
    return NULL;
}

int main(void)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return 1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(EVO_NTFS_HELPER_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* never reachable from the network */
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 8) != 0) {
        close(ls);   /* another copy is already serving */
        return 0;
    }
    touch(0);
    for (;;) {
        struct pollfd p = {ls, POLLIN, 0};
        int r = poll(&p, 1, 1000);
        if (r > 0) {
            int s = accept(ls, NULL, NULL);
            if (s >= 0) {
                touch(+1);
                pthread_t t;
                if (pthread_create(&t, NULL, client, (void *)(intptr_t)s) == 0) pthread_detach(t);
                else { close(s); touch(-1); }
            }
        }
        pthread_mutex_lock(&g_mx);
        const int idle = g_clients == 0 && time(NULL) - g_last_active >= IDLE_EXIT_SECS;
        pthread_mutex_unlock(&g_mx);
        if (idle) break;
    }
    close(ls);
    return 0;
}
