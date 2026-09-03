# Native hardware decode — integration plan

> **Status (2026-09-03):** **Phase 4 DONE — #31 closed.** GTA VI 4K H.264 plays
> real-time on `sceVideodec2` inside `PPSA99039` (`be=1` NATIVE, `pos` climbs
> 1.0×, `fatal=0`, colours correct). `media/src/evo_vdec_native.c` is the
> backend behind `evo_vdec.h`. Open: **#32** (seek → frozen picture on the V8
> 4K path, high) and **Phase 5** (the `Auto / FFmpeg / Native` settings row).
> History of the gate below.
>
> Phase 1 go/no-go gate **PASSED on hardware (2026-09-01)**. The
> "registered app slot" hypothesis is confirmed: from a fake-signed
> game-category app module the full `sceVideodec2` sequence —
> `CreateDecoder` **and `Decode`** — returns `0` and produces a valid
> 1920×1088 NV12 H.264 frame. The errno 5200 wall recorded in
> [hardware-decode.md](../hardware-decode.md) was a **process-context** limit
> (elfldr payload / hbldr borrowed slot), not a hardware, driver, or signing
> limit. That doc's two "never retry" rules (`sceVideoOutOpen` from a payload,
> kernel `.text` sweeps) still stand — they are unrelated to this result.
>
> **What proved it:** `blackbearreloaded/ProsperoLight` (a real Moonlight PS5
> client that uses VideoDec2) built with a `PROSPEROLIGHT_VDEC_SELF_TEST`
> patch that runs the decoder bring-up offline and feeds one bundled 1080p
> H.264 IDR through `sceVideodec2Decode`. Deployed to `/data/homebrew/` and
> launched via ShadowMountPlus. Every stage returned `0x00000000`; output
> `valid=1 error=0 picture_count=1 1920x1088 pitch=2048 codec=1`. No
> arbitration calls, no credential elevation. See
> [[native-decode-app-slot-plan]].
>
> **Consequence for the plan below:** Phase 1's open question ("does a
> stub-bootstrapped PIE payload inherit app privileges, or must the player be
> a static module?") is now the main design decision. ProsperoLight is a
> *static module* (NativeAOT-equivalent: SDK-linked `eboot.bin` + carried
> clean-room `libc.prx` + full `param.json`), launched from a folder under
> `/data/homebrew/` by ShadowMountPlus — **not** a launcher-stub that boots a
> PIE. Assume EVO Player must be repackaged the same way.

---

## 1. Why this is being revisited

The prior effort proved, over ten phases, that from EVO Player's **payload**
context the `sceVideodec2` driver refuses the decode ioctl with **errno 5200**
even though every byte on our side of the call is well formed. That result is
solid and this plan does not dispute it.

What changed is a reference implementation.
`D:\Projects\PS5-Research\references\SharpProspero` binds the **same two APIs**:

| SharpProspero file | Sony API | EVO Player status before this plan |
|---|---|---|
| `src/SharpProspero/Interop/Video/Videodec2.cs` + `Media/VideoDecoder.cs` | `libSceVideodec2` (`sceVideodec2*`) | tried from payload → **errno 5200**, closed |
| `src/SharpProspero/Interop/Media/AvPlayer.cs` + `Media/MediaPlayer.cs` | `libSceAvPlayer` (`sceAvPlayer*`) | six NIDs resolve; **never actually called** |

Two things about SharpProspero matter:

1. **It targets application modules, not payloads.** Every source file is
   headed *"a C# SDK for on-device application modules"*. Its media code is
   documented under *Application Modules* (`docs/media.md`). An application
   module is a signed `eboot.bin` in a `.pkg` / fake-SELF app: it launches
   with a **TITLE_ID**, a `param.json`, an app sandbox, and a user session.
   That is a different process context from an elfldr payload injected into a
   host process, and it is the one variable the prior research could not
   cleanly isolate.

2. **Its bindings do not appear to be hardware-validated.** The `prospero-media`
   sample and `docs/media.md` read as written-to-the-documented-ABI, and
   `VideoDecoder.CreateAvc` says in a comment its parameters *"mirror the
   arrangement the service documents"*. Treat SharpProspero as an **ABI
   reference**, not as proof the path works.

### The honest risk, stated up front

The backlog closure note records that the errno 5200 testing was done in a
context that *"does have a user session"* — so **a registered app slot is not
guaranteed to change the result.** The critical difference SharpProspero
still represents is that its `eboot.bin` **is** the application (NativeAOT +
SDK linker → static module), whereas EVO Player's `--format app` route
(`scripts/package-pkg.sh`) produces a *launcher stub* eboot that then boots
the **same PIE payload**. Whether a stub-bootstrapped payload inherits the
app-slot privileges, or whether the player itself must be built as a static
`ET_EXEC`, is the central unknown of this plan and is settled cheaply in
Phase 1 before any large refactor.

**Kill criteria are defined in §8. This plan is front-loaded so the go/no-go
costs a few console trips, not a rewrite.**

---

## 2. Decision

- **Route:** repackage EVO Player to run from a **fake-SELF app slot**
  (`--format app`, TITLE_ID, `param.json`) and bring up native decode there.
- **Scope:** full integration — decoder-backend abstraction, runtime probe,
  settings toggle with config migration, host-preview story, validation, docs.
- **Non-negotiable constraint (unchanged from
  [native-media-research.md](../native-media-research.md)):** the FFmpeg software
  path stays the always-available default. Native decode is selected at **run
  time** after a probe succeeds, never a build-time dependency, and any native
  failure falls back to FFmpeg without ending playback.

---

## 3. Target architecture

Today, decode lives inline in `main.c`: the play loop calls `av_read_frame` /
`avcodec_send_packet` / `avcodec_receive_frame`, then `pp_map_avframe()`
adapts the `AVFrame` into a `pp_frame` (the FFmpeg-free struct in
[pp_frame.h](../../projects/evoplayer/pp/include/pp_frame.h)), which
`pp_playback_push_frame()` converts and presents.

`pp_frame` is already the right seam. The plan introduces a decoder interface
that **produces `pp_frame` directly**, with FFmpeg as one implementation and
native as another.

```
                    ┌──────────────────────────────────────────┐
   container ─────► │  evo_demux (FFmpeg avformat, always)      │
   (USB / Emby)     └───────────────┬──────────────────────────┘
                                    │ AVPacket (compressed AU)
                        ┌───────────┴────────────┐
                        ▼                        ▼
             ┌────────────────────┐   ┌────────────────────────┐
             │ evo_vdec_ffmpeg    │   │ evo_vdec_native        │
             │ avcodec_*          │   │ sceAvPlayer  (Route A) │
             │ (always built)     │   │  or sceVideodec2 (B)   │
             └─────────┬──────────┘   └───────────┬────────────┘
                       │  pp_frame (NV12/420P)    │ pp_frame (NV12)
                       └────────────┬─────────────┘
                                    ▼
                     pp_playback_push_frame()  ──►  converter ──► VideoOut
```

New interface (`projects/evoplayer/media/include/evo_vdec.h`):

```c
typedef struct evo_vdec evo_vdec;

typedef enum {
    EVO_VDEC_BACKEND_FFMPEG = 0,   /* always available */
    EVO_VDEC_BACKEND_NATIVE = 1    /* only if probe succeeded */
} evo_vdec_backend;

typedef struct {
    evo_vdec_backend backend;      /* requested; may be downgraded */
    int   codec_id;                /* AVCodecID from the demuxer */
    int   width, height;
    const uint8_t *extradata; int extradata_size;   /* SPS/PPS etc. */
    void *avctx_params;            /* AVCodecParameters* for the ffmpeg path */
} evo_vdec_open_params;

evo_vdec *evo_vdec_open(const evo_vdec_open_params *p, evo_vdec_backend *chosen);
/* feed one compressed access unit; 0 = ok, <0 = fatal (caller falls back) */
int  evo_vdec_send(evo_vdec *v, const uint8_t *data, int size, int64_t pts_us);
/* pull a decoded frame; 1 = frame in *out, 0 = need more input, <0 = fatal */
int  evo_vdec_receive(evo_vdec *v, pp_frame *out);
void evo_vdec_flush(evo_vdec *v);      /* seek */
void evo_vdec_close(evo_vdec *v);
evo_vdec_backend evo_vdec_active(const evo_vdec *v);
```

Design notes:

- **Demux stays FFmpeg for both backends.** `sceAvPlayer` can demux its own
  file, but routing our existing container/USB/Emby/network stack through it
  would be a second integration with its own risks. For the native path we
  either (Route B) feed `sceVideodec2` raw AUs from our demuxer, or (Route A)
  use `sceAvPlayer` in its **stream-callback mode** where *we* supply the
  bytes (`AvPlayerFileReplacement` — the callback block SharpProspero maps at
  offsets 40–79 of `AvPlayerInitData`).
- **Audio is out of scope for v1.** Keep audio on FFmpeg. `sceAvPlayer`
  produces audio too, but mixing a native video clock with an FFmpeg audio
  clock is where A/V sync bugs live — defer until video is proven.
- **`evo_vdec_ffmpeg` is a near-verbatim lift** of the current `play_ctx`
  code out of `main.c`. This refactor has value on its own and is the only
  part that ships regardless of whether native ever works.
- **10-bit / HDR** ride along for free on the native path if frames come back
  as P010 — `pp_frame` gains `PP_FRAME_P010` and the converter learns it. Out
  of scope for the first milestone; noted so the interface doesn't foreclose
  it.

---

## 4. Phase plan

### Phase 0 — ABI harvest (offline, no console)

Turn the reference implementation's decode code into C headers under
`projects/evoplayer/media/include/sce/`.

- [x] **`sce_videodec2.h`** — done 2026-09-01, transcribed from ProsperoLight
      (`src/moonlight_stream.cpp`), not SharpProspero, because ProsperoLight's
      path is **hardware-verified**. Structs, codec values
      (`Avc=1`, `Hevc=974921`, `Vp9=2382845`), memory-typing crib, and the full
      call sequence are in [videodec2-abi.md](videodec2-abi.md). Header
      compiles as C11 and C++20.
- [x] `sce_avplayer.h` — done 2026-09-02, transcribed from SharpProspero
      (`AvPlayer.cs` + `MediaPlayer.cs`). `SceAvPlayerInitData` (120 B), the
      three callback blocks (`MemAllocator` 40B, `FileReplacement` 40B —
      **object-ptr first, `readOffset` only**, not the PS4 shape —
      `EventReplacement` 16B), `SceAvPlayerFrameInfoEx` (104 B, pitch @ 0x3C +
      four crop insets @ 0x2C..0x38), stream-info structs. Sizes/offsets
      `_Static_assert`ed; compiles as C11 and C++17. Full write-up:
      [avplayer-abi.md](avplayer-abi.md).
- [x] NID handling — the header pins the six hw-verified core NIDs in a
      comment for cross-check, but calling code **computes** every NID
      (`nid_encode` in a payload, inline SHA1 in the app module) rather than
      hardcoding — an earlier draft's pasted NID table was scrambled.
- [x] Port `MediaPlayer`'s allocator + texture-slot callbacks to C — done in
      `projects/avplayer_test/main.c` (general = aligned heap; texture =
      GPU-visible direct memory + `(addr→offset,size)` slot table; the
      deallocator unmaps *and* releases). Memory-typing diff vs.
      `hardware-decode.md` (old try used `WC_GARLIC` 3; SharpProspero uses
      `MemoryTypeCachedShared` 12 / prot 0x33) written up in
      [avplayer-abi.md](avplayer-abi.md) §4, including the **payload caveat**:
      `sceKernelGetDirectMemorySize()` is 0 in a payload so the spike uses the
      main pool; if the decoder rejects it, Route A needs the app-module
      context.

**Deliverable:** headers compile (done); memory-typing diff written
([avplayer-abi.md](avplayer-abi.md) §4).

### Phase 1 — run EVO Player from an app slot

> **Gate PASSED 2026-09-01** via the ProsperoLight self-test (see status block).
> The context *does* differ from the payload: `sceVideodec2Decode` succeeds.
> Remaining Phase 1 work is now purely about **repackaging EVO Player** as a
> static app module and confirming the *player's own* decode probe succeeds in
> that package (not just a standalone test binary).

Get the *unchanged* player (FFmpeg decode) launching as a registered title,
and establish whether that context differs from the payload.

- [ ] Port the launcher-stub pattern from
      `$PS5_PAYLOAD_SDK/samples/install_app/` (`eboot.c` + `eboot.x` +
      `payload.c`) into `projects/evoplayer_app/` — a static `ET_EXEC` eboot
      that boots the player. `scripts/package-pkg.sh --format app` already
      does the `make_fself.py` + `param.json` + FTP-install half; it currently
      `die`s because our ELF is PIE, so this phase makes the stub it expects.
- [ ] Install via the FTP + `sceAppInstUtil` sequence in
      [packaging.md](../packaging.md); launch from the home screen.
- [ ] **Probe the context from inside the running app** (extend the existing
      `decoder_test` logic, run it *as the app payload*): does
      `sceUserServiceGetInitialUser` now return a real user (not `0x80940004`)?
      Is `libSceVideodec2` mappable? What does a minimal
      `sceVideodec2QueryComputeMemoryInfo` → `AllocateComputeQueue` →
      `QueryDecoderMemoryInfo` → `CreateDecoder` → **`Decode`** sequence return?
- [ ] Decide whether the stub-bootstrapped payload inherits app privileges, or
      whether the player must be relinked as the static eboot itself. Record it.

**This is the go/no-go gate.** If `sceVideodec2Decode` still returns errno
5200 here *and* the `sceAvPlayer` spike in Phase 2 also fails, stop — §8.

### Phase 2 — native decode spike (in the app module — payloads can't decode)

Both probes run **inside `PPSA99039`** (the errno-5200 wall is a payload/hbldr
sandbox limit; only the registered app module has hardware decode). Timeboxed.
Run A first — it's the untried route.

**Route A — `sceAvPlayer`:** the probe is **built into the app module** behind
`-DEVO_AVPLAYER_PROBE` — `projects/evoplayer/src/evo_avplayer_probe.c` (boot-time,
like `evo_agc_probe.c`). It does all four steps below; Phase 2 is now just
*launching EVO on the console with a test file present* and reading the
`EVO avplayer:` popups against [avplayer-abi.md](avplayer-abi.md) §5.
- [x] `sceAvPlayerInit` with the Phase 0 struct; general heap + texture
      allocator (`AllocateMainDirectMemory` type 12→3, prot 0x33); log+serve
      file callbacks; debug `All`; watchdog thread `_exit()`s EVO on a hang.
- [x] `sceAvPlayerAddSource` (auto-finds `/data/probe.mp4` /
      `/mnt/usb0/probe.mp4` / `/download0/evoplayer/probe.mp4`), then
      `StreamCount` / `GetStreamInfo` / `EnableStream`.
- [x] `sceAvPlayerStart` → poll `sceAvPlayerGetVideoDataEx` → dump the first
      frame + metadata to `/download0/evoplayer/avpx_frame0.*`.
- [x] Characterise: `pitch` vs `width`, the four crop insets, CPU-readable
      (SIGSEGV-guarded), rough NV12 sanity, which texture memory type worked.
- [x] **RAN on hardware 2026-09-03** →
      `EVO avplayer: libSceAvPlayer.sprx load FAILED - Route A blocked`.
      Same wall as `libSceAgc` (#27): a fake-signed app module can't
      `sceKernelLoadStartModule` a system PRX it didn't declare NEEDED.
- [ ] **BLOCKER: `libSceAvPlayer` import stub** — must be added to the
      app-module link (`tools/native-app/` + `package-app.sh`), same
      prerequisite as #27's `libSceAgc` stub and Route B's `libSceVideodec2`.
      SharpProspero's `tools/SharpProspero.Prx/` (StubEmitter / StubCatalog) is
      the reference; `prospero-nid` + `evo_avplayer_probe.c`'s symbol list give
      the NID table. Then re-run the gate — symbols resolve directly, no probe
      dlsym.
- payload build `projects/avplayer_test/` kept as a compile-checked
      callback-port reference only.

**Route B — `sceVideodec2`: ✅ WORKS ON HARDWARE (2026-09-03, PPSA99039).**
`EVO vdec2: HARDWARE DECODE OK` — every call `0`, `out valid=1 err=0 pics=1
1920x1088 pitch=2048 codec=1` — from inside the full 43 MB EVO Player, which
then booted on to the menu. Probe: `projects/evoplayer/src/evo_videodec2_probe.c`
(`--videodec2-probe`), a C port of ProsperoLight's VDEC self-test.

Two things had to be true, both hard-won:
- [x] **`libSceVideodec2` (+ `libSceAgc` + `libSceAgcDriver`) linked as a
      POSITIONAL PRX import stub** (`tools/native-app/stubs/prx/*.syms`,
      `package-app.sh` step 6b), so the loader auto-loads the `.sprx`. `--as-needed`
      is not enough — the DT_NEEDED must be unconditional (ProsperoLight does the
      same). AGC/AgcDriver must be present too or `libSceVideodec2`'s own GPU
      imports don't resolve.
- [x] **The decode init must run BEFORE `evo_jailbreak_self()`.** The Lapy /
      etaHEN self-unjail swaps the process credentials mid-run (uid→0, caps,
      `fd_rdir`/`fd_jdir`=rootvnode); after that swap `sceSysmoduleLoadModule(207)`
      returns `0x80020063` (`ESDKVERSION`) and `libSceVideodec2` never finishes
      loading, so the first `sceVideodec2*` call faults. Run pre-unjail: `sysmod207
      → 0`, everything works. This is the whole reason ~14 hardware launches were
      needed — `evo_agc_probe`'s failed `sceKernelLoadStartModule` calls are a
      red herring; the unjail is the poison. → [[native-decode-app-slot-plan]]
- Note: `sceSysmoduleLoadModule(207)` (public) works pre-unjail; the *Internal*
      variant (`0x800000B2`) returns `0x80020008` — use the public one.

**Route A** (`sceAvPlayer`, module `0xA5`) stays dead: `sceSysmoduleLoadModule(0xA5)`
is refused (`0x80020063`) even pre-unjail, so `sceAvPlayerInit` faults.

**Deliverable met:** a decoded NV12 H.264 frame from `sceVideodec2` inside EVO's
own signed package. → Phase 4.

### Phase 3 — decoder abstraction refactor (ships regardless)

> **Prerequisite: Track A of [modularisation-plan.md](../modularisation-plan.md).**
> Steps A1–A7 there *are* this phase's groundwork — they pull the demux
> thread, audio path, and the pure decode loop out of `main.c` and define
> `evo_vdec.h`. Once Track A lands, Phase 3 is "add `evo_vdec_native.c` beside
> `evo_vdec_ffmpeg.c`" and the rest of this section is already done.

- [x] Add `evo_vdec.h` (§3) + `evo_vdec_ffmpeg.c` — move `play_ctx`,
      `avcodec_open2`, the send/receive loop, and `pp_map_avframe` /
      `pp_map_yuv420p10_to_8` out of `main.c` behind the interface. *(Track A A6.)*
- [x] `main.c` play loop calls `evo_vdec_send` / `evo_vdec_receive` and keeps
      pushing `pp_frame` into `pp_playback` exactly as now.
- [x] Seek path calls `evo_vdec_flush` — all three video seek entry points
      (`main.c` resume-seek, `evo_demux.c` scrub/seek-request, and the play
      loop's flush) route through it. The one-shot cover/poster extractor
      (`main.c`) and the scrub-preview worker (`media/src/prospero_thumbnail.c`)
      keep their own isolated `av_seek_frame` + `avcodec_flush_buffers` on
      purpose — separate AVFormatContext, not the play stream.
- [x] `#30` sign-off: dead `ffmpeg_mkv_test()` inline decoder removed; the
      cover/poster extractor documented as staying out of the seam (its home is
      `evo_cover`, modularisation-plan Track B / B6). `evo_vdec_ffmpeg.c` is now
      the only file with play-stream `avcodec_*` / `sws_*`.
- [ ] Verify bit-exact parity on hardware: codec sweep +
      [validation.md](../validation.md), `tools/bench.sh` plane hashes. Expected
      identical — the play loop has routed through `evo_vdec` since A6 and `#30`
      made no decode-path behaviour change — this is the empirical sign-off.
- [x] Host preview (`tools/uiview_playback_rml.sh`) builds + renders. *(It no
      longer links `evo_vdec_ffmpeg` directly — it exercises only the RmlUi
      screens — so it is structurally unaffected.)*

### Phase 4 — native backend — **#31**

Route B (`sceVideodec2`) survived Phase 2. Port the proven sequence from
`projects/evoplayer/src/evo_videodec2_probe.c` — do not re-derive.

**DONE on hardware (#31, 2026-09-03).** GTA VI 4K H.264 plays real-time on
`sceVideodec2` in EVO — colours correct, no judder, `fatal=0`. Frame order is
display-order. Remaining: **seek → frozen picture** on the V8 4K path (filed
separately, high priority), and Phase 5 (settings row).

- [x] `evo_vdec_native.c` implementing `evo_vdec.h` against `sceVideodec2`.
      Crop applied (`disp_w/h` from the demuxer vs the coded
      `OutputInfo.width/height`); chroma offset uses the **coded** luma height,
      `pitch_bytes` for stride. mp4/mkv AUs run through the `*_mp4toannexb` bsf.
      **Output format depends on the renderer (#27):** by default `ro_harvest`
      de-interleaves NV12 → planar **`PP_FRAME_YUV420P`** (EVO's fast / parallel
      / 4K CPU converters only accept YUV420P — NV12 at 4K silently draws
      black). When `pp_agc_available()` it emits **straight `PP_FRAME_NV12`**
      (one flat copy, no CPU touch) for the sceAgc GPU present path, and
      `pp_frame.coded_height` carries the MB-padded luma height so
      `pp_agc_present_nv12` can find the UV plane. `pp_playback.c` has an
      NV12→YUV420P fallback for any path that isn't the AGC one.
- [x] **Module load before the first `evo_jailbreak_self()`** —
      `evo_vdec_probe()` at `main.c` ~12146, right after `evo_videodec2_probe()`.
      Open HW question (does `CreateDecoder` after `evo_jailbreak_ensure()`
      still work) is called out in `status.md` for the first run.
- [x] `libSceVideodec2` + `libSceAgc` + `libSceAgcDriver` positional PRX stubs
      unconditional for `MODE == player` in `package-app.sh` step 6b.
- [x] Frame-buffer lifetime: `evo_vdec_native` **copies** each picture out of
      the frame pool into a small reorder window (needed anyway — the reorder
      window is also the B-frame display-order safety net, and there is no
      output-PTS/picture-detail call bound). One extra full-frame read+write
      per frame; the converter reads every byte immediately after regardless.
- [ ] Wire the direct-memory allocations through `evo_direct_mem`
      ([evo_direct_mem.c](../../projects/evoplayer/media/src/evo_direct_mem.c))
      rather than raw `sceKernelAllocateDirectMemory` — deferred; the probe's
      raw path is what's hardware-verified, revisit after multi-hour soak.
- [ ] Watchdog in the decode thread (hardware-decode-review §7) — deferred.
      Today a native call that *returns* an error falls back cleanly
      (`evo_playback`'s fatal-streak → finished screen); a native call that
      *hangs* still wedges the app slot. Add the watchdog thread once the
      happy path is confirmed on hardware.

### Phase 5 — settings toggle + runtime probe

- [ ] `evo_vdec_probe()` at startup (or first playback): load the module,
      resolve NIDs, run the cheapest non-destructive check
      (`QueryComputeMemoryInfo`, or `sceAvPlayerInit`+`Close`). Cache the
      result. Never probe on the render thread.
- [ ] Settings model — a new tri-state in the flat config:
      `EVO_VDEC_PREF_AUTO` / `_FFMPEG` / `_NATIVE`.
  - `AUTO` → native if the probe passed and the codec is supported (H.264,
    later HEVC), else FFmpeg.
  - `FFMPEG` → always FFmpeg.
  - `NATIVE` → native; if the probe failed, show it greyed with "unavailable"
    and behave as FFmpeg.
- [ ] Config migration in `prospero_settings_save` / `_load`
      ([main.c:12428](../../projects/evoplayer/main.c#L12428) /
      [:12511](../../projects/evoplayer/main.c#L12511)): append **one `%d`** after
      `evo_keyboard_get_type()` in the `fprintf`, add one field to the
      `fscanf` format and one default (`loaded_vdec_pref = 0`). This is the
      exact pattern the file's own comments describe for the theme-name /
      feedback / subtitle-face / keyboard-type appends — an older file still
      parses and keeps the default.
- [ ] Settings screen — add a **"Video decoder"** row to
      `SCREEN_SETTINGS_PLAYBACK`:
  - bump `EVO_SETTINGS_PLAYBACK_COUNT` 4 → 5
      ([main.c:232](../../projects/evoplayer/main.c#L232));
  - add the `settings_playback_selected == 4` branch in
    `settings_playback_activate()`
    ([main.c:12901](../../projects/evoplayer/main.c#L12901)) cycling
    AUTO→FFMPEG→NATIVE with a `toast()` and `prospero_settings_save()`;
  - render the row + current value in the playback-settings draw code (same
    place the other four rows draw);
  - mirror it into the RmlUi settings document
    (`assets/rml/`, `evo_rmlui_bridge.cpp`) if that screen has been migrated —
    check `docs/rmlui-integration-guide.md` for current parity.
- [ ] Changing the toggle mid-playback: apply on next `open_file`, toast
      "applies to next video". Don't hot-swap a live decoder.

### Phase 6 — host preview, validation, docs

- [ ] `tools/uiview_playback_rml` / `uiplay`: the native path can't run on the
      host. Guard `evo_vdec_native` behind `__PROSPERO__` (or the SDK macro
      already used) so the host build always gets `evo_vdec_ffmpeg` and the
      toggle shows "native unavailable on host". No host regression.
- [ ] Extend [validation.md](../validation.md) codec sweep with a
      **backend column** and per-file decode ms/frame + dropped-frame count
      for both backends (this also feeds
      [improvements-roadmap.md](../improvements-roadmap.md) P2).
- [ ] Paired A/B benchmark (hardware-decode-review §6): identical clip, same
      session, FFmpeg vs native — decode time and *copy* time measured
      separately. A native path that isn't decisively faster on a clip the CPU
      path already struggles with does not ship as the `AUTO` default.
- [ ] Rewrite the top of [hardware-decode.md](../hardware-decode.md): it stays
      the record of the payload-context closure, with a pointer here for the
      app-context outcome.
- [ ] Update [backlog.md](../backlog.md) item 10 and
      [improvements-roadmap.md](../improvements-roadmap.md) (the
      "decode is permanently on the CPU" framing).

---

## 5. Settings design detail

Config file (`/data/evoplayer/evo_player_settings.cfg`) today ends:

```
… <theme-name>\n <feedback-sound> <feedback-lightbar> <subtitle-face> <keyboard-type>
```

After:

```
… <theme-name>\n <feedback-sound> <feedback-lightbar> <subtitle-face> <keyboard-type> <vdec-pref>
```

`vdec-pref`: `0` AUTO (default), `1` FFMPEG, `2` NATIVE. Absent → `0`.

UI string per state, shown on the row and in the toast:

| Value | Row shows | When probe failed |
|---|---|---|
| AUTO | `AUTO (native)` or `AUTO (FFmpeg)` | `AUTO (FFmpeg)` |
| FFMPEG | `FFmpeg` | `FFmpeg` |
| NATIVE | `Native` | `Native — unavailable` (acts as FFmpeg) |

A one-line status under the row when native is active:
`H.264 · Sony decoder · 3.1 ms/frame` (from the pipeline metrics EVO already
collects — `pp_pipeline_metrics.h`).

---

## 6. File-by-file change list

**New:**

| Path | Purpose |
|---|---|
| `projects/evoplayer/media/include/evo_vdec.h` | decoder interface (§3) |
| `projects/evoplayer/media/src/evo_vdec_ffmpeg.c` | FFmpeg impl (lifted from `main.c`) |
| `projects/evoplayer/media/src/evo_vdec_native.c` | Sony-module impl (Phase 4) |
| `projects/evoplayer/media/src/evo_vdec_probe.c` | run-time capability probe |
| `projects/evoplayer/media/include/sce/sce_videodec2.h` | ABI (Phase 0) |
| `projects/evoplayer/media/include/sce/sce_avplayer.h` | ABI (Phase 0) |
| `projects/evoplayer_app/{eboot.c,eboot.x,payload.c,Makefile}` | fake-SELF launcher stub (Phase 1) |
| `projects/avplayer_test/main.c` | already a placeholder — fill in for Phase 2 |
| `docs/native-decode-app-context.md` | results log for this effort (mirror of the old one) |

**Modified:**

| Path | Change |
|---|---|
| `projects/evoplayer/main.c` | play loop + seek call `evo_vdec_*`; remove inline `avcodec` video path; `prospero_settings_{save,load}` +1 field; `SCREEN_SETTINGS_PLAYBACK` +1 row; `EVO_SETTINGS_PLAYBACK_COUNT` 4→5 |
| `projects/evoplayer/pp/include/pp_frame.h` | (later) `PP_FRAME_P010` for 10-bit native output |
| `scripts/package-pkg.sh` | `--format app` points at `projects/evoplayer_app/` stub instead of `die`-ing on PIE |
| `scripts/build-evoplayer.sh` | build the new `media/src/evo_vdec_*.c`; optional `evoplayer_app` target |
| `docs/hardware-decode.md`, `docs/backlog.md`, `docs/improvements-roadmap.md` | cross-reference this plan / update the "closed" framing |
| `assets/rml/*`, `ui_rml/src/evo_rmlui_bridge.cpp` | settings row parity if that screen is on RmlUi |
| `docs/tooling.md` | `--format app` install flow, native-probe env var |

---

## 7. What must not be broken (carried from the closed effort)

- **Never sweep kernel `.text`** from anywhere. Not needed by anything here.
- **Never call `sceVideoOutOpen` from a payload** — panics once a compute
  queue is allocated after it. The app already owns its VideoOut handle
  (`pp_videoout.c`); the native decoder must use the compute-queue path
  (`sceVideodec2AllocateComputeQueue`), **not** open video out.
- **Never stack launches / never leave a payload hanging on VideoOut.** The
  Phase 2 spike gets a hard watchdog.
- FFmpeg path stays the default and the fallback. No build-time dependency on
  any `libSceVdec*` / `libSceAvPlayer` stub — everything resolves by NID at
  run time, module absent = feature absent.

---

## 8. Kill criteria

Stop and revert to "CPU decode is permanent" if **all** of:

1. Phase 1 shows the app slot's process context is not materially different
   from the payload's for module access (`sceUserServiceGetInitialUser` still
   `0x80940004`, or `libSceVideodec2` behaves identically), **and**
2. Phase 2 Route A (`sceAvPlayerInit` … `GetVideoDataEx`) fails at or before
   the first frame with a consistent permission/arbitration error, **and**
3. Phase 2 Route B (`sceVideodec2Decode` with SharpProspero's exact params)
   still returns **errno 5200**.

Partial success still worth shipping:

- **Only `sceAvPlayer` works** → ship it as the native backend; skip the
  raw-`Videodec2` path entirely.
- **Native works but isn't faster** than FFmpeg + the GPU compute pipeline on
  4K → ship it as opt-in `NATIVE` only, never `AUTO`, and document that it
  exists for HEVC Main10 / very-high-bitrate cases the CPU can't sustain.
- **Phase 3 refactor only** (native never lands) → still merge it; the
  decoder abstraction and the settings toggle (native greyed out) are a
  cleaner codebase and a truthful UI.

---

## 9. Sequencing summary

| Phase | Console cost | Reversible? | Gate |
|---|---|---|---|
| 0 ABI harvest | none | n/a | headers compile |
| 1 app-slot bring-up | ~3–5 deploys | yes (parallel to payload) | context differs? |
| 2 native spike | ~3–6 deploys, watchdogged | yes | a Sony frame, or a taxonomy |
| 3 abstraction refactor | sweep + bench | yes | bit-exact parity |
| 4 native backend | iterative | yes (behind toggle) | paired A/B faster |
| 5 settings + probe | 1–2 deploys | yes | fallback verified |
| 6 host / validation / docs | none / sweep | yes | no host regression |

Phases 0 and 3 can start immediately and in parallel — 3 has value with or
without native decode. Phases 1–2 are the research gate and should be a
timeboxed spike before 4 begins.
