# EVO Player - evo_frame_decode
# Real H.264/HEVC Frame Decoder & Notification Pipeline for PS5

include ../common/evo.mk

ELF := evo_frame_decode.elf

CFLAGS += -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include

LIBS := \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libavformat.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libavcodec.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libswscale.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libavutil.a \
    -lSceSysmodule \
    -lSceUserService \
    -lpthread -lz -lm

SRCS := main.c $(EVO_COMMON_SRCS)

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(ELF)
