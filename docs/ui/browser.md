# Browser section (#237)

EVO's Browser section shows web pages from an engine that runs **outside
EVO**, as separate payload processes. EVO starts and stops the engine, shows
its frames on the GPU and passes input in. A browser crash cannot take EVO
down. The first engine is ProsperoWeb (kvaps, GPL-3; our fork is
[sainsaji/ProsperoWeb](https://github.com/sainsaji/ProsperoWeb)); the WPE GPU
engine from Prospero Browser replaces it behind the same seam later.

Status: Landed in v0.13.6 as an opt-in experimental feature behind **Settings → Experimental → WEB BROWSER** (defaults to off). When enabled, the Web section sits right of the navigation rail, rendered at sharp 2x on 4K displays and composited on Mesa through EVO's out-of-process GPU broker. Disabling the setting immediately hides the rail tab and releases engine resources. Remote control is supported via `evo-remote.sh pointer`, `weburl`, and `set web-browser on|off`.

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

**Dev remote.** `evo-remote.sh weburl <url>` opens a URL in the tab (http(s)
only: `data:` URLs show in the address bar but the engine does not load them).
`evo-remote.sh pointer <x> <y>` puts the free pointer at a 1920x1080 UI pixel
(rail included), as the left stick would; `key cross` then clicks there. A text
field opens EVO's keyboard, so with KEYBOARD INPUT = VIRTUAL KEYBOARD
`type --submit <text>` fills it (console 2026-11-05: prosperostore.net search).

**Idle repaints.** The engine host logs a 5 s damage report to
`/data/prosperoweb/log.txt` (`evo-host: damage 5s: needsDisplay ...`, the rects
that repeat most; nothing while a page is idle). Damage is tracked per rect: a
20 px CSS animation repaints 20x20. But pages with endless animations on large
elements (prosperostore.net's blurred background art, 30-61 s `breathe`/`drift`
keyframes) repaint the whole page about 34 times a second, CPU-bound: the engine
has no accelerated compositing (`wk_web_view.cpp`), so a transform animation is
a full repaint, where a desktop browser would move a GPU layer.

**Idle throttle.** After 2 s with no packet from EVO (buffer releases aside),
no load running and no fullscreen, the host sends at most 10 frames a second
(`evo-host: idle, ...` / `active, full frame rate` in its log; the damage
report counts `idle skips`). This cuts the frames EVO copies (prosperostore.net
at 1x: 34/s to 8/s), but not the WebProcess: it still uses about one core,
because WebKit keeps rendering the animation whatever the host takes.

**2x page.** EVO presents at 3840x2160, so `RESIZE` asks for scale 2
(`evob_size.scale`; an 8-byte `RESIZE` means 1). The host's view is then
3584x1888 at page zoom 2: the layout is still 1792 CSS px wide, the text is
drawn at 4K, and `POINTER` / `SCROLL` stay in EVO's UI pixels (the host
multiplies them). The host makes its shared buffers at 3840x2160 once; EVO maps
the whole buffer (its `fstat` size), not just the first `HELLO`'s rows. Each of
the kit's two page textures is ~29 MB, more than the 64 MiB pool has left, so a
texture over 16 MB gets direct memory of its own (`evo_direct_mem_map_gpu`);
the pool itself is unchanged. Cost: the WebProcess paints 4x the pixels. A
static page is fine (Wikipedia: ~0.65 core), but prosperostore.net's animated
page drops to ~2 frames a second at one full core.

**Idle animation pause.** On the idle switch the host runs a script that
pauses every running animation except CSS transitions (kept in
`window.__evoPaused`) and resumes them on the next input. A mirror of
prosperostore.net drops to ~0.25 core; the live site still uses about one core.

**GPU compositing (in progress, off unless `/data/prosperoweb/gpu` exists).**
The real fix for animations is GPU layers, on Sony's own GLES library
`libSceGLSlimVSH`, which the system browser's WebProcess uses. A payload can load it and create a context,
but its init asks Sony's compositor service (`/system/sys/AgcCompositor.elf`)
for memory. A payload is refused on its app category. With the category poked to
`0x1000000` (common_dialog) at struct proc `+0x4b0`, keeping the payload's own
authid, the service grants up to 48 MB (64 MB is refused) and GLSlim makes a
working context: `SlimGL`, OpenGL ES 2.0, max texture 4096. Do not borrow the
browser's authid: under it every direct-memory map fails `EPERM`. GLSlim talks
to the service through `libSceCompositeExt.sprx` (not `libSceComposite`). Never
call `sceCompositorInitWithProcessOrder` a second time: it zeroes the client
state, command ring included, before its "already initialised" check, and every
later call then says "disconnected from compositor server". With one init, the GPU runs our frames.

*Frame pacing.* `eglSwapBuffers` is GLSlim's frame boundary (a frame ended by
`glFinish` alone never lands). The service takes one frame per client per
60 Hz tick. GLSlim's submit flushes the command buffer to the service and only
then writes the frame's PostEvent and EndOfFrame commands, without flushing
them, so a frame only finishes at the next swap: readbacks were one frame
stale. GLSlim keeps two frame slots; when it reuses one whose GPU label is not
done, it waits in `sceCompositorWaitEndOfRendering`, and that wait never
fires for this client (60 s timeout, retried forever): that was the hang on
back-to-back swaps. Working frame end: take GLSlim's current slot before the
swap (context from libSceGLSlimVSH `0x13920`; slots at ctx `+0x8e8`, 0x200
each, index `+0x864`; in a slot, `+0x1d0` label pointer, `+0x1d8` target),
swap, call `sceCompositorFlush`, then spin until the label reaches its target.
30 frames run back to back at 60 fps with every readback current; the GPU
finishes about 0.1 to 0.3 ms after the tick for a light frame.

*Shaders.* GLSlim has no runtime compiler (`glShaderSource` is a stub). Its
`glShaderBinary` takes Sony-compiled shaders: small AMDGPU ELFs
(`.shader_text`, `.shader_header` with the `sl00` trailer, `.metadata` with
uniform names). NK WebKit's TextureMapper set is built into
`libSceNKWebKitRequirements.sprx`: 108 blobs named
`shaders/<613|616|main>/texmap/{v,f}<options>.{vert,frag}.bin`, found by the
SHA-1 of the name (13.60: table at `0x290070`). WebKit passes the shader's
own type as the binary format. `output/re/scripts/nkshaders.py` extracts them
(Sony's binaries: never commit them). Option 16384 is the Gaussian blur:
`u_gaussianKernel[6]`, `u_gaussianKernelOffset[6]`, int
`u_gaussianKernelHalfSize`, and `u_blurDirection` in texels. Measured at
1792x944: a two-pass blur of the whole page costs about 0.4 ms of GPU time,
the first frame that uses a program about 85 ms once, a full readback
3.5 to 4.5 ms. Option 1 is not a plain copy in this build (the blur program
with a one-tap kernel copies exactly). Clears seem to be applied lazily: a
texture seeded only with clears was not seen by later sampling, so the probe
seeds with `glTexSubImage2D`.
The service keeps a client's slot until the client sends Quit (command 1 on
`/system_tmp/GnmCompositor`), and a leftover slot needs a reboot, so the probe
sends Quit itself on every exit path, crashes included. Probe and helpers:
`projects/evoplayer/helpers/glslim_probe.c`, `proc_diff.c`, `proc_poke.c`,
built by `tools/build_glslim_probe.sh`.

*In the engine host* (fork branch `feat/evo-host`). With
`/data/prosperoweb/gpu` present, the WebProcess starts GLSlim before WebKit
(`src/ps5/glslim`: category poke, 48 MB grant, a 30-frame self-test) and the UI
turns accelerated compositing on. Patch `0022-webkit-ps5-glslim-compositing`
gives WebKit a display on GLSlim with pbuffer contexts, ends each frame with the
shell's frame end, and loads TextureMapper's programs from Sony's binaries in
`/data/prosperoweb/shaders` (uploaded by hand, never committed; a missing exact
program falls back to the uber pair, TextureCopy to the plain RGB program). The
compositor draws into an FBO; each frame is read back, rows reversed (GL's row
0 is the bottom; `PROSPEROWEB_GPU_FLIP=0` turns that off), swizzled to BGRA and
sent to EVO as an ordinary `Update`, acked by `DisplayDidRefresh`, so EVO's side
is unchanged. On the console at 1792x944: readback 1.5 ms plus swizzle 2.7 ms
per frame. Clips, rounded clips, opacity and rotated layers draw correctly.
Rules the console taught:

- Only the compositor thread touches GL. GLSlim aborts (SIGABRT, no message)
  when a second context is made current on another thread, which WebKit's
  `BitmapTexturePool` release timer did on the main thread with the sharing
  context. On PS5 that timer no longer borrows a context; the compositor thread
  releases unused pool textures after its readback, at most every 500 ms.
- The page is composited at 1x: the host caps `RESIZE`'s scale at 1 while the
  flag exists and EVO scales the page up. At 2x the 3584x1888 target alone is
  27 MB of the 48 MB, and the page's layers came out blank.
- Memory is still the open problem. prosperostore.net loses its hero text, and
  a page opened after it loses every composited layer; the same test page
  opened first draws completely. Next: log pool size and allocation failures,
  release textures when idle and on navigation, and shrink large layers.
- On a fatal signal the shell sends Quit first, then writes up to 96 code
  addresses from the interrupted stack to `glslim.txt` (image addresses as ELF
  offsets for `llvm-symbolizer`, system-module addresses raw). The console's
  signal context has rip at word 28 and rsp at word 31; the SDK's `ucontext_t`
  does not match it. elfldr maps no ELF header, so `__ehdr_start` must not be
  dereferenced. libc++'s verbose abort is overridden to log its message.

*Why not Mesa in the WebProcess.* A payload cannot drive the GPU itself, so a
Mesa driver (ps5-opengl or RADV) cannot run inside the WebProcess on its own.
Measured with `projects/evoplayer/helpers/agc_probe.c` (sent to elfldr with EVO
running, log `/data/prosperoweb/agc_probe.log`; FW 13.60, Pro):

- Direct memory: a payload gets 4 MB at most (8 MB fails `0x80020023`); the
  rest of the pool is EVO's. Flexible memory with CPU+GPU protection (`0x33`)
  maps up to 256 MB, but GPU access to it is untested.
- Loading `libSceAgcDriver.sprx` kills the payload at once, with no signal and
  no exit line. `libSceAgc.sprx` pulls it in and dies the same way.
- The sysmodules GLSlim needs load `libSceAgcVsh.sprx`: the command builders
  (`sceAgcInit` works, DMA and release-mem packets), but no submit.
- `libSceGnmDriver.sprx` loads, but its init ioctl on `/dev/gc` (`0xc008811b`)
  fails with EAGAIN, and its submit then crashes on a null pointer. Setting the
  category at `proc+0x4b0` to EVO's (0, game) changes nothing; the driver load
  then hangs instead.
- Likely cause: a payload starts with no GPU address space (`vm#0, dmem#0` in
  klog's EXEC line), and that is set when the process is created.

ps5-opengl has two more problems here: it opens VideoOut on its first draw
(`runtime_video_acquire`), which panics in a payload, and it is desktop GL
only, while WebKit's PlayStation port expects GLES.

*Mesa through EVO's GPU broker.* EVO, which is an app, owns the GPU and
submits for the WebProcess. The WebProcess runs Mesa EGL 1.5 + OpenGL ES 3.2
over Zink on RADV as a client of PS5_Vulkan's GPU broker: its GPU memory is
shared memory from EVO, and EVO submits its command buffers. No 48 MB grant,
no Sony shader binaries (WebKit's own GLSL compiles). This is the GPU path;
GLSlim stays behind its own flag as the fallback.

- *EVO side.* The broker's owner code is vendored in
  `projects/evoplayer/gpu_broker/` (PS5_Vulkan's `server.cpp`/`packet_io.c`,
  LGPL-2.1+; PS5_Mesa's `radv_ps5_platform.c`, MIT; `compat/` stands in for the
  few Mesa util headers, on pthreads). API: `include/evo_gpu_broker.h`. EVO
  holds the clients' address ranges (`0x240000000` + 512 MB, `0x3000000000` +
  16 GB, address space only) from `main()` on; RADV's platform init (a second
  `sceAgcInit`) runs before unjail, like EVO's other GPU inits. The broker
  starts on a worker thread when the Web section opens (about 11 ms).
  `/data/evoplayer/evo_gpu_broker_off` (or `/mnt/usb0/evo_gpu_broker_off`)
  switches it off and gives the ranges back at boot.
- *Handing the endpoint over.* Right after spawning the engine host, EVO sends
  it `EVOB_GPU_ENDPOINT` with a broker endpoint fd attached (cookie -1 and no
  fd when there is no broker). The host's spawn glue hands it to the next
  WebProcess as an inherited fd named by `PROSPEROWEB_GPU_FD` and answers
  `EVOB_GPU_CLIENT` (cookie, pid); EVO then sends a spare for the next
  WebProcess (at most 4 per host). With `/data/prosperoweb/mesa` present a
  WebProcess spawn waits up to 3 s for an endpoint.
- *Slots.* EVO registers each endpoint's cookie with the host's pid, then
  moves it to the WebProcess's pid. A reaper thread checks every 0.5 s
  (`kill(pid,0)` = ESRCH; the ProsperoWeb broker reaps its children) and calls
  `ps5_gpu_broker_close`, which frees the client's memory while EVO runs.
- *WebProcess side* (fork `feat/evo-host`, `src/ps5/mesa`, behind
  `/data/prosperoweb/mesa`). A constructor reserves the two ranges before
  WebKit's static initialisers; `ps5_mesa_start()` binds the endpoint, sets
  Zink, no shader cache, no gallium thread, raises every new thread's stack to
  2 MiB, and runs a self-test (EGL, ES 3 context, a GLSL ES 3.00 shader, an FBO
  clear read back). The GL link stubs resolve through `ps5_mesa_proc()` first,
  then GLSlim. Patch 0022 then makes surfaceless contexts, skips GLSlim's frame
  end and compiles TextureMapper's GLSL; the FBO, readback, row flip and
  `Update` path are shared with GLSlim. Pages stay at 1x for now. Log:
  `/data/prosperoweb/mesa.txt`.
- *The Mesa stack in the fork.* One relocatable object, `ps5_mesa_gpu.o`
  (`tools/build_mesa_bundle.sh`, run in the `prospero-gpu-dev:llvm18` image):
  Prospero Browser's Mesa/Zink/RADV, broker client and libps5platform
  pre-linked with `ld -r`; every symbol is local except
  `mesa_eglGetProcAddress` and `ps5_gpu_client_*`, so nothing clashes with
  WebKit's libraries. Against the fork's SDK v0.41.1 only `__dso_handle` stays
  open (the crt has it). The fork's CI fetches it from the fork's release
  `mesa-bundle-1` (`scripts/ps5/fetch-mesa-bundle.sh`, pinned by SHA-256);
  without it the build goes on with GLSlim and raster. The engine ELF grows
  from 177 to 205 MB. Licences: Mesa MIT, broker client LGPL-2.1+,
  libps5platform GPL-3.0+.
- *Traps the console taught.* Zink starts its submit thread at the first
  flush; on the default 64 KiB stack the process dies with no signal at the
  first `glReadPixels`. A 205 MB engine takes longer than 5 s to spawn, so EVO
  waits up to 30 s for that reply. `tools/build_mesa_bundle_test.sh` builds
  `helpers/mesa_bundle_test.c` like the fork's payload, to test the bundle
  outside WebKit through the spike (`/mnt/usb0/evo_gpu_broker`,
  `evo_browser_spike`, `evo_gpu_client.elf`).
- *Options.* Words in `/data/prosperoweb/mesa` (WebProcess, read once):
  `nodiff` sends every frame whole; `pace=N` is the shortest time between two
  frames in ms (default 16, 0 = none); `scale2` keeps 2x pages (without it the
  host holds Mesa at 1x, like GLSlim). `/data/prosperoweb/idle_throttle` brings
  back the host's idle mode (2 s without input: page animations paused, 10
  frames/s, and with Mesa the UI side holds the frame ack to 100 ms); it is off
  by default, so animations run at full rate. `/data/evoplayer/evo_gpu_host_mb`
  (EVO) sets the host-visible bytes each broker client may hold, 64 to 3072 MB
  (default 128, `PW_GPU_LIMIT` in the vendored `protocol.h`).
- *Send only what changed.* The WebProcess keeps the frame it sent last and
  compares the next read-back with it (16-row bands, 64-pixel blocks); only
  the changed rects travel, in a bitmap the size of their bounds, an unchanged
  frame is not sent, and a frame starts at most every `pace` ms. Surface stats
  (frames, sent, unchanged, render/readback/diff/copy ms) go to `mesa.txt`
  every 5 s. `ps5_ui_frame_interval_ms` (patch 0022, UI side) delays each
  frame ack; evo-host sets it only in idle mode.
- *Trap: the broker budget.* At 2x the WebProcess hit 127 of 128 MB
  host-visible (`stage=client_budget` in `/data/evoplayer-gpu/gpu-broker-results.txt`),
  retried the failed allocations until it died, and the next WebProcess could
  not bind (`ps5_gpu_client_bind` -> 5). It is not a console limit (flexible
  memory had 679 MB free). 384 MB runs prosperostore.net at 2x with no failures.
  Pick the default before shipping.
- *Measured* (2026-11-06, PS5 Pro, FW 13.60, prosperostore.net). EVO stays at
  60 fps. WebProcess CPU:

  | | raster 2x | Mesa 1x | Mesa 2x |
  |---|---|---|---|
  | idle mode (animations paused, 10 frames/s) | 0.75-0.93 | 0.31 | 0.67-0.72 |
  | animations on | 0.99 at ~3 frames/s | | 1.40 at ~14-20 frames/s |

  Before the changes above Mesa was 1.06 idle. With animations on Mesa draws
  about 5x the frames for 1.4x the CPU (about 4x cheaper per frame), but
  nothing is saved yet: raster is CPU-bound at 3 frames/s. The animation
  redraws nearly the whole page each frame (6.5 of 6.8 Mpx). Mesa 2x per frame:
  render 22-34 ms, readback 3.8, diff 5.3, copy into EVO's buffer 15; evo-host
  uses 0.20 core against 0.04 on raster.

  *Correction (2026-11-06, later).* The Mesa numbers in the first draft of that
  table were taken while EVO showed the home screen, not the Web section (the
  engine keeps its WebProcess across runs and `weburl` alone does not open the
  section), so they were not valid. Open the Web section and check with `ui`
  that focus is `#web-page-content` before measuring. The page also needs the
  engine restarted for a changed `mesa` flag (a full quit, close, launch).

### Mesa: what we know now (2026-11-06)

- *Whole-frame fast path* (patch 0022, `ps5ReadBackAndSend`): when most of the
  page changes it flips on the GPU (`glBlitFramebuffer`), reads back
  `GL_BGRA_EXT` straight into the sent bitmap and skips the CPU diff and
  swizzle; every 8th frame goes the diff way to re-measure. The "most of the
  page" test compares the *bounds* of the changed rects (more than 32 rects
  are merged into their bounds before sending), not the summed area: with the
  summed area the path switched itself off after ~10 s. `nofast` turns it off.
- *Broker budget.* At 2x (3584x1888) the first scroll of a busy page with the
  budget `/data/evoplayer/evo_gpu_host_mb` at 384 stalled one frame for 15.8 s
  (a GL error, most likely out of GPU memory), the WebProcess missed its IPC
  deadline, was killed and respawned (15 s to bind the GPU again): the page
  hangs. 768 does not. Use at least 768 for 2x pages; 512 is untested. The
  code default is not changed yet.
- *Idle frames.* A static page (Wikipedia, nothing animating) is rendered and
  read back at ~62 fps, ~95% of the frames unchanged, ~0.5-0.6 core spent
  comparing identical frames (render 1.2 ms, readback 2.4, diff 6.4 per
  frame). Not fixed: the host should draw only when WebKit reports damage.
- *EVO's view size* is sent when the Web section opens (`EVOB_RESIZE`, scale
  2); an engine that never saw it stays at its default 1920x1080.
- *Endpoint cap (fixed 2026-11-06).* EVO hands the engine host a spare GPU
  endpoint for the next WebProcess. `GPU_HOST_MAX_EPS` (4) used to count every
  endpoint ever sent, so from the 5th WebProcess of a session the host got
  `endpoint -1`, the WebProcess started with no GPU, exited (exit 0) within
  seconds and the page stayed white. It now counts only spares waiting at the
  host (`handle_gpu_client` drops it when a WebProcess takes one). Symptom to
  look for: `gpu: endpoint -1` in `evo.log`, or a spawn with `fds=[..->3]`
  only. The broker host budget default is now 768 MB in code
  (`/data/evoplayer/evo_gpu_host_mb` still overrides it).
- *Spawn time.* Each new WebProcess (every cross-site navigation, then a
  prewarm) takes 2-10 s to spawn through the broker (1.9 s idle, 6-10 s with a
  busy page open). Cause not found yet.
- *Blank card icons under Skia GPU (fixed, fork 003caf7).* Icons came out
  blank or as noise. `HAVE_GL_FENCE` is not defined for PlayStation, so
  `GLFence::create()` returns null and `SkiaGPUAtlas::uploadImages` had no
  sync: the Skia worker threads (own GL contexts) sampled the image atlas
  before the upload reached the GPU. Now `glFinish()` after the upload when
  there is no fence (patch 0022). Found by bisecting a copy of the real page
  (`tools/bench/ps_*.html`, `<base href>` to the site) with one CSS feature
  removed each; the noise gave the race away.

### Scroll benchmark

A page that does not animate scrolls fine on every setting, so the numbers
that count come from an animated page.

- *Pages* (fork `tools/bench/`): `article.html` (long text page: headings,
  floated images, tables, quotes) and `cards.html` (store-like grid), the same
  file, the type comes from the file name. Both animate: a spinner, a sliding
  bar and a marquee in the header (compositor-only), moving gradients,
  pulsing shadows and shimmer (repaint), a script-driven canvas. Text and
  colours come from a seeded generator. `file://` pages do not run scripts in
  the engine: serve the folder from the PC (`python3 -m http.server 8123
  --bind 0.0.0.0` in `third_party/ProsperoWeb/tools/bench`).
- *Driver.* Dropping `/data/prosperoweb/bench_go` (`speed=800 secs=30 warm=3`)
  makes the engine host scroll the open page with wheel events (the path a
  pad scroll takes) at `speed` UI px/s, 60 events a second, down for half the
  time and up for the other half. Touching `/data/prosperoweb/cpu_now` makes
  every process print its CPU line at once; the host touches it at both ends,
  so each process reports exactly the scroll window. The page's own report
  (page fps, px/s scrolled, frame gap p50/p95/worst, frames over 50 ms) is
  drawn on the page at the end; screenshot it.
- *Run it:* `tools/webbench/runbench.sh article 800 30` (add `NAV=1` the first
  time to open the Web section). It prints the host lines, the CPU of the
  window, the surface stats and EVO's frame counts, and leaves the screenshot
  in `output/screenshots/latest.png`.
- *Animation check:* `tools/webbench/anim_one.sh <bg|canvas|shadow|spin|text>`
  opens one animation per page (`animtest.html` copied to `t_<kind>.html`) and
  diffs three screenshots; for flicker, watch the screen.

Results, 2x (3584x1888), article page, 800 px/s for 30 s, PS5 Pro:

| | WebProcess CPU | host CPU | frames to EVO | page worst frame |
|---|---|---|---|---|
| Mesa, Skia on the CPU | 1.41 core (user 13.9 s, **sys 28.5 s**) | 0.09 | 6.5 fps | 407 ms (p95 367) |
| Mesa, Skia on the GPU | 1.52 core (user 20.9 s, sys 24.6 s) | 0.41 | 31 fps | 114 ms (p95 65) |

The same page without animations: Mesa 2x, 43.7 fps, WebProcess 0.66, host
0.72, worst frame 63 ms. Two things stand out: more than half of the CPU is
*kernel* time (a fresh 27 MB shared bitmap per frame: page faults and
mapping), and the host burns as much as the WebProcess when frames flow. Not
measured yet on this benchmark: raster, Mesa 1x, and Skia GPU after the canvas
change below.

### Skia on the GPU (opt-in: the word `skia` in `/data/prosperoweb/mesa`)

Skia's GL backend was already compiled into the fork's `libSkia.a`; what
was missing was the PlayStation glue. Patch 0022 builds WebKit's upstream GPU
tile path for PS5 (`PlatformDisplaySkia.cpp`: `ps5_skia_gpu()`, a Skia GL
context per thread, 48 MB resource cache each; `WebProcessPlayStation.cpp`
turns `setCanUseAcceleratedBuffers` on). Mesa dispatches GL per thread
(Sony's GLES library does not), which is why this works here. The
`SkiaGPUWorker` threads paint tiles into pool textures that the compositor
takes whole (`swapTexture`) or copies in part (`glCopyTexSubImage2D`).

- A tile's GPU work is awaited on the painting thread (`submit` with a CPU
  wait) instead of handing the compositor a fence: the fence did not explain
  the faults below, the wait is kept as the safer choice.
- `tiles:` lines in `mesa.txt` (every 5 s) count whole swaps, partial copies,
  updates without a texture and CPU uploads.
- *Canvas.* One animation per page on the console (`tools/webbench/anim_one.sh`):
  moving gradient, pulsing shadow and text work; a `<canvas>` flickered and
  never updated with the GPU canvas backend (it draws on the page's main
  thread). Canvases now stay on the CPU (`ImageBufferSkiaAcceleratedBackend`
  returns null for them under PS5); tiles keep painting on the GPU. Also seen:
  a small flicker at the start, end and middle of the transform-only spin
  page. Open.

### Building the engine

CI takes 7-8 minutes a push (the disk cleanup now runs in the background).
`tools/local-build/` in the fork builds the same engine in Docker on the PC
(`run.sh setup`, then `run.sh build`; the build tree and ccache live in the
`pw-work` volume). The first build has no ccache (GitHub's cache cannot be
downloaded and would not match anyway) and compiles all of WebKit, hours;
after it a one-file change should rebuild in a few minutes. See its README.

**Not yet:** playback from pages (Phase 3), downloads, more than one tab,
site favicons on the speed dial (they are emoji now, which the kit fonts show
as `?`), start page and About text still Prospero Browser's ("Native WebKit
Engine"), a restart after the engine dies, the RmlUi fallback rail (it has no
Web entry).

**Spike trigger.** With `/mnt/usb0/prosperoweb.elf` present, the spike spawns
the engine host instead of the frame test. Line 2 of
`/mnt/usb0/evo_browser_spike` is the page to load (default
`https://example.com/`). EVO logs every `EVENT` and sends one `SCROLL` when
the first load finishes. Phase 1 passes when `evo.log` shows the URL, title
and load-finished events, the page appears and moves after the scroll,
`quit` ends with `broker gone`, `close` is clean, and `map_fail` stays 0.
