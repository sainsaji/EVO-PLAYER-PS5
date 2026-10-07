#!/usr/bin/env python3
"""dualsense_dump.py - record what a real DualSense tells a host.

Part of the "wake the PS5 with a pretend controller" spike (docs/build/console-wake.md):
to imitate the controller, a Bluetooth device has to answer the console the way
the real one does. This reads the report descriptor and the read-only feature
reports over USB and saves them.

  pip install hidapi
  python tools/dualsense_dump.py --list
  python tools/dualsense_dump.py            # first Sony controller found

Use a USB cable to the PC. Plugging it in does not change its Bluetooth pairing
(pairing it to the PC over Bluetooth WOULD replace the PS5 pairing - do not do that).
Only GET_FEATURE reads of known report ids are made; nothing is written to the
controller.

Output: output/dualsense/<product>_<pid>.json (git-ignored). It contains the
controller's serial / Bluetooth address, so do not paste or commit it.
"""
import argparse
import json
import sys
from pathlib import Path

try:
    import hid
except ImportError:
    sys.exit("missing module: pip install hidapi")

SONY = 0x054C
PRODUCTS = {
    0x0CE6: "DualSense",
    0x0DF2: "DualSense Edge",
    0x05C4: "DualShock 4 v1",
    0x09CC: "DualShock 4 v2",
}
# id -> (name, length). Lengths are the USB sizes including the report id byte.
FEATURE_REPORTS = {
    0x05: ("calibration", 41),
    0x09: ("pairing info (Bluetooth address)", 20),
    0x20: ("firmware info", 64),
    0xA3: ("firmware version (DS4)", 49),
}


def sony_devices():
    return [d for d in hid.enumerate(SONY, 0) if d["product_id"] in PRODUCTS]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--list", action="store_true", help="only list Sony controllers")
    args = ap.parse_args()

    devs = sony_devices()
    if args.list or not devs:
        for d in devs:
            print(f"{PRODUCTS[d['product_id']]}  vid=0x{d['vendor_id']:04X} pid=0x{d['product_id']:04X}  "
                  f"interface={d.get('interface_number')}  usage_page=0x{d.get('usage_page', 0):X}")
        if not devs:
            print("no Sony controller found - plug it in with a USB cable (not Bluetooth).")
        return 0 if devs else 1

    d = devs[0]
    h = hid.device()
    h.open_path(d["path"])
    out = {
        "product": PRODUCTS[d["product_id"]],
        "vendor_id": f"0x{d['vendor_id']:04X}",
        "product_id": f"0x{d['product_id']:04X}",
        "manufacturer_string": h.get_manufacturer_string(),
        "product_string": h.get_product_string(),
        "serial_number_string": h.get_serial_number_string(),
        "release_number": f"0x{d.get('release_number', 0):04X}",
        "report_descriptor": None,
        "feature_reports": {},
    }
    try:
        desc = h.get_report_descriptor()
        out["report_descriptor"] = bytes(desc).hex()
        out["report_descriptor_length"] = len(desc)
    except Exception as e:                       # older hidapi builds lack it
        out["report_descriptor_error"] = f"{type(e).__name__}: {e}"

    for rid, (name, length) in FEATURE_REPORTS.items():
        try:
            data = h.get_feature_report(rid, length)
            out["feature_reports"][f"0x{rid:02X}"] = {"name": name, "bytes": bytes(data).hex()}
        except Exception as e:
            out["feature_reports"][f"0x{rid:02X}"] = {"name": name, "error": f"{type(e).__name__}: {e}"}
    h.close()

    dest = Path(__file__).resolve().parents[1] / "output" / "dualsense"
    dest.mkdir(parents=True, exist_ok=True)
    path = dest / f"{out['product'].replace(' ', '_')}_{d['product_id']:04x}.json"
    path.write_text(json.dumps(out, indent=2), encoding="utf-8", newline="\n")

    print(f"{out['product']} ({out['vendor_id']}:{out['product_id']}) saved to {path}")
    print(f"  descriptor: {out.get('report_descriptor_length', 'not read')} bytes")
    for rid, v in out["feature_reports"].items():
        print(f"  feature {rid} {v['name']}: " + (f"{len(v['bytes']) // 2} bytes" if "bytes" in v else v["error"]))
    print("This file holds the controller's serial / Bluetooth address: keep it out of git.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
