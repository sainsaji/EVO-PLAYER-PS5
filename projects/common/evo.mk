# =============================================================================
# projects/common/evo.mk - shared Makefile fragment for EVO Player projects.
#
# Include from a project Makefile:
#     include ../common/evo.mk
#
# It pulls in the SDK's prospero.mk (which sets CC/LD/AR/... to the prospero-*
# cross wrappers) and adds this repo's conventions on top:
#   * output ELFs are copied to output/elf/ so deploy.sh can find them
#   * common headers and helpers are on the include path
#   * `make test` deploys via prospero-deploy, matching the SDK samples
# =============================================================================

# Locate this fragment BEFORE including anything else.
# $(lastword $(MAKEFILE_LIST)) must be evaluated while evo.mk is still the most
# recently opened makefile - after `include prospero.mk` it would point at the
# SDK's toolchain directory instead, and EVO_ROOT would silently become /opt.
EVO_COMMON_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
EVO_ROOT       := $(abspath $(EVO_COMMON_DIR)/../..)
EVO_ELF_OUT    := $(EVO_ROOT)/output/elf

# Console connection. Never hard-code an address here; deploy.sh and the SDK
# helper both read these from the environment. 9021 is the elfldr port.
PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifndef PS5_PAYLOAD_SDK
    $(error PS5_PAYLOAD_SDK is undefined. Source $$PS5_PAYLOAD_SDK/toolchain/prospero.sh, \
            or run this inside the dev container (./scripts/shell.sh))
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

# -----------------------------------------------------------------------------
# Common flags.
#   -Wall -Wextra           catch the usual mistakes early
#   -g                      keep symbols; gdbsrv debugging needs them
#   -O2                     the software decode paths care about this
#   -Wno-unused-parameter   SCE callback signatures have unused args by design
# -----------------------------------------------------------------------------
EVO_CFLAGS := -Wall -Wextra -Wno-unused-parameter -g -O2 \
              -I$(EVO_COMMON_DIR)/include

# The prebuilt ports prefix (SDL2, FFmpeg, ...) supplied by pacbrew-repo.
EVO_HB_PREFIX  := $(PS5_PAYLOAD_SDK)/target/user/homebrew
EVO_HB_INCLUDE := -I$(EVO_HB_PREFIX)/include
EVO_HB_LIB     := $(EVO_HB_PREFIX)/lib

CFLAGS  := $(EVO_CFLAGS) $(CFLAGS)
LDFLAGS ?=

# Shared helper sources every project may use.
EVO_COMMON_SRCS := $(EVO_COMMON_DIR)/src/evo_notify.c

.PHONY: all clean test deploy install-elf debug

# -----------------------------------------------------------------------------
# install-elf: publish the built payload where deploy.sh and CI expect it.
#
# Depends on `all`, not on $(ELF): prerequisites are expanded when the rule is
# read, and the including Makefile sets ELF *after* this file. `all: $(ELF)`
# lives in the project Makefile, where ELF is already defined. The recipe body
# is expanded at run time, so $(ELF) resolves correctly there.
# -----------------------------------------------------------------------------
install-elf: all
	@mkdir -p $(EVO_ELF_OUT)
	@cp -f $(ELF) $(EVO_ELF_OUT)/
	@echo "  -> output/elf/$(notdir $(ELF))  ($$(stat -c %s $(ELF)) bytes)"

# -----------------------------------------------------------------------------
# test: build, publish, then push to the console. Mirrors the SDK samples'
# `make test` so muscle memory transfers.
# -----------------------------------------------------------------------------
test: install-elf
	@if [ -z "$$PS5_HOST" ] && [ "$(PS5_HOST)" = "ps5" ]; then \
	    echo "ERROR: PS5_HOST is not set."; \
	    echo "       PS5_HOST=192.168.1.50 make test"; \
	    exit 1; \
	fi
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)

deploy: test

# -----------------------------------------------------------------------------
# debug: attach gdb to ps5-payload-gdbsrv running on the console (TCP 2159).
# Same recipe the SDK samples use.
# -----------------------------------------------------------------------------
debug: all
	gdb-multiarch \
	  -ex "set architecture i386:x86-64" \
	  -ex "target extended-remote $(PS5_HOST):2159" \
	  -ex "file $(ELF)" \
	  -ex "remote put $(ELF) /data/$(notdir $(ELF))" \
	  -ex "set remote exec-file /data/$(notdir $(ELF))" \
	  -ex "start"
