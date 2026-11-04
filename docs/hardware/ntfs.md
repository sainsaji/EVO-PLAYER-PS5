# NTFS drives (#217)

The PS5 mounts exFAT and FAT32 USB drives at `/mnt/usbN`. An NTFS drive is not
mounted; it shows up only as a raw disk, `/dev/da0`, `/dev/da1`, ... EVO reads
those disks itself, read-only, with its own NTFS reader (`src/evo_ntfs.c`).

## How it fits in

- **Paths** are URLs: `ntfs://<volume>/<path>`, e.g. `ntfs://da0p1/Movies/a.mkv`.
  `<volume>` is the disk and partition (`da0p1`), or the disk alone (`da0`) when
  the volume fills the whole disk.
- **Finding volumes:** `evo_ntfs_refresh()` opens `/dev/da0..7`, reads the MBR or
  GPT (or a boot sector on the whole disk) and mounts every NTFS partition. A
  disk without NTFS (an exFAT stick) is remembered and not read again until it
  is unplugged. `FileSystemBrowser::usbMounts()` calls it, so NTFS volumes join
  the USB drive list ("NTFS DRIVE (label)") within the browser's 3 s rescan.
- **Browsing:** `evo_opendir`/`evo_readdir` hand `ntfs://` paths to `evo_ntfs`,
  with `d_type` set, so the browser lists NTFS folders unchanged.
- **Playing:** `evo_stream_io_open` gives FFmpeg an `AVIOContext` over
  `evo_ntfs_read` (1 MiB reads), the same pattern as `smb://`.
- **Read-only:** the Options menu on an NTFS drive says so; nothing is written.
- **Small files:** `evo_ntfs_read_file` reads a whole file into memory, which is
  how a sidecar `.srt` (`subtitle_text_open`) and a picture (`ImageViewerScreen`)
  open from an NTFS drive.
- **Not supported:** compressed and encrypted files (open fails with a log line),
  cover art from the drive.

## Reaching the disk: the helper payload

EVO's own `/dev` has no disk nodes, even after the jailbreak (measured
2026-11-03: EVO sees `usbctl usb ugen*`; a root payload sees `da1 da1s1`). So
EVO carries a small payload, `helpers/ntfs_helper.c`, built by
`tools/gen_ntfs_helper.sh` and embedded as `src/generated/evo_ntfs_helper_elf.c`:

- When the set of USB devices EVO can see (`/dev/ugen*`) changes, and once per
  launch, EVO asks the helper on `127.0.0.1:9786` for the disks (`LIST`). If it
  is not running, EVO sends it to the ELF loader on `127.0.0.1:9021` (at most 3
  tries per change) and asks again on the next refresh.
- The helper runs as root, lists `/dev/daN`, opens a disk read-only on `OPEN daN`
  and answers 4 KiB-aligned block reads. It listens on loopback only, refuses any
  name but `daN`/`daNsM`/`daNpM`, and exits 30 s after its last client.
- Each NTFS volume keeps one helper connection; `evo_ntfs` reads through it the
  same way it reads an image file.
- No ELF loader on 9021: a log line, and NTFS drives are not offered.

## The reader

Written for EVO from the NTFS on-disk layout. PS5-NTFS-BROWSER
(GPL-3.0) showed the approach - a payload opening `/dev/daN` and parsing NTFS in
user space - and no code was taken from it.

- Boot sector geometry, MFT records of any size with update-sequence fixups.
- Data runs (fragmented and sparse files), attribute lists (a file in thousands
  of pieces), resident files, initialized size.
- `$I30` directory indexes walked in order (B+ tree, any depth); DOS 8.3
  aliases and the root's `$` system files hidden; names case-insensitive.
- Device reads: `lseek` + `read` through a 4 KiB-aligned bounce buffer per volume
  (a raw disk takes sector-aligned reads only; 4 KiB suits 512 and 4K sectors).
  No `pread`/`ioctl`: they are not in EVO's import list.

## Testing without a drive

`tools/make_ntfs_image.sh` builds three images with ntfs-3g in a throwaway Docker
container (`output/ntfs/`): a volume on the whole disk, an MBR partition at
sector 63 and a GPT partition with 64 KiB clusters. Each holds a 9 MiB file
split into one-cluster pieces (an attribute list), a 300-file folder, Unicode
names and the sample clip. `tests/test_ntfs.cpp` reads them byte for byte.

On the console, a `--usb-remote` build also mounts
`/data/evoplayer/test/ntfs.img` as volume `img0`.
