/*
 * EVO Player - Phase 1b task 4: small libc gap fillers for the app module.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The static SDK libc.a (linked last, for __emutls_get_address) already carries
 * real C-locale implementations of the xlocale `*_l` family (no-locale.o),
 * gmtime_r/localtime_r, nl_langinfo, __assert, posix_fadvise, etc. This file
 * only covers what libc.a still can't give us cleanly:
 *
 *   _setjmp / _longjmp   - not in this libc.a; alias to the plain forms
 *                          (which the console libc exports).
 *   dladdr               - pulling libc.a's dladdr.o drags in the private
 *                          rtld interface (__dlopen ...) that no SDK stub
 *                          resolves; a local no-op def keeps that object out.
 *   __dl* internals      - belt-and-braces stubs in case something else pulls
 *                          the rtld path; harmless if unreferenced.
 *   recvmmsg / sendmmsg  - absent everywhere; EVO's networking is TCP, and the
 *                          UDP callers all fall back per-message.
 *   getaddrinfo / etc    - POSIX DNS & address resolution; on retail PS5 FW 12.70
 *                          libScePosixForWebKit.sprx exports empty NULL stubs
 *                          for getaddrinfo/freeaddrinfo/getnameinfo/gai_strerror.
 *                          Providing local definitions here binds them directly
 *                          inside eboot.bin (avoiding NULL crash at rip=0).
 *
 * Compiled INTO eboot.bin as local defs (never imports, never NULL).
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>

#ifndef INADDR_ANY
#define INADDR_ANY ((uint32_t)0x00000000)
#endif
#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK ((uint32_t)0x7f000001)
#endif

/* ------------------------------------------------------------------ setjmp -- */
/* _setjmp/_longjmp differ from setjmp/longjmp only by not touching the signal
 * mask. Tail-jump to the plain forms. */
__asm__(
    ".globl _setjmp\n"
    "_setjmp:\n"
    "    jmp *setjmp@GOTPCREL(%rip)\n"
    ".globl _longjmp\n"
    "_longjmp:\n"
    "    jmp *longjmp@GOTPCREL(%rip)\n"
);

/* --------------------------------------------------------------- dl* / dladdr */
int dladdr(const void *addr, void *info)
{
    (void)addr; (void)info;
    return 0;   /* "not found" - callers use it only for backtrace symbols */
}

void *__dlopen(const char *path, int mode) { (void)path; (void)mode; return NULL; }
void *__dlsym(void *h, const char *sym)    { (void)h; (void)sym; return NULL; }
int   __dlclose(void *h)                   { (void)h; return -1; }
char *__dlerror(void)                      { return (char *)"unsupported"; }

/* ------------------------------------------------------------ batch sockets -- */
int recvmmsg(int s, void *msgvec, unsigned int vlen, int flags, const void *timeout)
{
    (void)s; (void)msgvec; (void)vlen; (void)flags; (void)timeout;
    errno = ENOSYS;
    return -1;
}
int sendmmsg(int s, void *msgvec, unsigned int vlen, int flags)
{
    (void)s; (void)msgvec; (void)vlen; (void)flags;
    errno = ENOSYS;
    return -1;
}

/* ------------------------------------------- getaddrinfo / DNS resolution ---- */

#define DNS_CACHE_SIZE 64
#define DNS_CACHE_TTL_SEC 300 /* 5 minutes */

typedef struct {
    char host[128];
    struct in_addr addr;
    time_t expires;
    int valid;
} dns_cache_entry_t;

static dns_cache_entry_t s_dns_cache[DNS_CACHE_SIZE];
static pthread_mutex_t   s_dns_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint16_t          s_dns_txid  = 0x5a10;

static int parse_ipv4_literal(const char *host, struct in_addr *out)
{
    if (!host) return 0;
    unsigned int b0, b1, b2, b3;
    char tail = 0;
    if (sscanf(host, "%u.%u.%u.%u%c", &b0, &b1, &b2, &b3, &tail) == 4) {
        if (b0 <= 255 && b1 <= 255 && b2 <= 255 && b3 <= 255) {
            uint32_t ip = ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
            out->s_addr = htonl(ip);
            return 1;
        }
    }
    return 0;
}

static int dns_cache_lookup(const char *host, struct in_addr *out)
{
    time_t now = time(NULL);
    pthread_mutex_lock(&s_dns_mutex);
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (s_dns_cache[i].valid && strcasecmp(s_dns_cache[i].host, host) == 0) {
            if (s_dns_cache[i].expires >= now) {
                *out = s_dns_cache[i].addr;
                pthread_mutex_unlock(&s_dns_mutex);
                return 1;
            } else {
                s_dns_cache[i].valid = 0;
            }
        }
    }
    pthread_mutex_unlock(&s_dns_mutex);
    return 0;
}

static void dns_cache_insert(const char *host, const struct in_addr *addr)
{
    if (!host || strlen(host) >= 128) return;
    time_t now = time(NULL);
    pthread_mutex_lock(&s_dns_mutex);
    static int s_next_slot = 0;
    int slot = -1;
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (s_dns_cache[i].valid && strcasecmp(s_dns_cache[i].host, host) == 0) {
            slot = i;
            break;
        }
        if (!s_dns_cache[i].valid || s_dns_cache[i].expires < now) {
            if (slot == -1) slot = i;
        }
    }
    if (slot == -1) {
        slot = s_next_slot;
        s_next_slot = (s_next_slot + 1) % DNS_CACHE_SIZE;
    }
    strncpy(s_dns_cache[slot].host, host, sizeof(s_dns_cache[slot].host) - 1);
    s_dns_cache[slot].host[sizeof(s_dns_cache[slot].host) - 1] = '\0';
    s_dns_cache[slot].addr = *addr;
    s_dns_cache[slot].expires = now + DNS_CACHE_TTL_SEC;
    s_dns_cache[slot].valid = 1;
    pthread_mutex_unlock(&s_dns_mutex);
}

static int dns_query_server(const char *hostname, const char *dns_server_ip, struct in_addr *out)
{
    unsigned char query[512];
    int qlen = 0;

    /* DNS header: 12 bytes */
    uint16_t txid = __atomic_add_fetch(&s_dns_txid, 1, __ATOMIC_RELAXED);
    query[qlen++] = (unsigned char)(txid >> 8);
    query[qlen++] = (unsigned char)(txid & 0xff);
    query[qlen++] = 0x01; /* flags: standard query, recursion desired */
    query[qlen++] = 0x00;
    query[qlen++] = 0x00; /* qdcount = 1 */
    query[qlen++] = 0x01;
    query[qlen++] = 0x00; /* ancount = 0 */
    query[qlen++] = 0x00;
    query[qlen++] = 0x00; /* nscount = 0 */
    query[qlen++] = 0x00;
    query[qlen++] = 0x00; /* arcount = 0 */
    query[qlen++] = 0x00;

    /* QNAME */
    const char *p = hostname;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t label_len = dot ? (size_t)(dot - p) : strlen(p);
        if (label_len == 0 || label_len > 63 || qlen + label_len + 1 >= sizeof(query))
            return -1;
        query[qlen++] = (unsigned char)label_len;
        memcpy(query + qlen, p, label_len);
        qlen += (int)label_len;
        if (!dot) break;
        p = dot + 1;
    }
    query[qlen++] = 0x00; /* end of name */

    /* QTYPE = 1 (A), QCLASS = 1 (IN) */
    if (qlen + 4 >= (int)sizeof(query)) return -1;
    query[qlen++] = 0x00;
    query[qlen++] = 0x01;
    query[qlen++] = 0x00;
    query[qlen++] = 0x01;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_port = htons(53);
    if (!parse_ipv4_literal(dns_server_ip, &serv.sin_addr)) {
        close(sock);
        return -1;
    }

    if (sendto(sock, (const char *)query, (size_t)qlen, 0, (struct sockaddr *)&serv, sizeof(serv)) < 0) {
        close(sock);
        return -1;
    }

    unsigned char resp[1024];
    ssize_t rlen = recvfrom(sock, (char *)resp, sizeof(resp), 0, NULL, NULL);
    close(sock);

    if (rlen < 12) return -1;
    uint16_t rxid = ((uint16_t)resp[0] << 8) | resp[1];
    if (rxid != txid) return -1;
    if ((resp[2] & 0x80) == 0) return -1; /* QR bit must be set */
    if ((resp[3] & 0x0f) != 0) return -1; /* RCODE != 0 */
    uint16_t ancount = ((uint16_t)resp[6] << 8) | resp[7];
    if (ancount == 0) return -1;

    /* Skip question section */
    int idx = 12;
    while (idx < rlen && resp[idx] != 0) {
        if ((resp[idx] & 0xc0) == 0xc0) { idx += 2; goto qdone; }
        idx += (1 + (int)resp[idx]);
    }
    if (idx < rlen && resp[idx] == 0) idx++;
qdone:
    idx += 4; /* skip qtype & qclass */

    /* Parse answer records */
    for (uint16_t a = 0; a < ancount && idx + 10 <= rlen; ++a) {
        if ((resp[idx] & 0xc0) == 0xc0) {
            idx += 2;
        } else {
            while (idx < rlen && resp[idx] != 0) {
                if ((resp[idx] & 0xc0) == 0xc0) { idx += 2; break; }
                idx += (1 + (int)resp[idx]);
            }
            if (idx < rlen && resp[idx] == 0) idx++;
        }
        if (idx + 10 > rlen) break;
        uint16_t atype = ((uint16_t)resp[idx] << 8) | resp[idx + 1];
        uint16_t rdlen = ((uint16_t)resp[idx + 8] << 8) | resp[idx + 9];
        idx += 10;
        if (idx + rdlen > rlen) break;
        if (atype == 1 && rdlen == 4) {
            memcpy(&out->s_addr, resp + idx, 4);
            return 0;
        }
        idx += rdlen;
    }
    return -1;
}

static int resolve_hostname(const char *hostname, struct in_addr *out)
{
    if (!hostname || !*hostname) return -1;

    if (parse_ipv4_literal(hostname, out))
        return 0;

    if (strcmp(hostname, "localhost") == 0) {
        out->s_addr = htonl(0x7f000001);
        return 0;
    }

    if (dns_cache_lookup(hostname, out))
        return 0;

    static const char *kDnsServers[] = {
        "192.168.0.8", /* User laptop / LAN server */
        "192.168.0.1", /* Primary gateway */
        "192.168.1.1", /* Alternate gateway */
        "1.1.1.1",     /* Cloudflare DNS */
        "8.8.8.8",     /* Google DNS */
        "1.0.0.1",     /* Cloudflare secondary */
        "8.8.4.4",     /* Google secondary */
        "10.0.0.1"     /* 10.x gateway */
    };

    /* Optional: check /etc/resolv.conf if present */
    FILE *rc = fopen("/etc/resolv.conf", "r");
    if (rc) {
        char line[128];
        while (fgets(line, sizeof(line), rc)) {
            char *ns = strstr(line, "nameserver");
            if (ns) {
                ns += 10;
                while (*ns == ' ' || *ns == '\t') ns++;
                char ipbuf[32];
                size_t len = 0;
                while (*ns && *ns != ' ' && *ns != '\t' && *ns != '\r' && *ns != '\n' && len < sizeof(ipbuf) - 1) {
                    ipbuf[len++] = *ns++;
                }
                ipbuf[len] = '\0';
                struct in_addr test_ip;
                if (parse_ipv4_literal(ipbuf, &test_ip)) {
                    if (dns_query_server(hostname, ipbuf, out) == 0) {
                        fclose(rc);
                        dns_cache_insert(hostname, out);
                        return 0;
                    }
                }
            }
        }
        fclose(rc);
    }

    for (size_t i = 0; i < sizeof(kDnsServers) / sizeof(kDnsServers[0]); ++i) {
        if (dns_query_server(hostname, kDnsServers[i], out) == 0) {
            dns_cache_insert(hostname, out);
            return 0;
        }
    }

    return -1;
}

static int parse_service(const char *service, int socktype, int flags, int *out_port)
{
    (void)socktype;
    if (!service || !*service) {
        *out_port = 0;
        return 0;
    }

    char *end = NULL;
    unsigned long val = strtoul(service, &end, 10);
    if (*end == '\0') {
        if (val > 65535) return EAI_SERVICE;
        *out_port = (int)val;
        return 0;
    }

    if (flags & AI_NUMERICSERV)
        return EAI_SERVICE;

    if (strcasecmp(service, "http") == 0)        *out_port = 80;
    else if (strcasecmp(service, "https") == 0)  *out_port = 443;
    else if (strcasecmp(service, "rtsp") == 0)   *out_port = 554;
    else if (strcasecmp(service, "domain") == 0) *out_port = 53;
    else if (strcasecmp(service, "ftp") == 0)    *out_port = 21;
    else if (strcasecmp(service, "ssh") == 0)    *out_port = 22;
    else if (strcasecmp(service, "ntp") == 0)    *out_port = 123;
    else return EAI_SERVICE;

    return 0;
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints,
                struct addrinfo **res)
{
    if (!res) return EAI_FAIL;
    *res = NULL;

    if (!node && !service) return EAI_NONAME;

    int flags    = hints ? hints->ai_flags : 0;
    int family   = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints ? hints->ai_socktype : 0;
    int protocol = hints ? hints->ai_protocol : 0;

    if (family != AF_UNSPEC && family != AF_INET) {
        return EAI_FAMILY;
    }

    int port = 0;
    int err = parse_service(service, socktype, flags, &port);
    if (err != 0) return err;

    struct in_addr ip_addr;
    memset(&ip_addr, 0, sizeof(ip_addr));

    if (!node) {
        if (flags & AI_PASSIVE) {
            ip_addr.s_addr = htonl(INADDR_ANY);
        } else {
            ip_addr.s_addr = htonl(INADDR_LOOPBACK);
        }
    } else {
        char clean_node[256];
        const char *host_str = node;
        size_t nlen = strlen(node);
        if (node[0] == '[' && node[nlen - 1] == ']' && nlen > 2 && nlen < sizeof(clean_node)) {
            memcpy(clean_node, node + 1, nlen - 2);
            clean_node[nlen - 2] = '\0';
            host_str = clean_node;
        }

        if (parse_ipv4_literal(host_str, &ip_addr)) {
            /* IPv4 literal parsed successfully */
        } else {
            if (flags & AI_NUMERICHOST) {
                return EAI_NONAME;
            }
            if (resolve_hostname(host_str, &ip_addr) != 0) {
                return EAI_NONAME;
            }
        }
    }

    struct addrinfo *ai = (struct addrinfo *)calloc(1, sizeof(struct addrinfo));
    struct sockaddr_in *sin = (struct sockaddr_in *)calloc(1, sizeof(struct sockaddr_in));
    if (!ai || !sin) {
        if (ai) free(ai);
        if (sin) free(sin);
        return EAI_MEMORY;
    }

    sin->sin_family = AF_INET;
    sin->sin_port = htons((uint16_t)port);
    sin->sin_addr = ip_addr;

    ai->ai_family   = AF_INET;
    ai->ai_socktype = socktype ? socktype : SOCK_STREAM;
    ai->ai_protocol = protocol ? protocol : (ai->ai_socktype == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP);
    ai->ai_addrlen  = sizeof(struct sockaddr_in);
    ai->ai_addr     = (struct sockaddr *)sin;

    if ((flags & AI_CANONNAME) && node) {
        ai->ai_canonname = strdup(node);
    }

    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res)
{
    while (res) {
        struct addrinfo *next = res->ai_next;
        if (res->ai_canonname) {
            free(res->ai_canonname);
        }
        if (res->ai_addr) {
            free(res->ai_addr);
        }
        free(res);
        res = next;
    }
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen,
                char *host, size_t hostlen,
                char *serv, size_t servlen, int flags)
{
    (void)flags;
    if (!sa || salen < sizeof(struct sockaddr_in))
        return EAI_FAMILY;
    if (sa->sa_family != AF_INET)
        return EAI_FAMILY;

    const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;

    if (host && hostlen > 0) {
        const unsigned char *b = (const unsigned char *)&sin->sin_addr.s_addr;
        int n = snprintf(host, hostlen, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        if (n < 0 || (size_t)n >= hostlen)
            return EAI_OVERFLOW;
    }

    if (serv && servlen > 0) {
        int n = snprintf(serv, servlen, "%u", (unsigned int)ntohs(sin->sin_port));
        if (n < 0 || (size_t)n >= servlen)
            return EAI_OVERFLOW;
    }

    return 0;
}

const char *gai_strerror(int errcode)
{
    switch (errcode) {
        case 0:            return "Success";
        case EAI_AGAIN:    return "Temporary failure in name resolution";
        case EAI_BADFLAGS: return "Invalid value for ai_flags";
        case EAI_FAIL:     return "Non-recoverable failure in name resolution";
        case EAI_FAMILY:   return "ai_family not supported";
        case EAI_MEMORY:   return "Memory allocation failure";
        case EAI_NONAME:   return "Name or service not known";
        case EAI_SERVICE:  return "Servname not supported for ai_socktype";
        case EAI_SOCKTYPE: return "ai_socktype not supported";
        case EAI_SYSTEM:   return "System error";
        case EAI_OVERFLOW: return "Argument buffer overflow";
        default:           return "Unknown error";
    }
}
