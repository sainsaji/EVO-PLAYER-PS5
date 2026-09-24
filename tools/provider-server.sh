#!/usr/bin/env bash
#
# provider-server.sh — serve provider UI bundles and a test playlist to the
# console over HTTP (#90).
#
# WHY THIS RUNS ON THE WINDOWS HOST AND NOT IN THE CONTAINER
#
# Per docs/hardware/networking.md, the PS5 cannot reach into the Docker
# container on Windows bridge networking. Every other script in this repo
# re-execs itself through `docker compose`; this one must NOT, because the
# thing that has to be reachable is the listening socket. It therefore runs
# directly on the host and prints the LAN URL to configure on the console.
#
# WHAT IT SERVES
#
#   /ui/<provider>/manifest.json   the bundle manifest, regenerated on start
#   /ui/<provider>/...             the bundle's .rml / .rcss / fonts / images
#   /media/iptv.m3u                a small test playlist (generated if absent)
#   /media/epg.xml                 XMLTV now-and-next for that playlist
#
# USAGE
#
#   ./tools/provider-server.sh [--port 8099] [--host <ip>]
#
# Then, on the console, point the IPTV provider at it. The provider reads
# /data/evoplayer/iptv.conf; the fastest way to write it is over FTP:
#
#   playlist=http://<host>:<port>/media/iptv.m3u
#   xmltv=http://<host>:<port>/media/epg.xml
#   bundle=http://<host>:<port>/ui/iptv
#
# The script prints that file ready to paste.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT=8099
BIND_HOST=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port) shift; PORT="${1:?--port needs a value}" ;;
        --host) shift; BIND_HOST="${1:?--host needs a value}" ;;
        -h|--help) sed -n '2,32p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
    shift
done

if [[ -f /.dockerenv ]]; then
    cat >&2 <<'EOF'
error: provider-server.sh must run on the HOST, not in the dev container.

The PS5 cannot reach a listener inside the container on Windows bridge
networking (docs/hardware/networking.md), so a server started here would be
invisible to the console even though curl from inside the container works.

Run it from PowerShell or Git Bash on the host instead.
EOF
    exit 1
fi

command -v python3 >/dev/null 2>&1 || { echo "error: python3 not found on PATH" >&2; exit 1; }

SERVE_ROOT="${REPO_ROOT}/output/provider-serve"
rm -rf "${SERVE_ROOT}"
mkdir -p "${SERVE_ROOT}/ui" "${SERVE_ROOT}/media"

# ---------------------------------------------------------------------------
# Bundles: regenerate each manifest, then stage the directory.
#
# Regenerating on every start is the point - the manifest carries a sha256 per
# file and a version string EVO compares against its cache, so serving a stale
# manifest means an edited .rcss is either rejected for a hash mismatch or
# silently not picked up. Both look like the edit not working.
# ---------------------------------------------------------------------------
shopt -s nullglob
found_bundle=0
for dir in "${REPO_ROOT}"/assets/providers/*/; do
    id="$(basename "${dir}")"
    echo "--- bundle: ${id}"
    python3 "${REPO_ROOT}/tools/gen_provider_manifest.py" "${dir}"
    cp -r "${dir}" "${SERVE_ROOT}/ui/${id}"
    found_bundle=1
done
if [[ "${found_bundle}" != "1" ]]; then
    echo "warning: no bundles under assets/providers/" >&2
fi

# ---------------------------------------------------------------------------
# Test playlist.
#
# A handful of public test streams, grouped so the group-title folder path gets
# exercised, plus one deliberately dead entry: "a channel that does not resolve
# must produce a clean error, not a hang" is on #90's hardware checklist and
# needs something to fail against.
#
# An existing tools/testdata/iptv.m3u wins, so a real playlist can be dropped
# in without editing this script.
# ---------------------------------------------------------------------------
if [[ -f "${REPO_ROOT}/tools/testdata/iptv.m3u" ]]; then
    cp "${REPO_ROOT}/tools/testdata/iptv.m3u" "${SERVE_ROOT}/media/iptv.m3u"
    echo "--- playlist: tools/testdata/iptv.m3u"
else
    cat > "${SERVE_ROOT}/media/iptv.m3u" <<'EOF'
#EXTM3U
#EXTINF:-1 tvg-id="bbb.hls" tvg-name="Big Buck Bunny" group-title="Demo",Big Buck Bunny (HLS)
https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8
#EXTINF:-1 tvg-id="sintel.hls" tvg-name="Sintel" group-title="Demo",Sintel (HLS)
https://bitdash-a.akamaihd.net/content/sintel/hls/playlist.m3u8
#EXTINF:-1 tvg-id="tears.hls" tvg-name="Tears of Steel" group-title="Demo",Tears of Steel (HLS)
https://test-streams.mux.dev/pts_shift/master.m3u8
#EXTINF:-1 tvg-id="apple.bip" tvg-name="Apple BipBop" group-title="Reference",Apple BipBop 16x9
https://devstreaming-cdn.apple.com/videos/streaming/examples/img_bipbop_adv_example_fmp4/master.m3u8
#EXTINF:-1 tvg-id="unifi.ts" tvg-name="MPEG-TS sample" group-title="Reference",MPEG-TS over HTTP
http://commondatastorage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4
#EXTINF:-1 tvg-id="dead.one" tvg-name="Dead Channel" group-title="Diagnostics",Dead Channel (expected to fail)
http://127.0.0.1:1/never/resolves.m3u8
EOF
    echo "--- playlist: generated (6 channels, 3 groups, 1 deliberately dead)"
fi

if [[ -f "${REPO_ROOT}/tools/testdata/epg.xml" ]]; then
    cp "${REPO_ROOT}/tools/testdata/epg.xml" "${SERVE_ROOT}/media/epg.xml"
    echo "--- epg: tools/testdata/epg.xml"
else
    # XMLTV now-and-next for the generated playlist. Programme windows are relative
    # to now, so "NOW" and "NEXT" are always populated whenever the server starts.
    python3 - "${SERVE_ROOT}/media/epg.xml" <<'PYEOF'
import sys, time
out = sys.argv[1]
now = time.time()
def stamp(t):
    return time.strftime("%Y%m%d%H%M%S +0000", time.gmtime(t))
chans = [
    ("bbb.hls",    "Big Buck Bunny",  "Rabbit of Unusual Size", "Three Rodents"),
    ("sintel.hls", "Sintel",          "The Dragon Hunt",        "Epilogue"),
    ("tears.hls",  "Tears of Steel",  "Amsterdam 2075",         "Behind the Scenes"),
    ("apple.bip",  "Apple BipBop",    "Reference Loop",         "Reference Loop II"),
    ("unifi.ts",   "MPEG-TS sample",  "Transport Stream Test",  "Second Half"),
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
open(out, "w", encoding="utf-8").write("\n".join(lines) + "\n")
print("--- epg: generated (now + next for 5 channels)")
PYEOF
fi

# ---------------------------------------------------------------------------
# Work out the LAN address to print. The console needs a routable address, and
# the default 0.0.0.0 bind tells it nothing about which one.
# ---------------------------------------------------------------------------
if [[ -z "${BIND_HOST}" ]]; then
    BIND_HOST="$(python3 - <<'PYEOF'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    # Never actually sends anything; it just makes the OS pick the interface
    # it would route out of, which is the one the console can reach.
    s.connect(("8.8.8.8", 80))
    print(s.getsockname()[0])
except OSError:
    print("127.0.0.1")
finally:
    s.close()
PYEOF
)"
fi

BASE="http://${BIND_HOST}:${PORT}"
cat <<EOF

=========================================================================
 provider bundle server: ${BASE}
=========================================================================

 Write this to /data/evoplayer/iptv.conf on the console (over FTP), then
 open the provider screen:

   playlist=${BASE}/media/iptv.m3u
   xmltv=${BASE}/media/epg.xml
   bundle=${BASE}/ui/iptv

 Check reachability from another machine first - a Windows Firewall prompt
 on the first run is normal and must be allowed for Private networks:

   curl -s ${BASE}/ui/iptv/manifest.json | head -5

 Ctrl-C to stop.

EOF

cd "${SERVE_ROOT}"
exec python3 -m http.server "${PORT}" --bind 0.0.0.0
