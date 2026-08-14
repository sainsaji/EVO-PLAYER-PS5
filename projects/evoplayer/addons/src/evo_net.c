/*
 * evo_net.c — Non-blocking HTTP/REST client implementation using BSD sockets.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>

#include "evo_net.h"
#include <openssl/ssl.h>
#include <openssl/err.h>

#define EVO_NET_MAX_QUEUE    32
#define EVO_NET_BUFFER_SIZE  8192
#define EVO_NET_TIMEOUT_SEC  6

typedef struct evo_net_req {
    char        method[8];
    char        url[512];
    char       *post_data;
    char      **headers;
    int         header_count;
    evo_net_cb  callback;
    void       *user_data;

    /* Result */
    int         completed;
    int         success;
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

/* Parse http://host:port/path and https://host:port/path */
static int parse_url(const char *url, char *host, int *port, char *path, int *is_https)
{
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
    }

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');

    if (colon && (!slash || colon < slash)) {
        size_t host_len = colon - p;
        if (host_len >= 128) host_len = 127;
        strncpy(host, p, host_len);
        host[host_len] = '\0';

        *port = atoi(colon + 1);
        if (*port <= 0) *port = default_port;
    } else {
        size_t host_len = slash ? (size_t)(slash - p) : strlen(p);
        if (host_len >= 128) host_len = 127;
        strncpy(host, p, host_len);
        host[host_len] = '\0';
        *port = default_port;
    }

    if (slash) {
        strncpy(path, slash, 256);
        path[255] = '\0';
    } else {
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
    char host[128] = {0};
    int  port = 80;
    char path[256] = {0};
    int  is_https = 0;

    if (parse_url(url, host, &port, path, &is_https) != 0)
        return -1;

    struct addrinfo hints, *res = NULL, *rp = NULL;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
        return -2;

    int sock = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) continue;

        /* Set send/receive timeouts */
        struct timeval tv;
        tv.tv_sec = EVO_NET_TIMEOUT_SEC;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0)
            break;

        close(sock);
        sock = -1;
    }

    freeaddrinfo(res);

    if (sock < 0)
        return -3;

    /* Initialize SSL if HTTPS */
    SSL *ssl = NULL;
    if (is_https) {
        SSL_CTX *ctx = evo_net_get_ssl_ctx();
        if (!ctx) {
            close(sock);
            return -10;
        }
        ssl = SSL_new(ctx);
        if (!ssl) {
            close(sock);
            return -11;
        }
        SSL_set_tlsext_host_name(ssl, host);
        SSL_set_fd(ssl, sock);
        if (SSL_connect(ssl) <= 0) {
            SSL_free(ssl);
            close(sock);
            return -12;
        }
    }

    /* Build HTTP request header */
    char req_buf[2048];
    int req_len = snprintf(req_buf, sizeof(req_buf),
                           "%s %s HTTP/1.1\r\n"
                           "Host: %s:%d\r\n"
                           "User-Agent: EVOPlayer-PS5/0.7.0\r\n"
                           "Accept: application/json, text/plain, */*\r\n"
                           "Connection: close\r\n",
                           method, path, host, port);

    if (post_data) {
        size_t post_len = strlen(post_data);
        req_len += snprintf(req_buf + req_len, sizeof(req_buf) - req_len,
                            "Content-Type: application/json\r\n"
                            "Content-Length: %zu\r\n", post_len);
    }

    for (int i = 0; i < header_count; i++) {
        if (headers[i]) {
            req_len += snprintf(req_buf + req_len, sizeof(req_buf) - req_len,
                                "%s\r\n", headers[i]);
        }
    }

    req_len += snprintf(req_buf + req_len, sizeof(req_buf) - req_len, "\r\n");

    /* Send header */
    int send_err = 0;
    if (is_https) {
        if (SSL_write(ssl, req_buf, req_len) <= 0) send_err = 1;
    } else {
        if (send(sock, req_buf, req_len, 0) < 0) send_err = 1;
    }

    if (send_err) {
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
        close(sock);
        return -4;
    }

    /* Send payload if POST */
    if (post_data) {
        size_t post_len = strlen(post_data);
        int post_err = 0;
        if (is_https) {
            if (SSL_write(ssl, post_data, post_len) <= 0) post_err = 1;
        } else {
            if (send(sock, post_data, post_len, 0) < 0) post_err = 1;
        }
        if (post_err) {
            if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
            close(sock);
            return -5;
        }
    }

    /* Read response */
    size_t cap = 16384;
    size_t total = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
        close(sock);
        return -6;
    }

    ssize_t n;
    while (1) {
        if (is_https) {
            n = SSL_read(ssl, buf + total, cap - total - 1);
        } else {
            n = recv(sock, buf + total, cap - total - 1, 0);
        }

        if (n <= 0) break;

        total += n;
        if (total + 4096 >= cap) {
            cap *= 2;
            char *new_buf = (char *)realloc(buf, cap);
            if (!new_buf) {
                free(buf);
                if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
                close(sock);
                return -7;
            }
            buf = new_buf;
        }
    }

    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    close(sock);

    buf[total] = '\0';

    /* Parse Status Code and Header boundary */
    int status = 0;
    char *header_end = strstr(buf, "\r\n\r\n");
    if (!header_end) {
        header_end = strstr(buf, "\n\n");
        if (header_end) header_end += 2;
    } else {
        header_end += 4;
    }

    if (sscanf(buf, "HTTP/1.%*d %d", &status) != 1)
        status = 0;

    if (out_status) *out_status = status;

    if (header_end) {
        size_t body_len = total - (size_t)(header_end - buf);
        char *body = (char *)malloc(body_len + 1);
        if (body) {
            memcpy(body, header_end, body_len);
            body[body_len] = '\0';
            *out_body = body;
            if (out_len) *out_len = body_len;
        } else {
            *out_body = NULL;
            if (out_len) *out_len = 0;
        }
    } else {
        *out_body = strdup("");
        if (out_len) *out_len = 0;
    }

    free(buf);
    return (status >= 200 && status < 400) ? 0 : -8;
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
            req->completed = 1;

            /* Push to completed queue */
            pthread_mutex_lock(&g_queue_mutex);
            if (g_completed_count < EVO_NET_MAX_QUEUE) {
                g_completed_queue[g_completed_count++] = req;
            } else {
                /* Overflow drop */
                if (req->response_body) free(req->response_body);
                free(req);
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
        if (g_pending_queue[i]->post_data) free(g_pending_queue[i]->post_data);
        free(g_pending_queue[i]);
    }
    g_pending_count = 0;

    /* Free completed */
    for (int i = 0; i < g_completed_count; i++) {
        if (g_completed_queue[i]->post_data) free(g_completed_queue[i]->post_data);
        if (g_completed_queue[i]->response_body) free(g_completed_queue[i]->response_body);
        free(g_completed_queue[i]);
    }
    g_completed_count = 0;

    pthread_mutex_lock(&g_ssl_init_mutex);
    if (g_ssl_ctx) {
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = NULL;
    }
    pthread_mutex_unlock(&g_ssl_init_mutex);
}

int evo_net_request_async(const char *method,
                          const char *url,
                          const char *post_data,
                          const char **headers,
                          int header_count,
                          evo_net_cb callback,
                          void *user_data)
{
    if (!g_running) {
        if (evo_net_init() != 0) return -1;
    }

    evo_net_req_t *req = (evo_net_req_t *)malloc(sizeof(evo_net_req_t));
    if (!req) return -2;

    memset(req, 0, sizeof(*req));
    strncpy(req->method, method, sizeof(req->method) - 1);
    strncpy(req->url, url, sizeof(req->url) - 1);
    if (post_data) req->post_data = strdup(post_data);
    req->callback  = callback;
    req->user_data = user_data;

    if (header_count > 0 && headers) {
        req->headers = (char **)malloc(sizeof(char *) * header_count);
        if (req->headers) {
            req->header_count = header_count;
            for (int i = 0; i < header_count; i++) {
                req->headers[i] = headers[i] ? strdup(headers[i]) : NULL;
            }
        }
    }

    pthread_mutex_lock(&g_queue_mutex);
    if (g_pending_count >= EVO_NET_MAX_QUEUE) {
        pthread_mutex_unlock(&g_queue_mutex);
        if (req->post_data) free(req->post_data);
        free(req);
        return -3;
    }

    g_pending_queue[g_pending_count++] = req;
    pthread_cond_signal(&g_queue_cond);
    pthread_mutex_unlock(&g_queue_mutex);

    return 0;
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
        if (req->callback) {
            req->callback(req->success, req->status_code,
                          req->response_body, req->response_len,
                          req->user_data);
        }

        if (req->post_data) free(req->post_data);
        if (req->response_body) free(req->response_body);
        if (req->headers) {
            for (int h = 0; h < req->header_count; h++) {
                if (req->headers[h]) free(req->headers[h]);
            }
            free(req->headers);
        }
        free(req);
    }
}
