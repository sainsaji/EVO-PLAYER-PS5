# Proprietary files

**Nothing in `proprietary/` is ever committed.** It is excluded by
`.gitignore`, and so are `*.sprx`, `*.self`, `*.pkg`, `*.pup`, `*.sfo`, `*.key`
and `*.pem` wherever they appear in the tree.

Do not commit, and do not paste into issues:

- Sony `.sprx` / `.self` system modules, decrypted or otherwise
- Any part of the official Prospero SDK, including `prospero-pub-cmd`
- Console dumps, kernel dumps, `pfsSKKey` material
- Private keys, certificates, credentials, PSN account data

This repository builds entirely from open sources. Everything in
[the validation checklist](../README.md#25-final-validation-checklist) passes
without a single proprietary file. Proprietary material is only ever needed for
*optional* research paths, and each of those fails loudly rather than silently
substituting something wrong.

## Layout

```
proprietary/
├── README.md          committed - explains the layout
├── sprx/              your decrypted Sony modules      (ignored)
├── tools/             licensed Sony tooling            (ignored)
└── keys/              never share these                (ignored)
```

## Supplying SPRX modules for SCE stub generation

This is the supported route to calling a Sony module the SDK has no stub for —
`libSceVdecCore`, `libSceAvPlayer` and friends (see
[native-media-research.md](native-media-research.md)).

The SDK implements it natively. `sce_stubs/Makefile`:

```make
SPRXS := $(wildcard *.sprx)
STUBS := $(SPRXS:.sprx=.c)

NID_DB     := aerolib.csv
NID_DB_URL := https://raw.githubusercontent.com/zecoxao/sce_symbols/main/$(NID_DB)

stubs: $(NID_DB) $(STUBS)

%.c: %.sprx
	$(PYTHON) genstub.py $^ > $@
```

`genstub.py` walks the module's `PT_DYNAMIC` segment with pyelftools, maps each
exported NID back to a symbol name through `aerolib.csv`, and emits a `.c` of
bare `asm(".global ...")` declarations. That is then linked into a `.so` the
linker can resolve against.

### Procedure

1. Obtain the decrypted module legitimately, from **your own console**, and
   place it in `proprietary/sprx/`:

   ```
   proprietary/sprx/libSceVdecCore.sprx
   ```

2. Build the SDK from source, since stub generation happens in its tree:

   ```bash
   ./scripts/shell.sh
   ./scripts/setup-sdk.sh --from-source
   ```

3. Copy your module into the SDK's stub directory and generate:

   ```bash
   git clone https://github.com/ps5-payload-dev/sdk /tmp/sdk
   cp /workspace/proprietary/sprx/*.sprx /tmp/sdk/sce_stubs/
   make -C /tmp/sdk/sce_stubs stubs      # fetches aerolib.csv, runs genstub.py
   make -C /tmp/sdk/sce_stubs            # builds the .so stubs
   sudo make -C /tmp/sdk/sce_stubs install DESTDIR=$PS5_PAYLOAD_SDK
   ```

4. Link normally: `-lSceVdecCore`.

The generated `.c` contains only symbol names, no Sony code — but the `.sprx`
it came from must stay out of the repository regardless.

> Symbols whose NID is not in `aerolib.csv` come out as raw NID strings. That
> is a naming gap, not a failure; `prospero-nid` in the SDK's `bin/` helps
> correlate them.

## Licensed Sony tooling (fPKG signing)

True signed `.pkg` creation needs `prospero-pub-cmd` from the official Prospero
SDK. If you hold a legitimate licensed copy:

```
proprietary/tools/prospero-pub-cmd
```

`scripts/package-pkg.sh` checks for it and **refuses with a clear message** when
it is absent, rather than emitting something that is not a PKG. See
[packaging.md](packaging.md) — and note that homebrew distribution does not
need this at all.

## Fail loudly, never substitute

Per the brief: an absent optional proprietary dependency must produce a clear
error, not a silent fallback. Current behaviour:

| Missing | Behaviour |
|---|---|
| `proprietary/tools/prospero-pub-cmd` | `package-pkg.sh --format pkg` exits with an explanation and points at `--format homebrew` |
| SPRX for stub generation | not consulted at all unless you run the stub flow above; `decoder_test` instead reports which modules are reachable at run time |
| `proprietary/` missing entirely | everything in the checklist still passes |

## If you commit something by accident

A `git rm` is not enough — the blob stays in history.

```bash
# Stop immediately; do not push.
git rm --cached path/to/file
# Then purge it from history (rewrites commits):
git filter-repo --path path/to/file --invert-paths
```

If it was already pushed to a shared remote, treat the material as disclosed:
rotate any keys and assume the file is public.
