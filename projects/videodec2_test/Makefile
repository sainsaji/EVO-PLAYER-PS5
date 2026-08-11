# EVO Player - videodec2_test
#
# Route B first call: sceVideodec2QueryDecoderMemoryInfo on libSceVideodec2.
#
# Kept as its own project, separate from decoder_test (reconnaissance) and
# avplayer_test (Route A), so that a dead end here costs nothing elsewhere.
#
# No -lSceVideodec2: the SDK ships no stub for it. The module is loaded with
# sceKernelLoadStartModule and the entry point resolved by NID at run time -
# see docs/native-media-research.md.

include ../common/evo.mk

ELF  := videodec2_test.elf
SRCS := main.c $(EVO_COMMON_SRCS)

LIBS := -lSceSysmodule

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(ELF)
