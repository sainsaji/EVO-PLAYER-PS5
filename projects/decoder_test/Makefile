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

LIBS := -lSceSysmodule

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(ELF)
