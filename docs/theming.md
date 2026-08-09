# Theming EVO Player

Every colour and spacing value the UI draws with lives in one struct
(`evo_theme`, in [`pp/include/evo_theme.h`](../projects/evoplayer/pp/include/evo_theme.h)).
Screens ask for tokens by meaning — `surface`, `accent`, `text_secondary` —
never for a literal colour, so swapping the theme restyles the whole player
without touching drawing code.

There are two ways to get a theme in:

1. **Built-in** — compiled into the ELF. Four ship today.
2. **`.theme` file on USB** — dropped in `/mnt/usb0/evo_themes/`, discovered at
   startup. No rebuild, no repack. This is the plug-and-play path.

Switch themes in **Settings → THEME** (press ✕ to cycle). The choice is saved
and restored on the next launch.

---

## Built-in themes

| Name | Character |
|---|---|
| `MIDNIGHT` | Deep navy, cyan accent. The default. |
| `CARBON` | Neutral greys, near-monochrome. |
| `EMBER` | Warm charcoal, amber accent. |
| `AURORA` | Teal-green, mint accent. |

---

## Writing a `.theme` file

Create `/mnt/usb0/evo_themes/mytheme.theme`. Format is `key = value`, one per
line. `#` starts a comment **except** directly after `=`, where it introduces a
colour literal.

```ini
name = SUNSET

bg_top    = #1A0F1EFF
bg_bottom = #0B0710FF
accent    = #FF7A5CFF

radius = 14
row_h  = 96
```

Colours are `#RRGGBB` or `#RRGGBBAA`. Alpha defaults to `FF` when omitted, and
**alpha is meaningful** — `shadow` and `scrim` rely on it, and an opaque
`shadow` will paint a hard black slab under every card rather than a soft one.

**Any key you leave out inherits from `MIDNIGHT`.** A three-line file is a
valid theme. You do not need to specify all 23 keys to change the accent.

### Colour keys

| Key | Used for |
|---|---|
| `bg_top`, `bg_bottom` | Page background gradient |
| `scrim` | Darkening laid over artwork, video and the footer bar |
| `surface`, `surface_alt` | Resting card fill (top / bottom of gradient) |
| `surface_sel`, `surface_sel_alt` | Selected card fill |
| `border`, `border_sel` | Hairline around a card |
| `shadow` | Drop shadow beneath cards — **alpha matters** |
| `accent` | Icons, rails, selected values, header marks |
| `accent_soft` | The accent as a diffuse background bloom |
| `accent_alt` | Secondary accent, used sparingly |
| `text_primary` | Row titles |
| `text_secondary` | Row descriptions |
| `text_muted` | Footers, hints, disabled state |

### Metric keys

Pixels, authored at 1080p.

| Key | Meaning | Default |
|---|---|---|
| `radius` | Card corner radius | 14 |
| `border_px` | Hairline width | 1 |
| `shadow_px` | Drop shadow spread | 12 |
| `rail_px` | Accent rail on the selected card | 4 |
| `row_h` | Card height | 96 |
| `row_gap` | Gap between cards | 14 |
| `pad_x` | Inner horizontal padding | 28 |

---

## How discovery works

`evo_theme_init()` loads the built-ins, then scans `/mnt/usb0/evo_themes/` for
`*.theme` and appends whatever parses. The cap is `EVO_THEME_MAX` (12 total).
A file that fails to open is skipped silently — a bad theme cannot stop the
player from starting.

The active theme is persisted **by name**, not by index. That is deliberate: a
USB theme present on one boot and absent on the next would otherwise shift
every index after it and silently select a different theme. An unknown saved
name falls back to the default.

## Notes for anyone editing the drawing code

- Colours are `0xAABBGGRR` to match the framebuffer. A raw `0xFF00D7FF` is
  **yellow** here, not cyan — build colours with `EVO_RGBA(r,g,b,a)` or
  `RR_BGRA(r,g,b,a)`.
- Draw through [`evo_ui.h`](../projects/evoplayer/pp/include/evo_ui.h)
  (`evo_ui_card`, `evo_ui_round_rect`, `evo_ui_circle`, `evo_ui_hline`) rather
  than filling rectangles by hand. Those primitives rasterise from signed
  distance fields, so edges carry true analytic coverage and corners stay
  smooth. Hand-rolled fills are how the UI got its staircased edges in the
  first place.
- Never hardcode a colour in a screen. If a token is missing for what you need,
  add it to `evo_theme` and to all four built-ins.
