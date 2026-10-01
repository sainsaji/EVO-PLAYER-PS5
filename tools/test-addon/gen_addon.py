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

With a TV folder (Show/Season NN/<file with SxxEyy>), each show becomes a
series with its seasons and episodes, served from <BASE>/tv/:

    /catalog/series/evotest-series.json
    /meta/series/evotestshow-<n>.json        (videos = the episodes)
    /stream/series/evotestshow-<n>-s<s>e<e>.json

usage: gen_addon.py <media_dir> <out_dir> <base_url> [tv_dir]
"""
import json
import os
import re
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
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    media_dir, out_dir, base = sys.argv[1], sys.argv[2], sys.argv[3].rstrip("/")
    tv_dir = sys.argv[4] if len(sys.argv) == 5 else ""

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
        "idPrefixes": ["evotest-", "evotestshow-"],
        "catalogs": [{"type": "movie", "id": "evotest", "name": "EVO Test Library"}],
    }
    shows = scan_tv(tv_dir, base) if tv_dir and os.path.isdir(tv_dir) else []
    if shows:
        manifest["types"].append("series")
        manifest["catalogs"].append({"type": "series", "id": "evotest-series",
                                     "name": "EVO Test Shows"})
    write(os.path.join(out_dir, "manifest.json"), manifest)
    write_series(out_dir, shows, base + "/poster.png")

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
    print("%d items, %d shows -> %s" % (len(items), len(shows), out_dir))


EP_RE = re.compile(r"[Ss](\d+)[Ee](\d+)")


def scan_tv(tv_dir, base):
    """[(title, [(season, episode, name, url), ...]), ...]"""
    shows = []
    for show in sorted(os.listdir(tv_dir), key=str.lower):
        sdir = os.path.join(tv_dir, show)
        if not os.path.isdir(sdir):
            continue
        eps = []
        for root, _, files in os.walk(sdir):
            for f in files:
                m = EP_RE.search(f)
                if not m or os.path.splitext(f)[1].lower() not in VIDEO_EXT:
                    continue
                rel = os.path.relpath(os.path.join(root, f), tv_dir).replace(os.sep, "/")
                eps.append((int(m.group(1)), int(m.group(2)), os.path.splitext(f)[0],
                            base + "/tv/" + urllib.parse.quote(rel)))
        if eps:
            shows.append((re.sub(r"\s*\(\d{4}\)$", "", show), sorted(eps)))
    return shows


def write_series(out_dir, shows, poster):
    metas = []
    for n, (title, eps) in enumerate(shows):
        sid = "evotestshow-%d" % n
        videos = []
        for s, e, name, url in eps:
            vid = "%s-s%de%d" % (sid, s, e)
            videos.append({"id": vid, "title": name, "season": s, "episode": e,
                           "overview": "Season %d, episode %d of the test show" % (s, e)})
            write(os.path.join(out_dir, "stream", "series", vid + ".json"),
                  {"streams": [{"name": "EVO Test", "title": "%s\nMP4" % name, "url": url},
                               {"name": "EVO Test (copy)", "title": "Same file, second source", "url": url}]})
        meta = {"id": sid, "type": "series", "name": title, "poster": poster,
                "description": "A generated test series", "videos": videos}
        write(os.path.join(out_dir, "meta", "series", sid + ".json"), {"meta": meta})
        metas.append({"id": sid, "type": "series", "name": title, "poster": poster})
    if shows:
        write(os.path.join(out_dir, "catalog", "series", "evotest-series.json"), {"metas": metas})


if __name__ == "__main__":
    main()
