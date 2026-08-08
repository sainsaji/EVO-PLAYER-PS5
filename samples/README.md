# samples/

Scratch space for experiments and reference material that is not a first-class
project.

The **PS5 Payload SDK's own samples** live in the container at:

```
$PS5_PAYLOAD_SDK/samples/
```

and are worth reading — they are the authoritative examples of how the SDK
expects to be used:

| Sample | Shows |
|---|---|
| `hello_world` | the minimal payload; notification output |
| `hello_stdio` | stdio from a payload |
| `hello_cxx`, `hello_cxx23` | C++ payloads (needs libc++ from `libcxx.sh`) |
| `hello_dlfcn` | `dlopen`/`dlsym` at run time |
| `hello_sprx` | loading Sony `.sprx` system modules |
| `hello_so` | loading ELF shared objects |
| `hello_cmake`, `hello_meson` | non-Make build systems |
| `hwinfo`, `mntinfo`, `ps` | reading system state |
| `install_app` | fake-SELF + `param.json` + `sceAppInstUtil` — the basis of `scripts/package-pkg.sh --format app` |
| `notify`, `notify_debug` | notification APIs |
| `arbitrary_syscall` | raw syscalls |
| `relocatable_linking` | `-r` partial linking |

Build one out-of-tree so the SDK volume stays clean:

```bash
cp -r $PS5_PAYLOAD_SDK/samples/hello_stdio /tmp/x && make -C /tmp/x
```

For EVO Player's own progressively-built test suite, see
[`../projects/`](../projects/) — those are the ones wired into
`./scripts/build.sh` and CI.
