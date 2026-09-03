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

EVO_VERSION := $(shell tr -d '[:space:]' < VERSION)

EXTRA_CFLAGS ?=

CXXFLAGS := -Wall -g -O2 -std=c++17 $(EXTRA_CFLAGS) \
    -DEVO_PLAYER_VERSION=\"$(EVO_VERSION)\" \
    -DEVO_TARGET_PS5=1 \
    -Iinclude \
    -Ipp/include \
    -Iui/include \
    -Iui_rml/include \
    -Imedia/include \
    -Iaddons/include \
    -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include/SDL2 \
    -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include \
    -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include/freetype2 \
    -DPP_4K_PRODUCT_STAGE=$(STAGE)

CFLAGS := -Wall -g -O2 $(EXTRA_CFLAGS) \
    -DEVO_PLAYER_VERSION=\"$(EVO_VERSION)\" \
    -DEVO_TARGET_PS5=1 \
    -Iinclude \
    -Ipp/include \
    -Iui/include \
    -Iui_rml/include \
    -Imedia/include \
    -Iaddons/include \
    -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include/SDL2 \
    -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include \
    -DPP_4K_PRODUCT_STAGE=$(STAGE)

LIBS := \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/librmlui.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libfreetype.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libharfbuzz.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libpng16.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libz.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libbz2.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/liblzma.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libzstd.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libSDL2.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libavformat.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libavcodec.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libswresample.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libavutil.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libswscale.a \
    -lssl \
    -lcrypto \
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
    pp/src/pp_agc.c \
    pp/src/pp_audio_clock.c \
    pp/src/pp_stage_breadcrumb.c \
    pp/src/pp_clock.c \
    pp/src/pp_playback.c \
    pp/src/tile_copy.c \
    pp/src/evo_theme.c \
    pp/src/evo_ui.c

UI_SRCS := \
    ui/src/evo_draw.c \
    ui/src/evo_nav.c \
    ui/src/evo_focus.c \
    ui/src/evo_input.c \
    ui/src/evo_feedback.c \
    ui/src/evo_layout.c \
    ui/src/evo_widgets.c \
    ui/src/evo_keyboard.c

MEDIA_SRCS := \
    media/src/prospero_thumbnail.c \
    media/src/evo_textreader.c \
    media/src/evo_direct_mem.c \
    media/src/evo_stream_io.c \
    media/src/evo_packet_queue.c \
    media/src/evo_audio_resample.c \
    media/src/evo_audio_out.c \
    media/src/evo_subtitle.c \
    media/src/evo_demux.c \
    media/src/evo_vdec_ffmpeg.c \
    media/src/evo_vdec_native.c \
    media/src/evo_playback.c \
    src/evo_toast.c \
    src/evo_recent.c \
    src/evo_favorites.c \
    src/evo_data_path.c \
    src/evo_readdir.c \
    src/evo_boot_log.c \
    src/evo_agc_probe.c \
    src/evo_avplayer_probe.c \
    src/evo_videodec2_probe.c \
    src/evo_usb_remote.c \
    src/evo_jailbreak.c

ADDON_SRCS := \
    addons/src/cJSON.c \
    addons/src/evo_net.c \
    addons/src/addon_emby.c

C_SRCS := main.c $(PP_SRCS) $(UI_SRCS) $(MEDIA_SRCS) $(ADDON_SRCS)
C_OBJS := $(C_SRCS:.c=.o)

# .incbin'd sceAgc shader blobs (#27 video, #28 UI). Assembled by $(CC).
ASM_OBJS := pp/src/agc_blobs.o pp/src/agc_ui_blobs.o
C_OBJS   += $(ASM_OBJS)

RMLUI_SRCS := \
    ui_rml/src/evo_rmlui_render.cpp \
    ui_rml/src/evo_rmlui_system.cpp \
    ui_rml/src/evo_rmlui_app.cpp \
    ui_rml/src/evo_rmlui_bridge.cpp

RMLUI_OBJS := $(RMLUI_SRCS:.cpp=.o)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

all: $(ELF)

# rebuild the embedded blobs when a .bin changes (recipe = the %.o: %.S rule)
pp/src/agc_blobs.o: pp/src/agc_blobs.S $(wildcard pp/blobs/*.bin)
pp/src/agc_ui_blobs.o: pp/src/agc_ui_blobs.S $(wildcard pp/blobs/*.header.bin) $(wildcard ../../pp/shaders/*.bin)

# -----------------------------------------------------------------------------
# App-module packaging hooks (Phase 1b - docs/evo-pro/phase-1b-app-module.md).
# scripts/package-app.sh compiles the object set with the native-app toolchain
# (prospero-clang18: -femulated-tls -fno-plt -fno-stack-protector) and does its
# own link/convert/sign, so it needs the objects without the SDK-crt link.
#   objects        build every .o, no ELF
#   print-objects  emit the object paths (relative to this dir), space-separated
# -----------------------------------------------------------------------------
.PHONY: objects print-objects
objects: $(C_OBJS) $(RMLUI_OBJS)

print-objects:
	@echo $(C_OBJS) $(RMLUI_OBJS)

$(ELF): $(C_OBJS) $(RMLUI_OBJS)
	$(CXX) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "STAGE=$(STAGE)"
	@sha256sum $@
	@ls -la $@

clean:
	rm -f $(ELF) $(C_OBJS) $(RMLUI_OBJS)
	rm -rf artifacts/4k_stages
