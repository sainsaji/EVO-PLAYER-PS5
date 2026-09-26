#!/usr/bin/env bash
#
# serve.sh - generate the test Stremio addon and serve it with nginx.
#
#   ./tools/test-addon/serve.sh [media_dir] [port]
#
# Defaults: media_dir "D:\Downloads\Audio Test Files", port 8100. Runs on the
# HOST (the console must reach it; see docs/hardware/networking.md) as the
# Docker container `evo-test-addon`, replaced on every run. Then, in Nuvio (or
# any Stremio client), add the addon:
#
#   http://<host>:<port>/manifest.json
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MEDIA_DIR="${1:-D:\\Downloads\\Audio Test Files}"
PORT="${2:-8100}"
OUT="${REPO_ROOT}/output/test-addon"

HOST_IP="$(python3 - <<'PYEOF'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    s.connect(("8.8.8.8", 80))
    print(s.getsockname()[0])
except OSError:
    print("127.0.0.1")
finally:
    s.close()
PYEOF
)"
BASE="http://${HOST_IP}:${PORT}"

rm -rf "${OUT}"
mkdir -p "${OUT}"
python3 "${REPO_ROOT}/tools/test-addon/gen_addon.py" "$(cygpath -u "${MEDIA_DIR}" 2>/dev/null || echo "${MEDIA_DIR}")" "${OUT}" "${BASE}"
cp "${REPO_ROOT}/projects/evoplayer/sce_sys/pic0.png" "${OUT}/poster.png"

docker rm -f evo-test-addon >/dev/null 2>&1 || true
MSYS_NO_PATHCONV=1 docker run -d --name evo-test-addon -p "${PORT}:80" \
    -v "$(cygpath -w "${OUT}" 2>/dev/null || echo "${OUT}"):/usr/share/nginx/html:ro" \
    -v "${MEDIA_DIR}:/media:ro" \
    -v "$(cygpath -w "${REPO_ROOT}/tools/test-addon/nginx.conf" 2>/dev/null || echo "${REPO_ROOT}/tools/test-addon/nginx.conf"):/etc/nginx/conf.d/default.conf:ro" \
    nginx:alpine >/dev/null

echo
echo " test addon: ${BASE}/manifest.json"
echo
