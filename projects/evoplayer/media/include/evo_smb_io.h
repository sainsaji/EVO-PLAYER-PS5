#ifndef EVO_SMB_IO_H
#define EVO_SMB_IO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(EVO_TARGET_PS5)
#include <libavformat/avio.h>
#else
typedef struct AVIOContext AVIOContext;
#endif

typedef struct evo_smb_io evo_smb_io_t;

evo_smb_io_t *evo_smb_open(const char *url);
AVIOContext  *evo_smb_avio(evo_smb_io_t *smb);
void          evo_smb_close(evo_smb_io_t *smb);

/*
 * Decodes a percent-encoded string (%XX) in-place or into dst.
 * Returns 0 on success, -1 if dst was too small. Always null-terminates dst.
 */
int evo_url_decode(const char *src, char *dst, size_t dst_len);

/*
 * Bounded non-blocking TCP pre-check to host:port (default 445).
 * Polls for writability for at most timeout_sec (default 4 s).
 * Returns 0 if reachable, -1 on timeout or failure.
 */
int evo_smb_tcp_precheck(const char *host, int port, int timeout_sec);

#ifdef __cplusplus
}

#include <string>
#include <cstring>

inline std::string evo_url_decode_string(const std::string& src) {
    if (src.empty()) return src;
    std::string dst(src.size() + 1, '\0');
    if (evo_url_decode(src.c_str(), &dst[0], dst.size()) == 0) {
        dst.resize(std::strlen(dst.c_str()));
        return dst;
    }
    return src;
}
#endif

#endif /* EVO_SMB_IO_H */
