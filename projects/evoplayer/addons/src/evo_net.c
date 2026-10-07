#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * evo_net.c — Non-blocking HTTP/REST client implementation using BSD sockets.
 */
/* Matches the other translation units that report a version: the app build
 * passes -DEVO_PLAYER_VERSION from projects/evoplayer/VERSION; this is the
 * fallback for builds that do not. */
#ifndef EVO_PLAYER_VERSION
#define EVO_PLAYER_VERSION "v0.11.0-final"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>

#include "evo_net.h"
#include "evo_error.h"

/* ---- LAN discovery (Emby / Jellyfin, UDP 7359) ---- */

#define DISCOVER_PORT     7359
#define DISCOVER_WAIT_MS  1500

static pthread_mutex_t s_disc_mx = PTHREAD_MUTEX_INITIALIZER;
static int  s_disc_state = 0;            /* 0 idle/taken, 1 running, 2 found, 3 none */
static char s_disc_product[32];
static char s_disc_addr[128];

/* The reply is JSON with an "Address" like "http://172.17.0.2:8096". The
 * host part is what the server believes its address is - wrong behind Docker
 * or NAT - so only the scheme and port are taken from it, and the host is
 * where the reply actually came from. */
static void disc_build_addr(const char *reply, const struct sockaddr_in *from)
{
    const char *scheme = "http";
    int port = 8096;
    const char *a = strstr(reply, "\"Address\"");
    if (a) {
        a = strchr(a + 9, '"');
        if (a) {
            a++;
            if (strncmp(a, "https://", 8) == 0) scheme = "https";
            const char *hp = strstr(a, "://");
            hp = hp ? hp + 3 : a;
            const char *end = strchr(hp, '"');
            const char *colon = strchr(hp, ':');
            if (colon && (!end || colon < end)) port = atoi(colon + 1);
        }
    }
    char ip[INET_ADDRSTRLEN] = "";
    inet_ntop(AF_INET, &from->sin_addr, ip, sizeof ip);
    snprintf(s_disc_addr, sizeof s_disc_addr, "%s://%s:%d", scheme, ip, port);
}

static void *disc_thread(void *arg)
{
    (void)arg;
    int found = 0;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
        struct sockaddr_in to;
        memset(&to, 0, sizeof to);
        to.sin_family = AF_INET;
        to.sin_port = htons(DISCOVER_PORT);
        to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        char msg[64];
        int n = snprintf(msg, sizeof msg, "who is %s?", s_disc_product);
        if (sendto(fd, msg, (size_t)n, 0, (struct sockaddr *)&to, sizeof to) == n) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (poll(&pfd, 1, DISCOVER_WAIT_MS) > 0) {
                char buf[1024];
                struct sockaddr_in from;
                socklen_t fl = sizeof from;
                ssize_t r = recvfrom(fd, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
                if (r > 0) {
                    buf[r] = '\0';
                    pthread_mutex_lock(&s_disc_mx);
                    disc_build_addr(buf, &from);
                    pthread_mutex_unlock(&s_disc_mx);
                    found = 1;
                }
            }
        }
        close(fd);
    }
    pthread_mutex_lock(&s_disc_mx);
    s_disc_state = found ? 2 : 3;
    pthread_mutex_unlock(&s_disc_mx);
    return NULL;
}

void evo_net_discover_start(const char *product)
{
    pthread_mutex_lock(&s_disc_mx);
    if (s_disc_state == 1) { pthread_mutex_unlock(&s_disc_mx); return; }
    snprintf(s_disc_product, sizeof s_disc_product, "%s", product ? product : "");
    s_disc_addr[0] = '\0';
    s_disc_state = 1;
    pthread_mutex_unlock(&s_disc_mx);

    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, disc_thread, NULL) != 0) {
        pthread_mutex_lock(&s_disc_mx);
        s_disc_state = 3;
        pthread_mutex_unlock(&s_disc_mx);
    }
    pthread_attr_destroy(&at);
}

int evo_net_discover_poll(char *addr, size_t cap)
{
    int rc;
    pthread_mutex_lock(&s_disc_mx);
    if (s_disc_state == 1) {
        rc = 0;
    } else if (s_disc_state == 2) {
        snprintf(addr, cap, "%s", s_disc_addr);
        s_disc_state = 0;
        rc = 1;
    } else {
        s_disc_state = 0;
        rc = -1;
    }
    pthread_mutex_unlock(&s_disc_mx);
    return rc;
}
#ifndef NO_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

#ifndef EVO_NET_MAX_QUEUE
#define EVO_NET_MAX_QUEUE    32
#endif

#ifndef EVO_NET_TIMEOUT_SEC
#define EVO_NET_TIMEOUT_SEC  6
#endif

#define EVO_NET_BUFFER_SIZE  8192

/* How long one address may take to accept a connection before the next is tried.
 * Generous for a live CDN (a handshake is tens of milliseconds); short enough
 * that one dead address in a host's list costs a few seconds, not the kernel's
 * SYN retry of about 75. */
#ifndef EVO_NET_CONNECT_TIMEOUT_MS
#define EVO_NET_CONNECT_TIMEOUT_MS 3000
#endif

#define EVO_NET_BAD_ADDR_MAX     16
#define EVO_NET_BAD_ADDR_TTL_SEC 300

/* IPv4 addresses that timed out recently, tried after the ones that have not. */
static struct { uint32_t addr; time_t until; } s_bad_addr[EVO_NET_BAD_ADDR_MAX];
static pthread_mutex_t s_bad_addr_lock = PTHREAD_MUTEX_INITIALIZER;

static int rp_ipv4(const struct addrinfo *rp, uint32_t *out)
{
    if (rp->ai_family != AF_INET || !rp->ai_addr) return 0;
    *out = ((const struct sockaddr_in *)rp->ai_addr)->sin_addr.s_addr;
    return 1;
}

static int addr_is_bad(uint32_t a)
{
    const time_t now = time(NULL);
    int bad = 0;
    pthread_mutex_lock(&s_bad_addr_lock);
    for (int i = 0; i < EVO_NET_BAD_ADDR_MAX; i++) {
        if (s_bad_addr[i].addr == a && s_bad_addr[i].until > now) { bad = 1; break; }
    }
    pthread_mutex_unlock(&s_bad_addr_lock);
    return bad;
}

/* bad != 0: remember `a` as timed out. bad == 0: it answered, forget it. */
static void addr_note(uint32_t a, int bad)
{
    const time_t now = time(NULL);
    pthread_mutex_lock(&s_bad_addr_lock);
    int slot = -1, oldest = 0;
    for (int i = 0; i < EVO_NET_BAD_ADDR_MAX; i++) {
        if (s_bad_addr[i].addr == a) { slot = i; break; }
        if (slot < 0 && (s_bad_addr[i].addr == 0 || s_bad_addr[i].until <= now)) slot = i;
        if (s_bad_addr[i].until < s_bad_addr[oldest].until) oldest = i;
    }
    if (bad) {
        if (slot < 0) slot = oldest;
        s_bad_addr[slot].addr = a;
        s_bad_addr[slot].until = now + EVO_NET_BAD_ADDR_TTL_SEC;
    } else if (slot >= 0 && s_bad_addr[slot].addr == a) {
        s_bad_addr[slot].addr = 0;
        s_bad_addr[slot].until = 0;
    }
    pthread_mutex_unlock(&s_bad_addr_lock);
}

/* One address, bounded. Returns a connected blocking socket, or -1 (and sets
 * *timed_out when it ran out of time rather than being refused). */
static int connect_bounded(const struct addrinfo *rp, int timeout_ms, int *timed_out)
{
    *timed_out = 0;
    int sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sock < 0) return -1;

    /* Non-blocking, so connect() cannot sit in the kernel's ~75 s SYN retry. */
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(sock, rp->ai_addr, rp->ai_addrlen);
    if (rc != 0 && flags >= 0 && (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EINTR)) {
        struct pollfd pfd;
        pfd.fd = sock;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int pr;
        do {
            pr = poll(&pfd, 1, timeout_ms);
        } while (pr < 0 && errno == EINTR);

        if (pr > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            rc = (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) ? 0 : -1;
        } else {
            rc = -1;
            if (pr == 0) *timed_out = 1;
        }
    }
    if (rc != 0) {
        close(sock);
        return -1;
    }

    if (flags >= 0) fcntl(sock, F_SETFL, flags);
    struct timeval tv;
    tv.tv_sec = EVO_NET_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return sock;
}

int evo_net_connect_list(const struct addrinfo *list, int per_addr_timeout_ms)
{
    if (per_addr_timeout_ms <= 0) per_addr_timeout_ms = EVO_NET_CONNECT_TIMEOUT_MS;

    /* Pass 0: addresses that have not timed out lately. Pass 1: the ones that
     * have - last, not never, so a host whose only address is slow still works. */
    for (int pass = 0; pass < 2; ++pass) {
        for (const struct addrinfo *rp = list; rp; rp = rp->ai_next) {
            uint32_t a = 0;
            const int v4 = rp_ipv4(rp, &a);
            const int bad = v4 && addr_is_bad(a);
            if ((pass == 0) == bad) continue;

            int timed_out = 0;
            const int sock = connect_bounded(rp, per_addr_timeout_ms, &timed_out);
            if (sock >= 0) {
                if (v4) addr_note(a, 0);
                return sock;
            }
            if (v4 && timed_out) addr_note(a, 1);
        }
    }
    return -1;
}

typedef struct evo_net_req {
    char        method[16];
    char        url[EVO_NET_MAX_URL];
    char       *post_data;
    char      **headers;
    int         header_count;
    evo_net_cb  callback;
    void       *user_data;

    /* Result */
    int         completed;
    int         success;
    int         error;          /* EVO_NET_ERR_*; the reason when !success */
    int         status_code;
    char       *response_body;
    size_t      response_len;
} evo_net_req_t;

static pthread_t       g_worker_thread;
static pthread_mutex_t g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_queue_cond  = PTHREAD_COND_INITIALIZER;
static int             g_running     = 0;

static evo_net_req_t  *g_pending_queue[EVO_NET_MAX_QUEUE];
static int             g_pending_count = 0;

static evo_net_req_t  *g_completed_queue[EVO_NET_MAX_QUEUE];
static int             g_completed_count = 0;

#ifndef NO_OPENSSL
static SSL_CTX        *g_ssl_ctx = NULL;
static pthread_mutex_t g_ssl_init_mutex = PTHREAD_MUTEX_INITIALIZER;

static SSL_CTX *evo_net_get_ssl_ctx(void)
{
    pthread_mutex_lock(&g_ssl_init_mutex);
    if (!g_ssl_ctx) {
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        const SSL_METHOD *method = TLS_client_method();
        g_ssl_ctx = SSL_CTX_new(method);
        if (g_ssl_ctx) {
            SSL_CTX_set_mode(g_ssl_ctx, SSL_MODE_AUTO_RETRY);
        }
    }
    pthread_mutex_unlock(&g_ssl_init_mutex);
    return g_ssl_ctx;
}
#endif

/*
 * Releases all heap memory associated with an evo_net request structure.
 * Consistently invoked across async overflow rejection, worker overflow drop,
 * shutdown pending/completed cleanups, and the main poll dispatch loop.
 */
static void free_req(evo_net_req_t *req)
{
    if (!req) return;
    if (req->post_data) {
        free(req->post_data);
        req->post_data = NULL;
    }
    if (req->response_body) {
        free(req->response_body);
        req->response_body = NULL;
    }
    if (req->headers) {
        for (int i = 0; i < req->header_count; i++) {
            if (req->headers[i]) {
                free(req->headers[i]);
            }
        }
        free(req->headers);
        req->headers = NULL;
    }
    free(req);
}

/* Heap-allocated dynamic buffer for safe HTTP request header generation */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} evo_dynbuf_t;

static int dynbuf_init(evo_dynbuf_t *db, size_t initial_cap)
{
    db->data = (char *)malloc(initial_cap);
    if (!db->data) {
        db->len = 0;
        db->cap = 0;
        return -1;
    }
    db->len = 0;
    db->cap = initial_cap;
    db->data[0] = '\0';
    return 0;
}

static void dynbuf_free(evo_dynbuf_t *db)
{
    if (db->data) {
        free(db->data);
        db->data = NULL;
    }
    db->len = 0;
    db->cap = 0;
}

static int dynbuf_append(evo_dynbuf_t *db, const char *str, size_t str_len)
{
    if (db->len + str_len + 1 > db->cap) {
        size_t new_cap = (db->cap == 0) ? 1024 : (db->cap * 2);
        while (new_cap < db->len + str_len + 1) {
            new_cap *= 2;
        }
        char *new_data = (char *)realloc(db->data, new_cap);
        if (!new_data) return -1;
        db->data = new_data;
        db->cap = new_cap;
    }
    memcpy(db->data + db->len, str, str_len);
    db->len += str_len;
    db->data[db->len] = '\0';
    return 0;
}

static int dynbuf_append_fmt(evo_dynbuf_t *db, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap_copy);
        return -1;
    }
    size_t needed = (size_t)n;
    if (db->len + needed + 1 > db->cap) {
        size_t new_cap = (db->cap == 0) ? 1024 : (db->cap * 2);
        while (new_cap < db->len + needed + 1) {
            new_cap *= 2;
        }
        char *new_data = (char *)realloc(db->data, new_cap);
        if (!new_data) {
            va_end(ap_copy);
            return -1;
        }
        db->data = new_data;
        db->cap = new_cap;
    }
    vsnprintf(db->data + db->len, needed + 1, fmt, ap_copy);
    va_end(ap_copy);
    db->len += needed;
    return 0;
}

/*
 * Portable case-insensitive substring search in standard C11 without GNU extensions.
 */
static const char *evo_strcasestr(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return NULL;
    size_t nlen = strlen(needle);
    if (nlen == 0) return haystack;
    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, nlen) == 0)
            return haystack;
    }
    return NULL;
}

/*
 * Searches response headers between headers_start and headers_end for header_name
 * case-insensitively. Returns pointer to first character of trimmed value and sets *out_val_len.
 */
static const char *find_header_val(const char *headers_start,
                                   const char *headers_end,
                                   const char *header_name,
                                   size_t *out_val_len)
{
    if (!headers_start || !headers_end || !header_name) return NULL;
    size_t name_len = strlen(header_name);
    const char *p = headers_start;

    while (p < headers_end) {
        const char *line_start = p;
        const char *line_end = (const char *)memchr(line_start, '\n', (size_t)(headers_end - line_start));
        if (!line_end) {
            line_end = headers_end;
            p = headers_end;
        } else {
            p = line_end + 1;
        }

        if ((size_t)(line_end - line_start) > name_len &&
            strncasecmp(line_start, header_name, name_len) == 0 &&
            line_start[name_len] == ':') {
            const char *val = line_start + name_len + 1;
            while (val < line_end && (*val == ' ' || *val == '\t')) {
                val++;
            }
            const char *val_end = line_end;
            while (val_end > val && (val_end[-1] == '\r' || val_end[-1] == '\n' ||
                                     val_end[-1] == ' '  || val_end[-1] == '\t')) {
                val_end--;
            }
            if (out_val_len) *out_val_len = (size_t)(val_end - val);
            return val;
        }
    }
    return NULL;
}

/*
 * Decodes a chunked-transfer-encoded HTTP body in-place.
 * Reads hex chunk sizes, skips optional chunk extensions, copies chunk payloads,
 * verifies CRLF boundaries, and consumes the terminal chunk and optional trailers.
 * Returns 0 on success, EVO_NET_ERR_CHUNKED on malformed stream, EVO_NET_ERR_BODY_LIMIT if cap exceeded.
 */
static int dechunk_body(char *body, size_t body_len, size_t *out_dechunked_len)
{
    if (!body || !out_dechunked_len) return EVO_NET_ERR_CHUNKED;

    const char *src = body;
    const char *end = body + body_len;
    char *dst = body;

    while (1) {
        if (src >= end) {
            return EVO_NET_ERR_CHUNKED;
        }

        size_t chunk_size = 0;
        int hex_digits = 0;
        while (src < end) {
            char c = *src;
            int digit_val = -1;
            if (c >= '0' && c <= '9')      digit_val = c - '0';
            else if (c >= 'a' && c <= 'f') digit_val = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit_val = c - 'A' + 10;
            else break;

            if (chunk_size > (EVO_NET_MAX_BODY >> 4)) {
                return EVO_NET_ERR_BODY_LIMIT;
            }
            chunk_size = (chunk_size << 4) | (size_t)digit_val;
            hex_digits++;
            src++;
        }

        if (hex_digits == 0) {
            return EVO_NET_ERR_CHUNKED;
        }

        /* Skip optional chunk extensions (;name=value) */
        while (src < end && *src != '\r' && *src != '\n') {
            src++;
        }

        /* Require CRLF boundary after chunk size */
        if (src < end && *src == '\r') src++;
        if (src >= end || *src != '\n') {
            return EVO_NET_ERR_CHUNKED;
        }
        src++;

        /* Terminal chunk (size 0) ends the stream */
        if (chunk_size == 0) {
            /* Skip any trailing headers until an empty line (CRLF or LF) */
            while (src < end) {
                if (*src == '\r') {
                    src++;
                    if (src < end && *src == '\n') {
                        src++;
                        break;
                    }
                } else if (*src == '\n') {
                    src++;
                    break;
                } else {
                    while (src < end && *src != '\n') {
                        src++;
                    }
                    if (src < end && *src == '\n') {
                        src++;
                    }
                }
            }
            break;
        }

        /* Non-terminal chunk: verify available data and enforce body limit */
        if ((size_t)(end - src) < chunk_size) {
            return EVO_NET_ERR_CHUNKED;
        }

        if ((size_t)(dst - body) + chunk_size > EVO_NET_MAX_BODY) {
            return EVO_NET_ERR_BODY_LIMIT;
        }

        memmove(dst, src, chunk_size);
        dst += chunk_size;
        src += chunk_size;

        /* Require CRLF boundary after chunk payload */
        if (src < end && *src == '\r') src++;
        if (src >= end || *src != '\n') {
            return EVO_NET_ERR_CHUNKED;
        }
        src++;
    }

    *dst = '\0';
    *out_dechunked_len = (size_t)(dst - body);
    return 0;
}

/*
 * Parse http://host:port/path and https://host:port/path.
 * Returns 0 on success, -1 on invalid URL/scheme/empty host, -2 on host or path buffer overflow.
 */
static int parse_url(const char *url, char *host, size_t host_size,
                     int *port, char *path, size_t path_size, int *is_https)
{
    if (!url || !host || !port || !path || !is_https) return -1;

    const char *p = url;
    int default_port = 80;
    *is_https = 0;

    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
        default_port = 80;
        *is_https = 0;
    } else if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        default_port = 443;
        *is_https = 1;
    } else {
        return -1;
    }

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');

    if (colon && (!slash || colon < slash)) {
        size_t host_len = (size_t)(colon - p);
        if (host_len == 0) return -1;
        if (host_len >= host_size) return -2; /* Overflow */
        memcpy(host, p, host_len);
        host[host_len] = '\0';

        *port = atoi(colon + 1);
        if (*port <= 0 || *port > 65535) *port = default_port;
    } else {
        size_t host_len = slash ? (size_t)(slash - p) : strlen(p);
        if (host_len == 0) return -1;
        if (host_len >= host_size) return -2; /* Overflow */
        memcpy(host, p, host_len);
        host[host_len] = '\0';
        *port = default_port;
    }

    if (slash) {
        size_t path_len = strlen(slash);
        if (path_len >= path_size) return -2; /* Overflow */
        memcpy(path, slash, path_len + 1);
    } else {
        if (path_size < 2) return -2;
        strcpy(path, "/");
    }

    return 0;
}

static int execute_http(const char *method,
                        const char *url,
                        const char *post_data,
                        const char **headers,
                        int header_count,
                        char **out_body,
                        size_t *out_len,
                        int *out_status)
{
    if (out_body) *out_body = NULL;
    if (out_len) *out_len = 0;
    if (out_status) *out_status = 0;

    if (!url || strlen(url) >= EVO_NET_MAX_URL) {
        return EVO_NET_ERR_TOO_LONG;
    }

    char current_url[EVO_NET_MAX_URL];
    strcpy(current_url, url);

    char current_method[16];
    strncpy(current_method, method ? method : "GET", sizeof(current_method) - 1);
    current_method[sizeof(current_method) - 1] = '\0';

    const char *current_post_data = post_data;
    int redirect_count = 0;

    while (1) {
        char host[EVO_NET_MAX_HOST] = {0};
        int  port = 80;
        char path[EVO_NET_MAX_PATH] = {0};
        int  is_https = 0;

        int pres = parse_url(current_url, host, sizeof(host), &port, path, sizeof(path), &is_https);
        if (pres != 0) {
            return (pres == -2) ? EVO_NET_ERR_TOO_LONG : EVO_NET_ERR_INVALID_URL;
        }

        struct addrinfo hints, *res = NULL, *rp = NULL;
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);

        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
            return EVO_NET_ERR_DNS;
        }

        (void)rp;
        int sock = evo_net_connect_list(res, EVO_NET_CONNECT_TIMEOUT_MS);

        freeaddrinfo(res);

        if (sock < 0) {
            return EVO_NET_ERR_CONNECT;
        }

        /* Initialize SSL if HTTPS */
#ifndef NO_OPENSSL
        SSL *ssl = NULL;
        if (is_https) {
            SSL_CTX *ctx = evo_net_get_ssl_ctx();
            if (!ctx) {
                close(sock);
                return EVO_NET_ERR_SSL_CTX;
            }
            ssl = SSL_new(ctx);
            if (!ssl) {
                close(sock);
                return EVO_NET_ERR_SSL_NEW;
            }
            SSL_set_tlsext_host_name(ssl, host);
            SSL_set_fd(ssl, sock);
            if (SSL_connect(ssl) <= 0) {
                SSL_free(ssl);
                close(sock);
                return EVO_NET_ERR_SSL_CONN;
            }
        }
#else
        if (is_https) {
            close(sock);
            return EVO_NET_ERR_SSL_CTX;
        }
#endif

        /* Dynamically format request headers with full bounds checking */
        evo_dynbuf_t req_buf;
        if (dynbuf_init(&req_buf, 4096) != 0) {
#ifndef NO_OPENSSL
            if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
#endif
            close(sock);
            return EVO_NET_ERR_MEM;
        }

        int build_err = 0;
        char host_hdr[512];
        if ((is_https && port == 443) || (!is_https && port == 80)) {
            snprintf(host_hdr, sizeof host_hdr, "Host: %s\r\n", host);
        } else {
            snprintf(host_hdr, sizeof host_hdr, "Host: %s:%d\r\n", host, port);
        }

        if (dynbuf_append_fmt(&req_buf,
                              "%s %s HTTP/1.1\r\n"
                              "%s"
                              "User-Agent: EVOPlayer-PS5/" EVO_PLAYER_VERSION "\r\n"
                              "Accept: */*\r\n"
                              "Connection: close\r\n",
                              current_method, path, host_hdr) != 0) {
            build_err = 1;
        }

        if (!build_err && current_post_data) {
            size_t post_len = strlen(current_post_data);
            if (dynbuf_append_fmt(&req_buf,
                                  "Content-Type: application/json\r\n"
                                  "Content-Length: %zu\r\n",
                                  post_len) != 0) {
                build_err = 1;
            }
        }

        if (!build_err && headers && header_count > 0) {
            for (int i = 0; i < header_count; i++) {
                if (headers[i]) {
                    if (dynbuf_append_fmt(&req_buf, "%s\r\n", headers[i]) != 0) {
                        build_err = 1;
                        break;
                    }
                }
            }
        }

        if (!build_err) {
            if (dynbuf_append(&req_buf, "\r\n", 2) != 0) {
                build_err = 1;
            }
        }

        if (build_err) {
            dynbuf_free(&req_buf);
#ifndef NO_OPENSSL
            if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
#endif
            close(sock);
            return EVO_NET_ERR_MEM;
        }

        /* Send header */
        int send_err = 0;
#ifndef NO_OPENSSL
        if (is_https) {
            if (SSL_write(ssl, req_buf.data, (int)req_buf.len) <= 0) send_err = 1;
        } else
#endif
        {
            if (send(sock, req_buf.data, req_buf.len, 0) < 0) send_err = 1;
        }

        dynbuf_free(&req_buf);

        if (send_err) {
#ifndef NO_OPENSSL
            if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
#endif
            close(sock);
            return EVO_NET_ERR_SEND_HDR;
        }

        /* Send payload if POST. Not when it is empty: SSL_write of 0 bytes
         * returns 0, which reads as a failure, so every body-less POST over
         * https failed (Jellyfin's Quick Connect start, measured on a
         * reverse-proxied server 2026-10-01). */
        if (current_post_data && current_post_data[0]) {
            size_t post_len = strlen(current_post_data);
            int post_err = 0;
#ifndef NO_OPENSSL
            if (is_https) {
                if (SSL_write(ssl, current_post_data, (int)post_len) <= 0) post_err = 1;
            } else
#endif
            {
                if (send(sock, current_post_data, post_len, 0) < 0) post_err = 1;
            }

            if (post_err) {
#ifndef NO_OPENSSL
                if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
#endif
                close(sock);
                return EVO_NET_ERR_SEND_BODY;
            }
        }

        /* Read response with Content-Length cutoff and hard memory cap */
        size_t cap = 16384;
        size_t total = 0;
        char *buf = (char *)malloc(cap);
        if (!buf) {
#ifndef NO_OPENSSL
            if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
#endif
            close(sock);
            return EVO_NET_ERR_MEM;
        }

        ssize_t content_length = -1;
        size_t header_len = 0;
        int headers_parsed = 0;

        while (1) {
            size_t to_read = cap - total - 1;
            if (content_length >= 0) {
                size_t target_total = header_len + (size_t)content_length;
                if (total >= target_total) {
                    break;
                }
                if (to_read > target_total - total) {
                    to_read = target_total - total;
                }
            }

            ssize_t n;
#ifndef NO_OPENSSL
            if (is_https) {
                n = SSL_read(ssl, buf + total, (int)to_read);
            } else
#endif
            {
                n = recv(sock, buf + total, to_read, 0);
            }

            if (n <= 0) break;

            total += (size_t)n;
            buf[total] = '\0';

            /* Parse headers to locate Content-Length and Transfer-Encoding */
            if (!headers_parsed) {
                char *hend = strstr(buf, "\r\n\r\n");
                size_t delim_len = 4;
                if (!hend) {
                    hend = strstr(buf, "\n\n");
                    delim_len = 2;
                }
                if (hend) {
                    headers_parsed = 1;
                    header_len = (size_t)(hend - buf) + delim_len;

                    /* Check if chunked encoding overrides Content-Length */
                    size_t te_len = 0;
                    const char *te_val = find_header_val(buf, hend, "Transfer-Encoding", &te_len);
                    int is_chunked = 0;
                    if (te_val && te_len > 0) {
                        char te_buf[64];
                        if (te_len < sizeof(te_buf)) {
                            memcpy(te_buf, te_val, te_len);
                            te_buf[te_len] = '\0';
                            if (evo_strcasestr(te_buf, "chunked")) {
                                is_chunked = 1;
                            }
                        }
                    }

                    if (!is_chunked) {
                        size_t cl_len = 0;
                        const char *cl_val = find_header_val(buf, hend, "Content-Length", &cl_len);
                        if (cl_val && cl_len > 0) {
                            char cl_buf[32];
                            if (cl_len < sizeof(cl_buf)) {
                                memcpy(cl_buf, cl_val, cl_len);
                                cl_buf[cl_len] = '\0';
                                long long cl = atoll(cl_buf);
                                if (cl >= 0) {
                                    if ((size_t)cl > EVO_NET_MAX_BODY) {
                                        free(buf);
#ifndef NO_OPENSSL
                                        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
#endif
                                        close(sock);
                                        return EVO_NET_ERR_BODY_LIMIT;
                                    }
                                    content_length = (ssize_t)cl;
                                }
                            }
                        }
                    }
                }
            }

            /* Hard body cap check */
            if (headers_parsed) {
                size_t body_so_far = (total > header_len) ? (total - header_len) : 0;
                if (body_so_far > EVO_NET_MAX_BODY) {
                    free(buf);
#ifndef NO_OPENSSL
                    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
#endif
                    close(sock);
                    return EVO_NET_ERR_BODY_LIMIT;
                }
            } else {
                if (total > EVO_NET_MAX_BODY + 65536) {
                    free(buf);
#ifndef NO_OPENSSL
                    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
#endif
                    close(sock);
                    return EVO_NET_ERR_BODY_LIMIT;
                }
            }

            if (content_length >= 0 && total >= header_len + (size_t)content_length) {
                break;
            }

            /* Expand read buffer when close to capacity */
            if (total + 4096 >= cap) {
                size_t new_cap = cap * 2;
                if (new_cap > EVO_NET_MAX_BODY + 131072) {
                    new_cap = EVO_NET_MAX_BODY + 131072;
                }
                if (new_cap <= cap) {
                    free(buf);
#ifndef NO_OPENSSL
                    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
#endif
                    close(sock);
                    return EVO_NET_ERR_BODY_LIMIT;
                }
                char *new_buf = (char *)realloc(buf, new_cap);
                if (!new_buf) {
                    free(buf);
#ifndef NO_OPENSSL
                    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
#endif
                    close(sock);
                    return EVO_NET_ERR_REALLOC;
                }
                buf = new_buf;
                cap = new_cap;
            }
        }

#ifndef NO_OPENSSL
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ssl = NULL;
        }
#endif
        close(sock);
        sock = -1;

        buf[total] = '\0';

        /* Parse HTTP Status Code and header boundary */
        int status = 0;
        char *header_end = strstr(buf, "\r\n\r\n");
        if (!header_end) {
            header_end = strstr(buf, "\n\n");
            if (header_end) header_end += 2;
        } else {
            header_end += 4;
        }

        if (sscanf(buf, "HTTP/1.%*d %d", &status) != 1) {
            status = 0;
        }

        /* Check for HTTP Redirects */
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            size_t loc_len = 0;
            const char *loc_val = header_end ? find_header_val(buf, header_end, "Location", &loc_len) : NULL;
            if (loc_val && loc_len > 0) {
                if (redirect_count >= EVO_NET_MAX_REDIRECTS) {
                    free(buf);
                    return EVO_NET_ERR_REDIRECTS;
                }

                char loc_buf[EVO_NET_MAX_URL];
                if (loc_len >= sizeof(loc_buf)) {
                    free(buf);
                    return EVO_NET_ERR_TOO_LONG;
                }
                memcpy(loc_buf, loc_val, loc_len);
                loc_buf[loc_len] = '\0';

                char next_url[EVO_NET_MAX_URL];
                if (strncmp(loc_buf, "http://", 7) == 0) {
                    if (is_https) {
                        /* Disallow insecure downgrade */
                        free(buf);
                        return EVO_NET_ERR_DOWNGRADE;
                    }
                    strcpy(next_url, loc_buf);
                } else if (strncmp(loc_buf, "https://", 8) == 0) {
                    strcpy(next_url, loc_buf);
                } else {
                    /*
                     * A relative Location. Scheme, host and port carry over;
                     * what the path is relative TO depends on the leading '/'
                     * (RFC 3986 5.3):
                     *
                     *   "/b/c"  replaces the whole path
                     *   "b/c"   replaces the last SEGMENT of the current path
                     *
                     * The second case is why this is not simply a '/' prepend.
                     * A provider serving its bundle from /ui/iptv/ and
                     * redirecting manifest.json to "v2/manifest.json" means
                     * /ui/iptv/v2/manifest.json; prepending '/' asks the
                     * server for /v2/manifest.json, gets a 404, and the
                     * bundle load falls back to the embedded skin for no
                     * visible reason.
                     */
                    size_t scheme_len = is_https ? 8 : 7;
                    size_t host_len = strlen(host);
                    char port_part[16] = {0};
                    if (!((is_https && port == 443) || (!is_https && port == 80))) {
                        snprintf(port_part, sizeof(port_part), ":%d", port);
                    }
                    size_t port_len = strlen(port_part);

                    /* The current path up to and including its last '/', or
                     * "/" when the path has no directory part at all. */
                    size_t base_len = 0;
                    if (loc_buf[0] != '/') {
                        const char *last = strrchr(path, '/');
                        /* Stop at the query string: a '/' inside ?a=b/c is not
                         * a path separator. */
                        const char *q = strchr(path, '?');
                        if (q && last && last > q) {
                            for (last = q - 1; last >= path && *last != '/'; --last) { }
                            if (last < path) last = NULL;
                        }
                        base_len = last ? (size_t)(last - path) + 1 : 1;
                    }

                    if (scheme_len + host_len + port_len + base_len + loc_len
                            >= sizeof(next_url)) {
                        free(buf);
                        return EVO_NET_ERR_TOO_LONG;
                    }

                    char *wp = next_url;
                    memcpy(wp, is_https ? "https://" : "http://", scheme_len);
                    wp += scheme_len;
                    memcpy(wp, host, host_len);
                    wp += host_len;
                    if (port_len > 0) {
                        memcpy(wp, port_part, port_len);
                        wp += port_len;
                    }
                    if (base_len == 1 && loc_buf[0] != '/') {
                        *wp++ = '/';
                    } else if (base_len > 1) {
                        memcpy(wp, path, base_len);
                        wp += base_len;
                    }
                    memcpy(wp, loc_buf, loc_len);
                    wp += loc_len;
                    *wp = '\0';
                }

                /* Switch method and drop body on 303, or 301/302 from POST */
                if (status == 303 || ((status == 301 || status == 302) && strcmp(current_method, "POST") == 0)) {
                    strcpy(current_method, "GET");
                    current_post_data = NULL;
                }

                strncpy(current_url, next_url, sizeof(current_url) - 1);
                current_url[sizeof(current_url) - 1] = '\0';
                redirect_count++;

                free(buf);
                continue;
            }
        }

        /* Non-redirect: prepare final response body */
        if (out_status) *out_status = status;

        if (header_end) {
            size_t body_len = total - (size_t)(header_end - buf);
            char *body = (char *)malloc(body_len + 1);
            if (!body) {
                free(buf);
                return EVO_NET_ERR_MEM;
            }
            memcpy(body, header_end, body_len);
            body[body_len] = '\0';

            /* Check for chunked transfer encoding and decode in-place */
            size_t te_len = 0;
            const char *te_val = find_header_val(buf, header_end, "Transfer-Encoding", &te_len);
            if (te_val && te_len > 0) {
                char te_buf[64];
                if (te_len < sizeof(te_buf)) {
                    memcpy(te_buf, te_val, te_len);
                    te_buf[te_len] = '\0';
                    if (evo_strcasestr(te_buf, "chunked")) {
                        size_t dechunked_len = 0;
                        int dres = dechunk_body(body, body_len, &dechunked_len);
                        if (dres != 0) {
                            free(body);
                            free(buf);
                            return dres;
                        }
                        body_len = dechunked_len;
                    }
                }
            }

            if (body_len > EVO_NET_MAX_BODY) {
                free(body);
                free(buf);
                return EVO_NET_ERR_BODY_LIMIT;
            }

            if (out_body) {
                *out_body = body;
            } else {
                free(body);
            }
            if (out_len) *out_len = body_len;
        } else {
            if (out_body) {
                *out_body = strdup("");
            }
            if (out_len) *out_len = 0;
        }

        free(buf);
        return (status >= 200 && status < 400) ? EVO_NET_OK : EVO_NET_ERR_HTTP;
    }
}

int evo_net_http_get_sync(const char *url,
                          const char **headers,
                          int header_count,
                          char **out_body,
                          size_t *out_len,
                          int *out_status)
{
    return execute_http("GET", url, NULL, headers, header_count,
                        out_body, out_len, out_status);
}

int evo_net_http_post_sync(const char *url,
                           const char *post_data,
                           const char **headers,
                           int header_count,
                           char **out_body,
                           size_t *out_len,
                           int *out_status)
{
    return execute_http("POST", url, post_data, headers, header_count,
                        out_body, out_len, out_status);
}

/* Background worker thread */
static void *evo_net_worker(void *arg)
{
    (void)arg;

    while (g_running) {
        evo_net_req_t *req = NULL;

        pthread_mutex_lock(&g_queue_mutex);
        while (g_running && g_pending_count == 0) {
            pthread_cond_wait(&g_queue_cond, &g_queue_mutex);
        }

        if (!g_running) {
            pthread_mutex_unlock(&g_queue_mutex);
            break;
        }

        /* Dequeue first pending item */
        req = g_pending_queue[0];
        for (int i = 0; i < g_pending_count - 1; i++) {
            g_pending_queue[i] = g_pending_queue[i + 1];
        }
        g_pending_count--;
        pthread_mutex_unlock(&g_queue_mutex);

        if (req) {
            /* Execute HTTP */
            int res = execute_http(req->method, req->url, req->post_data,
                                   (const char **)req->headers, req->header_count,
                                   &req->response_body, &req->response_len,
                                   &req->status_code);

            req->success   = (res == 0);
            req->error     = res;       /* kept: the callback only gets success/status */
            req->completed = 1;

            /* Push to completed queue */
            pthread_mutex_lock(&g_queue_mutex);
            if (g_completed_count < EVO_NET_MAX_QUEUE) {
                g_completed_queue[g_completed_count++] = req;
            } else {
                /* Overflow drop */
                free_req(req);
            }
            pthread_mutex_unlock(&g_queue_mutex);
        }
    }

    return NULL;
}

int evo_net_init(void)
{
    if (g_running) return 0;

    g_running = 1;
    g_pending_count = 0;
    g_completed_count = 0;

    if (pthread_create(&g_worker_thread, NULL, evo_net_worker, NULL) != 0) {
        g_running = 0;
        return -1;
    }

    return 0;
}

void evo_net_shutdown(void)
{
    if (!g_running) return;

    g_running = 0;
    pthread_mutex_lock(&g_queue_mutex);
    pthread_cond_broadcast(&g_queue_cond);
    pthread_mutex_unlock(&g_queue_mutex);

    pthread_join(g_worker_thread, NULL);

    /* Free pending */
    for (int i = 0; i < g_pending_count; i++) {
        free_req(g_pending_queue[i]);
    }
    g_pending_count = 0;

    /* Free completed */
    for (int i = 0; i < g_completed_count; i++) {
        free_req(g_completed_queue[i]);
    }
    g_completed_count = 0;

#ifndef NO_OPENSSL
    pthread_mutex_lock(&g_ssl_init_mutex);
    if (g_ssl_ctx) {
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = NULL;
    }
    pthread_mutex_unlock(&g_ssl_init_mutex);
#endif
}

int evo_net_request_async(const char *method,
                          const char *url,
                          const char *post_data,
                          const char **headers,
                          int header_count,
                          evo_net_cb callback,
                          void *user_data)
{
    if (!method || !url || strlen(url) >= EVO_NET_MAX_URL || strlen(method) >= 16) {
        return EVO_NET_ASYNC_ERR_ARG;
    }

    if (!g_running) {
        if (evo_net_init() != 0) return EVO_NET_ASYNC_ERR_INIT;
    }

    evo_net_req_t *req = (evo_net_req_t *)malloc(sizeof(evo_net_req_t));
    if (!req) return EVO_NET_ASYNC_ERR_MEM;

    memset(req, 0, sizeof(*req));
    strncpy(req->method, method, sizeof(req->method) - 1);
    strncpy(req->url, url, sizeof(req->url) - 1);
    if (post_data) {
        req->post_data = strdup(post_data);
        if (!req->post_data) {
            free_req(req);
            return EVO_NET_ASYNC_ERR_MEM;
        }
    }
    req->callback  = callback;
    req->user_data = user_data;

    if (header_count > 0 && headers) {
        req->headers = (char **)calloc((size_t)header_count, sizeof(char *));
        if (!req->headers) {
            free_req(req);
            return EVO_NET_ASYNC_ERR_MEM;
        }
        req->header_count = header_count;
        for (int i = 0; i < header_count; i++) {
            if (headers[i]) {
                req->headers[i] = strdup(headers[i]);
                if (!req->headers[i]) {
                    free_req(req);
                    return EVO_NET_ASYNC_ERR_MEM;
                }
            }
        }
    }

    pthread_mutex_lock(&g_queue_mutex);
    if (g_pending_count >= EVO_NET_MAX_QUEUE) {
        pthread_mutex_unlock(&g_queue_mutex);
        free_req(req);
        return EVO_NET_ASYNC_ERR_FULL;
    }

    g_pending_queue[g_pending_count++] = req;
    pthread_cond_signal(&g_queue_cond);
    pthread_mutex_unlock(&g_queue_mutex);

    return EVO_NET_ASYNC_OK;
}

/* ---- Why a request failed, in words (the callback only gets success/status) ---- */

static int          g_cb_error;            /* EVO_NET_ERR_* of the callback running now */
static pthread_mutex_t g_fail_mx = PTHREAD_MUTEX_INITIALIZER;
static char         g_last_fail_msg[200];
static long long    g_last_fail_at;

static long long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

/* Posters and icons fail all the time while browsing; they are never "the
 * reason" something else did not load. */
static int url_is_image(const char *url)
{
    const char *end = url + strcspn(url, "?#");
    static const char *const ext[] = { ".jpg", ".jpeg", ".png", ".webp", ".gif", ".svg", ".ico", NULL };
    for (int i = 0; ext[i]; ++i) {
        size_t l = strlen(ext[i]);
        if ((size_t)(end - url) >= l && strncasecmp(end - l, ext[i], l) == 0) return 1;
    }
    return 0;
}

int evo_net_callback_error(void) { return g_cb_error; }

void evo_net_describe_failure(int err, int status, const char *url, char *out, size_t cap)
{
    if (!out || !cap) return;
    char host[EVO_NET_MAX_HOST];
    evo_error_url_host(url ? url : "", host, sizeof host);

    char tmp[128];
    const char *what;
    switch (err) {
    case EVO_NET_ERR_INVALID_URL: what = "that is not a valid http(s) address"; break;
    case EVO_NET_ERR_DNS:         what = "could not find the server (name lookup failed)"; break;
    case EVO_NET_ERR_CONNECT:     what = "could not connect to the server"; break;
    case EVO_NET_ERR_SEND_HDR:
    case EVO_NET_ERR_SEND_BODY:   what = "the connection dropped while sending the request"; break;
    case EVO_NET_ERR_MEM:
    case EVO_NET_ERR_REALLOC:     what = "ran out of memory downloading the reply"; break;
    case EVO_NET_ERR_HTTP: {
        const char *why = (status == 401 || status == 403) ? "refused - check the username, password or link"
                        : status == 404                    ? "not found - check the address"
                        : status == 429                    ? "too many requests - try again shortly"
                        : status >= 500                    ? "the server has a problem"
                                                           : "error";
        snprintf(tmp, sizeof tmp, "the server answered HTTP %d (%s)", status, why);
        what = tmp;
        break;
    }
    case EVO_NET_ERR_SSL_CTX:
    case EVO_NET_ERR_SSL_NEW:
    case EVO_NET_ERR_SSL_CONN:    what = "the secure (https) connection failed"; break;
    case EVO_NET_ERR_TOO_LONG:    what = "the address is too long"; break;
    case EVO_NET_ERR_REDIRECTS:   what = "too many redirects"; break;
    case EVO_NET_ERR_DOWNGRADE:   what = "redirect from https to http refused"; break;
    case EVO_NET_ERR_CHUNKED:     what = "the server's reply was malformed"; break;
    case EVO_NET_ERR_BODY_LIMIT:  what = "the reply is too large (over 64 MB)"; break;
    default:
        snprintf(tmp, sizeof tmp, "network error %d", err);
        what = tmp;
        break;
    }
    snprintf(out, cap, "%s%s%s", host, host[0] ? ": " : "", what);
}

int evo_net_last_failure(char *out, size_t cap, int max_age_ms)
{
    int have = 0;
    pthread_mutex_lock(&g_fail_mx);
    if (g_last_fail_msg[0] && mono_ms() - g_last_fail_at <= max_age_ms) {
        if (out && cap) snprintf(out, cap, "%s", g_last_fail_msg);
        have = 1;
    }
    pthread_mutex_unlock(&g_fail_mx);
    return have;
}

int evo_failure_reason(char *out, size_t cap)
{
    if (evo_error_take(out, cap)) return 1;
    return evo_net_last_failure(out, cap, 5000);
}

void evo_net_poll(void)
{
    evo_net_req_t *ready[EVO_NET_MAX_QUEUE];
    int count = 0;

    pthread_mutex_lock(&g_queue_mutex);
    count = g_completed_count;
    for (int i = 0; i < count; i++) {
        ready[i] = g_completed_queue[i];
    }
    g_completed_count = 0;
    pthread_mutex_unlock(&g_queue_mutex);

    for (int i = 0; i < count; i++) {
        evo_net_req_t *req = ready[i];
        g_cb_error = req->success ? 0 : req->error;
        if (!req->success && !url_is_image(req->url)) {
            char why[sizeof g_last_fail_msg];
            evo_net_describe_failure(req->error, req->status_code, req->url, why, sizeof why);
            pthread_mutex_lock(&g_fail_mx);
            memcpy(g_last_fail_msg, why, sizeof g_last_fail_msg);
            g_last_fail_at = mono_ms();
            pthread_mutex_unlock(&g_fail_mx);
        }
        if (req->callback) {
            req->callback(req->success, req->status_code,
                          req->response_body, req->response_len,
                          req->user_data);
        }
        free_req(req);
    }
}
