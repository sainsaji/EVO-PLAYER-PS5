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

# GL-4 (#80) Stage 3: the CPU YUV->BGRA converters (pp_converter[_parallel
# |_fused], pp_compute_pipeline) and the tiled VideoOut blit (tile_copy) are
# gone - the video goes up as R8/RG8 planes + a GLSL shader. pp_agc* and
# pp_videoout are dead code until GL-6 deletes them.
PP_SRCS := \
    pp/src/pp_videoout.c \
    pp/src/pp_agc.c \
    pp/src/pp_agc_osd.c \
    pp/src/pp_audio_clock.c \
    pp/src/pp_stage_breadcrumb.c \
    pp/src/pp_clock.c \
    pp/src/pp_playback.c \
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
    src/evo_usb_remote.c \
    src/evo_jailbreak.c

ADDON_SRCS := \
    addons/src/cJSON.c \
    addons/src/evo_net.c \
    addons/src/addon_emby.c

# --gl-smoke (#77, render-overhaul GL-1): compile the ps5-opengl smoke probe
# into the .ffpfsc. Off by default so build-evoplayer.sh and a normal
# package-app.sh --ffpfsc are byte-unchanged. package-app.sh passes GL_SMOKE=1
# plus the ps5-opengl-core33.mk cflags/archives; see docs/evo-pro/gl1-spike.md.
GL_SMOKE ?= 0
ifeq ($(GL_SMOKE),1)
    PP_SRCS  += pp/src/pp_gl_smoke.c pp/src/pp_gl_fatal.c
    CFLAGS   += -DEVO_GL_SMOKE=1
    CXXFLAGS += -DEVO_GL_SMOKE=1
    ifneq ($(PS5_OPENGL_PUBLIC_CFLAGS),)
        CFLAGS += $(PS5_OPENGL_PUBLIC_CFLAGS)
    endif
endif

# --gl (render-overhaul GL-3, #79): persistent device GL context through
# ps5-opengl. Cuts the boot over to GL/EGL; --gl and --gl-smoke are mutually
# exclusive (both replace pp_agc_init + the normal present path). Off by default;
# package-app.sh --gl passes GL_DEVICE=1 plus the ps5-opengl-core33.mk archives.
# (RMLUI_SRCS gets evo_gl_context_device.cpp appended below, after it is set.)
GL_DEVICE ?= 0
# GL-4 (#80) Stage 3: main.c's frame loop calls the evo_gl_* seam
# unconditionally. GL_DEVICE=1 appends the real device context to RMLUI_SRCS
# below; every other configuration links no-op stubs, so the ELF compile check
# (scripts/build-evoplayer.sh) still builds without ps5-opengl.
ifneq ($(GL_DEVICE),1)
    PP_SRCS += ui_rml/src/evo_gl_context_stub.c
endif
ifeq ($(GL_DEVICE),1)
    PP_SRCS  += pp/src/pp_gl_fatal.c
    CFLAGS   += -DEVO_GL_DEVICE=1
    CXXFLAGS += -DEVO_GL_DEVICE=1
    # B2: RmlUi's upstream GL3 render interface. ps5-opengl links GL directly, so
    # skip glad's dlopen loader - RMLUI_GL3_CUSTOM_LOADER points at a header that
    # just pulls <GL/gl.h> + prototypes (evo_gl3_device_loader.h).
    CXXFLAGS += -DRMLUI_GL3_CUSTOM_LOADER='"rmlui_gl3/evo_gl3_device_loader.h"'
    CXXFLAGS += -Iui_rml/src
    # B2 perf: ps5-opengl multisample-resolve is expensive. Menu AA can come back
    # once the render path is fast (or via EGL_SAMPLES on the window surface).
    CXXFLAGS += -DRMLUI_NUM_MSAA_SAMPLES=0
    ifneq ($(PS5_OPENGL_PUBLIC_CFLAGS),)
        CFLAGS   += $(PS5_OPENGL_PUBLIC_CFLAGS)
        CXXFLAGS += $(PS5_OPENGL_PUBLIC_CFLAGS)
    endif
endif

C_SRCS := main.c $(PP_SRCS) $(UI_SRCS) $(MEDIA_SRCS) $(ADDON_SRCS)
C_OBJS := $(C_SRCS:.c=.o)

# .incbin'd sceAgc shader blobs (#27 video, #28 UI). Assembled by $(CC).
ASM_OBJS := pp/src/agc_blobs.o pp/src/agc_ui_blobs.o
C_OBJS   += $(ASM_OBJS)

RMLUI_SRCS := \
    ui_rml/src/evo_rmlui_render.cpp \
    ui_rml/src/evo_rmlui_render_agc.cpp \
    ui_rml/src/evo_rmlui_system.cpp \
    ui_rml/src/evo_rmlui_app.cpp \
    ui_rml/src/evo_rmlui_bridge.cpp \
    ui_rml/src/evo_rmlui_fileinterface.cpp \
    ui_rml/src/evo_rmlui_bundle.cpp \
    ui_rml/src/evo_rmlui_bundle_data.cpp

# --gl (GL-3, #79): the device GL context + the RmlUi GL3 render interface,
# once RMLUI_SRCS exists.
ifeq ($(GL_DEVICE),1)
    RMLUI_SRCS += ui_rml/src/evo_gl_context_device.cpp \
                  ui_rml/src/evo_rmlui_render_gl.cpp \
                  ui_rml/src/rmlui_gl3/RmlUi_Renderer_GL3.cpp
endif

RMLUI_OBJS := $(RMLUI_SRCS:.cpp=.o)

# -MMD -MP: emit a .d next to each .o listing its header prerequisites, so an
# object rebuilds when a header it includes changes. Without this, editing e.g.
# pp_agc.h left stale .o files linked against the old ABI (#28, cost real
# hardware cycles). package-app.sh's app-cflags.stamp only force-cleans on a
# *flag* change, not a header edit.
DEPFLAGS := -MMD -MP
ALL_DEPS := $(C_OBJS:.o=.d) $(RMLUI_OBJS:.o=.d)

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

all: $(ELF)

-include $(ALL_DEPS)

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
	rm -f $(ELF) $(C_OBJS) $(RMLUI_OBJS) $(ALL_DEPS)
	rm -rf artifacts/4k_stages
