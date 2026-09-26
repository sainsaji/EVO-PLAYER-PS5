#!/usr/bin/env python3
"""
gen_addon.py - a static Stremio addon over a local media folder, for testing
the Nuvio provider (and any Stremio-addon client) without a debrid account.

Writes manifest.json plus catalog / meta / stream JSON under OUT, in the
shape a Stremio client fetches:

    /manifest.json
    /catalog/movie/evotest.json
    /meta/movie/evotest-<n>.json
    /stream/movie/evotest-<n>.json

Every video file in MEDIA_DIR becomes a movie whose one stream is
<BASE>/media/<file>; serve MEDIA_DIR at /media next to the JSON (serve.sh does,
with nginx, which answers the range requests a player needs to seek). One
public HLS test stream is added so the HLS path gets exercised too.

usage: gen_addon.py <media_dir> <out_dir> <base_url>
"""
import json
import os
import sys
import urllib.parse

VIDEO_EXT = {".mp4", ".mkv", ".m4v", ".mov", ".avi", ".ts", ".m2ts", ".webm", ".wmv"}

EXTRA = [
    ("Big Buck Bunny (HLS)", "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"),
]


def write(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obj, f, ensure_ascii=False, indent=1)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    media_dir, out_dir, base = sys.argv[1], sys.argv[2], sys.argv[3].rstrip("/")

    items = []
    for name in sorted(os.listdir(media_dir), key=str.lower):
        stem, ext = os.path.splitext(name)
        if ext.lower() in VIDEO_EXT and os.path.isfile(os.path.join(media_dir, name)):
            items.append((stem, base + "/media/" + urllib.parse.quote(name), ext.lstrip(".").upper()))
    for title, url in EXTRA:
        items.append((title, url, "HLS"))

    poster = base + "/poster.png"
    manifest = {
        "id": "org.evoplayer.testaddon",
        "version": "1.0.0",
        "name": "EVO Test Library",
        "description": "Local test media for EVO Player",
        "resources": ["catalog", "meta", "stream"],
        "types": ["movie"],
        "idPrefixes": ["evotest-"],
        "catalogs": [{"type": "movie", "id": "evotest", "name": "EVO Test Library"}],
    }
    write(os.path.join(out_dir, "manifest.json"), manifest)

    metas = []
    for i, (title, url, kind) in enumerate(items):
        vid = "evotest-%d" % i
        meta = {
            "id": vid,
            "type": "movie",
            "name": title,
            "poster": poster,
            "background": poster,
            "description": "%s test file" % kind,
            "genres": ["Test"],
        }
        metas.append({k: meta[k] for k in ("id", "type", "name", "poster")})
        write(os.path.join(out_dir, "meta", "movie", vid + ".json"), {"meta": meta})
        write(os.path.join(out_dir, "stream", "movie", vid + ".json"),
              {"streams": [{"name": "EVO Test", "title": "%s\n%s" % (title, kind), "url": url}]})
    write(os.path.join(out_dir, "catalog", "movie", "evotest.json"), {"metas": metas})
    print("%d items -> %s" % (len(items), out_dir))


if __name__ == "__main__":
    main()
