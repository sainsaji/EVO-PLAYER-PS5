#!/usr/bin/env python3
# =============================================================================
# tools/shader-rip.py - extract PSSL-compiled shader blobs from decrypted PS5
# game dumps, offline.
#
#   # 1. EXTRACT (host, pure stdlib - point it at a decrypted dump)
#   python tools/shader-rip.py "E:\Game Archives\...\PPSA02800-app0" \
#          -o output/shaderrip/billiards
#
#   # 2. CLASSIFY (needs llvm-mc-18 -> run in the container)
#   docker compose run --rm ps5-dev \
#          python3 tools/shader-rip.py --classify output/shaderrip/billiards
#
#   # or do both at once when llvm-mc-18 is already on PATH:
#   python3 tools/shader-rip.py <dump> -o <out> --classify
#
# WHY THIS EXISTS
#   sceAgcCreateShader validates the "sl00" / "barefoot" reflection trailer that
#   only Sony's PSSL compiler emits. Hand-written GCN that touches a texture
#   fails 0x8a6c001f. The fix is to reuse a real compiled shader ripped from a
#   game. Doing it by hand (find "barefoot", carve the blob, disassemble,
#   eyeball the ISA) is how pp/blobs/blit_ps.* was made - this automates it.
#   Do NOT scan console process memory for these (KP's the kernel) - dumps only.
#
# BLOB FORMAT (little-endian; confirmed against pp/blobs/*.header.bin, 2026-09)
#   A shader is two contiguous parts in the dump:
#     [ GCN code | s_code_end pad | "sl00" reflection | "barefoot" footer(0x30)
#       | pad | "1234" header (hdr[0x40] bytes) ]
#   header[0x00] = "1234"      magic
#   header[0x40] = u32         header size  (== the .header.bin file size)
#   header[0x44] = u32         code size    (== the .text.bin file size; %4==0)
#   header[0x5a] = u8          stage: 1 = pixel, 2 = vertex, 0 = compute
#   header[0x5b] = u8          stage: 0x09 = pixel, 0x0a = vertex, 0 = compute
#   header[0x100..]           (reg,val) pairs; reg 0x8f = CB_TARGET_MASK
#   The code blob is  data[hdr_pos - code_size : hdr_pos]  - fed verbatim to
#   sceAgcCreateShader together with the header blob.
#
# OUTPUT
#   <out>/manifest.json          one record per unique shader (dedup by code sha)
#   <out>/<id>_<stage>.header.bin
#   <out>/<id>_<stage>.text.bin
#   <out>/<id>.dis.txt           GCN disassembly            (--classify only)
#   <out>/SUMMARY.md             ranked, human-readable      (--classify adds kinds)
#
# The "candidate" column maps a shader to a role in EVO's GPU pipeline - see the
# shader inventory table in docs/evo-pro/agc-implementation.md. blit-ps is the
# one #67 needs; sdf-ps / blur-ps are what #70 wants.
# =============================================================================
import argparse
import hashlib
import json
import mmap
import os
import re
import shutil
import struct
import subprocess
import sys

MAGIC = b"1234"
FOOT = b"barefoot"
REFL = b"sl00"

HDR_SIZE_OFF = 0x40
CODE_SIZE_OFF = 0x44
STAGE_OFF = 0x5B

MIN_HDR, MAX_HDR = 0x80, 0x800
MIN_CODE, MAX_CODE = 0x40, 0x80000

# files that never contain shader blobs - skip for speed
SKIP_EXT = {".png", ".jpg", ".jpeg", ".gif", ".bmp", ".dds", ".ktx", ".webp",
            ".wav", ".mp3", ".ogg", ".at9", ".pcm", ".mp4", ".webm", ".bik",
            ".ttf", ".otf", ".woff", ".woff2", ".txt", ".xml", ".json", ".ini",
            ".sfo", ".trp", ".npbind", ".pfsc"}

LLVM_MC = shutil.which("llvm-mc-18") or shutil.which("llvm-mc")
TRIPLE = "amdgcn--amdpal"
MCPU = "gfx1030"


# --------------------------------------------------------------------------- #
# extraction
# --------------------------------------------------------------------------- #
def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def carve(data, foot_pos):
    """Given the offset of a "barefoot" hit, return (hdr_pos, hdr, code) or None."""
    # the "1234" header sits a few pad bytes after the 0x30 footer
    win = data[foot_pos + 0x30: foot_pos + 0x30 + 0x40]
    m = win.find(MAGIC)
    if m < 0 or (m & 3):
        return None
    hdr_pos = foot_pos + 0x30 + m
    if hdr_pos + MAX_HDR > len(data):
        blob_tail = len(data) - hdr_pos
    else:
        blob_tail = MAX_HDR
    hdr_probe = data[hdr_pos: hdr_pos + max(HDR_SIZE_OFF + 8, blob_tail)]
    if len(hdr_probe) < HDR_SIZE_OFF + 8:
        return None
    hdr_size = u32(hdr_probe, HDR_SIZE_OFF)
    code_size = u32(hdr_probe, CODE_SIZE_OFF)
    if not (MIN_HDR <= hdr_size <= MAX_HDR):
        return None
    if not (MIN_CODE <= code_size <= MAX_CODE) or (code_size & 3):
        return None
    if hdr_pos - code_size < 0 or hdr_pos + hdr_size > len(data):
        return None
    hdr = data[hdr_pos: hdr_pos + hdr_size]
    code = data[hdr_pos - code_size: hdr_pos]
    if hdr[:4] != MAGIC:
        return None
    # sanity: the footer + reflection must live inside the code blob we carved
    if FOOT not in code or REFL not in code:
        return None
    return hdr_pos, hdr, code


def stage_name(hdr):
    s = hdr[STAGE_OFF] if len(hdr) > STAGE_OFF else 0
    return {0x09: "ps", 0x0a: "vs", 0x00: "cs"}.get(s, f"s{s:02x}")


def scan_file(path, rel):
    out = []
    try:
        size = os.path.getsize(path)
        if size < 0x100:
            return out
        with open(path, "rb") as fh:
            mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
            try:
                for hit in re.finditer(re.escape(FOOT), mm):
                    r = carve(mm, hit.start())
                    if not r:
                        continue
                    hdr_pos, hdr, code = r
                    out.append({
                        "source_file": rel,
                        "file_offset": hdr_pos - len(code),
                        "stage": stage_name(hdr),
                        "header_size": len(hdr),
                        "code_size": len(code),
                        "code_sha256": hashlib.sha256(code).hexdigest(),
                        "_hdr": hdr,
                        "_code": code,
                    })
            finally:
                mm.close()
    except (OSError, ValueError):
        pass
    return out


def walk(root):
    if os.path.isfile(root):
        yield root, os.path.basename(root)
        return
    for dirpath, _, names in os.walk(root):
        for n in names:
            if os.path.splitext(n)[1].lower() in SKIP_EXT:
                continue
            p = os.path.join(dirpath, n)
            yield p, os.path.relpath(p, root).replace("\\", "/")


def extract(root, outdir):
    os.makedirs(outdir, exist_ok=True)
    seen = {}
    nfiles = 0
    for path, rel in walk(root):
        nfiles += 1
        for rec in scan_file(path, rel):
            sha = rec["code_sha256"]
            if sha in seen:
                seen[sha]["occurrences"] += 1
                continue
            rid = sha[:12]
            base = f"{rid}_{rec['stage']}"
            with open(os.path.join(outdir, base + ".header.bin"), "wb") as f:
                f.write(rec.pop("_hdr"))
            with open(os.path.join(outdir, base + ".text.bin"), "wb") as f:
                f.write(rec.pop("_code"))
            rec["id"] = rid
            rec["occurrences"] = 1
            seen[sha] = rec
    recs = sorted(seen.values(), key=lambda r: (r["stage"], -r["code_size"]))
    manifest = {"root": os.path.abspath(root), "files_scanned": nfiles,
                "shaders": recs}
    with open(os.path.join(outdir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"  scanned {nfiles} files -> {len(recs)} unique shaders  ({outdir})")
    return manifest


# --------------------------------------------------------------------------- #
# classification (needs llvm-mc)
# --------------------------------------------------------------------------- #
def disasm(code):
    """Disassemble the GCN portion (skip leading zero dwords, stop at 'sl00')."""
    end = code.find(REFL)
    if end < 0:
        end = len(code)
    start = 0
    while start + 4 <= end and code[start:start + 4] == b"\x00\x00\x00\x00":
        start += 4
    body = code[start:end]
    hexs = " ".join(f"0x{b:02x}" for b in body)
    try:
        p = subprocess.run(
            [LLVM_MC, "--disassemble", f"--triple={TRIPLE}", f"--mcpu={MCPU}"],
            input=hexs, capture_output=True, text=True, timeout=30)
        return p.stdout
    except (subprocess.SubprocessError, OSError):
        return ""


def classify(dis, stage):
    lines = []
    for ln in dis.splitlines():
        ln = ln.split(";")[0].strip()
        if not ln or ln.startswith((".", "//")) or ln.endswith(":"):
            continue
        lines.append(ln)
    mnem = [ln.split()[0] for ln in lines if ln.split()]

    def n(pred):
        return sum(1 for m in mnem if pred(m))

    img_sample = n(lambda m: m.startswith("image_sample"))
    img_rw = n(lambda m: m.startswith(("image_load", "image_store")))
    buf_store = n(lambda m: m.startswith(("buffer_store", "buffer_atomic",
                                          "tbuffer_store", "ds_write")))
    buf_load_fmt = n(lambda m: m.startswith(("buffer_load_format",
                                             "tbuffer_load")))
    buf_load = n(lambda m: m.startswith("buffer_load"))
    v_alu = n(lambda m: m.startswith("v_") and m not in ("v_nop",))
    has_barrier = any(m == "s_barrier" for m in mnem)
    exports = [ln for ln in lines if ln.startswith("exp ")]
    exp_mrt = any("mrt" in e for e in exports)
    exp_pos = any("pos" in e for e in exports)
    has_interp = n(lambda m: m.startswith("v_interp") or m == "lds_param_load"
                   or m.startswith("v_interp_"))

    info = dict(inst=len(mnem), v_alu=v_alu, image_sample=img_sample,
                buffer_load=buf_load, exports=[e.split(",")[0] for e in exports])

    # --- kind -----------------------------------------------------------
    if not exports and (img_rw or buf_store or has_barrier):
        kind, cand = "compute", "cs (image/buffer copy - not renderable)"
    elif stage == "vs" or exp_pos:
        if buf_load_fmt or buf_load > 2:
            kind, cand = "vertex", "vs-structured-mesh (reads a vertex buffer)"
        elif len(mnem) < 40:
            kind, cand = "vertex", "vs-fullscreen-quad *pairs with any blit PS*"
        else:
            kind, cand = "vertex", "vs-complex"
    elif stage == "ps" or exp_mrt:
        if img_sample == 0 and buf_load == 0:
            kind, cand = "pixel", "ps-solid (vertex-colour fill)"
        elif img_sample >= 3:
            kind, cand = "pixel", "ps-blur / multi-tap  <- #70 soft-shadow candidate"
        elif img_sample and v_alu <= 10:
            kind, cand = "pixel", "ps-rgba-blit  <- #67 text/OSD composite ***"
        elif img_sample and 10 < v_alu <= 70:
            kind, cand = "pixel", "ps-sdf / analytic-AA  <- #70 candidate"
        elif buf_load >= 2 and v_alu >= 8:
            kind, cand = "pixel", "ps-video-ish (YUV sample? check matrix ALU)"
        else:
            kind, cand = "pixel", "ps-complex"
    else:
        kind, cand = "unknown", "?"

    info["kind"] = kind
    info["candidate"] = cand
    return info


RANK = {"***": 0, "#67": 0, "#70 candidate": 1, "#70 soft": 1,
        "vs-fullscreen": 2, "ps-solid": 3, "vs-structured": 4,
        "ps-video": 5, "ps-complex": 6, "vs-complex": 6,
        "not renderable": 9, "compute": 9, "?": 8}


def rank(cand):
    for k, v in RANK.items():
        if k in cand:
            return v
    return 7


def classify_dir(outdir):
    if not LLVM_MC:
        sys.exit("llvm-mc-18 not on PATH - run this step in the ps5-dev container:\n"
                 f"  docker compose run --rm ps5-dev python3 tools/shader-rip.py --classify {outdir}")
    mpath = os.path.join(outdir, "manifest.json")
    manifest = json.load(open(mpath))
    for rec in manifest["shaders"]:
        base = f"{rec['id']}_{rec['stage']}"
        code = open(os.path.join(outdir, base + ".text.bin"), "rb").read()
        dis = disasm(code)
        open(os.path.join(outdir, rec["id"] + ".dis.txt"), "w").write(dis)
        rec.update(classify(dis, rec["stage"]))
    manifest["shaders"].sort(key=lambda r: (rank(r.get("candidate", "?")),
                                            -r.get("v_alu", 0)))
    json.dump(manifest, open(mpath, "w"), indent=2)
    write_summary(outdir, manifest)
    print(f"  classified {len(manifest['shaders'])} shaders -> {outdir}/SUMMARY.md")


def write_summary(outdir, manifest):
    rows = []
    for r in manifest["shaders"]:
        rows.append("| `{id}` | {stage} | {code_size} | {inst} | {va} | {ismp} | "
                    "{cand} | {src} |".format(
                        id=r["id"], stage=r["stage"], code_size=r["code_size"],
                        inst=r.get("inst", "?"), va=r.get("v_alu", "?"),
                        ismp=r.get("image_sample", "?"),
                        cand=r.get("candidate", "?"),
                        src=r["source_file"] + f"@{r['file_offset']:#x}"))
    md = [
        f"# shader-rip: {os.path.basename(manifest['root'])}",
        "",
        f"root: `{manifest['root']}`  ",
        f"files scanned: {manifest['files_scanned']} - unique shaders: "
        f"{len(manifest['shaders'])}",
        "",
        "Blobs are `<id>_<stage>.{header,text}.bin`; disasm is `<id>.dis.txt`.",
        "Wire a chosen pair into `pp/src/agc_ui_blobs.S` + `pp/blobs/`, then "
        "verify `sceAgcCreateShader` on hardware before trusting it.",
        "",
        "| id | stage | code | #inst | #vALU | #img_sample | candidate role | source |",
        "|----|-------|------|-------|-------|-------------|----------------|--------|",
        *rows,
        "",
        "Candidate legend: `***` = the shader #67 needs (RGBA blit PS). "
        "`#70` = SDF-AA / soft-shadow material. `vs-fullscreen-quad` links with "
        "any blit PS. `compute` = not usable for rendering.",
    ]
    open(os.path.join(outdir, "SUMMARY.md"), "w").write("\n".join(md) + "\n")


# --------------------------------------------------------------------------- #
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", nargs="?", help="decrypted game dump dir or a single file")
    ap.add_argument("-o", "--out", help="output dir (default output/shaderrip/<name>)")
    ap.add_argument("--classify", nargs="?", const=True,
                    help="disassemble + label. Pass a dir to (re)classify an "
                         "existing extract; bare flag classifies the fresh one.")
    args = ap.parse_args()

    if isinstance(args.classify, str):
        classify_dir(args.classify)
        return

    if not args.dump:
        ap.error("need a dump path (or --classify <dir>)")
    out = args.out or os.path.join("output", "shaderrip",
                                   re.sub(r"[^\w.-]+", "_",
                                          os.path.basename(args.dump.rstrip("/\\"))))
    extract(args.dump, out)
    if args.classify:
        classify_dir(out)
    elif not LLVM_MC:
        print(f"\n  next: docker compose run --rm ps5-dev python3 "
              f"tools/shader-rip.py --classify {out}")


if __name__ == "__main__":
    main()
