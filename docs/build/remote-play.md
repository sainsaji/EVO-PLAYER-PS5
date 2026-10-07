# PS Remote Play from the PC (chiaki-ng)

**Status (2026-10-07): WORKS. The console paired with chiaki-ng and streamed.**
Done with the account already offline-activated, `websrv` v0.34, **LinkDev**
(not the ptrace `rp-get-pin` payload) and chiaki-ng v1.10.0. See the results
table and the "What went wrong" notes at the end.

## Why it was thought impossible, and what changed

An earlier conclusion (2026-10-01 / 10-03) was that a jailbroken PS5 cannot pair.
It was never tested, and the reason given - Remote Play "needs PSN on the console"
- was a guess. The owner says PSN is not needed, and the guide
[How to use Remote Play on a Jailbroken PS5!](https://www.youtube.com/watch?v=8ijpU4-Qwz4)
(MODDED WARFARE, 2024-12-12) shows why: on an **offline-activated** account the
normal Settings -> Remote Play -> *Link Device* PIN screen is blocked in ShellUI,
but the function that makes the PIN, `sceRemoteplayGeneratePinCode`, works
without patching. A small payload calls it and prints the PIN and the account ID.
Its author says a real PSN id is only needed for Sony's official Remote Play
app, not for chiaki / chiaki-ng. (Read from the video description and the
payload's README; the video itself and its captions could not be read.)

Also true: another homebrew developer's workflow
([third_party/ps5-homebrew-dev-protocol/docs/CHIAKI_INPUT.md](../../third_party/ps5-homebrew-dev-protocol/docs/CHIAKI_INPUT.md))
uses a configured chiaki-ng profile against a jailbroken PS5 routinely.

## What it would give us

`evo-remote.sh key` injects into EVO's own pad state and `key l3` captures only
EVO's framebuffer, so neither reaches or shows a system dialog (the native IME,
the web browser dialog). A Remote Play session shows the real screen and sends
real pad input, which covers exactly those two gaps.

## PC side (done)

| | |
|---|---|
| Client | chiaki-ng **v1.10.0** (AGPL-3.0), `chiaki-ng-win_x64-MSYS2-Release-portable.zip` from the project's GitHub release |
| Checksum | sha256 `4f1e824fc522560730e0b7a901e8811de2bab4a45ad5000386f7124bd855e4ac`, checked against GitHub's published digest |
| Where | `%USERPROFILE%\tools\chiaki-ng\app\chiaki-ng-Win\chiaki.exe` (outside the repo) |
| Note | the zip contains a second zip whose file name says "test-build"; it is the asset published as v1.10.0 |
| PIN payload | `rp-get-pin.elf` v0.1.1 from [idlesauce/ps5-remoteplay-get-pin](https://github.com/idlesauce/ps5-remoteplay-get-pin), at `%USERPROFILE%\tools\chiaki-ng\payloads\rp-get-pin.elf`, sha256 `1d611c1856dd2f4b4b6cb42ead1128a7f08a26585788f92de79fa4f67d721472` (GitHub publishes no digest for it, so this is only the hash of what was downloaded) |
| Not in git | the payload repo has **no licence**, so the ELF is kept out of this repo |
| Launcher | `./tools/chiaki.ps1 where | open | stream` (reads `PS5_HOST` from `.env.local`, else `.env`) |

## Console side (needs the console online - do these in order)

Per the payload's README. Each step changes the console, so do them deliberately,
with **EVO closed** (`evo-remote.sh quit`, then `close`; never send a payload
while EVO is submitting GPU work).

1. **Offline-activate the account** with `offact` from
   [ps5-payload-dev/websrv](https://github.com/ps5-payload-dev/websrv/releases)
   (v0.14 or newer; the console's autoload chain already has `websrv`). The
   auto-generated account id is fine for chiaki; a real PSN id is only for the
   official app. This is a persistent change to the console's account state -
   check first whether it is already activated.
2. Reboot the console if it has not been rebooted since activating.
   (The payload's author found this optional; others found it required. Try the
   payload first and reboot only if pairing fails.)
3. Send `rp-get-pin.elf` to elfldr on **port 9021**. The PIN and the base64
   account id appear as a notification and on stdout; to see stdout:
   `socat -t 99999999 - TCP:<PS5_HOST>:9021 < rp-get-pin.elf` (socat is in the
   dev container). Only the displayed account id is accepted, and it is the
   currently logged-in user. Sending the payload again cancels the pairing.
4. In chiaki-ng (`./tools/chiaki.ps1 open`): add the console, **Register**, paste
   the base64 account id, type the PIN, mode PS5. Use the account id from step 3;
   do not use chiaki-ng's PSN login button.
5. `./tools/chiaki.ps1 stream -Fullscreen`.

## Record the result here

Date, firmware, and the exact outcome of each step (activation, payload output,
registration accepted / refused with message, stream started / error, whether pad
input reached a system dialog). A bare "does not work" is not a result.

| Date | Step | Result |
|---|---|---|
| 2026-10-07 | Console state | FW 12.70, address 192.168.0.12 (moved from .10 after a power cut), jailbroken, `websrv` not in the autoload list |
| 2026-10-07 | Offline activation | already done: OffAct showed `type np  flags 0x1002  name User1`, which is exactly what OffAct writes. Nothing changed, no reboot needed |
| 2026-10-07 | Load `websrv` v0.34 | sent to elfldr :9021, "Serving HTTP on 192.168.0.12:8080" |
| 2026-10-07 | LinkDev (v0.34 bundle), launched from the websrv page | PIN + Account ID shown on the TV, 300 s countdown |
| 2026-10-07 | chiaki-ng Register, attempt 1-2 | refused: HTTP 403, "Invalid PSN ID" (0x80108b02); LinkDev on the TV said "incorrect Account ID" |
| 2026-10-07 | Cause | one character misread from the TV: `dp93cW1Y034=` (digit one) instead of `dp93cWlY034=` (lowercase L). The default ID for the name `User1` is `0x7ed3586971779f76`, which gave it away |
| 2026-10-07 | Attempt 3, fresh PIN, corrected ID | **registered, stream connected** |

## What went wrong, so it is quicker next time

- **Don't retype the ID by eye.** The TV font makes `l`, `I` and `1` look the
  same. If it fails with "incorrect Account ID", compute the expected value
  instead: for the default ID of an account named `<name>`, FNV-1 with offset 0
  and prime `0x100000001B3` over the name's bytes, then base64 of the 8 bytes
  little-endian.
- **A failed pairing uses up the LinkDev session.** Exit with Circle and relaunch
  for a new PIN before each retry; the PIN lives 300 s.
- **LinkDev prints the ID with two `=`** (`...034==`); one `=` is the standard
  form and decodes to the same bytes.
- **The address moves** after a power cut: `ps5-remoteplay status` finds it.

## Notes

- The console answers Remote Play discovery (`host-type:PS5`,
  `system-version:12700001`) and TCP 9295 is open. That shows the service is
  up, not that pairing works.
- Windows Firewall may need to allow chiaki-ng (UDP) the first time; Windows
  prompts for it.
- Open question: whether sending a payload that uses ptrace on ShellUI can upset
  EVO or the console. It has not been run here. The kill-while-GPU-busy panic of
  2026-09-18 is why EVO is closed first.
