#!/usr/bin/env python3
"""
evo-panel.py - a Tkinter control panel for EVO Player's dev workflow.

One window over the scripts in scripts/ and tools/: package the app module with
any flag combination, deploy it, run the host compile check, drive the console
over the FTP dev remote, pull /mnt/usb0 logs, render the UI on the host, watch
klog, grab screenshots. Every button just shells out to the same script you'd
run by hand (`bash scripts/package-app.sh --ffpfsc` ...) and streams its output
into the console pane - the scripts re-exec themselves through
`docker compose run ps5-dev`, so nothing here needs the container directly.

Stdlib only (tkinter + ftplib). Run on the host:

    python tools/evo-panel.py

Host / FTP port default from .env (PS5_HOST=...) at the repo root.
One command runs at a time; Stop terminates it (and the docker run it spawned).
"""

from __future__ import annotations

import os
import queue
import shutil
import signal
import subprocess
import threading
import time
from ftplib import FTP, error_perm
from pathlib import Path

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

try:  # crisper text on Windows HiDPI; harmless elsewhere
    import ctypes

    ctypes.windll.shcore.SetProcessDpiAwareness(2)  # type: ignore[attr-defined]
except Exception:
    pass

REPO_ROOT = Path(__file__).resolve().parent.parent
LOG_OUT = REPO_ROOT / "output" / "logs"
DEFAULT_FTP_PORT = 2121
BASH = shutil.which("bash") or "bash"

# ---- /mnt/usb0 logs EVO writes (see the code inventory) --------------------
KNOWN_LOGS = [
    ("/mnt/usb0/evo_boot.log", "evo_boot.log  (boot trace / probes)", True),
    ("/mnt/usb0/pp_4k_stage_breadcrumb.txt", "pp_4k_stage_breadcrumb.txt", True),
    ("/mnt/usb0/pp_4k_stage_last.txt", "pp_4k_stage_last.txt", True),
    ("/mnt/usb0/pp_playback_stats.txt", "pp_playback_stats.txt", True),
    ("/mnt/usb0/evo_status", "evo_status  (--usb-remote)", False),
    ("/mnt/usb0/evo_vdec.log", "evo_vdec.log  (--usb-remote)", False),
    ("/mnt/usb0/evo_vo_debug.log", "evo_vo_debug.log  (-DEVO_VO_DEBUG)", False),
    ("/mnt/usb0/evo_compat_report.txt", "evo_compat_report.txt", False),
]

# ---- package-app.sh flags (case block in the script) ----------------------
PACKAGE_FLAGS = [
    ("--ffpfsc", "PFS image (.ffpfsc) - the hardware path", True),
    ("--usb-remote", "scriptable FTP remote + verbose vdec log", False),
    ("--agc-probe", "boot-time sceAgc reachability recon (#27)", False),
    ("--videodec2-probe", "sceVideodec2 gate (#31)", False),
    ("--avplayer-probe", "libSceAvPlayer gate (Route A - dead)", False),
    ("--geo-text", "compile the GPU text 2nd-pass (#28/#67)", False),
    ("--shader-scan", "rip PSSL shader blobs -> /mnt/usb0 (#67)", False),
    ("--breadcrumbs", "on-screen boot-trace popups (#51)", False),
    ("--rebuild-libc", "force-regenerate the runtime libc shim", False),
]


def read_env_host() -> str:
    env = REPO_ROOT / ".env"
    if env.is_file():
        for line in env.read_text(encoding="utf-8", errors="replace").splitlines():
            s = line.strip()
            if s.startswith("PS5_HOST=") and not s.split("=", 1)[0].strip().startswith("#"):
                v = s.split("=", 1)[1].split("#", 1)[0].strip()
                if v:
                    return v
    return ""


# ======================================================================
#  subprocess runner - one at a time, streamed
# ======================================================================
class Runner:
    def __init__(self, on_line, on_done):
        self.on_line = on_line
        self.on_done = on_done
        self.proc: subprocess.Popen | None = None
        self._lock = threading.Lock()

    @property
    def busy(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def start(self, argv: list[str], env_extra: dict[str, str], title: str) -> bool:
        with self._lock:
            if self.busy:
                return False
            env = os.environ.copy()
            env.update({k: v for k, v in env_extra.items() if v})
            try:
                self.proc = subprocess.Popen(
                    argv,
                    cwd=str(REPO_ROOT),
                    env=env,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    stdin=subprocess.DEVNULL,
                    text=True,
                    bufsize=1,
                    creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0),
                )
            except OSError as e:
                self.on_line(f"!! cannot run {argv[0]}: {e}\n")
                return False
        threading.Thread(target=self._pump, args=(title,), daemon=True).start()
        return True

    def _pump(self, title: str) -> None:
        assert self.proc and self.proc.stdout
        for line in self.proc.stdout:
            self.on_line(line)
        rc = self.proc.wait()
        self.on_done(title, rc)
        self.proc = None

    def stop(self) -> None:
        p = self.proc
        if not p or p.poll() is not None:
            return
        try:
            if os.name == "nt":
                p.send_signal(signal.CTRL_BREAK_EVENT)
                time.sleep(0.4)
            if p.poll() is None:
                p.terminate()
            time.sleep(0.3)
            if p.poll() is None:
                p.kill()
        except Exception:
            pass


# ======================================================================
#  FTP helpers (log pull) - pure python, no subprocess
# ======================================================================
def ftp_fetch(host: str, port: int, remote: str) -> str:
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


# ======================================================================
#  the panel
# ======================================================================
class EvoPanel:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("EVO control panel")
        self.root.geometry("1040x900")
        self.root.minsize(900, 640)
        self.q: queue.Queue = queue.Queue()
        self.runner = Runner(
            lambda ln: self.q.put(("out", ln)),
            lambda t, rc: self.q.put(("done", (t, rc))),
        )
        self.log_vars: dict[str, tk.BooleanVar] = {}
        self.pkg_vars: dict[str, tk.BooleanVar] = {}
        self._build()
        self.root.after(80, self._drain)

    # ---- layout --------------------------------------------------------
    def _build(self) -> None:
        bar = ttk.Frame(self.root, padding=(8, 6))
        bar.pack(fill="x")
        ttk.Label(bar, text="PS5 host").pack(side="left")
        self.host = tk.StringVar(value=read_env_host() or "192.168.0.15")
        ttk.Entry(bar, textvariable=self.host, width=16).pack(side="left", padx=(4, 12))
        ttk.Label(bar, text="FTP port").pack(side="left")
        self.port = tk.StringVar(value=str(DEFAULT_FTP_PORT))
        ttk.Entry(bar, textvariable=self.port, width=7).pack(side="left", padx=(4, 12))
        self.running_lbl = ttk.Label(bar, text="idle", foreground="#666")
        self.running_lbl.pack(side="left")
        self.stop_btn = ttk.Button(bar, text="Stop", width=8, command=self.runner.stop, state="disabled")
        self.stop_btn.pack(side="right")

        nb = ttk.Notebook(self.root)
        nb.pack(fill="both", expand=True, padx=8)
        self.nb = nb
        self._tab_build(nb)
        self._tab_remote(nb)
        self._tab_logs(nb)
        self._tab_ui(nb)
        self._tab_klog(nb)
        self._tab_tools(nb)

        out = ttk.LabelFrame(self.root, text="Console", padding=4)
        out.pack(fill="both", padx=8, pady=(4, 8))
        self.console = tk.Text(out, height=11, wrap="word", font=("Consolas", 9),
                               background="#1e1e1e", foreground="#d4d4d4", insertbackground="#d4d4d4")
        ys = ttk.Scrollbar(out, orient="vertical", command=self.console.yview)
        self.console.configure(yscrollcommand=ys.set)
        self.console.grid(row=0, column=0, sticky="nsew")
        ys.grid(row=0, column=1, sticky="ns")
        row = ttk.Frame(out)
        row.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(4, 0))
        ttk.Button(row, text="Clear", command=lambda: self.console.delete("1.0", "end")).pack(side="right")
        self.autoscroll = tk.BooleanVar(value=True)
        ttk.Checkbutton(row, text="autoscroll", variable=self.autoscroll).pack(side="right", padx=8)
        out.rowconfigure(0, weight=1)
        out.columnconfigure(0, weight=1)

        self.status = tk.StringVar(value="Ready.  bash: " + BASH)
        ttk.Label(self.root, textvariable=self.status, relief="sunken", anchor="w",
                  padding=(6, 3)).pack(fill="x", side="bottom")

    def _section(self, parent, text):
        f = ttk.LabelFrame(parent, text=text, padding=8)
        f.pack(fill="x", pady=4, padx=2)
        return f

    # ---- Build + Deploy ----------------------------------------------
    def _tab_build(self, nb):
        t = ttk.Frame(nb, padding=6)
        nb.add(t, text="Build + Deploy")

        pk = self._section(t, "Package app module  ·  scripts/package-app.sh")
        self.pkg_mode = tk.StringVar(value="player")
        mrow = ttk.Frame(pk)
        mrow.pack(anchor="w")
        ttk.Label(mrow, text="mode:").pack(side="left")
        ttk.Radiobutton(mrow, text="player", value="player", variable=self.pkg_mode).pack(side="left")
        ttk.Radiobutton(mrow, text="probe", value="probe", variable=self.pkg_mode).pack(side="left")
        grid = ttk.Frame(pk)
        grid.pack(anchor="w", pady=(4, 0))
        for i, (flag, desc, default) in enumerate(PACKAGE_FLAGS):
            v = tk.BooleanVar(value=default)
            self.pkg_vars[flag] = v
            ttk.Checkbutton(grid, text=f"{flag}  —  {desc}", variable=v).grid(
                row=i, column=0, sticky="w")
        brow = ttk.Frame(pk)
        brow.pack(anchor="w", pady=(6, 0))
        ttk.Button(brow, text="Package", command=self._do_package).pack(side="left")
        ttk.Button(brow, text="Package + Deploy", command=self._do_package_deploy).pack(side="left", padx=6)
        self.cmd_preview = ttk.Label(pk, text="", foreground="#888", font=("Consolas", 8))
        self.cmd_preview.pack(anchor="w", pady=(4, 0))
        for v in self.pkg_vars.values():
            v.trace_add("write", lambda *_: self._refresh_cmd_preview())
        self.pkg_mode.trace_add("write", lambda *_: self._refresh_cmd_preview())
        self._refresh_cmd_preview()

        dp = self._section(t, "Deploy  ·  scripts/deploy-app.sh  (FTP → /data/homebrew/PPSA99039)")
        self.deploy_ffpfsc = tk.BooleanVar(value=True)
        ttk.Checkbutton(dp, text="--ffpfsc  (upload the PFS image)", variable=self.deploy_ffpfsc).pack(anchor="w")
        drow = ttk.Frame(dp)
        drow.pack(anchor="w", pady=(4, 0))
        ttk.Button(drow, text="Deploy", command=lambda: self._run_deploy(False)).pack(side="left")
        ttk.Button(drow, text="Undeploy", command=lambda: self._run_deploy(True)).pack(side="left", padx=6)
        ttk.Label(dp, text="ShadowMount+ re-mounts + auto-launches on the .ffpfsc change; "
                           "PS-button-close a running EVO first.",
                  foreground="#888", wraplength=760).pack(anchor="w", pady=(4, 0))

        cc = self._section(t, "Host compile check  ·  scripts/build-evoplayer.sh  (never deploys)")
        srow = ttk.Frame(cc)
        srow.pack(anchor="w")
        ttk.Label(srow, text="--stage (optional):").pack(side="left")
        self.build_stage = tk.StringVar(value="")
        ttk.Entry(srow, textvariable=self.build_stage, width=6).pack(side="left", padx=4)
        ttk.Button(srow, text="Compile check", command=self._do_compile_check).pack(side="left", padx=6)

    def _pkg_argv(self) -> list[str]:
        argv = [BASH, "scripts/package-app.sh"]
        if self.pkg_mode.get() == "probe":
            argv.append("--probe")
        argv += [f for f, v in self.pkg_vars.items() if v.get()]
        return argv

    def _refresh_cmd_preview(self) -> None:
        self.cmd_preview.config(text="$ " + " ".join(self._pkg_argv()[1:]))

    def _do_package(self):
        self.run(self._pkg_argv(), "package-app")

    def _do_package_deploy(self):
        pkg = " ".join(self._pkg_argv()[1:])
        dep = "./scripts/deploy-app.sh" + (" --ffpfsc" if self.deploy_ffpfsc.get() else "")
        self.run([BASH, "-lc", f"./{pkg} && {dep}"], "package + deploy")

    def _run_deploy(self, undeploy: bool):
        argv = [BASH, "scripts/deploy-app.sh"]
        if undeploy:
            argv.append("--undeploy")
        if self.deploy_ffpfsc.get():
            argv.append("--ffpfsc")
        self.run(argv, "undeploy" if undeploy else "deploy")

    def _do_compile_check(self):
        argv = [BASH, "scripts/build-evoplayer.sh"]
        if self.build_stage.get().strip():
            argv += ["--stage", self.build_stage.get().strip()]
        self.run(argv, "compile check")

    # ---- Remote (evo-remote.sh) ------------------------------------
    def _tab_remote(self, nb):
        t = ttk.Frame(nb, padding=6)
        nb.add(t, text="Remote")
        s = self._section(t, "FTP dev remote  ·  tools/evo-remote.sh  (needs a --usb-remote build)")

        pr = ttk.Frame(s)
        pr.pack(anchor="w", pady=2)
        ttk.Button(pr, text="play", width=8,
                   command=lambda: self._remote("play", self.play_path.get())).pack(side="left")
        self.play_path = tk.StringVar(value="/mnt/usb0/")
        ttk.Entry(pr, textvariable=self.play_path, width=48).pack(side="left", padx=4)

        sr = ttk.Frame(s)
        sr.pack(anchor="w", pady=2)
        ttk.Button(sr, text="seek", width=8,
                   command=lambda: self._remote("seek", self.seek_val.get())).pack(side="left")
        self.seek_val = tk.StringVar(value="+30")
        ttk.Entry(sr, textvariable=self.seek_val, width=10).pack(side="left", padx=4)
        ttk.Label(sr, text="seconds, or +N / -N", foreground="#888").pack(side="left")

        gr = ttk.Frame(s)
        gr.pack(anchor="w", pady=(6, 0))
        for sub in ("status", "boot", "log", "watch", "clear", "build", "kill"):
            ttk.Button(gr, text=sub, width=8,
                       command=lambda x=sub: self._remote(x)).pack(side="left", padx=2)
        ttk.Label(s, text="watch streams until you press Stop.  build = package --usb-remote + deploy.",
                  foreground="#888").pack(anchor="w", pady=(4, 0))

    def _remote(self, sub: str, arg: str = ""):
        argv = [BASH, "tools/evo-remote.sh", sub]
        if arg.strip():
            argv.append(arg.strip())
        elif sub in ("play", "seek"):
            messagebox.showinfo("evo-remote", f"{sub} needs a value.")
            return
        self.run(argv, f"evo-remote {sub}")

    # ---- Logs (FTP pull) ------------------------------------------
    def _tab_logs(self, nb):
        t = ttk.Frame(nb, padding=6)
        nb.add(t, text="Logs")
        left = ttk.LabelFrame(t, text="Pull from /mnt/usb0 over FTP", padding=8)
        left.pack(side="left", fill="y")
        for path, label, default in KNOWN_LOGS:
            v = tk.BooleanVar(value=default)
            self.log_vars[path] = v
            ttk.Checkbutton(left, text=label, variable=v).pack(anchor="w")
        self.log_custom = tk.StringVar(value="")
        cf = ttk.Frame(left)
        cf.pack(anchor="w", fill="x", pady=(6, 0))
        ttk.Label(cf, text="custom path:").pack(anchor="w")
        ttk.Entry(cf, textvariable=self.log_custom, width=30).pack(anchor="w")
        self.log_save = tk.BooleanVar(value=True)
        ttk.Checkbutton(left, text="save copy to output/logs/", variable=self.log_save).pack(anchor="w", pady=(6, 0))
        bf = ttk.Frame(left)
        bf.pack(anchor="w", pady=(8, 0), fill="x")
        ttk.Button(bf, text="Fetch selected", command=self._logs_fetch).pack(fill="x")
        ttk.Button(bf, text="Delete selected on console", command=self._logs_delete).pack(fill="x", pady=(4, 0))

        right = ttk.Frame(t)
        right.pack(side="left", fill="both", expand=True, padx=(8, 0))
        self.log_view = tk.Text(right, wrap="none", font=("Consolas", 9))
        lys = ttk.Scrollbar(right, orient="vertical", command=self.log_view.yview)
        self.log_view.configure(yscrollcommand=lys.set)
        self.log_view.pack(side="left", fill="both", expand=True)
        lys.pack(side="left", fill="y")

    def _sel_logs(self) -> list[str]:
        out = [p for p, v in self.log_vars.items() if v.get()]
        c = self.log_custom.get().strip()
        if c:
            out.append(c)
        return out

    def _logs_fetch(self):
        paths = self._sel_logs()
        if not paths:
            self.status.set("Nothing selected.")
            return
        try:
            host, port = self.host.get().strip(), int(self.port.get())
        except ValueError:
            messagebox.showerror("Logs", "FTP port must be a number.")
            return
        self.status.set(f"Fetching {len(paths)} file(s) ...")

        def work():
            chunks, saved = [], 0
            for p in paths:
                try:
                    text = ftp_fetch(host, port, p)
                    chunks.append(f"# {p}  ({len(text)} bytes)\n{'-'*60}\n{text}\n\n")
                    if self.log_save.get() and text:
                        LOG_OUT.mkdir(parents=True, exist_ok=True)
                        (LOG_OUT / p.rsplit('/', 1)[-1]).write_text(text, encoding="utf-8")
                        saved += 1
                except error_perm:
                    chunks.append(f"# {p}\n!! not found on console\n\n")
                except Exception as e:
                    chunks.append(f"# {p}\n!! {e}\n\n")
            self.q.put(("logs", ("".join(chunks), f"fetched {len(paths)}"
                                 + (f", {saved} saved" if saved else ""))))

        threading.Thread(target=work, daemon=True).start()

    def _logs_delete(self):
        paths = [p for p, v in self.log_vars.items() if v.get()]
        c = self.log_custom.get().strip()
        if c:
            paths.append(c)
        if not paths or not messagebox.askyesno("Delete on console",
                                                "Delete over FTP?\n\n" + "\n".join(paths)):
            return
        try:
            host, port = self.host.get().strip(), int(self.port.get())
        except ValueError:
            return
        threading.Thread(target=lambda: self.q.put(
            ("logs", (f"deleted: {ftp_delete(host, port, paths)}", "delete done"))), daemon=True).start()

    # ---- UI + bench ---------------------------------------------
    def _tab_ui(self, nb):
        t = ttk.Frame(nb, padding=6)
        nb.add(t, text="UI + Bench")
        s = self._section(t, "Render the UI on the host  (no console)")
        r = ttk.Frame(s)
        r.pack(anchor="w")
        ttk.Button(r, text="uiview --all", width=16,
                   command=lambda: self.run([BASH, "tools/uiview.sh", "--all"], "uiview")).pack(side="left")
        ttk.Button(r, text="uiplay (contact sheet)", width=22,
                   command=lambda: self.run([BASH, "tools/uiplay.sh"], "uiplay")).pack(side="left", padx=6)
        ttk.Label(s, text="→ output/uiview/rml_*.png   ·   output/uiplay/index.html",
                  foreground="#888").pack(anchor="w", pady=(4, 0))

        p = self._section(t, "Profilers / benchmarks")
        pr = ttk.Frame(p)
        pr.pack(anchor="w")
        ttk.Button(pr, text="prof_rmlui", width=16,
                   command=lambda: self.run([BASH, "tools/prof_rmlui.sh"], "prof_rmlui")).pack(side="left")
        ttk.Button(pr, text="bench.sh", width=12,
                   command=lambda: self.run(
                       [BASH, "tools/bench.sh"] + ([self.bench_arg.get().strip()] if self.bench_arg.get().strip() else []),
                       "bench")).pack(side="left", padx=6)
        self.bench_arg = tk.StringVar(value="")
        ttk.Entry(pr, textvariable=self.bench_arg, width=12).pack(side="left")
        ttk.Label(pr, text="iterations, or --asan / --tsan", foreground="#888").pack(side="left", padx=4)

    # ---- klog -------------------------------------------------
    def _tab_klog(self, nb):
        t = ttk.Frame(nb, padding=6)
        nb.add(t, text="klog")
        s = self._section(t, "Console kernel log  ·  tools/klog.sh")
        r1 = ttk.Frame(s)
        r1.pack(anchor="w", pady=2)
        ttk.Button(r1, text="follow", width=10,
                   command=lambda: self._klog([])).pack(side="left")
        ttk.Button(r1, text="follow --grep", width=14,
                   command=lambda: self._klog(["--grep", self.klog_grep.get().strip()] if self.klog_grep.get().strip() else [])).pack(side="left", padx=4)
        self.klog_grep = tk.StringVar(value="evo")
        ttk.Entry(r1, textvariable=self.klog_grep, width=16).pack(side="left")
        r2 = ttk.Frame(s)
        r2.pack(anchor="w", pady=2)
        ttk.Button(r2, text="--once", width=10, command=lambda: self._klog(["--once"])).pack(side="left")
        ttk.Button(r2, text="--tail", width=10,
                   command=lambda: self._klog(["--tail", self.klog_tail.get().strip() or "200"])).pack(side="left", padx=4)
        self.klog_tail = tk.StringVar(value="200")
        ttk.Entry(r2, textvariable=self.klog_tail, width=6).pack(side="left")
        ttk.Button(r2, text="--sessions", width=12, command=lambda: self._klog(["--sessions"])).pack(side="left", padx=4)
        ttk.Label(s, text="follow streams until Stop.", foreground="#888").pack(anchor="w", pady=(4, 0))

    def _klog(self, args):
        self.run([BASH, "tools/klog.sh"] + args, "klog " + (" ".join(args) or "follow"))

    # ---- Tools --------------------------------------------------
    def _tab_tools(self, nb):
        t = ttk.Frame(nb, padding=6)
        nb.add(t, text="Tools")

        sh = self._section(t, "Screenshots  ·  tools/shot.sh")
        r = ttk.Frame(sh)
        r.pack(anchor="w")
        ttk.Button(r, text="grab", width=8, command=lambda: self.run([BASH, "tools/shot.sh", "grab"], "shot grab")).pack(side="left")
        ttk.Button(r, text="grab --full", width=12, command=lambda: self.run([BASH, "tools/shot.sh", "grab", "--full"], "shot grab --full")).pack(side="left", padx=4)
        ttk.Button(r, text="list", width=8, command=lambda: self.run([BASH, "tools/shot.sh", "list"], "shot list")).pack(side="left", padx=4)
        ttk.Button(r, text="clean", width=8, command=lambda: self.run([BASH, "tools/shot.sh", "clean"], "shot clean")).pack(side="left", padx=4)
        ttk.Label(sh, text="→ output/screenshots/latest.png", foreground="#888").pack(anchor="w", pady=(4, 0))

        jb = self._section(t, "Sandbox / assets")
        r2 = ttk.Frame(jb)
        r2.pack(anchor="w")
        ttk.Button(r2, text="sandbox-unjail", width=16,
                   command=lambda: self.run([BASH, "tools/sandbox-unjail.sh"], "sandbox-unjail")).pack(side="left")
        ttk.Button(r2, text="bundle RmlUi assets", width=20,
                   command=lambda: self.run([BASH, "-lc", "python3 tools/bundle_rml_assets.py"], "bundle assets")).pack(side="left", padx=6)
        ttk.Button(r2, text="gen icons", width=12,
                   command=lambda: self.run([BASH, "-lc", "python3 tools/gen_icons.py"], "gen icons")).pack(side="left")

        pp = self._section(t, "Legacy PKG  ·  scripts/package-pkg.sh")
        r3 = ttk.Frame(pp)
        r3.pack(anchor="w")
        ttk.Button(r3, text="pick ELF + package (homebrew)", command=self._package_pkg).pack(side="left")
        ttk.Label(pp, text="app-module .ffpfsc is the release path; this is for the old ELF route.",
                  foreground="#888").pack(anchor="w", pady=(4, 0))

    def _package_pkg(self):
        elf = filedialog.askopenfilename(title="ELF to package", initialdir=str(REPO_ROOT / "output" / "elf"),
                                         filetypes=[("ELF", "*.elf"), ("all", "*.*")])
        if not elf:
            return
        self.run([BASH, "scripts/package-pkg.sh", "--format", "homebrew", elf], "package-pkg")

    # ---- run + drain --------------------------------------------
    def run(self, argv: list[str], title: str) -> None:
        if self.runner.busy:
            messagebox.showinfo("Busy", "A command is already running. Stop it first.")
            return
        env = {"PS5_HOST": self.host.get().strip(), "PS5_PORT": "9021",
               "FTP_PORT": self.port.get().strip()}
        if len(argv) >= 3 and argv[1] in ("-lc", "-c"):
            shown = argv[2]
        elif argv and argv[0] == BASH:
            shown = " ".join(argv[1:])
        else:
            shown = " ".join(argv)
        self._log(f"\n$ {shown}\n", "cmd")
        if self.runner.start(argv, env, title):
            self.stop_btn.config(state="normal")
            self.running_lbl.config(text=f"running: {title}", foreground="#c60")
            self.status.set(f"running: {title}")

    def _log(self, text: str, tag: str | None = None) -> None:
        self.console.insert("end", text, tag or ())
        if self.autoscroll.get():
            self.console.see("end")

    def _drain(self) -> None:
        try:
            while True:
                kind, payload = self.q.get_nowait()
                if kind == "out":
                    self._log(payload)
                elif kind == "done":
                    title, rc = payload
                    self._log(f"[{title}] exit {rc}\n", "cmd")
                    self.stop_btn.config(state="disabled")
                    self.running_lbl.config(text="idle", foreground="#666")
                    self.status.set(f"{title}: {'ok' if rc == 0 else f'exit {rc}'}")
                elif kind == "logs":
                    text, msg = payload
                    self.log_view.delete("1.0", "end")
                    self.log_view.insert("1.0", text)
                    self.status.set(msg)
        except queue.Empty:
            pass
        self.root.after(80, self._drain)


def build_app(root: tk.Tk) -> EvoPanel:
    try:
        ttk.Style().theme_use("clam")
    except tk.TclError:
        pass
    panel = EvoPanel(root)
    panel.console.tag_configure("cmd", foreground="#4ec9b0")
    return panel


def main() -> None:
    root = tk.Tk()
    build_app(root)
    root.mainloop()


if __name__ == "__main__":
    main()
