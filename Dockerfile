# =============================================================================
# EVO Player - PS5 Homebrew Development Environment
# Target console firmware: PS5 12.70  (ps5-payload-sdk crt case 0x12700000)
# =============================================================================
#
# WHY UBUNTU 24.04 LTS (noble):
#   The PS5 Payload SDK documents and CI-tests a Debian-flavoured host. Its
#   own workflow (.github/workflows/ubuntu-latest.yml) runs:
#       sudo apt install clang-18 lld-18 mingw-w64 cmake meson
#   Ubuntu 24.04 carries clang-18/lld-18/llvm-18 in its archive directly, so
#   we match upstream CI exactly without adding the apt.llvm.org repository.
#   Debian 12 (bookworm) only ships clang-14/16 and would need a third-party
#   apt source; Alpine/musl is unsuitable because the SDK toolchain wrappers,
#   libc++ bootstrap and the FFmpeg build all assume a glibc/Debian layout.
#
# WHY LLVM 18 (and not "latest"):
#   1. sdk/.github/workflows/ubuntu-latest.yml installs clang-18 / lld-18.
#      That is the only configuration upstream actually builds and releases.
#   2. sdk/libcxx.sh hardcodes LLVM_VER="18.1.8" and compiles libc++/libc++abi/
#      libunwind from those exact sources into the PS5 sysroot. Building 18.1.8
#      runtime sources with a matching 18.x clang avoids version skew in the
#      C++ ABI that ProsperoPlayer links against (-lc++ -lc++abi).
#   3. sdk/Makefile.inc probes llvm-config-22 ... llvm-config-15 in that order
#      and takes the FIRST one found. So the SDK *tolerates* LLVM 15-22, but
#      only 18 is CI-verified end to end.
#
#   ALTERNATIVE (documented, not default): LLVM 19-22 are accepted by
#   Makefile.inc (release v0.41 added "host: add support for llvm-22").
#   To try one, build with:  --build-arg LLVM_VERSION=20
#   and add the apt.llvm.org source, since Ubuntu 24.04 has no clang-20.
#   Expect to re-run libcxx.sh; mixing a newer clang with 18.1.8 libc++
#   sources is the untested combination.
# =============================================================================

ARG UBUNTU_VERSION=24.04
FROM ubuntu:${UBUNTU_VERSION}

# --- Version pins -----------------------------------------------------------
# Each pin is an immutable, permanently-fetchable artifact so that rebuilding
# this image months from now produces the same toolchain.
ARG LLVM_VERSION=18
# ps5-payload-dev/sdk release. v0.42 published 2026-08-02.
# 12.70 support lives in crt/kernel.c: `case 0x12700000:` (added well before
# this tag; v0.38/v0.39 notes read "kernel: add 11.xx and 12.xx offsets").
ARG PS5_SDK_VERSION=v0.42
# ps5-payload-dev/pacbrew-repo release. v0.39 published 2026-08-02.
# Ships a prebuilt /opt/ps5-payload-sdk sysroot containing FFmpeg 7.0.1,
# SDL2, mesa, libass ... i.e. exactly what ProsperoPlayer links against.
ARG PACBREW_VERSION=v0.39
# CMake/Ninja are taken from upstream release binaries rather than apt so the
# exact version is reproducible (apt point-releases rotate out of the archive).
ARG CMAKE_VERSION=3.31.6
ARG CMAKE_SHA256=5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf
ARG NINJA_VERSION=1.12.1
ARG NINJA_SHA256=6f98805688d19672bd699fbbfa2c2cf0fc054ac3df1f0e6a47664d963d530255
# CMake 3.31.x deliberately, not 4.x: CMake 4 removed compatibility with
# cmake_minimum_required(<3.5), which breaks several pacbrew PKGBUILD ports.

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8

# -----------------------------------------------------------------------------
# System packages.
# Every entry below is present because something in the toolchain, the SDK,
# the deploy path or the FFmpeg build actually invokes it. Grouped by reason.
# -----------------------------------------------------------------------------
RUN apt-get update && apt-get install -y --no-install-recommends \
    \
    # -- shell + fetch: SDK README uses wget; libcxx.sh uses wget+tar;
    #    scripts/ are bash (not sh); TLS roots for github/ffmpeg.org.
    bash \
    git \
    wget \
    curl \
    ca-certificates \
    \
    # -- LLVM toolchain. clang/lld are the SDK's *required* deps.
    #    llvm-${V}-dev provides llvm-config-${V}, which sdk/Makefile.inc
    #    probes for and uses to locate clang/llvm-ar/ld.lld (LLVM_BINDIR).
    #    llvm-${V}-tools + the base llvm-${V} give llvm-nm / llvm-readelf /
    #    llvm-objdump, required for the libSceVdec*/libSceAvPlayer symbol
    #    reverse-engineering work (see docs/native-media-research.md).
    clang-${LLVM_VERSION} \
    lld-${LLVM_VERSION} \
    llvm-${LLVM_VERSION} \
    llvm-${LLVM_VERSION}-dev \
    llvm-${LLVM_VERSION}-tools \
    libclang-rt-${LLVM_VERSION}-dev \
    # clangd powers VS Code source navigation over compile_commands.json.
    clangd-${LLVM_VERSION} \
    # lldb: "where practical" - installed, but note the PS5 has no lldb-server.
    # Real on-console debugging uses gdb-multiarch against ps5-payload-gdbsrv
    # (the SDK sample Makefiles have a `debug:` target calling gdb-multiarch).
    lldb-${LLVM_VERSION} \
    \
    # -- host build systems. `make` builds the SDK itself and every sample.
    #    build-essential supplies a host gcc/libc headers, needed to compile
    #    the SDK's *host-side* helpers (host/bin/prospero-nid.c).
    make \
    build-essential \
    pkg-config \
    \
    # -- python: sdk/Makefile.inc sets PYTHON?=python3.
    #    pyelftools is imported by sce_stubs/genstub.py (ELFFile) to turn a
    #    decrypted .sprx into a linkable stub, and by samples/install_app/
    #    make_fself.py for fake-SELF generation.
    python3 \
    python3-pip \
    python3-pyelftools \
    python3-venv \
    \
    # -- meson: SDK ships toolchain/prospero.ini + samples/hello_meson and its
    #    CI builds them; several pacbrew ports are meson-based.
    meson \
    \
    # -- deployment. socat is NOT optional: host/bin/prospero-deploy pipes the
    #    ELF with `socat -t 9999999 - TCP:$HOST:$PORT`. Without it, `make test`
    #    and scripts/deploy.sh fail. netcat-openbsd provides `nc -vz` for the
    #    documented Docker->PS5 reachability check.
    socat \
    netcat-openbsd \
    iproute2 \
    iputils-ping \
    \
    # -- binary inspection. Used to verify a produced ELF is a PS5 payload and
    #    to mine symbols out of Sony modules. `strings` is referenced directly
    #    by prospero.mk (STRINGS := strings) and comes from binutils.
    file \
    binutils \
    xxd \
    \
    # -- debugging. gdb-multiarch is what the SDK sample `debug:` target runs
    #    against the console's gdbsrv on TCP 2159.
    gdb \
    gdb-multiarch \
    \
    # -- editors for in-container work
    vim \
    nano \
    \
    # -- archives. SDK release is a .zip; pacbrew sysroot is .tar.gz;
    #    FFmpeg tarballs are .xz; some ports ship .bz2.
    zip \
    unzip \
    tar \
    xz-utils \
    bzip2 \
    \
    # -- FFmpeg / autotools port build dependencies.
    #    yasm+nasm: FFmpeg's x86 SIMD (without them configure silently drops
    #    optimised DSP paths, which matters a lot for 4K software decode).
    #    autoconf/automake/libtool/bison/flex/gperf: required by the wider
    #    pacbrew dependency chain (see pacbrew-repo/README.md prerequisites).
    yasm \
    nasm \
    autoconf \
    automake \
    libtool \
    bison \
    flex \
    gperf \
    patch \
    diffutils \
    \
    # -- ccache: makes iterating on FFmpeg configure flags bearable. Its cache
    #    lives on a named volume (see docker-compose.yml).
    ccache \
    \
    # -- sudo: the container runs as a non-root developer user for bind-mount
    #    file ownership sanity; sudo lets that user write to /opt.
    sudo \
    && rm -rf /var/lib/apt/lists/*

# -----------------------------------------------------------------------------
# Make the versioned LLVM binaries discoverable under their plain names.
# sdk/Makefile.inc finds llvm-config-18 on its own, but user-facing commands
# from the task's validation checklist (`clang --version`, `ld.lld --version`)
# must work too.
# -----------------------------------------------------------------------------
RUN update-alternatives --install /usr/bin/clang        clang        /usr/bin/clang-${LLVM_VERSION} 100 \
      --slave /usr/bin/clang++     clang++     /usr/bin/clang++-${LLVM_VERSION} \
      --slave /usr/bin/clang-cpp   clang-cpp   /usr/bin/clang-cpp-${LLVM_VERSION} \
 && update-alternatives --install /usr/bin/llvm-config  llvm-config  /usr/bin/llvm-config-${LLVM_VERSION} 100 \
 && for t in ld.lld lld llvm-ar llvm-nm llvm-objdump llvm-readelf llvm-strip llvm-ranlib llvm-objcopy llvm-strings clangd lldb; do \
      if [ -x "/usr/bin/$t-${LLVM_VERSION}" ]; then \
        update-alternatives --install "/usr/bin/$t" "$t" "/usr/bin/$t-${LLVM_VERSION}" 100; \
      fi; \
    done

# -----------------------------------------------------------------------------
# Pinned CMake (upstream binary release, checksum-verified).
# -----------------------------------------------------------------------------
RUN cd /tmp \
 && wget -q "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz" \
 && echo "${CMAKE_SHA256}  cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz" | sha256sum -c - \
 && tar -xzf "cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz" -C /opt \
 && mv "/opt/cmake-${CMAKE_VERSION}-linux-x86_64" /opt/cmake \
 && ln -sf /opt/cmake/bin/cmake  /usr/local/bin/cmake \
 && ln -sf /opt/cmake/bin/ctest  /usr/local/bin/ctest \
 && ln -sf /opt/cmake/bin/cpack  /usr/local/bin/cpack \
 && rm -rf /tmp/cmake-*

# -----------------------------------------------------------------------------
# Pinned Ninja (upstream binary release, checksum-verified).
# -----------------------------------------------------------------------------
RUN cd /tmp \
 && wget -q -O ninja-linux.zip "https://github.com/ninja-build/ninja/releases/download/v${NINJA_VERSION}/ninja-linux.zip" \
 && echo "${NINJA_SHA256}  ninja-linux.zip" | sha256sum -c - \
 && unzip -q ninja-linux.zip -d /usr/local/bin \
 && chmod +x /usr/local/bin/ninja \
 && rm -f /tmp/ninja-linux.zip

# -----------------------------------------------------------------------------
# PS5 Payload SDK.
#
# Installed into the IMAGE at /opt/ps5-payload-sdk so that `docker compose
# build` alone yields a working toolchain (task 21: `docker compose build`
# must be enough). scripts/setup-sdk.sh can later refresh/rebuild it, and
# docker-compose.yml mounts a named volume over /opt/ps5-payload-sdk so that
# additions (custom FFmpeg, generated SCE stubs) survive container restarts.
#
# BUILD_SDK_FROM_SOURCE=1 switches from the released zip to a source build.
# -----------------------------------------------------------------------------
ARG BUILD_SDK_FROM_SOURCE=0
ENV PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk

COPY scripts/install-sdk-image.sh /usr/local/libexec/install-sdk-image.sh
RUN chmod +x /usr/local/libexec/install-sdk-image.sh \
 && /usr/local/libexec/install-sdk-image.sh

# -----------------------------------------------------------------------------
# Prebuilt PS5 library sysroot (pacbrew-repo).
#
# WHY: ProsperoPlayer's Makefile links these absolute paths:
#   $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libSDL2.a
#   .../libavformat.a .../libavcodec.a .../libswresample.a
#   .../libavutil.a   .../libswscale.a
# and includes .../target/user/homebrew/include{,/SDL2}. Those artifacts come
# from pacbrew-repo, whose CI tars up the whole /opt/ps5-payload-sdk tree.
# Installing it here is what makes task 15 ("make existing ProsperoPlayer
# compile") achievable without first building 40 ports by hand.
#
# Set INSTALL_PACBREW=0 for a bare SDK-only image (smaller, no FFmpeg/SDL2).
# -----------------------------------------------------------------------------
ARG INSTALL_PACBREW=1
COPY scripts/install-pacbrew-image.sh /usr/local/libexec/install-pacbrew-image.sh
RUN chmod +x /usr/local/libexec/install-pacbrew-image.sh \
 && /usr/local/libexec/install-pacbrew-image.sh

# -----------------------------------------------------------------------------
# Non-root developer user.
# Bind-mounted sources on Docker Desktop for Windows appear owned by the
# container user, so a plain `dev` user avoids root-owned build output leaking
# back into the Windows workspace via the build/ and output/ mounts.
# -----------------------------------------------------------------------------
ARG USERNAME=dev
ARG USER_UID=1001
ARG USER_GID=1001
RUN groupadd --gid ${USER_GID} ${USERNAME} \
 && useradd --uid ${USER_UID} --gid ${USER_GID} -m -s /bin/bash ${USERNAME} \
 && echo "${USERNAME} ALL=(root) NOPASSWD:ALL" > /etc/sudoers.d/${USERNAME} \
 && chmod 0440 /etc/sudoers.d/${USERNAME} \
 && chown -R ${USER_UID}:${USER_GID} /opt/ps5-payload-sdk

# -----------------------------------------------------------------------------
# Environment. Sourcing prospero.sh exports CC/CXX/LD/AR/CMAKE/MESON/
# PKG_CONFIG/PS5_DEPLOY etc. pointing at the prospero-* cross wrappers.
# -----------------------------------------------------------------------------
ENV PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk \
    PS5_SYSROOT=/opt/ps5-payload-sdk/target \
    PS5_HBROOT=/user/homebrew \
    PS5_PORT=9021 \
    CCACHE_DIR=/ccache \
    PATH=/opt/ps5-payload-sdk/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin

# Pre-create the mount points for the named volumes, owned by the dev user.
# Docker seeds a fresh named volume from the image's content AND ownership at
# that path; if these directories do not exist in the image, the volume is
# created root-owned and the non-root user cannot write to it.
RUN mkdir -p /ccache /build/ffmpeg \
 && chown -R ${USER_UID}:${USER_GID} /ccache /build

# Auto-source the PS5 toolchain in every interactive shell.
RUN printf '%s\n' \
    '# --- EVO Player PS5 dev environment ---' \
    'export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk' \
    'if [ -f "$PS5_PAYLOAD_SDK/toolchain/prospero.sh" ]; then' \
    '    source "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"' \
    'fi' \
    'export PS5_PORT="${PS5_PORT:-9021}"' \
    'export PATH="$PS5_PAYLOAD_SDK/bin:$PATH"' \
    > /etc/profile.d/ps5-sdk.sh \
 && cat /etc/profile.d/ps5-sdk.sh >> /home/${USERNAME}/.bashrc

USER ${USERNAME}
WORKDIR /workspace

# Bind-mounted Windows directories always present a uid that does not match the
# container user, and modern git refuses to operate on such repositories
# ("detected dubious ownership"). That ownership mismatch is inherent to Docker
# Desktop bind mounts, not a security signal, so trust the workspace.
RUN git config --global --add safe.directory '*' \
 && git config --global init.defaultBranch main

CMD ["/bin/bash", "-l"]
