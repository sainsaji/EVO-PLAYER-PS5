#!/usr/bin/env python3
"""
Generate a provider UI bundle's manifest.json (#90).

A bundle's manifest lists every file with its byte count and sha256, and EVO
refuses a file whose size or hash does not match. Writing that by hand is not
viable - the hashes change on every edit - so this regenerates it from whatever
is in the directory.

    tools/gen_provider_manifest.py assets/providers/iptv

Fields that are not derivable from the files (id, name, entry, data_model,
api_version) are read from an existing manifest.json if there is one, so a
regeneration never loses them, and fall back to sensible defaults keyed off the
directory name otherwise.

`version` is bumped to the current UTC timestamp on every run. That is what EVO
compares to decide whether a cached bundle is stale, so an edit that did not
change the version would be served and ignored - which looks exactly like the
edit not having worked.
"""
import hashlib
import json
import sys
import time
from pathlib import Path

# Only these extensions may be in a bundle. A bundle contains no code; this is
# the list from evo_provider_bundle.h's trust-boundary comment, enforced here
# too so a stray file cannot be shipped by accident.
ALLOWED = {".rml", ".rcss", ".ttf", ".otf", ".png", ".jpg", ".jpeg", ".webp"}

# Must match EVO_BUNDLE_MAX_* in evo_provider_bundle.h. Checked here so the
# failure is a build-time message instead of a runtime fallback skin.
MAX_ENTRIES = 64
MAX_FILE_BYTES = 2 * 1024 * 1024
MAX_TOTAL_BYTES = 8 * 1024 * 1024


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip())
        return 2

    root = Path(sys.argv[1]).resolve()
    if not root.is_dir():
        print(f"error: {root} is not a directory")
        return 1

    manifest_path = root / "manifest.json"
    prev = {}
    if manifest_path.is_file():
        try:
            prev = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            print(f"warning: existing manifest unreadable ({exc}); starting fresh")

    entries = []
    total = 0
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.name == "manifest.json":
            continue
        if path.suffix.lower() not in ALLOWED:
            print(f"error: {path.relative_to(root)} has a disallowed extension "
                  f"({path.suffix}); a bundle may only contain {sorted(ALLOWED)}")
            return 1

        data = path.read_bytes()
        if len(data) > MAX_FILE_BYTES:
            print(f"error: {path.relative_to(root)} is {len(data)} bytes, "
                  f"over the {MAX_FILE_BYTES} per-file cap")
            return 1
        total += len(data)

        entries.append({
            # Forward slashes always: the published path is what the .rml
            # references and what EVO resolves, on both platforms.
            "path": path.relative_to(root).as_posix(),
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        })

    if not entries:
        print(f"error: {root} contains no bundle files")
        return 1
    if len(entries) > MAX_ENTRIES:
        print(f"error: {len(entries)} files, over the {MAX_ENTRIES} cap")
        return 1
    if total > MAX_TOTAL_BYTES:
        print(f"error: {total} bytes total, over the {MAX_TOTAL_BYTES} cap")
        return 1

    entry = prev.get("entry") or "main.rml"
    if not any(e["path"] == entry for e in entries):
        print(f"error: entry document '{entry}' is not one of the bundle files")
        return 1

    manifest = {
        "id": prev.get("id") or root.name,
        "name": prev.get("name") or root.name.upper(),
        "version": time.strftime("%Y%m%d.%H%M%S", time.gmtime()),
        "api_version": prev.get("api_version", 1),
        "entry": entry,
        "data_model": prev.get("data_model") or root.name,
        "assets": entries,
    }

    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n",
                             encoding="utf-8")
    print(f"ok {manifest_path.relative_to(Path.cwd()) if manifest_path.is_relative_to(Path.cwd()) else manifest_path}")
    print(f"   id={manifest['id']} model={manifest['data_model']} "
          f"entry={manifest['entry']} version={manifest['version']}")
    print(f"   {len(entries)} files, {total} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
