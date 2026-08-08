# PS5 packaging

**PKG packaging is not a dependency of the ELF workflow.** Nothing in
`build.sh` or `deploy.sh` calls `package-pkg.sh`. Develop against ELFs; package
only when you want something installable.

## PS4 tools do not work here

The difference is structural, not cosmetic:

| | PS4 | PS5 |
|---|---|---|
| App metadata | `sce_sys/param.sfo` (binary SFO) | `sce_sys/param.json` (JSON) |
| Executable | `eboot.bin` (SELF) | `eboot.bin` (SELF, different header/authinfo) |

A PS4 fPKG builder emits the wrong metadata format outright. Assume nothing
carries over.

## The three routes

### 1. Homebrew bundle — recommended, no proprietary tooling

```bash
./scripts/package-pkg.sh --format homebrew output/elf/hello_world.elf
```

[`ps5-payload-websrv`](https://github.com/ps5-payload-dev/websrv) scans
`/data/homebrew`, `/mnt/usb*/homebrew` and `/mnt/ext*/homebrew` for:

```
<name>/eboot.elf              the payload
<name>/sce_sys/icon0.png      icon shown in the launcher
<name>/homebrew.js            optional - custom UI / argv
```

No signing, no encryption, no PKG. This is how essentially all
ps5-payload-dev homebrew is actually distributed.

Install:

1. Extract `output/pkg/<name>-homebrew.zip` onto a USB stick as
   `/homebrew/<name>/`
2. Plug it into the console, jailbreak, start `ps5-payload-websrv`
3. Open `http://<ps5>:8080/index.html` and launch

### 2. Fake-SELF application — appears on the home screen

```bash
./scripts/package-pkg.sh --format app output/elf/evoplayer.elf \
    --title-id FAKE00001 --title "EVO Player" --icon assets/icon0.png
```

Registers a real system application with a TITLE_ID. This is what
ProsperoPlayer's "Media tile" does.

> **You cannot simply convert a payload into an app.** `make_fself.py` requires
> a **static `ET_EXEC`** ELF and rejects position-independent payloads with
> `Unsupported type`. Every payload this repo builds is PIE (`ET_DYN`), so the
> script checks the ELF type and refuses with an explanation rather than
> emitting a broken `eboot.bin`.
>
> The reason is structural. In `samples/install_app`, `eboot.bin` is **not**
> the homebrew — it is a small launcher stub:
> ```make
> eboot.elf: eboot.o
> 	$(LD) --static -T eboot.x -o $@ $^
> ```
> a static link against a custom linker script (`eboot.x`). The real
> application still runs as an ordinary payload; the registered app only
> bootstraps it. To ship EVO Player as a home-screen title you must port that
> launcher pattern (`eboot.c` + `eboot.x` + `payload.c`) rather than reusing
> the player's own ELF.

The SDK implements this openly in `samples/install_app`:

- `make_fself.py` converts the ELF to `eboot.bin`, stamping a fake `authinfo`
  blob and `--paid 0x3800000000000022`
- `sce_sys/param.json` carries the PS5 metadata
- a small payload calls `sceAppInstUtil` to register the title
  (`-lSceAppInstUtil -lSceIpmi`)

Installation pushes files over FTP to
[`ps5-payload-ftpsrv`](https://github.com/ps5-payload-dev/ftpsrv) on port 2121.
The SDK's `install_app` Makefile `test:` target is the authoritative sequence:

```bash
FTP=ftp://$PS5_HOST:2121
curl -Q "MTRW" $FTP                                    # remount read-write
curl -Q "MKD /system_ex/app/$TITLE_ID"         $FTP
curl -Q "MKD /system_ex/app/$TITLE_ID/sce_sys" $FTP
curl -Q "MKD /user/app/$TITLE_ID"              $FTP
curl -Q "MKD /user/app/$TITLE_ID/sce_sys"      $FTP

curl -T eboot.bin            "$FTP/system_ex/app/$TITLE_ID/"
curl -T sce_sys/param.json   "$FTP/user/app/$TITLE_ID/sce_sys/"
curl -T sce_sys/icon0.png    "$FTP/user/app/$TITLE_ID/sce_sys/"

prospero-deploy -h $PS5_HOST -p 9021 payload.elf       # sceAppInstUtil register
```

> `--title-id` must be 4 uppercase letters + 5 digits, e.g. `FAKE00001`. Use a
> `FAKE`-prefixed id; do not squat on a real Sony title id.

### 3. Signed fPKG — not implemented, and cannot be

Requires Sony's proprietary `prospero-pub-cmd` from the official Prospero SDK.
Third-party GUI builders (LibProsperoPKG, PS-Multi-Tools) wrap that same tool;
they do not replace it.

`package-pkg.sh --format pkg` **refuses with an explanation** rather than
producing something that is not a PKG. If you hold a licensed copy, see
[proprietary.md](proprietary.md).

For homebrew you do not need this. Use `--format homebrew`.

## Which should EVO Player ship?

`--format homebrew` for releases: no signing, trivial to install from USB, and
it is what the ecosystem expects. Offer `--format app` as an extra for people
who want the home-screen tile, exactly as ProsperoPlayer does.
