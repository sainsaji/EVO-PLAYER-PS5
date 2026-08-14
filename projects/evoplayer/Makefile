PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := PS5MediaPlayerPRO.elf

# Progressive 4K product stages: 0=SAFE 1=G_VO 2=G_V8 3=G_AUDIO 4=H_FULL
# Default 4 after G-H stage ladder PASS (2026-07-21). Rollback: STAGE=0
STAGE ?= 4

# One version, one place. CI checks this file against the release tag, so
# injecting it here keeps the startup toast, the menu header and the footer
# from drifting apart - they were three separate literals, and two of them
# still said 1.0 long after 0.0.1 shipped.
EVO_VERSION := $(shell tr -d '[:space:]' < VERSION)

# Development-only switches, e.g.
#   make EXTRA_CFLAGS="-DEVO_AUTOSHOT=4 -DEVO_START_SCREEN=10"
# Empty for shipping builds.
EXTRA_CFLAGS ?=

CFLAGS := -Wall -g -O2 $(EXTRA_CFLAGS) \
    -DEVO_PLAYER_VERSION=\"$(EVO_VERSION)\" \
    -DEVO_TARGET_PS5=1 \
    -Ipp/include \
    -Iui/include \
    -Imedia/include \
    -Iaddons/include \
    -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include/SDL2 \
    -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include \
    -DPP_4K_PRODUCT_STAGE=$(STAGE)

LIBS := \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libSDL2.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libavformat.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libavcodec.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libswresample.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libavutil.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libswscale.a \
    -lSceNotification \
    -lSceSystemService \
    -lSceUserService \
    -lScePad \
    -lSceAudioOut \
    -lSceVideoOut \
    -lSceKeyboard \
    -lSceImeDialog \
    -lc++ -lc++abi -lpthread



PP_SRCS := \
    pp/src/pp_videoout.c \
    pp/src/pp_converter.c \
    pp/src/pp_converter_parallel.c \
    pp/src/pp_converter_fused.c \
    pp/src/pp_compute_pipeline.c \
    pp/src/pp_audio_clock.c \
    pp/src/pp_stage_breadcrumb.c \
    pp/src/pp_clock.c \
    pp/src/pp_playback.c \
    pp/src/tile_copy.c \
    pp/src/evo_theme.c \
    pp/src/evo_ui.c


# The UI layer. Kept out of main.c on purpose: these have no dependency on
# the player, the decoder or the ~1MB of asset headers, so they compile in a
# second and can be exercised on the host under sanitizers.
UI_SRCS := \
    ui/src/evo_draw.c \
    ui/src/evo_nav.c \
    ui/src/evo_focus.c \
    ui/src/evo_input.c \
    ui/src/evo_feedback.c \
    ui/src/evo_chrome.c \
    ui/src/evo_widgets.c \
    ui/src/evo_keyboard.c \
    ui/src/evo_screens.c

# Media subsystems carved out of main.c. Unlike the UI layer these do link
# against FFmpeg, but they own their own state and threads and expose a handful
# of functions - see the header of each for the measurement that justified the
# cut.
MEDIA_SRCS := \
    media/src/prospero_thumbnail.c \
    media/src/evo_textreader.c

# Streaming & Addon subsystems (Emby, Nuvio/Stremio, async HTTP REST queue)
ADDON_SRCS := \
    addons/src/cJSON.c \
    addons/src/evo_net.c \
    addons/src/addon_emby.c

all: $(ELF)

$(ELF): main.c $(PP_SRCS) $(UI_SRCS) $(MEDIA_SRCS) $(ADDON_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "STAGE=$(STAGE)"
	@sha256sum $@
	@ls -la $@

# Build all progressive stage ELFs into artifacts/
stage_all:
	@mkdir -p artifacts/4k_stages
	$(MAKE) STAGE=0 ELF=artifacts/4k_stages/PP_4K_STAGE0_SAFE.elf
	$(MAKE) STAGE=1 ELF=artifacts/4k_stages/PP_4K_STAGE1_G_VO.elf
	$(MAKE) STAGE=2 ELF=artifacts/4k_stages/PP_4K_STAGE2_G_V8.elf
	$(MAKE) STAGE=3 ELF=artifacts/4k_stages/PP_4K_STAGE3_G_AUDIO.elf
	$(MAKE) STAGE=4 ELF=artifacts/4k_stages/PP_4K_STAGE4_H_FULL.elf

clean:
	rm -f $(ELF)
	rm -rf artifacts/4k_stages
