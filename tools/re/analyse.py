#!/usr/bin/env python3
"""Offline static analysis of the mapped module images dumped by decoder_test.

PHASE 1 of docs/hardware-decode.md, per the reordering argued in
docs/hardware-decode-review.md. Nothing here touches the console.

WHAT WE HAVE AND WHAT WE DO NOT
    The dump is of the *mapped* image, which for a Sony SPRX is four segments:
        s0  --x   code. No ELF header: the loader does not map one.
        s1  r--   .eh_frame_hdr + .eh_frame
        s2  r--   relocated pointer tables (GOT, vtables, .data.rel.ro)
        s3  rw-   .data
    PT_DYNAMIC and the SCE dynlib data - where the import/export NID lists
    live - are NOT mapped, so the review's "read the import list from
    PT_DYNAMIC" is not available. Measured, not assumed.

    Two things replace it, and both are better than they sound:

    1. THE IMPORT GRAPH FROM THE RELOCATED GOT. Every pointer in s2/s3 has
       already been resolved by the loader to a real address. Classifying each
       one by which module's address range it lands in reconstructs the import
       graph as *linked*, which is stronger evidence than a declared DT_NEEDED
       list: it is what the module actually points at.

    2. THE FUNCTION LIST FROM .eh_frame_hdr. Its binary-search table holds the
       entry address of every function with unwind information, which for a
       C++ module is essentially all of them. That is a complete function
       inventory without a symbol table.

Usage:
    python analyse.py                 # everything
    python analyse.py imports         # import graph only
    python analyse.py funcs           # function inventory
    python analyse.py strings
"""

import os
import re
import struct
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
DUMP = os.path.join(HERE, "..", "..", "proprietary", "dump")
LOG = os.path.join(DUMP, "evo_dump_log.txt")
MANIFEST = os.path.join(DUMP, "evo_dump_manifest.txt")

TARGETS = ["libSceAvPlayer.sprx", "libSceVdecCore.sprx",
           "libSceVideoDecoderArbitration.sprx"]


# --------------------------------------------------------------------------
# Parsing what the console told us
# --------------------------------------------------------------------------

class Segment:
    def __init__(self, module, idx, vaddr, size, prot, path):
        self.module, self.idx = module, idx
        self.vaddr, self.size, self.prot = vaddr, size, prot
        self.path = path
        self._data = None

    @property
    def data(self):
        if self._data is None:
            with open(self.path, "rb") as fh:
                self._data = fh.read()
        return self._data

    def contains(self, addr):
        return self.vaddr <= addr < self.vaddr + self.size

    def __repr__(self):
        return f"<{self.module} s{self.idx} 0x{self.vaddr:x}+0x{self.size:x}>"


def load_modules():
    """Every module in the process, from the log's module table.

    Needed even for modules that were not dumped: classifying a GOT pointer
    requires knowing whose address range it falls in.
    """
    mods = {}
    cur = None
    started = False
    with open(LOG, "r", errors="replace") as fh:
        for line in fh:
            if line.startswith("Modules AFTER"):
                started = True
                continue
            if not started:
                continue
            if line.startswith("Delta"):
                break
            m = re.match(r"\s+modid=0x([0-9a-f]+)\s+(\S+)\s+segs=(\d+)", line)
            if m:
                cur = m.group(2)
                mods[cur] = {"modid": int(m.group(1), 16), "segs": []}
                continue
            m = re.match(r"\s+s(\d)\s+0x([0-9a-f]+)\s+size=0x([0-9a-f]+)\s+"
                         r"prot=\S+\((\d)\)", line)
            if m and cur:
                mods[cur]["segs"].append({
                    "idx": int(m.group(1)),
                    "vaddr": int(m.group(2), 16),
                    "size": int(m.group(3), 16),
                    "prot": int(m.group(4)),
                })
    return mods


def load_dumped():
    """The segments we actually have bytes for."""
    segs = defaultdict(dict)
    with open(MANIFEST, "r", errors="replace") as fh:
        for line in fh:
            if line.startswith("#"):
                continue
            f = line.rstrip("\n").split("\t")
            if len(f) < 3 or not f[1].startswith("seg"):
                continue
            kv = dict(x.split("=", 1) for x in f[2:] if "=" in x)
            idx = int(f[1][3:])
            segs[f[0]][idx] = Segment(
                f[0], idx, int(kv["vaddr"], 16), int(kv["size"], 16),
                int(kv["prot"]), os.path.join(DUMP, kv["file"]))
    return segs


def load_symbols():
    syms = defaultdict(dict)
    with open(MANIFEST, "r", errors="replace") as fh:
        for line in fh:
            f = line.rstrip("\n").split("\t")
            if len(f) < 3 or f[1] != "symbol":
                continue
            kv = dict(x.split("=", 1) for x in f[3:] if "=" in x)
            syms[f[0]][f[2]] = int(kv["offset"], 16)
    return syms


def owner_of(mods, addr):
    """Which module and segment an address belongs to, if any."""
    for name, info in mods.items():
        for s in info["segs"]:
            if s["vaddr"] <= addr < s["vaddr"] + s["size"]:
                return name, s["idx"]
    return None, None


# --------------------------------------------------------------------------
# 1. Import graph, reconstructed from relocated pointers
# --------------------------------------------------------------------------

def import_graph(mods, dumped):
    print("=" * 74)
    print("IMPORT GRAPH - from relocated pointers in the mapped image")
    print("=" * 74)
    print("Every 8-byte aligned value in the read-only and data segments that")
    print("lands inside another module's mapping is a resolved import or a")
    print("cross-module pointer. Counts are pointer slots, not call sites.\n")

    for target in TARGETS:
        if target not in dumped:
            continue
        print(f"--- {target}")
        hits = Counter()
        code_hits = Counter()
        distinct = defaultdict(set)

        for idx in sorted(dumped[target]):
            if idx in (0, 1):
                # s0 is code and s1 is packed DWARF; scanning either produces
                # convincing-looking garbage. Only s2/s3 hold real pointers.
                continue
            seg = dumped[target][idx]
            data = seg.data
            for off in range(0, len(data) - 8, 8):
                val = struct.unpack_from("<Q", data, off)[0]
                if val < 0x400000 or val > 0x1000000000:
                    continue
                owner, sidx = owner_of(mods, val)
                if owner is None:
                    continue
                if owner == target:
                    continue        # self-reference, not an import
                hits[owner] += 1
                distinct[owner].add(val)
                if sidx == 0:
                    code_hits[owner] += 1

        if not hits:
            print("    (no cross-module pointers found)\n")
            continue

        for owner, n in hits.most_common():
            d = distinct[owner]
            print(f"    {owner:38s} {n:5d} ptr  ({code_hits[owner]} into code,"
                  f" {len(d)} distinct target(s))")
        print()

        # For the small, interesting dependencies, the individual targets say
        # more than the count does: three pointers into one function is a
        # single API being called from three places.
        for owner, d in sorted(distinct.items(), key=lambda x: len(x[1])):
            if len(d) > 12:
                continue
            obase = min(s["vaddr"] for s in mods[owner]["segs"])
            print(f"    targets in {owner}:")
            for val in sorted(d):
                print(f"        0x{val:x}  (+0x{val - obase:x})")
        print()


def unresolved_pointers(mods, dumped):
    """Pointers that land nowhere - i.e. into modules that are NOT loaded.

    A module AvPlayer needs but that was never loaded cannot show up in the
    graph above, so this is the check that stops that being invisible.
    """
    print("=" * 74)
    print("POINTER SLOTS THAT RESOLVE TO NOTHING LOADED")
    print("=" * 74)
    for target in TARGETS:
        if target not in dumped:
            continue
        unowned = []
        for idx in sorted(dumped[target]):
            if idx in (0, 1):
                continue
            seg = dumped[target][idx]
            data = seg.data
            for off in range(0, len(data) - 8, 8):
                val = struct.unpack_from("<Q", data, off)[0]
                if val < 0x400000 or val > 0x1000000000:
                    continue
                if owner_of(mods, val)[0] is None:
                    unowned.append((seg.vaddr + off, val))
        print(f"--- {target}: {len(unowned)} unowned pointer(s)")
        for a, v in unowned[:12]:
            print(f"      [0x{a:x}] -> 0x{v:x}")
        print()


# --------------------------------------------------------------------------
# 2. Function inventory from .eh_frame_hdr
# --------------------------------------------------------------------------

def eh_frame_functions(seg):
    """Entry address of every function with unwind info, in order.

    .eh_frame_hdr's search table is (initial_location, fde_pointer) pairs
    sorted by address, both encoded DW_EH_PE_datarel|sdata4 - relative to the
    start of the section, which is the start of this segment.
    """
    d = seg.data
    if len(d) < 12 or d[0] != 1:
        return []
    if d[1] != 0x1b or d[2] != 0x03 or d[3] != 0x3b:
        return []               # a different encoding; not handled

    count = struct.unpack_from("<I", d, 8)[0]
    base = seg.vaddr
    out = []
    for i in range(count):
        off = 12 + i * 8
        if off + 8 > len(d):
            break
        loc = struct.unpack_from("<i", d, off)[0]
        out.append(base + loc)
    return out


def functions(mods, dumped, syms):
    print("=" * 74)
    print("FUNCTION INVENTORY - from .eh_frame_hdr")
    print("=" * 74)
    for target in TARGETS:
        if target not in dumped or 1 not in dumped[target]:
            continue
        seg = dumped[target][1]
        fns = eh_frame_functions(seg)
        text = dumped[target][0]
        inrange = [f for f in fns if text.contains(f)]
        print(f"--- {target}: {len(fns)} FDEs, {len(inrange)} inside the text "
              f"segment")
        if inrange:
            print(f"    first 0x{inrange[0] - text.vaddr:x}, "
                  f"last 0x{inrange[-1] - text.vaddr:x} "
                  f"(text is 0x{text.size:x})")
        for name, off in sorted(syms.get(target, {}).items(), key=lambda x: x[1]):
            addr = text.vaddr + off
            exact = addr in inrange
            print(f"    {name:28s} +0x{off:<8x} "
                  f"{'is an FDE start' if exact else 'NOT an FDE start'}")
        print()
    return


def function_bounds(dumped, target, off):
    """Where the function at this text offset ends, per .eh_frame_hdr."""
    text = dumped[target][0]
    fns = sorted(f for f in eh_frame_functions(dumped[target][1])
                 if text.contains(f))
    addr = text.vaddr + off
    for i, f in enumerate(fns):
        if f == addr:
            end = fns[i + 1] if i + 1 < len(fns) else text.vaddr + text.size
            return addr, end
    return addr, addr + 0x200


# --------------------------------------------------------------------------
# 3. Strings
# --------------------------------------------------------------------------

def strings(dumped, minlen=6):
    print("=" * 74)
    print("STRINGS")
    print("=" * 74)
    pat = re.compile(rb"[\x20-\x7e]{%d,}" % minlen)
    for target in TARGETS:
        if target not in dumped:
            continue
        print(f"--- {target}")
        seen = []
        for idx in sorted(dumped[target]):
            for m in pat.finditer(dumped[target][idx].data):
                s = m.group().decode("ascii")
                seen.append((idx, dumped[target][idx].vaddr + m.start(), s))
        print(f"    {len(seen)} strings")
        for idx, addr, s in seen:
            print(f"    s{idx} 0x{addr:x}  {s}")
        print()


# --------------------------------------------------------------------------

def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    mods = load_modules()
    dumped = load_dumped()
    syms = load_symbols()

    print(f"{len(mods)} modules in the process, "
          f"{sum(len(v) for v in dumped.values())} segments dumped\n")

    if what in ("all", "imports"):
        import_graph(mods, dumped)
        unresolved_pointers(mods, dumped)
    if what in ("all", "funcs"):
        functions(mods, dumped, syms)
    if what in ("all", "strings"):
        strings(dumped)


if __name__ == "__main__":
    main()
