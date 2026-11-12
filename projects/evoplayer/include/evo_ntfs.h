#ifndef EVO_NTFS_H
#define EVO_NTFS_H

/*
 * evo_ntfs - read NTFS drives the console does not mount (#217).
 *
 * The PS5 mounts exFAT and FAT32 USB drives at /mnt/usbN; an NTFS drive shows
 * up only as a raw disk (/dev/da0, /dev/da1, ...). This reads those disks
 * directly: it finds NTFS partitions (MBR, GPT or a volume on the whole disk)
 * and gives read-only access to their folders and files. Written for EVO from
 * the NTFS on-disk format; no filesystem driver, nothing is ever written.
 *
 * Paths are URLs:  ntfs://<volume>/<path>   e.g. ntfs://da0p1/Movies/a.mkv
 * <volume> is the disk plus the partition ("da0p1", "da0" for a volume on the
 * whole disk). evo_opendir/evo_readdir and the player's stream I/O take these
 * URLs, so the USB browser lists NTFS folders and plays from them like any
 * other drive.
 *
 * Supported: MFT records of any size with fixups, fragmented files (data
 * runs, attribute lists), sparse runs, resident files, directory indexes
 * (B+ tree, any depth), long Unicode names. Not supported (open fails with a
 * log line): compressed and encrypted files. Thread-safe: browsing and
 * playback may read the same volume at once.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVO_NTFS_SCHEME "ntfs://"

/* Looks for NTFS volumes on /dev/da0..da7 (and, in --usb-remote builds,
 * /data/evoplayer/test/ntfs.img). Volumes on a disk that is gone are dropped;
 * new disks are mounted. Cheap when nothing changed. Returns the count. */
int  evo_ntfs_refresh(void);

/* The same on a background thread, for the UI: probing a new drive reads it
 * (3.7 s through the helper for a 1 TB exFAT stick), which froze the frame
 * loop. Starts one refresh unless one is running and returns the count as it
 * is now; new volumes show on the caller's next look. */
int  evo_ntfs_refresh_async(void);

/* Mounts every NTFS volume found in an image file or device node under the
 * name `name` (+ "pN" for partitions). For tests and the test image. Returns
 * how many volumes were added. */
int  evo_ntfs_add_device(const char *path, const char *name);

int         evo_ntfs_volume_count(void);
/* "ntfs://da0p1" for volume i, and a label for lists ("EVOTEST", or the
 * volume name when it has no label). NULL past the end. */
const char *evo_ntfs_volume_url(int i);
const char *evo_ntfs_volume_label(int i);

/* 1 for a URL with the ntfs:// scheme. */
int evo_ntfs_is_url(const char *path);

/* Folder listing. Entries come in name order; "." and ".." are not listed,
 * nor the volume's $ system files. */
typedef struct evo_ntfs_dir evo_ntfs_dir_t;
evo_ntfs_dir_t *evo_ntfs_opendir(const char *url);
/* 1 and fills the entry, 0 at the end. `name` is UTF-8. */
int  evo_ntfs_readdir(evo_ntfs_dir_t *d, const char **name, int *is_dir, uint64_t *size);
void evo_ntfs_closedir(evo_ntfs_dir_t *d);

/* 0 and fills what exists; -1 when the path is not there. */
int evo_ntfs_stat(const char *url, int *is_dir, uint64_t *size);

/* Files. read returns the bytes read (0 at the end) or -1. */
typedef struct evo_ntfs_file evo_ntfs_file_t;
evo_ntfs_file_t *evo_ntfs_open(const char *url);
uint64_t evo_ntfs_size(const evo_ntfs_file_t *f);
long     evo_ntfs_read(evo_ntfs_file_t *f, void *buf, size_t n, uint64_t offset);
void     evo_ntfs_close(evo_ntfs_file_t *f);

/* A whole (small) file into memory, for callers that would fopen() a local
 * path: subtitles next to a video, images. 0 and a malloc'd buffer (free()),
 * or -1 when missing or larger than `max` bytes. */
int evo_ntfs_read_file(const char *url, unsigned char **out, size_t *len, size_t max);

#ifdef __cplusplus
}
#endif

#endif /* EVO_NTFS_H */
