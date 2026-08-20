#!/usr/bin/env python3
import ftplib
import os
import sys

PS5_HOST = os.environ.get("PS5_HOST", "192.168.0.12")
FTPSRV_PORT = int(os.environ.get("FTPSRV_PORT", 2121))

print(f"Connecting to PS5 FTP at {PS5_HOST}:{FTPSRV_PORT}...")
try:
    ftp = ftplib.FTP()
    ftp.connect(PS5_HOST, FTPSRV_PORT, timeout=10)
    ftp.login()
    ftp.set_pasv(True)
    print("Connected successfully!")
except Exception as e:
    print(f"Failed to connect to PS5 FTP: {e}")
    sys.exit(1)

def ensure_dir(d):
    cur = ""
    for p in d.strip("/").split("/"):
        cur += "/" + p
        try:
            ftp.mkd(cur)
        except Exception:
            pass

def upload_file(src, dst):
    print(f"  Uploading {src} -> {dst}")
    with open(src, "rb") as f:
        ftp.storbinary(f"STOR {dst}", f)

# 1. Sync assets
print("--- Syncing assets to PS5...")
asset_dirs = [
    "/data/homebrew/EVOPlayer/assets",
    "/data/evoplayer/app/assets"
]

for base_dst in asset_dirs:
    for root, dirs, files in os.walk("projects/evoplayer/assets"):
        rel = os.path.relpath(root, "projects/evoplayer/assets").replace("\\", "/")
        target_dir = base_dst if rel == "." else f"{base_dst}/{rel}"
        ensure_dir(target_dir)
        for f in files:
            src_path = os.path.join(root, f)
            dst_path = f"{target_dir}/{f}"
            upload_file(src_path, dst_path)

# 2. Upload ELF
print("--- Uploading EVOPlayer.elf...")
ensure_dir("/data/homebrew/EVOPlayer")
upload_file("output/elf/EVOPlayer.elf", "/data/homebrew/EVOPlayer/eboot.elf")

ftp.quit()
print("All files pushed to PS5 successfully!")
