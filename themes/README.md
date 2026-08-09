# Drop-in themes

Copy any `.theme` file here to `/mnt/usb0/evo_themes/` on the console. EVO
Player picks them up at startup and they appear in **Settings → THEME**.

No rebuild, no repack.

```
/mnt/usb0/
└── evo_themes/
    ├── sunset.theme
    └── nord.theme
```

Format and the full key list: [`docs/theming.md`](../docs/theming.md).

Anything you leave out inherits from the built-in `MIDNIGHT` theme, so a theme
can be three lines long.
