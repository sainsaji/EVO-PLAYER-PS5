#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "${REPO_ROOT}/output/uiview"

if [[ ! -f /.dockerenv ]]; then
    # Forward the render-size override so 4K reproduction works from the host.
    docker compose run --rm -e EVO_UIVIEW_SIZE="${EVO_UIVIEW_SIZE:-}" ps5-dev bash ./tools/uiview_playback_rml.sh
    exit $?
fi

cd "${REPO_ROOT}"

# #60: regenerate the embedded RmlUi asset bundle before compiling, so the
# host preview always renders whatever assets/{rml,fonts,icons} currently
# contains - see tools/bundle_rml_assets.py.
python3 tools/bundle_rml_assets.py

# #68: ui_rml/src/rmlui_patch/GeometryBackgroundBorder.cpp is RmlUi's corner
# tessellation with a finer GetNumPoints(); compiled into the binary it preempts
# librmlui.so's copies of that TU. See rmlui_patch/VENDORED.md.
#
# The harness renders through EVO's CPU rasteriser. It used to also link
# RmlUi's GL3 interface for a plane compare against the console's OpenGL path;
# that path is gone (the console is bare-metal AGC only), so the compare had
# nothing left to compare against and went with it.
# -----------------------------------------------------------------------------
# #90: the provider seam's C sources.
#
# Compiled as C with gcc and linked in as objects rather than handed to the g++
# command below: they use C11 _Thread_local (evo_data_path.c,
# evo_provider_bundle.c) and designated initialisers in the vtables, neither of
# which is C++17. One list, so adding a provider is one line here.
#
# Two host substitutions, both deliberate and documented where they are made:
#   NO_OPENSSL=1                 the dev image has no host OpenSSL. https is
#                                out; the fixture's local server speaks http.
#   EVO_PROVIDER_ART_NO_DECODE=1 no host FFmpeg development libraries, so
#                                posters render as the no-artwork branch. Every
#                                other part of the art path still runs.
# -----------------------------------------------------------------------------
echo "--- building provider seam (C)"
mkdir -p output/uiview/obj
PROVIDER_OBJS=()
for c in projects/evoplayer/addons/src/cJSON.c \
         projects/evoplayer/addons/src/evo_net.c \
         projects/evoplayer/addons/src/addon_emby.c \
         projects/evoplayer/addons/src/evo_provider_mgr.c \
         projects/evoplayer/addons/src/evo_provider_bundle.c \
         projects/evoplayer/addons/src/provider_iptv.c \
         projects/evoplayer/addons/src/provider_emby.c \
         projects/evoplayer/addons/src/provider_jellyfin.c \
         projects/evoplayer/addons/src/provider_nuvio.c \
         projects/evoplayer/src/evo_data_path.c; do
    o="output/uiview/obj/$(basename "${c%.c}").o"
    gcc -O2 -std=c11 -Wall -DNO_OPENSSL=1 \
        -Iprojects/evoplayer/include \
        -Iprojects/evoplayer/addons/include \
        -c "$c" -o "$o"
    PROVIDER_OBJS+=("$o")
done

echo "--- building uiview_playback_rml"
g++ -O2 -std=c++17 \
    -Iprojects/evoplayer \
    -Iprojects/evoplayer/ui_rml/include \
    -Iprojects/evoplayer/ui/include \
    -Iprojects/evoplayer/ui_rml/src \
    -Iprojects/evoplayer/pp/include \
    -Iprojects/evoplayer/include \
    -Ibuild/rmlui-host/RmlUi/Include \
    -Ibuild/rmlui-host-dist/include/freetype2 \
    -Iprojects/evoplayer/addons/include \
    -Iprojects/evoplayer/core/include \
    -DNO_OPENSSL=1 -DEVO_PROVIDER_ART_NO_DECODE=1 \
    -o output/uiview/uiview_playback_rml \
    tools/uiview_playback_rml.cpp \
    projects/evoplayer/ui_rml/src/rmlui_patch/GeometryBackgroundBorder.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_render.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_system.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_app.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_bridge.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_fileinterface.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_bundle.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_bundle_data.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_provider.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_provider_art.cpp \
    "${PROVIDER_OBJS[@]}" \
    -Lbuild/rmlui-host-dist/lib \
    -Lbuild/rmlui-host/RmlUi/build \
    -lrmlui -lfreetype -lpng16 -lz -lpthread \
    -Wl,-rpath,/workspace/build/rmlui-host-dist/lib:/workspace/build/rmlui-host/RmlUi/build

export LD_LIBRARY_PATH="/workspace/build/rmlui-host-dist/lib:/workspace/build/rmlui-host/RmlUi/build:${LD_LIBRARY_PATH:-}"

# -----------------------------------------------------------------------------
# #90: stage a provider so its screen renders for real.
#
# The fixture drives the actual IPTV provider, the actual evo_net HTTP client
# and the actual bundle fetch, so it needs a server and a data root. Both are
# local to this run: a python http.server on the loopback interface - which the
# container can always reach, unlike the console (docs/hardware/networking.md)
# - and a temp directory that EVO_DATA_DIR_OVERRIDE points the data root at.
#
# Anything failing here is non-fatal: the provider shots are skipped and every
# other screen still renders. A broken provider fixture must not cost the whole
# preview.
# -----------------------------------------------------------------------------
PROVIDER_PORT="${EVO_UIVIEW_PROVIDER_PORT:-18099}"
PROVIDER_SERVE="$(mktemp -d)"
PROVIDER_DATA="$(mktemp -d)"
export EVO_DATA_DIR_OVERRIDE="${PROVIDER_DATA}/evoplayer"
mkdir -p "${EVO_DATA_DIR_OVERRIDE}" "${PROVIDER_SERVE}/ui" "${PROVIDER_SERVE}/media"

PROVIDER_PID=""
cleanup_provider() {
    if [[ -n "${PROVIDER_PID}" ]]; then kill "${PROVIDER_PID}" 2>/dev/null || true; fi
    rm -rf "${PROVIDER_SERVE}" "${PROVIDER_DATA}" 2>/dev/null || true
}
trap cleanup_provider EXIT

if [[ -d assets/providers/iptv ]]; then
    python3 tools/gen_provider_manifest.py assets/providers/iptv >/dev/null 2>&1 || true
    cp -r assets/providers/iptv "${PROVIDER_SERVE}/ui/iptv"

    # A fixed playlist, not the one tools/provider-server.sh generates: these
    # shots get diffed against baselines by shot.sh, so the rows have to be
    # identical between runs. The group titles vary on purpose - the
    # group-folder path is what a flat list would not exercise - and one name
    # carries a comma, which is the M3U parse bug every quick parser has.
    cat > "${PROVIDER_SERVE}/media/iptv.m3u" <<'M3UEOF'
#EXTM3U
#EXTINF:-1 tvg-id="n1" tvg-name="News One" group-title="News",News One HD
http://127.0.0.1:1/n1.m3u8
#EXTINF:-1 tvg-id="n2" tvg-name="World Report" group-title="News",World Report
http://127.0.0.1:1/n2.m3u8
#EXTINF:-1 tvg-id="s1" tvg-name="Sports Prime" group-title="Sports",Sports Prime
http://127.0.0.1:1/s1.m3u8
#EXTINF:-1 tvg-id="s2" tvg-name="Match Centre" group-title="Sports",Match Centre
http://127.0.0.1:1/s2.m3u8
#EXTINF:-1 tvg-id="s3" tvg-name="Extra Time" group-title="Sports",Extra Time, Highlights
http://127.0.0.1:1/s3.m3u8
#EXTINF:-1 tvg-id="c1" tvg-name="Cinema Classics" group-title="Movies",Cinema Classics
http://127.0.0.1:1/c1.m3u8
#EXTINF:-1 tvg-id="c2" tvg-name="Action Now" group-title="Movies",Action Now
http://127.0.0.1:1/c2.m3u8
#EXTINF:-1 tvg-id="k1" tvg-name="Kids Zone" group-title="Kids",Kids Zone
http://127.0.0.1:1/k1.m3u8
M3UEOF

    # A second, deliberately LARGE playlist: one channel in each of 120 groups,
    # so the root level is 120 folder rows.
    #
    # It exists to exercise the row window past EVO_PROVIDER_ROW_WINDOW_MAX,
    # where the window stops growing and starts SLIDING. That path moves focus
    # itself to compensate for the slide, and nothing in the fixed playlist
    # above comes anywhere near it - four groups never leave the first window,
    # so the whole slide path would otherwise be first executed on hardware.
    # Generated rather than spelled out only because 120 stanzas is noise.
    #
    # Every row also carries a tvg-logo pointing back at this server, so the
    # fixture exercises the ARTWORK path: request -> fetch -> sniff the magic
    # bytes -> mkdir -p -> write the cache file. The host cannot decode it
    # (EVO_PROVIDER_ART_NO_DECODE), so the run ends at "art DECODE fail" - but
    # that line prints the full cache path, which is the thing worth asserting.
    # A build that gets the path wrong says `path=.png` there instead, which is
    # exactly the bug that shipped to hardware twice because nothing off-device
    # ever asked a provider for a poster.
    cp -f "${REPO_ROOT}/projects/evoplayer/assets/icons/icon_folder.png" \
          "${PROVIDER_SERVE}/media/logo.png"
    {
        echo "#EXTM3U"
        i=1
        while [ "${i}" -le 120 ]; do
            printf '#EXTINF:-1 tvg-id="g%03d" tvg-name="Group %03d" tvg-logo="http://127.0.0.1:%s/media/logo.png" group-title="Group %03d",Channel %03d\n' \
                   "${i}" "${i}" "${PROVIDER_PORT}" "${i}" "${i}"
            printf 'http://127.0.0.1:1/g%03d.m3u8\n' "${i}"
            i=$((i + 1))
        done
    } > "${PROVIDER_SERVE}/media/iptv_big.m3u"

    # XMLTV to go with it. Without an EPG the bundle's NOW/NEXT rows never
    # render, so the most distinctive part of a channel card was the one part
    # the preview could not show.
    #
    # The programme windows are relative to now, which keeps the shots
    # diffable: the times move every run but the titles do not, and the titles
    # are all that reaches a pixel. Only two channels are covered, on purpose -
    # a grid where every card has an EPG would not show what a card without one
    # falls back to.
    python3 - "${PROVIDER_SERVE}/media/epg.xml" <<'EPGEOF'
import sys, time
out = sys.argv[1]
now = time.time()
def stamp(t):
    return time.strftime("%Y%m%d%H%M%S +0000", time.gmtime(t))
chans = [
    ("n1", "News One",     "World News At One", "Markets Tonight"),
    ("n2", "World Report", "The Long Read",     "Correspondents"),
]
lines = ['<?xml version="1.0" encoding="UTF-8"?>', '<tv>']
for cid, name, _, _ in chans:
    lines.append(f'  <channel id="{cid}"><display-name>{name}</display-name></channel>')
for cid, _, now_t, next_t in chans:
    lines.append(f'  <programme start="{stamp(now - 900)}" stop="{stamp(now + 1800)}" channel="{cid}">')
    lines.append(f'    <title>{now_t}</title>')
    lines.append('  </programme>')
    lines.append(f'  <programme start="{stamp(now + 1800)}" stop="{stamp(now + 5400)}" channel="{cid}">')
    lines.append(f'    <title>{next_t}</title>')
    lines.append('  </programme>')
lines.append('</tv>')
open(out, "w", encoding="utf-8").write(chr(10).join(lines) + chr(10))
EPGEOF

    ( cd "${PROVIDER_SERVE}" && exec python3 -m http.server "${PROVIDER_PORT}" --bind 127.0.0.1 >/dev/null 2>&1 ) &
    PROVIDER_PID=$!

    cat > "${EVO_DATA_DIR_OVERRIDE}/iptv.conf" <<CONFEOF
playlist=http://127.0.0.1:${PROVIDER_PORT}/media/iptv.m3u
xmltv=http://127.0.0.1:${PROVIDER_PORT}/media/epg.xml
bundle=http://127.0.0.1:${PROVIDER_PORT}/ui/iptv
CONFEOF

    # Wait for the listener rather than sleeping a guessed interval: the
    # fixture's first request is the manifest, and losing that race produces a
    # fallback-skin shot where the bundle was supposed to be.
    for _ in $(seq 1 50); do
        if python3 -c "import socket,sys; s=socket.socket(); s.settimeout(0.2); sys.exit(0 if s.connect_ex(('127.0.0.1',${PROVIDER_PORT}))==0 else 1)"; then
            break
        fi
        sleep 0.1
    done
    echo "--- provider fixture: http://127.0.0.1:${PROVIDER_PORT} -> ${EVO_DATA_DIR_OVERRIDE}"
else
    echo "--- provider fixture: assets/providers/iptv missing, skipping"
fi

shopt -s nullglob
rm -f output/uiview/rml_*.bmp   # stale from a prior run

echo "--- rendering all launch + settings screenshots (CPU rasteriser)"
./output/uiview/uiview_playback_rml

for b in output/uiview/rml_*.bmp; do
    s="$(basename "${b%.bmp}")"
    python3 tools/shot.py png "$b" "output/uiview/$s.png"
    if [[ "${KEEP_BMP:-0}" != "1" ]]; then rm -f "$b"; fi
    echo "  ok -> output/uiview/$s.png"
done

