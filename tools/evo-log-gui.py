#!/usr/bin/env python3
"""
evo-log-gui.py - a tiny Tkinter GUI for pulling EVO Player's on-console logs.

EVO (PPSA99039, app module) writes its diagnostic logs to /mnt/usb0/ on the
console. The jailbreak chain exposes those over anonymous FTP (default port
2121) - the same transport tools/evo-remote.sh uses. This GUI is a
point-and-click front end for that: pick a console, tick the logs you want,
Fetch. Optionally auto-refresh, save a local copy, or delete the remote files.

Stdlib only (tkinter + ftplib) - no pip install, no container. Run on the host:

    python tools/evo-log-gui.py

Host/port default from .env (PS5_HOST=...) at the repo root, overridable in the
UI. Nothing here launches or deploys anything; it is read/delete only.
"""

from __future__ import annotations

import io
import os
import queue
import threading
import time
from ftplib import FTP, error_perm
from pathlib import Path

import tkinter as tk
from tkinter import messagebox, ttk

REPO_ROOT = Path(__file__).resolve().parent.parent
LOG_OUT = REPO_ROOT / "output" / "logs"
DEFAULT_FTP_PORT = 2121

# EVO's log/diagnostic files on /mnt/usb0 (see CLAUDE.md analysis + the code):
#   (path, label, on-by-default-in-a-normal-build)
KNOWN_LOGS = [
    ("/mnt/usb0/evo_boot.log",                "evo_boot.log  (boot trace / pre-unjail probes)", True),
    ("/mnt/usb0/pp_4k_stage_breadcrumb.txt",  "pp_4k_stage_breadcrumb.txt  (playback stages)",  True),
    ("/mnt/usb0/pp_4k_stage_last.txt",        "pp_4k_stage_last.txt  (last checkpoint)",        True),
    ("/mnt/usb0/pp_playback_stats.txt",       "pp_playback_stats.txt  (last file's stats)",     True),
    ("/mnt/usb0/evo_status",                  "evo_status  (--usb-remote, ~1/s)",               False),
    ("/mnt/usb0/evo_vdec.log",                "evo_vdec.log  (--usb-remote, per-frame decode)", False),
    ("/mnt/usb0/evo_vo_debug.log",            "evo_vo_debug.log  (-DEVO_VO_DEBUG only)",        False),
    ("/mnt/usb0/evo_compat_report.txt",       "evo_compat_report.txt  (user-triggered)",       False),
]


def read_env_host() -> str:
    env = REPO_ROOT / ".env"
    if env.is_file():
        for line in env.read_text(encoding="utf-8", errors="replace").splitlines():
            line = line.strip()
            if line.startswith("PS5_HOST=") and "#" not in line.split("=", 1)[0]:
                val = line.split("=", 1)[1].split("#", 1)[0].strip()
                if val:
                    return val
    return ""


def ftp_fetch(host: str, port: int, remote: str) -> str:
    """Return the remote file's text, or raise. error_perm => file not found."""
    with FTP() as f:
        f.connect(host, port, timeout=15)
        f.login()
        try:
            f.set_pasv(True)
        except Exception:
            pass
        buf: list[bytes] = []
        f.retrbinary("RETR " + remote, buf.append)
        return b"".join(buf).decode("utf-8", "replace")


def ftp_delete(host: str, port: int, remotes: list[str]) -> list[str]:
    done = []
    with FTP() as f:
        f.connect(host, port, timeout=15)
        f.login()
        try:
            f.set_pasv(True)
        except Exception:
            pass
        for r in remotes:
            try:
                f.sendcmd("DELE " + r)
                done.append(r)
            except error_perm:
                pass
    return done


class EvoLogGui:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("EVO log retrieval")
        self.root.geometry("960x620")
        self.q: queue.Queue = queue.Queue()
        self.tabs: dict[str, tk.Text] = {}
        self.vars: dict[str, tk.BooleanVar] = {}
        self._build()
        self.root.after(100, self._drain)

    # ---- layout -------------------------------------------------------------
    def _build(self) -> None:
        top = ttk.Frame(self.root, padding=8)
        top.pack(fill="x")

        ttk.Label(top, text="Console:").grid(row=0, column=0, sticky="w")
        self.host = tk.StringVar(value=read_env_host() or "192.168.0.15")
        ttk.Entry(top, textvariable=self.host, width=18).grid(row=0, column=1, padx=(4, 12))

        ttk.Label(top, text="FTP port:").grid(row=0, column=2, sticky="w")
        self.port = tk.StringVar(value=str(DEFAULT_FTP_PORT))
        ttk.Entry(top, textvariable=self.port, width=8).grid(row=0, column=3, padx=(4, 12))

        self.autosave = tk.BooleanVar(value=True)
        ttk.Checkbutton(top, text=f"Save copy to output/logs/", variable=self.autosave).grid(
            row=0, column=4, sticky="w", padx=(0, 12)
        )

        self.autorefresh = tk.BooleanVar(value=False)
        ttk.Checkbutton(top, text="Auto-refresh every", variable=self.autorefresh,
                        command=self._schedule_refresh).grid(row=0, column=5, sticky="e")
        self.interval = tk.StringVar(value="3")
        ttk.Entry(top, textvariable=self.interval, width=4).grid(row=0, column=6)
        ttk.Label(top, text="s").grid(row=0, column=7, sticky="w")

        body = ttk.Frame(self.root, padding=(8, 0, 8, 8))
        body.pack(fill="both", expand=True)

        left = ttk.LabelFrame(body, text="Files", padding=8)
        left.pack(side="left", fill="y")

        for path, label, default in KNOWN_LOGS:
            v = tk.BooleanVar(value=default)
            self.vars[path] = v
            ttk.Checkbutton(left, text=label, variable=v).pack(anchor="w")

        cust = ttk.Frame(left)
        cust.pack(anchor="w", fill="x", pady=(8, 0))
        ttk.Label(cust, text="Custom path:").pack(anchor="w")
        self.custom = tk.StringVar(value="")
        ttk.Entry(cust, textvariable=self.custom, width=30).pack(anchor="w")

        btns = ttk.Frame(left)
        btns.pack(anchor="w", pady=(10, 0), fill="x")
        self.fetch_btn = ttk.Button(btns, text="Fetch selected", command=self.fetch)
        self.fetch_btn.pack(fill="x")
        ttk.Button(btns, text="Select all", command=lambda: self._set_all(True)).pack(fill="x", pady=(4, 0))
        ttk.Button(btns, text="Select none", command=lambda: self._set_all(False)).pack(fill="x", pady=(4, 0))
        ttk.Separator(btns).pack(fill="x", pady=6)
        ttk.Button(btns, text="Delete selected on console", command=self.delete).pack(fill="x")

        right = ttk.Frame(body)
        right.pack(side="left", fill="both", expand=True, padx=(8, 0))
        self.nb = ttk.Notebook(right)
        self.nb.pack(fill="both", expand=True)

        self.status = tk.StringVar(value="Ready.")
        ttk.Label(self.root, textvariable=self.status, relief="sunken", anchor="w",
                  padding=(6, 3)).pack(fill="x", side="bottom")

    # ---- helpers ----------------------------------------------------------
    def _set_all(self, val: bool) -> None:
        for v in self.vars.values():
            v.set(val)

    def _selected(self) -> list[str]:
        out = [p for p, v in self.vars.items() if v.get()]
        c = self.custom.get().strip()
        if c:
            out.append(c)
        return out

    def _tab_for(self, path: str) -> tk.Text:
        if path in self.tabs:
            return self.tabs[path]
        frame = ttk.Frame(self.nb)
        txt = tk.Text(frame, wrap="none", font=("Consolas", 9), undo=False)
        ys = ttk.Scrollbar(frame, orient="vertical", command=txt.yview)
        xs = ttk.Scrollbar(frame, orient="horizontal", command=txt.xview)
        txt.configure(yscrollcommand=ys.set, xscrollcommand=xs.set)
        txt.grid(row=0, column=0, sticky="nsew")
        ys.grid(row=0, column=1, sticky="ns")
        xs.grid(row=1, column=0, sticky="ew")
        frame.rowconfigure(0, weight=1)
        frame.columnconfigure(0, weight=1)
        self.nb.add(frame, text=path.rsplit("/", 1)[-1])
        self.tabs[path] = txt
        return txt

    # ---- actions --------------------------------------------------------
    def fetch(self) -> None:
        paths = self._selected()
        if not paths:
            self.status.set("Nothing selected.")
            return
        try:
            host, port = self.host.get().strip(), int(self.port.get())
        except ValueError:
            messagebox.showerror("EVO log retrieval", "FTP port must be a number.")
            return
        self.fetch_btn.state(["disabled"])
        self.status.set(f"Fetching {len(paths)} file(s) from {host}:{port} ...")
        threading.Thread(target=self._fetch_worker, args=(host, port, paths), daemon=True).start()

    def _fetch_worker(self, host: str, port: int, paths: list[str]) -> None:
        results = []
        for p in paths:
            try:
                results.append((p, ftp_fetch(host, port, p), None))
            except error_perm:
                results.append((p, None, "not found on console"))
            except Exception as e:  # noqa: BLE001 - surface any transport error
                results.append((p, None, str(e)))
        self.q.put(("fetched", results))

    def delete(self) -> None:
        paths = [p for p, v in self.vars.items() if v.get()]
        c = self.custom.get().strip()
        if c:
            paths.append(c)
        if not paths:
            self.status.set("Nothing selected.")
            return
        if not messagebox.askyesno(
            "Delete on console",
            "Delete these files from the console over FTP?\n\n" + "\n".join(paths),
        ):
            return
        try:
            host, port = self.host.get().strip(), int(self.port.get())
        except ValueError:
            messagebox.showerror("EVO log retrieval", "FTP port must be a number.")
            return
        self.status.set("Deleting ...")
        threading.Thread(
            target=lambda: self.q.put(("deleted", self._safe(ftp_delete, host, port, paths))),
            daemon=True,
        ).start()

    @staticmethod
    def _safe(fn, *a):
        try:
            return fn(*a), None
        except Exception as e:  # noqa: BLE001
            return None, str(e)

    def _schedule_refresh(self) -> None:
        if not self.autorefresh.get():
            return
        try:
            secs = max(1, int(self.interval.get()))
        except ValueError:
            secs = 3
        self.root.after(secs * 1000, self._auto_tick)

    def _auto_tick(self) -> None:
        if not self.autorefresh.get():
            return
        if "disabled" not in self.fetch_btn.state():
            self.fetch()
        self._schedule_refresh()

    # ---- queue drain ----------------------------------------------------
    def _drain(self) -> None:
        try:
            while True:
                kind, payload = self.q.get_nowait()
                if kind == "fetched":
                    self._on_fetched(payload)
                elif kind == "deleted":
                    self._on_deleted(payload)
        except queue.Empty:
            pass
        self.root.after(100, self._drain)

    def _on_fetched(self, results) -> None:
        self.fetch_btn.state(["!disabled"])
        ok = 0
        saved = 0
        for path, text, err in results:
            txt = self._tab_for(path)
            txt.delete("1.0", "end")
            if err is not None:
                txt.insert("1.0", f"[{time.strftime('%H:%M:%S')}] {path}\n\n!! {err}\n")
                continue
            ok += 1
            stamp = time.strftime("%Y-%m-%d %H:%M:%S")
            txt.insert("1.0", f"# {path}  ({len(text)} bytes, fetched {stamp})\n"
                              f"{'-' * 60}\n{text}")
            txt.see("end")
            if self.autosave.get() and text:
                try:
                    LOG_OUT.mkdir(parents=True, exist_ok=True)
                    (LOG_OUT / path.rsplit("/", 1)[-1]).write_text(text, encoding="utf-8")
                    saved += 1
                except OSError:
                    pass
        tail = f"  ({saved} saved to output/logs/)" if saved else ""
        self.status.set(f"[{time.strftime('%H:%M:%S')}] fetched {ok}/{len(results)}{tail}")

    def _on_deleted(self, payload) -> None:
        done, err = payload
        if err:
            self.status.set(f"Delete failed: {err}")
        else:
            self.status.set(f"Deleted {len(done)} file(s): {', '.join(d.rsplit('/', 1)[-1] for d in done) or '(none matched)'}")


def main() -> None:
    root = tk.Tk()
    try:
        ttk.Style().theme_use("clam")
    except tk.TclError:
        pass
    EvoLogGui(root)
    root.mainloop()


if __name__ == "__main__":
    main()
