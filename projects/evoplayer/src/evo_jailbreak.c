/* evo_jailbreak.c - see evo_jailbreak.h. app-module only. */
#ifdef EVO_APP_MODULE

#include "evo_jailbreak.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include "evo_boot_trace.h"   /* evo_bt() - notification + klog */

/* etaHEN IPC. The daemon binds 127.0.0.1:9028 and reads one whole struct per
 * connection. Layout mirrors third_party/SharpProspero/docs/app-promotion.md
 * (SharpProspero's prospero-payload-unjail "mimics etaHEN's jailbreak-on-demand
 * API"). PS5-Lapy-JB-Daemon speaks the same protocol. */
#define ETAHEN_IPC_PORT      9028
#define HIJACKER_MAGIC       0xDEADBEEFu
#define HIJACKER_CMD_JAIL    5           /* JAILBREAK_CMD */
#define HIJACKER_CMD_SIZE    0xA10       /* 2576: magic+cmd+pid+ret + 2x1280 */

struct hijacker_cmd {
    uint32_t magic;
    int32_t  cmd;
    int32_t  pid;
    int32_t  ret;
    uint8_t  reserved1[1280];
    uint8_t  reserved2[1280];
};

/* True if we can already see outside the per-title sandbox. Cheap probe: a
 * jailed module gets ENOENT on /mnt, a promoted one gets a real handle. */
static int sandbox_is_open(void)
{
    int fd = open("/mnt", O_RDONLY | O_DIRECTORY);
    if (fd >= 0) { close(fd); return 1; }
    return 0;
}

static int request_promotion(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return 0;

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons(ETAHEN_IPC_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* 127.0.0.1 */

    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        close(s);
        return 0;   /* no daemon listening - fall back to the manual payload */
    }

    struct hijacker_cmd c;
    memset(&c, 0, sizeof c);
    c.magic = HIJACKER_MAGIC;
    c.cmd   = HIJACKER_CMD_JAIL;
    c.pid   = getpid();

    ssize_t n = send(s, &c, HIJACKER_CMD_SIZE, 0);
    int ok = 0;
    if (n == (ssize_t)HIJACKER_CMD_SIZE) {
        size_t got = 0;
        while (got < HIJACKER_CMD_SIZE) {
            ssize_t r = recv(s, (uint8_t *)&c + got, HIJACKER_CMD_SIZE - got, 0);
            if (r <= 0) break;
            got += (size_t)r;
        }
        if (got >= 16 && c.ret == 0)
            ok = 1;
    }
    close(s);
    return ok;
}

int evo_jailbreak_self(void)
{
    if (sandbox_is_open()) {
        evo_bt("jailbreak: sandbox already open");
        return 1;
    }
    int ok = request_promotion();
    if (ok && sandbox_is_open()) {
        evo_bt("jailbreak: promoted via etaHEN IPC");
        return 1;
    }
    evo_bt("jailbreak: %s - USB browse needs tools/sandbox-unjail.sh",
           ok ? "daemon ok but sandbox still closed" : "no daemon on :9028");
    return 0;
}

#endif /* EVO_APP_MODULE */
