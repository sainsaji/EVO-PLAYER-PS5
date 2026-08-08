# EVO Player - yuv_gpu_test  (FUTURE MILESTONE, not implemented)
#
# Placeholder so the GPU YUV renderer has a home from the start.
# When implementation begins this will link SDL2 + mesa from the pacbrew
# sysroot, roughly:
#   LIBS := $(EVO_HB_LIB)/libSDL2.a -lSceVideoOut -lSceUserService -lSceGnmDriver
# plus whatever mesa pulls in. Keep it out of prosperoplayer until it works.

include ../common/evo.mk

ELF  := yuv_gpu_test.elf
SRCS := main.c $(EVO_COMMON_SRCS)

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(ELF)
