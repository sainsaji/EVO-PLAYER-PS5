# Task: on-demand sceVideodec2 decoders (and 4K HDR playback)

**Status:** implemented (Phase 2, 2026-09-24), hw-verify-pending. Hardware evidence
gathered 2026-09-24.
**Area:** `projects/evoplayer/media/src/evo_vdec_native.c` (+ two call sites).
**Outcome:** 4K HEVC Main10 (4K HDR) plays, without removing any other codec.

Read this whole file before writing code. It contains hardware measurements that
cost several console cycles to obtain; re-deriving them is waste, and two of them
contradict comments still present in the source.

---

## 1. Why this task exists

EVO cannot play 4K HDR. A 4K HEVC Main10 file is refused at open:

```
PlaybackController: native declined 3840x2160 (codec=173); refusing the FFmpeg
                    fallback - will not fit the title memory budget
```

The refusal is correct given the current design, but the design rests on two
claims that were tested on 2026-09-24 and are **both false**.

### Claim 1 — "libSceVideodec2 stops working after the credential promotion"

Stated in the header comment of `evo_vdec_native.c` (still there, still says
this):

> The self-unjail (evo_jailbreak_self / _ensure) swaps process credentials
> mid-run, after which EVERY libSceVideodec2 call fails
> (sceVideodec2QueryComputeMemoryInfo -> 0x811D0111) ... Lazy per-playback
> creation is impossible: by open() time the unjail has already happened.

Measured:

```
AUTHZ pre-unjail:  uid=1 gid=1 sysmod=0x00000000 query=0x00000000 cpu_gpu=4805376
AUTHZ post-unjail: uid=0 gid=0 sysmod=0x00000000 query=0x00000000 cpu_gpu=4805376
LATE CREATE HEVC10 1920x1088 POST-UNJAIL OK  frame=6128KB flex_before=199MB
```

`uid` going 1 → 0 proves the promotion really landed, so this is not a promotion
that silently failed. A **complete** decoder bring-up — `AllocateComputeQueue`,
all four allocations, `CreateDecoder`, `Reset` — succeeds after it.

The claim dates from 2026-09-03 and was probably observed under etaHEN; the
console now runs the Lapy jailbreak daemon. Either the behaviour changed or the
original conclusion was wrong. **Lazy per-playback creation is possible.**

### Claim 2 — "4K HEVC Main10 is not supported"

First attempt failed, and looked conclusive:

```
HEVC10 config profile=2 max_level=123 max=3840x2176 dpb=auto depth=1
FAILED at [QueryDecoderMemoryInfo] rc=0x811d0200 flex_before=197MB
```

`max_level=123` is HEVC **Level 4.1**, whose ceiling is 1920x1088. A 4K surface
at Level 4.1 is an invalid configuration, which is what `0x811d0200` was saying.
The level came from the slot descriptor's `level_4k`, which was `123` only
because that slot had never been asked for 4K.

With `level_4k = 153` (Level 5.1 — what the 8-bit HEVC slot already uses at 4K):

```
HEVC10 config profile=2 max_level=153 max=3840x2176 dpb=auto depth=1
[QueryDecoderMemoryInfo] -> [MapNamedFlexibleMemory] -> [alloc(gpu)] ->
[alloc(cpu_gpu)] -> [alloc(input)] -> [alloc(frame)] -> [CreateDecoder] ->
[Reset] -> [ok]
LATE CREATE HEVC10 3840x2176 POST-UNJAIL OK  frame=24496KB flex_before=197MB
```

`24496KB` is one 4K 10-bit frame. **4K HEVC Main10 decodes on this hardware**,
created at runtime, with 197 MB of flexible memory free.

This is not in `third_party/ps5-hardware-video-decoding-research`'s proven table
(`docs/codecs-and-resolutions.md` stops at Main10 1080p), so treat it as a new
result and keep the evidence with the code.

The `level_4k = 153` change is **already in the working tree**. Keep it.

---

## 2. How the decoder layer works today

`projects/evoplayer/media/src/evo_vdec_native.c`, ~1500 lines. Structures:

| Thing (line numbers as of 2026-09-24; they drift) | What it is |
|---|---|
| `g_codec[]` (line 274) | Static descriptor table, one per `nat_codec`: codec id, sce codec type, profile, `level_1080`, `level_4k`, bsf name, tag, `pipeline_depth`. |
| `g_dec[]` | One `struct dec_slot` per codec: the live `decoder` handle plus every allocation backing it, and a `volatile int owned` flag. |
| `evo_vdec_native_probe()` (line 674) | Boot-time, **before** `evo_jailbreak_self()`. Loads the sysmodule, then `probe_slot()` per codec. Sets `g_boot_any`. |
| `probe_slot(c, w, h, required, sm)` (line 549) | Brings up `g_dec[c]` via `slot_bringup()`; on a 4K failure retries at 1080p; marks `ready`. |
| `slot_bringup(s, d, w, h, &stage)` (line 442) | The real work. `STAGE()` names each step **before** entering it. Returns rc; leaves `*stage` at the failing step. |
| `slot_teardown(s)` (line 408) | Frees every allocation and zeroes the slot. Safe on a partially built slot. |
| `evo_vdec_native_open(p)` (line 1262) | Per-playback. **Creates nothing.** Checks `evo_vdec_native_supports()`, refuses if `slot->owned`, claims the slot, builds the bsf, `sceVideodec2Reset()`s the resident decoder. |
| `evo_vdec_native_close(v)` (line 1486) | `Reset`s the decoder, clears `slot->owned`, frees the bsf. **Leaves the resident decoder alive.** |
| `evo_vdec_native_supports(...)` (line 732; the copy at 1516 is the host/payload stub) | Capability query. Includes a dimension gate against the slot's `max_w`/`max_h` **as brought up**, which is why a 1080p-sized slot refuses 4K. |

The resident set brought up at boot (from a real boot log):

| Slot | Size | frame | direct total | flex (`cpu_map`) |
|---|---|---:|---:|---:|
| AVC | 3840x2176 | 12256 KB | 434896 KB | 72688 KB |
| HEVC | 3840x2176 | 12256 KB | 480624 KB | 76800 KB |
| VP9 | 3840x2176 | 12256 KB | 683520 KB | 12896 KB |
| HEVC10 | **1920x1088** | 6128 KB | 199648 KB | 22288 KB |

`flex after bring-up = 199MB (resident decoders took 186MB)`.

**Read the header comment's MEMORY section and the `EVO_VDEC_NATIVE_VP9_MAX_W`
comment (line 205) before touching budgets.** The pool that runs out is
**flexible memory** (448 MB configured, 374 MB available at boot); the big
`total=` figures are *direct* memory and were never the constraint. A previous
change capped VP9 at 1080p on the strength of the direct-memory numbers and
"cost 4K VP9 hardware decode and bought nothing".

---

## 3. What to build

Make `evo_vdec_native_open()` able to **bring a slot up sized to the stream**,
and `evo_vdec_native_close()` tear it down again — instead of only resetting
whatever was pre-created at boot.

### Phase 1 — on-demand resize for the 10-bit slot

Smallest change that gets 4K HDR playing. Scope it to `NAT_HEVC10` first.

1. In `evo_vdec_native_open()`, after the descriptor is chosen and before the
   `supports()` gate: if the slot is not `owned` and the requested `w`/`h`
   exceed the slot's current `max_w`/`max_h`, `slot_teardown()` it and
   `slot_bringup()` it at the requested size. On failure, restore the previous
   size (bring it back up at the old dimensions) and return `NULL` so the caller
   still gets its FFmpeg fallback.
2. `evo_vdec_native_supports()` must stop being the thing that blocks this. Its
   dimension gate compares against the slot **as currently built**, so with
   on-demand resize it should compare against what the slot *could* be brought
   up as — i.e. the descriptor's ceiling (`EVO_VDEC_NATIVE_MAX_W/H`), not
   `s->max_w`/`s->max_h`. Keep the profile and bit-depth logic exactly as it is.
3. On `close()`, if the slot was resized above its boot size, bring it back down
   to the boot size so an idle EVO is not holding 150 MB of flex for nothing.
   Record the boot size per slot when `probe_slot()` first succeeds.

### Phase 2 — general lifecycle (optional, do only after Phase 1 is verified)

Same mechanism for every codec, with the resident set at boot reduced to
whatever is needed to prove the service works (or nothing at all). This is the
version that makes the budget stop being simultaneous. Do not start here: Phase 1
is independently useful and far easier to reason about if it regresses.

### Also in this change

- **Delete the diagnostic probes.** `evo_vdec_native_authz_probe()` and
  `evo_vdec_native_late_create_probe()` in `evo_vdec_native.c`, their
  declarations in `media/include/evo_vdec.h` and `media/src/evo_vdec_native.h`,
  their host/payload stubs, and both call sites in
  `core/src/Application.cpp::initialize()`. They allocate ~700 MB transiently on
  every boot and must not ship.
- **Fix the header comment.** The RESIDENT DECODERS paragraph (line 23) of
  `evo_vdec_native.c` states the 0x811D0111 constraint as fact. Replace it with
  what section 1 of this document establishes, including the AUTHZ/LATE CREATE
  evidence, so the next person does not restore the old design from the comment.
- **`PlaybackController.cpp` line 506** refuses the FFmpeg fallback above 1080p
  when native declines. Leave that refusal in place — it is correct and it is
  what stops a 4K software decode. It should simply stop triggering for 4K
  Main10 once native accepts.

---

## 4. Hard rules (violating these costs hardware)

From `CLAUDE.md` and `docs/build/tooling.md`. These are not style preferences.

- **Never run `make` directly.** Use `scripts/build-evoplayer.sh` (host compile
  check, never deploys) or `scripts/package-app.sh` (real build).
- **One hardware path:** `package-app.sh --ffpfsc` + `deploy-app.sh --ffpfsc`.
  There is no ELF deploy route; do not add one.
- **Never deploy over a running EVO.** It has kernel-panicked this console. A
  *crashed* EVO is already closed and needs no switcher close; a running or
  QUIT-parked one does. `deploy-app.sh` refuses when it sees `evo.log` unless
  `--force`.
- **A bad `codec_type` or `max_level` faults instead of returning** — see the
  comment above `STAGE()` (line 434). That is why every stage is logged before
  it is entered: when Sony's code dies, the last klog line is the only evidence.
  Run `tools/klog.sh` in another terminal for any launch that changes these.
- **Never `#include <iostream>`** in the app module (priority-100 static init
  derefs null in the native-app CRT; cost a whole session). `fprintf(stderr,…)`
  is also unsafe — **stderr is not usable**; use `evo_bt()` / `note()`. See
  `docs/…` and the `EvoAvLogCallback` comment in `Application.cpp`.
- All toolchain work runs in the pinned Docker container; the scripts re-exec
  themselves through `docker compose`.

---

## 5. Build, deploy, verify

```bash
# compile check (fast, no console)
docker compose run --rm ps5-dev ./scripts/build-evoplayer.sh

# real build + deploy
docker compose run --rm ps5-dev bash -lc '
  ./scripts/package-app.sh --ffpfsc
  ./scripts/deploy-app.sh --ffpfsc --force'

# watch the console live (separate terminal) - essential for max_level changes
docker compose run --rm ps5-dev ./tools/klog.sh

# pull the log afterwards
docker compose run --rm ps5-dev ./tools/evo-remote.sh log   # -> output/logs/evo.log
```

Diagnostics are one file: `/mnt/usb0/evo.log`, cleared by every deploy. A
`timeout`/`curl` timeout on an `evo-remote.sh` call is the normal successful
outcome.

### Acceptance criteria

1. A 4K HEVC Main10 file **plays**, with video and audio, from `/mnt/usb0` or
   `/data`. The log shows `OPEN ok resident HEVC10 decoder 3840x2176` (or the
   on-demand equivalent) and frames flowing (`app video blit: …`), and **no**
   `native declined 3840x2160` line.
2. A 1080p HEVC Main10 file still plays — the boot-size path must not regress.
3. 4K 8-bit HEVC, 4K AVC and VP9 still play; their slots are untouched.
4. Browsing a folder of mixed 4K/1080p files produces posters as before and does
   not crash. Watch for `no software poster` / `skipping … before
   find_stream_info` lines: those gates are correct and should stay.
5. After playback of a 4K HDR file stops, `flex after …` accounting shows the
   slot returned to its boot size (Phase 1 item 3).
6. No `AUTHZ` or `LATE CREATE` lines anywhere in the log — the probes are gone.
7. `scripts/build-evoplayer.sh` is green (it also builds the non-app-module path).

### Test material

`/mnt/usb0/movies/hdr/` on the dev console holds three 4K HEVC Main10 HDR10
files (`.ts`, `.mkv`, `.mp4`; PQ/BT.2020, yuv420p10le, 3840x2160, Level 5.1).
`/mnt/usb0/movies/` holds `EVO_TEST_hevc10_pq_4k.mp4` and 1080p 10-bit samples.

---

## 6. Traps specific to this area

- **`thumb_quarantine`.** `/data/evoplayer/thumb_quarantine` records files that
  previously crashed the process during poster extraction, and those files are
  then skipped. If a test file mysteriously produces no poster, check that file
  and delete it to retry (`evo_crash_note.h` explains the two stages).
- **Open-GOP seeks.** `drop_undecodable_leading()` drops RASL pictures after a
  random-access point; without it the hardware decoder returns `out.error=1` on
  every AU after a seek into an x265 open-GOP stream. Do not remove it while
  refactoring the decode path.
- **`pipeline_depth = 1`** for `NAT_HEVC10`/`NAT_VP92` while the others use 4.
  The comment above it says "2"; the value is 1. Unverified either way — leave
  it alone unless measuring it deliberately.
- **10-bit output format.** 10-bit slots emit `PP_FRAME_NV12_10` and the present
  path already has `video_yuv_p010_hdr` / `_hlg` pipes; a 1080p 10-bit file
  decodes and presents correctly today (198 frames verified). 4K 10-bit present
  is **unverified** — if the decoder comes up but the picture is wrong, that is
  the next thing to look at, not the decoder.
- **Don't resize the resident HEVC10 slot to 4K at boot** as a shortcut. It fits
  (197 MB free vs ~150 MB), but it leaves the poster path hovering at its own
  64 MB `ProbeFloorBytes` in `CoverArtService.cpp`, which trades a playback win
  for a new class of thumbnail failure. On-demand is the point of the task.
- **Don't drop VP9 or any other codec to make room.** Explicitly ruled out by
  the project owner. If Phase 1 cannot fit, say so with numbers rather than
  trading.

---

## 7. Open questions

- Does 4K 10-bit **present** work end to end, or only decode? See the trap above.
- Is `0x811D0111` gone for good, or does some *other* daemon state bring it back?
  The measurement is from one session with the Lapy daemon. If a build starts
  failing to create decoders at runtime, re-check this before assuming a code bug.
- `QueryDecoderMemoryInfo` accepted Level 5.1 at 4K for profile 2, but nothing
  has yet decoded a real 4K Main10 *stream* — only created the decoder. Frame
  throughput, DPB behaviour and `max_dpb_frames=-1` (auto) at that size are
  unmeasured.
