# ProsperoPlayer baseline

Upstream lives at `projects/prosperoplayer/`: a **pristine checkout, never
edited**. Populate it with

```bash
./scripts/build-prosperoplayer.sh
```

which clones <https://github.com/KINGDKAK/ProsperoPlayer> and builds it
**unmodified**, establishing the baseline EVO Player forks from. The whole
directory is excluded by `.gitignore`, so upstream code is never committed into
this repository.

Make your changes in [`projects/evoplayer/`](../projects/evoplayer/), not there.

## Baseline result

Builds successfully — a ~43 MB ELF — after one **environment** fix.

pacbrew builds FFmpeg with `--enable-openssl --enable-libass
--enable-libfreetype --enable-libfribidi --enable-libharfbuzz`. Static archives
carry no dependency metadata, so those libraries must appear on the link line,
and upstream's `LIBS` list omits them:

```
undefined symbol: BN_set_word, BN_num_bits, BN_rand, BN_CTX_new, BN_mod_exp
    referenced by rtmpdh.c in libavformat.a      -> -lssl -lcrypto
undefined symbol: libiconv, libiconv_open, libiconv_close
                                                 -> -liconv
```

`build-prosperoplayer.sh` fixes this by overriding `LIBS` on the make command
line rather than editing the source. Reproduce the raw failure with:

```bash
EVO_SKIP_LINK_FIX=1 ./scripts/build-prosperoplayer.sh
```

Audit dependencies without building:

```bash
./scripts/build-prosperoplayer.sh --audit
```

## What upstream expects

| | |
|---|---|
| Output | `PS5MediaPlayerPRO.elf` |
| Static libs | `libSDL2.a`, `libavformat.a`, `libavcodec.a`, `libswresample.a`, `libavutil.a`, `libswscale.a` from `$PS5_PAYLOAD_SDK/target/user/homebrew/lib` |
| Headers | `.../target/user/homebrew/include{,/SDL2}` |
| SCE stubs | `SceNotification SceSystemService SceUserService ScePad SceAudioOut SceVideoOut SceKeyboard SceImeDialog` — all present in SDK v0.42 |
| C++ runtime | `-lc++ -lc++abi -lpthread` |
| FFmpeg | 7.0.1 (from pacbrew-repo v0.39) |
| Build knob | `STAGE` 0–4, default 4; `make stage_all` builds all five |
