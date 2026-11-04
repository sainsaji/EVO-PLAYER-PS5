# Browser section (#237)

EVO's Browser section shows web pages from an engine that runs **outside
EVO**, as separate payload processes. EVO starts and stops the engine, shows
its frames on the GPU and passes input in. A browser crash cannot take EVO
down. The first engine is ProsperoWeb (kvaps, GPL-3; our fork is
[sainsaji/ProsperoWeb](https://github.com/sainsaji/ProsperoWeb)); the WPE GPU
engine from Prospero Browser replaces it behind the same seam later.

Status: Phase 0 spikes S1 (engine lifetime), S2a (transport) and S2b (frames
drawn as a texture) passed on a PS5 Pro on FW 13.60 with the stock
ProsperoWeb v0.2.1 broker. S2b measured about 58 drawn frames a second at
1080p, 0.22 ms and about 0.9 MB per dirty-rect copy, EVO at 60 fps, and
`map_fail=0`.

## The seam

`include/evo_browser_engine.h` is engine-neutral. Today it has the lifetime
calls only: `start`, `running`, `stop`, `wait_stopped`. `load`, `input`,
`poll_events` and `acquire_frame`/`release_frame` come with S2 and Phase 1.
The ProsperoWeb backend is `src/evo_browser_prosperoweb.c`.

## Lifetime (S1)

```
EVO --raw ELF--> elfldr 127.0.0.1:9021 --> prosperoweb-broker.elf (root payload)
EVO <--TCP 127.0.0.1:17771, one connection, held--> broker --> WebProcess, NetworkProcess
```

- EVO writes the broker ELF to elfldr (`src/evo_elfldr.c`, shared with the
  NTFS helper), polls 17771 for up to 10 s, sends `{"op":"ping"}` and keeps
  the connection. Frames on that socket are a 4-byte big-endian length and a
  JSON object (`third_party/ProsperoWeb/src/ps5/spawn/broker_proto.cpp`).
- The broker lives exactly as long as that connection. When it closes, the
  broker SIGKILLs every child, closes its listeners and exits. So:
  - **soft close** (`evo-remote.sh quit`): on the first soft-close frame EVO
    closes the connection; before parking it waits up to 1.5 s for 17771 to
    refuse and logs `browser: broker gone ...`.
  - **kill** (`close`, PS button, crash): the kernel closes the socket, and
    the broker cleans up the same way.
  - a broker that never got EVO as its client exits after 30 s alone, or at
    once on EVO's park-time probe (connect and close).
- EVO never takes over a broker it did not start: if 17771 already answers at
  start, it logs that and does nothing.
- The broker ignores SIGPIPE, so the raw send (whose socket elfldr may hand
  the payload as stdout) is safe to close straight away.

**Spike trigger:** create `/mnt/usb0/evo_browser_spike`. Its first line may
name the broker ELF; the default is `/mnt/usb0/prosperoweb-broker.elf` (the
0.7 MB `prosperoweb_broker` CMake target, not the 176 MB `prosperoweb.elf`).

S1 passes when `evo.log` shows `broker pid N answers` after launch and
`broker gone` after `quit`, and `close` then shows a clean
`runtime layers released` in ShadowMount's log.

## Frame handoff (S2 design)

### Rendezvous: no `listen()` in EVO

EVO makes `socketpair(AF_UNIX, SOCK_SEQPACKET)`, keeps `sv[0]` and asks the
broker to spawn the engine host with `src_pid` = EVO's pid and
`src_fds` = `[sv[1]]`. The broker copies the descriptor into the child with
`rdup`, at fd 3. This is the route ProsperoWeb's own app-slot title proved on
hardware: a SEQPACKET end rdup'ed into the broker, data both ways, and
`SCM_RIGHTS` carrying `shm_open(SHM_ANON)` memory both ways, mapped and read
(`third_party/ProsperoWeb/docs/native-title.md`, "The IPC self-test passed").
EVO's own AF_UNIX `bind` is not needed (it fails under `/data`).

S2 runs in two steps:
1. **S2a, stock broker.** EVO sends the broker's existing `ipc-selftest` op
   (`src_pid`, `src_fd` = `sv[1]`), receives the broker's SHM_ANON object by
   `SCM_RIGHTS`, maps it and checks the `PRWB-BROKER-SHM` pattern. This proves
   the transport inside PPSA99039 with no fork change.
2. **S2b, frame test.** No fork change either: the stock broker spawns any
   ELF by path, so `helpers/frame_test.c` (built by
   `tools/build_frame_test.sh` into `output/evo_frame_test.elf`) is spawned
   with EVO's socket end at fd 3, the same route the engine host will take.
   It speaks the frame protocol below (`include/evo_browser_wire.h`) and
   paints a moving bar plus red, green and blue squares (byte order check).
   EVO draws it full screen (`evo_hui_render_webframe`) when
   `/mnt/usb0/evo_frame_test.elf` exists next to the spike trigger.

### Wire protocol (SEQPACKET, one message per packet)

Header: `u32 magic 'EVOB'`, `u16 version`, `u16 type`, `u32 payload bytes`,
little-endian.

| type | direction | payload |
|---|---|---|
| `HELLO` | host to EVO | width, height, stride, format, buffer count (2); the buffer fds ride along as `SCM_RIGHTS` |
| `FRAME` | host to EVO | buffer index, sequence, up to 16 dirty rects (x, y, w, h) |
| `RELEASE` | EVO to host | buffer index: EVO has copied it and the host may paint into it again |
| `BYE` | either | orderly shutdown |

Later types (Phase 1) carry `load`, `input`, `resize` and the page events.

- **Buffers:** two SHM_ANON objects of `stride * height` (1920x1080x4, about
  8.3 MB each), made by the host. The host paints only into a buffer EVO has
  released, so EVO never reads a half-painted frame.
- **Pixel format:** the host paints `kRGBA_8888`, premultiplied. That is what
  EVO's kit textures (`evo_agc_build_tsharp_rgba8`) and premultiplied blend
  expect, so EVO needs no swizzle. Skia's default N32 is BGRA in memory and
  would swap red and blue silently.

### Drawing in EVO

- SHM_ANON memory is CPU memory; the GPU samples only direct memory. So EVO
  keeps **two** linear RGBA textures in GPU direct memory
  (`evo_direct_mem_alloc_gpu`, 256-byte aligned, pitch rounded to 256, as
  `AgcBatch::make_texture` does) and alternates them, so the CPU never writes
  the texture the GPU is still reading.
- Each frame, on the frame loop: drain `sv[0]` with `MSG_DONTWAIT`, keep the
  newest `FRAME`, copy its dirty rects (plus the previous frame's, since the
  textures alternate) from the mapped buffer into the free texture,
  `evo_agc_runtime_cache_flush` those rows, swap, send `RELEASE`. A full
  1080p copy is about 8 MB; dirty rects keep the usual cost far below that.
- The kit gets an `update_texture(handle, rect, src, src_pitch)` next to
  `create_texture`, and the page view draws that handle as one quad, scaled
  and scrolled on the GPU.
- `sv[0]` carries `SO_SNDTIMEO`/`SO_RCVTIMEO`. A closed socket means the
  engine is gone: EVO shows a message, never blocks.
- Memory: the two mapped buffers cost EVO about 17 MB of flexible memory and
  the two textures about 17 MB of direct memory. Check `map_fail` in
  `evo.log` on every run.

S2 passes when the test pattern moves smoothly inside EVO, the frame time
stays steady, `quit` still ends with `broker gone`, and `map_fail` stays 0.
