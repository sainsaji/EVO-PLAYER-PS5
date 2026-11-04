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
`map_fail=0`. Phase 1 (the `--evo-host` engine host) is written and
host-tested; it is not yet built in the fork's CI or run on the console.

## The seam

`include/evo_browser_engine.h` is engine-neutral: the lifetime calls
(`start`, `running`, `stop`, `wait_stopped`), the frames
(`frame_acquire`/`frame_release`), and what the Web section uses: `open`,
`state`, `load`, `nav`, `pointer`, `scroll`, `key`, `text`, `resize`,
`set_visible` and `poll_event`. All of them run on the main thread and never
block: commands go out with `MSG_DONTWAIT` and are dropped (and logged) when
the engine is not reading. The ProsperoWeb backend is
`src/evo_browser_prosperoweb.c`.

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

Phase 1 adds the control and event types below. Strings are raw UTF-8 at the
end of the payload, no NUL. A packet is at most 8 KB (`EVOB_MAX_PACKET`, the
kernel's default SEQPACKET limit); the sender cuts a longer string and logs it.

| type | direction | payload |
|---|---|---|
| `LOAD` | EVO to host | the URL |
| `NAV` | EVO to host | back, forward, reload or stop |
| `POINTER` | EVO to host | x, y, move/down/up, button (page pixels) |
| `SCROLL` | EVO to host | x, y, dx, dy (a wheel step; positive dy scrolls down) |
| `KEY` | EVO to host | DOM key code, modifiers, pressed, then the text it types |
| `TEXT` | EVO to host | text for the focused field (EVO's keyboard and IME) |
| `RESIZE` | EVO to host | width, height; the host answers with a `HELLO` with `nbuf` 0 (same buffers, new size, never larger than the buffers) |
| `VISIBLE` | EVO to host | 0 pauses painting (playback), 1 resumes with a full repaint |
| `JS` | EVO to host | a script for the main frame |
| `EVENT` | host to EVO | kind, arg, text: url, title, load started/finished/failed, back/forward state, text focus, script message (JSON), window.open URL, cursor, fullscreen, engine error |

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

## Engine host (Phase 1, our ProsperoWeb fork)

`prosperoweb.elf --evo-host` (branch `feat/evo-host` of
[sainsaji/ProsperoWeb](https://github.com/sainsaji/ProsperoWeb)) is the real
engine host. It runs in place of the frame test, with EVO's socket end at
fd 3, and has no SDL window, no `chrome.html` and no shell controller.

- `src/frontend/ps5/evo_host.{h,cpp}`: one web view from ProsperoWeb's own
  engine factory. Its `WKPagePaint` output goes into a private ARGB32 cache,
  dirty rects only; the changed rects are copied into whichever shared
  buffer EVO has released, with R and B swapped (`kRGBA_8888`, premultiplied).
  The page events go to EVO as `EVENT`s.
- `src/ps5/evohost/`: the wire header (an identical copy of EVO's
  `include/evo_browser_wire.h`; keep the two in step), message packing and the
  two-buffer bookkeeping. Each buffer tracks what it missed while EVO held
  it, so after a publish it equals the cache. Host tests:
  `tests/ps5/test_evo_host.cpp` (`ps5.evo_host` in the fork's CI).
- `main.cpp` picks the role from `--evo-host` (or `--role=evo-host`). argv[0]
  must be the ELF's absolute path: the host re-runs that ELF as its
  WebProcess and NetworkProcess.

**Lifetime.** The broker serves one client and a new connection replaces the
old one. The host's WebKit connects to 17771 to spawn its WebProcess and
NetworkProcess, so it takes over the connection EVO opened. From then on fd 3
keeps everything alive: EVO's soft close sends `BYE` and shuts its end down,
a kill or crash closes it, and either way the host leaves within 200 ms
without WebKit's teardown. Its broker connection closes with it, and the
broker kills the children and exits. EVO's park-time wait for 17771 to refuse
is unchanged. The broker needs no change.

**Profile and assets.** The host uses ProsperoWeb's own `/data/prosperoweb`
(log, profile, fonts, certificates). A normal ProsperoWeb install puts the
assets there, which is one more reason to run it standalone first.

**Not in Phase 1:** the audio server and video overlay (a page's own video
does not play yet; EVO plays video from Phase 3), the ad-block add-ons, and
`SCRIPT` messages longer than one packet.

## Web section (Phase 2)

The rail has a **Web** entry (globe icon) after Providers. Its section order
lives in `ScreenManager.cpp` (`kSecWeb`, `kSecSettings`, `kSecAbout`) and in
the kit's `rail_items`; keep the two in step.

The section is **Prospero Browser's own UI**, copied into EVO, not a screen
EVO draws itself: speed dial, address bar and keyboard, tab strip, Options
menu, bookmarks, history and settings. It sits right of EVO's rail (128 px,
Web lit), laid out for 1792x1080.

- **The copy** (`projects/evoplayer/browser/`, from `D:\Projects\Prospero
  Browser\src`): `browser/` (BrowserApp, tabs, page view, bookmarks, history,
  settings, cursor), `web/` (PageEngine interface, the built-in renderer for
  `about:` pages), `ui/browser_ui.cpp` (the kit drawing). Every line EVO
  changed is marked `EVO:`: the hard-coded 1920 widths now use the app's
  width, the header's right-hand buttons moved inside 1792 px, the page
  viewport draws EVO's page texture, storage is `/data/evoplayer/browser/`,
  tabs are capped at 1. Bring Prospero fixes over by diffing against it.
- **EVO stand-ins**, so the copies compile unchanged: `net/download_manager.hpp`
  (no downloads yet: a notice says so), `net/http_client.hpp` (fails at once:
  every web page goes to the engine), `browser/page_texture.hpp` (no GL
  texture), `platform/ps5/webkit_dialog.hpp` (no system browser), and
  `evo_shims.cpp` (the kit's `hui::sys::log` into `evo.log`).
- **`evo_page_engine.cpp`**: Prospero's `web::PageEngine` on this seam. The
  host has one web view, so the newest page owns it and an older one goes
  quiet (Prospero restores a session by opening the saved tab, then closing
  its start tab). Frames do not pass through it: the kit copies the host's
  buffers into its textures (`WebFrame`) and `evo_browser_page_texture()`
  hands that texture to the page viewport.
- **Kit glue** (`ui_kit/src/evo_hui_web.cpp`): creates the app, draws it
  shifted right of the rail, takes pad input (`evo_hui_web_input`,
  `evo_hui_web_sticks`). **Core screen** (`WebScreen`, `ScreenId::WebHome`):
  passes the pad in, and opens EVO's keyboard when a page field the user just
  clicked (within 1.5 s) takes focus; the text goes in with `TEXT`.
- **D-pad Left opens EVO's rail** from the leftmost header button, from the
  speed dial's first column, and always on a web page (Prospero would send
  the page an arrow key, and the D-pad would never leave it).
- **Host preview:** `hui_preview.sh` renders `hui_web`, `hui_web_menu` and
  `hui_web_address` with engine stubs (`tools/hui_preview/web_stubs.cpp`).
- **Engine files:** `prosperoweb-broker.elf` and `prosperoweb.elf` are looked
  for in `/mnt/usb0`, then `/data/evoplayer/browser` (until they are bundled
  in the `.ffpfsc`).

Prospero's own controls apply (pointer on the left stick, page scroll on the
right stick and triggers, D-pad to the page as arrow keys, Triangle for the
address, Options for the menu, L1/R1 tabs, Circle back). Leaving the section
sends `VISIBLE 0`, so the engine stops painting while EVO shows anything else.

**Not yet:** playback from pages (Phase 3), downloads, more than one tab,
emoji icons on the speed dial (the kit fonts show `?`), a restart after the
engine dies, the RmlUi fallback rail (it has no Web entry).

**Spike trigger.** With `/mnt/usb0/prosperoweb.elf` present, the spike spawns
the engine host instead of the frame test. Line 2 of
`/mnt/usb0/evo_browser_spike` is the page to load (default
`https://example.com/`). EVO logs every `EVENT` and sends one `SCROLL` when
the first load finishes. Phase 1 passes when `evo.log` shows the URL, title
and load-finished events, the page appears and moves after the scroll,
`quit` ends with `broker gone`, `close` is clean, and `map_fail` stays 0.
