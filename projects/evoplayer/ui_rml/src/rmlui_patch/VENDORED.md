# Vendored + patched — RmlUi rounded-corner tessellation (#68)

`GeometryBackgroundBorder.{cpp,h}` are copies of RmlUi 6.2's
`Source/Core/GeometryBackgroundBorder.{cpp,h}` (the same tree checked out at
`build/rmlui-host/RmlUi/`, and the same release the prebuilt PS5
`librmlui.a` in the pacbrew sysroot is built from — the public headers are
byte-identical, which is what makes the archive-member swap below safe).

Two deliberate deviations from verbatim:

1. **The fix.** `GetNumPoints()` — how many points RmlUi puts on a 90° corner
   arc:

   ```
   -  return Math::Clamp(3 + Math::RoundToInteger(R / 6.f), 2, 100);
   +  return Math::Clamp(4 + Math::RoundToInteger(R / 2.f), 2, 128);
   ```

   Upstream gives a 20 px radius **5 segments**, an inscribed polygon that sags
   `R·(1−cos 9°) ≈ 0.25 px` inside the true circle at each chord midpoint. A
   quarter-pixel geometry error is a ±25% coverage error in the antialiased
   edge pixel, repeating every ~6 px along the arc — which is exactly the
   faceted, bumpy corner #68 is about. It is a *geometry* error, so MSAA or a
   better rasteriser cannot fix it; only more points can. At `R/2` a 20 px
   corner gets 13 segments and sags 0.036 px (~4% coverage error), for ~2.3×
   the corner triangles — nothing next to a full-screen document.

2. **Include paths.** The upstream `"../../Include/RmlUi/Core/..."` relative
   includes became `<RmlUi/Core/...>` so the file compiles from this directory
   against whichever RmlUi include tree the build already uses (the host
   checkout, or `$PS5_HBROOT/include` on the device).

## How it reaches each build

- **Host preview** (`tools/uiview_playback_rml.sh`): compiled straight into the
  harness binary. Every symbol in this TU is exported from `librmlui.so` with
  default visibility, so the executable's copies preempt the library's for the
  whole process. (`GetNumPoints` itself is inlined inside the TU and not
  exported — which is why replacing the *file* is the unit that works, not the
  one function.)
- **Device `.ffpfsc`** (`scripts/package-app.sh`): compiled with the PS5
  toolchain and `llvm-ar r`'d over `GeometryBackgroundBorder.cpp.o` in a
  build-local **copy** of the pacbrew `librmlui.a`; the copy is what gets
  linked. The upstream archive in the container image is never modified.

To update RmlUi: re-copy both files from the new checkout, re-apply the two
deviations above, and re-verify `tools/uiview.sh --all`.
