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
    -Icore/include \
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
    -Icore/include \
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
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libdav1d.a \
    -lssl \
    -lcrypto \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libxml2.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libiconv.a \
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
# gone - the video goes up as R8/RG8 planes + a GLSL shader.
# GL-6 (#82): the hand-rolled sceAgc present/geo/OSD path (pp_agc*) and the
# sceVideoOut buffer manager (pp_videoout) are deleted - ps5-opengl owns
# sceAgc + sceVideoOut for the whole session (evo_gl_context_device.cpp).
PP_SRCS := \
    pp/src/pp_audio_clock.c \
    pp/src/pp_stage_breadcrumb.c \
    pp/src/pp_clock.c \
    pp/src/pp_playback.c \
    pp/src/evo_theme.c \
    pp/src/evo_ui.c

UI_SRCS := \
    ui/src/evo_nav.c \
    ui/src/evo_focus.c \
    ui/src/evo_input.c \
    ui/src/evo_feedback.c \
    ui/src/evo_layout.c \
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
    media/src/evo_perf_monitor.c \
    media/src/evo_demux.c \
    media/src/evo_vdec_ffmpeg.c \
    media/src/evo_adec_native.c \
    media/src/evo_vdec_native.c \
    media/src/evo_sweep.c \
    media/src/evo_playback.c \
    src/evo_toast.c \
    src/evo_recent.c \
    src/evo_favorites.c \
    src/evo_data_path.c \
    src/evo_readdir.c \
    src/evo_boot_log.c \
    src/evo_crash_note.c \
    src/evo_usb_remote.c \
    src/evo_webui.c \
    src/evo_jailbreak.c

ADDON_SRCS := \
    addons/src/cJSON.c \
    addons/src/evo_net.c \
    addons/src/addon_emby.c \
    addons/src/evo_provider_mgr.c \
    addons/src/evo_provider_bundle.c \
    addons/src/provider_iptv.c \
    addons/src/provider_emby.c \
    addons/src/provider_jellyfin.c

# The bare-metal AGC device context and its 100% GPU RmlUi render interface
# are the only device render path - the AGC runtime owns sceAgc and
# sceVideoOut outright. The OpenGL path, the ps5-opengl submodule and the
# --gl / --gl-smoke / --gl-hdr-probe build modes were removed. This is off
# only for the host compile check, which has no graphics at all.
AGC_DEVICE ?= 1
ifeq ($(AGC_DEVICE),1)
    MEDIA_SRCS += \
        media/src/evo_agc_transient_ring.c \
        media/src/evo_agc_writer.c \
        media/src/evo_agc_shader_header.c \
        media/src/evo_agc_runtime.c
    # EVO_AGC_HAVE_VIDEO_PIPES: the four video pipelines are converted to .pipe
    # and compiled by tools/build_agc_pipes.py, so the video present path is
    # live. Drop it to bring the runtime up with the UI pipeline only.
    CFLAGS   += -DEVO_AGC_DEVICE=1 -DEVO_AGC_HAVE_VIDEO_PIPES=1 -Ishaders/agc
    CXXFLAGS += -DEVO_AGC_DEVICE=1 -DEVO_AGC_HAVE_VIDEO_PIPES=1 -Ishaders/agc
endif

C_SRCS := $(PP_SRCS) $(UI_SRCS) $(MEDIA_SRCS) $(ADDON_SRCS)
C_OBJS := $(C_SRCS:.c=.o)

CORE_SRCS := \
    main.cpp \
    core/src/Application.cpp \
    core/src/Bridge.cpp \
    core/src/services/SettingsService.cpp \
    core/src/services/MediaMetadataService.cpp \
    core/src/services/CoverArtService.cpp \
    core/src/services/SoundEffectEngine.cpp \
    core/src/services/SurroundTestService.cpp \
    core/src/services/FileSystemBrowser.cpp \
    core/src/services/PlaybackController.cpp \
    core/src/screens/ScreenManager.cpp \
    core/src/screens/LaunchScreen.cpp \
    core/src/screens/BrowserScreen.cpp \
    core/src/screens/PlayerScreen.cpp \
    core/src/screens/SettingsScreen.cpp \
    core/src/screens/SubtitlePickerScreen.cpp \
    core/src/screens/AudioTrackPickerScreen.cpp \
    core/src/screens/TextReaderScreen.cpp \
    core/src/screens/MediaInfoScreen.cpp \
    core/src/screens/SurroundTestScreen.cpp \
    core/src/screens/DeveloperToolsScreen.cpp \
    core/src/screens/EmbyScreen.cpp \
    core/src/screens/ProviderHostScreen.cpp \
    core/src/screens/RecentFilesScreen.cpp \
    core/src/screens/FavoritesScreen.cpp \
    core/src/screens/AboutSupportScreen.cpp \
    core/src/screens/SafeToCloseScreen.cpp \
    core/src/screens/ChangelogScreen.cpp \
    core/src/screens/ImageViewerScreen.cpp \
    core/src/screens/ModalDialogScreen.cpp \
    core/src/screens/StatefulScreen.cpp

CORE_OBJS := $(CORE_SRCS:.cpp=.o)

RMLUI_SRCS := \
    ui_rml/src/evo_rmlui_render.cpp \
    ui_rml/src/evo_rmlui_system.cpp \
    ui_rml/src/evo_rmlui_app.cpp \
    ui_rml/src/evo_rmlui_bridge.cpp \
    ui_rml/src/evo_rmlui_fileinterface.cpp \
    ui_rml/src/evo_rmlui_bundle.cpp \
    ui_rml/src/evo_rmlui_bundle_data.cpp \
    ui_rml/src/evo_rmlui_provider.cpp \
    ui_rml/src/evo_rmlui_provider_art.cpp

ifeq ($(AGC_DEVICE),1)
    RMLUI_SRCS += ui_rml/src/evo_rmlui_render_agc.cpp
endif

RMLUI_OBJS := $(RMLUI_SRCS:.cpp=.o)

# -MMD -MP: emit a .d next to each .o listing its header prerequisites, so an
# object rebuilds when a header it includes changes. Without this, editing e.g.
# a shared ABI header left stale .o files linked against the old ABI (#28, cost
# real hardware cycles). package-app.sh's app-cflags.stamp only force-cleans on
# a *flag* change, not a header edit.
DEPFLAGS := -MMD -MP
ALL_DEPS := $(C_OBJS:.o=.d) $(RMLUI_OBJS:.o=.d) $(CORE_OBJS:.o=.d)

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

all: $(ELF)

-include $(ALL_DEPS)

# -----------------------------------------------------------------------------
# App-module packaging hooks (Phase 1b - docs/evo-pro/phase-1b-app-module.md).
# scripts/package-app.sh compiles the object set with the native-app toolchain
# (prospero-clang18: -femulated-tls -fno-plt -fno-stack-protector) and does its
# own link/convert/sign, so it needs the objects without the SDK-crt link.
#   objects        build every .o, no ELF
#   print-objects  emit the object paths (relative to this dir), space-separated
# -----------------------------------------------------------------------------
.PHONY: objects print-objects
objects: $(C_OBJS) $(RMLUI_OBJS) $(CORE_OBJS)

print-objects:
	@echo $(C_OBJS) $(RMLUI_OBJS) $(CORE_OBJS)

$(ELF): $(C_OBJS) $(RMLUI_OBJS) $(CORE_OBJS)
	$(CXX) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "STAGE=$(STAGE)"
	@sha256sum $@
	@ls -la $@

clean:
	rm -f $(ELF) $(C_OBJS) $(RMLUI_OBJS) $(CORE_OBJS) $(ALL_DEPS)
	rm -rf artifacts/4k_stages
