# The hardware cycle

How to put a build on the console, run it, and close it again without panicking
the machine. Every step that touches the console goes through `tools/evo-remote.sh`
or `scripts/deploy-app.sh`, which carry the safety rules below. The rules that
must never be broken are listed in the root [CLAUDE.md](../../CLAUDE.md).

```bash
# build with the dev remote (always, for testing)
docker compose run --rm ps5-dev ./scripts/package-app.sh --ffpfsc --usb-remote
# one step at a time; stop at the first anomaly; never auto-retry
timeout 100 ./tools/evo-remote.sh quit      # soft close; waits for parked=1
timeout 100 ./tools/evo-remote.sh close     # kills the (parked) app; waits for "released"
docker compose run --rm ps5-dev ./scripts/deploy-app.sh --ffpfsc    # sha256-verified
timeout 200 ./tools/evo-remote.sh launch    # waits for ShadowMount to go idle
```

The whole loop as one bounded, classified run: `evo-remote.sh cycle --secs 60`
([below](#cycle)). Driving the UI once it is up: [remote-control.md](remote-control.md).

## Why the order matters

**The app slot stays resident.** Launching again does not replace the running
instance; it adds one, each opening VideoOut, an audio port, the pad and decoder
threads. Stacked launches kernel-panicked the console on 2026-08-09 (about 50
minutes lost), and deploying a new image over a running EVO has done it too.

**Close the soft way first.** *QUIT EVO* (Settings → System & Diagnostics, or
`evo-remote.sh quit`) stops media, drains the GPU (`agc_wait_gpu_idle`, bounded at
500 ms), and parks the frame loop, after which `evo_status` carries `parked=1`.
`close` is then a kill, the same as a PS-button close, but of a quiescent GPU. The
PS button on a live EVO skips the drain: the kernel reclaims the VideoOut
registration and a 64 MB direct-memory mapping under a GPU that may still be
executing a submitted command buffer, and that panicked the console on 2026-09-18.
So:

- `close` is refused unless `evo_status` says `parked=1` (`--force` only when there
  is no heartbeat to read, and never while `t=` advances).
- `launch` is refused while ShadowMount shows the title started and not released,
  or while the heartbeat moves.
- `deploy-app.sh` refuses while EVO is resident ([../build/packaging.md](../build/packaging.md#deploy-appsh)).
- A **crashed** EVO is already closed: no switcher close is needed before the next
  deploy. A frozen `evo.log` proves nothing about exit (the idle heartbeat is about
  six minutes apart); never force past the interlock on that basis.

**ShadowMount+ v1.7 does not auto-launch an updated image.** It test-mounts the new
image, logs `unmount complete: source=/data/homebrew/PPSA99039.ffpfsc`, and
unmounts it. Launch from the Games row or with `evo-remote.sh launch`. Launching
right after a deploy, while ShadowMount is still scanning, gives "Can't start game
or app"; `launch` waits for its log to be quiet for 15 s for that reason.

**The truth about lifecycle is ShadowMount's `/data/shadowmount/debug.log`**, not an
exit code. `tools/evo_lifecycle.py` reads it (and EVO's heartbeat) over FTP:

| Line | Means |
|---|---|
| `NOTIFY: installed game` / `[LINK] mount.lnk created` | registered |
| `[IMG][LVD] unmount complete: source=/data/homebrew/PPSA99039.ffpfsc` | an updated image was verified (v1.7) |
| `[GAME] started: PPSA99039` | launched |
| `[LINK] runtime layers released: PPSA99039` | process gone, slot free |

and, from `evo_status`: `t=` advancing means running; `parked=1` is written once,
as the soft close parks.

## The controllers

`launch` and `close` send two tiny ELF payloads from `tools/ps5-controllers/` to
elfldr on `:9021`, with the title id compiled in (vendored GPL-3.0 from
`ps5-homebrew-dev-protocol`). `launch.c` calls `sceSystemServiceLaunchApp` for the
foreground user; `close.c` kills the foreground app only if its title is
`PPSA99039`. **Never send them by hand.** A zero exit only means elfldr took the
bytes. The harmless `getAppStatus: LNC_ISOK::0x80940004` line is echoed
SceLncUtil chatter, not a failure; the old `0x80940005` came from hbldr. The
`launchApp: LNC_ISOK::0x...` line is the system's real answer, and a launch that
never reaches `[GAME] started` quotes it in the error. `0x80940033` with no
`[SHELLCORE]` line for EVO in ShadowMount's log means ShadowMount's launch hook
is not live, even though its log still shows scans: run ShadowMount+ again from
the payload list, then `launch` works (2026-11-04).

A `timeout` or `curl` timeout (exit 124 / 28) on an `evo-remote.sh` call is a
normal, successful outcome: the far end holds the socket open. Bound every
console-facing call with one, because a resident payload never closes elfldr's
socket and an unbounded call hangs.

## Cycle

```bash
docker compose run --rm ps5-dev ./tools/evo-remote.sh cycle --secs 60
docker compose run --rm ps5-dev ./tools/evo-remote.sh cycle --no-build --play /mnt/usb0/media/gta.mp4
```

One case is one changed variable, one frozen candidate, one bounded console cycle
and one classified result (the method of `ps5-homebrew-dev-protocol`). In order, and
stopping at the first anomaly:

1. **Preflight**: `:2121`, `:3232` and `:9021` must answer; a dirty tree is
   flagged `+dirty`.
2. **Free the slot**: if resident, `quit` then `close`; no heartbeat is `no-run`.
3. **Package** `--ffpfsc --usb-remote` and record the sha256.
4. **Deploy**: read back and compared before the rename; a mismatch is
   `transport-failure`. ShadowMount must log the registration within 90 s.
5. **Observe**: klog starts recording first; `launch` is sent if `[GAME] started`
   does not appear within 40 s; then `--play`, the `--secs` window, and pull
   `evo.log` and `evo_status`.
6. **Close**: `quit`, parked, `close`, released.
7. **Classify** as exactly one of: `pass`; `partial-pass` (started but did not
   park); `failed` (never started, a crash in klog, `fatal=1`, no release);
   `inconclusive` (started, then the heartbeat vanished); `transport-failure`;
   `no-run`.

Evidence goes to `output/cycles/<stamp>/` (`result.json`, `klog.log`, `evo.log`,
`evo_status.txt`, the ShadowMount slice, `sha256.txt`, step logs) and one line is
appended to `output/cycles/ledger.txt`. Paste a proven milestone's ledger line into
the issue; do not paste raw logs.

`evo-remote.sh verify` is the same cycle ending in a feature check instead of a
timed wait ([../build/validation.md](../build/validation.md#hardware-checks)).

Protocol rules on top: run cycles one at a time (two runners fighting over
`evo_cmd` once corrupted a whole run, and stopping a backgrounded `docker compose
run` does not stop its container, so check `docker ps`); after any `failed`,
`inconclusive` or `transport-failure`, look at the console before the next run;
after a suspected kernel panic, analyse the evidence offline and do not rerun;
never touch the console's own Settings and never approve a system update.

## Services the console must be running

All need the jailbreak re-run after every console reboot. A port that pings but
refuses connections almost always means the jailbreak lapsed, not that a tool is
wrong ([networking.md](networking.md)).

| Task | Needs |
|---|---|
| `deploy-app.sh`, `evo-remote.sh` (everything over FTP) | `ps5-payload-ftpsrv` on 2121 |
| mounting and launching the image | ShadowMountPlus |
| `launch`, `close` | `ps5-payload-elfldr` on 9021 |
| `klog.sh` | `ps5-payload-klogsrv` on 3232 |
| `shot.sh` | `ps5-payload-websrv` on 8080 (the `/fs` route is read-only: delete over FTP) |
| the dashboard, log stream, phone input | EVO itself, port 9780 by default (user-settable, dev tools accept `PS5_LOG_PORT`; [diagnostics.md](diagnostics.md)) |

Do not kill `kstuff`, do not sweep kernel `.text`, and never call `sceVideoOutOpen`
from a payload; each has panicked the console. Remote Play is an owner-initiated
tool ([remote-play.md](remote-play.md)); do not start a stream the owner did not ask
for, and close EVO before sending any payload.
