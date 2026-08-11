#!/usr/bin/env python3
"""Sony NID hashing, offline.

A NID is how a Sony module advertises an exported symbol: the name is never
stored, only a hash of it. The hash is one-way, so the *only* practical way to
put a name to a NID is to hash candidate names and look for a match - never to
invert it. That is why this module exists and why `encode` is the important
function here, not some hypothetical `decode`.

Validated against the eleven name/NID pairs decoder_test resolved on hardware
(12.70) - run this file directly to re-check them.

Usage:
    from nid import encode, AeroLib
    encode("sceAvPlayerInit")            -> 'aS66RI0gGgo'
    AeroLib().lookup('aS66RI0gGgo')      -> 'sceAvPlayerInit'
"""

import csv
import hashlib
import os
import struct

# The constant Sony appends to the symbol name before hashing. Without it the
# digest does not match anything.
_SUFFIX = bytes([0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1,
                 0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30])

# Base64 with '+' and '-' as the last two digits, not '+/' and not '-_'.
_ALPHABET = ("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
             "abcdefghijklmnopqrstuvwxyz"
             "0123456789+-")

_HERE = os.path.dirname(os.path.abspath(__file__))
AEROLIB_PATH = os.path.join(_HERE, "aerolib.csv")


def encode(name: str) -> str:
    """Hash a symbol name into its 11-character NID."""
    digest = hashlib.sha1(name.encode("utf-8") + _SUFFIX).digest()
    value = struct.unpack("<Q", digest[:8])[0]

    # 11 base64 digits, most significant first, covering 66 bits; the top two
    # bits of the first digit are discarded, which is why the encoding is not
    # reversible even before the hash is considered.
    out = []
    for i in range(11):
        shift = 58 - 6 * i
        out.append(_ALPHABET[(value >> shift) & 0x3F] if shift >= 0
                   else _ALPHABET[(value << -shift) & 0x3F])
    return "".join(out)


class AeroLib:
    """The community NID/name catalogue from zecoxao/sce_symbols.

    Looking a NID up here costs milliseconds and settles most of the naming
    problem for free, so it is always worth doing before hashing candidates.
    """

    def __init__(self, path: str = AEROLIB_PATH):
        self.by_nid = {}
        self.by_name = {}
        if not os.path.exists(path):
            raise FileNotFoundError(
                f"{path} not found. Fetch it with:\n"
                "  curl -sfLo tools/re/aerolib.csv https://raw.githubusercontent.com"
                "/zecoxao/sce_symbols/master/aerolib.csv")
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for row in csv.reader(fh, delimiter=" ", skipinitialspace=True):
                if len(row) < 2:
                    continue
                nid, name = row[0], row[1]
                self.by_nid.setdefault(nid, name)
                self.by_name.setdefault(name, nid)

    def lookup(self, nid: str):
        return self.by_nid.get(nid)

    def names_matching(self, *substrings, case_sensitive=False):
        """Every catalogued name containing any of the given substrings.

        This is the candidate generator for the dictionary attack: filter the
        catalogue down to plausible names, hash them, and intersect with a
        module's real export list.
        """
        subs = substrings if case_sensitive else [s.lower() for s in substrings]
        out = []
        for name in self.by_name:
            hay = name if case_sensitive else name.lower()
            if any(s in hay for s in subs):
                out.append(name)
        return sorted(out)


# Measured on hardware, 12.70, 2026-08-09 and 2026-08-10.
_KNOWN = {
    "sceAvPlayerInit":                  "aS66RI0gGgo",
    "sceAvPlayerAddSource":             "KMcEa+rHsIo",
    "sceAvPlayerGetVideoData":          "o3+RWnHViSg",
    "sceAvPlayerGetAudioData":          "Wnp1OVcrZgk",
    "sceAvPlayerIsActive":              "UbQoYawOsfY",
    "sceAvPlayerClose":                 "NkJwDzKmIlw",
    "sceVideoDecoderQueryResourceInfo": "eJy1cz7NtgE",
    "sceVideoDecoderCreate":            "eZ0RSV2X1aY",
    "sceVideoDecoderDecode":            "AxKjazKn2GE",
    "sceVideoDecoderFlush":             "LqaAODB8fJ8",
    "sceVideoDecoderDelete":            "qIbsRYBeuCk",
}


if __name__ == "__main__":
    bad = 0
    for name, want in _KNOWN.items():
        got = encode(name)
        flag = "ok " if got == want else "BAD"
        if got != want:
            bad += 1
        print(f"  {flag} {name:38s} want={want} got={got}")
    print(f"\n{len(_KNOWN) - bad}/{len(_KNOWN)} match")
    raise SystemExit(1 if bad else 0)
