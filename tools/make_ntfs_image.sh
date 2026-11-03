#!/bin/bash
# tools/make_ntfs_image.sh [OUT_DIR] - build NTFS test images for EVO's NTFS reader (#217).
#
# Runs a throwaway privileged Linux container (needs Docker) with ntfs-3g:
#   whole.img   NTFS straight on the "disk" (no partition table), 4 KB clusters
#   mbr.img     MBR, one NTFS partition starting at sector 63 (not 4 KB aligned)
#   gpt.img     GPT, one NTFS partition, 64 KB clusters
# Each holds: Movies/Some Movie (2019)/movie.mkv (the sample clip if present),
# a folder with 300 files (index blocks), a fragmented file (many data runs),
# a tiny file (resident data), unicode names, an empty folder and nested dirs.
# The host tests read them (tests/test_ntfs.cpp); copy one to
# /data/evoplayer/test/ntfs.img on the console and a --usb-remote build mounts it.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${1:-output/ntfs}"
mkdir -p "${OUT}"
SAMPLE="output/sample/evo_sample.mkv"
[[ -f "${SAMPLE}" ]] || { echo "make_ntfs_image: ${SAMPLE} missing - run tools/make_sample.sh first" >&2; exit 1; }
cp "${SAMPLE}" "${OUT}/sample.mkv"

MSYS_NO_PATHCONV=1 docker run --rm --privileged -v "$(pwd)/${OUT}:/w" ubuntu:24.04 bash -c '
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null && apt-get install -y -qq ntfs-3g fdisk python3 >/dev/null
fill() {
  m=$1
  mkdir -p "$m/Movies/Some Movie (2019)" "$m/Big Folder" "$m/Empty" "$m/a/b/c/d" "$m/Ünïcödé 日本"
  cp /w/sample.mkv "$m/Movies/Some Movie (2019)/movie.mkv"
  echo "hello" > "$m/tiny.txt"
  for i in $(seq 1 300); do echo "file $i" > "$m/Big Folder/file_$i.txt"; done
  echo deep > "$m/a/b/c/d/deep.txt"
  echo uni > "$m/Ünïcödé 日本/ファイル.txt"
  echo gone > "$m/frag_b.bin"; rm "$m/frag_b.bin"   # a deleted name must not list
  # Fragment frag_c.bin into one-cluster pieces: fill with one-cluster files,
  # delete every other one, pad the rest of the free space, then write it -
  # the holes are all that is left. With 4 KiB clusters that is ~2300 runs,
  # too many for one MFT record: an attribute list with extension records.
  cl=$3
  n=$(( 9 * 1024 * 1024 / cl + 64 ))
  mkdir "$m/fill"
  for i in $(seq 1 $((2 * n))); do head -c $cl /dev/zero > "$m/fill/f$i"; done
  for i in $(seq 1 2 $((2 * n))); do rm "$m/fill/f$i"; done
  sync
  avail=$(df -B1 --output=avail "$m" | tail -1)
  pad=$(( avail - n * cl - 2 * 1024 * 1024 ))
  [ $pad -gt 0 ] && head -c $pad /dev/zero > "$m/pad.bin"
  sync
  python3 -c "import sys; open(sys.argv[1],\"wb\").write(bytes(i % 251 for i in range(9*1024*1024+123)))" "$m/frag_c.bin"
}
mk() { # name size cluster partition-offset-sectors(0 = whole disk) table
  img=/w/$1.img; rm -f $img; truncate -s $2 $img
  if [ "$5" = mbr ]; then
    printf "label: dos\nstart=63, type=7\n" | sfdisk -q $img
  elif [ "$5" = gpt ]; then
    printf "label: gpt\nstart=2048, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7\n" | sfdisk -q $img
  fi
  if [ "$4" = 0 ]; then
    mkntfs -q -F -Q -c $3 -L EVOTEST $img
    ntfs-3g $img /mnt
  else
    loop=$(losetup -f --show -o $(( $4 * 512 )) $img)
    mkntfs -q -F -Q -c $3 -L EVOTEST -p $4 -H 255 -S 63 $loop
    ntfs-3g $loop /mnt
  fi
  fill /mnt $1 $3
  umount /mnt
  [ "$4" = 0 ] || losetup -d $loop
  echo made $img
}
mk whole 64M 4096 0 none
mk mbr   64M 4096 63 mbr
mk gpt   96M 65536 2048 gpt
'
ls -la "${OUT}"/*.img
