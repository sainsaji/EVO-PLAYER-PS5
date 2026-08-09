# EVO Player - decoder_test
#
# RESEARCH SCAFFOLD. Builds a symbol-reconnaissance payload for the native PS5
# video decoder modules. It deliberately implements no decoding yet.
#
# Note there are no -lSceVdec* flags here, and that is not an oversight: the
# SDK ships no stubs for those modules (verified against sce_stubs/ in v0.42),
# so they cannot be linked. Everything is resolved at run time through
# kernel_dynlib_dlsym() instead.

include ../common/evo.mk

ELF  := decoder_test.elf
SRCS := main.c $(EVO_COMMON_SRCS)

# VideoOut/AudioOut are linked purely as a CONTROL: they prove the dynlib
# probe works, since a module is only mapped when the payload links its stub.
LIBS := -lSceSysmodule -lSceVideoOut -lSceAudioOut

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(ELF)
