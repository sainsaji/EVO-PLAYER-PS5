#!/usr/bin/env bash
# Compatibility shim. The screenshot workflow now lives in tools/shot.sh,
# which also does measurement (probe / scan / crop / diff) rather than only
# fetching. Kept because this path appears in docs and in shell history.
#
#   tools/fetch_shot.sh              -> tools/shot.sh grab
#   tools/fetch_shot.sh evo_shot_003.bmp -> tools/shot.sh grab evo_shot_003.bmp
exec "$(dirname "${BASH_SOURCE[0]}")/shot.sh" grab "$@"
