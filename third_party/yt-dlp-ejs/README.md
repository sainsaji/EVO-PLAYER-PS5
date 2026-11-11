# yt-dlp-ejs (vendored)

yt-dlp's JavaScript challenge solver, version **0.8.0** (the `yt-dlp-ejs` PyPI
package that yt-dlp 2026.10.26 pins; source https://github.com/yt-dlp/ejs).
`lib.min.js` and `core.min.js` are copied unchanged from
`yt_dlp_ejs/yt/solver/`. Licence: Unlicense; `lib.min.js` bundles meriyah (ISC)
and astring (MIT), named in its header.

EVO runs them in QuickJS-NG to solve the `n` challenge of a YouTube HLS link
(`addons/src/evo_yt_link.c`). To update: copy the two files from a newer
release, run `python tools/gen_ytjs_blob.py`, rebuild.
