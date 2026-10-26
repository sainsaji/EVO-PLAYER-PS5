#include "evo_smb_io.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <fcntl.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>

#if defined(EVO_TARGET_PS5)
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
};

static void url_decode(const char *src, char *dst, size_t dst_len) {
    size_t i = 0, j = 0;
    while (src[i] && j + 1 < dst_len) {
        if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) && isxdigit((unsigned char)src[i + 2])) {
            char hex[3] = { src[i + 1], src[i + 2], '\0' };
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

typedef struct {
    char user[64];
    char password[64];
    char domain[64];
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

    char auth_host[256];
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
        url_decode(enc_share, out->share, sizeof(out->share));

        const char *subpath = second_slash + 1;
        while (*subpath == '/') subpath++;
        url_decode(subpath, out->path, sizeof(out->path));
    } else {
        url_decode(path_start, out->share, sizeof(out->share));
        out->path[0] = '\0';
    }

    if (out->share[0] == '\0') return -1;

    // Parse auth_host: [domain;][user[:pass]@]host[:port]
    const char *at = strrchr(auth_host, '@');
    const char *host_part = auth_host;
    if (at) {
        char auth_str[128];
        size_t auth_len = (size_t)(at - auth_host);
        if (auth_len < sizeof(auth_str)) {
            memcpy(auth_str, auth_host, auth_len);
            auth_str[auth_len] = '\0';

            const char *user_start = auth_str;
            const char *semi = strchr(auth_str, ';');
            if (semi) {
                size_t dom_len = (size_t)(semi - auth_str);
                char enc_dom[64];
                if (dom_len < sizeof(enc_dom)) {
                    memcpy(enc_dom, auth_str, dom_len);
                    enc_dom[dom_len] = '\0';
                    url_decode(enc_dom, out->domain, sizeof(out->domain));
                }
                user_start = semi + 1;
            }

            const char *colon = strchr(user_start, ':');
            if (colon) {
                char enc_user[64];
                size_t u_len = (size_t)(colon - user_start);
                if (u_len < sizeof(enc_user)) {
                    memcpy(enc_user, user_start, u_len);
                    enc_user[u_len] = '\0';
                    url_decode(enc_user, out->user, sizeof(out->user));
                }
                url_decode(colon + 1, out->password, sizeof(out->password));
            } else {
                url_decode(user_start, out->user, sizeof(out->user));
            }
        }
        host_part = at + 1;
    }

    const char *colon_port = strrchr(host_part, ':');
    if (colon_port) {
        size_t h_len = (size_t)(colon_port - host_part);
        if (h_len < sizeof(out->host)) {
            memcpy(out->host, host_part, h_len);
            out->host[h_len] = '\0';
        }
        out->port = atoi(colon_port + 1);
        if (out->port <= 0 || out->port > 65535) out->port = 445;
    } else {
        strncpy(out->host, host_part, sizeof(out->host) - 1);
    }

    return (out->host[0] != '\0') ? 0 : -1;
}

#if defined(HAVE_LIBSMB2)

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
        return AVERROR(EIO);
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
    struct smb2_context *ctx = smb2_init_context();
    if (!ctx) return NULL;

    smb2_set_timeout(ctx, 15);
    smb2_set_security_mode(ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);

    const char *user = (parsed.user[0] != '\0') ? parsed.user : "guest";
    smb2_set_user(ctx, user);

    if (parsed.password[0] != '\0') {
        smb2_set_password(ctx, parsed.password);
    }
    if (parsed.domain[0] != '\0') {
        smb2_set_domain(ctx, parsed.domain);
    }

    char server_port[160];
    if (parsed.port > 0 && parsed.port != 445) {
        snprintf(server_port, sizeof(server_port), "%s:%d", parsed.host, parsed.port);
    } else {
        snprintf(server_port, sizeof(server_port), "%s", parsed.host);
    }

    if (smb2_connect_share(ctx, server_port, parsed.share, user) < 0) {
        smb2_destroy_context(ctx);
        return NULL;
    }

    struct smb2fh *fh = smb2_open(ctx, parsed.path, O_RDONLY);
    if (!fh) {
        smb2_disconnect_share(ctx);
        smb2_destroy_context(ctx);
        return NULL;
    }

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

    if (smb->avio) {
        av_freep(&smb->avio->buffer);
        avio_context_free(&smb->avio);
        smb->avio = NULL;
    }

#if defined(HAVE_LIBSMB2)
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
