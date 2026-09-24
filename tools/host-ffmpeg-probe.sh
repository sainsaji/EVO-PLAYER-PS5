set -e
# The minimal profile's COMPONENT flags only. Comments are stripped first: the
# script's own NOTE about there being no "--enable-image2" option would
# otherwise be scraped in as a flag. No cross-compile flags, no libdav1d (AV1
# is not involved and dav1d is not on the host), plus programs for an ffprobe.
FLAGS=$(sed -n '/^minimal)/,/^    ;;/p' /workspace/scripts/build-ffmpeg.sh |
        sed 's/#.*$//' |
        grep -oE -- "--(enable|disable)-[a-z0-9_=.-]+" |
        grep -v -e '--disable-programs' -e 'libdav1d' -e '--disable-debug')
echo "$FLAGS" | wc -l
mkdir -p /tmp/ffb && cd /tmp/ffb
/workspace/third_party/ffmpeg/ffmpeg-7.0.1/configure \
    $FLAGS --disable-doc \
    --disable-optimizations --enable-debug=3 > /tmp/ffb/conf.log 2>&1 \
    || { tail -20 /tmp/ffb/conf.log; exit 1; }
make -j"$(nproc)" ffprobe > /tmp/ffb/make.log 2>&1 || { tail -20 /tmp/ffb/make.log; exit 1; }
cp /tmp/ffb/ffprobe /workspace/output/ffprobe-minimal
ls -l /workspace/output/ffprobe-minimal
echo BUILD_OK
