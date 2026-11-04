/*
 * Loopback sockets and the ELF loader. See include/evo_elfldr.h.
 */
#include "evo_elfldr.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

int evo_loopback_connect(int port, int timeout_ms)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) {   /* connect-ok: loopback, answers at once */
        close(s);
        return -1;
    }
    if (timeout_ms <= 0) timeout_ms = 1000;
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return s;
}

int evo_loopback_port_open(int port)
{
    const int s = evo_loopback_connect(port, 1000);
    if (s < 0) return 0;
    close(s);
    return 1;
}

int evo_sock_send_all(int s, const void *buf, size_t n)
{
    const unsigned char *b = (const unsigned char *)buf;
    while (n) {
        const ssize_t r = send(s, b, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

int evo_sock_recv_all(int s, void *buf, size_t n)
{
    unsigned char *b = (unsigned char *)buf;
    while (n) {
        const ssize_t r = recv(s, b, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

int evo_elfldr_send(const void *elf, size_t len, int timeout_ms)
{
    const int s = evo_loopback_connect(EVO_ELFLDR_PORT, timeout_ms);
    if (s < 0) return -1;
    const int rc = evo_sock_send_all(s, elf, len);
    close(s);
    return rc;
}
