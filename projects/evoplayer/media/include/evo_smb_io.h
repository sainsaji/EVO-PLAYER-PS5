#ifndef EVO_SMB_IO_H
#define EVO_SMB_IO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <libavformat/avio.h>

typedef struct evo_smb_io evo_smb_io_t;

evo_smb_io_t *evo_smb_open(const char *url);
AVIOContext  *evo_smb_avio(evo_smb_io_t *smb);
void          evo_smb_close(evo_smb_io_t *smb);

#ifdef __cplusplus
}
#endif

#endif /* EVO_SMB_IO_H */
