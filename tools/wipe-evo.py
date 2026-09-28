#!/usr/bin/env python3
"""
tools/wipe-evo.py - remove every EVO Player file from the console, for a
fresh install. EVO ONLY: other homebrew, games and the rest of the USB stick
are never touched.

    python3 tools/wipe-evo.py <host>            # dry run: list what would go
    python3 tools/wipe-evo.py <host> --do       # delete it
    ./scripts/deploy-app.sh --ffpfsc --fresh    # wipe + deploy in one go

What goes:
  /data/homebrew/<TITLE_ID>.ffpfsc and /data/homebrew/<TITLE_ID>/   the app
  /data/evoplayer/          settings, recent, favourites, resume points,
                            provider logins + playlists, web sessions,
                            speaker calibration
  /mnt/usb0/evo*, /mnt/usb0/pp_4k_stage*   logs, screenshots, dev-remote files

Two quirks of the console's FTP server, both found the hard way:
  * MLSD ignores its path argument and lists the current directory, so every
    listing cwd()s first (a path-argument walk silently lists "/" instead).
  * DELE answers 226, not 250, which ftplib treats as an error.

Never run this against a running EVO: deploy-app.sh --fresh checks first.
"""
import ftplib
import sys

TITLE_ID = "PPSA99039"
DATA_DIR = "/data/evoplayer"
USB = "/mnt/usb0"


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1].startswith("-"):
        print(__doc__)
        return 2
    host = sys.argv[1]
    port = 2121
    do = "--do" in sys.argv[2:]

    ftp = ftplib.FTP()
    ftp.connect(host, port, timeout=20)
    ftp.login()
    ftp.set_pasv(True)
    counts = {"files": 0, "dirs": 0, "bytes": 0}

    def ls(path):
        ftp.cwd(path)
        out = [(n, f.get("type"), int(f.get("size", 0) or 0))
               for n, f in ftp.mlsd() if n not in (".", "..")]
        ftp.cwd("/")
        return out

    def ok2xx(cmd):
        try:
            resp = ftp.sendcmd(cmd)
        except ftplib.error_reply as e:      # 226 "File deleted" lands here
            resp = str(e)
        if not resp.startswith("2"):
            raise RuntimeError(f"{cmd}: {resp}")

    def rm_file(path, size):
        counts["files"] += 1
        counts["bytes"] += size
        print(("  del " if do else "  would del ") + path)
        if do:
            ok2xx("DELE " + path)

    def rm_tree(path):
        for name, kind, size in ls(path):
            full = path.rstrip("/") + "/" + name
            if kind == "dir":
                rm_tree(full)
            else:
                rm_file(full, size)
        counts["dirs"] += 1
        print(("  rmd " if do else "  would rmd ") + path)
        if do:
            ok2xx("RMD " + path)

    for name, kind, size in ls("/data/homebrew"):
        if name == TITLE_ID + ".ffpfsc":
            rm_file("/data/homebrew/" + name, size)
        elif name == TITLE_ID and kind == "dir":
            rm_tree("/data/homebrew/" + name)

    if any(n == DATA_DIR.rsplit("/", 1)[1] for n, _, _ in ls("/data")):
        rm_tree(DATA_DIR)

    try:
        usb = ls(USB)
    except ftplib.all_errors:
        usb = []
        print(f"  ({USB} not mounted - skipped)")
    for name, kind, size in usb:
        if name.startswith("evo") or name.startswith("pp_4k_stage"):
            full = f"{USB}/{name}"
            rm_tree(full) if kind == "dir" else rm_file(full, size)

    try:
        ftp.quit()
    except ftplib.all_errors:
        pass
    print(f"{'wiped' if do else 'dry run'}: {counts['files']} files, "
          f"{counts['dirs']} dirs, {counts['bytes'] / 1e6:.1f} MB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
