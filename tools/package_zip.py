#!/usr/bin/env python3
"""
package_zip.py - Package an app folder into a distribution ZIP archive.

Usage:
    python3 tools/package_zip.py <source_dir> <output_zip> [title_id]

Example:
    python3 tools/package_zip.py output/app/PPSA99039 dist/EVOPlayer-v0.10.0-PPSA99039.zip PPSA99039
"""

import os
import sys
import zipfile


def create_app_zip(src_dir: str, out_zip: str, title_id: str) -> None:
    src_dir = os.path.abspath(src_dir)
    out_zip = os.path.abspath(out_zip)
    os.makedirs(os.path.dirname(out_zip), exist_ok=True)

    if not os.path.isdir(src_dir):
        raise FileNotFoundError(f"Source directory not found: {src_dir}")

    print(f"Creating ZIP archive: {out_zip}")
    print(f"  Source directory: {src_dir}")
    print(f"  Root folder: {title_id}/")

    count = 0
    total_bytes = 0

    with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        # Root directory entry
        zf.writestr(f"{title_id}/", "")

        for root, dirs, files in os.walk(src_dir):
            dirs.sort()
            files.sort()
            rel_dir = os.path.relpath(root, src_dir)

            if rel_dir != ".":
                norm_rel_dir = rel_dir.replace("\\", "/")
                zf.writestr(f"{title_id}/{norm_rel_dir}/", "")

            for filename in files:
                full_path = os.path.join(root, filename)
                if rel_dir == ".":
                    arcname = f"{title_id}/{filename}"
                else:
                    norm_rel_dir = rel_dir.replace("\\", "/")
                    arcname = f"{title_id}/{norm_rel_dir}/{filename}"

                # Preserve posix file permissions if available, or default to 0o755 for binaries / 0o644 for files
                zinfo = zipfile.ZipInfo.from_file(full_path, arcname=arcname)
                zinfo.compress_type = zipfile.ZIP_DEFLATED
                if filename.endswith(".bin") or filename.endswith(".prx") or filename.endswith(".elf"):
                    zinfo.external_attr = 0o100755 << 16
                else:
                    zinfo.external_attr = 0o100644 << 16

                with open(full_path, "rb") as f:
                    zf.writestr(zinfo, f.read())

                count += 1
                total_bytes += os.path.getsize(full_path)

    zip_size = os.path.getsize(out_zip)
    print(f"ZIP package created successfully.")
    print(f"  Files packaged: {count} ({total_bytes:,} uncompressed bytes)")
    print(f"  Archive size:   {zip_size:,} bytes")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    src_dir = sys.argv[1]
    out_zip = sys.argv[2]
    title_id = sys.argv[3] if len(sys.argv) > 3 else "PPSA99039"

    create_app_zip(src_dir, out_zip, title_id)


if __name__ == "__main__":
    main()
