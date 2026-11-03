# Packaging and deploying

One path ships EVO: build the app module with `scripts/package-app.sh`, PFS-pack
it into `PPSA99039.ffpfsc`, and put that image in `/data/homebrew/` with
`scripts/deploy-app.sh`. The older routes (homebrew bundles, a fake-SELF ELF
installed with `sceAppInstUtil`, hbldr launches) were dropped: they reach none of
the features that need an app module ([../architecture/app-module.md](../architecture/app-module.md)).
`scripts/package-pkg.sh` is a leftover from that era; it is not part of any
workflow, and a signed fPKG needs Sony's licensed `prospero-pub-cmd`, which this
repo does not have ([../proprietary.md](../proprietary.md)).

```bash
docker compose run --rm ps5-dev ./scripts/package-app.sh --ffpfsc --usb-remote
docker compose run --rm ps5-dev ./scripts/deploy-app.sh --ffpfsc    # sha256-verified
./tools/evo-remote.sh launch      # ShadowMount+ v1.7 does not launch an updated image
```

Never build or deploy by hand over a running EVO. The close sequence and the
bounded hardware cycle are in [../console/hardware-cycle.md](../console/hardware-cycle.md).

## `package-app.sh`

| Flag | Effect |
|---|---|
| `--ffpfsc` | also PFS-pack the app folder into `output/app/PPSA99039.ffpfsc` with MkPFS |
| `--zip` | also write `PPSA99039.zip`, the folder-title archive (the layout Kodi PS5 uses) |
| `--usb-remote` | add the scriptable FTP dev remote (`evo_cmd`, `evo_status`, `evo_ui.json`, verbose decoder log). **Always use it for testing; off in releases** |
| `--pro-memory` | set `attribute3` bit 22 in the packaged `param.json` only, so a PS5 Pro gives the app about 1.2 GiB more direct memory (#143) |
| `--virtual-keyboard` | debug: always use EVO's own keyboard, never the system IME that the dev remote can neither press nor see |
| `--breadcrumbs` | pop each boot-trace line as an on-screen notification (off by default; klog carries the same lines) |
| `--probe` | build the sandbox probe instead of the player |
| `--rebuild-libc` | force-regenerate the clean-room `libc.prx` |
| `--no-native-secondary`, `--no-native-secondary-4k`, `--no-native-10bit` | rollback switches for the resident decoder set; the matching positive flags are no-ops kept for old scripts |
| `--agc` | accepted and redundant: bare-metal `sceAgc` is the only render path. `--gl`, `--no-gl`, `--gl-smoke` and `--gl-hdr-probe` were removed with the OpenGL path and fail with that explanation |

What the script does, in order: validate `param.json` (`titleId` and the version
format); build the native-app converter against static zlib; compile EVO; patch a
build-local copy of `librmlui.a`; build the PRX import stubs from
`tools/native-app/stubs/prx/*.syms` (refusing dead entries); link; convert the
PIE into a PS5 module; sign it to `eboot.bin` (FSELF); assemble the app folder; and,
with `--ffpfsc`, pack it. The embedded UI assets are regenerated from `assets/`
on every build by `tools/bundle_rml_assets.py`.

Output:

```
output/app/PPSA99039/
├── eboot.bin                 FSELF-signed. Reads as opaque "data" to file/readelf;
│                             inspect it with `ps5-native-tool self --inspect`
├── sce_sys/{param.json, icon0.png, pic0.png, pic1.png}
├── sce_module/libc.prx
└── assets/                   loose copy of the embedded bundle (CI checks it is not empty)
output/app/PPSA99039.ffpfsc   the same files in a PFS image
output/app/.build/eboot.elf   the pre-sign ELF, the one to check with readelf
```

The folder and the image carry byte-identical `eboot.bin`, `param.json` and
`libc.prx`; the image is only a different container. MkPFS packs with
`--compression-backend zlib`: its default ISA-L streams made the console read some
64 KB blocks as the block before them (strings turned into OpenSSL names, Recent
came up empty, depending on the build's content). If that shows again, repack the
same folder before suspecting code.

## `deploy-app.sh`

```bash
./scripts/deploy-app.sh --ffpfsc          # upload the image
./scripts/deploy-app.sh                   # upload the folder instead
./scripts/deploy-app.sh --ffpfsc --fresh  # fresh install: wipe every EVO file first (tools/wipe-evo.py)
./scripts/deploy-app.sh --undeploy        # remove the staged title and image
```

- The image uploads as `<TID>.ffpfsc.upload`, is read back and sha256-compared with
  the local file, and is renamed into place only on a match. A mismatch deletes the
  upload, so ShadowMount+ never sees bad bytes. For a folder deploy, `eboot.bin` and
  `param.json` go up last, so a half-finished folder is never mountable.
- It deletes `/mnt/usb0/{evo.log, evo_status, evo_report.txt}` so each
  launch starts with a fresh log.
- **It refuses to run while EVO is resident.** It asks ShadowMount's
  `/data/shadowmount/debug.log` (via `tools/evo_lifecycle.py`): started and not
  released means resident, a parked EVO included. A heartbeat that is still moving
  is refused outright and `--force` does not override it. `--force` only applies
  when there is no heartbeat to read.
- It never launches anything. **ShadowMount+ v1.7 test-mounts an updated image and
  unmounts it again; it does not launch it.** Launch from the Games row or with
  `evo-remote.sh launch`.
- `--fresh` goes through `tools/wipe-evo.py`: the app, `/data/evoplayer` (settings,
  logins, playlists, calibration) and `/mnt/usb0/evo*`. A dry run
  (`python3 tools/wipe-evo.py <host>`) lists what would go; `--do` deletes. It never
  touches other homebrew, games or non-EVO USB files.

## CI and releases

`.github/workflows/build.yml` and `release.yml` run the same container and scripts.
Release and versioning rules are in [release.md](release.md).
