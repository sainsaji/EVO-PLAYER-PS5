#include "evo_smb_io.h"
#include "evo_boot_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>
#include <poll.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#if defined(EVO_TARGET_PS5)
#include <libavutil/error.h>
#include <libavutil/mem.h>
#define HAVE_LIBSMB2 1
#include <time.h>
#include <sys/types.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#endif

#define SMB_AVIO_BUFFER_SIZE (1 << 20) /* 1 MiB stream buffer */

struct evo_smb_io {
#if defined(HAVE_LIBSMB2)
    struct smb2_context *ctx;
    struct smb2fh       *fh;
#endif
    AVIOContext         *avio;
    uint64_t             pos;
    uint64_t             size;
    char                 host[128];
    int                  port;
    char                 user[256];
    char                 password[256];
    char                 domain[128];
    char                 share[128];
    char                 path[1024];
};

int evo_url_decode(const char *src, char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return -1;
    if (!src) { dst[0] = '\0'; return 0; }
    size_t i = 0, j = 0;
    while (src[i]) {
        if (j + 1 >= dst_len) {
            dst[j] = '\0';
            return -1;
        }
        if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) && isxdigit((unsigned char)src[i + 2])) {
            char hex[3] = { src[i + 1], src[i + 2], '\0' };
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
    return 0;
}

static long evo_smb_ms_since(const struct timeval *t0) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return (long)((now.tv_sec - t0->tv_sec) * 1000L + (now.tv_usec - t0->tv_usec) / 1000L);
}

/*
 * A connect() to a host that silently drops packets sits in the kernel's SYN
 * retry for 75 s, and on the PS5 even a non-blocking connect bounded by poll()
 * did not return earlier here (hardware, 2026-10-27: precheck took 75000 ms).
 * So the connect runs in a detached helper and the caller only waits for it
 * for `timeout_sec`; an abandoned helper finishes on its own and frees itself.
 */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int             done;
    int             ok;
    int             abandoned;
    char            host[160];
    int             port;
} smb_probe_t;

static void *smb_probe_thread(void *arg) {
    smb_probe_t *p = (smb_probe_t *)arg;
    int ok = 0;
    char port_str[16];
    struct addrinfo hints, *res = NULL;
    snprintf(port_str, sizeof(port_str), "%d", p->port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(p->host, port_str, &hints, &res) == 0 && res) {
        int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd >= 0) {
            ok = (connect(fd, res->ai_addr, res->ai_addrlen) == 0);   /* connect-ok: on the precheck's own worker */
            close(fd);
        }
        freeaddrinfo(res);
    }
    pthread_mutex_lock(&p->lock);
    p->ok = ok;
    p->done = 1;
    int free_it = p->abandoned;
    pthread_cond_signal(&p->cond);
    pthread_mutex_unlock(&p->lock);
    if (free_it) {
        pthread_mutex_destroy(&p->lock);
        pthread_cond_destroy(&p->cond);
        free(p);
    }
    return NULL;
}

int evo_smb_tcp_precheck(const char *host, int port, int timeout_sec) {
    if (!host || !host[0] || strlen(host) >= sizeof(((smb_probe_t *)0)->host)) return -1;
    if (port <= 0) port = 445;
    int timeout_ms = (timeout_sec > 0) ? (timeout_sec * 1000) : 4000;

    struct timeval t0;
    gettimeofday(&t0, NULL);

    smb_probe_t *p = (smb_probe_t *)calloc(1, sizeof(*p));
    if (!p) return -1;
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->cond, NULL);
    snprintf(p->host, sizeof(p->host), "%s", host);
    p->port = port;

    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int started = (pthread_create(&th, &attr, smb_probe_thread, p) == 0);
    pthread_attr_destroy(&attr);
    if (!started) {
        pthread_mutex_destroy(&p->lock);
        pthread_cond_destroy(&p->cond);
        free(p);
        return -1;
    }

    struct timespec until;
    struct timeval now;
    gettimeofday(&now, NULL);
    until.tv_sec = now.tv_sec + timeout_ms / 1000;
    until.tv_nsec = (long)now.tv_usec * 1000L + (long)(timeout_ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&p->lock);
    while (!p->done) {
        if (pthread_cond_timedwait(&p->cond, &p->lock, &until) != 0) break;
    }
    int done = p->done;
    int ok = done && p->ok;
    if (!done) p->abandoned = 1;
    pthread_mutex_unlock(&p->lock);
    if (done) {
        pthread_mutex_destroy(&p->lock);
        pthread_cond_destroy(&p->cond);
        free(p);
    }

    evo_log("smb precheck: %s:%d %s after %ld ms", host, port,
            ok ? "reachable" : (done ? "refused" : "timed out"), evo_smb_ms_since(&t0));
    return ok ? 0 : -1;
}


static inline int url_decode(const char *src, char *dst, size_t dst_len) {
    return evo_url_decode(src, dst, dst_len);
}

typedef struct {
    char user[256];
    char password[256];
    char domain[128];
    char host[128];
    int  port;
    char share[128];
    char path[1024];
} smb_parsed_url_t;

static int parse_smb_url(const char *url, smb_parsed_url_t *out) {
    if (!url || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->port = 445;

    if (strncmp(url, "smb://", 6) != 0) {
        return -1;
    }

    const char *p = url + 6;
    const char *first_slash = strchr(p, '/');
    if (!first_slash) return -1;

    char auth_host[1024];
    size_t auth_host_len = (size_t)(first_slash - p);
    if (auth_host_len >= sizeof(auth_host)) return -1;
    memcpy(auth_host, p, auth_host_len);
    auth_host[auth_host_len] = '\0';

    const char *path_start = first_slash + 1;
    while (*path_start == '/') path_start++;

    const char *second_slash = strchr(path_start, '/');
    if (second_slash) {
        size_t share_len = (size_t)(second_slash - path_start);
        char enc_share[128];
        if (share_len >= sizeof(enc_share)) return -1;
        memcpy(enc_share, path_start, share_len);
        enc_share[share_len] = '\0';
        if (url_decode(enc_share, out->share, sizeof(out->share)) < 0) return -1;

        const char *subpath = second_slash + 1;
        while (*subpath == '/') subpath++;
        if (url_decode(subpath, out->path, sizeof(out->path)) < 0) return -1;
    } else {
        if (url_decode(path_start, out->share, sizeof(out->share)) < 0) return -1;
        out->path[0] = '\0';
    }

    if (out->share[0] == '\0') return -1;

    // Parse auth_host: [domain;][user[:pass]@]host[:port]
    const char *at = strrchr(auth_host, '@');
    const char *host_part = auth_host;
    if (at) {
        char auth_str[640];
        size_t auth_len = (size_t)(at - auth_host);
        if (auth_len >= sizeof(auth_str)) return -1;
        memcpy(auth_str, auth_host, auth_len);
        auth_str[auth_len] = '\0';

        const char *user_start = auth_str;
        const char *semi = strchr(auth_str, ';');
        if (semi) {
            size_t dom_len = (size_t)(semi - auth_str);
            char enc_dom[128];
            if (dom_len >= sizeof(enc_dom)) return -1;
            memcpy(enc_dom, auth_str, dom_len);
            enc_dom[dom_len] = '\0';
            if (url_decode(enc_dom, out->domain, sizeof(out->domain)) < 0) return -1;
            user_start = semi + 1;
        }

        const char *colon = strchr(user_start, ':');
        if (colon) {
            size_t u_len = (size_t)(colon - user_start);
            char enc_user[256];
            if (u_len >= sizeof(enc_user)) return -1;
            memcpy(enc_user, user_start, u_len);
            enc_user[u_len] = '\0';
            if (url_decode(enc_user, out->user, sizeof(out->user)) < 0) return -1;
            if (url_decode(colon + 1, out->password, sizeof(out->password)) < 0) return -1;
        } else {
            if (url_decode(user_start, out->user, sizeof(out->user)) < 0) return -1;
        }
        host_part = at + 1;
    }

    const char *colon_port = strrchr(host_part, ':');
    if (colon_port) {
        size_t h_len = (size_t)(colon_port - host_part);
        if (h_len >= sizeof(out->host)) return -1;
        memcpy(out->host, host_part, h_len);
        out->host[h_len] = '\0';
        out->port = atoi(colon_port + 1);
        if (out->port <= 0 || out->port > 65535) out->port = 445;
    } else {
        if (strlen(host_part) >= sizeof(out->host)) return -1;
        strncpy(out->host, host_part, sizeof(out->host) - 1);
        out->host[sizeof(out->host) - 1] = '\0';
    }

    return (out->host[0] != '\0') ? 0 : -1;
}

#if defined(HAVE_LIBSMB2)

static int smb_connect_and_open(const char *host, int port,
                                const char *user, const char *password,
                                const char *domain, const char *share,
                                const char *path,
                                struct smb2_context **out_ctx,
                                struct smb2fh **out_fh,
                                uint64_t *out_size) {
    if (!out_ctx || !out_fh) return -1;
    *out_ctx = NULL;
    *out_fh = NULL;

    if (evo_smb_tcp_precheck(host, port, 4) != 0) {
        return -1;
    }

    struct smb2_context *ctx = smb2_init_context();
    if (!ctx) return -1;

    smb2_set_timeout(ctx, 5);
    smb2_set_security_mode(ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);

    const char *eff_user = (user && user[0] != '\0') ? user : "guest";
    smb2_set_user(ctx, eff_user);

    if (password && password[0] != '\0') {
        smb2_set_password(ctx, password);
    }
    if (domain && domain[0] != '\0') {
        smb2_set_domain(ctx, domain);
    }

    char server_port[160];
    if (port > 0 && port != 445) {
        snprintf(server_port, sizeof(server_port), "%s:%d", host, port);
    } else {
        snprintf(server_port, sizeof(server_port), "%s", host);
    }

    if (smb2_connect_share(ctx, server_port, share, eff_user) < 0) {
        smb2_destroy_context(ctx);
        return -1;
    }

    struct smb2fh *fh = smb2_open(ctx, path, O_RDONLY);
    if (!fh) {
        smb2_disconnect_share(ctx);
        smb2_destroy_context(ctx);
        return -1;
    }

    if (out_size) {
        uint64_t file_size = 0;
        int64_t end = smb2_lseek(ctx, fh, 0, SEEK_END, NULL);
        if (end > 0) {
            file_size = (uint64_t)end;
        } else {
            struct smb2_stat_64 st;
            if (smb2_fstat(ctx, fh, &st) == 0) {
                file_size = st.smb2_size;
            }
        }
        smb2_lseek(ctx, fh, 0, SEEK_SET, NULL);
        *out_size = file_size;
    }

    *out_ctx = ctx;
    *out_fh = fh;
    return 0;
}

static int smb_reconnect(evo_smb_io_t *smb) {
    if (!smb) return -1;

    if (smb->fh) {
        smb2_close(smb->ctx, smb->fh);
        smb->fh = NULL;
    }
    if (smb->ctx) {
        smb2_disconnect_share(smb->ctx);
        smb2_destroy_context(smb->ctx);
        smb->ctx = NULL;
    }

    struct smb2_context *new_ctx = NULL;
    struct smb2fh *new_fh = NULL;
    uint64_t file_size = 0;
    if (smb_connect_and_open(smb->host, smb->port, smb->user,
                             smb->password, smb->domain, smb->share,
                             smb->path, &new_ctx, &new_fh, &file_size) < 0) {
        return -1;
    }

    smb->ctx = new_ctx;
    smb->fh = new_fh;
    if (file_size > 0) {
        smb->size = file_size;
    }
    return 0;
}

static int smb_read_packet(void *opaque, uint8_t *buf, int buf_size) {
    evo_smb_io_t *smb = (evo_smb_io_t *)opaque;
    if (!smb || !smb->ctx || !smb->fh) return AVERROR(EINVAL);

    if (smb->size > 0 && smb->pos >= smb->size) {
        return AVERROR_EOF;
    }

    uint32_t to_read = (uint32_t)buf_size;
    if (smb->size > 0 && smb->pos + to_read > smb->size) {
        to_read = (uint32_t)(smb->size - smb->pos);
    }
    if (to_read == 0) {
        return AVERROR_EOF;
    }

    int rc = smb2_pread(smb->ctx, smb->fh, buf, to_read, smb->pos);
    if (rc < 0) {
        /* Retry once with reconnect */
        if (smb_reconnect(smb) == 0 && smb->ctx && smb->fh) {
            rc = smb2_pread(smb->ctx, smb->fh, buf, to_read, smb->pos);
        }
        if (rc < 0) {
            return AVERROR(EIO);
        }
    }
    if (rc == 0) {
        return AVERROR_EOF;
    }

    smb->pos += (uint64_t)rc;
    return rc;
}

static int64_t smb_seek_packet(void *opaque, int64_t offset, int whence) {
    evo_smb_io_t *smb = (evo_smb_io_t *)opaque;
    if (!smb) return AVERROR(EINVAL);

    if (whence == AVSEEK_SIZE) {
        return smb->size > 0 ? (int64_t)smb->size : AVERROR(ENOSYS);
    }

    whence &= ~AVSEEK_FORCE;
    int64_t target = 0;
    if (whence == SEEK_SET) {
        target = offset;
    } else if (whence == SEEK_CUR) {
        target = (int64_t)smb->pos + offset;
    } else if (whence == SEEK_END) {
        if (smb->size == 0) return AVERROR(EINVAL);
        target = (int64_t)smb->size + offset;
    } else {
        return AVERROR(EINVAL);
    }

    if (target < 0) {
        return AVERROR(EINVAL);
    }

    smb->pos = (uint64_t)target;
    return (int64_t)smb->pos;
}

#endif // HAVE_LIBSMB2

evo_smb_io_t *evo_smb_open(const char *url) {
    if (!url) return NULL;

    smb_parsed_url_t parsed;
    if (parse_smb_url(url, &parsed) < 0) {
        return NULL;
    }

#if defined(HAVE_LIBSMB2)
    struct smb2_context *ctx = NULL;
    struct smb2fh *fh = NULL;
    uint64_t file_size = 0;
    if (smb_connect_and_open(parsed.host, parsed.port, parsed.user,
                             parsed.password, parsed.domain, parsed.share,
                             parsed.path, &ctx, &fh, &file_size) < 0) {
        return NULL;
    }

    evo_smb_io_t *smb = (evo_smb_io_t *)calloc(1, sizeof(evo_smb_io_t));
    if (!smb) {
        smb2_close(ctx, fh);
        smb2_disconnect_share(ctx);
        smb2_destroy_context(ctx);
        return NULL;
    }

    smb->ctx = ctx;
    smb->fh = fh;
    smb->size = file_size;
    smb->pos = 0;
    snprintf(smb->host, sizeof(smb->host), "%s", parsed.host);
    smb->port = parsed.port;
    snprintf(smb->user, sizeof(smb->user), "%s", parsed.user);
    snprintf(smb->password, sizeof(smb->password), "%s", parsed.password);
    snprintf(smb->domain, sizeof(smb->domain), "%s", parsed.domain);
    snprintf(smb->share, sizeof(smb->share), "%s", parsed.share);
    snprintf(smb->path, sizeof(smb->path), "%s", parsed.path);

    uint8_t *iobuf = (uint8_t *)av_malloc(SMB_AVIO_BUFFER_SIZE);
    if (!iobuf) {
        evo_smb_close(smb);
        return NULL;
    }

    smb->avio = avio_alloc_context(iobuf, SMB_AVIO_BUFFER_SIZE, 0, smb,
                                   smb_read_packet, NULL, smb_seek_packet);
    if (!smb->avio) {
        av_free(iobuf);
        evo_smb_close(smb);
        return NULL;
    }

    smb->avio->seekable = AVIO_SEEKABLE_NORMAL;
    return smb;
#else
    return NULL;
#endif
}

AVIOContext *evo_smb_avio(evo_smb_io_t *smb) {
    return smb ? smb->avio : NULL;
}

void evo_smb_close(evo_smb_io_t *smb) {
    if (!smb) return;

#if defined(HAVE_LIBSMB2)
    if (smb->avio) {
        av_freep(&smb->avio->buffer);
        avio_context_free(&smb->avio);
        smb->avio = NULL;
    }

    if (smb->ctx) {
        if (smb->fh) {
            smb2_close(smb->ctx, smb->fh);
            smb->fh = NULL;
        }
        smb2_disconnect_share(smb->ctx);
        smb2_destroy_context(smb->ctx);
        smb->ctx = NULL;
    }
#endif

    free(smb);
}
