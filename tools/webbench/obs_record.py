#!/usr/bin/env python3
"""Record the PC's OBS (the HDMI capture of the PS5) for N seconds through obs-websocket v5.

usage: python3 tools/webbench/obs_record.py [seconds]   (default 15)

OBS must be running with the websocket server on (port 4455, no password, or set
OBS_WS_PASSWORD). Prints the path of the finished recording. Then scan it:
  python3 <scan_dark.py> "<path>"   (black-frame scan used for #237)
"""
import base64
import hashlib
import json
import os
import sys
import time
import uuid

import websocket  # websocket-client

SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0
URL = os.environ.get("OBS_WS_URL", "ws://127.0.0.1:4455")
PASSWORD = os.environ.get("OBS_WS_PASSWORD", "")


def auth_string(challenge, salt):
    secret = base64.b64encode(hashlib.sha256((PASSWORD + salt).encode()).digest()).decode()
    return base64.b64encode(hashlib.sha256((secret + challenge).encode()).digest()).decode()


ws = websocket.create_connection(URL, timeout=15)
hello = json.loads(ws.recv())
ident = {"op": 1, "d": {"rpcVersion": 1}}
if "authentication" in hello["d"]:
    a = hello["d"]["authentication"]
    ident["d"]["authentication"] = auth_string(a["challenge"], a["salt"])
ws.send(json.dumps(ident))
json.loads(ws.recv())  # Identified


def request(kind, data=None):
    rid = str(uuid.uuid4())
    ws.send(json.dumps({"op": 6, "d": {"requestType": kind, "requestId": rid, "requestData": data or {}}}))
    while True:
        msg = json.loads(ws.recv())
        if msg.get("op") == 7 and msg["d"].get("requestId") == rid:
            return msg["d"]


status = request("GetRecordStatus")["responseData"]
if status.get("outputActive"):
    print("OBS is already recording; stopping that first", file=sys.stderr)
    request("StopRecord")
    time.sleep(2)
res = request("StartRecord")
if not res["requestStatus"].get("result"):
    sys.exit("StartRecord failed: %s" % res["requestStatus"])
t0 = time.time()
print("recording %.0f s ..." % SECS, flush=True)
time.sleep(SECS)
res = request("StopRecord")
path = (res.get("responseData") or {}).get("outputPath")
if not path:
    sys.exit("StopRecord gave no path: %s" % res)
print("recorded %.1f s" % (time.time() - t0))
print(path)
ws.close()
