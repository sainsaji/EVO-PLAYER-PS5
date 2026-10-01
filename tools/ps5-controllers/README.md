# Title-aware launch / close controllers

Vendored verbatim from
[ps5-homebrew-dev-protocol](https://github.com/blackbearreloaded/ps5-homebrew-dev-protocol)
(`scripts/controllers/`, commit `51dc332`), GPL-3.0-or-later. `COPYING` is
that licence. They are standalone payloads, built and sent by
`tools/evo-remote.sh`, and never linked into EVO.

| File | Calls | Does |
| --- | --- | --- |
| `launch.c` | `sceUserServiceGetForegroundUser`, `sceSystemServiceLaunchApp` | Launches one title for the foreground user |
| `close.c` | `sceLncUtilGetAppIdOfRunningBigApp` + `sceLncUtilGetAppTitleId` + `sceLncUtilKillApp` (fallback `sceSystemServiceKillApp`) | Kills the foreground app **only if** its title ID matches |

The title ID is compiled in (`-DBOOTSTRAP_TITLE_ID="PPSA99039"`), so each ELF
does exactly one thing to exactly one title. They go to elfldr on `:9021`.

**Never call these directly. Use `tools/evo-remote.sh launch|quit|close|cycle`**,
which guard them:

- `close` is a kill, the same thing a PS-button close does. That panicked the
  console on 2026-09-18 when EVO was still submitting GPU work. `evo-remote.sh
  close` only sends it once `evo_status` says `parked=1`, which means the soft
  close (`quit`) has drained the GPU and stopped the frame loop.
- `launch` is refused while ShadowMount's log shows PPSA99039 started and not
  yet released, and while the `evo_status` heartbeat is advancing. That keeps
  it from stacking a launch.

A zero exit from the send only means elfldr took the bytes. The result comes
from ShadowMount's `/data/shadowmount/debug.log`: `[GAME] started: PPSA99039`
for launch, and `[LINK] runtime layers released: PPSA99039` for close.

Hardware-verified on 2026-10-01: both controllers work from elfldr on this
console (PS5 Pro, FW 12.70, ShadowMount+ v1.7beta2). The old `0x80940005`
came from an hbldr payload. Both print a harmless
`getAppStatus: LNC_ISOK::0x80940004` line, which elfldr echoes back.
